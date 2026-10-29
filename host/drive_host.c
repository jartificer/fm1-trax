/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE on the host: the whole app (UI, settings, tracker, the Mega Drive's chips) against a
 * simulated FM-1 driven by a script. Audio goes to a WAV, the screen to PNGs, the lights to text. The
 * device-only parts (HAL, USB, OTA, flash driver) are replaced by plat.h implemented here. (FM1-GM's
 * gm_host, from FoMni's and X0X's.)
 *
 *   drive_host SCRIPT [OUTDIR]
 *
 * Script, one command per line (# comments):
 *   wait MS                      run the device for MS milliseconds
 *   press BTN | release BTN | tap BTN | hold BTN MS   (BTN: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY
 *                                REC OCT- OCT+)
 *   key K down|up | tapkey K     (K: 0..26 = F3..G5 at octave 0)
 *   turn ENC N [MS]              (ENC: SELECT ALGO PRESET K1 K2 K3 K4; N detents, MS apart, default 80)
 *   spin ENC N                   N detents at once, exactly N (no acceleration)
 *   master N                     MASTER pot 0..4096
 *   battery N                    the battery: 0..3 bars, 4 powered over USB (the default)
 *   midi B0 B1 B2                incoming USB MIDI message (hex bytes)
 *   wav FILE | wavstop           start / stop recording the output
 *   peakreset                    restart the output peak (expect peak_db_min / peak_db_max)
 *   shot FILE.png                save the screen
 *   leds                         print the lit buttons and keys
 *   expect WHAT VALUE            check state: exit 1 on mismatch; VALUE "<N" / ">N" is a bound
 *   reboot                       re-run boot from the simulated flash (persistence test)
 *   echo TEXT
 */
#define OM_HOST 1
#define _POSIX_C_SOURCE 199309L      /* clock_gettime under -std=c99 (Linux) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#define __attribute__(x)

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];          /* RGB565, byte-swapped as the panel takes it (gfx.c) */
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static uint32_t blits;
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"
static unsigned str_len(const char *s) { return (unsigned)strlen(s); }
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
static int battery = 4;
int plat_battery(void) { return battery; }
void plat_leds(uint32_t b, uint32_t k)
{
    lit_btn = b;
    lit_keys = k;
}
static int glow_on = 1;                 /* the device's button glow (plat_glow); the lights printed are the bright ones */
void plat_glow(int on) { glow_on = on; }
void plat_play_red(int red) { (void)red; }      /* the device's PLAY colour: green, red while recording */

#define MQ 256
static uint32_t min_q[MQ], mi_w, mi_r;
static FILE *midi_log;
static uint32_t midi_out_count, clock_out_count;
static int out_on_note = -1, out_on_chan = -1;   /* the last note on sent on MIDI out */
int plat_midi_in(uint32_t *pkt)
{
    if (mi_r == mi_w)
        return 0;
    *pkt = min_q[mi_r++ % MQ];
    return 1;
}
void plat_midi_out(uint32_t pkt)
{
    midi_out_count++;
    if (((pkt >> 8) & 0xFF) == 0xF8) {
        clock_out_count++;
        return;
    }
    if (((pkt >> 8) & 0xF0) == 0x90 && (pkt >> 24)) {
        out_on_note = (int)((pkt >> 16) & 0x7F);
        out_on_chan = (int)((pkt >> 8) & 0x0F) + 1;
    }
    if (midi_log)
        fprintf(midi_log, "%u ms: %02X %02X %02X\n", now_ms, (pkt >> 8) & 0xFF, (pkt >> 16) & 0xFF, (pkt >> 24) & 0xFF);
}

/* flash: a simulated NOR (erase -> 0xFF, programming clears bits), kept across a simulated reboot,
 * under the firmware's own storage.c */
static uint8_t nor[0x100000];
static uint32_t store_writes;                    /* sectors erased: one a save of an object, or a song's sector */
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
    if ((off & 0xFFu) + n > 256u) {
        fprintf(stderr, "flash: a program crosses a page at %#x\n", off);
        exit(1);
    }
    for (i = 0; i < n; i++)
        nor[off + i] &= p[i];
    return 0;
}
#include "../firmware/src/storage.c"
#include "../firmware/src/app/plat_store.c"

static uint32_t cpu_pct;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }

