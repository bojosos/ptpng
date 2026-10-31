/* Separate compilation keeps fixed-code decoding out of the established
 * general inflater's optimization decisions and instruction placement. */
#include "ptpng_internal.h"

#ifndef INFLATE_FIXED_STREAM
#define INFLATE_FIXED_STREAM 1
#endif

#if INFLATE_FIXED_STREAM
#define F_LIT UINT32_C(0x80000000)
#define F_EOB UINT32_C(0x40000000)

int ptpng_inflate_fallback(const uint8_t *in, size_t in_len, uint8_t *out,
                           size_t out_len, uint32_t flags);

#if defined(_MSC_VER)
#pragma bss_seg(push, ".bss$zzfixed")
#endif
static struct {
    uint32_t lit[512], dist[32];
    int ready;
} fixed;
#if defined(_MSC_VER)
#pragma bss_seg(pop)
#endif

#if defined(_MSC_VER)
#pragma code_seg(push, ".text$zzfixed")
#pragma const_seg(push, ".rdata$zzfixed")
#endif

static void fixed_init(void)
{
    static const uint16_t lengths[] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
        115,131,163,195,227,258};
    static const uint8_t length_extra[] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t distances[] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,
        1537,2049,3073,4097,6145,8193,12289,16385,24577};
    unsigned symbol;
    for (symbol = 0; symbol < 288; ++symbol) {
        unsigned code, nb, reversed = 0, i, slot;
        uint32_t entry;
        if (symbol < 144) { code = symbol + 48; nb = 8; }
        else if (symbol < 256) { code = symbol + 256; nb = 9; }
        else if (symbol < 280) { code = symbol - 256; nb = 7; }
        else { code = symbol - 280 + 192; nb = 8; }
        for (i = 0; i < nb; ++i) {
            reversed = (reversed << 1) | (code & 1);
            code >>= 1;
        }
        if (symbol < 256) entry = F_LIT | (symbol << 8) | nb;
        else if (symbol == 256) entry = F_EOB | nb;
        else if (symbol < 286) entry = ((uint32_t)lengths[symbol - 257] << 8) |
                ((uint32_t)length_extra[symbol - 257] << 17) | nb;
        else entry = F_LIT;
        for (slot = reversed; slot < 512; slot += 1u << nb) fixed.lit[slot] = entry;
    }
    for (symbol = 0; symbol < 32; ++symbol) {
        unsigned code = symbol, reversed = 0, i;
        for (i = 0; i < 5; ++i) {
            reversed = (reversed << 1) | (code & 1);
            code >>= 1;
        }
        fixed.dist[reversed] = symbol < 30 ? ((uint32_t)distances[symbol] << 8) |
            ((uint32_t)(symbol < 4 ? 0 : symbol / 2 - 1) << 24) | 5 : 0;
    }
    fixed.ready = 1;
}

static int inflate_fixed_stream(const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t out_len, uint32_t flags)
{
    const uint8_t *end = in + in_len;
    uint64_t bits = 0;
    unsigned count = 0, final;
    size_t pos = 0;
    unsigned cmf = in[0], flg = in[1];
    if ((cmf & 15) != 8 || (cmf >> 4) > 7 ||
        ((cmf << 8) | flg) % 31 || (flg & 32)) return 0;
    if (!fixed.ready) fixed_init();
    in += 2;

#define FIXED_REFILL() do {                                             \
    if (count < 48) {                                                   \
        if ((size_t)(end - in) >= 8) {                                  \
            uint64_t word;                                             \
            memcpy(&word, in, sizeof(word));                            \
            bits |= word << count;                                     \
            in += (64 - count) >> 3;                                   \
            count += (64 - count) & ~7u;                               \
        } else {                                                       \
            while (count <= 56 && in < end) {                           \
                bits |= (uint64_t)*in++ << count;                       \
                count += 8;                                           \
            }                                                          \
        }                                                              \
    }                                                                  \
} while (0)
#define FIXED_DROP(n) do { bits >>= (n); count -= (n); } while (0)

    do {
        FIXED_REFILL();
        if (count < 3 || ((unsigned)bits & 6) != 2) return 0;
        final = (unsigned)bits & 1;
        FIXED_DROP(3);
        for (;;) {
            uint32_t entry;
            unsigned nb;
            FIXED_REFILL();
            if (out_len - pos >= 4 && count >= 36) {
#define FIXED_LITERAL() do {                                            \
    unsigned width = 8u + ((UINT32_C(0x88888880) >>                      \
                            ((unsigned)bits & 31)) & 1u);              \
    entry = fixed.lit[(unsigned)bits & 511];                            \
    if (entry <= F_LIT) goto fixed_single;                              \
    out[pos++] = (uint8_t)(entry >> 8);                                 \
    FIXED_DROP(width);                                                 \
} while (0)
                FIXED_LITERAL();
                FIXED_LITERAL();
                FIXED_LITERAL();
                FIXED_LITERAL();
#undef FIXED_LITERAL
                continue;
            }
fixed_single:
            FIXED_REFILL();
            entry = fixed.lit[(unsigned)bits & 511];
            nb = entry & 15;
            if (!nb || count < nb) return 0;
            FIXED_DROP(nb);
            if (entry & F_LIT) {
                if (pos == out_len) return 0;
                out[pos++] = (uint8_t)(entry >> 8);
            } else if (entry & F_EOB) {
                break;
            } else {
                unsigned len = (entry >> 8) & 511;
                unsigned extra = (entry >> 17) & 7;
                unsigned distance;
                FIXED_REFILL();
                if (count < extra + 5) return 0;
                len += (unsigned)bits & ((1u << extra) - 1);
                FIXED_DROP(extra);
                entry = fixed.dist[(unsigned)bits & 31];
                if (!(entry & 15)) return 0;
                FIXED_DROP(5);
                extra = (entry >> 24) & 15;
                if (count < extra) return 0;
                distance = ((entry >> 8) & 32767) +
                           ((unsigned)bits & ((1u << extra) - 1));
                FIXED_DROP(extra);
                if (distance > pos || len > out_len - pos) return 0;
                while (len--) {
                    out[pos] = out[pos - distance];
                    ++pos;
                }
            }
        }
    } while (!final);
#undef FIXED_REFILL
#undef FIXED_DROP

    in -= count >> 3;
    if ((size_t)(end - in) != 4 || pos != out_len) return 0;
    if (!(flags & PTPNG_INF_NO_ADLER)) {
        uint32_t want = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                        ((uint32_t)in[2] << 8) | in[3];
        if (ptpng_adler32(out, pos) != want) return 0;
    }
    return 1;
}

int ptpng_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
                  size_t out_len, uint32_t flags)
{
    /* Incompressible fixed streams can amortize this separate decoder.
     * Any unsupported or malformed stream retains the original result. */
    if (!in || !out || !out_len) return PTPNG_E_BAD_ARG;
    if (ptpng_inflate_prefers_fixed(in, in_len, out_len) &&
        inflate_fixed_stream(in, in_len, out, out_len, flags)) return PTPNG_OK;
    return ptpng_inflate_fallback(in, in_len, out, out_len, flags);
}

int ptpng_inflate_prefers_fixed(const uint8_t *in, size_t in_len,
                                size_t out_len)
{
    return in_len >= 6 && in_len >= out_len && (in[2] & 6) == 2;
}

#if defined(_MSC_VER)
#pragma const_seg(pop)
#pragma code_seg(pop)
#endif
#endif
