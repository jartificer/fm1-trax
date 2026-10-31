/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: fuzzing the sound path for loud-noise bugs. Random songs (instruments, macros, every
 * effect with any value, any timing), random live commands between the blocks (play, stop, the
 * keys, mute, reset, instrument edits), and damaged song streams through song_read, all rendered
 * by the player and the chips. Every sample must stay inside the range the device scales to its
 * 24-bit output without wrapping, and a stop must go quiet. The keys are fuzzed for hanging notes too:
 * once every key is let go, nothing may go on sounding (player_hanging).
 *
 * The safety net itself (player_render) is tested first: a poisoned engine trips it once, goes
 * silent, and the keys play again.
 *
 *   fuzz_test [SEEDS] [FIRST]     (default 300 seeds from 1) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/trk/player.h"
#include "../../firmware/src/trk/presets.h"
#include "../../firmware/src/trk/demo_song.h"

song_t song;

/* the device: chips x master (Q12, up to ~4096) >> 4 into 24 bits, so the chips' mix must stay
 * within 16 bits (main_fm1.c, render_block); the editor divides by 32768 into floats */
#define LIMIT 32767
#define BLOCK 256u
#define QUIET 64                       /* the mix's peak 200 ms after a stop (the DC blocker settles) */

static uint32_t rng;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static int rn(int n) { return n > 0 ? (int)(rnd() % (uint32_t)n) : 0; }

static int fails;
static int32_t worst;
static uint32_t clipped;
static uint32_t frames_total;
static char where[160];

static int32_t block_peak(void)
{
    static int32_t b[2 * BLOCK];
    int32_t m = 0;
    uint32_t i;
    memset(b, 0, sizeof b);
    player_render(b, BLOCK);
    for (i = 0; i < 2 * BLOCK; i++) {
        int32_t v = b[i] < 0 ? -b[i] : b[i];
        if (v > m)
            m = v;
    }
    frames_total += BLOCK;
    if (player_raw_peak > worst)
        worst = player_raw_peak;
    if (player_raw_peak > LIMIT)
        clipped++;
    if (m > LIMIT && fails < 20) {             /* past the clip: the safety net is not in the path */
        fails++;
        printf("FAIL %s: peak %d (limit %d)\n", where, m, LIMIT);
    }
    return m;
}

static void rnd_macro(macro_t *m, int lo, int hi)
{
    int i;
    m->len = (uint8_t)(rn(3) ? 0 : 1 + rn(MACRO_LEN));
    m->loop = (uint8_t)(rn(2) && m->len ? rn(m->len) : MACRO_NONE);
    m->rel = (uint8_t)(rn(3) == 0 && m->len ? rn(m->len) : MACRO_NONE);
    for (i = 0; i < MACRO_LEN; i++)
        m->v[i] = (int8_t)(lo + rn(hi - lo + 1));
}

static void rnd_ins(ins_t *in)
{
    int t = rn(8), k;
    ins_init(in, t < 4 ? INS_FM : t < 6 ? INS_PSG : t < 7 ? INS_DAC : INS_EMPTY);
    if (in->type == INS_FM) {
        fm_patch_t *p = &in->u.fm;
        if (rn(2))
            preset_fm(rn(128), in);
        else {
            p->alg = (uint8_t)rn(8);
            p->fb = (uint8_t)rn(8);
            p->ams = (uint8_t)rn(4);
            p->pms = (uint8_t)rn(8);
            for (k = 0; k < 4; k++) {
                fm_op_t *o = &p->op[k];
                o->ar = (uint8_t)rn(32);
                o->d1r = (uint8_t)rn(32);
                o->d2r = (uint8_t)rn(32);
                o->rr = (uint8_t)rn(16);
                o->sl = (uint8_t)rn(16);
                o->tl = (uint8_t)(rn(3) ? rn(128) : 0);   /* often at full level */
                o->ks = (uint8_t)rn(4);
                o->mul = (uint8_t)rn(16);
                o->dt = (uint8_t)rn(8);
                o->am = (uint8_t)rn(2);
                o->ssg = (uint8_t)(rn(3) ? 0 : 8 + rn(8));
            }
        }
        rnd_macro(&in->mac[MAC_ARP], -48, 48);
        rnd_macro(&in->mac[MAC_PITCH], -128, 127);
    } else if (in->type == INS_PSG) {
        if (rn(2))
            preset_psg(rn(32), in);
        rnd_macro(&in->mac[MAC_VOL], 0, 15);
        rnd_macro(&in->mac[MAC_ARP], -48, 48);
        rnd_macro(&in->mac[MAC_PITCH], -128, 127);
        rnd_macro(&in->mac[MAC_NOISE], 0, 7);
    } else if (in->type == INS_DAC) {
        in->u.dac.sample = (uint8_t)rn((int)bank_n + 2);   /* past the kit too */
        in->u.dac.root = (uint8_t)(1 + rn(NOTE_MAX));
        in->u.dac.loop = (uint8_t)rn(2);
        in->u.dac.pitched = (uint8_t)rn(2);
    }
}

