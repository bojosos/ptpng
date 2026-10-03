/* Decoder regressions built in memory, independent of a zlib dependency. */
#include "ptpng_internal.h"
#include <stdio.h>
#include <string.h>

static unsigned char png_data[65536];
static size_t png_size;
static int failures;

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static void be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static unsigned long crc32_ref(const unsigned char *p, size_t n)
{
    unsigned long c = 0xffffffffUL;
    while (n--) {
        unsigned k;
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ ((c & 1) ? 0xedb88320UL : 0);
    }
    return c ^ 0xffffffffUL;
}

static void chunk(const char *type, const void *data, size_t size)
{
    unsigned char *p = png_data + png_size;
    be32(p, (unsigned long)size);
    memcpy(p + 4, type, 4);
    if (size) memcpy(p + 8, data, size);
    be32(p + 8 + size, crc32_ref(p + 4, size + 4));
    png_size += size + 12;
}

/* One stored DEFLATE block with a zlib header and independent Adler-32. */
static size_t zstream(unsigned char *dst, const unsigned char *src, size_t n)
{
    unsigned long a = 1, b = 0;
    size_t i;
    dst[0] = 0x78; dst[1] = 0x01; dst[2] = 1;
    dst[3] = (unsigned char)n; dst[4] = (unsigned char)(n >> 8);
    dst[5] = (unsigned char)~n; dst[6] = (unsigned char)(~n >> 8);
    memcpy(dst + 7, src, n);
    for (i = 0; i < n; i++) {
        a = (a + src[i]) % 65521; b = (b + a) % 65521;
    }
    be32(dst + 7 + n, (b << 16) | a);
    return n + 11;
}

static const unsigned char ihdr[13] = {0,0,0,1, 0,0,0,1, 8,0,0,0,0};

static void begin_png(void)
{
    static const unsigned char sig[8] = {137,80,78,71,13,10,26,10};
    memcpy(png_data, sig, 8);
    png_size = 8;
    chunk("IHDR", ihdr, sizeof(ihdr));
}

static void pixels(void)
{
    unsigned char zs[32];
    static const unsigned char raw[2] = {0, 73};
    chunk("IDAT", zs, zstream(zs, raw, sizeof(raw)));
}

static int decode(ptpng_info *info)
{
    void *out = NULL;
    size_t len = 0;
    int rc = ptpng_decode(png_data, png_size, NULL, &out, &len, info);
    if (rc == PTPNG_OK) {
        CHECK(len == 1 && ((unsigned char *)out)[0] == 73);
    } else {
        CHECK(out == NULL && len == 0);
    }
    ptpng_free(out);
    return rc;
}

static void test_metadata(void)
{
    unsigned char payload[5200], body[5001];
    static const unsigned char date[7] = {7,234,9,24,23,59,60};
    static const unsigned char splt8[] = {'p',0,8, 1,2,3,4,0,9};
    static const unsigned char splt16[] = {'q',0,16, 0,1,0,2,0,3,0,4,0,9};
    ptpng_info info;
    size_t n;
    memset(body, 'x', sizeof(body));
    begin_png();
    chunk("tIME", date, sizeof(date));
    chunk("sPLT", splt8, sizeof(splt8));
    chunk("sPLT", splt16, sizeof(splt16));
    memcpy(payload, "short\0\0", 7);
    n = zstream(payload + 7, (const unsigned char *)"hello", 5);
    chunk("zTXt", payload, n + 7);
    memcpy(payload, "long\0\1\0en\0title\0", 16);
    n = zstream(payload + 16, body, sizeof(body));
    chunk("iTXt", payload, n + 16);
    memcpy(payload, "profile\0\0", 9);
    n = zstream(payload + 9, body, 37);
    chunk("iCCP", payload, n + 9);
    pixels(); chunk("IEND", NULL, 0);
    memset(&info, 0, sizeof(info));
    CHECK(decode(&info) == PTPNG_OK);
    CHECK(info.has_tIME && memcmp(info.tIME, date, sizeof(date)) == 0);
    CHECK(info.num_splts == 2);
    if (info.num_splts == 2) {
        CHECK(info.splts[0].nentries == 1 && info.splts[0].sample_depth == 8);
        CHECK(info.splts[1].nentries == 1 && info.splts[1].sample_depth == 16);
        CHECK(memcmp(info.splts[1].entries, splt16 + 3, 10) == 0);
    }
    CHECK(info.num_texts == 2);
    if (info.num_texts == 2) {
        CHECK(strcmp(info.texts[0].text, "hello") == 0);
        CHECK(strlen(info.texts[1].text) == sizeof(body));
        CHECK(memcmp(info.texts[1].text, body, sizeof(body)) == 0);
        CHECK(strcmp(info.texts[1].language, "en") == 0);
    }
    CHECK(info.has_iCCP && info.iccp_profile_len == 37);
    if (info.has_iCCP) CHECK(memcmp(info.iccp_profile, body, 37) == 0);
    ptpng_info_free(&info);
    ptpng_info_free(&info);
}

