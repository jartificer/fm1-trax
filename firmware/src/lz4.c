/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: LZ4 blocks (the block format, no frame:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md), for the songs in flash. A greedy
 * compressor with a 4096-entry hash table (tools/lz4blk.py is the same in Python, and the web editor
 * has it in JavaScript), and a decoder that checks every length against both buffers. */
#include <stdint.h>

#define LZ4_HASH_BITS 12u
#define LZ4_MAX_IN 4096u                       /* the app compresses in blocks of this */
/* the worst case: incompressible data grows by one byte in 255, plus a token */
#define LZ4_BOUND(n) ((n) + (n) / 255u + 16u)

static uint16_t lz4_table[1u << LZ4_HASH_BITS] __attribute__((section(".pool")));

static uint32_t lz4_rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t lz4_hash(uint32_t v) { return (v * 2654435761u) >> (32u - LZ4_HASH_BITS); }

static uint8_t *lz4_len(uint8_t *o, uint32_t n)
{
    while (n >= 255u) {
        *o++ = 255u;
        n -= 255u;
    }
    *o++ = (uint8_t)n;
    return o;
}

/* src[0..n) (n <= LZ4_MAX_IN) -> dst, which holds LZ4_BOUND(n); returns the compressed length */
static uint32_t lz4_compress(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t i = 0, anchor = 0, k;
    uint8_t *o = dst;
    for (k = 0; k < (1u << LZ4_HASH_BITS); k++)
        lz4_table[k] = 0xFFFFu;
    while (n >= 13u && i < n - 12u) {          /* a match starts 12 bytes before the end at the latest */
        uint32_t h = lz4_hash(lz4_rd32(src + i)), cand = lz4_table[h], m, lit;
        lz4_table[h] = (uint16_t)i;
        if (cand == 0xFFFFu || lz4_rd32(src + cand) != lz4_rd32(src + i)) {
            i++;
            continue;
        }
        m = 4;                                 /* the last 5 bytes stay literals */
        while (i + m < n - 5u && src[cand + m] == src[i + m])
            m++;
        lit = i - anchor;
        *o++ = (uint8_t)((lit < 15u ? lit : 15u) << 4 | (m - 4u < 15u ? m - 4u : 15u));
        if (lit >= 15u)
            o = lz4_len(o, lit - 15u);
        for (k = 0; k < lit; k++)
            *o++ = src[anchor + k];
        *o++ = (uint8_t)(i - cand);
        *o++ = (uint8_t)((i - cand) >> 8);
        if (m - 4u >= 15u)
            o = lz4_len(o, m - 4u - 15u);
        i += m;
        anchor = i;
    }
    k = n - anchor;                            /* the last literals */
    *o++ = (uint8_t)((k < 15u ? k : 15u) << 4);
    if (k >= 15u)
        o = lz4_len(o, k - 15u);
    for (i = 0; i < k; i++)
        *o++ = src[anchor + i];
    return (uint32_t)(o - dst);
}

/* src[0..n) -> dst (cap bytes); returns the decompressed length, or -1: damaged */
static int lz4_decompress(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t cap)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t tok = src[i++], lit = tok >> 4, m, off, k;
        if (lit == 15u)
            do {
                if (i >= n)
                    return -1;
                lit += src[i];
            } while (src[i++] == 255u);
        if (lit > n - i || lit > cap - o)
            return -1;
        for (k = 0; k < lit; k++)
            dst[o++] = src[i++];
        if (i == n)
            break;                             /* the last sequence has no match */
        if (n - i < 2u)
            return -1;
        off = src[i] | (uint32_t)src[i + 1] << 8;
        i += 2;
        m = (tok & 15u) + 4u;
        if ((tok & 15u) == 15u)
            do {
                if (i >= n)
                    return -1;
                m += src[i];
            } while (src[i++] == 255u);
        if (!off || off > o || m > cap - o)
            return -1;
        for (k = 0; k < m; k++, o++)
            dst[o] = dst[o - off];
    }
    return (int)o;
}
