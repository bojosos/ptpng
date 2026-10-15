/* Fast PNG writing. The format and filters follow W3C PNG, sections 5/9/10. */
#include "ptpng_internal.h"

static unsigned paeth(unsigned a, unsigned b, unsigned c)
{
    int p = (int)a + (int)b - (int)c;
    int da = abs(p - (int)a), db = abs(p - (int)b), dc = abs(p - (int)c);
    return da <= db && da <= dc ? a : db <= dc ? b : c;
}

void ptpng_encode_filter_scalar(uint8_t *dst, const uint8_t *src,
    const uint8_t *prev, size_t count, unsigned bpp, int filter)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        unsigned a = i >= bpp ? src[i-bpp] : 0;
        unsigned b = prev ? prev[i] : 0;
        unsigned c = prev && i >= bpp ? prev[i-bpp] : 0;
        unsigned predictor = filter == 1 ? a : filter == 2 ? b :
            filter == 3 ? (a+b)/2 : filter == 4 ? paeth(a,b,c) : 0;
        dst[i] = (uint8_t)(src[i] - predictor);
    }
}

uint64_t ptpng_encode_score_scalar(const uint8_t *src, size_t count)
{
    uint64_t score = 0;
    size_t i;
    for (i = 0; i < count; ++i) {
        unsigned x = src[i];
        score += x < 128 ? x : 256-x;
    }
    return score;
}

#if defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64)
/* Paired MSVC x64 measurements favor the existing encoder layout. */
static int choose_filter(uint8_t *dst, uint8_t *scratch, const uint8_t *src,
                         const uint8_t *prev, size_t count, unsigned bpp,
                         ptpng_encode_filter_fn forward, ptpng_encode_score_fn score)
{
    uint64_t best_score = score(src, count);
    int best = 0, f;
    memcpy(dst, src, count);
    for (f = 1; f < 5 && best_score; ++f) {
        uint64_t candidate;
        if (!prev && (f == 2 || f == 4)) continue;
        forward(scratch, src, prev, count, bpp, f);
        candidate = score(scratch, count);
        if (candidate < best_score) {
            best = f;
            best_score = candidate;
            memcpy(dst, scratch, count);
        }
    }
    return best;
}
#else
/* Score every residual, including details outside the former sample ranges.
 * Alternate output and scratch on improvement so each row needs at most one copy. */
static int choose_filter(uint8_t *dst, uint8_t *scratch, const uint8_t *src,
                         const uint8_t *prev, size_t count, unsigned bpp,
                         ptpng_encode_filter_fn forward, ptpng_encode_score_fn score)
{
    uint8_t *candidate_row = scratch;
    const uint8_t *best_row = src;
    uint64_t best_score = score(src, count);
    int best = 0, f;
    for (f = 1; f < 5 && best_score; ++f) {
        uint64_t candidate;
        /* On the first row Up duplicates None and Paeth duplicates Sub. */
        if (!prev && (f == 2 || f == 4)) continue;
        forward(candidate_row, src, prev, count, bpp, f);
        candidate = score(candidate_row, count);
        if (candidate < best_score) {
            best = f;
            best_score = candidate;
            best_row = candidate_row;
            candidate_row = candidate_row == scratch ? dst : scratch;
        }
    }
    if (best_row != dst) memcpy(dst, best_row, count);
    return best;
}
#endif

static void put32(uint8_t *p, uint32_t n)
{
    p[0] = (uint8_t)(n >> 24); p[1] = (uint8_t)(n >> 16);
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
}

static uint8_t *chunk(uint8_t *dst, const char *type,
                       const uint8_t *data, size_t size)
{
    put32(dst, (uint32_t)size);
    memcpy(dst+4, type, 4);
    if (size) memcpy(dst+8, data, size);
    put32(dst+8+size, ptpng_crc32(dst+4, size+4));
    return dst+size+12;
}

