/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE in the browser: the whole app (UI, settings, tracker, the Mega Drive's chips, the song
 * store, the link) as WebAssembly, against a simulated FM-1 that web/emu.js drives; and the same
 * module as the web editor's engine (web/editor.js), which edits the song in place and plays it. The
 * same sources as the firmware and host/drive_host.c, built freestanding (no libc) by
 * host/build_web.sh into web/fm1drive.wasm. (FM1-GM's gm_web.c, reworked.)
 *
 * Emulator: the page sets the panel's state (buttons, keys, encoders, MASTER), queues MIDI in, and
 * calls web_run(ms) to run the device; it reads the screen (web_fb, RGB565 as the panel takes it),
 * the lights, MIDI out and the audio (stereo float, 44.1 kHz, web_audio_*). The flash (web_nor) is
 * 1 MiB the page keeps between visits.
 *
 * Editor: web_ed_init() instead of web_boot(): no UI, no flash, just the song (web_song, the layout
 * of its struct from web_layout) and the player. web_io is the buffer files go through. */
#define OM_HOST 1
#define OM_WEB 1
#include <stddef.h>
#include <stdint.h>
#define WEB(name) __attribute__((export_name(#name)))

/* ------------------------------------------------------------ libc --- */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = d;
    while (n--)
        *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d;
    const unsigned char *q = s;
    if (p < q)
        while (n--)
            *p++ = *q++;
    else
        while (n--)
            p[n] = q[n];
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];          /* RGB565, byte-swapped as the panel takes it (gfx.c) */
static uint32_t fb_gen;                 /* bumped on every draw: the page repaints when it changes */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    fb_gen++;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    fb_gen++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"
static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}
static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (s[i] && i + 1 < max) {
        d[i] = s[i];
        i++;
    }
    d[i] = 0;
}
static void fmt_fix(char *b, int32_t v, int decimals)
{
    char t[12];
    int n = 0, neg = v < 0, i;
    uint32_t u = (uint32_t)(neg ? -v : v);
    do {
        t[n++] = (char)('0' + u % 10u);
        u /= 10u;
        if (n == decimals)
            t[n++] = '.';
    } while (u || n <= decimals);
    if (neg)
        t[n++] = '-';
    for (i = 0; i < n; i++)
        b[i] = t[n - 1 - i];
    b[n] = 0;
}
static void fmt_int(char *b, int32_t v) { fmt_fix(b, v, 0); }
#include "../firmware/src/lz4.c"

/* ---------------------------------------------------------- platform --- */
#include "../firmware/src/app/plat.h"

static uint32_t now_ms, held_btn, held_keys, master = 2048;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys;
static int glow_on = 1, play_red;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role)
{
    int32_t v = enc_acc[role];
    enc_acc[role] = 0;
    return v;
}
uint32_t plat_master(void) { return master; }
int plat_battery(void) { return 4; }   /* the emulator runs on USB power */
void plat_leds(uint32_t b, uint32_t k)
{
    lit_btn = b;
    lit_keys = k;
}
void plat_glow(int on) { glow_on = on; }
void plat_play_red(int red) { play_red = red; }

#define MQ 256u                          /* USB-MIDI packets, both ways */
static uint32_t min_q[MQ], mi_w, mi_r, mout_q[MQ], mo_w, mo_r;
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
void plat_midi_out(uint32_t pkt)
{
    if (mo_w - mo_r < MQ)
        mout_q[mo_w++ % MQ] = pkt;
}

/* flash: a simulated NOR (erase -> 0xFF, programming clears bits) under the firmware's storage.c */
static uint8_t nor[0x100000];
static uint32_t store_writes;           /* the page saves the flash when this changes */
static const uint8_t flash_ok = 1;
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    memset(nor + off, 0xFF, 4096);
    store_writes++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *p = src;
    uint32_t i;
    for (i = 0; i < n; i++)
        nor[off + i] &= p[i];
    store_writes++;
    return 0;
}
#include "../firmware/src/storage.c"
#include "../firmware/src/app/plat_store.c"

static uint32_t cpu_pct;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }

/* -------------------------------------------------------------- app --- */
#include "../firmware/src/app/app.h"
#include "../firmware/src/trk/vgm.h"
song_t song;
#include "../firmware/src/app/settings.c"
#include "../firmware/src/app/ui.c"

/* SysEx (link.c, the web editor): one message in at a time, as usb.c keeps it; the answers wait in
 * sx_out until the page takes them */