static void test_invalid_metadata(void)
{
    unsigned char payload[64];
    static const unsigned char bad_date[7] = {7,234,9,24,23,59,61};
    size_t n;
    ptpng_info info;
    begin_png();
    chunk("tIME", bad_date, sizeof(bad_date));
    memcpy(payload, "bad\0\0", 5);
    n = zstream(payload + 5, (const unsigned char *)"hello", 5);
    payload[5 + n - 1] ^= 1; /* valid chunk CRC, wrong stream Adler */
    chunk("zTXt", payload, n + 5);
    pixels(); chunk("IEND", NULL, 0);
    memset(&info, 0, sizeof(info));
    CHECK(decode(&info) == PTPNG_OK);
    CHECK(info.num_texts == 0 && !info.has_tIME);
    ptpng_info_free(&info);
}

static void test_cleanup_and_chunk_order(void)
{
    unsigned i;
    ptpng_info info;
    begin_png();
    for (i = 0; i < 80; i++) chunk("tEXt", "key\0value", 9);
    pixels(); chunk("IEND", NULL, 0);
    memset(&info, 0, sizeof(info));
    CHECK(decode(&info) == PTPNG_OK);
    CHECK(info.num_texts > 0 && info._n_allocs <= 64);
    ptpng_info_free(&info);
    begin_png();
    chunk("tEXt", "key\0value", 9);
    pixels();
    chunk("IHDR", ihdr, sizeof(ihdr));
    chunk("IEND", NULL, 0);
    CHECK(decode(NULL) == PTPNG_E_CHUNK_ORDER);
    begin_png(); pixels(); chunk("IEND", "x", 1);
    CHECK(decode(NULL) == PTPNG_E_BAD_CHUNK_LEN);
}

static void test_inflate_capacity(void)
{
    unsigned char source[5001], zs[5020];
    unsigned char *out = NULL;
    size_t n, len = 0;
    memset(source, 'x', sizeof(source));
    n = zstream(zs, source, sizeof(source));
    CHECK(ptpng_inflate_dyn(zs, n, 5000, &out, &len) == PTPNG_E_INFLATE_SIZE);
    CHECK(out == NULL && len == 0);
    CHECK(ptpng_inflate_dyn(zs, n, 5001, &out, &len) == PTPNG_OK);
    CHECK(out != NULL && len == 5001);
    if (out) CHECK(memcmp(out, source, len) == 0 && out[len] == 0);
    free(out);
    CHECK(ptpng_inflate_dyn(zs, n, 0, &out, &len) == PTPNG_E_BAD_ARG);
    CHECK(ptpng_inflate_dyn(zs, n, SIZE_MAX, &out, &len) == PTPNG_E_BAD_ARG);
    n = zstream(zs, source, 0);
    CHECK(ptpng_inflate_dyn(zs, n, 1, &out, &len) == PTPNG_OK);
    CHECK(out != NULL && len == 0);
    if (out) CHECK(out[0] == 0);
    free(out);
}

