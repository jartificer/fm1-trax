/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: firmware/src/lz4.c, round trips (text-like, tracker-like, random, short, empty) and
 * damaged input that must be refused without reading or writing out of bounds. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/src/lz4.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("%-48s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static uint32_t seed = 1;
static uint32_t rnd(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static int trip(const uint8_t *in, uint32_t n, uint32_t *packed)
{
    static uint8_t z[LZ4_BOUND(LZ4_MAX_IN)], out[LZ4_MAX_IN];
    uint32_t c = lz4_compress(in, n, z);
    int d = lz4_decompress(z, c, out, sizeof out);
    *packed = c;
    return c <= LZ4_BOUND(n) && d == (int)n && !memcmp(in, out, n);
}

int main(void)
{
    static uint8_t a[LZ4_MAX_IN], z[LZ4_BOUND(LZ4_MAX_IN)], out[LZ4_MAX_IN];
    uint32_t i, c, n, k;
    int ok;
    for (i = 0; i < sizeof a; i++)              /* rows of a tracker: mostly empty, some notes */
        a[i] = (uint8_t)(i % 7u == 0 ? 0x80u | (i / 7u % 5u) : i % 7u == 1 ? 49u + (i / 50u) % 12u : 0xFFu);
    check("tracker-like rows round trip", trip(a, sizeof a, &c));
    printf("  4096 bytes -> %u\n", c);
    check("... and shrink to under a third", c < sizeof a / 3u);
    for (i = 0; i < sizeof a; i++)
        a[i] = (uint8_t)rnd();
    check("random bytes round trip (they grow a little)", trip(a, sizeof a, &c) && c <= LZ4_BOUND(sizeof a));
    for (ok = 1, n = 0; n < 40u && ok; n++)     /* short inputs: the last literals' rules */
        ok = trip(a, n, &c);
    check("every length 0..39", ok);
    for (ok = 1, k = 0; k < 300u && ok; k++) {  /* random lengths of mixed data */
        n = rnd() % sizeof a;
        for (i = 0; i < n; i++)
            a[i] = (uint8_t)(rnd() % 4u == 0 ? rnd() : a[i ? i - 1 : 0]);
        ok = trip(a, n, &c);
    }
    check("300 mixed inputs", ok);
    memset(a, 'A', sizeof a);
    check("one byte repeated (overlapping matches)", trip(a, sizeof a, &c) && c < 40u);
    /* damage: every flipped byte of a packed block must be refused or decode within bounds */
    for (i = 0; i < sizeof a; i++)
        a[i] = (uint8_t)(i % 13u == 0 ? rnd() : i / 9u);
    c = lz4_compress(a, sizeof a, z);
    for (ok = 1, k = 0; k < 3000u; k++) {
        static uint8_t y[LZ4_BOUND(LZ4_MAX_IN)];
        int d;
        memcpy(y, z, c);
        y[rnd() % c] ^= (uint8_t)(1u << (rnd() % 8u));
        d = lz4_decompress(y, c - (k % 3u == 0 ? rnd() % 8u : 0u), out, sizeof out);
        ok = d <= (int)sizeof out;
    }
    check("3000 damaged blocks stay in bounds", ok);
    check("a match before the start is refused", lz4_decompress((const uint8_t[]){0x10, 'x', 0x05, 0x00, 0x00}, 5, out, 64) < 0);
    check("output larger than the room is refused", lz4_decompress(z, c, out, 100) < 0);
    printf("%s\n", bad ? "LZ4 TEST FAILED" : "lz4 test passed");
    return bad != 0;
}
