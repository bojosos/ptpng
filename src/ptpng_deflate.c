/* Dependency-free DEFLATE encoder with bounded lazy matching and dynamic,
 * fixed, or stored blocks selected by their bit cost. All state is per call. */
#include "ptpng_internal.h"

/* These 64-bit targets permit unaligned stores. Keep big-endian ARM and
 * architectures with unknown byte order on the byte-oriented writer. */
#if PTPNG_X64 || defined(_M_ARM64) || \
    (defined(__aarch64__) && defined(__BYTE_ORDER__) && \
     defined(__ORDER_LITTLE_ENDIAN__) && \
     __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define PTPNG_DEFLATE_WORD_STORE 1
#else
#define PTPNG_DEFLATE_WORD_STORE 0
#endif

static const uint16_t deflate_len_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,
    115,131,163,195,227,258
};
static const uint8_t deflate_len_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint8_t deflate_reverse4[16] = {
    0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15
};

typedef struct {
    uint8_t *dst;
    size_t pos, limit;
    uint64_t bits;
    unsigned count;
} deflate_writer;

PTPNG_API_INLINE unsigned deflate_reverse8(unsigned value)
{
    return ((unsigned)deflate_reverse4[value & 15] << 4) |
           deflate_reverse4[(value >> 4) & 15];
}

/* count is at most 31. The word-store writer keeps fewer than eight pending
 * bits; the portable writer keeps fewer than 32. */
PTPNG_API_INLINE int deflate_put(deflate_writer *w, uint32_t value,
                                 unsigned count)
{
    w->bits |= (uint64_t)value << w->count;
    w->count += count;
#if PTPNG_DEFLATE_WORD_STORE
    {
        unsigned bytes = w->count >> 3;
        /* An unaligned little-endian store commits all complete bytes at
         * once. Extra bytes stay inside the allocation and are overwritten
         * by later writes. Close to the limit, store only valid bytes. */
        if (w->limit - w->pos >= 8) {
            memcpy(w->dst + w->pos, &w->bits, sizeof(w->bits));
        } else {
            unsigned i;
            if (bytes > w->limit - w->pos) return 0;
            for (i = 0; i < bytes; ++i)
                w->dst[w->pos+i] = (uint8_t)(w->bits >> (8*i));
        }
        w->pos += bytes;
        w->bits >>= bytes*8;
        w->count &= 7;
    }
#else
    if (w->count >= 32) {
        uint32_t word = (uint32_t)w->bits;
        if (w->limit - w->pos < 4) return 0;
        w->dst[w->pos] = (uint8_t)word;
        w->dst[w->pos + 1] = (uint8_t)(word >> 8);
        w->dst[w->pos + 2] = (uint8_t)(word >> 16);
        w->dst[w->pos + 3] = (uint8_t)(word >> 24);
        w->pos += 4;
        w->bits >>= 32;
        w->count -= 32;
    }
#endif
    return 1;
}

PTPNG_API_INLINE unsigned deflate_log2(unsigned value)
{
#if defined(_MSC_VER)
    unsigned long bit;
    _BitScanReverse(&bit, value);
    return (unsigned)bit;
#elif defined(__GNUC__) || defined(__clang__)
    return 31u - (unsigned)__builtin_clz(value);
#else
    unsigned bit = 0;
    while (value >>= 1) ++bit;
    return bit;
#endif
}

/* Bounded per-call state. Token-limited blocks avoid large headers on
 * repetitive data while adapting the trees to changing image statistics. */
#define DEFLATE_HASH_BITS 16
#define DEFLATE_WINDOW 32768
#define DEFLATE_TOKENS 32768
#define DEFLATE_BLOCK_BYTES (1024u * 1024u)
#ifndef DEFLATE_CHAIN
#define DEFLATE_CHAIN 128
#endif

typedef struct { uint16_t value, distance; } deflate_token;
typedef struct {
    uint32_t head[1u << DEFLATE_HASH_BITS];
    uint32_t previous[DEFLATE_WINDOW];
    /* The long chain hashes six bytes. A separate, shallow three-byte
     * chain recovers profitable short matches in noisier image residuals. */
    uint32_t short_head[1u << DEFLATE_HASH_BITS];
    uint32_t short_previous[DEFLATE_WINDOW];
    deflate_token tokens[DEFLATE_TOKENS];
} deflate_state;
typedef struct { uint8_t symbol, extra, extra_bits; } deflate_run;
typedef struct {
    uint8_t lit_lengths[286], dist_lengths[30], cl_lengths[19];
    uint32_t lit_codes[286], dist_codes[30], cl_codes[19];
    deflate_run runs[316];
    unsigned lit_count, dist_count, cl_count, run_count;
    uint64_t bits;
} deflate_tree;