static void test_matching_output_formats(void)
{
    static const unsigned char rgb[] = {17,29,43, 61,73,89, 97,109,127, 149,163,181};
    static const unsigned char rgba[] = {17,29,43,0, 61,73,89,91,
                                        97,109,127,173, 149,163,181,255};
    static const unsigned char trns[] = {0,17, 0,29, 0,43};
    unsigned test;
    for (test = 0; test < 4; test++) {
        unsigned char header[13], raw[18], zs[32], expected[16];
        unsigned channels = test >= 2 ? 4 : 3;
        const unsigned char *samples = test >= 2 ? rgba : rgb;
        ptpng_opts opts = {0, PTPNG_OUT_RGB8, 0};
        ptpng_info info;
        void *out = NULL;
        size_t len = 0, rowbytes = 2 * channels, expected_len = 4 * channels;
        unsigned y, i;
        if (test == 1 || test == 2) opts.output_format = PTPNG_OUT_RGBA8;
        memcpy(header, ihdr, sizeof(header));
        header[3] = header[7] = 2;
        header[9] = test >= 2 ? 6 : 2;
        begin_png();
        png_size = 8;
        chunk("IHDR", header, sizeof(header));
        if (test < 2) chunk("tRNS", trns, sizeof(trns));
        for (y = 0; y < 2; y++) {
            raw[y * (rowbytes + 1)] = 0;
            memcpy(raw + y * (rowbytes + 1) + 1, samples + y * rowbytes, rowbytes);
        }
        chunk("IDAT", zs, zstream(zs, raw, 2 * (rowbytes + 1)));
        chunk("IEND", NULL, 0);
        memcpy(expected, samples, expected_len);
        if (test == 1) {
            /* RGB identity output ignores tRNS; RGBA must still expand it. */
            for (i = 0; i < 4; i++) {
                memcpy(expected + i * 4, rgb + i * 3, 3);
                expected[i * 4 + 3] = i == 0 ? 0 : 255;
            }
            expected_len = 16;
        } else if (test == 3) {
            /* RGB compaction must preserve the unread second RGBA row. */
            for (i = 0; i < 4; i++) memcpy(expected + i * 3, rgba + i * 4, 3);
            expected_len = 12;
        }
        memset(&info, 0, sizeof(info));
        CHECK(ptpng_decode(png_data, png_size, &opts, &out, &len, &info) == PTPNG_OK);
        CHECK(len == expected_len && info.rowbytes == expected_len / 2);
        CHECK(info.width == 2 && info.height == 2);
        if (out && len == expected_len) CHECK(memcmp(out, expected, len) == 0);
        else CHECK(out != NULL);
        CHECK(info.has_trns == (test < 2));
        ptpng_free(out);
        ptpng_info_free(&info);
    }
}

static void test_rgb_expansion_transparency(void)
{
    /* Cross SIMD block and row boundaries, with transparent pixels inside
     * the vector-sized prefix and the final scalar-sized pixel. */
    static const unsigned char trns[] = {0,17, 0,29, 0,43};
    unsigned transparent, y, x;
    for (transparent = 0; transparent < 2; transparent++) {
        unsigned char header[13], raw[104], zs[115], expected[136];
        ptpng_opts opts = {0, PTPNG_OUT_RGBA8, 0};
        ptpng_info info = {0};
        void *out = NULL;
        size_t len = 0;
        memcpy(header, ihdr, sizeof(header));
        be32(header, 17); be32(header + 4, 2); header[9] = 2;
        for (y = 0; y < 2; y++) {
            raw[y * 52] = 0;
            for (x = 0; x < 17; x++) {
                unsigned char *pixel = raw + y * 52 + 1 + x * 3;
                unsigned char *rgba = expected + (y * 17 + x) * 4;
                int match = x == 0 || x == 15 || x == 16;
                pixel[0] = match ? 17 : (unsigned char)(x + y);
                pixel[1] = match ? 29 : (unsigned char)(x * 7);
                pixel[2] = match ? 43 : (unsigned char)(x * 13);
                memcpy(rgba, pixel, 3);
                rgba[3] = transparent && match ? 0 : 255;
            }
        }
        begin_png(); png_size = 8;
        chunk("IHDR", header, sizeof(header));
        if (transparent) chunk("tRNS", trns, sizeof(trns));
        chunk("IDAT", zs, zstream(zs, raw, sizeof(raw)));
        chunk("IEND", NULL, 0);
        CHECK(ptpng_decode(png_data, png_size, &opts, &out, &len, &info) == PTPNG_OK);
        CHECK(len == sizeof(expected) && info.rowbytes == 68);
        if (out && len == sizeof(expected)) CHECK(memcmp(out, expected, len) == 0);
        else CHECK(out != NULL);
        ptpng_free(out); ptpng_info_free(&info);
    }
}

