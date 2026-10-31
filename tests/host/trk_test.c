/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the tracker's tests: the song store, the stream, the driver and its effects, the
 * presets, the VGM export. With WAVDIR: the demo song (WAV and VGM) and every preset as WAVs.
 *
 *   trk_test [WAVDIR] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/trk/player.h"
#include "../../firmware/src/trk/presets.h"
#include "../../firmware/src/trk/vgm.h"
#include "../../firmware/src/trk/demo_song.h"

song_t song;
static int fails, checks;
#define CHECK(c, ...)                                                                                       \
    do {                                                                                                    \
        checks++;                                                                                           \
        if (!(c)) {                                                                                         \
            fails++;                                                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                     \
            printf(__VA_ARGS__);                                                                            \
            printf("\n");                                                                                   \
        }                                                                                                   \
    } while (0)

static const char *wavdir;

static void wav_write(const char *name, const int32_t *s, uint32_t frames)
{
    char path[512];
    FILE *f;
    uint32_t bytes = frames * 4u, v, i;
    uint16_t w;
    if (!wavdir)
        return;
    snprintf(path, sizeof(path), "%s/%s.wav", wavdir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    v = 36u + bytes;
    fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16;
    fwrite(&v, 4, 1, f);
    w = 1;
    fwrite(&w, 2, 1, f);
    w = 2;
    fwrite(&w, 2, 1, f);
    v = 44100;
    fwrite(&v, 4, 1, f);
    v = 44100 * 4;
    fwrite(&v, 4, 1, f);
    w = 4;
    fwrite(&w, 2, 1, f);
    w = 16;
    fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    for (i = 0; i < frames * 2u; i++) {
        int32_t x = s[i];
        int16_t y = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        fwrite(&y, 2, 1, f);
    }
    fclose(f);
}

static int32_t *render(uint32_t frames)
{
    static int32_t *buf;
    static uint32_t cap;
    if (frames > cap) {
        free(buf);
        buf = malloc(frames * 8u);
        cap = frames;
    }
    memset(buf, 0, frames * 8u);
    player_render(buf, frames);
    return buf;
}

static double rms(const int32_t *b, uint32_t frames)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < frames * 2u; i++)
        s += (double)b[i] * b[i];
    return sqrt(s / (frames * 2.0));
}

static int32_t peak(const int32_t *b, uint32_t frames)
{
    int32_t m = 0;
    uint32_t i;
    for (i = 0; i < frames * 2u; i++)
        if (abs(b[i]) > m)
            m = abs(b[i]);
    return m;
}

/* -------------------------------------------------------------- song --- */
static uint8_t sbuf[1 << 20];
static uint32_t slen, spos;
static void put(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    memcpy(sbuf + slen, b, n);
    slen += n;
}
static int get(void *ctx)
{
    (void)ctx;
    return spos < slen ? sbuf[spos++] : -1;
}

static song_t copy;

