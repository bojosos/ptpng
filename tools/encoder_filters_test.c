/* Forward filters: independent predictors, exact tails, and every Paeth triple. */
#include "ptpng_internal.h"
#include <stdio.h>

static unsigned rng = 987654321u;

static uint8_t next_byte(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (uint8_t)rng;
}

static int check_scores(ptpng_encode_score_fn fn,
                         ptpng_encode_filter_score_fn fused, const char *name)
{
    static const size_t lengths[] = {255,256,257,4095,4096,4097,65537};
    size_t index, offset, i;
    unsigned pattern;
    for (index = 0; index < 130 + sizeof(lengths)/sizeof(lengths[0]); ++index) {
        size_t count = index < 130 ? index : lengths[index-130];
        for (offset = 0; offset < 32; ++offset) {
            uint8_t *src = (uint8_t *)malloc(count+offset ? count+offset : 1);
            if (!src) return 1;
            for (pattern = 0; pattern < 3; ++pattern) {
                uint64_t expected = 0;
                for (i = 0; i < count; ++i) {
                    unsigned x = pattern == 0 ? 128 : pattern == 1 ? 255 : next_byte();
                    src[offset+i] = (uint8_t)x;
                    expected += x < 128 ? x : 256-x;
                }
                if (fn(src+offset, count) != expected) {
                    fprintf(stderr, "%s score mismatch count=%zu alignment=%zu pattern=%u\n",
                            name, count, offset, pattern);
                    free(src); return 1;
                }
            }
            free(src);
        }
    }
    {
        /* 128 * (32 MiB + 1) exceeds UINT32_MAX. */
        const size_t count = ((size_t)1 << 25) + 1;
        uint8_t *src = (uint8_t *)malloc(count);
        uint8_t *dst = (uint8_t *)malloc(count);
        int failed;
        if (!src || !dst) { free(src); free(dst); return 1; }
        memset(src, 128, count);
        failed = fn(src, count) != (uint64_t)count * 128;
        /* Alternating source bytes produce Sub residuals of magnitude 128. */
        for (i = 0; i < count; ++i) src[i] = i & 1 ? 0 : 128;
        failed |= fused(dst, src, NULL, count, 1, 1) != (uint64_t)count * 128;
        free(src); free(dst);
        if (failed) { fprintf(stderr, "%s score overflow\n", name); return 1; }
    }
    return 0;
}

static int paeth_reference(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void filter_reference(uint8_t *dst, const uint8_t *src,
                              const uint8_t *prev, size_t count,
                              unsigned bpp, int filter)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        int a = i >= bpp ? src[i - bpp] : 0;
        int b = prev ? prev[i] : 0;
        int c = prev && i >= bpp ? prev[i - bpp] : 0;
        int predictor = 0;
        if (filter == 1) predictor = a;
        else if (filter == 2) predictor = b;
        else if (filter == 3) predictor = (a + b) / 2;
        else if (filter == 4) predictor = paeth_reference(a, b, c);
        dst[i] = (uint8_t)(src[i] - predictor);
    }
}

static int check_rows(ptpng_encode_filter_fn fn,
                       ptpng_encode_filter_score_fn fused, const char *name)
{
    static const unsigned bpps[] = {1, 2, 3, 4, 6, 8};
    static const size_t longer[] = {255, 256, 257, 511, 512, 513, 4095, 4096, 4097};
    size_t index, offset, k, i;
    int filter, first;
    for (index = 0; index < 130 + sizeof(longer) / sizeof(longer[0]); ++index) {
        size_t count = index < 130 ? index : longer[index - 130];
        for (offset = 0; offset < 32; ++offset) {
            size_t prev_offset = 31 - offset, dst_offset = (offset * 7) & 31;
            /* Each buffer ends at the last logical byte. Sanitizers therefore
             * catch vector reads and stores that cross a partial final block. */
            uint8_t *src = (uint8_t *)malloc(count + offset ? count + offset : 1);
            uint8_t *prev = (uint8_t *)malloc(count + prev_offset ? count + prev_offset : 1);
            uint8_t *dst = (uint8_t *)malloc(count + dst_offset ? count + dst_offset : 1);
            uint8_t *expected = (uint8_t *)malloc(count ? count : 1);
            if (!src || !prev || !dst || !expected) {
                free(src); free(prev); free(dst); free(expected);
                fprintf(stderr, "encoder filter test allocation failed\n");
                return 1;
            }
            for (i = 0; i < count; ++i) {
                src[offset + i] = next_byte();
                prev[prev_offset + i] = next_byte();
            }
            for (k = 0; k < sizeof(bpps) / sizeof(bpps[0]); ++k)
                for (filter = 0; filter <= 4; ++filter)
                    for (first = 0; first < 2; ++first) {
                        const uint8_t *above = first ? NULL : prev + prev_offset;
                        uint64_t expected_score = 0, actual_score;
                        memset(dst, 0xa5, count + dst_offset);
                        filter_reference(expected, src + offset, above, count, bpps[k], filter);
                        fn(dst + dst_offset, src + offset, above, count, bpps[k], filter);
                        if (memcmp(expected, dst + dst_offset, count)) {
                            fprintf(stderr, "%s mismatch count=%zu alignment=%zu bpp=%u filter=%d first=%d\n",
                                    name, count, offset, bpps[k], filter, first);
                            free(src); free(prev); free(dst); free(expected);
                            return 1;
                        }
                        for (i = 0; i < count; ++i) {
                            unsigned x = expected[i];
                            expected_score += x < 128 ? x : 256-x;
                        }
                        memset(dst, 0xa5, count + dst_offset);
                        actual_score = fused(dst + dst_offset, src + offset, above,
                                             count, bpps[k], filter);
                        if (actual_score != expected_score ||
                            memcmp(expected, dst + dst_offset, count)) {
                            fprintf(stderr, "%s fused mismatch count=%zu alignment=%zu bpp=%u filter=%d first=%d\n",
                                    name, count, offset, bpps[k], filter, first);
                            free(src); free(prev); free(dst); free(expected);
                            return 1;
                        }
                        for (i = 0; i < dst_offset; ++i)
                            if (dst[i] != 0xa5) {
                                fprintf(stderr, "%s wrote before the output row\n", name);
                                free(src); free(prev); free(dst); free(expected);
                                return 1;
                            }
                    }
            free(src); free(prev); free(dst); free(expected);
        }
    }
    return 0;
}

