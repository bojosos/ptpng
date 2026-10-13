/*
 * ptpng.c - core PNG decoder: chunk parsing, spec validation, unfilter,
 * Adam7 deinterlacing, output conversions, CPU dispatch.
 */
#include "ptpng_internal.h"
#include <stdio.h>

#if PTPNG_X86 && !defined(_MSC_VER)
#include <cpuid.h>
#endif

#if PTPNG_X86
static unsigned long long pt_tsc(void) { return __rdtsc(); }
#else
static unsigned long long pt_tsc(void) { return 0; }
#endif

#define PTPNG_SIG0 0x89
static const uint8_t png_sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};

/* chunk type constants (big-endian byte order in file) */
#define T(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define CH_IHDR T('I','H','D','R')
#define CH_PLTE T('P','L','T','E')
#define CH_IDAT T('I','D','A','T')
#define CH_IEND T('I','E','N','D')
#define CH_TRNS T('t','R','N','S')
#define CH_GAMA T('g','A','M','A')
#define CH_CHRM T('c','H','R','M')
#define CH_SRGB T('s','R','G','B')
#define CH_SBIT T('s','B','I','T')
#define CH_BKGD T('b','K','G','D')
#define CH_PHYS T('p','H','Y','s')
#define CH_TIME T('t','I','M','E')
#define CH_TEXT T('t','E','X','t')
#define CH_ZTXT T('z','T','X','t')
#define CH_ITXT T('i','T','X','t')
#define CH_HIST T('h','I','S','T')
#define CH_SPLT T('s','P','L','T')
#define CH_ICCP T('i','C','C','P')
#define CH_EXIF T('e','X','I','f')

/* ================= cpu dispatch ================= */

struct ptpng_cpu ptpng_cpu;

#if PTPNG_X86
static void pt_cpuid(unsigned f, unsigned sub, int regs[4])
{
#if defined(_MSC_VER)
    __cpuidex(regs, (int)f, (int)sub);
#else
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_count(f, sub, &a, &b, &c, &d)) {
        regs[0] = (int)a; regs[1] = (int)b; regs[2] = (int)c;
        regs[3] = (int)d;
    } else {
        regs[0] = regs[1] = regs[2] = regs[3] = 0;
    }
#endif
}

static unsigned long long pt_xgetbv0(void)
{
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned eax, edx;
    __asm__ volatile ("xgetbv" : "=a" (eax), "=d" (edx) : "c" (0));
    return ((unsigned long long)edx << 32) | eax;
#endif
}
#endif /* PTPNG_X86 */

void ptpng_cpu_init(void)
{
    int regs[4];
    if (ptpng_cpu.crc32)
        return;
    memset(&ptpng_cpu, 0, sizeof(ptpng_cpu));
#if PTPNG_X86
    pt_cpuid(1, 0, regs);
    ptpng_cpu.sse2 = (regs[3] & (1 << 26)) != 0;
    ptpng_cpu.ssse3 = (regs[2] & (1 << 9)) != 0;
    ptpng_cpu.sse41 = (regs[2] & (1 << 19)) != 0;
    ptpng_cpu.pclmul = (regs[2] & (1 << 1)) != 0;
    if ((regs[2] & (1 << 27)) && (regs[2] & (1 << 28)) &&
        (pt_xgetbv0() & 0x6) == 0x6) {
        pt_cpuid(7, 0, regs);
        ptpng_cpu.avx2 = (regs[1] & (1 << 5)) != 0;
        ptpng_cpu.bmi1 = (regs[1] & (1 << 3)) != 0;
        ptpng_cpu.bmi2 = (regs[1] & (1 << 8)) != 0;
    }
#endif
#if PTPNG_ARM_NEON
    ptpng_cpu.neon = 1; /* aarch64 always has NEON */
#endif
    ptpng_cpu.filter_sub = ptpng_filter_sub_scalar;
    ptpng_cpu.filter_up = ptpng_filter_up_scalar;
    ptpng_cpu.filter_paeth = ptpng_filter_paeth_scalar;
    ptpng_cpu.crc32 = ptpng_crc32_slice8;
    ptpng_cpu.adler32 = ptpng_adler32_scalar;
    ptpng_cpu.cvt_table_rgba8 = ptpng_cvt_table_rgba8_scalar;
    ptpng_cpu.cvt_table_rgb8 = ptpng_cvt_table_rgb8_scalar;
#if PTPNG_X64
    if (ptpng_cpu.sse2) {
        ptpng_cpu.filter_sub = ptpng_filter_sub_sse2;
        ptpng_cpu.filter_up = ptpng_filter_up_sse2;
        ptpng_cpu.adler32 = ptpng_adler32_sse2;
    }
#endif
#if PTPNG_X86
    ptpng_avx2_init();
    ptpng_crc_x86_init();
#endif
#if PTPNG_ARM_NEON
    ptpng_neon_init();
    ptpng_crc_arm_init();
#endif
}

uint32_t ptpng_crc32(const uint8_t *p, size_t n)
{
    if (!ptpng_cpu.crc32)
        ptpng_cpu_init();
    return ptpng_cpu.crc32(p, n);
}
uint32_t ptpng_adler32(const uint8_t *p, size_t n)
{
    if (!ptpng_cpu.adler32)
        ptpng_cpu_init();
    return ptpng_cpu.adler32(p, n);
}

const char *ptpng_version(void)
{
    return "ptpng 1.1"
#if PTPNG_X64
           " x64"
#elif PTPNG_X86
           " x86"
#elif PTPNG_ARM_NEON
           " arm-neon"
#else
           " portable"
#endif
           ;
}

/* space-separated list of the CPU features detected (and used) at
 * runtime; useful for benchmark labeling */
const char *ptpng_features(void)
{
    static char feats[64];
    if (!ptpng_cpu.crc32)
        ptpng_cpu_init();
    feats[0] = 0;
#if PTPNG_X86
    if (ptpng_cpu.avx2)
        strcat(feats, "avx2 ");
    if (ptpng_cpu.sse41)
        strcat(feats, "sse4.1 ");
    if (ptpng_cpu.ssse3)
        strcat(feats, "ssse3 ");
    if (ptpng_cpu.sse2)
        strcat(feats, "sse2 ");
    if (ptpng_cpu.pclmul)
        strcat(feats, "pclmul ");
    if (ptpng_cpu.bmi2)
        strcat(feats, "bmi2 ");
#endif
#if PTPNG_ARM_NEON
    strcat(feats, "neon ");
    if (ptpng_cpu.arm_crc32)
        strcat(feats, "crc32 ");
#endif
    if (!feats[0])
        strcat(feats, "scalar ");
    return feats;
}

