/*
 * ptpng_inflate.c - high performance zlib (RFC1950/1951) inflater.
 *
 * Performance design:
 *  - 64-bit bit reader; bits consumed from the low end; Huffman tables
 *    built with pre-reversed codes so one lookup is a single masked peek
 *    tbl[bitbuf & (TBL-1)].  Word refill keeps 48..64 bits available via
 *    whole-byte absorption (OR of overlapping bits is idempotent).
 *  - Flat root tables (litlen 10 bits, dist 8 bits, codelen 7 bits) with
 *    subtable arenas for codes longer than the root; entries precompute
 *    length/distance bases and extra-bit counts so decoding is
 *    load/shift/add with no symbol post-processing.
 *  - Fast loop while input/output slack remains: one branchless refill per
 *    match or per four literals, no per-symbol bounds checks, and match
 *    copies in 16-byte (dist >= 16), 8-byte (dist >= 8), broadcast
 *    (dist 1) or pattern-extending (dist 2..7) steps that may overrun the
 *    match end inside the slack.
 *  - A careful single-symbol path handles the stream ends with exact
 *    bounds and truncation checks.
 *  - The PNG caller knows the exact uncompressed size; we decode
 *    directly into the final buffer and verify the adler32.
 */
#include "ptpng_internal.h"

#define LITLEN_ROOT   10
#define LITLEN_SIZE   (1u << LITLEN_ROOT)
#define LITLEN_ARENA  2048
#ifndef INFLATE_FIXED_STREAM
#define INFLATE_FIXED_STREAM 1
#endif
#define DIST_ROOT     8
#define DIST_SIZE     (1u << DIST_ROOT)
#define DIST_ARENA    4096
#define CL_ROOT       7
#define CL_SIZE       (1u << CL_ROOT)

/* Decode table entries:
 *   bits 0..7   bits consumed: code length plus extra bits
 *   bits 8..11  code length (extra bits follow it); subtable index bits
 *   bit  12     F_SUB  subtable pointer: arena offset in bits 16..30
 *   bit  13     F_EOB  end of block
 *   bit  14     F_BAD  invalid or unused code
 *   bits 16..30 literal byte, length base, distance base or CL symbol
 *   bit  31     F_LIT  literal (also CL symbols)
 * Consuming bits 0..7 at once lets extra bits come from a saved copy of
 * the bit buffer instead of a second dependent shift. */
#define F_LIT       0x80000000u
#define F_BAD       0x00004000u
#define F_EOB       0x00002000u
#define F_SUB       0x00001000u
#define F_EXC       (F_BAD | F_EOB | F_SUB)
#define FAST_IN_MARGIN  32
#define FAST_OUT_MARGIN 320
/* Sink notifications: every SINK_STEP output bytes, for the bytes at least
 * one deflate window behind the current position. */
#define SINK_WINDOW 32768
#define SINK_STEP   65536

#define SINK_NOTIFY()                                                       do {                                                                        if (pos >= next_sink) {                                                     sink->progress(sink, pos - SINK_WINDOW, 0);                             next_sink = pos + SINK_STEP;                                        }                                                                   } while (0)

static const uint16_t len_base_tab[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
    115,131,163,195,227,258 };
