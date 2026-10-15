/* filters_test.c - verify SIMD filter kernels and paeth predictor */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ptpng_internal.h"

static unsigned rng = 987654321u;
#define NEXT() (rng = rng * 1664525u + 1013904223u)

static int paeth_ref(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p - a; if (pa < 0) pa = -pa;
    int pb = p - b; if (pb < 0) pb = -pb;
    int pc = p - c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

extern int ptpng_paeth_pred_ext(int, int, int);

static int test_paeth_pred(void)
{
    int a, b, c;
    for (a = 0; a < 256; a++)
        for (b = 0; b < 256; b++)
            for (c = 0; c < 256; c += 1)
                if (ptpng_paeth_pred_ext(a, b, c) != paeth_ref(a, b, c)) {
                    printf("paeth_pred mismatch a=%d b=%d c=%d\n", a, b, c);
                    return 1;
                }
    return 0;
}

static int test_paeth_dispatch(unsigned bpp)
{
    uint8_t filtered[24], above[24], output[24];
    unsigned channel;
    int a, b, c;
    /* The first pixel reconstructs a with upper neighbor c. The second
     * pixel then exercises every predictor triple inside the SIMD loop. */
    for (a = 0; a < 256; a++)
        for (b = 0; b < 256; b++)
            for (c = 0; c < 256; c++) {
                memset(filtered, 0, sizeof(filtered));
                memset(above, 0, sizeof(above));
                for (channel = 0; channel < bpp; channel++) {
                    filtered[channel] = (uint8_t)(a - c);
                    above[channel] = (uint8_t)c;
                    above[bpp + channel] = (uint8_t)b;
                }
                ptpng_cpu.filter_paeth(output, filtered, above, 3 * bpp, bpp);
                for (channel = 0; channel < bpp; channel++)
                    if (output[bpp + channel] != paeth_ref(a, b, c)) {
                        printf("dispatched paeth bpp=%u a=%d b=%d c=%d mismatch\n",
                               bpp, a, b, c);
                        return 1;
                    }
            }
    return 0;
}

enum { N = 4096 };
static uint8_t src[N + 64], ref[N + 64], got[N + 64], prev[N + 64];

static int check_kernel(ptpng_filter_fn fn, const char *name, unsigned bpp,
                        size_t count)
{
    /* Different alignments and the compacting overlaps used by the decoder. */
    static const unsigned gaps[] = {0, 1, 3, 15, 16, 31};
    unsigned alignment, g;
    uint8_t work[N + 128];
    uint8_t *exact_src = (uint8_t *)malloc(count ? count : 1);
    uint8_t *exact_prev = (uint8_t *)malloc(count ? count : 1);
    if (!exact_src || !exact_prev) {
        free(exact_src);
        free(exact_prev);
        return 1;
    }
    memcpy(exact_src, src, count);
    memcpy(exact_prev, prev, count);
    /* ASan checks the final load against exact row allocations. */
    fn(work, exact_src, exact_prev, count, bpp);
    free(exact_src);
    free(exact_prev);
    if (memcmp(work, ref, count)) return 1;
    for (alignment = 0; alignment < 16; alignment++) {
        uint8_t *dst = work + 16 + alignment;
        memset(work, 0xa5, sizeof(work));
        fn(dst, src, prev, count, bpp);
        if (memcmp(dst, ref, count) || dst[-1] != 0xa5 || dst[count] != 0xa5)
            goto fail;
        for (g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
            unsigned gap = gaps[g];
            uint8_t after;
            memset(work, 0xa5, sizeof(work));
            memcpy(dst + gap, src, count);
            after = dst[count];
            fn(dst, dst + gap, prev, count, bpp);
            if (memcmp(dst, ref, count) || dst[-1] != 0xa5 || dst[count] != after)
                goto fail;
        }
    }
    return 0;
fail:
    printf("%s bpp=%u count=%zu alignment=%u mismatch or overwrite\n",
           name, bpp, count, alignment);
    return 1;
}

static int check_full(int kind, const char *name, unsigned bpp)
{
    size_t counts[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63,
                       64, 65, 100, 127, 128, 129, 255, 256, 257, 511,
                       512, 513, 767, 768, 769, 1023, 1024, 2048, 4096};
    unsigned c, i;
    size_t k;
    for (c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        size_t count = counts[c];
        for (i = 0; i < count; i++) {
            src[i] = (uint8_t)(NEXT() >> 24);
            prev[i] = (uint8_t)(NEXT() >> 24);
        }
        /* reference fused reconstruction */
        for (k = 0; k < count; k++) {
            if (kind == 1) { /* sub: no filtering before bpp */
                ref[k] = (k >= bpp)
                    ? (uint8_t)(src[k] + ref[k - bpp]) : src[k];
            } else if (kind == 2) { /* up */
                ref[k] = (uint8_t)(src[k] + prev[k]);
            } else {
                int a = (k >= bpp) ? ref[k - bpp] : 0;
                int b = prev[k], c2 = (k >= bpp) ? prev[k - bpp] : 0;
                if (k < bpp) {
                    /* prologue: a = c = 0 */
                    ref[k] = (kind == 3)
                        ? (uint8_t)(src[k] + (b >> 1))
                        : (uint8_t)(src[k] + b);
                } else if (kind == 3) {
                    ref[k] = (uint8_t)(src[k] + ((a + b) >> 1));
                } else {
                    ref[k] = (uint8_t)(src[k] + paeth_ref(a, b, c2));
                }
            }
        }
        /* kernel under test */
        if (kind == 1) ptpng_filter_sub_scalar(got, src, prev, count, bpp);
        else if (kind == 2) ptpng_filter_up_scalar(got, src, prev, count, bpp);
        else if (kind == 3) ptpng_filter_avg_scalar(got, src, prev, count, bpp);
        else ptpng_filter_paeth_scalar(got, src, prev, count, bpp);
        if (memcmp(ref, got, count)) {
            k = 0;
            while (ref[k] == got[k]) k++;
            printf("%s bpp=%u count=%zu MISMATCH at %zu (ref=%d got=%d)\n",
                   name, bpp, count, k, ref[k], got[k]);
            return 1;
        }
        {
            ptpng_filter_fn fn = kind == 1 ? ptpng_filter_sub_scalar :
                kind == 2 ? ptpng_filter_up_scalar :
                kind == 3 ? ptpng_filter_avg_scalar : ptpng_filter_paeth_scalar;
            if (check_kernel(fn, name, bpp, count)) return 1;
        }
        /* Dispatch exercises NEON on ARM and the selected x86 path. */
        if (kind == 4 && check_kernel(ptpng_cpu.filter_paeth,
                                      "dispatched paeth", bpp, count)) return 1;
        if (kind == 1 || kind == 2) {
            ptpng_filter_fn fn = kind == 1 ? ptpng_cpu.filter_sub : ptpng_cpu.filter_up;
            if (check_kernel(fn, "dispatched", bpp, count)) return 1;
#if PTPNG_X64
            fn = (kind == 1) ? ptpng_filter_sub_sse2 : ptpng_filter_up_sse2;
            if (check_kernel(fn, "sse2", bpp, count)) return 1;
            if (ptpng_cpu.avx2) {
                fn = (kind == 1) ? ptpng_filter_sub_avx2 : ptpng_filter_up_avx2;
                if (check_kernel(fn, "avx2", bpp, count)) return 1;
            }
#endif
        }
    }
    return 0;
}

static int test_conversions(void)
{
    static const unsigned channels[] = {1, 0, 3, 1, 2, 0, 4};
    struct ptpng_cvt cvt;
    uint8_t palette[768], trans[256], expected[260 + 32], actual[260 + 32];
    unsigned i, key, output, alignment, n;
    memset(&cvt, 0, sizeof(cvt));
    cvt.palette = palette;
    cvt.trans = trans;
    cvt.num_trans = 197;
    for (i = 0; i < 768; i++) palette[i] = (uint8_t)(NEXT() >> 24);
    for (i = 0; i < 256; i++) {
        trans[i] = (uint8_t)(NEXT() >> 24);
        memcpy(cvt.pal_rgba + i * 4, palette + i * 3, 3);
        cvt.pal_rgba[i * 4 + 3] = i < cvt.num_trans ? trans[i] : 255;
    }
    for (output = 3; output <= 4; output++) {
        const ptpng_cvt_fn *scalar = output == 4 ? ptpng_cvt_table_rgba8_scalar :
                                                  ptpng_cvt_table_rgb8_scalar;
        const ptpng_cvt_fn *selected = output == 4 ? ptpng_cpu.cvt_table_rgba8 :
                                                    ptpng_cpu.cvt_table_rgb8;
        for (key = 0; key < 128; key++) {
            if (!scalar[key]) continue;
            for (n = 0; n <= 65; n++) {
                size_t bytes = ((size_t)n * channels[key >> 4] *
                                ((size_t)1 << (key & 15)) + 7) / 8;
                for (alignment = 0; alignment < 16; alignment++) {
                    /* No readable padding after the row: ASan catches full
                     * vector loads that extend past the final pixel. */
                    size_t allocation = bytes + alignment;
                    uint8_t *storage = (uint8_t *)malloc(allocation ? allocation : 1);
                    uint8_t *pixels;
                    if (!storage) return 1;
                    pixels = storage + alignment;
                    for (i = 0; i < bytes; i++) pixels[i] = (uint8_t)(NEXT() >> 24);
                    memset(expected, 0xa5, sizeof(expected));
                    memset(actual, 0xa5, sizeof(actual));
                    scalar[key](pixels, expected + alignment, n, &cvt);
                    selected[key](pixels, actual + alignment, n, &cvt);
                    free(storage);
                    if (memcmp(expected, actual, sizeof(expected))) {
                        printf("conversion key=%u output=%u pixels=%u alignment=%u mismatch\n",
                               key, output, n, alignment);
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

static void benchmark(int paeth)
{
    static const unsigned bpps[] = {1, 2, 3, 4, 6, 8};
    static const size_t counts[] = {127, 4096};
    unsigned b, c, i, f;
    ptpng_filter_fn kernels[] = {ptpng_filter_sub_scalar, ptpng_cpu.filter_sub};
    const char *names[] = {"scalar", "dispatched"};
    if (paeth) {
        kernels[0] = ptpng_filter_paeth_scalar;
        kernels[1] = ptpng_cpu.filter_paeth;
    }
    for (i = 0; i < N; i++) src[i] = (uint8_t)(NEXT() >> 24);
    for (i = 0; i < N; i++) prev[i] = (uint8_t)(NEXT() >> 24);
    for (c = 0; c < sizeof(counts) / sizeof(counts[0]); c++) {
        for (b = 0; b < sizeof(bpps) / sizeof(bpps[0]); b++) {
            size_t count = counts[c];
            unsigned iterations = (unsigned)((paeth ? 128u : 1024u) * 1024u * 1024u / count);
            for (f = 0; f < sizeof(kernels) / sizeof(kernels[0]); f++) {
                clock_t start = clock();
                for (i = 0; i < iterations; i++)
                    kernels[f](got, src, prev, count, bpps[b]);
                printf("%s %-10s bpp=%u bytes=%zu %.1f MB/s\n",
                       paeth ? "paeth" : "sub", names[f],
                       bpps[b], count, (double)count * iterations * CLOCKS_PER_SEC /
                       ((double)(clock() - start) * 1000000.0));
            }
        }
    }
}

int main(int argc, char **argv)
{
    unsigned bpps[] = {1, 2, 3, 4, 6, 8};
    unsigned i;
    int bad = 0;
    ptpng_cpu_init();
    if (argc == 2 && strcmp(argv[1], "--bench") == 0) {
        benchmark(0);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--bench-paeth") == 0) {
        benchmark(1);
        return 0;
    }
    if (test_paeth_pred()) bad = 1;
    if (test_paeth_dispatch(3)) bad = 1;
    if (test_paeth_dispatch(4)) bad = 1;
    if (test_paeth_dispatch(6)) bad = 1;
    if (test_paeth_dispatch(8)) bad = 1;
    if (test_conversions()) bad = 1;
    for (i = 0; i < 6; i++) {
        if (check_full(1, "sub", bpps[i])) bad = 1;
        if (check_full(2, "up", bpps[i])) bad = 1;
        if (check_full(3, "avg", bpps[i])) bad = 1;
        if (check_full(4, "paeth", bpps[i])) bad = 1;
    }
    if (bad) return 1;
    printf("filters OK\n");
    return 0;
}
