/*
 * ptpng_neon.c - ARM NEON accelerated paths, hooked from
 * ptpng_cpu_init() on NEON builds.  Each kernel mirrors the tested
 * x86 equivalent 1:1 (vqtbl1q zeroes out-of-range indices exactly like
 * pshufb's high-bit control bytes).
 */
#include "ptpng_internal.h"

#if PTPNG_ARM_NEON

/* ---- filters ---- */

/* Prefix sums within each register, then repeat the final pixel as the
 * carry for the following register. The table also rotates RGB carries
 * when a register ends partway through a pixel. */
#define SUB_PREFIX(v, BPP)                                               \
    (v) = vaddq_u8((v), vextq_u8(zero, (v), 16 - (BPP)));                \
    if ((BPP) < 8) (v) = vaddq_u8((v),                                  \
        vextq_u8(zero, (v), (16 - (BPP) * 2) & 15));                     \
    if ((BPP) < 4) (v) = vaddq_u8((v),                                  \
        vextq_u8(zero, (v), (16 - (BPP) * 4) & 15));                     \
    if ((BPP) == 1) (v) = vaddq_u8((v), vextq_u8(zero, (v), 8));

#define SUB_BLOCKS(BPP, ...)                                             \
    {                                                                    \
        static const uint8_t ctrl_bytes[16] = {__VA_ARGS__};              \
        const uint8x16_t ctrl = vld1q_u8(ctrl_bytes);                     \
        const uint8x16_t zero = vdupq_n_u8(0);                            \
        uint8x16_t carry = zero;                                         \
        size_t i = 0;                                                    \
        for (; i + 64 <= count; i += 64) {                               \
            uint8x16_t v0 = vld1q_u8(src + i);                           \
            uint8x16_t v1 = vld1q_u8(src + i + 16);                      \
            uint8x16_t v2 = vld1q_u8(src + i + 32);                      \
            uint8x16_t v3 = vld1q_u8(src + i + 48);                      \
            SUB_PREFIX(v0, BPP) SUB_PREFIX(v1, BPP)                      \
            SUB_PREFIX(v2, BPP) SUB_PREFIX(v3, BPP)                      \
            v0 = vaddq_u8(v0, carry);                                    \
            v1 = vaddq_u8(v1, vqtbl1q_u8(v0, ctrl));                      \
            v2 = vaddq_u8(v2, vqtbl1q_u8(v1, ctrl));                      \
            v3 = vaddq_u8(v3, vqtbl1q_u8(v2, ctrl));                      \
            carry = vqtbl1q_u8(v3, ctrl);                                \
            vst1q_u8(dst + i, v0);                                      \
            vst1q_u8(dst + i + 16, v1);                                 \
            vst1q_u8(dst + i + 32, v2);                                 \
            vst1q_u8(dst + i + 48, v3);                                 \
        }                                                                \
        for (; i + 16 <= count; i += 16) {                               \
            uint8x16_t v = vld1q_u8(src + i);                            \
            SUB_PREFIX(v, BPP)                                          \
            v = vaddq_u8(v, carry);                                      \
            carry = vqtbl1q_u8(v, ctrl);                                 \
            vst1q_u8(dst + i, v);                                       \
        }                                                                \
        for (; i < count; i++)                                          \
            dst[i] = (uint8_t)(src[i] + (i >= (BPP) ? dst[i - (BPP)] : 0));\
        return;                                                          \
    }

static void ptpng_filter_sub_neon(uint8_t *dst, const uint8_t *src,
                                 const uint8_t *prev, size_t count, unsigned bpp)
{
    switch (bpp) {
    case 1: SUB_BLOCKS(1, 15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15)
    case 2: SUB_BLOCKS(2, 14,15,14,15,14,15,14,15,14,15,14,15,14,15,14,15)
    case 3: SUB_BLOCKS(3, 13,14,15,13,14,15,13,14,15,13,14,15,13,14,15,13)
    case 4: SUB_BLOCKS(4, 12,13,14,15,12,13,14,15,12,13,14,15,12,13,14,15)
    case 6: SUB_BLOCKS(6, 10,11,12,13,14,15,10,11,12,13,14,15,10,11,12,13)
    case 8: SUB_BLOCKS(8, 8,9,10,11,12,13,14,15,8,9,10,11,12,13,14,15)
    default: ptpng_filter_sub_scalar(dst, src, prev, count, bpp);
    }
}