static const uint16_t deflate_dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t deflate_cl_order[19] = {
    16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
};

PTPNG_API_INLINE unsigned deflate_dist_symbol(unsigned distance)
{
    unsigned top;
    if (distance <= 4) return distance - 1;
    top = deflate_log2(distance - 1);
    return top * 2 + (((distance - 1) >> (top - 1)) & 1);
}

PTPNG_API_INLINE unsigned deflate_reverse(unsigned value, unsigned count)
{
    return ((deflate_reverse8(value) << 8) |
            deflate_reverse8(value >> 8)) >> (16 - count);
}

PTPNG_API_INLINE unsigned deflate_hash6(const uint8_t *p)
{
    uint32_t low;
    uint16_t high;
    uint64_t value;
    memcpy(&low, p, sizeof(low));
    memcpy(&high, p + 4, sizeof(high));
    value = low | ((uint64_t)high << 32);
    return (unsigned)((value * UINT64_C(0x9e3779b97f4a7c15)) >>
                      (64 - DEFLATE_HASH_BITS));
}

PTPNG_API_INLINE uint32_t deflate_insert(deflate_state *state,
                                         const uint8_t *src, size_t pos,
                                         size_t size)
{
    unsigned hash;
    uint32_t old;
#if PTPNG_DEFLATE_WORD_STORE
    if (size - pos >= 8) {
        uint64_t word;
        uint32_t short_value;
        unsigned short_hash;
        /* On known little-endian targets, one in-bounds load supplies both
         * hashes. Mask before multiplying: bytes six and seven must not
         * affect the existing six-byte hash or its candidate ordering. */
        memcpy(&word, src + pos, sizeof(word));
        short_value = (uint32_t)word & UINT32_C(0x00ffffff);
        short_hash = (short_value * UINT32_C(2654435761)) >>
                     (32 - DEFLATE_HASH_BITS);
        word &= UINT64_C(0x0000ffffffffffff);
        hash = (unsigned)((word * UINT64_C(0x9e3779b97f4a7c15)) >>
                          (64 - DEFLATE_HASH_BITS));
        state->short_previous[pos & (DEFLATE_WINDOW - 1)] = state->short_head[short_hash];
        state->short_head[short_hash] = (uint32_t)pos + 1;
        old = state->head[hash];
        state->previous[pos & (DEFLATE_WINDOW - 1)] = old;
        state->head[hash] = (uint32_t)pos + 1;
        return old;
    }
#endif
    if (size - pos >= 3) {
        uint32_t value = src[pos] | ((uint32_t)src[pos + 1] << 8) |
                         ((uint32_t)src[pos + 2] << 16);
        unsigned short_hash = (value * UINT32_C(2654435761)) >>
                              (32 - DEFLATE_HASH_BITS);
        state->short_previous[pos & (DEFLATE_WINDOW - 1)] = state->short_head[short_hash];
        state->short_head[short_hash] = (uint32_t)pos + 1;
    }
    if (size - pos < 6) return 0;
    hash = deflate_hash6(src + pos);
    old = state->head[hash];
    state->previous[pos & (DEFLATE_WINDOW - 1)] = old;
    /* Modular positions retain the full window even beyond 4 GiB. Zero is
     * the empty-link sentinel, sacrificing only one insertion per 4 GiB. */
    state->head[hash] = (uint32_t)pos + 1;
    return old;
}