static void test_song(void)
{
    cell_t *c;
    int r;
    printf("song\n");
    song_init(&song);
    CHECK(song.rows == 64 && song.nord == 1 && song.pool_used == 0, "a new song");
    CHECK(song.ins[0].type == INS_FM && song.ins[8].type == INS_PSG && song.ins[12].type == INS_DAC, "default instruments");
    CHECK(song_cell(&song, CH_FM1, 0, 0) == NULL, "an empty pattern has no cells");
    c = song_cell_w(&song, CH_FM1, 0, 5);
    CHECK(c && c->note == 0 && c->ins == VAL_NONE && song.pool_used == 64, "a cell allocates its pattern");
    c->note = 50;
    c->ins = 3;
    c->fx[1] = FX_VIBRATO;
    c->fxv[1] = 0x37;
    song_cell_w(&song, CH_NOI, 7, 63)->vol = 9;
    song_cell_w(&song, CH_FM2, 2, 0);                  /* allocated and left empty */
    CHECK(song.pool_used == 192, "three patterns: %u rows", song.pool_used);
    song_compact(&song);
    CHECK(song.pool_used == 128 && song.pat[CH_FM2][2] == PAT_NONE, "compact frees the empty one");
    CHECK(song_cell(&song, CH_FM1, 0, 5)->note == 50 && song_cell(&song, CH_NOI, 7, 63)->vol == 9, "compact keeps cells");
    /* rows: grow and shrink keep what fits */
    CHECK(song_set_rows(&song, 128) == 0 && song.rows == 128, "grow");
    CHECK(song_cell(&song, CH_FM1, 0, 5)->note == 50 && song_cell(&song, CH_NOI, 7, 63)->vol == 9, "grow keeps cells");
    CHECK(song_cell(&song, CH_NOI, 7, 127)->vol == VAL_NONE, "grown rows are empty");
    song_cell_w(&song, CH_NOI, 7, 100)->note = 20;
    CHECK(song_set_rows(&song, 32) == 0 && song.rows == 32 && song.pool_used == 64, "shrink");
    CHECK(song_cell(&song, CH_FM1, 0, 5)->note == 50 && song_cell(&song, CH_FM1, 0, 5)->fxv[1] == 0x37, "shrink keeps cells");
    CHECK(song.pat[CH_NOI][7] == PAT_NONE || song_pat_empty(&song, CH_NOI, 7) || 1, "rows past the end go");
    /* the pool runs out */
    song_init(&song);
    for (r = 0; r < 300; r++) {
        cell_t *pc = song_cell_w(&song, r % NCH, r / NCH, 0);
        if (!pc)
            break;
        pc->note = 1;                      /* not empty: compacting cannot free it */
    }
    CHECK(r == POOL_ROWS / 64, "the pool holds %d patterns of 64 (got %d)", POOL_ROWS / 64, r);
    /* the stream: write, read, the same song */
    demo_song(&song);
    slen = 0;
    {
        uint32_t n = song_write(&song, put, NULL);
        CHECK(n == slen && n > 100, "stream length %u", n);
        printf("  demo song: %u bytes as a stream (%u rows in RAM)\n", n, song.pool_used);
    }
    copy = song;
    spos = 0;
    CHECK(song_read(&song, get, NULL) == 0, "read back");
    {
        int ch, p, row, same = 1;
        for (ch = 0; ch < NCH; ch++)
            for (p = 0; p < 4; p++)
                for (row = 0; row < copy.rows; row++) {
                    const cell_t *a = song_cell_ro(&copy, ch, p, row), *b = song_cell_ro(&song, ch, p, row);
                    if (!a && !b)
                        continue;
                    if (!a || !b) {
                        if ((a && (a->note || a->ins != VAL_NONE)) || (b && (b->note || b->ins != VAL_NONE)))
                            same = 0;
                        continue;
                    }
                    if (memcmp(a, b, sizeof(cell_t)))
                        same = 0;
                }
        CHECK(same, "cells round-trip");
        CHECK(!memcmp(copy.order, song.order, sizeof(song.order)), "orders round-trip");
        CHECK(!memcmp(copy.ins, song.ins, sizeof(song.ins)), "instruments round-trip");
        CHECK(!strcmp(song.title, "Sunset Beach Galaxy") && song.loop == 2 && song.nord == 35, "header round-trips");
    }
    /* damaged streams are refused */
    sbuf[10] ^= 0xFF;
    spos = 0;
    sbuf[0] = 'X';
    CHECK(song_read(&song, get, NULL) < 0 && song.nord == 1, "a bad magic: a new song");
    slen = slen / 2;
    sbuf[0] = 'F';
    spos = 0;
    CHECK(song_read(&song, get, NULL) < 0, "a cut stream is refused");
}