const char *ptpng_strerror(int err)
{
    switch (err) {
    case PTPNG_OK: return "ok";
    case PTPNG_E_BAD_ARG: return "bad argument";
    case PTPNG_E_BAD_SIGNATURE: return "not a PNG file";
    case PTPNG_E_TRUNCATED: return "truncated file or chunk";
    case PTPNG_E_BAD_CHUNK_LEN: return "impossible chunk length";
    case PTPNG_E_BAD_CRC: return "CRC mismatch on critical chunk";
    case PTPNG_E_BAD_ZLIB_HEADER: return "bad zlib header";
    case PTPNG_E_BAD_ADLER: return "zlib adler32 mismatch";
    case PTPNG_E_INFLATE_CORRUPT: return "corrupt deflate stream";
    case PTPNG_E_INFLATE_SIZE: return "inflated size mismatch";
    case PTPNG_E_BAD_IHDR: return "invalid IHDR";
    case PTPNG_E_COLOR_DEPTH: return "invalid color type / bit depth";
    case PTPNG_E_NO_PLTE: return "palette image missing PLTE";
    case PTPNG_E_BAD_PLTE: return "malformed PLTE";
    case PTPNG_E_BAD_TRNS: return "malformed tRNS";
    case PTPNG_E_INTERLACE: return "invalid interlace method";
    case PTPNG_E_UNKNOWN_CRITICAL: return "unknown critical chunk";
    case PTPNG_E_OUT_OF_MEMORY: return "out of memory";
    case PTPNG_E_CHUNK_ORDER: return "chunk in wrong position";
    case PTPNG_E_NO_IDAT: return "no IDAT chunk";
    case PTPNG_E_NO_IEND: return "no IEND chunk";
    case PTPNG_E_TOO_LARGE: return "image exceeds size limit";
    case PTPNG_E_EMPTY_PLTE: return "zero-color palette";
    case PTPNG_E_BAD_HUFFMAN: return "invalid Huffman code lengths";
    case PTPNG_E_UNSUPPORTED: return "unsupported feature";
    }
    return "unknown error";
}

/* ================= geometry helpers ================= */

static const unsigned adam7_xstart[7] = {0,4,0,2,0,1,0};
static const unsigned adam7_ystart[7] = {0,0,4,0,2,0,1};
static const unsigned adam7_xstep[7]  = {8,8,4,4,2,2,1};
static const unsigned adam7_ystep[7]  = {8,8,8,4,4,2,2};

static unsigned channels_of(unsigned ct)
{
    switch (ct) {
    case 0: case 3: return 1;
    case 2: return 3;
    case 4: return 2;
    case 6: return 4;
    }
    return 0;
}

static int depth_ok(unsigned ct, unsigned d)
{
    switch (ct) {
    case 0: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
    case 2: case 4: case 6: return d == 8 || d == 16;
    case 3: return d == 1 || d == 2 || d == 4 || d == 8;
    }
    return 0;
}

/* ================= conversions (scalar) ================= */

static const uint8_t gray1_lut[2] = {0, 255};
static const uint8_t gray2_lut[4] = {0, 85, 170, 255};
static const uint8_t gray4_lut[16] = {0,17,34,51,68,85,102,119,136,153,
                                      170,187,204,221,238,255};

#define BIT_AT(src, i, d) \
    ((d) == 1 ? (unsigned)((src)[(i) >> 3] >> (7 - ((i) & 7))) & 1 \
   : (d) == 2 ? (unsigned)((src)[(i) >> 2] >> (6 - (((i) & 3) << 1))) & 3 \
   : (d) == 4 ? (unsigned)((i) & 1 ? ((src)[(i) >> 1] & 15) \
                                    : ((src)[(i) >> 1] >> 4)) \
              : (unsigned)((src)[(i)]))

/* ---- to RGBA8 ---- */
#define GRAY_TO_RGBA8(d)                                                  \
static void rgba8_g##d(const uint8_t *src, uint8_t *dst, uint32_t n,      \
                       const struct ptpng_cvt *c)                         \
{                                                                         \
    uint16_t mask = c->trns_mask;                                         \
    uint32_t i;                                                           \
    for (i = 0; i < n; i++) {                                             \
        unsigned v = BIT_AT(src, i, d);                                   \
        uint8_t g = (d == 1) ? gray1_lut[v] : (d == 2) ? gray2_lut[v]     \
                                                       : gray4_lut[v];     \
        dst[0] = dst[1] = dst[2] = g;                                     \
        dst[3] = (c->has_trns && v == mask) ? 0 : 255;                    \
        dst += 4;                                                         \
    }                                                                     \
}
GRAY_TO_RGBA8(1)
GRAY_TO_RGBA8(2)
GRAY_TO_RGBA8(4)

static void rgba8_g8(const uint8_t *src, uint8_t *dst, uint32_t n,
                     const struct ptpng_cvt *c)
{
    uint16_t mask = c->trns_mask;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint8_t g = src[i];
        dst[0] = dst[1] = dst[2] = g;
        dst[3] = (c->has_trns && g == (mask & 0xFF)) ? 0 : 255;
        dst += 4;
    }
}

static void rgba8_g16(const uint8_t *src, uint8_t *dst, uint32_t n,
                      const struct ptpng_cvt *c)
{
    uint16_t mask = c->trns_mask;
    uint32_t i;
    for (i = 0; i < n; i++) {
        uint8_t g = src[i * 2];
        dst[0] = dst[1] = dst[2] = g;
        dst[3] = (c->has_trns &&
                  (unsigned)((src[i * 2] << 8) | src[i * 2 + 1]) == mask)
                     ? 0 : 255;
        dst += 4;
    }
}

static void rgba8_rgb8(const uint8_t *src, uint8_t *dst, uint32_t n,
                       const struct ptpng_cvt *c)
{
    uint16_t r = c->trns_r, g = c->trns_g, b = c->trns_b;
    uint32_t i;
#if PTPNG_REUSE_CVT_BUFFER
    if (c->reverse) {
        src += (size_t)n * 3; dst += (size_t)n * 4;
        while (n--) {
            uint8_t sr, sg, sb;
            src -= 3; dst -= 4;
            sr = src[0]; sg = src[1]; sb = src[2];
            dst[0] = sr; dst[1] = sg; dst[2] = sb;
            dst[3] = (c->has_trns && sr == (r & 0xFF) &&
                      sg == (g & 0xFF) && sb == (b & 0xFF)) ? 0 : 255;
        }
        return;
    }
#endif
    for (i = 0; i < n; i++) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst[3] = (c->has_trns && src[0] == (r & 0xFF) &&
                  src[1] == (g & 0xFF) && src[2] == (b & 0xFF)) ? 0 : 255;
        src += 3; dst += 4;
    }
}

static void rgba8_rgb16(const uint8_t *src, uint8_t *dst, uint32_t n,
                        const struct ptpng_cvt *c)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        dst[0] = src[0]; dst[1] = src[2]; dst[2] = src[4];
        dst[3] = (c->has_trns &&
                  pt_be16(src) == c->trns_r && pt_be16(src + 2) == c->trns_g &&
                  pt_be16(src + 4) == c->trns_b) ? 0 : 255;
        src += 6; dst += 4;
    }
}

#define PAL_TO_RGBA8(d)                                                   \
static void rgba8_p##d(const uint8_t *src, uint8_t *dst, uint32_t n,      \
                       const struct ptpng_cvt *c)                         \
{                                                                         \
    const uint8_t *pal = c->palette;                                      \
    const uint8_t *trns = c->trans;                                       \
    uint32_t nt = c->num_trans;                                           \
    uint32_t i;                                                           \
    for (i = 0; i < n; i++) {                                             \
        unsigned v = BIT_AT(src, i, d);                                   \
        dst[0] = pal[v * 3];                                              \
        dst[1] = pal[v * 3 + 1];                                          \
        dst[2] = pal[v * 3 + 2];                                          \
        dst[3] = (v < nt) ? trns[v] : 255;                                \
        dst += 4;                                                         \
    }                                                                     \
}
PAL_TO_RGBA8(1)
PAL_TO_RGBA8(2)
PAL_TO_RGBA8(4)
PAL_TO_RGBA8(8)

static void rgba8_ga8(const uint8_t *src, uint8_t *dst, uint32_t n,
                      const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        uint8_t g = src[0];
        dst[0] = dst[1] = dst[2] = g;
        dst[3] = src[1];
        src += 2; dst += 4;
    }
}