static unsigned deflate_match(const deflate_state *state, const uint8_t *src,
                              size_t pos, size_t end, uint32_t link,
                              unsigned minimum,
                              unsigned *best_distance)
{
    unsigned limit = (unsigned)(end - pos > 258 ? 258 : end - pos);
    unsigned best = minimum < 5 ? 5 : minimum, previous_distance = 0;
    unsigned attempts = DEFLATE_CHAIN, distance_best = 0;
    uint32_t prefix_low;
    uint32_t best_tail = 0;
    uint16_t prefix_high;
#if PTPNG_DEFLATE_WORD_STORE
    uint64_t prefix_word = 0;
#endif
    *best_distance = 0;
    if (!link || limit <= best) return 0;
    memcpy(&prefix_low, src + pos, sizeof(prefix_low));
    memcpy(&prefix_high, src + pos + 4, sizeof(prefix_high));
    if (best > 5)
        memcpy(&best_tail, src + pos + best - 3, sizeof(best_tail));
#if PTPNG_DEFLATE_WORD_STORE
    if (limit >= 8) {
        memcpy(&prefix_word, src + pos, sizeof(prefix_word));
        prefix_word &= UINT64_C(0x0000ffffffffffff);
    }
#endif
    if (best >= 8) attempts >>= 2;
    while (link && attempts--) {
        unsigned distance = (uint32_t)pos - (link - 1);
        size_t ref;
        unsigned length;
        uint32_t next_link;
        uint32_t candidate_tail;
        int matches;
        if (!distance || distance > DEFLATE_WINDOW || distance > pos ||
            distance <= previous_distance) break;
        ref = pos - distance;
        next_link = state->previous[ref & (DEFLATE_WINDOW - 1)];
        /* The six-byte prefix already proves an improvement over five.
         * For longer best matches, reject candidates with four bytes ending
         * at best before scanning their prefix. best < limit bounds both. */
        if (best > 5) {
            memcpy(&candidate_tail, src + ref + best - 3, sizeof(candidate_tail));
            if (candidate_tail != best_tail) goto next_match;
        }
#if PTPNG_DEFLATE_WORD_STORE
        if (limit >= 8) {
            uint64_t candidate;
            /* Masking keeps the same six-byte prefix. Both eight-byte loads
             * are bounded by end, including references before pos. */
            memcpy(&candidate, src + ref, sizeof(candidate));
            matches = (candidate & UINT64_C(0x0000ffffffffffff)) == prefix_word;
        } else
#endif
        {
            uint32_t candidate_low;
            uint16_t candidate_high;
            memcpy(&candidate_low, src + ref, sizeof(candidate_low));
            memcpy(&candidate_high, src + ref + 4, sizeof(candidate_high));
            matches = candidate_low == prefix_low && candidate_high == prefix_high;
        }
        if (matches) {
            length = 6;
            while (limit - length >= 8) {
                uint64_t a, b;
                memcpy(&a, src + ref + length, sizeof(a));
                memcpy(&b, src + pos + length, sizeof(b));
                if (a != b) {
#if PTPNG_DEFLATE_WORD_STORE && defined(_MSC_VER)
                    unsigned long bit;
                    _BitScanForward64(&bit, a ^ b);
                    length += (unsigned)bit >> 3;
#elif PTPNG_DEFLATE_WORD_STORE && (defined(__GNUC__) || defined(__clang__))
                    length += (unsigned)__builtin_ctzll(a ^ b) >> 3;
#else
                    while (src[ref + length] == src[pos + length]) ++length;
#endif
                    /* The first differing byte is already known. */
                    goto match_done;
                }
                length += 8;
            }
            while (length < limit && src[ref + length] == src[pos + length])
                ++length;
match_done:
            if (length > best) {
                best = length;
                distance_best = distance;
                if (best == limit || best >= 128) break;
                memcpy(&best_tail, src + pos + best - 3, sizeof(best_tail));
            }
        }
next_match:
        previous_distance = distance;
        link = next_link;
    }
    *best_distance = distance_best;
    return distance_best ? best : 0;
}

/* Short matches compete with literal codes, rather than being accepted just
 * because they exist. The estimate includes 12 bits for length/distance
 * symbols, a two-bit margin, and the exact distance extra-bit count. */