#undef SUB_BLOCKS
#undef SUB_PREFIX

/* Four- and eight-byte pixels contain independent Paeth chains. Signed
 * 16-bit thresholds cover -510..765; equality selects a or b, never c. */
PTPNG_API_INLINE uint16x8_t paeth_pixel_neon(uint16x8_t s, uint16x8_t a,
                                          uint16x8_t b, uint16x8_t c)
{
    int16x8_t lo = vreinterpretq_s16_u16(vminq_u16(a, b));
    int16x8_t hi = vreinterpretq_s16_u16(vmaxq_u16(a, b));
    int16x8_t threshold = vsubq_s16(
        vreinterpretq_s16_u16(vaddq_u16(c, vshlq_n_u16(c, 1))),
        vreinterpretq_s16_u16(vaddq_u16(a, b)));
    uint16x8_t predictor = vbslq_u16(vcgtq_s16(threshold, lo), c,
                                    vreinterpretq_u16_s16(hi));
    predictor = vbslq_u16(vcgtq_s16(hi, threshold), predictor,
                          vreinterpretq_u16_s16(lo));
    return vandq_u16(vaddq_u16(s, predictor), vdupq_n_u16(255));
}

/* Clang uses a separate stride-selection wrapper. MSVC keeps the
 * original kernel and direct dispatch to avoid an eight-byte regression. */
#if defined(__clang__)
__attribute__((noinline))
#endif
static void ptpng_filter_paeth_neon(uint8_t *dst, const uint8_t *src,
                                   const uint8_t *prev, size_t count,
                                   unsigned bpp)
{
    size_t i = 0;
    uint16x8_t a = vdupq_n_u16(0), c = vdupq_n_u16(0);
#if defined(__GNUC__) && !defined(__clang__)
    /* Preserve the measured GCC layout: four-byte pixels branch to the
     * shared tail, while eight-byte pixels reach their loop directly. */
    if (bpp == 4) {
#else
    if (bpp == 8) {
        for (; i + 8 <= count; i += 8) {
            uint16x8_t b = vmovl_u8(vld1_u8(prev + i));
            uint16x8_t s = vmovl_u8(vld1_u8(src + i));
            a = paeth_pixel_neon(s, a, b, c);
            c = b;
            vst1_u8(dst + i, vmovn_u16(a));
        }
    } else if (bpp == 4) {
#endif
        for (; i + 4 <= count; i += 4) {
            uint32_t sv, bv, output;
            uint16x8_t b, s;
            /* Exactly four bytes are accessible at the end of a short row. */
            memcpy(&sv, src + i, 4);
            memcpy(&bv, prev + i, 4);
            b = vmovl_u8(vcreate_u8(bv));
            s = vmovl_u8(vcreate_u8(sv));
            a = paeth_pixel_neon(s, a, b, c);
            c = b;
            output = vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(a)), 0);
            memcpy(dst + i, &output, 4);
        }
#if defined(__GNUC__) && !defined(__clang__)
        goto tail;
    }
    if (bpp != 8) {
#else
    } else {
#endif
        ptpng_filter_paeth_scalar(dst, src, prev, count, bpp);
        return;
    }
#if defined(__GNUC__) && !defined(__clang__)
    for (; i + 8 <= count; i += 8) {
        uint16x8_t b = vmovl_u8(vld1_u8(prev + i));
        uint16x8_t s = vmovl_u8(vld1_u8(src + i));
        a = paeth_pixel_neon(s, a, b, c);
        c = b;
        vst1_u8(dst + i, vmovn_u16(a));
    }
tail:
#endif
    for (; i < count; i++) {
        int left = i >= bpp ? dst[i - bpp] : 0;
        int up = prev[i], upper_left = i >= bpp ? prev[i - bpp] : 0;
        int lo = left < up ? left : up, hi = left < up ? up : left;
        int threshold = 3 * upper_left - left - up;
        int predictor = threshold >= hi ? lo : threshold <= lo ? hi : upper_left;
        dst[i] = (uint8_t)(src[i] + predictor);
    }
}

