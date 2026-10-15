/* adler/crc unit test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ptpng_internal.h"

int main(void)
{
    unsigned i, j;
    /* deterministic pseudo-random */
    unsigned rng = 12345;
#define NEXT() (rng = rng * 1664525u + 1013904223u)
    for (i = 0; i < 3000; i++) {
        size_t n = i;
        uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
        size_t k;
        uint32_t a_ref, c_ref;
        for (k = 0; k < n; k++)
            buf[k] = (uint8_t)(NEXT() >> 24);
        /* reference adler */
        {
            uint32_t a = 1, b = 0;
            for (k = 0; k < n; k++) {
                a += buf[k]; b += a;
            }
            a %= 65521u; b %= 65521u;
            a_ref = (b << 16) | a;
        }
        /* reference crc (bit-by-bit) */
        {
            uint32_t c = 0xFFFFFFFFu;
            for (k = 0; k < n; k++) {
                c ^= buf[k];
                for (j = 0; j < 8; j++)
                    c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
            }
            c_ref = ~c;
        }
        ptpng_cpu_init();
        {
            uint32_t got = ptpng_adler32(buf, n);
            uint32_t got2 = ptpng_adler32_scalar(buf, n);
            uint32_t gotc = ptpng_crc32_slice8(buf, n);
            uint32_t dispatched_crc = ptpng_crc32(buf, n);
            if (got != a_ref || got2 != a_ref) {
                printf("adler FAIL n=%zu sse2=%08x scalar=%08x ref=%08x\n",
                       n, got, got2, a_ref);
                if (n <= 32) {
                    printf("bytes:");
                    for (k = 0; k < n; k++) printf(" %02x", buf[k]);
                    printf("\n");
                    {
                        uint32_t W = 0;
                        for (k = 0; k < n; k++)
                            W += (uint32_t)(n - k) * buf[k];
                        printf("expected W=%u chunk*a=%u => b=%u\n",
                               W, (unsigned)n, W + (unsigned)n);
                    }
                }
                return 1;
            }
            if (gotc != c_ref || dispatched_crc != c_ref) {
                printf("crc FAIL n=%zu scalar=%08x dispatched=%08x ref=%08x\n",
                       n, gotc, dispatched_crc, c_ref);
                return 1;
            }
        }
        free(buf);
    }
    /* all-0xFF and all-0x00 large buffers (weight edge cases) */
    for (i = 0; i < 2; i++) {
        size_t n = 70000;
        uint8_t *buf = (uint8_t *)malloc(n);
        unsigned long long a = 1, b = 0;
        memset(buf, i ? 0xFF : 0x00, n);
        for (size_t k = 0; k < n; k++) { a += buf[k]; b += a; }
        a %= 65521u; b %= 65521u;
        ptpng_cpu_init();
        if (ptpng_adler32(buf, n) != (uint32_t)((b << 16) | a)) {
            printf("adler large FAIL pattern=%u\n", i);
            return 1;
        }
        if (ptpng_crc32(buf, n) != ptpng_crc32_slice8(buf, n)) {
            printf("crc large FAIL pattern=%u\n", i);
            free(buf);
            return 1;
        }
        free(buf);
    }
    /* Cross SIMD chunk boundaries and every AVX2 alignment, with an
     * exact-sized allocation so sanitizers also catch tail overreads. */
    for (i = 0; i < 32; i++) {
        static const size_t lengths[] = {0,1,7,8,15,16,17,31,32,33,63,64,65,
                                        127,128,129,255,256,257,511,512,513,
                                        767,768,769,1535,1536,1537,
                                        2047,2048,2049,4095,4096,4097,8191,8192,8193};
        for (j = 0; j < sizeof(lengths) / sizeof(lengths[0]); j++) {
            size_t n = lengths[j], k;
            uint8_t *allocation = (uint8_t *)malloc(n + i ? n + i : 1);
            uint8_t *buf = allocation + i;
            uint64_t a = 1, b = 0;
            for (k = 0; k < n; k++) {
                buf[k] = i & 1 ? 255 : (uint8_t)(NEXT() >> 24);
                a += buf[k]; b += a;
            }
            if (ptpng_adler32(buf, n) !=
                (uint32_t)(((b % 65521u) << 16) | (a % 65521u))) {
                printf("adler boundary FAIL n=%zu offset=%u\n", n, i);
                free(allocation);
                return 1;
            }
            if (ptpng_crc32(buf, n) != ptpng_crc32_slice8(buf, n)) {
                printf("crc boundary FAIL n=%zu offset=%u\n", n, i);
                free(allocation);
                return 1;
            }
            free(allocation);
        }
    }
    printf("checksums OK\n");
    return 0;
}