static unsigned deflate_short_match(const deflate_state *state, const uint8_t *src,
                                    size_t pos, size_t end, unsigned minimum,
                                    const uint8_t literal_bits[256],
                                    unsigned *best_distance)
{
    unsigned limit = (unsigned)(end - pos > 5 ? 5 : end - pos);
    unsigned attempts = 4, previous_distance = 0, best = 0, best_saving = 0;
    unsigned distance_best = 0;
    uint32_t link;
#if PTPNG_DEFLATE_WORD_STORE
    uint32_t prefix = 0;
#endif
    *best_distance = 0;
    if (limit < 3 || limit <= minimum) return 0;
    link = state->short_previous[pos & (DEFLATE_WINDOW - 1)];
    if (!link) return 0;
#if PTPNG_DEFLATE_WORD_STORE
    if (limit >= 4) memcpy(&prefix, src + pos, sizeof(prefix));
    else prefix = src[pos] | ((uint32_t)src[pos + 1] << 8) |
                  ((uint32_t)src[pos + 2] << 16);
    prefix &= UINT32_C(0x00ffffff);
#endif
    while (link && attempts--) {
        unsigned distance = (uint32_t)pos - (link - 1);
        unsigned length, cost, literal_cost;
        uint32_t next_link;
        size_t ref;
        int matches;
        if (!distance || distance > DEFLATE_WINDOW || distance > pos ||
            distance <= previous_distance) break;
        ref = pos - distance;
        next_link = state->short_previous[ref & (DEFLATE_WINDOW - 1)];
#if PTPNG_DEFLATE_WORD_STORE
        {
            uint32_t candidate = 0;
            if (limit >= 4) memcpy(&candidate, src + ref, sizeof(candidate));
            else candidate = src[ref] | ((uint32_t)src[ref + 1] << 8) |
                             ((uint32_t)src[ref + 2] << 16);
            matches = (candidate & UINT32_C(0x00ffffff)) == prefix;
        }
#else
        matches = src[ref] == src[pos] && src[ref + 1] == src[pos + 1] &&
                  src[ref + 2] == src[pos + 2];
#endif
        if (matches) {
            length = 3;
            while (length < limit && src[ref + length] == src[pos + length]) ++length;
            if (length > minimum) {
                cost = 14 + (distance <= 4 ? 0 : deflate_log2(distance - 1) - 1);
                /* Every short match covers three bytes, and at most five. */
                literal_cost = literal_bits[src[pos]] + literal_bits[src[pos + 1]] +
                               literal_bits[src[pos + 2]];
                if (length > 3) literal_cost += literal_bits[src[pos + 3]];
                if (length > 4) literal_cost += literal_bits[src[pos + 4]];
                if (literal_cost > cost && literal_cost - cost > best_saving) {
                    best = length;
                    distance_best = distance;
                    best_saving = literal_cost - cost;
                }
            }
        }
        previous_distance = distance;
        link = next_link;
    }
    *best_distance = distance_best;
    return best;
}

static size_t deflate_parse(deflate_state *state, const uint8_t *src,
                            size_t size, size_t start, unsigned *token_count,
                            uint32_t lit_freq[286], uint32_t dist_freq[30],
                            uint64_t *extra_bits, const uint8_t len_symbol[259])
{
    size_t end = start + (size - start > DEFLATE_BLOCK_BYTES ?
                         DEFLATE_BLOCK_BYTES : size - start);
    size_t pos = start;
    unsigned count = 0, cached = 0, cached_length = 0, cached_distance = 0;
    uint32_t sample_freq[256] = {0};
    uint8_t literal_bits[256];
    unsigned sample_size = (unsigned)(end - start > 65536 ? 65536 : end - start);
    unsigned sample;
    /* Approximate literal code lengths from the next 64 KiB. This only
     * guides short-match selection; emitted trees use exact token counts. */
    for (sample = 0; sample < sample_size; ++sample) ++sample_freq[src[start + sample]];
    for (sample = 0; sample < 256; ++sample) {
        unsigned bits = 1;
        while (bits < 9 && sample_freq[sample] < (sample_size >> bits)) ++bits;
        literal_bits[sample] = (uint8_t)bits;
    }
    memset(lit_freq, 0, 286 * sizeof(*lit_freq));
    memset(dist_freq, 0, 30 * sizeof(*dist_freq));
    *extra_bits = 0;
    while (pos < end && count < DEFLATE_TOKENS) {
        unsigned length, distance, inserted_next = 0;
        if (cached) {
            length = cached_length;
            distance = cached_distance;
            cached = 0;
        } else {
            uint32_t link = deflate_insert(state, src, pos, size);
            length = deflate_match(state, src, pos, end, link, 5, &distance);
            if (!length)
                length = deflate_short_match(state, src, pos, end, 2, literal_bits, &distance);
        }
        /* A one-byte lookahead avoids committing to a short match directly
         * before a longer one. Every consumed position joins the chain. */
        if (length >= 3 && length < 64 && end - pos >= 4 &&
            count + 1 < DEFLATE_TOKENS) {
            unsigned next_distance, next_length;
            uint32_t link = deflate_insert(state, src, pos + 1, size);
            inserted_next = 1;
            next_length = deflate_match(state, src, pos + 1, end, link, length,
                                        &next_distance);
            if (!next_length)
                next_length = deflate_short_match(state, src, pos + 1, end, length,
                                                  literal_bits, &next_distance);
            if (next_length > length) {
                state->tokens[count].value = src[pos];
                state->tokens[count++].distance = 0;
                ++lit_freq[src[pos++]];
                cached = 1;
                cached_length = next_length;
                cached_distance = next_distance;
                continue;
            }
        }
        if (length >= 3) {
            unsigned ls = len_symbol[length], ds = deflate_dist_symbol(distance);
            size_t insert;
            state->tokens[count].value = (uint16_t)length;
            state->tokens[count++].distance = (uint16_t)distance;
            ++lit_freq[257 + ls];
            ++dist_freq[ds];
            *extra_bits += deflate_len_extra[ls] + (ds < 4 ? 0 : (ds >> 1) - 1);
            for (insert = pos + 1 + inserted_next; insert < pos + length; ++insert)
                deflate_insert(state, src, insert, size);
            pos += length;
        } else {
            state->tokens[count].value = src[pos];
            state->tokens[count++].distance = 0;
            ++lit_freq[src[pos++]];
        }
    }
    lit_freq[256] = 1;
    *token_count = count;
    return pos;
}