/* -------------------------------------------------------------- app --- */
#include "../firmware/src/app/app.h"
song_t song;
#include "../firmware/src/app/settings.c"
#include "../firmware/src/app/ui.c"
/* the SysEx link, fed by the script's "sysex" lines; its answers are printed */
static uint8_t sx_in[1024];
static uint32_t sx_n, sx_pending;
static int link_rx(const uint8_t **p, uint32_t *n)
{
    if (!sx_pending)
        return 0;
    *p = sx_in;
    *n = sx_n;
    return 1;
}
static void link_rx_done(void) { sx_pending = 0; }
static uint8_t sx_last[1024];
static uint32_t sx_last_n;
static int sx_quiet, sx_st[8];                  /* sx_st: each answer's status, by command (-1: none) */
static int link_tx(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    memcpy(sx_last, p, n < sizeof sx_last ? n : sizeof sx_last);
    sx_last_n = n;
    if (n >= 7u)
        sx_st[p[4] & 7u] = p[5];
    if (sx_quiet)
        return 0;
    printf("  sysex <");
    for (i = 0; i < n && i < 24u; i++)
        printf(" %02X", p[i]);
    printf(n > 24u ? " ... (%u bytes)\n" : "\n", n);
    return 0;
}
#include "../firmware/src/app/link.c"