static void rgba8_ga16(const uint8_t *src, uint8_t *dst, uint32_t n,
                       const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        uint8_t g = src[0];
        dst[0] = dst[1] = dst[2] = g;
        dst[3] = src[2];
        src += 4; dst += 4;
    }
}

static void rgba8_rgba8(const uint8_t *src, uint8_t *dst, uint32_t n,
                        const struct ptpng_cvt *c)
{
    (void)c;
    memcpy(dst, src, (size_t)n * 4);
}

static void rgba8_rgba16(const uint8_t *src, uint8_t *dst, uint32_t n,
                         const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        dst[0] = src[0]; dst[1] = src[2]; dst[2] = src[4]; dst[3] = src[6];
        src += 8; dst += 4;
    }
}

/* ---- to RGB8 ---- */
#define GRAY_TO_RGB8(d)                                                   \
static void rgb8_g##d(const uint8_t *src, uint8_t *dst, uint32_t n,       \
                      const struct ptpng_cvt *c)                          \
{                                                                         \
    uint32_t i;                                                           \
    (void)c;                                                              \
    for (i = 0; i < n; i++) {                                             \
        unsigned v = BIT_AT(src, i, d);                                   \
        uint8_t g = (d == 1) ? gray1_lut[v] : (d == 2) ? gray2_lut[v]     \
                                                       : gray4_lut[v];     \
        *dst++ = g; *dst++ = g; *dst++ = g;                               \
    }                                                                     \
}
GRAY_TO_RGB8(1)
GRAY_TO_RGB8(2)
GRAY_TO_RGB8(4)

static void rgb8_g8(const uint8_t *src, uint8_t *dst, uint32_t n,
                    const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        uint8_t g = src[i];
        *dst++ = g; *dst++ = g; *dst++ = g;
    }
}

static void rgb8_g16(const uint8_t *src, uint8_t *dst, uint32_t n,
                     const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        uint8_t g = src[i * 2];
        *dst++ = g; *dst++ = g; *dst++ = g;
    }
}

static void rgb8_rgb8_(const uint8_t *src, uint8_t *dst, uint32_t n,
                       const struct ptpng_cvt *c)
{
    (void)c;
    memcpy(dst, src, (size_t)n * 3);
}

static void rgb8_rgb16(const uint8_t *src, uint8_t *dst, uint32_t n,
                       const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        *dst++ = src[0]; *dst++ = src[2]; *dst++ = src[4];
        src += 6;
    }
}

#define PAL_TO_RGB8(d)                                                    \
static void rgb8_p##d(const uint8_t *src, uint8_t *dst, uint32_t n,       \
                      const struct ptpng_cvt *c)                          \
{                                                                         \
    const uint8_t *pal = c->pal_rgba;                                     \
    uint32_t i;                                                           \
    /* The next pixel overwrites the fourth byte; keep the final store   \
     * exact so no write crosses the output row. */                       \
    for (i = 0; i + 1 < n; i++) {                                         \
        unsigned v = BIT_AT(src, i, d);                                   \
        memcpy(dst, pal + v * 4, 4);                                      \
        dst += 3;                                                         \
    }                                                                     \
    if (i < n) {                                                          \
        unsigned v = BIT_AT(src, i, d);                                   \
        memcpy(dst, pal + v * 4, 3);                                      \
    }                                                                     \
}
PAL_TO_RGB8(1)
PAL_TO_RGB8(2)
PAL_TO_RGB8(4)
PAL_TO_RGB8(8)

static void rgb8_ga8(const uint8_t *src, uint8_t *dst, uint32_t n,
                     const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        uint8_t g = src[0];
        *dst++ = g; *dst++ = g; *dst++ = g;
        src += 2;
    }
}

static void rgb8_ga16(const uint8_t *src, uint8_t *dst, uint32_t n,
                      const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        uint8_t g = src[0];
        *dst++ = g; *dst++ = g; *dst++ = g;
        src += 4;
    }
}

static void rgb8_rgba8(const uint8_t *src, uint8_t *dst, uint32_t n,
                       const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        src += 4; dst += 3;
    }
}

static void rgb8_rgba16(const uint8_t *src, uint8_t *dst, uint32_t n,
                        const struct ptpng_cvt *c)
{
    uint32_t i;
    (void)c;
    for (i = 0; i < n; i++) {
        dst[0] = src[0]; dst[1] = src[2]; dst[2] = src[4];
        src += 8; dst += 3;
    }
}

/* index = (color_type << 4) | log2(depth) */
#define DC(d) ((d) == 1 ? 0 : (d) == 2 ? 1 : (d) == 4 ? 2 : (d) == 8 ? 3 : 4)

const ptpng_cvt_fn ptpng_cvt_table_rgba8_scalar[128] = {
    [(0<<4)|DC(1)]  = rgba8_g1,  [(0<<4)|DC(2)]  = rgba8_g2,
    [(0<<4)|DC(4)]  = rgba8_g4,  [(0<<4)|DC(8)]  = rgba8_g8,
    [(0<<4)|DC(16)] = rgba8_g16,
    [(2<<4)|DC(8)]  = rgba8_rgb8, [(2<<4)|DC(16)] = rgba8_rgb16,
    [(3<<4)|DC(1)]  = rgba8_p1,  [(3<<4)|DC(2)]  = rgba8_p2,
    [(3<<4)|DC(4)]  = rgba8_p4,  [(3<<4)|DC(8)]  = rgba8_p8,
    [(4<<4)|DC(8)]  = rgba8_ga8, [(4<<4)|DC(16)] = rgba8_ga16,
    [(6<<4)|DC(8)]  = rgba8_rgba8, [(6<<4)|DC(16)] = rgba8_rgba16,
};

const ptpng_cvt_fn ptpng_cvt_table_rgb8_scalar[128] = {
    [(0<<4)|DC(1)]  = rgb8_g1,   [(0<<4)|DC(2)]  = rgb8_g2,
    [(0<<4)|DC(4)]  = rgb8_g4,   [(0<<4)|DC(8)]  = rgb8_g8,
    [(0<<4)|DC(16)] = rgb8_g16,
    [(2<<4)|DC(8)]  = rgb8_rgb8_, [(2<<4)|DC(16)] = rgb8_rgb16,
    [(3<<4)|DC(1)]  = rgb8_p1,   [(3<<4)|DC(2)]  = rgb8_p2,
    [(3<<4)|DC(4)]  = rgb8_p4,   [(3<<4)|DC(8)]  = rgb8_p8,
    [(4<<4)|DC(8)]  = rgb8_ga8,  [(4<<4)|DC(16)] = rgb8_ga16,
    [(6<<4)|DC(8)]  = rgb8_rgba8, [(6<<4)|DC(16)] = rgb8_rgba16,
};

/* ================= Adam7 extraction ================= */

static void extract_pass_row(uint8_t *dst_row, const uint8_t *src_row,
                             uint32_t pw, unsigned xstart, unsigned xstep,
                             unsigned bytes_pp)
{
    uint32_t k;
    if (xstep == 1) {
        memcpy(dst_row + (size_t)xstart * bytes_pp, src_row,
               (size_t)pw * bytes_pp);
        return;
    }
    if (bytes_pp == 1) {
        for (k = 0; k < pw; k++)
            dst_row[xstart + k * xstep] = src_row[k];
        return;
    }
    /* 2, 3, 4, 6, 8 bytes per pixel */
    for (k = 0; k < pw; k++)
        memcpy(dst_row + (size_t)(xstart + k * xstep) * bytes_pp,
               src_row + (size_t)k * bytes_pp, bytes_pp);
}