static void test_color_conversion_boundaries(void)
{
    unsigned expand, n, offset, i;
    ptpng_cpu_init();
    for (expand = 0; expand < 2; ++expand)
    for (n = 0; n <= 65; ++n)
    for (offset = 0; offset < 32; ++offset) {
        unsigned input_channels = expand ? 3 : 4;
        unsigned output_channels = expand ? 4 : 3;
        size_t input_size = (size_t)n * input_channels;
        size_t output_size = (size_t)n * output_channels;
        unsigned char *src = (unsigned char *)malloc(input_size + offset ? input_size + offset : 1);
        unsigned char *dst = (unsigned char *)malloc(output_size + offset + 1);
        unsigned char expected[65 * 4];
        struct ptpng_cvt cvt = {0};
        ptpng_cvt_fn fn = expand ? ptpng_cpu.cvt_table_rgba8[(2 << 4) | 3] :
                                  ptpng_cpu.cvt_table_rgb8[(6 << 4) | 3];
        if (!src || !dst) { free(src); free(dst); CHECK(0); return; }
        memset(src, 0xa5, offset);
        for (i = 0; i < input_size; ++i)
            src[offset + i] = (unsigned char)(i * 37 + n * 11);
        for (i = 0; i < n; ++i) {
            memcpy(expected + i * output_channels, src + offset + i * input_channels, 3);
            if (expand) expected[i * 4 + 3] = 255;
        }
        memset(dst, 0xa5, output_size + offset + 1);
        fn(src + offset, dst + offset, n, &cvt);
        CHECK(memcmp(dst + offset, expected, output_size) == 0);
        CHECK(dst[offset + output_size] == 0xa5);
        for (i = 0; i < offset; ++i) CHECK(dst[i] == 0xa5);
        if (!expand) {
            unsigned char guard = n ? src[offset + output_size] : 0;
            fn(src + offset, src + offset, n, &cvt);
            CHECK(memcmp(src + offset, expected, output_size) == 0);
            if (n) CHECK(src[offset + output_size] == guard);
            for (i = 0; i < offset; ++i) CHECK(src[i] == 0xa5);
        } else {
            memcpy(dst + offset, src + offset, input_size);
            cvt.reverse = 1;
            fn(dst + offset, dst + offset, n, &cvt);
            CHECK(memcmp(dst + offset, expected, output_size) == 0);
            CHECK(dst[offset + output_size] == 0xa5);
            for (i = 0; i < offset; ++i) CHECK(dst[i] == 0xa5);
        }
        free(src); free(dst);
    }
}

