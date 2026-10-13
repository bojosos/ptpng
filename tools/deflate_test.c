/* Raw encoder coverage, including hash-window wraps and stored blocks.
 * Define PTPNG_TEST_ZLIB to also decode each stream with independent zlib. */
#include "ptpng_internal.h"
#include <stdio.h>

#ifdef PTPNG_TEST_ZLIB
#include <zlib.h>

/* Common PNG residuals have a small alphabet or long flat runs. Compare
 * compression quality as well as round trips: fixed codes alone miss both. */
static int check_compression_quality(void)
{
    const size_t size = 262144;
    uint8_t *src = (uint8_t *)malloc(size);
    uint8_t *reference = (uint8_t *)malloc(compressBound((uLong)size));
    uint32_t state = UINT32_C(0x87ad130f);
    unsigned mode;
    if (!src || !reference) { free(src); free(reference); return 1; }
    for (mode = 0; mode < 5; ++mode) {
        uint8_t *encoded = NULL;
        size_t encoded_size = 0, i;
        uLongf reference_size = compressBound((uLong)size);
        for (i = 0; i < size; ++i) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            /* Binary residuals put more than 64 candidates in six-byte
             * hash chains. Retain quality there and on mixed content. */
            src[i] = mode == 0 ? 0 : mode == 1 ? (uint8_t)(state & 3) :
                mode == 2 ? (i < size/2 ? (uint8_t)(state & 3) : (uint8_t)state) :
                mode == 3 ? (uint8_t)(state & 1) :
                i < size/2 ? (uint8_t)(state & 1) : (uint8_t)state;
        }
        if (compress2(reference, &reference_size, src, (uLong)size, 6) != Z_OK ||
            ptpng_deflate(src, size, &encoded, &encoded_size) != PTPNG_OK ||
            encoded_size > (size_t)reference_size * 11 / 10 + 16) {
            fprintf(stderr, "compression quality pattern=%u: ptpng=%zu zlib6=%lu "
                    "(limit: reference + 10%% + 16 bytes)\n", mode, encoded_size,
                    (unsigned long)reference_size);
            free(encoded); free(reference); free(src); return 1;
        }
        printf("compression quality pattern=%u: %zu vs %lu bytes\n", mode,
               encoded_size, (unsigned long)reference_size);
        {
            uLongf decoded_size = (uLongf)size;
            if (uncompress(reference, &decoded_size, encoded, (uLong)encoded_size) != Z_OK ||
                decoded_size != size || memcmp(reference, src, size)) {
                fprintf(stderr, "compression quality round trip failed, pattern=%u\n", mode);
                free(encoded); free(reference); free(src); return 1;
            }
        }
        free(encoded);
    }
    free(reference); free(src); return 0;
}
#endif

static uint32_t random_state = UINT32_C(0x592182a);

static uint32_t random_word(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void fill_input(uint8_t *src, size_t size, unsigned mode)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        switch (mode) {
        case 0: src[i] = 0; break;
        case 1: src[i] = (uint8_t)random_word(); break;
        case 2: src[i] = (uint8_t)i; break;
        case 3: src[i] = (uint8_t)(random_word() & 3); break;
        case 4: src[i] = (uint8_t)(i % 3); break;
        case 5:
            src[i] = i >= 32768 ? src[i - 32768] : (uint8_t)random_word();
            break;
        case 6: src[i] = (uint8_t)((i / 1000) % 3); break;
        default:
            src[i] = i >= 31 && random_word() % 3 ?
                     src[i - 31] : (uint8_t)random_word();
            break;
        }
    }
}

