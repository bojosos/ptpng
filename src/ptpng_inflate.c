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
 *  - Four-literal fast path: up to four literals decoded per refill with one
 *    bounds check; matches/EOB/subtables take the generic path.
 *  - Match copy: 8x64-bit move blocks when dist >= 64, 4x64-bit when
 *    dist >= 32 (32-byte blocks are overlap-safe exactly then), and
 *    periodic pattern replication through a 64-byte scratch for smaller
 *    distances.
 *  - The PNG caller knows the exact uncompressed size; we decode
 *    directly into the final buffer and verify the adler32.
 */
#include "ptpng_internal.h"

#define LITLEN_ROOT   10
#define LITLEN_SIZE   (1u << LITLEN_ROOT)
#define LITLEN_ARENA  2048
#if defined(__linux__) && defined(__aarch64__) && defined(__GNUC__) && \
    !defined(__clang__)
#define INFLATE_BATCH_BIT_GUARD 1
#else
#define INFLATE_BATCH_BIT_GUARD 0
#endif
#define DIST_ROOT     8
#define DIST_SIZE     (1u << DIST_ROOT)
#define DIST_ARENA    4096
#define CL_ROOT       7
#define CL_SIZE       (1u << CL_ROOT)

#define F_LIT       0x80000000u   /* literal: byte in bits 8..15        */
#define F_EOB       0x40000000u   /* end of block symbol 256            */
#define F_SUB       0x20000000u   /* subtable ref: off<<8, bits in 24..27 */

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
        memset(root, 0, root_size * sizeof(uint32_t));
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
        memset(root, 0, root_size * sizeof(uint32_t));
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
                memset(arena + sub_off_at[p], 0,
                       (size_t)(1u << sub_bits_at[p]) * sizeof(uint32_t));
                root[p] = F_SUB | ((uint32_t)sub_off_at[p] << 8) |
                          ((uint32_t)sub_bits_at[p] << 24);
            }
        }
    }

    for (i = 0; i < n; i++) {
        unsigned l = lens[i];
        uint32_t rev, e;
        if (l == 0)
            continue;
        rev = rev_bits(sym_code[i], l);

        if (kind == 1) {
            if (i < 30)
                e = ((uint32_t)dist_base_tab[i] << 8) |
                    ((uint32_t)dist_extra_tab[i] << 24) | l;
            else
                e = F_LIT; /* symbols 30/31: invalid (len nibble 0) */
        } else if (kind == 2) {
            e = F_LIT | ((uint32_t)i << 8) | l;
        } else if (i < 256) {
            e = F_LIT | ((uint32_t)i << 8) | l;
        } else if (i == 256) {
            e = F_EOB | l;
        } else if (i < 286) {
            unsigned s = i - 257;
            e = ((uint32_t)len_base_tab[s] << 8) |
                ((uint32_t)len_extra_tab[s] << 17) | l;
        } else {
            e = F_LIT; /* symbols 286/287: invalid (len nibble 0) */
        }

        if (l <= root_bits) {
            unsigned k;
            for (k = rev; k < root_size; k += 1u << l)
                root[k] = e;
        } else {
            unsigned pfx = rev & (root_size - 1);
            unsigned sb = sub_bits_at[pfx];
            unsigned off = sub_off_at[pfx];
            unsigned j, stride = 1u << (l - root_bits);
            e = (e & ~0xFu) | (l - root_bits); /* remaining bit count */
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

#if defined(__linux__) && defined(__x86_64__) && defined(__GNUC__) && \
    !defined(__clang__)
/* Keep the hot literal loop's cache-line phase stable as callers change. */
__attribute__((aligned(64)))
#endif
static int inflate_impl(const uint8_t *in, size_t in_len, uint8_t *out,
                        size_t out_len, uint32_t flags, int exact_size,
                        size_t *produced)
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
                in += (64 - bitcnt) >> 3;                               \
                bitcnt += (64 - bitcnt) & ~7;                           \
            } else {                                                    \
                while (bitcnt <= 56 && in < in_end) {                   \
                    bitbuf |= (uint64_t)*in++ << bitcnt;                \
                    bitcnt += 8;                                        \
                }                                                       \
            }                                                           \
        }                                                               \
    } while (0)

