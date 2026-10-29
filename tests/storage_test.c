/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of firmware/src/storage.c against a simulated NOR flash:
 * erase -> 0xFF, program can only clear bits, page writes must not wrap. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t nor[0x100000];
static int fail_after = -1;            /* torn-write injection: stop after N programs */

static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (fail_after == 0)
        return -9;
    if (fail_after > 0)
        fail_after--;
    if ((off & 0xFFu) + n > 256u) {
        printf("page wrap at %#x\n", off);
        exit(1);
    }
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    return 0;
}
/* the regions the app's flash driver may write (firmware/hal/fm1_flash.h, which is device code) */
#define FL_DATA_LO 0x00097000u
#define FL_DATA_HI 0x000E0000u
#define FL_GLOB_LO 0x000FC000u
#define FL_GLOB_HI 0x000FF000u
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_STORE_OK(off, n) (FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) || FL_IN(off, n, FL_GLOB_LO, FL_GLOB_HI))
#include "../firmware/src/app/plat.h"     /* the object map */
#include "../firmware/src/storage.c"
#define OBJ_PROJECT0 OBJ_PANEL                 /* the objects are the settings and the panel */
#define OBJ_SETTINGS OBJ_SET

static int check(const char *what, int ok)
{
    printf("%-46s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

/* the chunk store: an object is bytes, here a pattern from a seed */
static uint8_t pat(uint32_t seed, uint32_t i) { return (uint8_t)(i * 7u + seed * 13u + (i >> 9)); }
static int obj_is(uint32_t o, uint32_t seed, uint32_t len)   /* o holds that: 1 */
{
    static uint8_t b[70000];
    uint32_t i;
    int n;
    if (cs_open(o) || cs_bytes(o) != len)
        return 0;
    n = cs_read(b, sizeof b);
    if (n != (int)len)
        return 0;
    for (i = 0; i < len; i++)
        if (b[i] != pat(seed, i))
            return 0;
    return 1;
}
static int save(uint32_t o, uint32_t seed, uint32_t len)
{
    static uint8_t b[70000];
    uint32_t i, k;
    for (i = 0; i < len; i++)
        b[i] = pat(seed, i);
    if (cs_wopen(o))
        return -1;
    for (i = 0; i < len; i += k) {             /* in uneven pieces, as a compressor writes */
        k = 1u + (i * 31u + seed) % 700u;
        if (k > len - i)
            k = len - i;
        if (cs_write(b + i, k)) {
            cs_wabort();
            return cs_w.err;
        }
    }
    return cs_wclose();
}

static int chunk_store_tests(void)
{
    int bad = 0;
    uint32_t i, n, full = (CS_N - CS_RESERVE) * CS_PAY;
    memset(nor, 0xFF, sizeof nor);
    cs_scan();
    bad += check("chunks: an empty store", cs_room() == full && cs_open(0) < 0 && cs_bytes(5) == 0);
    bad += check("a small object saves", save(3, 1, 200) == 0 && obj_is(3, 1, 200));
    bad += check("... in one chunk", cs_room() == full - CS_PAY);
    bad += check("an object of many chunks", save(300, 2, 20 * CS_PAY + 17) == 0 && obj_is(300, 2, 20 * CS_PAY + 17));
    bad += check("exactly a chunk", save(9, 3, CS_PAY) == 0 && obj_is(9, 3, CS_PAY));
    bad += check("an empty object", save(10, 3, 0) == 0 && cs_open(10) == 0 && cs_read(&n, 4) == 0);
    cs_scan();                                   /* a reboot */
    bad += check("after a reboot: all of them", obj_is(3, 1, 200) && obj_is(300, 2, 20 * CS_PAY + 17) &&
                                              obj_is(9, 3, CS_PAY) && cs_open(10) == 0 && cs_open(4) < 0);
    bad += check("a resave replaces it", save(300, 4, CS_PAY + 1) == 0 && obj_is(300, 4, CS_PAY + 1));
    cs_scan();
    bad += check("... and its old chunks are room again", obj_is(300, 4, CS_PAY + 1) &&
                                                          cs_room() == full - 5 * CS_PAY);
    for (n = 1; n < 12; n++) {                   /* torn after n programs: the object before stays */
        fail_after = (int)n;
        save(300, 5, 5 * CS_PAY + 100);
        fail_after = -1;
        cs_scan();
        if (!obj_is(300, 4, CS_PAY + 1) && !obj_is(300, 5, 5 * CS_PAY + 100)) {
            printf("  torn after %u programs: neither\n", n);
            bad++;
            break;
        }
        if (obj_is(300, 5, 5 * CS_PAY + 100) && n < 6u) {
            printf("  torn after %u programs: the new one, too early\n", n);
            bad++;
            break;
        }
        if (save(300, 4, CS_PAY + 1) != 0)
            bad++;
    }
    bad += check("torn saves keep the version before", n == 12 && obj_is(3, 1, 200));
    for (i = 0; i < CS_N; i++)                   /* bit rot in object 3 */
        if (cs_obj[i] == 3)
            nor[cs_addr(i) + CS_HDR + 3] ^= 0x01;
    cs_scan();
    bad += check("a rotten object is gone, the others stay", cs_open(3) < 0 && obj_is(300, 4, CS_PAY + 1));
    bad += check("delete", save(3, 6, 100) == 0 && save(3, 7, 2000) == 0 && cs_delete(3) == 0 && cs_open(3) < 0);
    cs_scan();
    bad += check("... stays deleted (no older version comes back)", cs_open(3) < 0 && obj_is(300, 4, CS_PAY + 1));
    memset(nor, 0xFF, sizeof nor);
    cs_scan();
#define SZ(o) ((o) < 256u ? 700u : 1000u)
    for (i = 0; i < 256u && save(i, i, SZ(i)) == 0; i++)
        ;
    bad += check("256 songs of 700 bytes fit", i == 256u && cs_room() == full - 256u * 3u * CS_PAY);
    for (i = 256; i < 280u && save(i, i, SZ(i)) == 0; i++)
        ;
    bad += check("and the library's 24 banks", i == 280u);
    for (n = 0; n < 3000u; n++)                  /* resaves, everywhere: the store compacts */
        if (save(n * 7u % 280u, n, SZ(n * 7u % 280u)) != 0)
            break;
    bad += check("3000 resaves in a full-ish store", n == 3000u);
    cs_scan();
    for (i = 0; i < 280u; i++) {                 /* each holds its newest */
        uint32_t k, last = i;
        for (k = 0; k < 3000u; k++)
            if (k * 7u % 280u == i)
                last = k;
        if (!obj_is(i, last, SZ(i)))
            break;
    }
    bad += check("... and every object loads after a reboot", i == 280u);
    {
        uint32_t room = cs_room();
        uint32_t k, last = 5;
        for (k = 0; k < 3000u; k++)
            if (k * 7u % 280u == 5)
                last = k;
        bad += check("one too big for the room is refused", save(5, 9, room + CS_PAY) == -10 && obj_is(5, last, 700));
        cs_scan();
        bad += check("... and leaves the store as it was", cs_room() == room);
        bad += check("one that just fits", save(400, 9, room) == 0 && obj_is(400, 9, room) && cs_room() == 0);
        cs_scan();
        bad += check("... loads", obj_is(400, 9, room));
        bad += check("a full store refuses more", save(401, 1, 300) == -10);
        bad += check("deleting makes room", cs_delete(400) == 0 && save(401, 1, 300) == 0 && obj_is(401, 1, 300));
    }
    {   /* another firmware's data in the area: dead, erased as room is needed */
        memset(nor + 0x97000, 0x5A, 0xDC000 - 0x97000);
        memset(nor + 0xDE000, 0x5A, 0x2000);
        cs_scan();
        bad += check("a foreign store: room for everything", cs_room() == full && save(0, 1, 50000) == 0 &&
                                                             obj_is(0, 1, 50000));
    }
    {   /* the erases spread: many saves of one small object wear every sector about the same */
        static uint32_t erases[CS_NSEC];
        uint32_t lo = 0xFFFFFFFFu, hi = 0, k;
        memset(nor, 0xFF, sizeof nor);
        cs_scan();
        for (i = 0; i < 40u * CS_N; i++) {
            uint32_t had[CS_NSEC];
            for (k = 0; k < CS_NSEC; k++)
                had[k] = cs_obj[k * CS_PER] == CS_FREE;
            save(1, i, 300);
            for (k = 0; k < CS_NSEC; k++)
                if (!had[k] && cs_obj[k * CS_PER] == CS_FREE)
                    erases[k]++;
        }
        for (i = 0; i < CS_NSEC; i++) {
            lo = erases[i] < lo ? erases[i] : lo;
            hi = erases[i] > hi ? erases[i] : hi;
        }
        printf("  erases a sector: %u .. %u\n", lo, hi);
        bad += check("wear is spread over the sectors", lo + 2u >= hi && lo > 0);
    }
    return bad;
}

int main(void)
{
    char a[600], b[600], got[600];
    int bad = 0, n;
    memset(nor, 0xFF, sizeof nor);
    memset(a, 'A', sizeof a);
    memset(b, 'B', sizeof b);
    bad += check("empty flash loads nothing", st_load(OBJ_PROJECT0, got, sizeof got) < 0);
    bad += check("save A", st_save(OBJ_PROJECT0, a, sizeof a) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns A", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    bad += check("save B", st_save(OBJ_PROJECT0, b, sizeof b) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns B (newer seq)", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    fail_after = 1;                                  /* payload page 1 written, the rest torn */
    st_save(OBJ_PROJECT0, a, sizeof a);
    fail_after = -1;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("torn save keeps B", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    fail_after = 3;                                  /* all payload pages, header torn */
    st_save(OBJ_PROJECT0, a, sizeof a);
    fail_after = -1;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("header not written keeps B", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    bad += check("save A again", st_save(OBJ_PROJECT0, a, sizeof a) == 0);
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("load returns A", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    nor[st_sector(OBJ_PROJECT0, 0) + ST_PAYLOAD_OFF + 10] ^= 0x01;   /* bit rot in one copy */
    nor[st_sector(OBJ_PROJECT0, 1) + ST_PAYLOAD_OFF + 10] ^= 0x01;
    n = st_load(OBJ_PROJECT0, got, sizeof got);
    bad += check("both copies corrupt -> nothing", n < 0);
    bad += check("other objects untouched", st_load(OBJ_SET, got, sizeof got) < 0);
    bad += check("settings save/load",
                 st_save(OBJ_SETTINGS, "hello", 5) == 0 && st_load(OBJ_SETTINGS, got, 5) == 5 && !memcmp(got, "hello", 5));
    bad += check("CRC-32 is zlib's (check value 0xCBF43926)", st_crc32("123456789", 9) == 0xCBF43926u);
    memset(nor, 0xFF, sizeof nor);
    st_save(OBJ_PANEL, a, sizeof a);
    st_save(OBJ_PANEL, b, sizeof b);              /* B is newer, in the other copy */
    {
        st_hdr_t h;
        int cur = st_current(OBJ_PANEL, &h);
        nor[st_sector(OBJ_PANEL, (uint32_t)cur) + ST_PAYLOAD_OFF + 300] ^= 0x10;   /* rot in the newer copy */
    }
    n = st_load(OBJ_PANEL, got, sizeof got);
    bad += check("newer copy rotten -> the older one loads", n == (int)sizeof a && !memcmp(got, a, sizeof a));
    bad += check("save over the rotten copy", st_save(OBJ_PANEL, b, sizeof b) == 0);
    n = st_load(OBJ_PANEL, got, sizeof got);
    bad += check("... loads the new data", n == (int)sizeof b && !memcmp(got, b, sizeof b));
    nor[st_sector(OBJ_PANEL, 0) + 8] ^= 0x01;    /* both headers broken */
    nor[st_sector(OBJ_PANEL, 1) + 8] ^= 0x01;
    bad += check("both headers broken -> nothing", st_load(OBJ_PANEL, got, sizeof got) < 0);
    bad += chunk_store_tests();
    {   /* the map: the objects and the chunk store's sectors inside Felucca's data areas, below the loader's
         * staging area (0xE0000) but the settings, and no overlaps */
        static uint8_t owner[0x100];             /* by sector */
        uint32_t o, c, i, ok = 1;
        memset(owner, 0, sizeof owner);
        for (o = 0; o < OBJ_NOBJ; o++)
            for (c = 0; c < 2; c++) {
                uint32_t a0 = st_sector(o, c);
                if (!FL_STORE_OK(a0, 4096) || owner[a0 >> 12]++)
                    ok = 0;
            }
        for (i = 0; i < CS_NSEC; i++) {
            uint32_t a0 = cs_addr(i * CS_PER);
            if (!FL_STORE_OK(a0, 4096) || a0 < 0x97000 || a0 >= 0xE0000 || owner[a0 >> 12]++)
                ok = 0;
        }
        for (i = 0x97; i < 0xE0; i++)            /* and every sector of the data area is used */
            if (owner[i] != 1)
                ok = 0;
        bad += check("objects, chunks in the data areas, no overlaps", (int)ok);
    }
    printf("%s\n", bad ? "STORAGE TEST FAILED" : "storage test passed");
    return bad != 0;
}
