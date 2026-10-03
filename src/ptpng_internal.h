/* ptpng_internal.h - internal cross-TU contracts. Not public. */
#ifndef PTPNG_INTERNAL_H
#define PTPNG_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ptpng.h"

#if defined(_MSC_VER)
#define PTPNG_API_INLINE static __forceinline
#else
#define PTPNG_API_INLINE static inline __attribute__((always_inline))
#endif

/* ---- architecture / intrinsics -------------------------------------- */
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64) || \
    defined(__i386__) || defined(_M_IX86)
#define PTPNG_X86 1
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#define PTPNG_X64 1
#endif
#endif

#if defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON)
#define PTPNG_ARM_NEON 1
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(PTPNG_X86)
#include <x86intrin.h>
#endif
#if defined(PTPNG_ARM_NEON)
#include <arm_neon.h>
#endif

/* ---- checksums ------------------------------------------------------- */
uint32_t ptpng_crc32(const uint8_t *p, size_t n);   /* dispatching */
uint32_t ptpng_adler32(const uint8_t *p, size_t n); /* dispatching */
uint32_t ptpng_crc32_slice8(const uint8_t *p, size_t n);
void ptpng_crc_x86_init(void);
void ptpng_crc_arm_init(void);
uint32_t ptpng_adler32_scalar(const uint8_t *p, size_t n);
#if PTPNG_X64
uint32_t ptpng_adler32_sse2(const uint8_t *p, size_t n);
#endif

/* ---- inflate --------------------------------------------------------- */
#define PTPNG_INF_NO_ADLER 0x1

int ptpng_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
                  size_t out_len, uint32_t flags);
int ptpng_inflate_dyn(const uint8_t *in, size_t in_len, size_t max_out,
                      uint8_t **out, size_t *out_len);

/* ---- filters ---------------------------------------------------------
 * Reconstruct one scanline from the filtered bytes, writing the final
 * compacted row: dst receives count reconstructed bytes read from src
 * (the filtered scanline); prev is the previous row's reconstructed
 * pixels (zero-filled buffer for the first row).  dst and src may
 * overlap (dst <= src); kernels that need in-place operation (sub)
 * copy internally.
 */
/* Forward filters: disjoint buffers; NULL prev denotes the first row. */
typedef void (*ptpng_encode_filter_fn)(uint8_t *, const uint8_t *,
                                      const uint8_t *, size_t, unsigned, int);
typedef uint64_t (*ptpng_encode_score_fn)(const uint8_t *, size_t);
uint64_t ptpng_encode_score_scalar(const uint8_t *, size_t);
void ptpng_encode_filter_scalar(uint8_t *, const uint8_t *, const uint8_t *,
                                 size_t, unsigned, int);
#if PTPNG_X86
uint64_t ptpng_encode_score_avx2(const uint8_t *, size_t);
void ptpng_encode_filter_avx2(uint8_t *, const uint8_t *, const uint8_t *,
                               size_t, unsigned, int);
#endif
#if PTPNG_ARM_NEON
uint64_t ptpng_encode_score_neon(const uint8_t *, size_t);
void ptpng_encode_filter_neon(uint8_t *, const uint8_t *, const uint8_t *,
                               size_t, unsigned, int);
#endif
int ptpng_deflate(const uint8_t *, size_t, uint8_t **, size_t *);

typedef void (*ptpng_filter_fn)(uint8_t *dst, const uint8_t *src,
                                const uint8_t *prev, size_t count,
                                unsigned bpp);

void ptpng_filter_sub_scalar(uint8_t *dst, const uint8_t *src,
                             const uint8_t *prev, size_t count, unsigned bpp);
void ptpng_filter_up_scalar(uint8_t *dst, const uint8_t *src,
                            const uint8_t *prev, size_t count, unsigned bpp);
void ptpng_filter_avg_scalar(uint8_t *dst, const uint8_t *src,
                             const uint8_t *prev, size_t count, unsigned bpp);
void ptpng_filter_paeth_scalar(uint8_t *dst, const uint8_t *src,
                               const uint8_t *prev, size_t count,
                               unsigned bpp);
