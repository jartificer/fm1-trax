/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE settings, song slots and the instrument library. Part of the unity build, after app.h
 * and lz4.c; the host simulator includes it too.
 *
 * A song in flash: "FDS1", its title (24 bytes, for the song list), then its byte stream
 * (song_write) in LZ4 blocks of up to 4096 bytes: [raw length u16][packed length u16][bytes], a
 * packed length equal to the raw one meaning stored as is; a raw length of 0 ends it.
 * A library bank: "FDL1", then its 16 instruments as the song stream has them (ins_write). */
#include "../trk/demo_song.h"

settings_t set;
slot_info_t slot_info[NSLOTS] OM_POOL;

void settings_defaults(void)
{
    memset(&set, 0, sizeof set);
    set.magic = SET_MAGIC;
    set.format = SET_FORMAT;
    set.crt = 1;
    set.intro = 1;
    set.leds = 1;
    set.step = 1;
    set.oct = 3;
    set.follow = 1;
    set.midi_in = 1;
    set.wipes = 1;
}

static int settings_valid(void)
{
    return set.magic == SET_MAGIC && set.format == SET_FORMAT && set.theme < NTHEMES && set.crt <= 1 &&
           set.intro <= 1 && set.leds <= 1 && set.step <= 16 && set.oct <= 7 && set.follow <= 1 &&
           set.midi_in <= 1 && set.wipes <= 1;
}

int settings_load(void)
{
    int n;
    memset(&set, 0, sizeof set);
    n = plat_store_load(OBJ_SET, &set, sizeof set);
    if (n >= 8 && settings_valid())
        return 0;
    settings_defaults();
    return -1;
}

int settings_save(void) { return plat_store_save(OBJ_SET, &set, sizeof set); }

/* ------------------------------------------------------------- packing --- */
static uint8_t pk_raw[LZ4_MAX_IN] __attribute__((section(".pool")));
static uint8_t pk_lz[LZ4_BOUND(LZ4_MAX_IN) + 4u] __attribute__((section(".pool")));
static struct {
    void (*put)(void *, const uint8_t *, uint32_t);
    void *ctx;
    uint32_t fill, out;
} pk;

static void pk_emit(const uint8_t *b, uint32_t n)
{
    pk.put(pk.ctx, b, n);
    pk.out += n;
}

static void pk_block(void)
{
    uint32_t c = lz4_compress(pk_raw, pk.fill, pk_lz + 4);
    const uint8_t *body = pk_lz + 4;
    if (c >= pk.fill) {                         /* no gain: stored */
        c = pk.fill;
        body = pk_raw;
    }
    pk_lz[0] = (uint8_t)pk.fill;
    pk_lz[1] = (uint8_t)(pk.fill >> 8);
    pk_lz[2] = (uint8_t)c;
    pk_lz[3] = (uint8_t)(c >> 8);
    pk_emit(pk_lz, 4);
    pk_emit(body, c);
    pk.fill = 0;
}

static void pk_put(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    while (n) {
        uint32_t k = LZ4_MAX_IN - pk.fill;
        if (k > n)
            k = n;
        memcpy(pk_raw + pk.fill, b, k);
        pk.fill += k;
        b += k;
        n -= k;
        if (pk.fill == LZ4_MAX_IN)
            pk_block();
    }
}

uint32_t song_packed(const song_t *s, void (*put)(void *, const uint8_t *, uint32_t), void *ctx)
{
    static const uint8_t end[4] = {0, 0, 0, 0};
    pk.put = put;
    pk.ctx = ctx;
    pk.fill = pk.out = 0;
    pk_emit((const uint8_t *)"FDS1", 4);
    pk_emit((const uint8_t *)s->title, sizeof s->title);
    song_write(s, pk_put, 0);
    if (pk.fill)
        pk_block();
    pk_emit(end, 4);
    return pk.out;
}

/* reading one back: the blocks from the chunk store, a byte at a time */
static struct {
    uint32_t pos, len;
    int bad, end;
} up;

static int up_get(void *ctx)
{
    (void)ctx;
    while (up.pos >= up.len) {
        uint8_t h[4];
        uint32_t raw, c;
        if (up.bad || up.end)
            return -1;
        if (plat_obj_read(h, 4) != 4) {
            up.bad = 1;
            return -1;
        }
        raw = h[0] | (uint32_t)h[1] << 8;
        c = h[2] | (uint32_t)h[3] << 8;
        if (!raw) {
            up.end = 1;
            return -1;
        }
        if (raw > LZ4_MAX_IN || c > raw || plat_obj_read(pk_lz, c) != (int)c) {
            up.bad = 1;
            return -1;
        }
        if (c == raw)
            memcpy(pk_raw, pk_lz, raw);
        else if (lz4_decompress(pk_lz, c, pk_raw, LZ4_MAX_IN) != (int)raw) {
            up.bad = 1;
            return -1;
        }
        up.pos = 0;
        up.len = raw;
    }
    return pk_raw[up.pos++];
}