static int deflate_heap_less(unsigned a, unsigned b, const uint32_t *weights)
{
    return weights[a] < weights[b] || (weights[a] == weights[b] && a < b);
}

static void deflate_heap_push(uint16_t *heap, unsigned *count, unsigned node,
                              const uint32_t *weights)
{
    unsigned at = (*count)++;
    while (at) {
        unsigned parent = (at - 1) >> 1;
        if (!deflate_heap_less(node, heap[parent], weights)) break;
        heap[at] = heap[parent];
        at = parent;
    }
    heap[at] = (uint16_t)node;
}

static unsigned deflate_heap_pop(uint16_t *heap, unsigned *count,
                                 const uint32_t *weights)
{
    unsigned result = heap[0], last = heap[--*count], at = 0;
    while (at * 2 + 1 < *count) {
        unsigned child = at * 2 + 1;
        if (child + 1 < *count && deflate_heap_less(heap[child + 1],
                                                   heap[child], weights))
            ++child;
        if (!deflate_heap_less(heap[child], last, weights)) break;
        heap[at] = heap[child];
        at = child;
    }
    if (*count) heap[at] = (uint16_t)last;
    return result;
}

/* First construct a Huffman tree, then redistribute overlong leaves while
 * preserving their count and the Kraft sum. Assign the longest codes to the
 * least frequent symbols. Dummy leaves ensure complete, nonempty trees. */