static void test_mixed_blocks(void)
{
    /* zlib-generated dynamic, fixed, stored, dynamic blocks, with sync
     * flush boundaries. Expected bytes are independent of the fixture. */
    static const unsigned char stream[] = {
        0x78,0x01,0xec,0xcb,0x59,0x02,0x42,0x00,0x00,0x40,0x41,0x29,0x54,0x28,0x25,0x2d,
        0x88,0xf6,0x52,0x68,0x51,0x59,0xe3,0xfe,0xb7,0x72,0x8d,0x3e,0xde,0xfc,0x8f,0xd0,
        0x11,0xbb,0x3d,0x49,0x56,0xfa,0x83,0xa1,0xaa,0xe9,0xa3,0xb1,0x31,0x99,0x9a,0x33,
        0x6b,0xbe,0x58,0xae,0x6c,0xc7,0x5d,0x7b,0xfe,0x66,0xbb,0xdb,0x1f,0x8e,0xa7,0xf3,
        0x25,0xb8,0xde,0xc2,0x28,0xbe,0x3f,0x9e,0xaf,0xe4,0xfd,0xf9,0xa6,0x59,0x5e,0x94,
        0xd5,0xaf,0x6e,0x04,0x3e,0x9f,0xcf,0xe7,0xf3,0xf9,0x7c,0x3e,0x9f,0xcf,0xe7,0xf3,
        0xf9,0x7c,0x3e,0x9f,0xcf,0xe7,0xf3,0xf9,0xfc,0x3f,0xfb,0x2d,0x00,0x00,0x00,0xff,
        0xff,0x4a,0xcb,0xac,0x48,0x4d,0x49,0x43,0x25,0x00,0x00,0x00,0x00,0xff,0xff,0x00,
        0x06,0x00,0xf9,0xff,0x73,0x74,0x6f,0x72,0x65,0x64,0x00,0x00,0x00,0xff,0xff,0xed,
        0xcb,0x59,0x02,0x42,0x00,0x00,0x40,0x41,0x29,0x54,0x28,0x25,0x2d,0x88,0xf6,0x52,
        0x68,0x51,0x59,0xe3,0xfe,0xb7,0x72,0x8d,0x3e,0xde,0xfc,0x8f,0xd0,0x11,0xbb,0x3d,
        0x49,0x56,0xfa,0x83,0xa1,0xaa,0xe9,0xa3,0xb1,0x31,0x99,0x9a,0x33,0x6b,0xbe,0x58,
        0xae,0x6c,0xc7,0x5d,0x7b,0xfe,0x66,0xbb,0xdb,0x1f,0x8e,0xa7,0xf3,0x25,0xb8,0xde,
        0xc2,0x28,0xbe,0x3f,0x9e,0xaf,0xe4,0xfd,0xf9,0xa6,0x59,0x5e,0x94,0xd5,0xaf,0x6e,
        0x04,0x3e,0x9f,0xcf,0xe7,0xf3,0xf9,0x7c,0x3e,0x9f,0xcf,0xe7,0xf3,0xf9,0x7c,0x3e,
        0x9f,0xcf,0xe7,0xf3,0xf9,0xfc,0x3f,0xfb,0x2d,0xa9,0x09,0x32,0x2c
    };
    unsigned char expected[12826], out[12826];
    size_t i;
    for (i = 0; i < 6400; i++) expected[i] = (unsigned char)(i % 64);
    memcpy(expected + 6400, "fixedfixedfixedfixedstored", 26);
    memcpy(expected + 6426, expected, 6400);
    CHECK(ptpng_inflate(stream, sizeof(stream), out, sizeof(out), 0) == PTPNG_OK);
    CHECK(memcmp(out, expected, sizeof(out)) == 0);
    for (i = 0; i < sizeof(stream); i++)
        CHECK(ptpng_inflate(stream, i, out, sizeof(out), 0) != PTPNG_OK);
}

static void put_bits(unsigned char *dst, unsigned *bit, unsigned v, unsigned n)
{
    while (n--) {
        dst[*bit >> 3] |= (unsigned char)((v & 1) << (*bit & 7));
        ++*bit;
        v >>= 1;
    }
}

static void fixed_symbol(unsigned char *dst, unsigned *bit, unsigned symbol)
{
    unsigned code, n, reversed = 0, i;
    if (symbol < 144) { code = symbol + 48; n = 8; }
    else if (symbol < 256) { code = symbol + 256; n = 9; }
    else if (symbol < 280) { code = symbol - 256; n = 7; }
    else { code = symbol - 280 + 192; n = 8; }
    for (i = 0; i < n; i++) { reversed = (reversed << 1) | (code & 1); code >>= 1; }
    put_bits(dst, bit, reversed, n);
}

static void test_literal_boundaries(void)
{
    unsigned len, invalid;
    for (len = 0; len <= 33; len++)
    for (invalid = 0; invalid <= 2; invalid++) {
        unsigned char zs[64] = {0x78,0x01}, expected[33], out[42];
        unsigned bit = 16, i;
        unsigned long a = 1, b = 0;
        size_t n, cut, capacity = invalid ? len + 8 : len;
        put_bits(zs, &bit, 3, 3); /* final fixed block */
        for (i = 0; i < len; i++) {
            /* Alternate eight- and nine-bit codes across batch boundaries. */
            expected[i] = (unsigned char)(i & 1 ? 144 + i : i * 3);
            fixed_symbol(zs, &bit, expected[i]);
            a += expected[i]; b += a;
        }
        if (invalid) fixed_symbol(zs, &bit, 285 + invalid); /* reserved 286/287 */
        fixed_symbol(zs, &bit, 256);
        n = (bit + 7) / 8;
        be32(zs + n, ((b % 65521) << 16) | (a % 65521));
        n += 4;
        memset(out, 0xa5, sizeof(out));
        if (invalid) {
            CHECK(ptpng_inflate(zs, n, out, capacity, 0) != PTPNG_OK);
        } else if (len) {
            CHECK(ptpng_inflate(zs, n, out, len, 0) == PTPNG_OK);
            CHECK(memcmp(out, expected, len) == 0);
        } else {
            unsigned char *empty = NULL;
            size_t empty_len = 1;
            CHECK(ptpng_inflate_dyn(zs, n, 1, &empty, &empty_len) == PTPNG_OK);
            CHECK(empty != NULL && empty_len == 0);
            free(empty);
        }
        CHECK(out[capacity] == 0xa5);
        if (invalid) continue;
        for (capacity = 0; capacity < len; capacity++) {
            memset(out, 0xa5, sizeof(out));
            CHECK(ptpng_inflate(zs, n, out, capacity, 0) != PTPNG_OK);
            CHECK(out[capacity] == 0xa5);
        }
        for (cut = 0; cut < n; cut++) {
            /* Exact input allocations let ASan detect refill overreads. */
            unsigned char *prefix = (unsigned char *)malloc(cut ? cut : 1);
            if (!prefix) { CHECK(prefix != NULL); return; }
            memcpy(prefix, zs, cut);
            capacity = len ? len : 1;
            memset(out, 0xa5, sizeof(out));
            CHECK(ptpng_inflate(prefix, cut, out, capacity, 0) != PTPNG_OK);
            CHECK(out[capacity] == 0xa5);
            free(prefix);
        }
    }
}

