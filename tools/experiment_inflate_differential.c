/* Temporary paired-inflater driver for GCC ASan/UBSan experiments. */
#include "ptpng_internal.h"
#include <stdio.h>

int baseline_inflate(const uint8_t *, size_t, uint8_t *, size_t, uint32_t);

uint32_t ptpng_adler32(const uint8_t *data, size_t size)
{
    uint32_t a = 1, b = 0;
    while (size--) { a = (a + *data++) % 65521; b = (b + a) % 65521; }
    return a | (b << 16);
}

static uint32_t read32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

int main(int argc, char **argv)
{
    FILE *file;
    uint8_t header[12];
    size_t count = 0, valid = 0;
    if (argc != 2 || !(file = fopen(argv[1], "rb"))) return 2;
    while (fread(header, 1, sizeof(header), file) == sizeof(header)) {
        size_t size = read32(header), cap = read32(header + 4);
        uint32_t expected_size = read32(header + 8), flags;
        uint8_t *data = malloc(size ? size : 1), *out[2];
        uint8_t *expected = NULL;
        int rc[2];
        if (!data || !cap || cap > (1u << 20)) abort();
        if (fread(data, 1, size, file) != size) abort();
        if (expected_size != UINT32_MAX) {
            expected = malloc(expected_size ? expected_size : 1);
            if (!expected || expected_size != cap ||
                fread(expected, 1, expected_size, file) != expected_size) abort();
        }
        out[0] = malloc(cap); out[1] = malloc(cap);
        if (!out[0] || !out[1]) abort();
        for (flags = 0; flags < 2; ++flags) {
            rc[0] = baseline_inflate(data, size, out[0], cap, flags);
            rc[1] = ptpng_inflate(data, size, out[1], cap, flags);
            if (rc[0] != rc[1] || (!rc[0] && memcmp(out[0], out[1], cap)) ||
                (expected && (rc[1] || memcmp(expected, out[1], cap)))) {
                fprintf(stderr, "case=%zu input=%zu cap=%zu flags=%u rc=%d/%d\n",
                        count, size, cap, flags, rc[0], rc[1]);
                abort();
            }
            count++;
            if (expected) valid++;
        }
        free(data); free(out[0]); free(out[1]); free(expected);
    }
    if (!feof(file)) abort();
    fclose(file);
    printf("%zu sanitizer comparisons match baseline status and pixels; "
           "%zu valid cases independently match zlib\n", count, valid);
    return 0;
}