/* -------------------------------------------------------------- slots --- */
static void slot_read_info(int k)
{
    uint8_t h[28];
    slot_info_t *i = &slot_info[k];
    memset(i, 0, sizeof *i);
    if (plat_obj_open(OBJ_SONG(k)) != 0 || plat_obj_read(h, sizeof h) != (int)sizeof h || memcmp(h, "FDS1", 4))
        return;
    i->used = 1;
    memcpy(i->title, h + 4, sizeof i->title);
    i->title[sizeof i->title - 1u] = 0;
    i->bytes = plat_obj_bytes(OBJ_SONG(k));
}

void slots_scan(void)
{
    int k;
    plat_obj_scan();
    for (k = 0; k < NSLOTS; k++)
        slot_read_info(k);
}

int song_load_slot(int slot)
{
    uint8_t h[28];
    int rc;
    if (plat_obj_open(OBJ_SONG(slot)) != 0) {
        song_init(&song);
        return 1;
    }
    memset(&up, 0, sizeof up);
    if (plat_obj_read(h, sizeof h) != (int)sizeof h || memcmp(h, "FDS1", 4)) {
        song_init(&song);
        return -1;
    }
    rc = song_read(&song, up_get, 0);
    if (rc != 0 || up.bad) {
        song_init(&song);
        return -1;
    }
    return 0;
}

static int sv_err;
static void sv_put(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    if (!sv_err)
        sv_err = plat_obj_write(b, n);
}

int song_save_slot(int slot)
{
    int rc;
    if ((rc = plat_obj_wopen(OBJ_SONG(slot))) != 0)
        return rc;
    sv_err = 0;
    song_packed(&song, sv_put, 0);
    if (sv_err) {
        plat_obj_wabort();
        rc = sv_err;
    } else {
        rc = plat_obj_wclose();
    }
    slot_read_info(slot);
    return rc;
}

int song_delete_slot(int slot)
{
    int rc = plat_obj_delete(OBJ_SONG(slot));
    slot_read_info(slot);
    return rc;
}

void song_boot_load(void)
{
    int k, any = 0;
    slots_scan();
    for (k = NSLOTS - 1; k >= 0; k--)
        if (slot_info[k].used)
            any = k + 1;
    if (any && !slot_info[set.song].used)       /* the slot in use was deleted: the first song there is */
        set.song = (uint8_t)(any - 1);
    player.lock = 1;
    if (!any)
        demo_song(&song);                       /* a new FM-1: something to play at once */
    else if (song_load_slot(set.song) != 0)
        song_init(&song);
    player.lock = 0;
    player_post(PC_RESET, 0, 0, 0);
}

/* ------------------------------------------------------------ library --- */
static ins_t lib_cache[LIB_BANK] __attribute__((section(".pool")));
static int lib_cached = -1;

static int lib_get_byte(void *ctx)
{
    uint8_t b;
    (void)ctx;
    return plat_obj_read(&b, 1) == 1 ? b : -1;
}

static void lib_load(int bank)
{
    char m[4];
    int k;
    if (bank == lib_cached)
        return;
    lib_cached = bank;
    for (k = 0; k < LIB_BANK; k++)
        ins_init(&lib_cache[k], INS_EMPTY);
    if (plat_obj_open(OBJ_LIB(bank)) != 0 || plat_obj_read(m, 4) != 4 || memcmp(m, "FDL1", 4))
        return;
    for (k = 0; k < LIB_BANK; k++)
        if (ins_read(&lib_cache[k], lib_get_byte, 0) != 0)
            break;
}

void lib_forget(void) { lib_cached = -1; }

void lib_get(int slot, ins_t *out)
{
    if (slot < 0 || slot >= LIB_N) {
        ins_init(out, INS_EMPTY);
        return;
    }
    lib_load(slot / LIB_BANK);
    *out = lib_cache[slot % LIB_BANK];
}

int lib_used(int slot)
{
    if (slot < 0 || slot >= LIB_N)
        return 0;
    lib_load(slot / LIB_BANK);
    return lib_cache[slot % LIB_BANK].type != INS_EMPTY;
}

int lib_put(int slot, const ins_t *in)
{
    int bank = slot / LIB_BANK, k, rc;
    if (slot < 0 || slot >= LIB_N)
        return -1;
    lib_load(bank);
    lib_cache[slot % LIB_BANK] = *in;
    if ((rc = plat_obj_wopen(OBJ_LIB(bank))) != 0)
        return rc;
    sv_err = plat_obj_write("FDL1", 4);
    for (k = 0; k < LIB_BANK; k++)
        ins_write(&lib_cache[k], sv_put, 0);
    if (sv_err) {
        plat_obj_wabort();
        lib_cached = -1;
        return sv_err;
    }
    rc = plat_obj_wclose();
    if (rc)
        lib_cached = -1;                        /* the bank in flash is the one before */
    return rc;
}