/* a random valid effect, or (rarely) any byte */
static uint8_t rnd_fx(void)
{
    static uint8_t list[256];
    static int n;
    if (!n) {
        int f;
        for (f = 1; f < 256; f++)
            if (fx_name((uint8_t)f))
                list[n++] = (uint8_t)f;
    }
    return rn(20) ? list[rn(n)] : (uint8_t)rnd();
}

static void rnd_song(void)
{
    int i, ch, p, r, nins;
    song_init(&song);
    song.region = (uint8_t)rn(2);
    song.chip = (uint8_t)rn(2);
    song.filter = (uint8_t)rn(3);
    song.ext3 = (uint8_t)(rn(4) == 0);
    song.tick_hz = (uint16_t)(rn(3) ? 0 : 100 + rn(2400));
    song.speed[0] = (uint8_t)(1 + rn(rn(2) ? 31 : 8));
    song.speed[1] = (uint8_t)(rn(2) ? song.speed[0] : 1 + rn(16));
    song.rows = (uint8_t)(1 + rn(rn(2) ? 128 : 16));
    song.lfo = (uint8_t)rn(16);
    song.nord = (uint16_t)(1 + rn(6));
    song.loop = (uint16_t)rn(song.nord + 1);
    nins = 1 + rn(12);
    for (i = 0; i < nins; i++)
        rnd_ins(&song.ins[i]);
    for (i = 0; i < song.nord; i++)
        for (ch = 0; ch < NCH; ch++)
            song.order[i][ch] = (uint8_t)rn(4);
    for (ch = 0; ch < NCH; ch++)
        for (p = 0; p < 4; p++) {
            int dens = rn(4);
            for (r = 0; r < song.rows; r++) {
                cell_t *c;
                if (rn(4) > dens)
                    continue;
                c = song_cell_w(&song, ch, p, r);
                if (!c)
                    break;
                i = rn(10);
                c->note = (uint8_t)(i < 7 ? 1 + rn(NOTE_MAX) : i == 7 ? NOTE_OFF : i == 8 ? NOTE_CUT : 0);
                c->ins = (uint8_t)(rn(3) ? rn(nins + 1) : VAL_NONE);
                c->vol = (uint8_t)(rn(2) ? (rn(4) ? (uint32_t)rn(128) : rnd()) : VAL_NONE);
                for (i = 0; i < 2; i++)
                    if (rn(3) == 0) {
                        c->fx[i] = rnd_fx();
                        c->fxv[i] = (uint8_t)rnd();
                    }
            }
        }
}