static int check_stream(size_t size, unsigned mode)
{
    static const uint8_t empty_stream[8] = { 0x78,1,3,0,0,0,0,1 };
    uint8_t *src = (uint8_t *)malloc(size ? size : 1);
    uint8_t *decoded = (uint8_t *)malloc(size ? size : 1);
    uint8_t *encoded = NULL;
    size_t encoded_size = 0;
    size_t blocks = size ? size / 65535 + (size % 65535 != 0) : 1;
    int failed = 1, err;
    if (!src || !decoded) {
        fprintf(stderr, "test allocation failed\n");
        goto done;
    }
    fill_input(src, size, mode);
    err = ptpng_deflate(src, size, &encoded, &encoded_size);
    if (err != PTPNG_OK || !encoded || encoded_size < 8 ||
        encoded_size > size + blocks * 5 + 6) {
        fprintf(stderr, "deflate failed: error=%d bytes=%zu\n", err, encoded_size);
        goto done;
    }
    /* The PNG inflater requires nonempty output. Check the empty raw
     * stream directly, and independently with zlib when available. */
    if (size) {
        err = ptpng_inflate(encoded, encoded_size, decoded, size, 0);
        if (err != PTPNG_OK || memcmp(decoded, src, size)) {
            fprintf(stderr, "ptpng inflate failed: error=%d\n", err);
            goto done;
        }
    } else if (encoded_size != sizeof(empty_stream) ||
               memcmp(encoded, empty_stream, sizeof(empty_stream))) {
        fprintf(stderr, "invalid empty stream\n");
        goto done;
    }
#ifdef PTPNG_TEST_ZLIB
    {
        uLongf decoded_size = (uLongf)(size ? size : 1);
        memset(decoded, 0xa5, size ? size : 1);
        err = uncompress(decoded, &decoded_size, encoded, (uLong)encoded_size);
        if (err != Z_OK || decoded_size != size || memcmp(decoded, src, size)) {
            fprintf(stderr, "zlib inflate failed: error=%d bytes=%lu\n",
                    err, (unsigned long)decoded_size);
            goto done;
        }
    }
#endif
    failed = 0;
done:
    if (failed) fprintf(stderr, "input size=%zu pattern=%u\n", size, mode);
    free(encoded);
    free(decoded);
    free(src);
    return failed;
}

int main(int argc, char **argv)
{
    static const size_t boundaries[] = {
        63,64,65,127,128,129,255,256,257,258,259,260,
        1023,1024,1025,32767,32768,32769,65534,65535,65536,65537,
        131069,131070,131071,131072,131073,
        1048575,1048576,1048577,1048834,2097153
    };
    unsigned long iterations = 1000, i;
    unsigned mode, count = 0;
    uint8_t dummy = 0, *output = &dummy;
    size_t output_size = 123;
    if (argc > 2) {
        fprintf(stderr, "usage: deflate_test [random-iterations]\n");
        return 2;
    }
    if (argc == 2) {
        char *end;
        iterations = strtoul(argv[1], &end, 10);
        if (!argv[1][0] || *end || !iterations || iterations > 100000) {
            fprintf(stderr, "random-iterations must be 1..100000\n");
            return 2;
        }
    }
    ptpng_cpu_init();
#ifdef PTPNG_TEST_ZLIB
    if (check_compression_quality()) return 1;
#endif
    if (ptpng_deflate(&dummy, SIZE_MAX, &output, &output_size) !=
        PTPNG_E_TOO_LARGE || output != NULL || output_size != 0) {
        fprintf(stderr, "overflow must fail before reading input and clear output\n");
        return 1;
    }
    for (mode = 0; mode < 8; ++mode) {
        for (i = 0; i <= 33; ++i) {
            if (check_stream(i, mode)) return 1;
            ++count;
        }
        for (i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); ++i) {
            if (check_stream(boundaries[i], mode)) return 1;
            ++count;
        }
    }
    for (i = 0; i < iterations; ++i) {
        size_t size = random_word() % 131073;
        if (check_stream(size, (unsigned)(i % 8))) return 1;
        ++count;
    }
    printf("deflate: %u streams passed", count);
#ifdef PTPNG_TEST_ZLIB
    printf(" (ptpng and independent zlib)");
#endif
    printf("\n");
    return 0;
}
