/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the engine's output fingerprints. Random register logs for each chip (both YM2612 ports,
 * every register including CSM, channel 3's special mode, the timers, SSG-EG, the LFO, the DAC; the
 * PSG's tones and noise), the whole mixer (md_render, with samples on the DAC), and random songs
 * through the player, each hashed sample by sample. An optimisation of the engine must leave every
 * hash as it was: `golden_test write FILE` records them, `golden_test FILE` compares.
 *
 *   golden_test [write] FILE [SEEDS] */
#define main fuzz_main
#include "fuzz_test.c"
#undef main

static uint32_t hash;
static void mix(int32_t v) { hash = (hash ^ (uint32_t)v) * 16777619u; }

static void chip_fm(void)
{
    static opn2_t c;
    int i, k;
    opn2_reset(&c, rn(2));
    for (i = 0; i < 3000; i++) {
        int32_t l, r;
        for (k = rn(4); k > 0; k--) {
            int port = rn(2);
            uint8_t reg = (uint8_t)(rn(5) == 0 ? 0x20 + rn(16) : 0x28 + rn(0x98)), val = (uint8_t)rnd();
            if (rn(4) == 0)
                reg = 0x28, port = 0;
            if (rn(40) == 0)
                reg = 0x27, val = (uint8_t)((rn(3) == 0 ? 0x80 : rn(2) ? 0x40 : 0) | (rnd() & 0x3F));
            opn2_write(&c, port, reg, val);
        }
        for (k = 1 + rn(40); k > 0; k--) {
            opn2_clock(&c, &l, &r);
            mix(l), mix(r);
            mix(c.out[0] ^ c.out[2] << 4 ^ c.out[5] << 8);
        }
        mix(c.status);
    }
}

static void chip_psg(void)
{
    static psg_t p;
    static int32_t o[300];
    int i, k;
    psg_reset(&p, rn(2) ? 3579545u : 3546893u, rn(3) ? 44100u : 22050u + rn(40000));
    for (i = 0; i < 2000; i++) {
        uint32_t n = 1 + (uint32_t)rn(300);
        for (k = rn(4); k > 0; k--) {
            uint8_t v = (uint8_t)rnd();
            if (rn(3) == 0)                     /* low periods (0, 1 and short) as often as long ones */
                v = (uint8_t)(rn(2) ? 0x80 | (rn(8) << 4) | rn(3) : rn(2));
            psg_write(&p, v);
        }
        if (rn(20) == 0)
            p.mute = (uint8_t)rn(16);
        for (k = 0; k < (int)n; k++)
            o[k] = rn(2) ? 0 : (int32_t)rnd() >> 16;
        psg_render(&p, o, n);
        for (k = 0; k < (int)n; k++)
            mix(o[k]);
        for (k = 0; k < PSG_NCH; k++)
            mix(p.level[k]);
    }
}

static void chip_md(void)
{
    static md_t m;
    static int32_t o[2 * 300];
    static uint8_t smp[4000];
    int i, k;
    for (k = 0; k < (int)sizeof smp; k++)
        smp[k] = (uint8_t)rnd();
    md_init(&m, 44100, rn(2), rn(2), rn(3));
    for (i = 0; i < 600; i++) {
        uint32_t n = 1 + (uint32_t)rn(300);
        for (k = rn(5); k > 0; k--) {
            int what = rn(10);
            if (what < 6)
                md_fm(&m, rn(2), (uint8_t)(rn(4) ? 0x30 + rn(0x88) : rn(2) ? 0x28 : 0x20 + rn(16)), (uint8_t)rnd());
            else if (what < 8)
                md_psg(&m, (uint8_t)rnd());
            else if (what < 9)
                md_dac_play(&m, smp + rn(2000), (uint32_t)rn(2000), (rn(2) ? (uint32_t)rn(2000) : 0xFFFFFFFFu),
                            4000u + (uint32_t)rn(30000));
            else
                md_fm(&m, 0, 0x2B, (uint8_t)(rn(2) << 7));
        }
        if (rn(30) == 0)
            md_mute(&m, (uint32_t)rnd());
        for (k = 0; k < 2 * (int)n; k++)
            o[k] = 0;
        md_render(&m, o, n);
        for (k = 0; k < 2 * (int)n; k++)
            mix(o[k]);
        for (k = 0; k < MD_NCH; k++)
            mix(m.peak[k]);
        mix(m.scope[0][m.scope_pos] ^ m.scope[7][m.scope_pos] << 8);
    }
}

static void songs(void)
{
    static int32_t b[2 * BLOCK];
    int i, k;
    rnd_song();
    player_post(PC_RESET, 0, 0, 0);
    player_post(PC_PLAY, (uint16_t)rn(song.nord), 0, 0);
    for (i = 0; i < 300; i++) {
        if (rn(10) == 0)
            player_post(PC_JAM_ON, (uint16_t)rn(NCH), (uint8_t)(1 + rn(NOTE_MAX)), (uint8_t)rn(16));
        else if (rn(10) == 0)
            player_post(PC_JAM_OFF, (uint16_t)rn(NCH), 0, 0);
        for (k = 0; k < 2 * (int)BLOCK; k++)
            b[k] = 0;
        player_render(b, BLOCK);
        for (k = 0; k < 2 * (int)BLOCK; k++)
            mix(b[k]);
    }
}

int main(int argc, char **argv)
{
    int wr = argc > 1 && !strcmp(argv[1], "write"), seeds, s, bad = 0;
    const char *path = argc > 1 + wr ? argv[1 + wr] : NULL;
    FILE *f;
    if (!path) {
        fprintf(stderr, "usage: golden_test [write] FILE [SEEDS]\n");
        return 2;
    }
    seeds = argc > 2 + wr ? atoi(argv[2 + wr]) : 40;
    f = fopen(path, wr ? "w" : "r");
    if (!f) {
        perror(path);
        return 2;
    }
    presets_bank_init();
    player_init(44100);
    for (s = 1; s <= seeds; s++) {
        static void (*const T[4])(void) = {chip_fm, chip_psg, chip_md, songs};
        static const char *const N[4] = {"YM2612", "PSG", "mixer", "song"};
        int t;
        for (t = 0; t < 4; t++) {
            unsigned want;
            rng = 0x9E3779B9u * (uint32_t)(s * 4 + t) | 1u;
            hash = 2166136261u;
            T[t]();
            if (wr)
                fprintf(f, "%08x\n", hash);
            else if (fscanf(f, "%x", &want) != 1 || want != hash) {
                if (bad++ < 20)
                    printf("FAIL seed %d, %s: %08x, recorded %08x\n", s, N[t], hash, want);
            }
        }
    }
    fclose(f);
    printf("%d seeds, 4 engines each: %s\n", seeds, wr ? "recorded" : bad ? "DIFFERENT" : "identical");
    return bad != 0;
}