#if defined(__clang__)
static void ptpng_filter_paeth_neon_dispatch(uint8_t *dst, const uint8_t *src,
                                            const uint8_t *prev, size_t count,
                                            unsigned bpp)
{
    /* Clang favors scalar four-byte chains. GCC retains SIMD because
     * its scalar fourth-channel branch slows random-alpha images. */
    if (bpp == 4)
        ptpng_filter_paeth_scalar(dst, src, prev, count, bpp);
    else
        ptpng_filter_paeth_neon(dst, src, prev, count, bpp);
}
#endif

/* Collect byte columns in independent 16-bit sums, then apply positional
 * weights once per chunk. A column reaches at most 32*255=8160; the full
 * weighted sum is at most 255*2048*2049/2, safely below 2^31. Keeping this
 * separate avoids its register-save overhead for small inputs. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static uint32_t ptpng_adler32_neon_large(const uint8_t *p, size_t n)
{
    static const uint16_t weights[64] = {
        64,63,62,61,60,59,58,57, 56,55,54,53,52,51,50,49,
        48,47,46,45,44,43,42,41, 40,39,38,37,36,35,34,33,
        32,31,30,29,28,27,26,25, 24,23,22,21,20,19,18,17,
        16,15,14,13,12,11,10,9, 8,7,6,5,4,3,2,1
    };
    uint32_t a = 1, b = 0;
    while (n >= 64) {
        unsigned chunk = n < 2048 ? (unsigned)n : 2048;
        unsigned i;
        uint16x8_t c0 = vdupq_n_u16(0), c1 = c0, c2 = c0, c3 = c0;
        uint16x8_t c4 = c0, c5 = c0, c6 = c0, c7 = c0;
        uint32x4_t sums = vdupq_n_u32(0), prior = sums, weighted;
        chunk &= ~63u;
        for (i = 0; i < chunk; i += 64) {
            uint8x16_t d0 = vld1q_u8(p + i), d1 = vld1q_u8(p + i + 16);
            uint8x16_t d2 = vld1q_u8(p + i + 32), d3 = vld1q_u8(p + i + 48);
            uint16x8_t total = vaddq_u16(
                vaddq_u16(vpaddlq_u8(d0), vpaddlq_u8(d1)),
                vaddq_u16(vpaddlq_u8(d2), vpaddlq_u8(d3)));
            /* Each earlier byte contributes once per following byte. */
            prior = vaddq_u32(prior, sums);
            sums = vpadalq_u16(sums, total);
            c0 = vaddw_u8(c0, vget_low_u8(d0));
            c1 = vaddw_u8(c1, vget_high_u8(d0));
            c2 = vaddw_u8(c2, vget_low_u8(d1));
            c3 = vaddw_u8(c3, vget_high_u8(d1));
            c4 = vaddw_u8(c4, vget_low_u8(d2));
            c5 = vaddw_u8(c5, vget_high_u8(d2));
            c6 = vaddw_u8(c6, vget_low_u8(d3));
            c7 = vaddw_u8(c7, vget_high_u8(d3));
        }
        weighted = vshlq_n_u32(prior, 6);
#define ADLER_WEIGHT(C, INDEX) do {                                      \
            uint16x8_t w = vld1q_u16(weights + (INDEX) * 8);              \
            weighted = vmlal_u16(weighted, vget_low_u16(C), vget_low_u16(w)); \
            weighted = vmlal_u16(weighted, vget_high_u16(C), vget_high_u16(w)); \
        } while (0)
        ADLER_WEIGHT(c0, 0); ADLER_WEIGHT(c1, 1);
        ADLER_WEIGHT(c2, 2); ADLER_WEIGHT(c3, 3);
        ADLER_WEIGHT(c4, 4); ADLER_WEIGHT(c5, 5);
        ADLER_WEIGHT(c6, 6); ADLER_WEIGHT(c7, 7);
#undef ADLER_WEIGHT
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

/* Modulo reduction every 2048 bytes keeps the weighted sum below 2^31,
 * even for all-255 input. Accumulate four independent sums per vector. */
