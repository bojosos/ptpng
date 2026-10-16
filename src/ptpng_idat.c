/* Fragmented IDAT input handling stays outside the general PNG and Huffman
 * decoder loops. Single-chunk input continues to use the caller's buffer. */
#include "ptpng_internal.h"

#if defined(__linux__)
PTPNG_API_INLINE int stored_byte(const struct ptpng_inflate_chunk *chunks,
                                 size_t count, size_t *index, size_t *offset,
                                 uint8_t *byte)
{
    while (*index < count && *offset == chunks[*index].len) {
        ++*index;
        *offset = 0;
    }
    if (*index == count) return 0;
    *byte = chunks[*index].p[(*offset)++];
    return 1;
}

static int try_inflate_stored(const struct ptpng_inflate_chunk *chunks,
                              size_t count, uint8_t *out, size_t out_len,
                              uint32_t flags)
{
    size_t index = 0, offset = 0, pos = 0;
    uint8_t cmf, flg, header, lengths[4], byte;
    uint32_t want = 0;
    unsigned i;
    if (!stored_byte(chunks, count, &index, &offset, &cmf) ||
        !stored_byte(chunks, count, &index, &offset, &flg)) return 0;
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 ||
        (((unsigned)cmf << 8) | flg) % 31 || (flg & 32)) return 0;
    for (;;) {
        unsigned length, complement;
        if (!stored_byte(chunks, count, &index, &offset, &header) ||
            (header & 6)) return 0;
        /* Each stored block starts and ends byte-aligned. Its five high
         * header bits are padding, which the general inflater also ignores. */
        for (i = 0; i < 4; ++i)
            if (!stored_byte(chunks, count, &index, &offset, &lengths[i])) return 0;
        length = (unsigned)lengths[0] | ((unsigned)lengths[1] << 8);
        complement = (unsigned)lengths[2] | ((unsigned)lengths[3] << 8);
        if (length != (complement ^ 65535u) || length > out_len - pos) return 0;
        while (length) {
            size_t take;
            while (index < count && offset == chunks[index].len) {
                ++index;
                offset = 0;
            }
            if (index == count) return 0;
            take = chunks[index].len - offset;
            if (take > length) take = length;
            memcpy(out + pos, chunks[index].p + offset, take);
            pos += take;
            offset += take;
            length -= (unsigned)take;
        }
        if (header & 1) break;
    }
    if (pos != out_len) return 0;
    for (i = 0; i < 4; ++i) {
        if (!stored_byte(chunks, count, &index, &offset, &byte)) return 0;
        want = (want << 8) | byte;
    }
    while (index < count && offset == chunks[index].len) {
        ++index;
        offset = 0;
    }
    if (index != count) return 0; /* no bytes may follow the Adler checksum */
    if (!(flags & PTPNG_INF_NO_ADLER) && ptpng_adler32(out, out_len) != want) return 0;
    return 1;
}

int ptpng_inflate_chunks(const struct ptpng_inflate_chunk *chunks,
                         size_t count, size_t in_len, uint8_t *out,
                         size_t out_len, uint32_t flags)
{
    uint8_t *input;
    size_t i, offset = 0;
    int rc;
    if (count <= 1)
        return ptpng_inflate(count ? chunks[0].p : (const uint8_t *)"",
                              in_len, out, out_len, flags);
    if (try_inflate_stored(chunks, count, out, out_len, flags))
        return PTPNG_OK;
    input = (uint8_t *)malloc(in_len);
    if (!input) return PTPNG_E_OUT_OF_MEMORY;
    for (i = 0; i < count; ++i) {
        memcpy(input + offset, chunks[i].p, chunks[i].len);
        offset += chunks[i].len;
    }
    rc = ptpng_inflate(input, in_len, out, out_len, flags);
    free(input);
    return rc;
}
#endif
