/* Optional AArch64 IEEE CRC-32 instructions. Runtime detection keeps the
 * library usable on Armv8.0 CPUs that do not implement the CRC extension. */
#include "ptpng_internal.h"

#if (defined(__aarch64__) || defined(_M_ARM64)) && \
    (defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)) && \
    (defined(__linux__) || defined(__APPLE__) || defined(_WIN32))
#define PTPNG_HAVE_ARM_CRC 1

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#else
#include <arm_acle.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#else
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

/* Only this function may contain CRC instructions. The initializer is
 * compiled for the baseline ISA, including in builds without -march=+crc. */
#if defined(__clang__)
__attribute__((target("crc"), noinline))
#elif defined(__GNUC__)
__attribute__((target("+crc"), noinline))
#else
__declspec(noinline)
#endif
static uint32_t ptpng_crc32_arm(const uint8_t *p, size_t n)
{
    uint32_t crc = UINT32_C(0xffffffff);
    while (n >= 8) {
        uint64_t word;
        memcpy(&word, p, sizeof(word));
#if defined(__AARCH64EB__) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
     __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        word = __builtin_bswap64(word);
#endif
        /* The non-c variants use PNG's IEEE polynomial, not CRC-32C. */
        crc = __crc32d(crc, word);
        p += 8;
        n -= 8;
    }
    while (n) {
        crc = __crc32b(crc, *p++);
        --n;
    }
    return ~crc;
}

/* Only this function may contain DotProd instructions. Two independent
 * accumulator chains hide udot latency across 64-byte strides. */
#if defined(__clang__)
__attribute__((target("dotprod"), noinline))
#elif defined(__GNUC__)
__attribute__((target("+dotprod"), noinline))
#else
__declspec(noinline)
#endif
static uint32_t ptpng_adler32_arm_dotprod(const uint8_t *p, size_t n)
{
    static const uint8_t w0_bytes[16] = {
        64,63,62,61,60,59,58,57,56,55,54,53,52,51,50,49 };
    static const uint8_t w1_bytes[16] = {
        48,47,46,45,44,43,42,41,40,39,38,37,36,35,34,33 };
    static const uint8_t w2_bytes[16] = {
        32,31,30,29,28,27,26,25,24,23,22,21,20,19,18,17 };
    static const uint8_t w3_bytes[16] = {
        16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1 };
    const uint8x16_t ones = vdupq_n_u8(1);
    const uint8x16_t w0 = vld1q_u8(w0_bytes);
    const uint8x16_t w1 = vld1q_u8(w1_bytes);
    const uint8x16_t w2 = vld1q_u8(w2_bytes);
    const uint8x16_t w3 = vld1q_u8(w3_bytes);
    uint32_t a = 1, b = 0;

    while (n >= 64) {
        unsigned chunk = (unsigned)(n < 5504 ? n : 5504) & ~63u;
        uint32x4_t s0 = vdupq_n_u32(0), s1 = s0, s2 = s0, s3 = s0;
        uint32x4_t p0 = s0, p1 = s0, p2 = s0, p3 = s0;
        uint32x4_t wt0 = s0, wt1 = s0, wt2 = s0, wt3 = s0;
        uint32x4_t sums, prior, weighted;
        unsigned i;
        for (i = 0; i < chunk; i += 64) {
            uint8x16_t d0 = vld1q_u8(p + i);
            uint8x16_t d1 = vld1q_u8(p + i + 16);
            uint8x16_t d2 = vld1q_u8(p + i + 32);
            uint8x16_t d3 = vld1q_u8(p + i + 48);
            p0 = vaddq_u32(p0, s0);
            p1 = vaddq_u32(p1, s1);
            p2 = vaddq_u32(p2, s2);
            p3 = vaddq_u32(p3, s3);
            s0 = vdotq_u32(s0, d0, ones);
            s1 = vdotq_u32(s1, d1, ones);
            s2 = vdotq_u32(s2, d2, ones);
            s3 = vdotq_u32(s3, d3, ones);
            wt0 = vdotq_u32(wt0, d0, w0);
            wt1 = vdotq_u32(wt1, d1, w1);
            wt2 = vdotq_u32(wt2, d2, w2);
            wt3 = vdotq_u32(wt3, d3, w3);
        }
        sums = vaddq_u32(vaddq_u32(s0, s1), vaddq_u32(s2, s3));
        prior = vaddq_u32(vaddq_u32(p0, p1), vaddq_u32(p2, p3));
        weighted = vaddq_u32(vaddq_u32(vaddq_u32(wt0, wt1), vaddq_u32(wt2, wt3)),
                             vshlq_n_u32(prior, 6));
        /* 5504*65520 + 255*5504*5505/2 + 65520 < 2^32. */
        b = (b + chunk * a + vaddvq_u32(weighted)) % 65521u;
        a = (a + vaddvq_u32(sums)) % 65521u;
        p += chunk;
        n -= chunk;
    }
    while (n--) {
        a += *p++;
        b += a;
    }
    return ((b % 65521u) << 16) | (a % 65521u);
}

static int ptpng_arm_has_crc(void)
{
#if defined(_WIN32)
    return IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE) != 0;
#elif defined(__APPLE__)
    int available = 0;
    size_t size = sizeof(available);
    return sysctlbyname("hw.optional.armv8_crc32", &available, &size, NULL, 0) == 0 &&
           size == sizeof(available) && available != 0;
#elif defined(HWCAP_CRC32)
    return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
    return 0;
#endif
}

static int ptpng_arm_has_dotprod(void)
{
#if defined(_WIN32) && defined(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE)
    return IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE) != 0;
#elif defined(__APPLE__)
    int available = 0;
    size_t size = sizeof(available);
    return sysctlbyname("hw.optional.arm.FEAT_DotProd", &available, &size, NULL, 0) == 0 &&
           size == sizeof(available) && available != 0;
#elif defined(HWCAP_ASIMDDP)
    return (getauxval(AT_HWCAP) & HWCAP_ASIMDDP) != 0;
#else
    return 0;
#endif
}
#endif

void ptpng_crc_arm_init(void)
{
#if PTPNG_HAVE_ARM_CRC
    if (ptpng_arm_has_crc()) {
        ptpng_cpu.arm_crc32 = 1;
        ptpng_cpu.crc32 = ptpng_crc32_arm;
    }
    if (ptpng_arm_has_dotprod()) {
        ptpng_cpu.arm_dotprod = 1;
        ptpng_cpu.adler32 = ptpng_adler32_arm_dotprod;
    }
#endif
}