static uint32_t ptpng_adler32_neon(const uint8_t *p, size_t n)
{
    if (n >= 512)
        return ptpng_adler32_neon_large(p, n);
    static const uint8_t weight_bytes[16] = {16, 15, 14, 13, 12, 11, 10, 9,
                                            8, 7, 6, 5, 4, 3, 2, 1};
    const uint8x8_t weight_lo = vld1_u8(weight_bytes);
    const uint8x8_t weight_hi = vld1_u8(weight_bytes + 8);
    uint32_t a = 1, b = 0;
    while (n >= 16) {
        unsigned chunk = n < 2048 ? (unsigned)n : 2048;
        uint32x4_t sums = vdupq_n_u32(0), weighted = vdupq_n_u32(0);
        unsigned i;
        chunk &= ~15u;
        for (i = 0; i < chunk; i += 16) {
            uint8x16_t bytes = vld1q_u8(p + i);
            uint16x8_t weights = vaddq_u16(
                vmull_u8(vget_low_u8(bytes), weight_lo),
                vmull_u8(vget_high_u8(bytes), weight_hi));
            weighted = vaddq_u32(weighted, vshlq_n_u32(sums, 4));
            weighted = vpadalq_u16(weighted, weights);
            sums = vpadalq_u16(sums, vpaddlq_u8(bytes));
        }
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

void ptpng_filter_up_neon(uint8_t *dst, const uint8_t *src,
                          const uint8_t *prev, size_t count, unsigned bpp)
{
    size_t i = 0;
    (void)bpp;
    for (; i + 64 <= count; i += 64) {
        vst1q_u8(dst + i, vaddq_u8(vld1q_u8(src + i), vld1q_u8(prev + i)));
        vst1q_u8(dst + i + 16,
                 vaddq_u8(vld1q_u8(src + i + 16), vld1q_u8(prev + i + 16)));
        vst1q_u8(dst + i + 32,
                 vaddq_u8(vld1q_u8(src + i + 32), vld1q_u8(prev + i + 32)));
        vst1q_u8(dst + i + 48,
                 vaddq_u8(vld1q_u8(src + i + 48), vld1q_u8(prev + i + 48)));
    }
    for (; i + 16 <= count; i += 16)
        vst1q_u8(dst + i, vaddq_u8(vld1q_u8(src + i), vld1q_u8(prev + i)));
    for (; i < count; i++)
        dst[i] = (uint8_t)(src[i] + prev[i]);
}

/* ---- conversions (mirror of the pshufb versions) ---- */

/* RGB8 -> RGBA8. Structured loads/stores expand sixteen pixels without
 * overlapping source loads or byte lookup tables. tRNS uses the scalar
 * converter selected by ptpng_decode before this function is called. */
static void rgba8_rgb8_neon(const uint8_t *src, uint8_t *dst, uint32_t n,
                            const struct ptpng_cvt *c)
{
    uint8x16x4_t rgba;
    rgba.val[3] = vdupq_n_u8(255);
#if PTPNG_REUSE_CVT_BUFFER
    if (c->reverse) {
        while (n >= 16) {
            uint8x16x3_t rgb;
            n -= 16;
            rgb = vld3q_u8(src + (size_t)n * 3);
            rgba.val[0] = rgb.val[0];
            rgba.val[1] = rgb.val[1];
            rgba.val[2] = rgb.val[2];
            vst4q_u8(dst + (size_t)n * 4, rgba);
        }
        if (n >= 8) {
            uint8x8x4_t small;
            uint8x8x3_t rgb;
            n -= 8;
            rgb = vld3_u8(src + (size_t)n * 3);
            small.val[0] = rgb.val[0]; small.val[1] = rgb.val[1];
            small.val[2] = rgb.val[2]; small.val[3] = vdup_n_u8(255);
            vst4_u8(dst + (size_t)n * 4, small);
        }
        while (n) {
            uint8_t r, g, b;
            --n;
            r = src[(size_t)n * 3]; g = src[(size_t)n * 3 + 1];
            b = src[(size_t)n * 3 + 2];
            dst[(size_t)n * 4] = r; dst[(size_t)n * 4 + 1] = g;
            dst[(size_t)n * 4 + 2] = b; dst[(size_t)n * 4 + 3] = 255;
        }
        return;
    }
#else
    (void)c;
#endif
    for (; n >= 16; n -= 16, src += 48, dst += 64) {
        uint8x16x3_t rgb = vld3q_u8(src);
        rgba.val[0] = rgb.val[0];
        rgba.val[1] = rgb.val[1];
        rgba.val[2] = rgb.val[2];
        vst4q_u8(dst, rgba);
    }
    if (n >= 8) {
        uint8x8x3_t rgb = vld3_u8(src);
        uint8x8x4_t small;
        small.val[0] = rgb.val[0];
        small.val[1] = rgb.val[1];
        small.val[2] = rgb.val[2];
        small.val[3] = vdup_n_u8(255);
        vst4_u8(dst, small);
        n -= 8; src += 24; dst += 32;
    }
    for (; n; n--, src += 3, dst += 4) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
    }
}

/* RGBA8 -> RGB8 with bounded structured loads and exact RGB stores. */
#if defined(__clang__) || defined(_MSC_VER)
static void rgb8_rgba8_neon(const uint8_t *src, uint8_t *dst, uint32_t n,
                            const struct ptpng_cvt *c)
{
    (void)c;
    for (; n >= 16; n -= 16, src += 64, dst += 48) {
        uint8x16x4_t rgba = vld4q_u8(src);
        uint8x16x3_t rgb;
        rgb.val[0] = rgba.val[0];
        rgb.val[1] = rgba.val[1];
        rgb.val[2] = rgba.val[2];
        vst3q_u8(dst, rgb);
    }
    if (n >= 8) {
        uint8x8x4_t rgba = vld4_u8(src);
        uint8x8x3_t rgb;
        rgb.val[0] = rgba.val[0];
        rgb.val[1] = rgba.val[1];
        rgb.val[2] = rgba.val[2];
        vst3_u8(dst, rgb);
        n -= 8; src += 32; dst += 24;
    }
    for (; n; --n, src += 4, dst += 3) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
    }
}
#endif