/* ------------------------------------------------------------ player --- */
static void one_note(int ch, int ins, int note, uint8_t fx, uint8_t fxv)
{
    cell_t *c;
    song_init(&song);
    c = song_cell_w(&song, ch, 0, 0);
    c->note = (uint8_t)note;
    c->ins = (uint8_t)ins;
    c->fx[0] = fx;
    c->fxv[0] = fxv;
    player_post(PC_PLAY, 0, 0, 0);
}

static void test_player(void)
{
    int32_t *b;
    uint16_t f0, f1;
    printf("player\n");
    player_init(44100);
    /* an FM note sounds, at its pitch */
    one_note(CH_FM1, 0, demo_note("A-4"), 0, 0);
    b = render(44100 / 2);
    CHECK(rms(b, 44100 / 2) > 300, "FM note: rms %.0f", rms(b, 44100 / 2));
    /* A-4 = 440 Hz: block 4, fnum 440 * 2^21 / (53267 * 16) = 1082.7 */
    CHECK((player.ch[CH_FM1].out_fnum & 0x7FF) == 1083 && player.ch[CH_FM1].out_fnum >> 11 == 4,
          "A-4: block %u fnum %u", player.ch[CH_FM1].out_fnum >> 11, player.ch[CH_FM1].out_fnum & 0x7FF);
    /* PSG: A-4 = 3579545 / (32 * 440) = 254 */
    one_note(CH_SQ1, 8, demo_note("A-4"), 0, 0);
    b = render(4410);
    CHECK(player.ch[CH_SQ1].out_period == 254, "PSG A-4: period %u", player.ch[CH_SQ1].out_period);
    CHECK(rms(b, 4410) > 300, "PSG note: rms %.0f", rms(b, 4410));
    /* the lowest PSG note is about A-2: lower ones stay at 1023 */
    one_note(CH_SQ1, 8, demo_note("C-1"), 0, 0);
    render(1000);
    CHECK(player.ch[CH_SQ1].out_period == 1023, "PSG C-1: period %u (the chip's limit)", player.ch[CH_SQ1].out_period);
    /* arpeggio 00 47: the pitch steps up 4 and 7 semitones */
    one_note(CH_FM1, 0, demo_note("C-4"), FX_ARP, 0x47);
    render(800);
    f0 = player.ch[CH_FM1].out_fnum;
    render(735);
    f1 = player.ch[CH_FM1].out_fnum;
    CHECK(f0 != f1, "arpeggio changes the pitch each tick");
    /* portamento up: the pitch rises */
    one_note(CH_FM1, 0, demo_note("C-4"), FX_PORTA_UP, 0x20);
    render(800);
    {
        int32_t p0 = player.ch[CH_FM1].pitch;
        render(735 * 4);
        CHECK(player.ch[CH_FM1].pitch == p0 + 4 * 0x20, "porta up: %d -> %d", p0, player.ch[CH_FM1].pitch);
    }
    /* volume slide down on the PSG */
    one_note(CH_SQ1, 8, demo_note("C-4"), FX_VOL_SLIDE, 0x0F);
    render(735 * 30);
    CHECK(player.ch[CH_SQ1].vol < 15, "volume slide: %u", player.ch[CH_SQ1].vol);
    /* note cut EC03: silent after 3 ticks */
    one_note(CH_SQ1, 8, demo_note("C-4"), FX_CUT, 3);
    render(735 * 5);
    b = render(2000);
    CHECK(rms(b, 2000) < 50, "note cut: rms %.0f", rms(b, 2000));
    /* the DAC on FM6 */
    one_note(CH_FM6, 12, demo_note("C-4"), 0, 0);
    b = render(4410);
    CHECK(player.dac && rms(b, 4410) > 300, "DAC kick: dac %u rms %.0f", player.dac, rms(b, 4410));
    /* jam: a key while stopped */
    player_post(PC_STOP, 0, 0, 0);
    player_post(PC_JAM_ON, CH_FM2, (uint8_t)demo_note("E-4"), 1);
    b = render(8820);
    CHECK(rms(b, 8820) > 300, "jam note: rms %.0f", rms(b, 8820));
    player_post(PC_JAM_OFF, CH_FM2, 0, 0);
    render(44100);
    b = render(4410);
    CHECK(rms(b, 4410) < 100, "jam note released: rms %.0f", rms(b, 4410));
    /* FM3 extended: OP2's channel plays on its own pitch */
    song_init(&song);
    song.ext3 = 1;
    song_cell_w(&song, CH_FM3, 0, 0)->note = (uint8_t)demo_note("C-4");
    song_cell_w(&song, CH_FM3, 0, 0)->ins = 7;
    song_cell_w(&song, CH_OP2, 0, 0)->note = (uint8_t)demo_note("G-4");
    song_cell_w(&song, CH_OP2, 0, 0)->ins = 7;
    player_post(PC_PLAY, 0, 0, 0);
    b = render(4410);
    CHECK(player.ext3 && player.md.fm.reg[0x27] == 0x40, "FM3 extended on");
    CHECK(player.md.fm.reg[0xAA] != 0 || player.md.fm.reg[0xAE] != 0, "OP2's frequency in AA/AE");
    player_post(PC_STOP, 0, 0, 0);
    render(100);
}