static int check_paeth(ptpng_encode_filter_fn fn, const char *name)
{
    const size_t triples = 256u * 256u;
    const size_t count = 2u * 256u * 256u + 32;
    uint8_t *src = (uint8_t *)malloc(count);
    uint8_t *prev = (uint8_t *)malloc(count);
    uint8_t *dst = (uint8_t *)malloc(count);
    uint8_t *expected = (uint8_t *)malloc(count);
    unsigned a;
    size_t i;
    int failed = 0;
    if (!src || !prev || !dst || !expected) {
        fprintf(stderr, "encoder Paeth test allocation failed\n");
        failed = 1;
        goto done;
    }
    memset(src + 2 * triples, 0, 32);
    memset(prev + 2 * triples, 0, 32);
    /* With bpp=1 each odd byte has independently controlled left, above,
     * and upper-left inputs. Every triple reaches the vector body. */
    for (a = 0; a < 256; ++a) {
        for (i = 0; i < triples; ++i) {
            src[2 * i] = (uint8_t)a;
            src[2 * i + 1] = 137;
            prev[2 * i] = (uint8_t)i;
            prev[2 * i + 1] = (uint8_t)(i >> 8);
        }
        filter_reference(expected, src, prev, count, 1, 4);
        fn(dst, src, prev, count, 1, 4);
        if (memcmp(dst, expected, count)) {
            fprintf(stderr, "%s exhaustive Paeth mismatch a=%u\n", name, a);
            failed = 1;
            break;
        }
    }
done:
    free(src); free(prev); free(dst); free(expected);
    return failed;
}

int main(void)
{
    ptpng_encode_filter_fn accelerated = NULL;
    ptpng_encode_score_fn accelerated_score = NULL;
    ptpng_encode_filter_score_fn accelerated_fused = NULL;
    const char *name = "scalar";
    ptpng_cpu_init();
#if PTPNG_X86
    if (ptpng_cpu.avx2) {
        accelerated = ptpng_encode_filter_avx2;
        accelerated_score = ptpng_encode_score_avx2;
        accelerated_fused = ptpng_encode_filter_score_avx2;
        name = "AVX2";
    }
#endif
#if PTPNG_ARM_NEON
    if (ptpng_cpu.neon) {
        accelerated = ptpng_encode_filter_neon;
        accelerated_score = ptpng_encode_score_neon;
        accelerated_fused = ptpng_encode_filter_score_neon;
        name = "NEON";
    }
#endif
    if (check_rows(ptpng_encode_filter_scalar, ptpng_encode_filter_score_scalar, "scalar") ||
        check_paeth(ptpng_encode_filter_scalar, "scalar")) return 1;
    if (accelerated && (check_rows(accelerated, accelerated_fused, name) ||
                         check_paeth(accelerated, name))) return 1;
    if (check_scores(ptpng_encode_score_scalar, ptpng_encode_filter_score_scalar, "scalar") ||
        (accelerated_score && check_scores(accelerated_score, accelerated_fused, name))) return 1;
    printf("Encoder filters passed: scalar%s%s; all Paeth triples and row tails\n",
           accelerated ? " + " : "", accelerated ? name : "");
    return 0;
}