/* ---------------------------------------------------------- outputs --- */
static void png_write(const char *path)
{
    /* RGB PNG with stored (uncompressed) deflate blocks: no zlib needed */
    static uint8_t raw[240 * (1 + 240 * 3)];
    FILE *f = fopen(path, "wb");
    uint32_t crc_t[256], i, j, k, n = 0, a = 1, b = 0, pos, len = sizeof raw;
    uint8_t hdr[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_t[i] = c;
    }
    for (j = 0; j < 240; j++) {
        raw[n++] = 0;
        for (i = 0; i < 240; i++) {
            uint16_t p = fb[j * 240 + i];
            p = (uint16_t)((p >> 8) | (p << 8));
            raw[n++] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            raw[n++] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            raw[n++] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    for (i = 0; i < len; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
#define PUT32(v) do { uint8_t q_[4] = {(uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)}; fwrite(q_, 1, 4, f); } while (0)
    fwrite(hdr, 1, 8, f);
    {
        uint8_t ih[17] = {'I', 'H', 'D', 'R', 0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(13u);
        fwrite(ih, 1, 17, f);
        for (i = 0; i < 17; i++)
            c = crc_t[(c ^ ih[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    {
        uint32_t nblk = (len + 65534) / 65535, zlen = 2 + len + nblk * 5 + 4, c = 0xFFFFFFFFu;
        uint8_t *z = malloc(zlen + 4), *q = z;
        memcpy(q, "IDAT", 4);
        q += 4;
        *q++ = 0x78;
        *q++ = 0x01;
        for (pos = 0; pos < len;) {
            uint32_t m = len - pos > 65535 ? 65535 : len - pos;
            *q++ = pos + m >= len ? 1 : 0;
            *q++ = (uint8_t)m;
            *q++ = (uint8_t)(m >> 8);
            *q++ = (uint8_t)~m;
            *q++ = (uint8_t)(~m >> 8);
            memcpy(q, raw + pos, m);
            q += m;
            pos += m;
        }
        *q++ = (uint8_t)(b >> 8);
        *q++ = (uint8_t)b;
        *q++ = (uint8_t)(a >> 8);
        *q++ = (uint8_t)a;
        PUT32(zlen);
        fwrite(z, 1, zlen + 4, f);
        for (i = 0; i < zlen + 4; i++)
            c = crc_t[(c ^ z[i]) & 255] ^ (c >> 8);
        PUT32(~c);
        free(z);
    }
    {
        uint8_t ie[4] = {'I', 'E', 'N', 'D'};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(0u);
        fwrite(ie, 1, 4, f);
        for (i = 0; i < 4; i++)
            c = crc_t[(c ^ ie[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    fclose(f);
}

static FILE *wav;
static uint32_t wav_frames;
static float peak_out;
static void wav_open(const char *path)
{
    static const uint8_t h[44] = {0};
    wav = fopen(path, "wb");
    if (wav)
        fwrite(h, 1, 44, wav);
    wav_frames = 0;
}
static void wav_close(void)
{
    uint32_t bytes = wav_frames * 4, v;
    if (!wav)
        return;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav);
    v = 36 + bytes;
    fwrite(&v, 4, 1, wav);
    fwrite("WAVEfmt ", 1, 8, wav);
    v = 16;
    fwrite(&v, 4, 1, wav);
    {
        uint16_t fmt[2] = {1, 2};
        uint32_t r[2] = {44100, 44100 * 4};
        uint16_t al[2] = {4, 16};
        fwrite(fmt, 2, 2, wav);
        fwrite(r, 4, 2, wav);
        fwrite(al, 2, 2, wav);
    }
    fwrite("data", 1, 4, wav);
    fwrite(&bytes, 4, 1, wav);
    fclose(wav);
    wav = 0;
}

/* --------------------------------------------------------- the device --- */
static double audio_due_ms, render_ns_total, render_budget_ns_total;
static uint32_t ui_due;
static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void boot(void)
{
    static int fresh = 1;
    if (fresh)                                   /* a new device: the flash erased */
        memset(nor, 0xFF, sizeof nor);
    fresh = 0;
    presets_bank_init();
    player_init(44100);
    settings_load();
    song_boot_load();
    lcd_fill(0, 0, 240, 240, 0);
    ui_init();
}

static void run_ms(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i, k;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due_ms <= (double)now_ms) {      /* 256-frame blocks, as the device renders them */
            double t0 = now_ns(), dt;
            memset(blk, 0, sizeof blk);
            player_render(blk, 256);
            for (k = 0; k < 512; k++)                 /* the master: as the device (main_fm1.c) */
                blk[k] = blk[k] * (int32_t)master >> 4;
#ifdef OM_WEB
            web_audio(blk, 256);
#endif
            dt = now_ns() - t0;
            render_ns_total += dt;
            render_budget_ns_total += 256.0 * 1e9 / 44100.0;
            cpu_pct = (uint32_t)(100.0 * dt / (256.0 * 1e9 / 44100.0));
            for (k = 0; k < 256; k++) {
                float l = (float)blk[2 * k] / 8388608.0f, r = (float)blk[2 * k + 1] / 8388608.0f;
                int16_t s[2];
                if (fabsf(l) > peak_out)
                    peak_out = fabsf(l);
                if (fabsf(r) > peak_out)
                    peak_out = fabsf(r);
                s[0] = (int16_t)(l * 32767.0f);
                s[1] = (int16_t)(r * 32767.0f);
                if (wav) {
                    fwrite(s, 2, 2, wav);
                    wav_frames++;
                }
            }
            audio_due_ms += 256.0 * 1000.0 / 44100.0;
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

/* ---------------------------------------------------------- script --- */
static const char *const BTN_N[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
                                      "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_N[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};

static int find_name(const char *const *t, int n, const char *s)
{
    int i;
    for (i = 0; i < n; i++)
        if (!strcmp(t[i], s))
            return i;
    return -1;
}

/* linkcopy SRC DST: what the web editor does, through the link: object SRC read whole (READ),
 * written to DST (WRITE, DATA, then the load once the player has stopped), and read back to compare */
static uint8_t lc_a[1u << 18], lc_b[1u << 18];
static uint32_t lc_bytes;
static int lc_send(const uint8_t *m, uint32_t n, uint32_t want)
{
    memset(sx_st, -1, sizeof sx_st);
    memcpy(sx_in, m, n);
    sx_n = n;
    sx_pending = 1;
    run_ms(10);
    return sx_st[want & 7u];
}

static int lc_read(uint32_t obj, uint8_t *out, uint32_t *len)
{
    uint32_t off = 0, total = 1;
    while (off < total) {
        uint8_t m[16] = {0x7D, 0x46, 0x44, 3};
        int st;
        lk_put(lk_put(m + 4, obj, 2), off, 3);
        if ((st = lc_send(m, 9, 0x43)) != 0)
            return st;
        total = lk_num(sx_last + 11, 3);
        if (total > sizeof lc_a)
            return -2;
        off += lk_unpack(out + off, sx_last + 14, sx_last_n - 15u, total - off);
    }
    *len = total;
    return 0;
}

static int linkcopy(uint32_t src, uint32_t dst)
{
    uint32_t na, nb, off;
    uint8_t m[16 + 600] = {0x7D, 0x46, 0x44, 4};
    int st, i;
    sx_quiet = 1;
    lc_bytes = 0;
    if ((st = lc_read(src, lc_a, &na)) != 0) {
        printf("  linkcopy: READ %u: status %d\n", src, st);
        goto out;
    }
    lk_put(lk_put(m + 4, dst, 2), na, 3);
    if ((st = lc_send(m, 9, 0x44)) != 0) {
        printf("  linkcopy: WRITE %u: status %d\n", dst, st);
        goto out;
    }
    for (off = 0; off < na; off += LK_CHUNK) {
        uint32_t k = na - off < LK_CHUNK ? na - off : LK_CHUNK;
        m[3] = 5;
        lk_put(m + 4, off, 3);
        if ((st = lc_send(m, 7u + lk_pack(m + 7, lc_a + off, k), 0x45)) != 0) {
            printf("  linkcopy: DATA at %u: status %d\n", off, st);
            goto out;
        }
    }
    for (i = 0; dst < LK_LIB && i < 400 && sx_st[7] < 0; i++)
        run_ms(10);                             /* a song: the load, once the player has stopped */
    if (dst < LK_LIB && (st = sx_st[7]) != 0) {
        printf("  linkcopy: load: status %d\n", st);
        goto out;
    }
    if (dst < LK_LIB)                           /* a song comes back as the song now in memory */
        dst = dst < LK_MEM ? dst : LK_MEM;
    if ((st = lc_read(dst, lc_b, &nb)) != 0 || nb != na || memcmp(lc_a, lc_b, na)) {
        printf("  linkcopy: read back %u: status %d, %u bytes of %u%s\n", dst, st, nb, na,
               st || nb != na ? "" : ", different");
        st = st ? st : -3;
        goto out;
    }
    lc_bytes = na;
out:
    sx_quiet = 0;
    return st;
}

static int expect(const char *what, const char *val)
{
    int got;
    if (!strcmp(what, "playing"))
        got = player.playing;
    else if (!strcmp(what, "edit"))
        got = ui.edit;
    else if (!strcmp(what, "view"))
        got = ui.view;
    else if (!strcmp(what, "row"))
        got = ui.row;
    else if (!strcmp(what, "ord"))
        got = ui.ord;
    else if (!strcmp(what, "ch"))
        got = ui.ch;
    else if (!strcmp(what, "ins"))
        got = ui.ins;
    else if (!strcmp(what, "oct"))
        got = set.oct;
    else if (!strcmp(what, "song"))
        got = set.song;
    else if (!strcmp(what, "dirty"))
        got = ui.dirty;
    else if (!strcmp(what, "theme"))
        got = set.theme;
    else if (!strcmp(what, "nord"))
        got = song.nord;
    else if (!strcmp(what, "rows"))
        got = song.rows;
    else if (!strcmp(what, "loop"))
        got = song.loop;
    else if (!strcmp(what, "play_ord"))
        got = player.ord;
    else if (!strcmp(what, "play_row"))
        got = player.row;
    else if (!strcmp(what, "mute"))
        got = (int)player.mute;
    else if (!strcmp(what, "free"))                 /* bytes free in the chunk store */
        got = (int)plat_obj_room();
    else if (!strcmp(what, "store_writes"))
        got = (int)store_writes;
    else if (!strcmp(what, "blk"))
        got = ui.blk;
    else if (!strcmp(what, "linkbytes"))            /* the bytes the last linkcopy moved (0: it failed) */
        got = (int)lc_bytes;
    else if (!strcmp(what, "sxst"))                 /* the last SysEx answer's status */
        got = sx_last_n >= 7u ? sx_last[5] : -1;
    else if (!strcmp(what, "fxmode"))
        got = ui.fxmode;
    else if (!strcmp(what, "page"))
        got = ui.i_page;
    else if (!strncmp(what, "slotused", 8))         /* slotusedS: 0..255 */
        got = slot_info[atoi(what + 8)].used;
    else if (!strncmp(what, "note", 4) || !strncmp(what, "cins", 4) || !strncmp(what, "cvol", 4) ||
             !strncmp(what, "cfx", 3)) {
        /* noteC.R / cinsC.R / cvolC.R / cfxC.R: the cell of channel C, row R, at the cursor's position */
        const char *p = what + (what[1] == 'f' ? 3 : 4);
        int c = atoi(p), r = atoi(strchr(p, '.') ? strchr(p, '.') + 1 : "0");
        const cell_t *x = song_cell_ro(&song, c, song.order[ui.ord][c], r);
        got = !x ? 0 : what[0] == 'n' ? x->note : what[1] == 'i' ? x->ins : what[1] == 'v' ? x->vol : x->fx[0];
    } else if (!strncmp(what, "instype", 7))        /* instypeI: the song's instrument I's kind */
        got = song.ins[atoi(what + 7)].type;
    else if (!strncmp(what, "alg", 3))              /* algI */
        got = song.ins[atoi(what + 3)].u.fm.alg;
    else if (!strncmp(what, "libused", 7))
        got = lib_used(atoi(what + 7));
    else if (!strcmp(what, "level")) {              /* the loudest channel's level now */
        int c;
        got = 0;
        for (c = 0; c < NCH; c++)
            if (player.ch[c].level > got)
                got = player.ch[c].level;
    }
    else if (!strcmp(what, "peak_db_max") || !strcmp(what, "peak_db_min")) {
        double db = 20.0 * log10(peak_out > 1e-9f ? peak_out : 1e-9f);
        int bad = what[8] == 'm' && what[9] == 'a' ? db > atof(val) : db < atof(val);
        printf("%s peak %.1f dBFS (%s %s)\n", bad ? "FAIL" : "  ok", db, what + 8, val);
        return bad;
    } else {
        printf("FAIL unknown expect %s\n", what);
        return 1;
    }
    if (val[0] == '<' || val[0] == '>') {
        int lim = (int)strtol(val + 1, 0, 0);
        if (val[0] == '<' ? got >= lim : got <= lim) {
            printf("FAIL expect %s %s, got %d\n", what, val, got);
            return 1;
        }
        printf("  ok %s == %d (%s)\n", what, got, val);
        return 0;
    }
    if (got != (int)strtol(val, 0, 0)) {
        printf("FAIL expect %s == %s, got %d\n", what, val, got);
        return 1;
    }
    printf("  ok %s == %d\n", what, got);
    return 0;
}

#ifndef OM_WEB
int main(int argc, char **argv)
{
    FILE *sc;
    char line[512], out[400];
    const char *dir = argc > 2 ? argv[2] : ".";
    int lineno = 0, fails = 0;
    if (argc < 2) {
        fprintf(stderr, "usage: drive_host SCRIPT [OUTDIR]\n");
        return 2;
    }
    sc = fopen(argv[1], "r");
    if (!sc) {
        perror(argv[1]);
        return 2;
    }
    snprintf(out, sizeof out, "%s/midi_out.txt", dir);
    midi_log = fopen(out, "w");
    boot();
    while (fgets(line, sizeof line, sc)) {
        char cmd[32] = {0}, a[256] = {0}, b[64] = {0}, c[64] = {0};
        lineno++;
        if (line[0] == '#' || sscanf(line, "%31s %255s %63s %63s", cmd, a, b, c) < 1)
            continue;
        if (!strcmp(cmd, "wait"))
            run_ms((uint32_t)atoi(a));
        else if (!strcmp(cmd, "hold")) {             /* hold BTN MS */
            int i = find_name(BTN_N, NB, a);
            if (i < 0) {
                printf("line %d: no button %s\n", lineno, a);
                return 2;
            }
            held_btn |= 1u << i;
            run_ms((uint32_t)atoi(b));
            held_btn &= ~(1u << i);
            run_ms(40);
        } else if (!strcmp(cmd, "press") || !strcmp(cmd, "release") || !strcmp(cmd, "tap")) {
            int i = find_name(BTN_N, NB, a);
            if (i < 0) {
                printf("line %d: no button %s\n", lineno, a);
                return 2;
            }
            if (cmd[0] != 'r')
                held_btn |= 1u << i;
            if (cmd[0] == 't')
                run_ms(40);
            if (cmd[0] != 'p')
                held_btn &= ~(1u << i);
            run_ms(cmd[0] == 't' ? 40 : 2);
        } else if (!strcmp(cmd, "key") || !strcmp(cmd, "tapkey")) {
            int k = atoi(a) % NKEYS;
            if (cmd[0] == 't') {
                held_keys |= 1u << k;
                run_ms(60);
                held_keys &= ~(1u << k);
                run_ms(30);
            } else {
                if (!strcmp(b, "down"))
                    held_keys |= 1u << k;
                else
                    held_keys &= ~(1u << k);
                run_ms(2);
            }
        } else if (!strcmp(cmd, "turn")) {
            int e = find_name(ENC_N, NE, a), n = atoi(b), s = n > 0 ? 1 : -1;
            uint32_t gap = c[0] ? (uint32_t)atoi(c) : 80u;
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            for (; n; n -= s) {
                enc_acc[e] += s;
                run_ms(gap);
            }
        } else if (!strcmp(cmd, "spin")) {
            int e = find_name(ENC_N, NE, a);
            if (e < 0) {
                printf("line %d: no encoder %s\n", lineno, a);
                return 2;
            }
            enc_acc[e] += atoi(b);
            accel_off = 1;
            run_ms(1);
            accel_off = 0;
        } else if (!strcmp(cmd, "master"))
            master = (uint32_t)atoi(a);
        else if (!strcmp(cmd, "battery"))
            battery = atoi(a);
        else if (!strcmp(cmd, "midi")) {
            uint32_t s = (uint32_t)strtoul(a, 0, 16), d1 = (uint32_t)strtoul(b, 0, 16), d2 = (uint32_t)strtoul(c, 0, 16);
            uint32_t cin = s >= 0xF0 ? 0x0F : s >> 4;
            min_q[mi_w++ % MQ] = cin | s << 8 | d1 << 16 | d2 << 24;
        } else if (!strcmp(cmd, "wav")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            wav_open(out);
        } else if (!strcmp(cmd, "wavstop"))
            wav_close();
        else if (!strcmp(cmd, "peakreset"))
            peak_out = 0.0f;
        else if (!strcmp(cmd, "shot")) {
            snprintf(out, sizeof out, "%s/%s", dir, a);
            png_write(out);
        } else if (!strcmp(cmd, "leds")) {
            int i;
            printf("  leds: buttons");
            for (i = 0; i < NB; i++)
                if (lit_btn >> i & 1u)
                    printf(" %s", BTN_N[i]);
            printf(" | keys ");
            for (i = 0; i < NKEYS; i++)
                printf("%c", (lit_keys >> i & 1u) ? '#' : '.');
            printf("\n");
        } else if (!strcmp(cmd, "expect"))
            fails += expect(a, b);
        else if (!strcmp(cmd, "reboot")) {
            memset(&set, 0, sizeof set);
            memset(&song, 0xA5, sizeof song);           /* RAM is lost: everything from the flash */
            boot();
        } else if (!strcmp(cmd, "sysex")) {         /* sysex HEX HEX ...: the bytes between F0 and F7 */
            char *q = line + 6;
            sx_n = 0;
            while (*q && sx_n < sizeof sx_in) {
                char *e;
                unsigned long v = strtoul(q, &e, 16);
                if (e == q)
                    break;
                sx_in[sx_n++] = (uint8_t)v;
                q = e;
            }
            sx_pending = 1;
            run_ms(20);
        } else if (!strcmp(cmd, "linkcopy")) {      /* linkcopy SRC DST: objects as link.c numbers them */
            if (linkcopy((uint32_t)atoi(a), (uint32_t)atoi(b)) != 0) {
                printf("line %d: linkcopy %s %s failed\n", lineno, a, b);
                fails++;
            } else {
                printf("  ok linkcopy %s %s (%u bytes)\n", a, b, lc_bytes);
            }
        } else if (!strcmp(cmd, "echo"))
            printf("%s", line + 5);
        else {
            printf("line %d: unknown command %s\n", lineno, cmd);
            return 2;
        }
    }
    wav_close();
    if (midi_log)
        fclose(midi_log);
    printf("host: %u ms simulated, %u blits, render %.1f%% of real time (host CPU)%s\n", now_ms, blits,
           render_budget_ns_total > 0 ? 100.0 * render_ns_total / render_budget_ns_total : 0.0,
           fails ? ", EXPECTATIONS FAILED" : "");
    return fails ? 1 : 0;
}
#endif /* OM_WEB */