#if PTPNG_X64
void ptpng_filter_sub_sse2(uint8_t *dst, const uint8_t *src,
                           const uint8_t *prev, size_t count, unsigned bpp);
void ptpng_filter_up_sse2(uint8_t *dst, const uint8_t *src,
                          const uint8_t *prev, size_t count, unsigned bpp);
void ptpng_sub_blocked_sse2(uint8_t *dst, size_t count, unsigned bpp);
void ptpng_filter_sub_avx2(uint8_t *dst, const uint8_t *src,
                           const uint8_t *prev, size_t count, unsigned bpp);
void ptpng_filter_up_avx2(uint8_t *dst, const uint8_t *src,
                          const uint8_t *prev, size_t count, unsigned bpp);
void ptpng_filter_paeth_avx2(uint8_t *dst, const uint8_t *src,
                             const uint8_t *prev, size_t count, unsigned bpp);
#endif /* PTPNG_X64 */

/* SIMD hookups: defined in ptpng_avx2.c (x86, built with AVX2) and
 * ptpng_neon.c (ARM NEON); selected at link time by the build system. */
#if PTPNG_X86
void ptpng_avx2_init(void);
#endif
#if PTPNG_ARM_NEON
void ptpng_neon_init(void);
void ptpng_filter_up_neon(uint8_t *dst, const uint8_t *src,
                          const uint8_t *prev, size_t count, unsigned bpp);
#endif

/* ---- pixel conversions ----------------------------------------------- */
struct ptpng_cvt;

typedef void (*ptpng_cvt_fn)(const uint8_t *src, uint8_t *dst, uint32_t n,
                             const struct ptpng_cvt *cvt);

struct ptpng_cvt {
    ptpng_cvt_fn fn;
    const uint8_t *palette;   /* ct3: rgb triples            */
    const uint8_t *trans;     /* ct3: alpha per index        */
    uint32_t num_trans;
    uint16_t trns_mask;       /* ct0: gray sample            */
    uint16_t trns_r, trns_g, trns_b; /* ct2 samples          */
    uint8_t has_trns;         /* ct0/2: apply transparency   */
    uint8_t reverse;          /* RGB8 -> RGBA8: expand backward in place */
    uint8_t pal_rgba[1024];   /* ct3: precombined rgba table (AVX2 gather path) */
};

/* table index: (color_type << 4) | log2(bit_depth), 128 entries */
extern const ptpng_cvt_fn ptpng_cvt_table_rgba8_scalar[128];
extern const ptpng_cvt_fn ptpng_cvt_table_rgb8_scalar[128];
#if PTPNG_ARM_NEON
extern ptpng_cvt_fn ptpng_cvt_table_rgba8_neon[128];
extern ptpng_cvt_fn ptpng_cvt_table_rgb8_neon[128];
#endif
#if PTPNG_X86
/* filled at init from the scalar tables, then overridden */
extern ptpng_cvt_fn ptpng_cvt_table_rgba8_avx2[128];
extern ptpng_cvt_fn ptpng_cvt_table_rgb8_avx2[128];
#endif

/* ---- cpu dispatch ---------------------------------------------------- */
struct ptpng_cpu {
    unsigned sse2 : 1, ssse3 : 1, sse41 : 1, pclmul : 1;
    unsigned avx2 : 1, bmi1 : 1, bmi2 : 1;
    unsigned neon : 1, arm_crc32 : 1;
    ptpng_filter_fn filter_sub, filter_up, filter_paeth;
    const ptpng_cvt_fn *cvt_table_rgba8;
    const ptpng_cvt_fn *cvt_table_rgb8;
    uint32_t (*crc32)(const uint8_t *, size_t);
    uint32_t (*adler32)(const uint8_t *, size_t);
};

extern struct ptpng_cpu ptpng_cpu;
void ptpng_cpu_init(void); /* idempotent, cheap, single-threaded */

/* big-endian readers */
PTPNG_API_INLINE uint32_t pt_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
PTPNG_API_INLINE uint16_t pt_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

PTPNG_API_INLINE int pt_chunk_critical(uint32_t type)
{
    return !(type & 0x20000000u); /* bit 5 of first byte */
}

#endif /* PTPNG_INTERNAL_H */
