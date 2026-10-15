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

/* Advance a raw CRC by 256 or 512 zero bytes. Three independent hardware
 * chains can then be joined without waiting on one CRC dependency chain.
 * Initialization, like the other checksum tables, is single-threaded. */
static uint32_t crc_shift_tables[2][4][256];
static int crc_shift_ready;

static uint32_t crc_matrix_times(const uint32_t *matrix, uint32_t value)
{
    uint32_t result = 0;
    while (value) {
        if (value & 1) result ^= *matrix;
        value >>= 1;
        ++matrix;
    }
    return result;
}

static void crc_shift_init(void)
{
    uint32_t matrix[32], square[32];
    unsigned i, round, table, byte, value, bit;
    for (i = 0; i < 32; ++i) {
        uint32_t v = UINT32_C(1) << i;
        matrix[i] = (v >> 1) ^ (v & 1 ? UINT32_C(0xedb88320) : 0);
    }
    /* Eleven squarings turn a one-bit shift into a 2048-bit shift. */
    for (round = 0; round < 11; ++round) {
        for (i = 0; i < 32; ++i)
            square[i] = crc_matrix_times(matrix, matrix[i]);
        memcpy(matrix, square, sizeof(matrix));
    }
    for (table = 0; table < 2; ++table) {
        for (byte = 0; byte < 4; ++byte) {
            for (value = 0; value < 256; ++value) {
                uint32_t v = 0;
                for (bit = 0; bit < 8; ++bit)
                    if (value & (1u << bit)) v ^= matrix[8 * byte + bit];
                crc_shift_tables[table][byte][value] = v;
            }
        }
        if (table == 0) {
            for (i = 0; i < 32; ++i)
                square[i] = crc_matrix_times(matrix, matrix[i]);
            memcpy(matrix, square, sizeof(matrix));
        }
    }
    crc_shift_ready = 1;
}

PTPNG_API_INLINE uint32_t crc_shift(uint32_t crc, unsigned table)
{
    return crc_shift_tables[table][0][crc & 255] ^
           crc_shift_tables[table][1][(crc >> 8) & 255] ^
           crc_shift_tables[table][2][(crc >> 16) & 255] ^
           crc_shift_tables[table][3][crc >> 24];
}

PTPNG_API_INLINE uint64_t crc_load64le(const uint8_t *p)
{
    uint64_t word;
    memcpy(&word, p, sizeof(word));
#if defined(__AARCH64EB__) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
     __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    word = __builtin_bswap64(word);
#endif
    return word;
}

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
    if (n >= 768 && !crc_shift_ready) crc_shift_init();
    while (n >= 768) {
        uint32_t a = crc, b = 0, c = 0;
        unsigned i;
        for (i = 0; i < 256; i += 8) {
            a = __crc32d(a, crc_load64le(p + i));
            b = __crc32d(b, crc_load64le(p + 256 + i));
            c = __crc32d(c, crc_load64le(p + 512 + i));
        }
        crc = crc_shift(a, 1) ^ crc_shift(b, 0) ^ c;
        p += 768;
        n -= 768;
    }
    while (n >= 8) {
        /* The non-c variants use PNG's IEEE polynomial, not CRC-32C. */
        crc = __crc32d(crc, crc_load64le(p));
        p += 8;
        n -= 8;
    }
    while (n) {
        crc = __crc32b(crc, *p++);
        --n;
    }
    return ~crc;
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
#endif

void ptpng_crc_arm_init(void)
{
#if PTPNG_HAVE_ARM_CRC
    if (ptpng_arm_has_crc()) {
        ptpng_cpu.arm_crc32 = 1;
        ptpng_cpu.crc32 = ptpng_crc32_arm;
    }
#endif
}