static void extract_pass_row_subbyte(uint8_t *dst_row, const uint8_t *src_row,
                                     uint32_t pw, unsigned xstart,
                                     unsigned xstep, unsigned depth)
{
    uint32_t k;
    unsigned mask = (1u << depth) - 1;
    for (k = 0; k < pw; k++) {
        unsigned x = xstart + k * xstep;
        unsigned v = (src_row[(k * depth) >> 3] >>
                      (8 - depth - ((k * depth) & 7))) & mask;
        unsigned ob = (x * depth) >> 3;
        unsigned osh = 8 - depth - ((x * depth) & 7);
        dst_row[ob] = (uint8_t)((dst_row[ob] & ~(mask << osh)) | (v << osh));
    }
}

/* ================= unfiltering ================= */

/* Reconstruct scanlines in the raw (filtered) buffer.
 * non-interlaced: rows are compacted in place (filter bytes dropped).
 * interlaced: rows are reconstructed in place, filter bytes kept. */
static int unfilter_image(uint8_t *raw, uint32_t w, uint32_t h,
                          unsigned depth, unsigned channels,
                          int interlaced, uint8_t *zeros)
{
    unsigned bpp = (channels * depth + 7) >> 3;
    unsigned bitpp = channels * depth;
    ptpng_filter_fn fsub = ptpng_cpu.filter_sub;
    ptpng_filter_fn fup = ptpng_cpu.filter_up;
    ptpng_filter_fn fpaeth = ptpng_cpu.filter_paeth;

    if (!interlaced) {
        size_t rb = (size_t)((((uint64_t)w * bitpp) + 7) >> 3);
        uint32_t y;
        const uint8_t *prev = zeros;
        for (y = 0; y < h; y++) {
            uint8_t *frow = raw + (size_t)y * (rb + 1);
            unsigned f = frow[0];
            uint8_t *dst = raw + (size_t)y * rb; /* compacted position */
            const uint8_t *src = frow + 1;
            if (f > 4)
                return PTPNG_E_INFLATE_CORRUPT;
            if (y)
                prev = raw + (size_t)(y - 1) * rb;
            switch (f) {
            case 0: memmove(dst, src, rb); break;
            case 1: fsub(dst, src, prev, rb, bpp); break;
            case 2: fup(dst, src, prev, rb, bpp); break;
            case 3: ptpng_filter_avg_scalar(dst, src, prev, rb, bpp); break;
            case 4: fpaeth(dst, src, prev, rb, bpp); break;
            }
        }
        return PTPNG_OK;
    } else {
        unsigned p;
        size_t off = 0;
        for (p = 0; p < 7; p++) {
            uint32_t pw = (w > adam7_xstart[p])
                ? (w - adam7_xstart[p] + adam7_xstep[p] - 1) / adam7_xstep[p]
                : 0;
            uint32_t ph = (h > adam7_ystart[p])
                ? (h - adam7_ystart[p] + adam7_ystep[p] - 1) / adam7_ystep[p]
                : 0;
            size_t rbp;
            uint32_t y;
            const uint8_t *prev = zeros;
            if (pw == 0 || ph == 0)
                continue;
            rbp = (size_t)((((uint64_t)pw * bitpp) + 7) >> 3);
            for (y = 0; y < ph; y++) {
                uint8_t *frow = raw + off + (size_t)y * (rbp + 1);
                unsigned f = frow[0];
                uint8_t *row = frow + 1;
                if (f > 4)
                    return PTPNG_E_INFLATE_CORRUPT;
                switch (f) {
                case 0: break;
                case 1: fsub(row, row, prev, rbp, bpp); break;
                case 2: fup(row, row, prev, rbp, bpp); break;
                case 3: ptpng_filter_avg_scalar(row, row, prev, rbp, bpp); break;
                case 4: fpaeth(row, row, prev, rbp, bpp); break;
                }
                prev = row;
            }
            off += (size_t)ph * (rbp + 1);
        }
        return PTPNG_OK;
    }
}

/* Extract Adam7 passes from the (unfiltered) raw buffer into out. */
static void adam7_extract(uint8_t *out, const uint8_t *raw, uint32_t w,
                          uint32_t h, unsigned depth, unsigned channels)
{
    unsigned bitpp = channels * depth;
    size_t rb = (size_t)(((uint64_t)w * bitpp + 7) >> 3);
    unsigned p;
    size_t off = 0;
    for (p = 0; p < 7; p++) {
        uint32_t pw = (w > adam7_xstart[p])
            ? (w - adam7_xstart[p] + adam7_xstep[p] - 1) / adam7_xstep[p]
            : 0;
        uint32_t ph = (h > adam7_ystart[p])
            ? (h - adam7_ystart[p] + adam7_ystep[p] - 1) / adam7_ystep[p]
            : 0;
        size_t rbp;
        uint32_t y;
        if (pw == 0 || ph == 0)
            continue;
        rbp = (size_t)(((uint64_t)pw * bitpp + 7) >> 3);
        for (y = 0; y < ph; y++) {
            const uint8_t *src = raw + off + (size_t)y * (rbp + 1) + 1;
            uint8_t *dst = out + (size_t)(adam7_ystart[p] + y * adam7_ystep[p]) * rb;
            if (depth < 8)
                extract_pass_row_subbyte(dst, src, pw, adam7_xstart[p],
                                         adam7_xstep[p], depth);
            else
                extract_pass_row(dst, src, pw, adam7_xstart[p],
                                 adam7_xstep[p], bitpp >> 3);
        }
        off += (size_t)ph * (rbp + 1);
    }
}

/* zero the unused low bits of each row's final byte (libpng parity) */
static void zero_pad_bits(uint8_t *buf, uint32_t w, uint32_t h,
                          unsigned depth, unsigned channels)
{
    unsigned bitpp = channels * depth;
    unsigned tail = ((uint64_t)w * bitpp) & 7;
    size_t rb;
    uint8_t mask;
    uint32_t y;
    if (depth >= 8 || tail == 0)
        return;
    rb = (((size_t)w * bitpp) + 7) >> 3;
    mask = (uint8_t)(0xFFu << (8 - tail));
    for (y = 0; y < h; y++)
        buf[(size_t)y * rb + (rb - 1)] &= mask;
}

/* ================= ancillary allocation tracking ================= */

#define PT_MAX_ALLOC 64

static void *pt_track(ptpng_info *info, size_t sz)
{
    void *p;
    if (info->_n_allocs >= PT_MAX_ALLOC)
        return NULL;
    p = malloc(sz);
    if (!p)
        return NULL;
    info->_allocs[info->_n_allocs++] = p;
    return p;
}

static int pt_adopt(ptpng_info *info, void *p)
{
    if (!p)
        return 0;
    if (info->_n_allocs >= PT_MAX_ALLOC) {
        free(p);
        return 0;
    }
    info->_allocs[info->_n_allocs++] = p;
    return 1;
}

