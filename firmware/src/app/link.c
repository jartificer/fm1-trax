/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the songs and the library over USB-MIDI SysEx, for the web editor (FM1-GM's link.c,
 * reworked). Part of the unity build after ui.c; the web emulator and the simulator include it too.
 * The includer supplies:
 *   link_rx(&p, &n)   the SysEx received and not yet taken (the bytes between F0 and F7): 1, else 0
 *   link_rx_done()    taken
 *   link_tx(p, n)     send one whole message, F0 .. F7
 *
 *   host -> FM-1:  F0 7D 46 44 cmd args .. F7        FM-1 -> host:  F0 7D 46 44 cmd|40 status args .. F7
 * 7D is MIDI's ID for non-commercial use, 46 44 "FD". Numbers go 7 bits a byte, the low bits first;
 * bytes are packed 8 bits into 7, the low bits first (as ota.c's). An object (2 bytes): a song slot
 * 0..255, 256 the song in memory, 512 + n library bank n (0..23: 16 FM banks, then 8 PSG).
 * A song goes as its .fm1d stream (song_write: what a file holds), a bank as stored ("FDL1" ...).
 *
 *   01 HELLO                    41 0   protocol, song format, slots (2), store KiB (2), KiB free (2),
 *                                      the slot in use (2), unsaved changes (1), version (ASCII)
 *   02 LIST first (2)           42 0   first (2), n, then n x (used, bytes in flash (3), title (24))
 *   03 READ obj (2) offset (3)  43 st  obj (2), offset (3), the bytes (3), up to LK_CHUNK of them, packed
 *   04 WRITE obj (2) bytes (3)  44 st  then its bytes in order, each DATA at most LK_CHUNK of them:
 *   05 DATA offset (3) packed   45 st  the bytes in so far (3); when they are all in, once the player
 *                               47 st  obj (2)   has stopped: the song is loaded (and saved), or why not
 *   06 DELETE obj (2)           46 st  obj (2)
 * status: 0 done, 1 a bad request (or an empty slot), 2 not a song, 3 no room, 4 a flash error, 5 busy.
 *
 * Nothing is held in RAM: a READ streams the song again up to its offset (a song in flash comes out
 * of its LZ4 blocks), and a WRITE goes straight into a scratch object in the chunk store, read from
 * there once it is all in (a bank: into the bank itself, which stays as it was until the last byte).
 * The host waits for each answer before it sends again (usb.c keeps one SysEx at a time). */
#define LK_PROTO 1u
#define LK_CHUNK 512u                    /* -> 586 bytes packed: a DATA fits usb.c's 640-byte frame */
#define LK_MEM 256u                      /* the song in memory */
#define LK_LIB 512u                      /* + bank */
#define OBJ_SCRATCH (PLAT_NOBJ - 1)      /* a song coming in */

enum { LK_IDLE, LK_RECV, LK_LOAD };
static uint8_t lk_out[16u + (LK_CHUNK * 8u + 6u) / 7u + 2u];
static uint8_t lk_chunk[LK_CHUNK];
static struct {
    uint8_t mode;
    uint32_t obj, total, have, t;
    /* a READ in progress: where the stream is */
    uint32_t r_obj, r_pos, r_total, r_epoch;
    int r_open;
} lk;

static uint32_t lk_num(const uint8_t *p, int k)
{
    uint32_t v = 0;
    while (k--)
        v = v << 7 | (p[k] & 127u);
    return v;
}

static uint8_t *lk_put(uint8_t *o, uint32_t v, int k)
{
    while (k--) {
        *o++ = (uint8_t)(v & 127u);
        v >>= 7;
    }
    return o;
}

static uint8_t *lk_head(uint32_t cmd, uint32_t st)
{
    lk_out[0] = 0xF0;
    lk_out[1] = 0x7D;
    lk_out[2] = 0x46;
    lk_out[3] = 0x44;
    lk_out[4] = (uint8_t)(cmd | 0x40u);
    lk_out[5] = (uint8_t)st;
    return lk_out + 6;
}

static void lk_send(uint8_t *end)
{
    *end++ = 0xF7;
    link_tx(lk_out, (uint32_t)(end - lk_out));
}

static int lk_valid(uint32_t obj) { return obj <= LK_MEM || (obj >= LK_LIB && obj < LK_LIB + LIB_N / LIB_BANK); }

/* ---- reading: n bytes of an object's stream from offset `from` into lk_chunk; returns how many */
static struct {
    uint32_t from, n, pos, got;
} lk_win;

static void lk_win_put(void *ctx, const uint8_t *b, uint32_t n)   /* song_write's bytes: keep the window's */
{
    (void)ctx;
    while (n--) {
        if (lk_win.pos >= lk_win.from && lk_win.pos < lk_win.from + lk_win.n)
            lk_chunk[lk_win.got++] = *b;
        lk_win.pos++;
        b++;
    }
}

static int lk_open(uint32_t obj)               /* the object's stream from its start: 0 */
{
    uint8_t h[28];
    lk.r_open = 0;
    if (obj < LK_MEM) {
        memset(&up, 0, sizeof up);             /* settings.c's unpacker */
        if (plat_obj_open(OBJ_SONG((int)obj)) != 0 || plat_obj_read(h, sizeof h) != (int)sizeof h || memcmp(h, "FDS1", 4))
            return -1;
    } else if (obj >= LK_LIB) {
        if (plat_obj_open(OBJ_LIB((int)(obj - LK_LIB))) != 0)
            return -1;
    }
    lk.r_obj = obj;
    lk.r_pos = 0;
    lk.r_open = 1;
    lk.r_epoch = plat_obj_epoch();
    return 0;
}

static int lk_byte(void)                       /* the next byte of the open stream, -1 at its end */
{
    if (lk.r_obj < LK_MEM)
        return up_get(0);
    {
        uint8_t b;
        return plat_obj_read(&b, 1) == 1 ? b : -1;
    }
}

static uint32_t lk_read(uint32_t obj, uint32_t from, uint32_t n)
{
    lk_win.from = from;
    lk_win.n = n;
    lk_win.pos = lk_win.got = 0;
    if (obj == LK_MEM) {                       /* the song in memory: streamed again */
        song_write(&song, lk_win_put, 0);
        return lk_win.got;
    }
    if (!lk.r_open || lk.r_obj != obj || lk.r_pos > from || lk.r_epoch != plat_obj_epoch()) {
        if (lk_open(obj))
            return 0;
    }
    while (lk.r_pos < from) {                  /* (the store's reader is shared: a resend starts over) */
        if (lk_byte() < 0)
            return 0;
        lk.r_pos++;
    }
    while (lk_win.got < n) {
        int c = lk_byte();
        if (c < 0)
            break;
        lk_chunk[lk_win.got++] = (uint8_t)c;
        lk.r_pos++;
    }
    return lk_win.got;
}

static uint32_t lk_length(uint32_t obj)        /* the stream's bytes (0: none) */
{
    uint32_t n = 0;
    if (obj == LK_MEM) {
        lk_win.from = lk_win.n = 0;
        lk_win.pos = lk_win.got = 0;
        return song_write(&song, lk_win_put, 0);
    }
    if (lk_open(obj))
        return 0;
    while (lk_byte() >= 0)
        n++;
    lk.r_open = 0;
    return n;
}

/* ---- writing */
static int lk_get_scratch(void *ctx)
{
    uint8_t b;
    (void)ctx;
    return plat_obj_read(&b, 1) == 1 ? b : -1;
}

static void lk_load(void)                      /* the song that came in, once the player has stopped */
{
    uint32_t st = 0;
    int rc;
    if (player.playing) {
        if (plat_ms() - lk.t > 3000u) {
            lk.mode = LK_IDLE;
            lk_put(lk_head(7, 5), lk.obj, 2);
            lk_send(lk_out + 8);
        }
        return;
    }
    player.lock = 1;
    if (plat_obj_open(OBJ_SCRATCH) != 0 || song_read(&song, lk_get_scratch, 0) != 0) {
        st = 2;
    } else if (lk.obj < LK_MEM) {
        rc = song_save_slot((int)lk.obj);
        st = rc == 0 ? 0u : rc == -10 ? 3u : 4u;
    }
    player.lock = 0;
    plat_obj_delete(OBJ_SCRATCH);
    ui_song_replaced(st == 0u && lk.obj < LK_MEM ? (int)lk.obj : -1);
    lk.mode = LK_IDLE;
    lk_put(lk_head(7, st), lk.obj, 2);
    lk_send(lk_out + 8);
}

static uint32_t lk_pack(uint8_t *o, const uint8_t *in, uint32_t n)
{
    uint32_t acc = 0, nb = 0, k = 0;
    while (n--) {
        acc |= (uint32_t)*in++ << nb;
        for (nb += 8u; nb >= 7u; nb -= 7u, acc >>= 7)
            o[k++] = (uint8_t)(acc & 127u);
    }
    if (nb)
        o[k++] = (uint8_t)acc;
    return k;
}

static uint32_t lk_unpack(uint8_t *o, const uint8_t *in, uint32_t n, uint32_t max)
{
    uint32_t acc = 0, nb = 0, k = 0;
    while (n--) {
        acc |= (uint32_t)(*in++ & 127u) << nb;
        if ((nb += 7u) >= 8u) {
            if (k < max)
                o[k++] = (uint8_t)acc;
            acc >>= 8;
            nb -= 8u;
        }
    }
    return k;
}

static void link_service(void)                 /* main loop */
{
    const uint8_t *p;
    uint32_t n, cmd, a, b, st = 0, k;
    uint8_t *o;
    if (lk.mode == LK_LOAD)
        lk_load();
    if (!link_rx(&p, &n))
        return;
    if (n < 4u || p[0] != 0x7D || p[1] != 0x46 || p[2] != 0x44)
        return;                                /* not ours (ota.c's, on the device) */
    cmd = p[3];
    p += 4;
    n -= 4u;
    a = n >= 2u ? lk_num(p, 2) : 0u;
    o = lk_head(cmd, 0);
    switch (cmd) {
    case 1: {
        static const char V[] = OM_VERSION;
        *o++ = LK_PROTO;
        *o++ = SONG_FORMAT;
        o = lk_put(o, NSLOTS, 2);
        o = lk_put(o, plat_obj_capacity() / 1024u, 2);
        o = lk_put(o, plat_obj_room() / 1024u, 2);
        o = lk_put(o, set.song, 2);
        *o++ = (uint8_t)ui_song_dirty();
        for (k = 0; k + 1u < sizeof V && k < 16u; k++)
            *o++ = (uint8_t)(V[k] & 127);
        break;
    }
    case 2:
        if (a >= NSLOTS || n < 2u) {
            st = 1;
            break;
        }
        b = NSLOTS - a < 8u ? NSLOTS - a : 8u;
        o = lk_put(o, a, 2);
        *o++ = (uint8_t)b;
        for (k = a; k < a + b; k++) {
            const slot_info_t *i = &slot_info[k];
            uint32_t c;
            *o++ = i->used;
            o = lk_put(o, i->bytes, 3);
            for (c = 0; c < 24u; c++)
                *o++ = (uint8_t)(i->title[c] & 127);
        }
        break;
    case 3:
        b = n >= 5u ? lk_num(p + 2, 3) : 0u;   /* the offset */
        if (lk.mode != LK_IDLE)
            st = 5;
        else if (n < 5u || !lk_valid(a) || (a < LK_MEM && !slot_info[a].used))
            st = 1;
        else if (!b && !(lk.r_total = lk_length(a)))
            st = 1;
        o = lk_put(o, a, 2);
        o = lk_put(o, b, 3);
        o = lk_put(o, st ? 0u : lk.r_total, 3);
        if (!st && b < lk.r_total) {
            k = lk_read(a, b, lk.r_total - b < LK_CHUNK ? lk.r_total - b : LK_CHUNK);
            o += lk_pack(o, lk_chunk, k);
        }
        break;
    case 4:
        b = n >= 5u ? lk_num(p + 2, 3) : 0u;   /* the bytes coming */
        if (lk.mode == LK_LOAD) {
            st = 5;
        } else if (n < 5u || !lk_valid(a) || !b) {
            st = 1;
        } else {
            if (lk.mode == LK_RECV)
                plat_obj_wabort();
            lk.r_open = 0;
            if (plat_obj_wopen(a >= LK_LIB ? OBJ_LIB((int)(a - LK_LIB)) : OBJ_SCRATCH) != 0) {
                st = 5;
            } else {
                lk.mode = LK_RECV;
                lk.obj = a;
                lk.total = b;
                lk.have = 0;
            }
        }
        break;
    case 5:
        b = n >= 3u ? lk_num(p, 3) : 0u;       /* where these bytes go */
        if ((lk.mode != LK_RECV && lk.mode != LK_LOAD) || n < 3u || b > lk.have) {
            st = 1;
        } else if (lk.mode == LK_RECV && b == lk.have) {   /* else a resend of what is in: the answer again */
            k = lk_unpack(lk_chunk, p + 3, n - 3u, lk.total - b < LK_CHUNK ? lk.total - b : LK_CHUNK);
            if (plat_obj_write(lk_chunk, k) != 0) {
                plat_obj_wabort();
                lk.mode = LK_IDLE;
                st = 3;
            } else {
                lk.have += k;
            }
            if (!st && lk.have == lk.total) {
                int rc = plat_obj_wclose();
                if (rc) {
                    lk.mode = LK_IDLE;
                    st = rc == -10 ? 3u : 4u;
                } else if (lk.obj >= LK_LIB) {
                    lib_forget();
                    lk.mode = LK_IDLE;
                } else {
                    lk.mode = LK_LOAD;
                    lk.t = plat_ms();
                    ui_stop();
                }
            }
        }
        o = lk_put(o, lk.mode == LK_RECV || lk.mode == LK_LOAD || (lk.obj >= LK_LIB && !st) ? lk.have : 0u, 3);
        break;
    case 6:
        if (n < 2u || !lk_valid(a) || a == LK_MEM) {
            st = 1;
        } else if (a >= LK_LIB) {
            st = plat_obj_delete(OBJ_LIB((int)(a - LK_LIB))) ? 4u : 0u;
            lib_forget();
        } else {
            st = song_delete_slot((int)a) ? 4u : 0u;
            ui_song_deleted((int)a);
        }
        lk.r_open = 0;
        o = lk_put(o, a, 2);
        break;
    default:
        st = 1;
    }
    link_rx_done();
    lk_out[5] = (uint8_t)st;
    lk_send(o);
}