int ptpng_encode(const void *pixels, size_t pixels_size,
                 uint32_t width, uint32_t height, size_t stride,
                 int color_type, int bit_depth, const ptpng_encode_opts *opts,
                 void **out, size_t *out_len)
{
    static const uint8_t signature[8] = {137,80,78,71,13,10,26,10};
    const size_t chunk_size = 1u << 20;
    const uint8_t *src = (const uint8_t *)pixels, *prev = NULL;
    uint8_t *raw, *scratch = NULL, *compressed = NULL, *png, *p, header[13] = {0};
    size_t rowbytes, raw_size, compressed_size = 0, total, nchunks, offset;
    unsigned channels, bpp;
    uint32_t y;
    int rc, filter = opts ? opts->filter : PTPNG_ENCODE_FILTER_ADAPTIVE;
    ptpng_encode_filter_fn forward = ptpng_encode_filter_scalar;
    ptpng_encode_score_fn score = ptpng_encode_score_scalar;
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!out || !out_len || !pixels || !width || !height ||
        width > 0x7fffffffu || height > 0x7fffffffu || filter < -1 || filter > 4)
        return PTPNG_E_BAD_ARG;
    if ((bit_depth != 8 && bit_depth != 16) ||
        (color_type != 0 && color_type != 2 && color_type != 4 && color_type != 6))
        return PTPNG_E_COLOR_DEPTH;
    channels = color_type == 0 ? 1 : color_type == 2 ? 3 : color_type == 4 ? 2 : 4;
    bpp = channels * (unsigned)(bit_depth/8);
    if ((size_t)width > (SIZE_MAX-1)/bpp) return PTPNG_E_TOO_LARGE;
    rowbytes = (size_t)width*bpp;
    if (!stride) stride = rowbytes;
    if (stride < rowbytes) return PTPNG_E_BAD_ARG;
    if ((size_t)(height-1) > (SIZE_MAX-rowbytes)/stride)
        return PTPNG_E_TOO_LARGE;
    if ((size_t)(height-1)*stride + rowbytes > pixels_size)
        return PTPNG_E_BAD_ARG;
    if (height > SIZE_MAX/(rowbytes+1) ||
        height > PTPNG_DEFAULT_MAX_BYTES/(rowbytes+1))
        return PTPNG_E_TOO_LARGE;
    raw_size = (rowbytes+1)*height;
    raw = (uint8_t *)malloc(raw_size);
    if (!raw) return PTPNG_E_OUT_OF_MEMORY;
    if (filter < 0) {
        scratch = (uint8_t *)malloc(rowbytes);
        if (!scratch) { free(raw); return PTPNG_E_OUT_OF_MEMORY; }
    }
    ptpng_cpu_init();
#if PTPNG_X86
    if (ptpng_cpu.avx2) {
        forward = ptpng_encode_filter_avx2;
        score = ptpng_encode_score_avx2;
    }
#elif PTPNG_ARM_NEON
    if (ptpng_cpu.neon) {
        forward = ptpng_encode_filter_neon;
        score = ptpng_encode_score_neon;
    }
#endif
    for (y = 0; y < height; ++y) {
        int selected = filter;
        uint8_t *row = raw+(rowbytes+1)*y;
        if (filter < 0)
            selected = choose_filter(row+1, scratch, src, prev, rowbytes, bpp, forward, score);
        else if (!selected) memcpy(row+1, src, rowbytes);
        else forward(row+1, src, prev, rowbytes, bpp, selected);
        row[0] = (uint8_t)selected;
        prev = src;
        if (y+1 < height) src += stride;
    }
    free(scratch);
    rc = ptpng_deflate(raw, raw_size, &compressed, &compressed_size);
    free(raw);
    if (rc != PTPNG_OK) return rc;
    nchunks = compressed_size/chunk_size + (compressed_size%chunk_size != 0);
    if (compressed_size > SIZE_MAX-45 || nchunks > (SIZE_MAX-45-compressed_size)/12) {
        free(compressed);
        return PTPNG_E_TOO_LARGE;
    }
    total = 45 + compressed_size + 12*nchunks;
    png = (uint8_t *)malloc(total);
    if (!png) { free(compressed); return PTPNG_E_OUT_OF_MEMORY; }
    memcpy(png, signature, 8);
    put32(header, width); put32(header+4, height);
    header[8] = (uint8_t)bit_depth; header[9] = (uint8_t)color_type;
    p = chunk(png+8, "IHDR", header, 13);
    for (offset = 0; offset < compressed_size;) {
        size_t n = compressed_size-offset;
        if (n > chunk_size) n = chunk_size;
        p = chunk(p, "IDAT", compressed+offset, n);
        offset += n;
    }
    chunk(p, "IEND", NULL, 0);
    free(compressed);
    *out = png; *out_len = total;
    return PTPNG_OK;
}