static void deflate_huffman(const uint32_t *freq, unsigned alphabet,
                             unsigned max_bits, uint8_t *lengths,
                             uint32_t *codes)
{
    uint32_t weights[572] = {0};
    uint16_t parents[572], heap[286], order[286];
    unsigned counts[16] = {0}, next_code[16];
    unsigned used = 0, heap_count = 0, next = alphabet, i, bits, slots = 0;
    memset(parents, 255, sizeof(parents));
    memset(lengths, 0, alphabet * sizeof(*lengths));
    memset(codes, 0, alphabet * sizeof(*codes));
    for (i = 0; i < alphabet; ++i) {
        if (freq[i]) {
            weights[i] = freq[i];
            order[used++] = (uint16_t)i;
        }
    }
    for (i = 0; used < 2; ++i) {
        if (!weights[i]) {
            weights[i] = 1;
            order[used++] = (uint16_t)i;
        }
    }
    for (i = 0; i < used; ++i)
        deflate_heap_push(heap, &heap_count, order[i], weights);
    while (heap_count > 1) {
        unsigned a = deflate_heap_pop(heap, &heap_count, weights);
        unsigned b = deflate_heap_pop(heap, &heap_count, weights);
        parents[a] = parents[b] = (uint16_t)next;
        weights[next] = weights[a] + weights[b];
        deflate_heap_push(heap, &heap_count, next++, weights);
    }
    for (i = 0; i < used; ++i) {
        unsigned node = order[i], depth = 0;
        while (parents[node] != UINT16_MAX) {
            ++depth;
            node = parents[node];
        }
        if (depth > max_bits) depth = max_bits;
        ++counts[depth];
    }
    for (bits = 1; bits <= max_bits; ++bits)
        slots += counts[bits] << (max_bits - bits);
    while (slots > (1u << max_bits)) {
        bits = max_bits - 1;
        while (!counts[bits]) --bits;
        --counts[bits];
        counts[bits + 1] += 2;
        --counts[max_bits];
        --slots;
    }
    for (i = 1; i < used; ++i) {
        unsigned j = i, symbol = order[i];
        while (j && deflate_heap_less(symbol, order[j - 1], weights)) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = (uint16_t)symbol;
    }
    next = 0;
    for (bits = max_bits; bits; --bits) {
        for (i = 0; i < counts[bits]; ++i)
            lengths[order[next++]] = (uint8_t)bits;
    }
    next = 0;
    for (bits = 1; bits <= max_bits; ++bits) {
        next = (next + counts[bits - 1]) << 1;
        next_code[bits] = next;
    }
    for (i = 0; i < alphabet; ++i) {
        bits = lengths[i];
        if (bits)
            codes[i] = deflate_reverse(next_code[bits]++, bits) | (bits << 16);
    }
}

static void deflate_add_run(deflate_tree *tree, uint32_t freq[19],
                            unsigned symbol, unsigned extra, unsigned bits)
{
    deflate_run *run = &tree->runs[tree->run_count++];
    run->symbol = (uint8_t)symbol;
    run->extra = (uint8_t)extra;
    run->extra_bits = (uint8_t)bits;
    ++freq[symbol];
}

static void deflate_make_tree(deflate_tree *tree, const uint32_t lit_freq[286],
                              const uint32_t dist_freq[30], uint64_t extra_bits)
{
    uint8_t lengths[316];
    uint32_t cl_freq[19] = {0};
    unsigned i, total;
    deflate_huffman(lit_freq, 286, 15, tree->lit_lengths, tree->lit_codes);
    deflate_huffman(dist_freq, 30, 15, tree->dist_lengths, tree->dist_codes);
    tree->lit_count = 286;
    while (tree->lit_count > 257 && !tree->lit_lengths[tree->lit_count - 1])
        --tree->lit_count;
    tree->dist_count = 30;
    while (tree->dist_count > 1 && !tree->dist_lengths[tree->dist_count - 1])
        --tree->dist_count;
    memcpy(lengths, tree->lit_lengths, tree->lit_count);
    memcpy(lengths + tree->lit_count, tree->dist_lengths, tree->dist_count);
    total = tree->lit_count + tree->dist_count;
    tree->run_count = 0;
    for (i = 0; i < total;) {
        unsigned value = lengths[i], count = 1;
        while (i + count < total && lengths[i + count] == value) ++count;
        i += count;
        if (!value) {
            while (count >= 11) {
                unsigned n = count > 138 ? 138 : count;
                deflate_add_run(tree, cl_freq, 18, n - 11, 7);
                count -= n;
            }
            if (count >= 3) {
                unsigned n = count > 10 ? 10 : count;
                deflate_add_run(tree, cl_freq, 17, n - 3, 3);
                count -= n;
            }
            while (count--) deflate_add_run(tree, cl_freq, 0, 0, 0);
        } else {
            deflate_add_run(tree, cl_freq, value, 0, 0);
            --count;
            while (count >= 3) {
                unsigned n = count > 6 ? 6 : count;
                deflate_add_run(tree, cl_freq, 16, n - 3, 2);
                count -= n;
            }
            while (count--) deflate_add_run(tree, cl_freq, value, 0, 0);
        }
    }
    deflate_huffman(cl_freq, 19, 7, tree->cl_lengths, tree->cl_codes);
    tree->cl_count = 19;
    while (tree->cl_count > 4 && !tree->cl_lengths[deflate_cl_order[tree->cl_count - 1]])
        --tree->cl_count;
    tree->bits = 3 + 14 + 3 * tree->cl_count + extra_bits;
    for (i = 0; i < tree->run_count; ++i)
        tree->bits += tree->cl_lengths[tree->runs[i].symbol] + tree->runs[i].extra_bits;
    for (i = 0; i < 286; ++i) tree->bits += (uint64_t)lit_freq[i] * tree->lit_lengths[i];
    for (i = 0; i < 30; ++i) tree->bits += (uint64_t)dist_freq[i] * tree->dist_lengths[i];
}