static char *pt_track_strndup(ptpng_info *info, const char *s, size_t n)
{
    char *p = (char *)pt_track(info, n + 1);
    if (!p)
        return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void ptpng_info_free(ptpng_info *info)
{
    if (!info)
        return;
    while (info->_n_allocs > 0)
        free(info->_allocs[--info->_n_allocs]);
}

/* duplicate a NUL-terminated substring starting at *pp, bounded by end.
 * advances *pp past the NUL. returns NULL if no NUL found. */
static const uint8_t *find_nul(const uint8_t *p, const uint8_t *end)
{
    while (p < end)
        if (!*p++)
            return p; /* one past the NUL */
    return NULL;
}

/* ================= main decode ================= */

int ptpng_decode(const void *data, size_t size, const ptpng_opts *opts,
                 void **out, size_t *out_len, ptpng_info *info)
{
    const uint8_t *buf = (const uint8_t *)data;
    ptpng_opts def;
    ptpng_info linfo;
    uint32_t w = 0, h = 0;
    unsigned depth = 0, ct = 0, interlace = 0;
    int have_ihdr = 0, have_iend = 0, idat_state = 0; /* 0 none, 1 in, 2 done */
    size_t idat_total = 0;
    const uint8_t *idat_first = NULL;
    uint8_t *idat_buf = NULL;
    struct idat_ref { const uint8_t *p; size_t len; };
    struct idat_ref *idat_refs = NULL;
    size_t n_idat_refs = 0, cap_idat_refs = 0;
    int rc;
    uint64_t bitpp, raw_size, rb64, out_size;
    size_t rb;
    uint8_t *raw, *zeros, *native;
    int expand_in_place;

    if (!data || !out)
        return PTPNG_E_BAD_ARG;
    *out = NULL;
    if (out_len)
        *out_len = 0;

    ptpng_cpu_init();

    if (!opts) {
        memset(&def, 0, sizeof(def));
        def.output_format = PTPNG_OUT_NATIVE;
        opts = &def;
    }
    if (opts->max_bytes == 0 || opts->output_format == 0) {
        def = *opts; /* writable copy with defaults filled in */
        if (def.max_bytes == 0)
            def.max_bytes = PTPNG_DEFAULT_MAX_BYTES;
        if (def.output_format == 0)
            def.output_format = PTPNG_OUT_NATIVE;
        opts = &def;
    }
    if (opts->output_format != PTPNG_OUT_NATIVE &&
        opts->output_format != PTPNG_OUT_RGBA8 &&
        opts->output_format != PTPNG_OUT_RGB8)
        return PTPNG_E_BAD_ARG;
    if (!opts->max_bytes)
        return PTPNG_E_BAD_ARG;

    memset(&linfo, 0, sizeof(linfo));

    if (size < 8 || memcmp(buf, png_sig, 8) != 0)
        return PTPNG_E_BAD_SIGNATURE;

    {
    unsigned long long P0 = 0, P1 = 0, P2 = 0, P3 = 0, P4;
    int prof = getenv("PTPNG_PROF") != NULL;
    if (prof) P0 = pt_tsc();

    /* ---- chunk loop ---- */
    {
        size_t pos = 8;
        while (pos + 8 <= size) {
            uint32_t len = pt_be32(buf + pos);
            uint32_t type = pt_be32(buf + pos + 4);
            const uint8_t *cdata = buf + pos + 8;
            int critical = pt_chunk_critical(type);
            int skip_chunk = 0;

            if (len > 0x7FFFFFFFu)
                { rc = PTPNG_E_BAD_CHUNK_LEN; goto fail_cleanup; }
            if ((uint64_t)pos + 12 + len > size)
                { rc = PTPNG_E_TRUNCATED; goto fail_cleanup; }

            if (!(opts->flags & PTPNG_FLAG_NO_VERIFY_CRC)) {
                uint32_t want = pt_be32(cdata + len);
                uint32_t got = ptpng_crc32(buf + pos + 4, (size_t)len + 4);
                if (got != want) {
                    if (critical)
                        { rc = PTPNG_E_BAD_CRC; goto fail_cleanup; }
                    linfo.crc_warnings++;
                    skip_chunk = 1; /* ancillary: warn and drop */
                }
            }

            if (!skip_chunk) {
                if (!have_ihdr) {
                    if (type != CH_IHDR)
                        { rc = PTPNG_E_CHUNK_ORDER; goto fail_cleanup; }
                } else if (type == CH_IHDR) {
                    rc = PTPNG_E_CHUNK_ORDER;
                    goto fail_cleanup; /* duplicate */
                }

                switch (type) {
                case CH_IHDR:
                    if (len != 13)
                        { rc = PTPNG_E_BAD_IHDR; goto fail_cleanup; }
                    w = pt_be32(cdata);
                    h = pt_be32(cdata + 4);
                    depth = cdata[8];
                    ct = cdata[9];
                    if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu)
                        { rc = PTPNG_E_BAD_IHDR; goto fail_cleanup; }
                    if (!depth_ok(ct, depth))
                        { rc = PTPNG_E_COLOR_DEPTH; goto fail_cleanup; }
                    if (cdata[10] != 0) /* compression */
                        { rc = PTPNG_E_BAD_IHDR; goto fail_cleanup; }
                    if (cdata[11] != 0) /* filter */
                        { rc = PTPNG_E_BAD_IHDR; goto fail_cleanup; }
                    if (cdata[12] > 1)
                        { rc = PTPNG_E_INTERLACE; goto fail_cleanup; }
                    interlace = cdata[12];
                    have_ihdr = 1;
                    linfo.width = w;
                    linfo.height = h;
                    linfo.bit_depth = (uint8_t)depth;
                    linfo.color_type = (uint8_t)ct;
                    linfo.interlace = (uint8_t)interlace;
                    linfo.channels = (uint8_t)channels_of(ct);
                    break;

                case CH_PLTE: {
                    unsigned ncol;
                    if (idat_state)
                        { rc = PTPNG_E_CHUNK_ORDER; goto fail_cleanup; }
                    if (len == 0 || len % 3 != 0 || len > 768)
                        { rc = PTPNG_E_BAD_PLTE; goto fail_cleanup; }
                    ncol = len / 3;
                    if (ct == 0 || ct == 4)
                        break; /* not allowed: ignore (libpng benign) */
                    memcpy(linfo.palette, cdata, len);
                    linfo.num_palette = (uint16_t)ncol;
                    break;
                }

                case CH_IDAT:
                    if (!have_ihdr || idat_state == 2)
                        { rc = PTPNG_E_CHUNK_ORDER; goto fail_cleanup; }
                    if (ct == 3 && linfo.num_palette == 0)
                        { rc = PTPNG_E_NO_PLTE; goto fail_cleanup; }
                    idat_state = 1;
                    if (n_idat_refs == cap_idat_refs) {
                        size_t nc = cap_idat_refs ? cap_idat_refs * 2 : 16;
                        struct idat_ref *nr = (struct idat_ref *)realloc(
                            idat_refs, nc * sizeof(*nr));
                        if (!nr) {
                            { rc = PTPNG_E_OUT_OF_MEMORY; goto fail_cleanup; }
                        }
                        idat_refs = nr;
                        cap_idat_refs = nc;
                    }
                    idat_refs[n_idat_refs].p = cdata;
                    idat_refs[n_idat_refs].len = len;
                    n_idat_refs++;
                    idat_total += len;
                    break;

                case CH_IEND:
                    if (len != 0)
                        { rc = PTPNG_E_BAD_CHUNK_LEN; goto fail_cleanup; }
                    have_iend = 1;
                    pos = size; /* ignore anything after IEND */
                    continue;

                case CH_TRNS:
                    if (idat_state)
                        { rc = PTPNG_E_CHUNK_ORDER; goto fail_cleanup; }
                    if (ct == 0) {
                        if (len != 2)
                            { rc = PTPNG_E_BAD_TRNS; goto fail_cleanup; }
                        linfo.trns[0] = pt_be16(cdata);
                        if (linfo.trns[0] >= (1u << depth))
                            break; /* invalid: ignore like libpng */
                        linfo.has_trns = 1;
                    } else if (ct == 2) {
                        if (len != 6)
                            { rc = PTPNG_E_BAD_TRNS; goto fail_cleanup; }
                        linfo.trns[0] = pt_be16(cdata);
                        linfo.trns[1] = pt_be16(cdata + 2);
                        linfo.trns[2] = pt_be16(cdata + 4);
                        if (linfo.trns[0] >= (1u << depth) ||
                            linfo.trns[1] >= (1u << depth) ||
                            linfo.trns[2] >= (1u << depth))
                            break;
                        linfo.has_trns = 1;
                    } else if (ct == 3) {
                        if (linfo.num_palette == 0)
                            { rc = PTPNG_E_CHUNK_ORDER; goto fail_cleanup; }
                        if (len > (uint32_t)linfo.num_palette)
                            break; /* exceeds palette: ignore (libpng) */
                        memcpy(linfo.palette + 768, cdata, len);
                        linfo.num_trans = (uint16_t)len;
                    }
                    /* ct 4/6: invalid; ignore */
                    break;

                case CH_GAMA:
                    if (idat_state || len != 4)
                        break;
                    linfo.gAMA = pt_be32(cdata);
                    linfo.has_gAMA = 1;
                    break;

                case CH_CHRM:
                    if (idat_state || len != 32)
                        break;
                    for (unsigned i = 0; i < 8; i++)
                        linfo.cHRM[i] = pt_be32(cdata + i * 4);
                    linfo.has_cHRM = 1;
                    break;

                case CH_SRGB:
                    if (idat_state || len != 1 || cdata[0] > 3)
                        break;
                    linfo.sRGB_intent = cdata[0];
                    linfo.has_sRGB = 1;
                    break;

                case CH_SBIT: {
                    unsigned want = (ct == 0) ? 1 : (ct == 2 || ct == 3) ? 3
                                   : (ct == 4) ? 2 : 4;
                    if (idat_state || len != want)
                        break;
                    for (unsigned i = 0; i < want; i++) {
                        unsigned lim = (ct == 3) ? 8 : depth;
                        if (cdata[i] == 0 || cdata[i] > lim)
                            break;
                        if (i + 1 == want) {
                            memcpy(linfo.sBIT, cdata, want);
                            linfo.has_sBIT = 1;
                        }
                    }
                    break;
                }

                case CH_BKGD:
                    if (idat_state)
                        break;
                    if (ct == 3) {
                        if (len != 1 || linfo.num_palette == 0 ||
                            cdata[0] >= linfo.num_palette)
                            break;
                        linfo.bKGD[0] = cdata[0];
                        linfo.has_bKGD = 1;
                    } else if (ct == 0) {
                        if (len != 2)
                            break;
                        linfo.bKGD[0] = pt_be16(cdata);
                        if (linfo.bKGD[0] < (1u << depth))
                            linfo.has_bKGD = 1;
                    } else if (ct == 2) {
                        if (len != 6)
                            break;
                        linfo.bKGD[0] = pt_be16(cdata);
                        linfo.bKGD[1] = pt_be16(cdata + 2);
                        linfo.bKGD[2] = pt_be16(cdata + 4);
                        if (linfo.bKGD[0] < (1u << depth) &&
                            linfo.bKGD[1] < (1u << depth) &&
                            linfo.bKGD[2] < (1u << depth))
                            linfo.has_bKGD = 1;
                    }
                    break;

                case CH_PHYS:
                    if (idat_state || len != 9 || cdata[8] > 1)
                        break;
                    linfo.pHYs[0] = pt_be32(cdata);
                    linfo.pHYs[1] = pt_be32(cdata + 4);
                    linfo.pHYs[2] = cdata[8];
                    linfo.has_pHYs = 1;
                    break;

                case CH_TIME:
                    if (len != 7)
                        break;
                    if (cdata[2] < 1 || cdata[2] > 12 ||
                        cdata[3] < 1 || cdata[3] > 31 || cdata[4] > 23 ||
                        cdata[5] > 59 || cdata[6] > 60)
                        break;
                    memcpy(linfo.tIME, cdata, 7);
                    linfo.has_tIME = 1;
                    break;

                case CH_HIST: {
                    size_t i;
                    uint16_t *ht;
                    if (idat_state || len % 2 != 0 ||
                        len / 2 != linfo.num_palette || linfo.num_palette == 0 ||
                        linfo.has_hIST || linfo._n_allocs >= PT_MAX_ALLOC)
                        break;
                    ht = (uint16_t *)pt_track(&linfo, len);
                    linfo.hIST = ht;
                    if (!ht)
                        { rc = PTPNG_E_OUT_OF_MEMORY; goto fail_cleanup; }
                    for (i = 0; i < len / 2; i++)
                        ht[i] = pt_be16(cdata + i * 2);
                    linfo.num_hIST = (uint16_t)(len / 2);
                    linfo.has_hIST = 1;
                    break;
                }

                case CH_TEXT:
                case CH_ZTXT:
                case CH_ITXT: {
                    const uint8_t *p = cdata, *end = cdata + len;
                    const uint8_t *after_kw;
                    ptpng_text *t;
                    if (linfo.num_texts >= PTPNG_MAX_TEXT ||
                        linfo._n_allocs > PT_MAX_ALLOC - (type == CH_ITXT ? 4 : 2))
                        break;
                    after_kw = find_nul(p, end);
                    if (!after_kw || after_kw - p - 1 > 79 ||
                        after_kw - p == 1)
                        break;
                    t = &linfo.texts[linfo.num_texts];
                    memset(t, 0, sizeof(*t));
                    t->keyword = pt_track_strndup(&linfo, (const char *)p,
                                                  (size_t)(after_kw - p - 1));
                    p = after_kw;
                    if (type == CH_TEXT) {
                        t->text = pt_track_strndup(&linfo, (const char *)p,
                                                   (size_t)(end - p));
                    } else if (type == CH_ZTXT) {
                        uint8_t *dec; size_t dlen;
                        if (p >= end || *p != 0)
                            break; /* compression method must be 0 */
                        p++;
                        if (ptpng_inflate_dyn(p, (size_t)(end - p),
                                              (1u << 22), &dec, &dlen) ||
                            !pt_adopt(&linfo, dec))
                            break;
                        t->text = (const char *)dec;
                        t->compressed = 1;
                    } else { /* iTXt */
                        unsigned cflag, cmethod;
                        const uint8_t *after_lang, *after_trans;
                        if (p + 2 > end)
                            break;
                        cflag = p[0]; cmethod = p[1];
                        if (cflag > 1 || cmethod != 0)
                            break;
                        p += 2;
                        after_lang = find_nul(p, end);
                        if (!after_lang)
                            break;
                        t->language = pt_track_strndup(&linfo,
                            (const char *)p,
                            (size_t)(after_lang - p - 1));
                        p = after_lang;
                        after_trans = find_nul(p, end);
                        if (!after_trans)
                            break;
                        t->translated = pt_track_strndup(&linfo,
                            (const char *)p,
                            (size_t)(after_trans - p - 1));
                        p = after_trans;
                        if (cflag) {
                            uint8_t *dec; size_t dlen;
                            if (ptpng_inflate_dyn(p, (size_t)(end - p),
                                                  (1u << 22), &dec, &dlen) ||
                                !pt_adopt(&linfo, dec))
                                break;
                            t->text = (const char *)dec;
                            t->compressed = 1;
                        } else {
                            t->text = pt_track_strndup(&linfo,
                                (const char *)p, (size_t)(end - p));
                        }
                    }
                    if (!t->keyword || !t->text)
                        { rc = PTPNG_E_OUT_OF_MEMORY; goto fail_cleanup; }
                    linfo.num_texts++;
                    break;
                }

                case CH_SPLT: {
                    const uint8_t *p = cdata, *end = cdata + len;
                    const uint8_t *after_kw;
                    ptpng_splt *s;
                    size_t posn, esz;
                    if (linfo.num_splts >= PTPNG_MAX_SPLT ||
                        linfo._n_allocs > PT_MAX_ALLOC - 2)
                        break;
                    after_kw = find_nul(p, end);
                    if (!after_kw || after_kw - p == 1 || after_kw - p - 1 > 79)
                        break;
                    p = after_kw;
                    if (p >= end || (*p != 8 && *p != 16))
                        break;
                    esz = (*p == 8) ? 6 : 10;
                    p++;
                    if (p == end || (size_t)(end - p) % esz != 0)
                        break;
                    s = &linfo.splts[linfo.num_splts];
                    memset(s, 0, sizeof(*s));
                    posn = (size_t)(p - cdata);
                    s->entries = cdata; /* replaced below with a copy */
                    s->nentries = (uint32_t)((end - p) / esz);
                    s->name = pt_track_strndup(&linfo, (const char *)cdata,
                                               (size_t)(after_kw - cdata - 1));
                    {
                        uint8_t *cp = (uint8_t *)pt_track(&linfo, len);
                        if (!s->name || !cp)
                            { rc = PTPNG_E_OUT_OF_MEMORY; goto fail_cleanup; }
                        memcpy(cp, cdata, len);
                        s->entries = cp + posn;
                    }
                    s->sample_depth = (uint8_t)(esz == 6 ? 8 : 16);
                    linfo.num_splts++;
                    break;
                }

                case CH_ICCP: {
                    const uint8_t *p = cdata, *end = cdata + len;
                    const uint8_t *after_kw;
                    uint8_t *dec; size_t dlen;
                    if (idat_state || linfo.has_iCCP ||
                        linfo._n_allocs >= PT_MAX_ALLOC)
                        break;
                    after_kw = find_nul(p, end);
                    if (!after_kw || after_kw - p - 1 > 79)
                        break;
                    p = after_kw;
                    if (p >= end || *p != 0)
                        break;
                    p++;
                    if (ptpng_inflate_dyn(p, (size_t)(end - p), (1u << 25),
                                          &dec, &dlen) ||
                        !pt_adopt(&linfo, dec))
                        break;
                    linfo.iccp_profile = dec;
                    linfo.iccp_profile_len = dlen;
                    {
                        size_t klen = (size_t)(after_kw - cdata - 1);
                        if (klen >= sizeof(linfo.iccp_name))
                            klen = sizeof(linfo.iccp_name) - 1;
                        memcpy(linfo.iccp_name, cdata, klen);
                        linfo.iccp_name[klen] = 0;
                    }
                    linfo.has_iCCP = 1;
                    break;
                }

                case CH_EXIF:
                    if (linfo.has_eXIf || len == 0 ||
                        linfo._n_allocs >= PT_MAX_ALLOC)
                        break;
                    {
                        uint8_t *cp = (uint8_t *)pt_track(&linfo, len);
                        if (!cp)
                            { rc = PTPNG_E_OUT_OF_MEMORY; goto fail_cleanup; }
                        memcpy(cp, cdata, len);
                        linfo.eXIf_data = cp;
                        linfo.eXIf_len = len;
                        linfo.has_eXIf = 1;
                    }
                    break;

                default:
                    if (critical)
                        { rc = PTPNG_E_UNKNOWN_CRITICAL; goto fail_cleanup; }
                    if (idat_state == 1)
                        idat_state = 2; /* IDATs must be consecutive */
                    break;
                }
                if (idat_state == 1 && type != CH_IDAT)
                    idat_state = 2;
            } else {
                if (idat_state == 1)
                    idat_state = 2;
            }

            pos += 12 + (size_t)len;
        }
    }

    if (!have_ihdr || idat_state == 0)
        { rc = PTPNG_E_NO_IDAT; goto fail_cleanup; }
    if (!have_iend)
        { rc = PTPNG_E_NO_IEND; goto fail_cleanup; }
    if (idat_total == 0)
        { rc = PTPNG_E_NO_IDAT; goto fail_cleanup; }

    if (ct == 3 && linfo.num_palette == 0)
        { rc = PTPNG_E_NO_PLTE; goto fail_cleanup; }

    /* ---- geometry (overflow-checked: dimensions come from the file) ---- */
    bitpp = (uint64_t)channels_of(ct) * depth;
    /* w <= 2^31-1 and bitpp <= 64, so w*bitpp+7 fits in uint64 safely */
    rb64 = ((uint64_t)w * bitpp + 7) >> 3;
    rb = (size_t)rb64;

    raw_size = 0;
    if (!interlace) {
        /* raw_size = (rb+1)*h must not wrap and must fit the budget;
         * check by division before multiplying */
        if (h > opts->max_bytes / (rb64 + 1))
            { rc = PTPNG_E_TOO_LARGE; goto fail_cleanup; }
        raw_size = (rb64 + 1) * h;
    } else {
        unsigned p;
        for (p = 0; p < 7; p++) {
            uint32_t pw = (w > adam7_xstart[p])
                ? (w - adam7_xstart[p] + adam7_xstep[p] - 1) / adam7_xstep[p]
                : 0;
            uint32_t ph = (h > adam7_ystart[p])
                ? (h - adam7_ystart[p] + adam7_ystep[p] - 1) / adam7_ystep[p]
                : 0;
            if (pw && ph) {
                uint64_t rbp = (((uint64_t)pw * bitpp) + 7) >> 3;
                uint64_t pass_bytes;
                if (ph > opts->max_bytes / (rbp + 1))
                    { rc = PTPNG_E_TOO_LARGE; goto fail_cleanup; }
                pass_bytes = (rbp + 1) * ph;
                if (pass_bytes > opts->max_bytes - raw_size)
                    { rc = PTPNG_E_TOO_LARGE; goto fail_cleanup; }
                raw_size += pass_bytes;
            }
        }
    }
    if (raw_size == 0)
        { rc = PTPNG_E_TOO_LARGE; goto fail_cleanup; }

    /* output buffer sizes, checked the same way (w*h*bytes can wrap) */
    if (opts->output_format == PTPNG_OUT_RGBA8) {
        if ((uint64_t)w > opts->max_bytes / 4 / h)
            { rc = PTPNG_E_TOO_LARGE; goto fail_cleanup; }
        out_size = (uint64_t)w * h * 4;
    } else if (opts->output_format == PTPNG_OUT_RGB8) {
        if ((uint64_t)w > opts->max_bytes / 3 / h)
            { rc = PTPNG_E_TOO_LARGE; goto fail_cleanup; }
        out_size = (uint64_t)w * h * 3;
    } else {
        /* native: rb*h <= (rb+1)*h = raw_size, already checked */
        out_size = rb64 * h;
    }

    if (prof) P1 = pt_tsc();
    expand_in_place = PTPNG_REUSE_CVT_BUFFER && !interlace &&
                      depth == 8 && ct == 2 &&
                      opts->output_format == PTPNG_OUT_RGBA8;
    raw = (uint8_t *)malloc((size_t)(expand_in_place && out_size > raw_size
                                  ? out_size : raw_size));
    zeros = (uint8_t *)calloc(rb + 16, 1);
    if (!raw || !zeros) {
        free(raw); free(zeros);
        rc = PTPNG_E_OUT_OF_MEMORY;
        goto fail_cleanup;
    }
    /* materialize the zlib stream: zero-copy for a single IDAT chunk */
    if (n_idat_refs == 1) {
        idat_first = idat_refs[0].p;
    } else if (n_idat_refs > 1) {
        size_t off = 0, i;
        idat_buf = (uint8_t *)malloc(idat_total);
        if (!idat_buf) {
            free(raw); free(zeros);
            rc = PTPNG_E_OUT_OF_MEMORY;
            goto fail_cleanup;
        }
        for (i = 0; i < n_idat_refs; i++) {
            memcpy(idat_buf + off, idat_refs[i].p, idat_refs[i].len);
            off += idat_refs[i].len;
        }
    }
    free(idat_refs);
    idat_refs = NULL;
    if (prof) P2 = pt_tsc();

    {
        const uint8_t *zin = idat_buf ? idat_buf : idat_first;
        uint32_t ifl = (opts->flags & PTPNG_FLAG_NO_VERIFY_ADLER)
                           ? PTPNG_INF_NO_ADLER : 0;
        rc = ptpng_inflate(zin ? zin : (const uint8_t *)"", idat_total, raw,
                           (size_t)raw_size, ifl);
        free(idat_buf);
        if (rc) {
            free(raw); free(zeros);
            ptpng_info_free(&linfo);
            return rc;
        }
        if (getenv("PTPNG_DUMP_RAW")) {
            FILE *df = fopen(getenv("PTPNG_DUMP_RAW"), "wb");
            if (df) { fwrite(raw, 1, (size_t)raw_size, df); fclose(df); }
        }
        rc = unfilter_image(raw, w, h, depth, channels_of(ct), (int)interlace,
                            zeros);
        if (prof) P3 = pt_tsc();
    }
    free(zeros);
    if (rc) {
        free(raw);
        ptpng_info_free(&linfo);
        return rc;
    }

    if (interlace) {
        native = (uint8_t *)malloc((size_t)(rb * (size_t)h));
        if (!native) {
            free(raw);
            ptpng_info_free(&linfo);
            return PTPNG_E_OUT_OF_MEMORY;
        }
        adam7_extract(native, raw, w, h, depth, channels_of(ct));
        free(raw);
    } else {
        native = raw; /* already compacted in place */
    }
    /* sub-byte row-tail padding bits are zeroed (libpng leaves them as
     * uninitialized row-buffer garbage; zeroing is the deterministic
     * canonical form) */
    zero_pad_bits(native, w, h, depth, channels_of(ct));

    /* ---- output conversion ---- */
    /* Matching 8-bit formats can return the reconstructed allocation;
     * another full-image allocation and row copy would change no pixels. */
    if (opts->output_format == PTPNG_OUT_NATIVE ||
        (depth == 8 &&
         ((ct == 6 && opts->output_format == PTPNG_OUT_RGBA8) ||
          (ct == 2 && opts->output_format == PTPNG_OUT_RGB8)))) {
        *out = native;
        if (out_len)
            *out_len = (size_t)out_size;
        linfo.rowbytes = rb;
    } else {
        /* Backward RGB8 expansion reuses the larger raw allocation.
         * Alpha removal writes fewer bytes than it reads. RGBA8-to-RGB8
         * converters support dst <= src, so compact into the native
         * allocation without touching unread pixels or later rows. */
        uint8_t *conv = expand_in_place ||
                       (PTPNG_REUSE_CVT_BUFFER && depth == 8 && ct == 6 &&
                        opts->output_format == PTPNG_OUT_RGB8)
                      ? native : (uint8_t *)malloc((size_t)out_size);
        struct ptpng_cvt cvt;
        const ptpng_cvt_fn *tbl;
        size_t dst_row = (size_t)(out_size / h);
        uint32_t y;
        if (!conv) {
            free(native);
            ptpng_info_free(&linfo);
            return PTPNG_E_OUT_OF_MEMORY;
        }
        memset(&cvt, 0, sizeof(cvt));
        cvt.palette = linfo.palette;
        cvt.trans = linfo.palette + 768;
        cvt.num_trans = linfo.num_trans;
        cvt.trns_mask = linfo.has_trns ? linfo.trns[0] : 0;
        cvt.trns_r = linfo.trns[0];
        cvt.trns_g = linfo.trns[1];
        cvt.trns_b = linfo.trns[2];
        cvt.has_trns = linfo.has_trns;
#if PTPNG_REUSE_CVT_BUFFER
        cvt.reverse = (uint8_t)expand_in_place;
#endif
        if (ct == 3) {
            /* Packed entries serve RGB stores and the RGBA gather path. */
            unsigned pi;
            for (pi = 0; pi < linfo.num_palette; pi++) {
                cvt.pal_rgba[pi * 4 + 0] = linfo.palette[pi * 3 + 0];
                cvt.pal_rgba[pi * 4 + 1] = linfo.palette[pi * 3 + 1];
                cvt.pal_rgba[pi * 4 + 2] = linfo.palette[pi * 3 + 2];
                cvt.pal_rgba[pi * 4 + 3] =
                    (pi < linfo.num_trans) ? linfo.palette[768 + pi] : 255;
            }
        }
        tbl = (opts->output_format == PTPNG_OUT_RGBA8)
                  ? ptpng_cpu.cvt_table_rgba8
                  : ptpng_cpu.cvt_table_rgb8;
        cvt.fn = tbl[((unsigned)ct << 4) | DC(depth)];
        /* SIMD converters don't apply tRNS; fall back to scalar */
        if (linfo.has_trns && (ct == 0 || ct == 2) &&
            opts->output_format == PTPNG_OUT_RGBA8)
            cvt.fn = ptpng_cvt_table_rgba8_scalar[
                ((unsigned)ct << 4) | DC(depth)];
        if (!cvt.fn) {
#if PTPNG_REUSE_CVT_BUFFER
            if (conv != native)
#endif
                free(conv);
            free(native);
            ptpng_info_free(&linfo);
            return PTPNG_E_UNSUPPORTED;
        }
        for (y = 0; y < h; y++) {
            uint32_t row = expand_in_place ? h - 1 - y : y;
            cvt.fn(native + (size_t)row * rb, conv + (size_t)row * dst_row,
                   w, &cvt);
        }
#if PTPNG_REUSE_CVT_BUFFER
        if (conv != native)
#endif
            free(native);
        *out = conv;
        if (out_len)
            *out_len = (size_t)out_size;
        linfo.rowbytes = dst_row;
    }

    if (info) {
        memcpy(info, &linfo, sizeof(*info));
    } else {
        ptpng_info_free(&linfo);
    }
    rc = PTPNG_OK;
    goto done;
fail_cleanup:
    free(idat_refs);
    ptpng_info_free(&linfo);
done:
    if (prof) {
        P4 = pt_tsc();
        fprintf(stderr,
                "prof: parse+CRC=%lluK alloc=%lluK inflate=%lluK "
                "unfilt=%lluK out=%lluK\n",
                (P1 - P0) / 1000, (P2 - P1) / 1000, (P3 - P2) / 1000,
                (P4 - P3) / 1000, 0ull);
    }
    return rc;
    }
}

void ptpng_free(void *p)
{
    free(p);
}

/* internal helpers reused by tests */
#undef T