static void test_match_copies(void)
{
    static const unsigned bases[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,
        31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned distances[] = {1,2,3,4,5,7,9,13,17,25,33,49};
    unsigned len, dist, padding;
    for (dist = 1; dist <= 64; dist++)
    for (len = 3; len <= 258; len++)
    for (padding = 0; padding <= 32; padding += 32) {
        unsigned char zs[160] = {0x78,0x01}, expected[354];
        unsigned size = dist + len + padding;
        unsigned char *out = (unsigned char *)malloc(size);
        unsigned bit = 16, index = 0, extra, i, dcode = 0;
        size_t n;
        unsigned long a = 1, b = 0;
        if (!out) { CHECK(out != NULL); return; }
        put_bits(zs, &bit, 3, 3); /* final fixed block */
        for (i = 0; i < dist + len; i++)
            expected[i] = (unsigned char)((i % dist) * 37 + 11);
        for (i = 0; i < dist; i++) fixed_symbol(zs, &bit, expected[i]);
        while (index < 28 && bases[index + 1] <= len) ++index;
        extra = index < 8 || index == 28 ? 0 : (index - 4) / 4;
        fixed_symbol(zs, &bit, index + 257);
        put_bits(zs, &bit, len - bases[index], extra);
        while (dcode < 11 && distances[dcode + 1] <= dist) ++dcode;
        for (i = 0; i < 5; i++) put_bits(zs, &bit, (dcode >> (4 - i)) & 1, 1);
        extra = dcode < 4 ? 0 : dcode / 2 - 1;
        put_bits(zs, &bit, dist - distances[dcode], extra);
        /* Trailing literals exercise speculative word stores inside the
         * allocation; padding=0 exercises the exact output boundary. */
        for (i = dist + len; i < size; i++) {
            expected[i] = (unsigned char)(i ^ 0xa5);
            fixed_symbol(zs, &bit, expected[i]);
        }
        fixed_symbol(zs, &bit, 256);
        n = (bit + 7) / 8;
        for (i = 0; i < size; i++) { a += expected[i]; b += a; }
        be32(zs + n, ((b % 65521) << 16) | (a % 65521));
        n += 4;
        CHECK(ptpng_inflate(zs, n, out, size, 0) == PTPNG_OK);
        CHECK(memcmp(out, expected, size) == 0);
        out[size - 1] = 0xa5;
        CHECK(ptpng_inflate(zs, n, out, size - 1, 0) == PTPNG_E_INFLATE_SIZE);
        CHECK(out[size - 1] == 0xa5);
        free(out);
    }
}

int main(void)
{
    test_metadata();
    test_invalid_metadata();
    test_cleanup_and_chunk_order();
    test_inflate_capacity();
    test_matching_output_formats();
    test_rgb_expansion_transparency();
    test_color_conversion_boundaries();
    test_mixed_blocks();
    test_literal_boundaries();
    test_match_copies();
    if (failures) return 1;
    puts("decoder: metadata, limits, and malformed chunk regressions OK");
    return 0;
}