PTPNG_API_INLINE int deflate_code(deflate_writer *w, uint32_t code)
{
    return deflate_put(w, code & 65535, code >> 16);
}

static int deflate_header(deflate_writer *w, const deflate_tree *tree, unsigned final)
{
    unsigned i;
    if (!deflate_put(w, 4 | final, 3) ||
        !deflate_put(w, tree->lit_count - 257, 5) ||
        !deflate_put(w, tree->dist_count - 1, 5) ||
        !deflate_put(w, tree->cl_count - 4, 4)) return 0;
    for (i = 0; i < tree->cl_count; ++i)
        if (!deflate_put(w, tree->cl_lengths[deflate_cl_order[i]], 3)) return 0;
    for (i = 0; i < tree->run_count; ++i) {
        const deflate_run *run = &tree->runs[i];
        if (!deflate_code(w, tree->cl_codes[run->symbol]) ||
            !deflate_put(w, run->extra, run->extra_bits)) return 0;
    }
    return 1;
}

static int deflate_emit_tokens(deflate_writer *w, const deflate_token *tokens,
                               unsigned count, const uint32_t *lit_codes,
                               const uint32_t *dist_codes,
                               const uint8_t len_symbol[259])
{
    unsigned i;
    for (i = 0; i < count; ++i) {
        unsigned value = tokens[i].value, distance = tokens[i].distance;
        if (!distance) {
            if (!deflate_code(w, lit_codes[value])) return 0;
        } else {
            unsigned ls = len_symbol[value], ds = deflate_dist_symbol(distance);
            uint32_t code = lit_codes[257 + ls];
            unsigned bits = code >> 16;
            if (!deflate_put(w, (code & 65535) |
                               ((value - deflate_len_base[ls]) << bits),
                             bits + deflate_len_extra[ls])) return 0;
            code = dist_codes[ds];
            bits = code >> 16;
            if (!deflate_put(w, (code & 65535) |
                               ((distance - deflate_dist_base[ds]) << bits),
                             bits + (ds < 4 ? 0 : (ds >> 1) - 1))) return 0;
        }
    }
    return deflate_code(w, lit_codes[256]);
}

static int deflate_align(deflate_writer *w)
{
    while (w->count) {
        if (w->pos == w->limit) return 0;
        w->dst[w->pos++] = (uint8_t)w->bits;
        w->bits >>= 8;
        w->count = w->count > 8 ? w->count - 8 : 0;
    }
    return 1;
}

static int deflate_stored_block(deflate_writer *w, const uint8_t *src,
                                size_t size, unsigned final)
{
    do {
        unsigned n = (unsigned)(size > 65535 ? 65535 : size);
        if (!deflate_put(w, final && size <= 65535, 3) || !deflate_align(w) ||
            w->limit - w->pos < (size_t)n + 4) return 0;
        w->dst[w->pos++] = (uint8_t)n;
        w->dst[w->pos++] = (uint8_t)(n >> 8);
        w->dst[w->pos++] = (uint8_t)~n;
        w->dst[w->pos++] = (uint8_t)(~n >> 8);
        if (n) {
            memcpy(w->dst + w->pos, src, n);
            src += n;
        }
        w->pos += n;
        size -= n;
    } while (size);
    return 1;
}