/* gray8 -> RGBA8: gg holds each value twice; controls index 0,2,4,6 and
 * 8,10,12,14; index 16 (out of table) zeroes the alpha lane which is
 * then filled by the OR constant. */
static void rgba8_g8_neon(const uint8_t *src, uint8_t *dst, uint32_t n,
                          const struct ptpng_cvt *c)
{
    static const uint8_t dup_lo_bytes[16] = {
        0, 0, 0, 16, 2, 2, 2, 16, 4, 4, 4, 16, 6, 6, 6, 16 };
    static const uint8_t dup_hi_bytes[16] = {
        8, 8, 8, 16, 10, 10, 10, 16, 12, 12, 12, 16, 14, 14, 14, 16 };
    static const uint8_t alpha_bytes[16] = { 0, 0, 0, 255, 0, 0, 0, 255,
                                      0, 0, 0, 255, 0, 0, 0, 255 };
    const uint8x16_t dup_lo = vld1q_u8(dup_lo_bytes);
    const uint8x16_t dup_hi = vld1q_u8(dup_hi_bytes);
    const uint8x16_t alpha = vld1q_u8(alpha_bytes);
    uint32_t i = 0;
    (void)c;
    for (; i + 8 <= n; i += 8) {
        uint8x8_t g = vld1_u8(src + i);
        uint8x8x2_t z = vzip_u8(g, g);       /* g0 g0 g1 g1 ... g7 g7 */
        uint8x16_t gg = vcombine_u8(z.val[0], z.val[1]);
        vst1q_u8(dst + i * 4,
                 vorrq_u8(vqtbl1q_u8(gg, dup_lo), alpha));
        vst1q_u8(dst + i * 4 + 16,
                 vorrq_u8(vqtbl1q_u8(gg, dup_hi), alpha));
    }
    for (; i < n; i++) {
        uint8_t gv = src[i];
        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = gv;
        dst[i * 4 + 3] = 255;
    }
}

/* gray+alpha 8-bit -> RGBA8: [g,g,g,a] straight from the interleaved
 * source bytes. */