#define PEEK32() ((uint32_t)bitbuf)
#define DROP(n) do { bitbuf >>= (n); bitcnt -= (n); } while (0)

    for (;;) { /* per deflate block */
        unsigned bfinal, btype, v;

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
                nb = e & 0xF;
                if (nb == 0)
                    return PTPNG_E_INFLATE_CORRUPT;
                DROP(nb);
                {
                    unsigned sym = (e >> 8) & 0xFF;
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
            uint32_t e, nb;

            if (bitcnt < 0)
                return PTPNG_E_TRUNCATED;

            REFILL();
#if INFLATE_BATCH_BIT_GUARD
            if (pos + 8 <= out_len && bitcnt >= 4 * LITLEN_ROOT) {
#else
            if (pos + 8 <= out_len) {
#endif
                /* Up to four root-table literals share one refill. */
                /* On selected compilers, the batch guard proves all four
                 * root literals fit. Short input tails use slow_decode. */
                /* build_table gives valid literals a nonzero bit count, so
                 * their entries exceed F_LIT. Reserved symbols are exactly
                 * F_LIT; matches, EOB and subtable entries are below it. */
                e = lit_tbl[PEEK32() & (LITLEN_SIZE - 1)];
                nb = e & 0xF;
                if (e > F_LIT) {
                    out[pos] = (uint8_t)(e >> 8);
                    bitbuf >>= nb; bitcnt -= nb;
                    e = lit_tbl[PEEK32() & (LITLEN_SIZE - 1)];
                    nb = e & 0xF;
#if INFLATE_BATCH_BIT_GUARD
                    if (e > F_LIT) {
#else
                    if (e > F_LIT &&
                        (unsigned)bitcnt >= nb) {
#endif
                        out[pos + 1] = (uint8_t)(e >> 8);
                        bitbuf >>= nb; bitcnt -= nb;
                        pos += 2;
                        e = lit_tbl[PEEK32() & (LITLEN_SIZE - 1)];
                        nb = e & 0xF;
#if INFLATE_BATCH_BIT_GUARD
                        if (e > F_LIT) {
#else
                        if (e > F_LIT &&
                            bitcnt >= (int)nb) {
#endif
                            out[pos++] = (uint8_t)(e >> 8);
                            DROP(nb);
                            e = lit_tbl[PEEK32() & (LITLEN_SIZE - 1)];
                            nb = e & 0xF;
#if INFLATE_BATCH_BIT_GUARD
                            if (e > F_LIT) {
#else
                            if (e > F_LIT &&
                                bitcnt >= (int)nb) {
#endif
                                out[pos++] = (uint8_t)(e >> 8);
                                DROP(nb);
                                continue;
                            }
                        }
                        /* A following match can need more bits than remain.
                         * Refill before consuming any nonliteral entry. */
                        goto slow_decode;
                    }
                    /* second symbol is not a simple literal; the first
                     * literal is stored and consumed already.  A simple
                     * EOB can be consumed directly (its bits are valid);
                     * anything else re-decodes after a refill. */
                    pos += 1;
                    if (e & F_EOB) {
                        DROP(nb);
                        break;
                    }
                    goto slow_decode;
                }
                /* first symbol is not a simple literal */
                if (e & F_SUB) {
                    DROP(LITLEN_ROOT);
                    e = lit_arena[((e >> 8) & 0xFFFF) +
                                  (PEEK32() & ((1u << ((e >> 24) & 0xF)) - 1))];
                }
                goto have_entry;
            }

slow_decode:
            /* generic single-symbol path (near end of output, or the
             * continuation after a fast-path literal); the peek is
             * idempotent, so re-decoding after the refill is safe */
            REFILL();
            e = lit_tbl[PEEK32() & (LITLEN_SIZE - 1)];
            if (e & F_SUB) {
                DROP(LITLEN_ROOT);
                e = lit_arena[((e >> 8) & 0xFFFF) +
                              (PEEK32() & ((1u << ((e >> 24) & 0xF)) - 1))];
            }
have_entry:
            nb = e & 0xF;
            if (nb == 0)
                return PTPNG_E_INFLATE_CORRUPT;
            DROP(nb);

            if (e & F_LIT) {
                if (pos >= out_len)
                    return PTPNG_E_INFLATE_SIZE;
                out[pos++] = (uint8_t)(e >> 8);
                continue;
            }
            if (e & F_EOB)
                break;

            {
                unsigned len = (e >> 8) & 0x1FF;
                unsigned lextra = (e >> 17) & 0x7;
                unsigned dist, dextra;
                uint8_t *dst;
                size_t rem;

                if (lextra) {
                    v = PEEK32() & ((1u << lextra) - 1); DROP(lextra);
                    len += v;
                }

                /* no refill needed here: after the length code the buffer
                 * still holds >= 28 bits (48-bit refill guarantee minus
                 * 15-bit len code and 5 extra), exactly the worst case
                 * of a 15-bit dist code plus 13 extra bits */
                e = dist_tbl[PEEK32() & (DIST_SIZE - 1)];
                if (e & F_SUB) {
                    DROP(DIST_ROOT);
                    e = dist_arena[((e >> 8) & 0xFFFF) +
                                   (PEEK32() & ((1u << ((e >> 24) & 0xF)) - 1))];
                    nb = e & 0xF;
                    if (nb == 0)
                        return PTPNG_E_INFLATE_CORRUPT;
                    DROP(nb);
                } else {
                    nb = e & 0xF;
                    if (nb == 0)
                        return PTPNG_E_INFLATE_CORRUPT;
                    DROP(nb);
                }
                dist = (e >> 8) & 0x7FFF;
                dextra = (e >> 24) & 0xF;
                if (dextra) {
                    v = PEEK32() & ((1u << dextra) - 1); DROP(dextra);
                    dist += v;
                }

                if (dist > pos)
                    return PTPNG_E_INFLATE_CORRUPT;
                if (pos + len > out_len)
                    return PTPNG_E_INFLATE_SIZE;

                dst = out + pos;
                rem = len;
                if (len <= 16 && dist >= 16 && out_len - pos >= 16) {
                    /* Common photo matches fit in one vector-sized copy.
                     * The source is decoded history, the ranges do not
                     * overlap, and any excess store stays inside out. */
                    memcpy(dst, dst - dist, 16);
                } else if (dist == 1) {
                    /* Runs are common in filtered PNG data. Avoid building
                     * a periodic scratch buffer for a single repeated byte. */
                    memset(dst, dst[-1], rem);
                } else if (dist >= 32) {
                    const uint8_t *src = dst - dist;
                    if (dist >= 64) {
                        while (rem >= 64) {
                            uint64_t a, b, c, d, f, g, h2, i2;
                            a = LOAD64LE(src);      b = LOAD64LE(src + 8);
                            c = LOAD64LE(src + 16); d = LOAD64LE(src + 24);
                            f = LOAD64LE(src + 32); g = LOAD64LE(src + 40);
                            h2 = LOAD64LE(src + 48); i2 = LOAD64LE(src + 56);
                            STORE64LE(dst, a);      STORE64LE(dst + 8, b);
                            STORE64LE(dst + 16, c); STORE64LE(dst + 24, d);
                            STORE64LE(dst + 32, f); STORE64LE(dst + 40, g);
                            STORE64LE(dst + 48, h2);STORE64LE(dst + 56, i2);
                            dst += 64; src += 64; rem -= 64;
                        }
                    }
                    while (rem >= 32) {
                        uint64_t a, b, c, d;
                        a = LOAD64LE(src);      b = LOAD64LE(src + 8);
                        c = LOAD64LE(src + 16); d = LOAD64LE(src + 24);
                        STORE64LE(dst, a);      STORE64LE(dst + 8, b);
                        STORE64LE(dst + 16, c); STORE64LE(dst + 24, d);
                        dst += 32; src += 32; rem -= 32;
                    }
                    if (rem) {
                        if ((size_t)(dst - out) + 32 <= out_len) {
                            uint64_t a, b, c, d;
                            const uint8_t *s2 = dst - dist;
                            a = LOAD64LE(s2);      b = LOAD64LE(s2 + 8);
                            c = LOAD64LE(s2 + 16); d = LOAD64LE(s2 + 24);
                            STORE64LE(dst, a);      STORE64LE(dst + 8, b);
                            STORE64LE(dst + 16, c); STORE64LE(dst + 24, d);
                        } else {
                            const uint8_t *s2 = dst - dist;
                            while (rem--) *dst++ = *s2++;
                        }
                    }
                } else if (rem > 32) {
                    uint8_t tmp[64];
                    const uint8_t *src = dst - dist;
                    unsigned i2, r, m;
                    for (i2 = 0; i2 < dist; i2++) tmp[i2] = src[i2];
                    for (; i2 < 64; i2++) tmp[i2] = tmp[i2 - dist];
                    r = 0; m = 32 % dist;
                    while (rem >= 32 && (size_t)(dst - out) + 32 <= out_len) {
                        uint64_t a, b, c, d;
                        a = LOAD64LE(tmp + r);      b = LOAD64LE(tmp + r + 8);
                        c = LOAD64LE(tmp + r + 16); d = LOAD64LE(tmp + r + 24);
                        STORE64LE(dst, a);      STORE64LE(dst + 8, b);
                        STORE64LE(dst + 16, c); STORE64LE(dst + 24, d);
                        dst += 32; rem -= 32;
                        r += m;
                        if (r >= dist) r -= dist;
                    }
                    while (rem--) {
                        *dst++ = tmp[r];
                        if (++r == dist) r = 0;
                    }
                } else {
                    const uint8_t *src = dst - dist;
                    size_t rem2 = len;
                    while (rem2--) {
                        *dst = *src;
                        ++dst; ++src;
                    }
                }
                pos += len;
                continue;
            }
        } /* symbol loop */

        if (bfinal)
            break;
    } /* block loop */

#undef REFILL
#undef PEEK32
#undef DROP

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

    if (!(flags & PTPNG_INF_NO_ADLER)) {
        uint32_t want = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
                        ((uint32_t)in[2] << 8) | (uint32_t)in[3];
        uint32_t got = ptpng_adler32(out, pos);
        if (want != got)
            return PTPNG_E_BAD_ADLER;
    }
    in += 4;

    if ((size_t)(in_end - in) > 0)
        return PTPNG_E_INFLATE_CORRUPT; /* trailing garbage */

    *produced = pos;
    return PTPNG_OK;
}

int ptpng_inflate(const uint8_t *in, size_t in_len, uint8_t *out,
                  size_t out_len, uint32_t flags)
{
    size_t produced;
    if (!in || !out || out_len == 0)
        return PTPNG_E_BAD_ARG;
    return inflate_impl(in, in_len, out, out_len, flags, 1, &produced);
}

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
                          &produced);
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
#endif