static uint8_t sx_in[640], sx_out[16384];
static uint32_t sx_in_n, sx_out_n;
static int link_rx(const uint8_t **p, uint32_t *n)
{
    if (!sx_in_n)
        return 0;
    *p = sx_in;
    *n = sx_in_n;
    return 1;
}
static void link_rx_done(void) { sx_in_n = 0; }
static int link_tx(const uint8_t *p, uint32_t n)
{
    if (sx_out_n + n > sizeof sx_out)
        return -1;
    memcpy(sx_out + sx_out_n, p, n);
    sx_out_n += n;
    return 0;
}
#include "../firmware/src/app/link.c"

/* ------------------------------------------------------------ audio --- */
/* rendered frames wait here until the page takes them (web_audio_take) */
#define AQ 16384u                        /* frames, a power of two */
static float aq[AQ * 2];
static uint32_t aq_w, aq_r;
static float aout[AQ * 2];
static int32_t blk[512];

static void render_block(void)          /* 256 frames, as the device renders them (main_fm1.c) */
{
    uint32_t k;
    memset(blk, 0, sizeof blk);
    player_render(blk, 256);
    for (k = 0; k < 256; k++) {
        uint32_t w = (aq_w + k) & (AQ - 1u);
        aq[2 * w] = (float)(blk[2 * k] * (int32_t)master >> 4) / 8388608.0f;
        aq[2 * w + 1] = (float)(blk[2 * k + 1] * (int32_t)master >> 4) / 8388608.0f;
    }
    aq_w += 256;
    if (aq_w - aq_r > AQ)                /* nobody listening: keep the newest */
        aq_r = aq_w - AQ;
}

/* --------------------------------------------------------- the device --- */
static uint32_t audio_due_x256;         /* the next block, in 1/256 ms */
static uint32_t ui_due;

WEB(web_boot) void web_boot(int fresh)
{
    if (fresh)                           /* a new device: the flash erased (else the page loaded it) */
        memset(nor, 0xFF, sizeof nor);
    now_ms = 0;
    audio_due_x256 = 0;
    ui_due = 0;
    aq_w = aq_r = 0;
    memset(&set, 0, sizeof set);
    memset(&lk, 0, sizeof lk);
    sx_in_n = sx_out_n = 0;
    presets_bank_init();
    player_init(44100);
    settings_load();
    song_boot_load();
    lcd_fill(0, 0, 240, 240, 0);
    ui_init();
}