/* the song played with live commands in between, as the panel and the editor send them */
static void play_fuzzed(int blocks)
{
    int b;
    player_post(PC_RESET, 0, 0, 0);
    player_post(PC_PLAY, (uint16_t)rn(song.nord), (uint8_t)rn(song.rows), (uint8_t)(rn(4) == 0));
    for (b = 0; b < blocks; b++) {
        int k = rn(12);
        if (k == 0)
            player_post(PC_JAM_ON, (uint16_t)rn(NCH), (uint8_t)(1 + rn(NOTE_MAX)), (uint8_t)rn(16));
        else if (k == 1)
            player_post(PC_JAM_OFF, (uint16_t)rn(NCH), 0, 0);
        else if (k == 2 && rn(4) == 0)
            player_post(PC_MUTE, (uint16_t)rnd(), (uint8_t)rnd(), 0);
        else if (k == 3 && rn(6) == 0)
            player_post(PC_PLAY, (uint16_t)rn(song.nord), (uint8_t)rn(song.rows), (uint8_t)rn(2));
        else if (k == 4 && rn(8) == 0)
            player_post(PC_INS, (uint16_t)rn(16), 0, 0);
        else if (k == 5 && rn(8) == 0)
            rnd_ins(&song.ins[rn(12)]);    /* an edit while it plays (the player rereads it) */
        else if (k == 6 && rn(10) == 0) {
            /* a stop: then quiet within 200 ms, and the keys play (the loudness checked above) */
            int q, m = 0;
            player_post(PC_STOP, 0, 0, 0);
            for (q = 0; q < 35; q++)
                m = block_peak();
            if (m > QUIET && fails < 20) {
                fails++;
                printf("FAIL %s: still %d 200 ms after a stop\n", where, m);
            }
            player_post(PC_JAM_ON, (uint16_t)rn(NCH), (uint8_t)(1 + rn(NOTE_MAX)), (uint8_t)rn(16));
            if (rn(2))
                player_post(PC_PLAY, 0, 0, 0);
        } else if (k == 7 && rn(12) == 0)
            player_post(PC_RESET, 0, 0, 0);
        block_peak();
    }
    player_post(PC_STOP, 0, 0, 0);
    block_peak();
}

/* hanging notes: the keys played (song stopped, or playing and then stopped by the song's end or a
 * pause in between), instruments changed meanwhile, every key let go: then nothing may sound once the
 * releases have run out (PSG_REL_MAX ticks at the slowest tick rate, 50 Hz) */
static void jam_session(int events)
{
    uint32_t held = 0, left = 0;
    int e, q;
    player_post(PC_RESET, 0, 0, 0);
    for (e = 0; e < events; e++) {
        int k = rn(10), ch = rn(NCH);
        if (k < 4) {
            player_post(PC_JAM_ON, (uint16_t)ch, (uint8_t)(1 + rn(NOTE_MAX)), (uint8_t)rn(16));
            held |= 1u << ch;
        } else if (k < 7) {
            player_post(PC_JAM_OFF, (uint16_t)ch, 0, 0);
            held &= ~(1u << ch);
        } else if (k == 7)
            rnd_ins(&song.ins[rn(12)]);
        else if (k == 8)
            player_post(PC_INS, (uint16_t)rn(16), 0, 0);
        else if (rn(4) == 0)
            player_post(PC_MUTE, (uint16_t)rnd(), (uint8_t)rnd(), 0);
        for (q = rn(6); q >= 0; q--)
            block_peak();
    }
    for (e = 0; e < NCH; e++)
        if (held >> e & 1u)
            player_post(PC_JAM_OFF, (uint16_t)e, 0, 0);
    for (q = 0; q < 900; q++) {                /* 5.2 s */
        block_peak();
        if (!(left = player_hanging()))
            break;
    }
    if (left && fails < 20) {
        fails++;
        printf("FAIL %s: a hanging note 5 s after the keys were let go (channels %#x)\n", where, left);
    }
    for (e = 0; e < NCH; e++)
        if (player.ch[e].jam && fails < 20) {
            fails++;
            printf("FAIL %s: channel %d still the keys' after they were let go\n", where, e);
            break;
        }
}

/* the stream through song_read, damaged: bits flipped, bytes changed, cut short, or noise */
static uint8_t sbuf[1 << 16], dbuf[1 << 16];
static uint32_t slen, dlen, spos;
static void put(void *ctx, const uint8_t *b, uint32_t n)
{
    (void)ctx;
    if (slen + n <= sizeof sbuf)
        memcpy(sbuf + slen, b, n);
    slen += n;
}
static int get(void *ctx)
{
    (void)ctx;
    return spos < dlen ? dbuf[spos++] : -1;
}