static int deflate_compress(deflate_writer *w, deflate_state *state,
                            const uint8_t *src, size_t size)
{
    uint32_t fixed_lit[286], fixed_dist[30], lit_freq[286], dist_freq[30];
    uint8_t len_symbol[259] = {0};
    deflate_tree tree;
    size_t pos = 0;
    unsigned i;
    for (i = 0; i < 286; ++i) {
        unsigned bits = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        unsigned code = i < 144 ? i + 48 : i < 256 ? i + 256 :
                        i < 280 ? i - 256 : i - 88;
        fixed_lit[i] = deflate_reverse(code, bits) | (bits << 16);
    }
    for (i = 0; i < 30; ++i)
        fixed_dist[i] = deflate_reverse(i, 5) | (5u << 16);
    for (i = 0; i < 29; ++i) {
        unsigned n, end = i == 28 ? 259 : deflate_len_base[i + 1];
        for (n = deflate_len_base[i]; n < end; ++n) len_symbol[n] = (uint8_t)i;
    }
    if (!size)
        return deflate_put(w, 3, 3) && deflate_code(w, fixed_lit[256]) &&
               deflate_align(w);
    while (pos < size) {
        size_t start = pos, block_size, stored_blocks;
        unsigned count, final;
        uint64_t extra_bits, fixed_bits, stored_bits;
        pos = deflate_parse(state, src, size, pos, &count, lit_freq, dist_freq,
                            &extra_bits, len_symbol);
        final = pos == size;
        block_size = pos - start;
        fixed_bits = 3 + extra_bits;
        for (i = 0; i < 286; ++i)
            fixed_bits += (uint64_t)lit_freq[i] * (fixed_lit[i] >> 16);
        for (i = 0; i < 30; ++i) fixed_bits += (uint64_t)dist_freq[i] * 5;
        stored_blocks = (block_size + 65534) / 65535;
        stored_bits = (uint64_t)block_size * 8 + (stored_blocks - 1) * 40 +
                      3 + ((8 - ((w->count + 3) & 7)) & 7) + 32;
        deflate_make_tree(&tree, lit_freq, dist_freq, extra_bits);
        if (stored_bits <= fixed_bits && stored_bits <= tree.bits) {
            if (!deflate_stored_block(w, src + start, block_size, final)) return 0;
        } else if (fixed_bits <= tree.bits) {
            if (!deflate_put(w, 2 | final, 3) ||
                !deflate_emit_tokens(w, state->tokens, count, fixed_lit,
                                      fixed_dist, len_symbol)) return 0;
        } else {
            if (!deflate_header(w, &tree, final) ||
                !deflate_emit_tokens(w, state->tokens, count, tree.lit_codes,
                                      tree.dist_codes, len_symbol)) return 0;
        }
    }
    return deflate_align(w);
}

int ptpng_deflate(const uint8_t *src, size_t size, uint8_t **out, size_t *out_len)
{
    size_t blocks = size / 65535 + (size % 65535 != 0), capacity;
    uint8_t *dst;
    deflate_state *state = NULL;
    deflate_writer writer;
    uint32_t adler;
    *out = NULL;
    *out_len = 0;
    if (!blocks) blocks = 1;
    if (size > SIZE_MAX - 6 || blocks > (SIZE_MAX - size - 6) / 5)
        return PTPNG_E_TOO_LARGE;
    capacity = size + blocks * 5 + 6;
    dst = (uint8_t *)malloc(capacity);
    if (!dst) return PTPNG_E_OUT_OF_MEMORY;
    if (size) {
        state = (deflate_state *)calloc(1, sizeof(*state));
        if (!state) {
            free(dst);
            return PTPNG_E_OUT_OF_MEMORY;
        }
    }
    dst[0] = 0x78;
    dst[1] = 0x01;
    writer.dst = dst;
    writer.pos = 2;
    writer.limit = capacity - 4;
    writer.bits = 0;
    writer.count = 0;
    if (!deflate_compress(&writer, state, src, size)) {
        /* Mixed block boundaries can exceed the single stored-stream bound.
         * Replacing the entire stream preserves the public allocation bound. */
        writer.pos = 2;
        writer.bits = 0;
        writer.count = 0;
        if (!deflate_stored_block(&writer, src, size, 1)) {
            free(state);
            free(dst);
            return PTPNG_E_TOO_LARGE;
        }
    }
    free(state);
    adler = ptpng_adler32(src, size);
    dst[writer.pos++] = (uint8_t)(adler >> 24);
    dst[writer.pos++] = (uint8_t)(adler >> 16);
    dst[writer.pos++] = (uint8_t)(adler >> 8);
    dst[writer.pos++] = (uint8_t)adler;
    *out = dst;
    *out_len = writer.pos;
    return PTPNG_OK;
}