static const uint8_t len_extra_tab[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dist_base_tab[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,
    1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t dist_extra_tab[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };
static const uint8_t cl_perm[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };

/* unaligned little-endian moves via memcpy: free of aliasing UB on
 * every compiler, and still a single mov on x86-64 */
#define LOAD64LE(p)     ptpng_load64le(p)
#define STORE64LE(p, v) ptpng_store64le(p, v)

PTPNG_API_INLINE uint64_t ptpng_load64le(const void *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}
PTPNG_API_INLINE void ptpng_store64le(void *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

PTPNG_API_INLINE uint32_t rev_bits(uint32_t v, unsigned n)
{
    uint32_t r = 0;
    do { r = (r << 1) | (v & 1); v >>= 1; } while (--n);
    return r;
}

/* scratch shared by builds (single-threaded by contract) */
static uint32_t sym_code[320];

static void fill_bad(uint32_t *t, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        t[i] = F_BAD;
}

/* Entry for symbol i whose remaining code length in this table is cl. */
static uint32_t make_entry(int kind, unsigned i, unsigned cl)
{
    if (kind == 1) {
        if (i < 30)
            return ((uint32_t)dist_base_tab[i] << 16) | (cl << 8) |
                   (cl + dist_extra_tab[i]);
        return F_BAD | cl; /* symbols 30/31 are invalid */
    }
    if (kind == 2 || i < 256)
        return F_LIT | ((uint32_t)i << 16) | cl;
    if (i == 256)
        return F_EOB | cl;
    if (i < 286) {
        unsigned s = i - 257;
        return ((uint32_t)len_base_tab[s] << 16) | (cl << 8) |
               (cl + len_extra_tab[s]);
    }
    return F_BAD | cl; /* symbols 286/287 are invalid */
}

/*
 * Build one Huffman decode table.
 *   root     : root table, 1 << root_bits entries
 *   arena    : subtable storage (NULL if no long codes possible)
 *   kind 0   : litlen; 1: dist; 2: code-length tree
 * Incompleteness rules mirror zlib: the CL tree must be complete; litlen
 * and dist trees may be incomplete only with exactly one code of len 1.
 */
static int build_table(uint32_t *root, uint32_t *arena, unsigned root_bits,
                       const uint8_t *lens, unsigned n, int kind)
{
    unsigned count[16], next_code[16];
    unsigned i, len, left, total = 0, max_len = 0;
    const unsigned root_size = 1u << root_bits;
    unsigned arena_used = 0;
    unsigned arena_cap = (root_bits == LITLEN_ROOT) ? LITLEN_ARENA
                        : (root_bits == DIST_ROOT) ? DIST_ARENA : 0;
    static unsigned sub_bits_at[1u << LITLEN_ROOT];
    static unsigned sub_off_at[1u << LITLEN_ROOT];

    memset(count, 0, sizeof(count));
    for (i = 0; i < n; i++)
        count[lens[i]]++;
    count[0] = 0;

    for (len = 1; len <= 15; len++) {
        total += count[len];
        if (count[len]) max_len = len;
    }

    if (total == 0) {
        if (kind != 1)
            return PTPNG_E_BAD_HUFFMAN;
        fill_bad(root, root_size);
        return PTPNG_OK; /* dist tree with no codes; error only if used */
    }

    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        if (count[len] > left)
            return PTPNG_E_BAD_HUFFMAN; /* oversubscribed */
        left -= count[len];
    }
    if (left != 0) {
        if (kind == 2)
            return PTPNG_E_BAD_HUFFMAN; /* CL tree must be complete */
        if (!(total == 1 && max_len == 1))
            return PTPNG_E_BAD_HUFFMAN;
        fill_bad(root, root_size);
    }

    {
        unsigned code = 0;
        for (len = 1; len <= 15; len++) {
            next_code[len] = code;
            code = (code + count[len]) << 1;
        }
    }
    for (i = 0; i < n; i++)
        sym_code[i] = lens[i] ? next_code[lens[i]]++ : 0;

    if (max_len > root_bits) {
        unsigned p;
        memset(sub_bits_at, 0, sizeof(unsigned) << root_bits);
        for (i = 0; i < n; i++) {
            unsigned l = lens[i];
            if (l > root_bits) {
                unsigned pfx = rev_bits(sym_code[i], l) & (root_size - 1);
                if (l > sub_bits_at[pfx] + root_bits)
                    sub_bits_at[pfx] = l - root_bits;
            }
        }
        for (p = 0; p < root_size; p++) {
            if (sub_bits_at[p]) {
                sub_off_at[p] = arena_used;
                arena_used += 1u << sub_bits_at[p];
                if (arena_used > arena_cap)
                    return PTPNG_E_BAD_HUFFMAN;
                fill_bad(arena + sub_off_at[p], 1u << sub_bits_at[p]);
                root[p] = F_SUB | ((uint32_t)sub_off_at[p] << 16) |
                          ((uint32_t)sub_bits_at[p] << 8) | root_bits;
            }
        }
    }

    for (i = 0; i < n; i++) {
        unsigned l = lens[i];
        uint32_t rev, e;
        if (l == 0)
            continue;
        rev = rev_bits(sym_code[i], l);

        e = make_entry(kind, i, l <= root_bits ? l : l - root_bits);

        if (l <= root_bits) {
            unsigned k;
            for (k = rev; k < root_size; k += 1u << l)
                root[k] = e;
        } else {
            unsigned pfx = rev & (root_size - 1);
            unsigned sb = sub_bits_at[pfx];
            unsigned off = sub_off_at[pfx];
            unsigned j, stride = 1u << (l - root_bits);
            for (j = (rev >> root_bits) & (stride - 1); j < (1u << sb);
                 j += stride)
                arena[off + j] = e;
        }
    }
    return PTPNG_OK;
}

/* fixed Huffman tables, built once */
static struct {
    uint32_t lit_root[LITLEN_SIZE];
    uint32_t dist_root[DIST_SIZE];
    int ready;
} fixed_tabs;

static int build_fixed(void)
{
    uint8_t lens[288];
    unsigned i;
    int rc;
    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    rc = build_table(fixed_tabs.lit_root, NULL, LITLEN_ROOT,
                     lens, 288, 0);
    if (rc) return rc;
    for (i = 0; i < 32; i++) lens[i] = 5;
    rc = build_table(fixed_tabs.dist_root, NULL, DIST_ROOT,
                     lens, 32, 1);
    if (rc) return rc;
    fixed_tabs.ready = 1;
    return PTPNG_OK;
}

/* --------------------------------------------------------------------- */

static int inflate_impl(const uint8_t *in, size_t in_len, uint8_t *out,
                        size_t out_len, uint32_t flags, int exact_size,
                        size_t *produced, struct ptpng_inflate_sink *sink)
{
    const uint8_t *in_end = in + in_len;
    uint64_t bitbuf = 0;
    int bitcnt = 0;
    size_t pos = 0;
    /* single-stream by contract (PNG decodes one zlib stream at a time):
     * decode tables live in .bss instead of a ~30KB stack frame */
    static uint32_t dynamic_lit[LITLEN_SIZE], lit_arena[LITLEN_ARENA];
    static uint32_t dynamic_dist[DIST_SIZE], dist_arena[DIST_ARENA];
    const uint32_t *lit_tbl = dynamic_lit, *dist_tbl = dynamic_dist;
    static uint32_t cl_tbl[CL_SIZE];
    /* output position of the next sink notification */
    size_t next_sink = sink ? (size_t)SINK_WINDOW + SINK_STEP : SIZE_MAX;

    *produced = 0;
    if (in_len < 6)
        return PTPNG_E_BAD_ZLIB_HEADER; /* 2 header + 4 adler minimum */
    {
        unsigned cmf = in[0], flg = in[1];
        if ((cmf & 0x0F) != 8)
            return PTPNG_E_BAD_ZLIB_HEADER;
        if ((cmf >> 4) > 7)
            return PTPNG_E_BAD_ZLIB_HEADER; /* PNG: window <= 32K */
        if (((cmf << 8) | flg) % 31 != 0)
            return PTPNG_E_BAD_ZLIB_HEADER;
        if (flg & 0x20)
            return PTPNG_E_BAD_ZLIB_HEADER; /* FDICT not allowed in PNG */
        in += 2;
    }

#define REFILL()                                                        \
    do {                                                                \
        if (bitcnt >= 0 && bitcnt < 48) {                               \
            if ((size_t)(in_end - in) >= 8) {                            \
                bitbuf |= LOAD64LE(in) << bitcnt;                       \
                in += (63 - bitcnt) >> 3;                               \
                bitcnt |= 56;                                           \
            } else {                                                    \
                while (bitcnt <= 56 && in < in_end) {                   \
                    bitbuf |= (uint64_t)*in++ << bitcnt;                \
                    bitcnt += 8;                                        \
                }                                                       \
            }                                                           \
        }                                                               \
    } while (0)

/* Requires 0 <= bitcnt <= 63 and eight readable input bytes. Bits above
 * bitcnt are zero or already hold the same stream bits, so the OR is
 * idempotent; afterwards 56 <= bitcnt <= 63. */
#define REFILL_FAST()                                                   \
    do {                                                                \
        bitbuf |= LOAD64LE(in) << bitcnt;                               \
        in += (63 - bitcnt) >> 3;                                       \
        bitcnt |= 56;                                                   \
    } while (0)

#define PEEK32() ((uint32_t)bitbuf)
#define DROP(n) do { bitbuf >>= (n); bitcnt -= (n); } while (0)
#define DROP_E(e)                                                       \
    do { bitbuf >>= (uint8_t)(e); bitcnt -= (int)((e) & 0xFF); } while (0)
#define EXTRA_BITS(saved, e)                                            \
    (((uint32_t)(saved) & ((1u << ((e) & 0xFF)) - 1)) >> (((e) >> 8) & 0xF))
#define COPY16(d, s) memcpy((d), (s), 16)

    for (;;) { /* per deflate block */
        unsigned bfinal, btype;

        SINK_NOTIFY();
        REFILL();
        if (bitcnt < 3)
            return PTPNG_E_TRUNCATED;
        bfinal = PEEK32() & 1; DROP(1);
        btype = PEEK32() & 3; DROP(2);

        if (btype == 0) {
            /* stored block: byte-align, push back whole bytes */
            unsigned drop = bitcnt & 7;
            unsigned slen, nlen;
            DROP(drop);
            in -= bitcnt >> 3;
            bitbuf = 0; bitcnt = 0;
            if ((size_t)(in_end - in) < 4)
                return PTPNG_E_TRUNCATED;
            slen = (unsigned)in[0] | ((unsigned)in[1] << 8);
            nlen = (unsigned)in[2] | ((unsigned)in[3] << 8);
            if (slen != (~nlen & 0xFFFF))
                return PTPNG_E_INFLATE_CORRUPT;
            in += 4;
            if ((size_t)(in_end - in) < slen)
                return PTPNG_E_TRUNCATED;
            if (pos + slen > out_len)
                return PTPNG_E_INFLATE_SIZE;
            memcpy(out + pos, in, slen);
            pos += slen;
            in += slen;
            if (bfinal)
                break;
            continue;
        }

        if (btype == 1) {
            if (!fixed_tabs.ready) {
                int rc = build_fixed();
                if (rc) return rc;
            }
            /* Fixed codes fit entirely in the roots. Reuse the cached
             * tables without copying 29 KiB for each fixed block. */
            lit_tbl = fixed_tabs.lit_root;
            dist_tbl = fixed_tabs.dist_root;
        } else if (btype == 2) {
            /* dynamic Huffman block */
            unsigned hlit, hdist, hclen, i, v2;
            uint8_t lens[288 + 32];
            uint8_t cl_lens[19];
            unsigned prev = 0xFF;
            unsigned total, li;
            int rc;

            REFILL();
            if (bitcnt < 14 && in >= in_end)
                return PTPNG_E_TRUNCATED;
            v2 = PEEK32() & 31; DROP(5); hlit = v2 + 257;
            v2 = PEEK32() & 31; DROP(5); hdist = v2 + 1;
            v2 = PEEK32() & 15; DROP(4); hclen = v2 + 4;
            if (hlit > 286 || hdist > 30)
                return PTPNG_E_INFLATE_CORRUPT;

            memset(cl_lens, 0, 19);
            for (i = 0; i < hclen; i++) {
                REFILL();
                if (bitcnt < 3 && in >= in_end)
                    return PTPNG_E_TRUNCATED;
                cl_lens[cl_perm[i]] = (uint8_t)(PEEK32() & 7);
                DROP(3);
            }
            rc = build_table(cl_tbl, NULL, CL_ROOT, cl_lens, 19, 2);
            if (rc) return rc;

            total = hlit + hdist;
            memset(lens, 0, sizeof(lens));
            li = 0;
            while (li < total) {
                uint32_t e, nb;
                REFILL();
                if (bitcnt <= 0)
                    return PTPNG_E_TRUNCATED;
                e = cl_tbl[PEEK32() & (CL_SIZE - 1)];
                nb = e & 0xFF;
                if (nb == 0 || (e & F_EXC))
                    return PTPNG_E_INFLATE_CORRUPT;
                DROP(nb);
                {
                    unsigned sym = (e >> 16) & 0xFF;
                    if (sym < 16) {
                        lens[li++] = (uint8_t)sym;
                        prev = sym;
                    } else {
                        unsigned rep;
                        switch (sym) {
                        case 16:
                            if (prev == 0xFF)
                                return PTPNG_E_INFLATE_CORRUPT;
                            rep = 3 + (PEEK32() & 3); DROP(2);
                            break;
                        case 17:
                            rep = 3 + (PEEK32() & 7); DROP(3);
                            prev = 0;
                            break;
                        default: /* 18 */
                            rep = 11 + (PEEK32() & 127); DROP(7);
                            prev = 0;
                            break;
                        }
                        if (li + rep > total)
                            return PTPNG_E_INFLATE_CORRUPT;
                        while (rep--) lens[li++] = (uint8_t)prev;
                    }
                }
            }
            lit_tbl = dynamic_lit;
            dist_tbl = dynamic_dist;
            rc = build_table(dynamic_lit, lit_arena, LITLEN_ROOT, lens, hlit, 0);
            if (rc) return rc;
            rc = build_table(dynamic_dist, dist_arena, DIST_ROOT, lens + hlit,
                             hdist, 1);
            if (rc) return rc;
        } else {
            return PTPNG_E_INFLATE_CORRUPT; /* reserved btype */
        }

        /* hot symbol decode loop */
        for (;;) {
            uint32_t e;
            uint64_t saved;
            unsigned len, dist;

            SINK_NOTIFY();

            /* Fast loop: input and output slack remove per-symbol bounds
             * checks. Each iteration refills once to >= 56 bits, enough for
             * a length code with extra bits plus a distance code with extra
             * bits (20 + 28), or for four root-table literals (40). Matches
             * may store up to 15 bytes beyond their end; the output margin
             * keeps those stores inside out, and later symbols overwrite
             * them. */
            if ((size_t)(in_end - in) >= FAST_IN_MARGIN &&
                out_len - pos >= FAST_OUT_MARGIN && bitcnt >= 0) {
                uint8_t *op = out + pos;
                uint8_t *const op_limit = out +
                    (out_len - FAST_OUT_MARGIN < next_sink
                         ? out_len - FAST_OUT_MARGIN : next_sink);
                const uint8_t *const in_limit = in_end - FAST_IN_MARGIN;
                do {
                    REFILL_FAST();
                    e = lit_tbl[bitbuf & (LITLEN_SIZE - 1)];
                    if ((int32_t)e < 0) {
                        DROP_E(e);
                        *op++ = (uint8_t)(e >> 16);
                        e = lit_tbl[bitbuf & (LITLEN_SIZE - 1)];
                        if ((int32_t)e < 0) {
                            DROP_E(e);
                            *op++ = (uint8_t)(e >> 16);
                            e = lit_tbl[bitbuf & (LITLEN_SIZE - 1)];
                            if ((int32_t)e < 0) {
                                DROP_E(e);
                                *op++ = (uint8_t)(e >> 16);
                                e = lit_tbl[bitbuf & (LITLEN_SIZE - 1)];
                                if ((int32_t)e < 0) {
                                    DROP_E(e);
                                    *op++ = (uint8_t)(e >> 16);
                                    continue;
                                }
                            }
                        }
                        /* The peeked entry stays valid: refilling only adds
                         * bits above the ones it was indexed with. */
                        REFILL_FAST();
                    }
                    if (e & F_EXC) {
                        if (e & F_SUB) {
                            DROP_E(e);
                            e = lit_arena[(e >> 16) + ((uint32_t)bitbuf &
                                          ((1u << ((e >> 8) & 0xF)) - 1))];
                            if ((int32_t)e < 0) {
                                DROP_E(e);
                                *op++ = (uint8_t)(e >> 16);
                                continue;
                            }
                        }
                        if (e & F_EXC) {
                            if (e & F_EOB) {
                                DROP_E(e);
                                pos = (size_t)(op - out);
                                goto block_done;
                            }
                            return PTPNG_E_INFLATE_CORRUPT;
                        }
                    }
                    saved = bitbuf;
                    DROP_E(e);
                    len = (e >> 16) + EXTRA_BITS(saved, e);
                    e = dist_tbl[bitbuf & (DIST_SIZE - 1)];
                    if (e & F_EXC) {
                        if (!(e & F_SUB))
                            return PTPNG_E_INFLATE_CORRUPT;
                        DROP_E(e);
                        e = dist_arena[(e >> 16) + ((uint32_t)bitbuf &
                                       ((1u << ((e >> 8) & 0xF)) - 1))];
                        if (e & F_EXC)
                            return PTPNG_E_INFLATE_CORRUPT;
                    }
                    saved = bitbuf;
                    DROP_E(e);
                    dist = (e >> 16) + EXTRA_BITS(saved, e);
                    if (dist > (size_t)(op - out))
                        return PTPNG_E_INFLATE_CORRUPT;
                    {
                        uint8_t *dst = op;
                        const uint8_t *src = op - dist;
                        uint8_t *const end = op + len;
                        if (dist >= 16) {
                            /* Each 16-byte source chunk ends at or before
                             * its destination, so it is already decoded. */
                            COPY16(dst, src);
                            if (len > 16) {
                                COPY16(dst + 16, src + 16);
                                while (dst + 32 < end) {
                                    dst += 32; src += 32;
                                    COPY16(dst, src);
                                    COPY16(dst + 16, src + 16);
                                }
                            }
                        } else if (dist >= 8) {
                            do {
                                STORE64LE(dst, LOAD64LE(src));
                                dst += 8; src += 8;
                            } while (dst < end);
                        } else if (dist == 1) {
                            if (len > 32) {
                                /* long runs: the C library's fill is widest */
                                memset(dst, src[0], len);
                            } else {
                                uint64_t v = (uint64_t)src[0] *
                                             UINT64_C(0x0101010101010101);
                                STORE64LE(dst, v);
                                STORE64LE(dst + 8, v);
                                STORE64LE(dst + 16, v);
                                STORE64LE(dst + 24, v);
                            }
                        } else {
                            /* The first dist loaded bytes are decoded; each
                             * step therefore extends the pattern by dist. */
                            do {
                                STORE64LE(dst, LOAD64LE(src));
                                dst += dist; src += dist;
                            } while (dst < end);
                        }
                    }
                    op += len;
                } while (op < op_limit && in < in_limit);
                pos = (size_t)(op - out);
            }

            /* Careful path: one symbol at a time with full checks, used near
             * the ends of the input and output. */
            if (bitcnt < 0)
                return PTPNG_E_TRUNCATED;
            REFILL();
            e = lit_tbl[PEEK32() & (LITLEN_SIZE - 1)];
            if (e & F_SUB) {
                DROP_E(e);
                e = lit_arena[(e >> 16) +
                              (PEEK32() & ((1u << ((e >> 8) & 0xF)) - 1))];
            }
            if ((int32_t)e < 0) {
                DROP_E(e);
                if (pos >= out_len)
                    return PTPNG_E_INFLATE_SIZE;
                out[pos++] = (uint8_t)(e >> 16);
                continue;
            }
            if (e & F_EXC) {
                if (e & F_EOB) {
                    DROP_E(e);
                    break;
                }
                return PTPNG_E_INFLATE_CORRUPT;
            }
            saved = bitbuf;
            DROP_E(e);
            len = (e >> 16) + EXTRA_BITS(saved, e);
            /* No refill needed: the 48-bit refill guarantee leaves at least
             * 28 bits after a 15-bit length code and 5 extra bits, exactly
             * a 15-bit distance code plus 13 extra bits. */
            e = dist_tbl[PEEK32() & (DIST_SIZE - 1)];
            if (e & F_SUB) {
                DROP_E(e);
                e = dist_arena[(e >> 16) +
                               (PEEK32() & ((1u << ((e >> 8) & 0xF)) - 1))];
            }
            if (e & F_EXC)
                return PTPNG_E_INFLATE_CORRUPT;
            saved = bitbuf;
            DROP_E(e);
            dist = (e >> 16) + EXTRA_BITS(saved, e);
            if (dist > pos)
                return PTPNG_E_INFLATE_CORRUPT;
            if (pos + len > out_len)
                return PTPNG_E_INFLATE_SIZE;
            {
                uint8_t *dst = out + pos;
                const uint8_t *src = dst - dist;
                unsigned k;
                for (k = 0; k < len; k++)
                    dst[k] = src[k];
            }
            pos += len;
        } /* symbol loop */
block_done:

        if (bfinal)
            break;
    } /* block loop */

#undef REFILL
#undef PEEK32
#undef DROP
#undef DROP_E
#undef EXTRA_BITS
#undef COPY16
#undef REFILL_FAST
#undef SINK_NOTIFY

    /* byte-align and locate the adler32: pushing back floor(bitcnt/8)
     * whole bytes lands exactly on the byte after the final code's
     * padding bits */
    if (bitcnt < 0)
        return PTPNG_E_TRUNCATED;
    in -= bitcnt >> 3;
    bitbuf = 0; bitcnt = 0;

    if ((size_t)(in_end - in) < 4)
        return PTPNG_E_TRUNCATED;
    if (exact_size && pos != out_len)
        return PTPNG_E_INFLATE_SIZE;
    if (sink)
        sink->progress(sink, pos, 1);

    if (!(flags & PTPNG_INF_NO_ADLER)) {
        uint32_t want = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                        ((uint32_t)in[2] << 8) | (uint32_t)in[3];
        uint32_t got = sink ? sink->adler : ptpng_adler32(out, pos);
        if (want != got)
            return PTPNG_E_BAD_ADLER;
    }
    in += 4;

    if ((size_t)(in_end - in) > 0)
        return PTPNG_E_INFLATE_CORRUPT; /* trailing garbage */

    *produced = pos;
    return PTPNG_OK;
}

#if INFLATE_FIXED_STREAM
#define INFLATE_PUBLIC ptpng_inflate_fallback
#else
#define INFLATE_PUBLIC ptpng_inflate
#endif
int INFLATE_PUBLIC(const uint8_t *in, size_t in_len, uint8_t *out,
                  size_t out_len, uint32_t flags)
{
    size_t produced;
    if (!in || !out || out_len == 0)
        return PTPNG_E_BAD_ARG;
    return inflate_impl(in, in_len, out, out_len, flags, 1, &produced, NULL);
}
#undef INFLATE_PUBLIC

int ptpng_inflate_sink(const uint8_t *in, size_t in_len, uint8_t *out,
                       size_t out_len, uint32_t flags,
                       struct ptpng_inflate_sink *sink)
{
    size_t produced;
    if (!in || !out || out_len == 0 || !sink)
        return PTPNG_E_BAD_ARG;
    return inflate_impl(in, in_len, out, out_len, flags, 1, &produced, sink);
}

#if !INFLATE_FIXED_STREAM
int ptpng_inflate_prefers_fixed(const uint8_t *in, size_t in_len,
                                size_t out_len)
{
    (void)in; (void)in_len; (void)out_len;
    return 0;
}
#endif

int ptpng_inflate_dyn(const uint8_t *in, size_t in_len, size_t max_out,
                      uint8_t **out, size_t *out_len)
{
    size_t cap = 4096, produced = 0;
    uint8_t *buf;
    int rc;
    if (!in || !out || !out_len || in_len == 0 || max_out == SIZE_MAX)
        return PTPNG_E_BAD_ARG;
    *out = NULL;
    *out_len = 0;
    if (cap > max_out) cap = max_out;
    if (cap == 0) return PTPNG_E_BAD_ARG;
    for (;;) {
        buf = (uint8_t *)malloc(cap + 1);
        if (!buf) return PTPNG_E_OUT_OF_MEMORY;
        rc = inflate_impl(in, in_len, buf, cap, 0, 0,
                          &produced, NULL);
        if (rc == PTPNG_OK)
            break;
        free(buf);
        if (rc != PTPNG_E_INFLATE_SIZE || cap >= max_out)
            return rc;
        cap = cap > max_out / 2 ? max_out : cap * 2;
    }
    if (produced != cap) {
        uint8_t *nb = (uint8_t *)realloc(buf, produced + 1);
        if (nb) buf = nb;
    }
    buf[produced] = 0; /* compressed PNG text is exposed as a C string */
    *out = buf;
    *out_len = produced;
    return PTPNG_OK;
}

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

int ptpng_try_inflate_stored(const struct ptpng_inflate_chunk *chunks,
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