static void rgba8_ga8_neon(const uint8_t *src, uint8_t *dst, uint32_t n,
                           const struct ptpng_cvt *c)
{
    static const uint8_t lo_bytes[16] = {
        0, 0, 0, 1, 2, 2, 2, 3, 4, 4, 4, 5, 6, 6, 6, 7 };
    static const uint8_t hi_bytes[16] = {
        8, 8, 8, 9, 10, 10, 10, 11, 12, 12, 12, 13, 14, 14, 14, 15 };
    const uint8x16_t lo = vld1q_u8(lo_bytes);
    const uint8x16_t hi = vld1q_u8(hi_bytes);
    uint32_t i = 0;
    (void)c;
    for (; i + 8 <= n; i += 8) {
        uint8x16_t v = vld1q_u8(src + i * 2); /* g0 a0 g1 a1 ... */
        vst1q_u8(dst + i * 4, vqtbl1q_u8(v, lo));
        vst1q_u8(dst + i * 4 + 16, vqtbl1q_u8(v, hi));
    }
    for (; i < n; i++) {
        uint8_t g = src[i * 2];
        dst[i * 4] = dst[i * 4 + 1] = dst[i * 4 + 2] = g;
        dst[i * 4 + 3] = src[i * 2 + 1];
    }
}

/* rgba16 BE -> rgba8: keep the high byte of each sample; process four
 * pixels (32 in-bytes) per iteration. */
static void rgba8_rgba16_neon(const uint8_t *src, uint8_t *dst, uint32_t n,
                              const struct ptpng_cvt *c)
{
    static const uint8_t pick_bytes[16] = {
        0, 2, 4, 6, 8, 10, 12, 14, 0, 0, 0, 0, 0, 0, 0, 0 };
    const uint8x16_t pick = vld1q_u8(pick_bytes);
    uint32_t i = 0;
    (void)c;
    for (; i + 4 <= n; i += 4) {
        uint8x16_t a = vld1q_u8(src + i * 8);      /* px0 px1 */
        uint8x16_t b = vld1q_u8(src + i * 8 + 16); /* px2 px3 */
        uint8x16_t pa = vqtbl1q_u8(a, pick);
        uint8x16_t pb = vqtbl1q_u8(b, pick);
        vst1q_u8(dst + i * 4,
                 vcombine_u8(vget_low_u8(pa), vget_low_u8(pb)));
    }
    for (; i < n; i++) {
        dst[i * 4] = src[i * 8];
        dst[i * 4 + 1] = src[i * 8 + 2];
        dst[i * 4 + 2] = src[i * 8 + 4];
        dst[i * 4 + 3] = src[i * 8 + 6];
    }
}

/* ---- conversion tables (mutable copies, hot entries overridden) ---- */

ptpng_cvt_fn ptpng_cvt_table_rgba8_neon[128];
ptpng_cvt_fn ptpng_cvt_table_rgb8_neon[128];

void ptpng_neon_init(void)
{
    memcpy(ptpng_cvt_table_rgba8_neon, ptpng_cvt_table_rgba8_scalar,
           sizeof(ptpng_cvt_table_rgba8_neon));
    memcpy(ptpng_cvt_table_rgb8_neon, ptpng_cvt_table_rgb8_scalar,
           sizeof(ptpng_cvt_table_rgb8_neon));

    if (!ptpng_cpu.neon)
        return;
    ptpng_cpu.filter_sub = ptpng_filter_sub_neon;
    ptpng_cpu.adler32 = ptpng_adler32_neon;
    ptpng_cpu.filter_up = ptpng_filter_up_neon;
#if defined(__clang__)
    ptpng_cpu.filter_paeth = ptpng_filter_paeth_neon_dispatch;
#else
    ptpng_cpu.filter_paeth = ptpng_filter_paeth_neon;
#endif
    ptpng_cpu.cvt_table_rgba8 = ptpng_cvt_table_rgba8_neon;
    ptpng_cpu.cvt_table_rgb8 = ptpng_cvt_table_rgb8_neon;
    ptpng_cvt_table_rgba8_neon[(0 << 4) | 3] = rgba8_g8_neon;
    ptpng_cvt_table_rgba8_neon[(2 << 4) | 3] = rgba8_rgb8_neon;
    ptpng_cvt_table_rgba8_neon[(4 << 4) | 3] = rgba8_ga8_neon;
    ptpng_cvt_table_rgba8_neon[(6 << 4) | 4] = rgba8_rgba16_neon;
    /* GCC's scalar converter auto-vectorizes faster on Neoverse-N2. */
#if defined(__clang__) || defined(_MSC_VER)
    ptpng_cvt_table_rgb8_neon[(6 << 4) | 3] = rgb8_rgba8_neon;
#endif
}

#endif /* PTPNG_ARM_NEON */