static void test_safety_net(void)
{
    uint32_t t0 = player_guard_trips;
    int32_t m;
    int k;
    snprintf(where, sizeof where, "safety net");
    demo_song(&song);
    player_post(PC_PLAY, 0, 0, 0);
    for (k = 0; k < 40; k++)
        block_peak();
    player_test_poison();
    m = block_peak();
    if (player_guard_trips != t0 + 1 || m != 0 || player.playing) {
        fails++;
        printf("FAIL safety net: %u trips, peak %d, playing %d (expected 1 trip, silence, stopped)\n",
               player_guard_trips - t0, m, player.playing);
    }
    for (k = 0; k < 35; k++)
        m = block_peak();
    if (m > QUIET) {
        fails++;
        printf("FAIL safety net: still %d after it went off\n", m);
    }
    player_post(PC_JAM_ON, CH_FM1, 49, 0);
    for (k = 0, m = 0; k < 20; k++) {
        int32_t p = block_peak();
        if (p > m)
            m = p;
    }
    player_post(PC_JAM_OFF, CH_FM1, 0, 0);
    if (m < 500 || player_guard_trips != t0 + 1) {
        fails++;
        printf("FAIL safety net: a key afterwards peaks %d, %u trips\n", m, player_guard_trips - t0);
    }
    printf("safety net: a poisoned engine went silent after one block, and a key plays again (peak %d)\n", m);
    player_post(PC_STOP, 0, 0, 0);
    for (k = 0; k < 20; k++)
        block_peak();
}

int main(int argc, char **argv)
{
    int seeds = argc > 1 ? atoi(argv[1]) : 300, first = argc > 2 ? atoi(argv[2]) : 1, s, read_ok = 0;
    presets_bank_init();
    player_init(44100);
    test_safety_net();
    worst = 0;
    for (s = first; s < first + seeds; s++) {
        rng = 0x9E3779B9u * (uint32_t)s | 1u;
        /* a random song, played and poked */
        snprintf(where, sizeof where, "seed %d, random song", s);
        rnd_song();
        play_fuzzed(120 + rn(200));
        snprintf(where, sizeof where, "seed %d, keys", s);
        jam_session(20 + rn(60));
        /* a damaged copy of a song through song_read, then played */
        if (s % 2)
            demo_song(&song);
        slen = 0;
        song_write(&song, put, NULL);
        memcpy(dbuf, sbuf, slen);
        dlen = slen;
        {
            int k, n = 1 + rn(12);
            for (k = 0; k < n; k++) {
                int how = rn(4);
                uint32_t at = (uint32_t)rn((int)dlen);
                if (how == 0)
                    dbuf[at] ^= (uint8_t)(1u << rn(8));
                else if (how == 1)
                    dbuf[at] = (uint8_t)rnd();
                else if (how == 2 && rn(4) == 0)
                    dlen = at;
                else if (at >= 6)
                    dbuf[at] = (uint8_t)(rn(2) ? 0xFF : 0x00);
            }
            if (rn(20) == 0)
                for (k = 5; k < (int)dlen; k++)
                    dbuf[k] = (uint8_t)rnd();
        }
        spos = 0;
        snprintf(where, sizeof where, "seed %d, damaged stream", s);
        player_post(PC_STOP, 0, 0, 0);
        block_peak();
        if (song_read(&song, get, NULL) == 0) {
            read_ok++;
            play_fuzzed(80 + rn(80));
        }
    }
    printf("%d seeds: %.0f s rendered, %d damaged streams read and played; loudest mix %d before the clip "
           "(%.1f dB of the 16-bit range), %u of %u blocks clipped\n",
           seeds, frames_total / 44100.0, read_ok, worst, 20.0 * log10(worst / 32768.0 + 1e-9), clipped, frames_total / BLOCK);
    if (player_guard_trips != 1) {
        fails++;
        printf("FAIL the safety net went off %u times on real songs (only the poison should)\n", player_guard_trips - 1);
    }
    if (fails)
        printf("%d failures\n", fails);
    return fails != 0;
}