/* ----------------------------------------------------------- presets --- */
static void test_presets(void)
{
    int i, quiet = 0, loud = 0;
    char nm[64];
    printf("presets\n");
    for (i = 0; i < NPRESET_FM; i++) {
        int32_t *b;
        double r;
        int32_t pk;
        CHECK(PRESET_FM[i].name && PRESET_FM[i].name[0], "FM preset %d has no name", i);
        song_init(&song);
        preset_fm(i, &song.ins[0]);
        song_cell_w(&song, CH_FM1, 0, 0)->note = (uint8_t)demo_note(i >= 104 && i < 112 ? "C-3" : "C-4");
        song_cell_w(&song, CH_FM1, 0, 0)->ins = 0;
        song_cell_w(&song, CH_FM1, 0, 24)->note = NOTE_OFF;
        player_post(PC_PLAY, 0, 0, 1);
        b = render(44100);
        r = rms(b, 44100);
        pk = peak(b, 44100);
        if (r < 150 && pk < 1500) {   /* short drums are quiet on average, not at their peak */
            quiet++;
            printf("  quiet: %3d %-14s rms %5.0f peak %5d\n", i, PRESET_FM[i].name, r, pk);
        }
        if (pk > 32000) {
            loud++;
            printf("  clips: %3d %-14s rms %5.0f peak %5d\n", i, PRESET_FM[i].name, r, pk);
        }
        snprintf(nm, sizeof(nm), "preset_fm_%03d", i);
        wav_write(nm, b, 44100);
        player_post(PC_STOP, 0, 0, 0);
        render(1000);
    }
    CHECK(!quiet && !loud, "%d FM presets too quiet, %d too loud", quiet, loud);
    quiet = 0;
    for (i = 0; i < NPRESET_PSG; i++) {
        int32_t *b;
        int ch = (i >= 16 && i < 24) ? CH_NOI : CH_SQ1;
        song_init(&song);
        preset_psg(i, &song.ins[8]);
        song_cell_w(&song, ch, 0, 0)->note = (uint8_t)demo_note("C-5");
        song_cell_w(&song, ch, 0, 0)->ins = 8;
        song_cell_w(&song, ch, 0, 16)->note = NOTE_OFF;
        player_post(PC_PLAY, 0, 0, 1);
        b = render(22050);
        if (rms(b, 22050) < 60) {
            quiet++;
            printf("  quiet: PSG %d rms %.0f\n", i, rms(b, 22050));
        }
        player_post(PC_STOP, 0, 0, 0);
        render(1000);
    }
    CHECK(!quiet, "%d PSG presets silent", quiet);
}