WEB(web_run) void web_run(uint32_t ms)
{
    uint32_t i;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due_x256 <= now_ms * 256u) {
            render_block();
            audio_due_x256 += 256u * 256u * 1000u / 44100u;
        }
        link_service();
        if (now_ms >= ui_due) {
            ui_due = now_ms + 16;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

/* the frames rendered and not yet taken, at most max, copied to web_audio_buf (L R L R ...) */
WEB(web_audio_take) uint32_t web_audio_take(uint32_t max)
{
    uint32_t n = aq_w - aq_r, i;
    if (n > max)
        n = max;
    if (n > AQ)
        n = AQ;
    for (i = 0; i < n; i++) {
        uint32_t r = (aq_r + i) & (AQ - 1u);
        aout[2 * i] = aq[2 * r];
        aout[2 * i + 1] = aq[2 * r + 1];
    }
    aq_r += n;
    return n;
}
WEB(web_audio_buf) float *web_audio_buf(void) { return aout; }

WEB(web_fb) uint16_t *web_fb(void) { return fb; }
WEB(web_fb_gen) uint32_t web_fb_gen(void) { return fb_gen; }
WEB(web_nor) uint8_t *web_nor(void) { return nor; }
WEB(web_nor_size) uint32_t web_nor_size(void) { return sizeof nor; }
WEB(web_store_writes) uint32_t web_store_writes(void) { return store_writes; }

WEB(web_set_buttons) void web_set_buttons(uint32_t b) { held_btn = b; }
WEB(web_set_keys) void web_set_keys(uint32_t k) { held_keys = k; }
WEB(web_turn) void web_turn(int e, int32_t n)
{
    if (e >= 0 && e < NE)
        enc_acc[e] += n;
}
WEB(web_set_master) void web_set_master(uint32_t m) { master = m > 4096u ? 4096u : m; }
WEB(web_lit_buttons) uint32_t web_lit_buttons(void) { return lit_btn; }
WEB(web_lit_keys) uint32_t web_lit_keys(void) { return lit_keys; }
WEB(web_glow) int web_glow(void) { return glow_on; }
WEB(web_play_red) int web_play_red(void) { return play_red; }

/* a channel or realtime message from the page's MIDI in (USB-MIDI packet, cable 0) */
WEB(web_midi_in) void web_midi_in(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t cin = st >= 0xF0u ? 0xFu : st >> 4;
    if (mi_w - mi_r < MQ)
        min_q[mi_w++ % MQ] = cin | st << 8 | d1 << 16 | d2 << 24;
}
/* the next MIDI out packet, 0 = none */
WEB(web_midi_out) uint32_t web_midi_out(void) { return mo_r == mo_w ? 0u : mout_q[mo_r++ % MQ]; }

WEB(web_cpu) uint32_t web_cpu(void) { return cpu_pct; }
WEB(web_set_cpu) void web_set_cpu(uint32_t p) { cpu_pct = p; }
WEB(web_view) int web_view(void) { return ui.view; }
WEB(web_playing) int web_playing(void) { return player.playing; }

/* a SysEx message from the page: the bytes between F0 and F7 written to web_sx_in_buf first. 0: one is
 * still waiting (send it again later) */
WEB(web_sx_in_buf) uint8_t *web_sx_in_buf(void) { return sx_in; }
WEB(web_sx_in) int web_sx_in(uint32_t n)
{
    if (sx_in_n || !n || n > sizeof sx_in)
        return 0;
    sx_in_n = n;
    return 1;
}
WEB(web_sx_busy) int web_sx_busy(void) { return sx_in_n != 0; }
/* the SysEx messages sent (F0 .. F7 each, one after another), web_sx_out_len bytes; web_sx_out_clear
 * once taken */
WEB(web_sx_out) uint8_t *web_sx_out(void) { return sx_out; }
WEB(web_sx_out_len) uint32_t web_sx_out_len(void) { return sx_out_n; }
WEB(web_sx_out_clear) void web_sx_out_clear(void) { sx_out_n = 0; }

/* ---------------------------------------------------------- the editor --- */
static uint8_t io[1u << 20];            /* files in and out: songs, instruments, VGM */
static uint32_t io_pos, io_len;

WEB(web_io) uint8_t *web_io(void) { return io; }
WEB(web_io_size) uint32_t web_io_size(void) { return sizeof io; }

static void io_put(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    while (n--) {
        if (io_len < sizeof io)
            io[io_len] = *b;
        io_len++;
        b++;
    }
}
static int io_get(void *ctx)
{
    (void)ctx;
    return io_pos < io_len ? io[io_pos++] : -1;
}

WEB(web_ed_init) void web_ed_init(void)
{
    aq_w = aq_r = 0;
    master = 4096;
    presets_bank_init();
    player_init(44100);
    song_init(&song);
    player_post(PC_RESET, 0, 0, 0);
}

WEB(web_song) song_t *web_song(void) { return &song; }

/* where song_t's and ins_t's fields are (the page reads and writes them in place) */
WEB(web_layout) uint32_t web_layout(int k)
{
    static const uint32_t L[] = {
        offsetof(song_t, title), offsetof(song_t, author), offsetof(song_t, region), offsetof(song_t, chip),
        offsetof(song_t, filter), offsetof(song_t, ext3), offsetof(song_t, tick_hz), offsetof(song_t, speed),
        offsetof(song_t, rows), offsetof(song_t, hl), offsetof(song_t, lfo), offsetof(song_t, nord),
        offsetof(song_t, loop), offsetof(song_t, order), offsetof(song_t, ins), sizeof(ins_t),
        offsetof(ins_t, type), offsetof(ins_t, name), offsetof(ins_t, u), offsetof(ins_t, mac),
        offsetof(fm_patch_t, op), sizeof(fm_op_t), sizeof(macro_t), NCH, MAX_ORD, MAX_PAT, MAX_ROWS, MAX_INS,
        sizeof(cell_t), MACRO_LEN, sizeof(song_t),
    };
    return k >= 0 && k < (int)(sizeof L / sizeof L[0]) ? L[k] : 0u;
}

/* a cell: 0 for an empty pattern (alloc: allocate it; 0 when the song has no room) */
WEB(web_cell) cell_t *web_cell(int ch, int pat, int row, int alloc)
{
    if (ch < 0 || ch >= NCH || pat < 0 || pat >= MAX_PAT || row < 0 || row >= song.rows)
        return 0;
    return alloc ? song_cell_w(&song, ch, pat, row) : song_cell(&song, ch, pat, row);
}
WEB(web_pat_empty) int web_pat_empty(int ch, int pat) { return song_pat_empty(&song, ch, pat); }
WEB(web_set_rows) int web_set_rows(int rows) { return song_set_rows(&song, rows); }
WEB(web_compact) void web_compact(void) { song_compact(&song); }
WEB(web_pool_free) uint32_t web_pool_free(void) { return song_pool_free(&song); }
WEB(web_ins_used) int web_ins_used(int i) { return song_ins_used(&song, i); }

/* a song file (.fm1d) in web_io: 0 loaded; else not a song (a new one instead) */
WEB(web_song_read) int web_song_read(uint32_t n)
{
    int rc;
    io_pos = 0;
    io_len = n;
    player_post(PC_STOP, 0, 0, 0);
    player_run_commands();
    rc = song_read(&song, io_get, 0);
    player_post(PC_RESET, 0, 0, 0);
    return rc;
}
/* the song as a file into web_io: its length (0: too long for it) */
WEB(web_song_write) uint32_t web_song_write(void)
{
    io_len = 0;
    song_write(&song, io_put, 0);
    return io_len <= sizeof io ? io_len : 0u;
}
WEB(web_song_new) void web_song_new(void)
{
    player_post(PC_STOP, 0, 0, 0);
    player_run_commands();
    song_init(&song);
    player_post(PC_RESET, 0, 0, 0);
}
WEB(web_song_demo) void web_song_demo(void)
{
    player_post(PC_STOP, 0, 0, 0);
    player_run_commands();
    demo_song(&song);
    player_post(PC_RESET, 0, 0, 0);
}

/* one instrument as a file into web_io (its length), or from it (0 = read) */
WEB(web_ins_write) uint32_t web_ins_write(int i)
{
    io_len = 0;
    if (i < 0 || i >= MAX_INS)
        return 0;
    ins_write(&song.ins[i], io_put, 0);
    return io_len;
}
WEB(web_ins_read) int web_ins_read(int i, uint32_t n)
{
    ins_t t;
    if (i < 0 || i >= MAX_INS)
        return -1;
    io_pos = 0;
    io_len = n;
    if (ins_read(&t, io_get, 0) != 0)
        return -1;
    song.ins[i] = t;
    player_post(PC_INS, (uint16_t)i, 0, 0);
    return 0;
}
/* a preset into the song's instrument i: kind 0 FM (0..127), 1 PSG (0..31), 2 DAC; 3: empty */
WEB(web_preset) void web_preset(int i, int kind, int n)
{
    if (i < 0 || i >= MAX_INS)
        return;
    if (kind == 0)
        preset_fm(n, &song.ins[i]);
    else if (kind == 1)
        preset_psg(n, &song.ins[i]);
    else if (kind == 2)
        preset_dac(n, &song.ins[i]);
    else
        ins_init(&song.ins[i], INS_EMPTY);
    player_post(PC_INS, (uint16_t)i, 0, 0);
}
WEB(web_preset_count) int web_preset_count(int kind) { return kind == 0 ? 128 : kind == 1 ? 32 : preset_dac_count(); }
WEB(web_ins_init) void web_ins_init(int i, int type)
{
    if (i >= 0 && i < MAX_INS) {
        ins_init(&song.ins[i], type);
        player_post(PC_INS, (uint16_t)i, 0, 0);
    }
}
WEB(web_sample_name) const char *web_sample_name(int i) { return i >= 0 && (uint32_t)i < bank_n ? bank[i].name : ""; }
WEB(web_sample_count) int web_sample_count(void) { return (int)bank_n; }
WEB(web_guard_trips) uint32_t web_guard_trips(void) { return player_guard_trips; }   /* the safety net (player_render) */
WEB(web_sample_data) const uint8_t *web_sample_data(int i) { return i >= 0 && (uint32_t)i < bank_n ? bank[i].data : 0; }
WEB(web_sample_len) uint32_t web_sample_len(int i) { return i >= 0 && (uint32_t)i < bank_n ? bank[i].len : 0; }
WEB(web_sample_rate) int web_sample_rate(int i) { return i >= 0 && (uint32_t)i < bank_n ? bank[i].rate : 0; }
WEB(web_is_carrier) int web_is_carrier(int alg, int op) { return ins_is_carrier(alg, op); }

/* the effects, by name, for the page's lists */
WEB(web_fx_name) const char *web_fx_name(int fx) { const char *s = fx_name((uint8_t)fx); return s ? s : ""; }
WEB(web_fx_next) int web_fx_next(int fx, int dir) { return fx_next((uint8_t)fx, dir); }

/* the player: commands (player.h's PC_*), the audio rendered on demand, where it is */
WEB(web_post) int web_post(int cmd, int a, int b, int c) { return player_post((uint8_t)cmd, (uint16_t)a, (uint8_t)b, (uint8_t)c); }
WEB(web_render) uint32_t web_render(uint32_t frames)    /* up to AQ frames into web_audio_buf */
{
    uint32_t n = 0;
    while (n + 256u <= frames && n + 256u <= AQ) {
        render_block();
        n += 256u;
    }
    return web_audio_take(n);
}
WEB(web_pos) uint32_t web_pos(void) { return (uint32_t)player.ord << 16 | player.row; }
WEB(web_ch_level) int web_ch_level(int ch) { return ch >= 0 && ch < NCH ? player.ch[ch].level : 0; }
WEB(web_scope) int8_t *web_scope(int ch) { return ch >= 0 && ch < MD_NCH ? player.md.scope[ch] : 0; }
WEB(web_scope_pos) uint32_t web_scope_pos(void) { return player.md.scope_pos; }
WEB(web_set_mute) void web_set_mute(uint32_t m)
{
    player_post(PC_MUTE, (uint16_t)(m & 0xFFFFu), (uint8_t)(m >> 16), 0);
}
WEB(web_stop_at_end) void web_stop_at_end(int on) { player.stop_at_end = (uint8_t)on; }
/* the song as a VGM file into web_io: its length, 0 when it did not fit */
WEB(web_vgm) uint32_t web_vgm(uint32_t max_s) { return vgm_export(io, sizeof io, max_s); }

/* the FM-1's library banks (link.c's objects 512 + n: "FDL1", then 16 instruments) */
static ins_t wbank[LIB_BANK];
WEB(web_bank_read) int web_bank_read(uint32_t n)        /* from web_io: 0 = read */
{
    int k;
    io_pos = 0;
    io_len = n;
    for (k = 0; k < LIB_BANK; k++)
        ins_init(&wbank[k], INS_EMPTY);
    if (n < 4u || memcmp(io, "FDL1", 4))
        return -1;
    io_pos = 4;
    for (k = 0; k < LIB_BANK; k++)
        if (ins_read(&wbank[k], io_get, 0) != 0)
            return -1;
    return 0;
}
WEB(web_bank_write) uint32_t web_bank_write(void)        /* into web_io: its length */
{
    int k;
    io_len = 0;
    io_put(0, (const uint8_t *)"FDL1", 4);
    for (k = 0; k < LIB_BANK; k++)
        ins_write(&wbank[k], io_put, 0);
    return io_len;
}
WEB(web_bank_ins) ins_t *web_bank_ins(int k) { return k >= 0 && k < LIB_BANK ? &wbank[k] : 0; }
WEB(web_bank_get) void web_bank_get(int k, int i)       /* bank slot k into the song's instrument i */
{
    if (k >= 0 && k < LIB_BANK && i >= 0 && i < MAX_INS) {
        song.ins[i] = wbank[k];
        player_post(PC_INS, (uint16_t)i, 0, 0);
    }
}
WEB(web_bank_put) void web_bank_put(int k, int i)       /* the song's instrument i into bank slot k */
{
    if (k >= 0 && k < LIB_BANK && i >= 0 && i < MAX_INS)
        wbank[k] = song.ins[i];
}
WEB(web_bank_clear) void web_bank_clear(void)
{
    int k;
    for (k = 0; k < LIB_BANK; k++)
        ins_init(&wbank[k], INS_EMPTY);
}
WEB(web_ins_changed) void web_ins_changed(int i) { player_post(PC_INS, (uint16_t)i, 0, 0); }
WEB(web_reset) void web_reset(void) { player_post(PC_RESET, 0, 0, 0); }
WEB(web_note_name) const char *web_note_name(int note)
{
    static char b[4];
    note_name((uint8_t)note, b);
    return b;
}