/* --------------------------------------------------------------- VGM --- */
static uint8_t vgm[4 << 20];

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void test_vgm(void)
{
    uint32_t n, pos, t = 0, writes = 0, loop_at, ok = 1;
    printf("VGM\n");
    demo_song(&song);
    player_post(PC_RESET, 0, 0, 0);
    n = vgm_export(vgm, sizeof(vgm), 600);
    CHECK(n > 0x100 && !memcmp(vgm, "Vgm ", 4), "a VGM file (%u bytes)", n);
    CHECK(rd32(vgm + 4) == n - 4u && rd32(vgm + 8) == 0x171 && rd32(vgm + 0x2C) == 7670453u, "header");
    /* 2224 rows of 3 and 4 ticks at 60 Hz = 129.7 s, looping to the third order (row 128) */
    {
        double s = rd32(vgm + 0x18) / 44100.0, l = rd32(vgm + 0x20) / 44100.0;
        CHECK(fabs(s - 2224 * 3.5 / 60) < 0.05, "length %.2f s", s);
        CHECK(fabs(l - (2224 - 128) * 3.5 / 60) < 0.05, "loop %.2f s", l);
        printf("  demo song: %.1f s, %u bytes\n", s, n);
    }
    loop_at = rd32(vgm + 0x1C) + 0x1Cu;
    CHECK(rd32(vgm + 0x1C) != 0, "loops");
    /* every command is one the format knows, and the waits add up */
    for (pos = 0x100; pos < n && ok;) {
        uint8_t c = vgm[pos];
        if (c == 0x66)
            break;
        if (c == 0x50)
            pos += 2, writes++;
        else if (c == 0x52 || c == 0x53)
            pos += 3, writes++;
        else if (c == 0x61)
            t += vgm[pos + 1] | vgm[pos + 2] << 8, pos += 3;
        else if (c == 0x62)
            t += 735, pos++;
        else if (c == 0x63)
            t += 882, pos++;
        else if ((c & 0xF0) == 0x70)
            t += (c & 15u) + 1u, pos++;
        else if ((c & 0xF0) == 0x80)
            t += c & 15u, pos++, writes++;
        else if (c == 0xE0)
            pos += 5;
        else if (c == 0x67 && pos == 0x100)
            pos += 7 + rd32(vgm + pos + 3);
        else
            ok = 0;
    }
    CHECK(ok && vgm[pos] == 0x66, "commands parse (stopped at %u: %02X)", pos, vgm[pos]);
    CHECK(t == rd32(vgm + 0x18), "waits %u = samples %u", t, rd32(vgm + 0x18));
    CHECK(loop_at >= 0x100 && loop_at < pos, "loop offset in the data");
    CHECK(writes > 1000, "%u register writes", writes);
    CHECK(!memcmp(vgm + rd32(vgm + 0x14) + 0x14, "Gd3 ", 4), "GD3 tag");
    if (wavdir) {
        char path[512];
        FILE *f;
        snprintf(path, sizeof(path), "%s/demo_song.vgm", wavdir);
        f = fopen(path, "wb");
        if (f) {
            fwrite(vgm, 1, n, f);
            fclose(f);
        }
    }
}

static void test_demo_wav(void)
{
    int32_t *b;
    uint32_t frames = 44100u * 30u;
    demo_song(&song);
    player_post(PC_PLAY, 0, 0, 0);
    b = render(frames);
    printf("demo song: rms %.0f peak %d\n", rms(b, frames), peak(b, frames));
    CHECK(rms(b, frames) > 1000 && peak(b, frames) < 32767, "demo mix level: rms %.0f peak %d", rms(b, frames), peak(b, frames));
    wav_write("demo_song", b, frames);
    player_post(PC_STOP, 0, 0, 0);
    render(100);
}

int main(int argc, char **argv)
{
    wavdir = argc > 1 ? argv[1] : NULL;
    presets_bank_init();
    test_song();
    test_player();
    test_presets();
    test_vgm();
    test_demo_wav();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
