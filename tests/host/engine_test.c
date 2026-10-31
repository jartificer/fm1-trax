/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the sound engine's tests.
 *
 *   engine_test [WAVDIR]
 *
 * The YM2612 is checked against Nuked-OPN2 (the cycle-accurate reference, built in when the tests are
 * given -DWITH_NUKED and its source; tests/run_tests.sh fetches it): the same register writes go to
 * both, and their outputs must match after lining up the reference's pipeline delay and its output
 * scale. The PSG and the mixer are checked against what the chips are documented to do. With WAVDIR,
 * each case is also written as a WAV to listen to. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/dsp/mdsnd.h"
#ifdef WITH_NUKED
#include "ym3438.h"
#endif

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

static void wav_write(const char *name, const int16_t *s, uint32_t frames, uint32_t ch, uint32_t hz)
{
    char path[512];
    FILE *f;
    uint32_t bytes = frames * ch * 2u, v;
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
    w = (uint16_t)ch;
    fwrite(&w, 2, 1, f);
    fwrite(&hz, 4, 1, f);
    v = hz * ch * 2u;
    fwrite(&v, 4, 1, f);
    w = (uint16_t)(ch * 2u);
    fwrite(&w, 2, 1, f);
    w = 16;
    fwrite(&w, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&bytes, 4, 1, f);
    fwrite(s, 2, frames * ch, f);
    fclose(f);
}

/* ------------------------------------------------------------- cases --- */
/* a case: register writes at sample times (in the chip's own samples) */
typedef struct {
    uint32_t t;
    uint8_t port, reg, val;
} wr_t;
#define END {0xFFFFFFFFu, 0, 0, 0}

/* a patch on a channel: ALG/FB, then per operator (OP1..OP4 in register order S1 S3 S2 S4) */
typedef struct {
    uint8_t alg, fb;
    uint8_t dtmul[4], tl[4], ksar[4], amd1r[4], d2r[4], slrr[4], ssg[4];
} patch_t;

static uint32_t patch_writes(wr_t *w, uint32_t t, uint32_t ch, const patch_t *p, uint8_t pan_ams_pms)
{
    uint32_t n = 0, i, port = ch >= 3u, c = ch % 3u;
    static const uint8_t SLOT[4] = {0, 8, 4, 12};          /* OP1..OP4 -> register offset */
    for (i = 0; i < 4u; i++) {
        uint8_t o = (uint8_t)(c + SLOT[i]);
        wr_t a[7] = {{t, (uint8_t)port, (uint8_t)(0x30 + o), p->dtmul[i]}, {t, (uint8_t)port, (uint8_t)(0x40 + o), p->tl[i]},
                     {t, (uint8_t)port, (uint8_t)(0x50 + o), p->ksar[i]}, {t, (uint8_t)port, (uint8_t)(0x60 + o), p->amd1r[i]},
                     {t, (uint8_t)port, (uint8_t)(0x70 + o), p->d2r[i]}, {t, (uint8_t)port, (uint8_t)(0x80 + o), p->slrr[i]},
                     {t, (uint8_t)port, (uint8_t)(0x90 + o), p->ssg[i]}};
        memcpy(&w[n], a, sizeof(a));
        n += 7;
    }
    w[n++] = (wr_t){t, (uint8_t)port, (uint8_t)(0xB0 + c), (uint8_t)(p->fb << 3 | p->alg)};
    w[n++] = (wr_t){t, (uint8_t)port, (uint8_t)(0xB4 + c), pan_ams_pms};
    return n;
}

static uint32_t note_writes(wr_t *w, uint32_t t, uint32_t ch, uint32_t block, uint32_t fnum, int on)
{
    uint32_t n = 0, port = ch >= 3u, c = ch % 3u;
    w[n++] = (wr_t){t, (uint8_t)port, (uint8_t)(0xA4 + c), (uint8_t)(block << 3 | fnum >> 8)};
    w[n++] = (wr_t){t, (uint8_t)port, (uint8_t)(0xA0 + c), (uint8_t)(fnum & 0xFF)};
    w[n++] = (wr_t){t, 0, 0x28, (uint8_t)((on ? 0xF0 : 0) | (ch >= 3u ? 4u + c : c))};
    return n;
}

static uint32_t key_writes(wr_t *w, uint32_t t, uint32_t ch, int on)
{
    w[0] = (wr_t){t, 0, 0x28, (uint8_t)((on ? 0xF0 : 0) | (ch >= 3u ? 4u + ch % 3u : ch))};
    return 1;
}

/* render a case through this engine, one write per sample (as the reference takes them) */
static void run_ours(const wr_t *w, uint32_t n, int chip, int16_t *out)
{
    static opn2_t c;
    uint32_t s, k = 0;
    opn2_reset(&c, chip);
    for (s = 0; s < n; s++) {
        int32_t l, r;
        if (w[k].t <= s && w[k].t != 0xFFFFFFFFu) {
            opn2_write(&c, w[k].port, w[k].reg, w[k].val);
            k++;
        }
        opn2_clock(&c, &l, &r);
        out[2 * s] = (int16_t)l;
        out[2 * s + 1] = (int16_t)r;
    }
}

#ifdef WITH_NUKED
static void run_nuked(const wr_t *w, uint32_t n, int chip, int16_t *out)
{
    static ym3438_t c;
    uint32_t s, k = 0, i;
    OPN2_SetChipType(chip == OPN2_YM2612 ? ym3438_mode_ym2612 : 0);
    OPN2_Reset(&c);
    for (s = 0; s < n; s++) {
        int32_t l = 0, r = 0;
        int phase = -1;
        if (w[k].t <= s && w[k].t != 0xFFFFFFFFu) {
            phase = (int)k;
            k++;
        }
        for (i = 0; i < 24u; i++) {
            Bit16s b[2];
            if (phase >= 0 && i == 0)
                OPN2_Write(&c, (uint32_t)w[phase].port * 2u, w[phase].reg);
            if (phase >= 0 && i == 12)
                OPN2_Write(&c, (uint32_t)w[phase].port * 2u + 1u, w[phase].val);
            OPN2_Clock(&c, b);
            l += b[0];
            r += b[1];
        }
        out[2 * s] = (int16_t)l;
        out[2 * s + 1] = (int16_t)r;
    }
}
#endif

#ifdef WITH_NUKED
/* how alike two streams are: the best normalised correlation over lags 0..maxlag (b later than a) */
static double match(const int16_t *a, const int16_t *b, uint32_t n, uint32_t from, int *lag_out)
{
    double best = -2;
    int lag, bestlag = 0;
    for (lag = 0; lag <= 8; lag++) {
        double sab = 0, saa = 0, sbb = 0, ma = 0, mb = 0;
        uint32_t i, m = 0;
        for (i = from; i + (uint32_t)lag < n; i++) {
            ma += a[2 * i] + a[2 * i + 1];
            mb += b[2 * (i + lag)] + b[2 * (i + lag) + 1];
            m += 2;
        }
        ma /= m;
        mb /= m;
        for (i = from; i + (uint32_t)lag < n; i++) {
            int c;
            for (c = 0; c < 2; c++) {
                double x = a[2 * i + c] - ma, y = b[2 * (i + lag) + c] - mb;
                sab += x * y;
                saa += x * x;
                sbb += y * y;
            }
        }
        if (saa > 0 && sbb > 0 && sab / sqrt(saa * sbb) > best) {
            best = sab / sqrt(saa * sbb);
            bestlag = lag;
        }
    }
    if (lag_out)
        *lag_out = bestlag;
    return best;
}
#endif

static double rms(const int16_t *a, uint32_t from, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = from; i < n; i++)
        s += (double)a[2 * i] * a[2 * i] + (double)a[2 * i + 1] * a[2 * i + 1];
    return sqrt(s / (2.0 * (n - from)));
}

#define NS 40000u                                           /* 0.75 s at 53,267 Hz */
static int16_t ours[NS * 2];
#ifdef WITH_NUKED
static int16_t ref[NS * 2];
#endif

static void compare(const char *name, const wr_t *w, int chip, double min_corr)
{
    int lag = 0;
    char wn[64];
    run_ours(w, NS, chip, ours);
    snprintf(wn, sizeof(wn), "opn2_%s", name);
    wav_write(wn, ours, NS, 2, 53267);
    CHECK(rms(ours, 200, NS) > 20.0, "%s: silent (rms %.1f)", name, rms(ours, 200, NS));
#ifdef WITH_NUKED
    {
        double c;
        run_nuked(w, NS, chip, ref);
        snprintf(wn, sizeof(wn), "nuked_%s", name);
        wav_write(wn, ref, NS, 2, 53267);
        c = match(ours, ref, NS, 200, &lag);
        printf("  %-14s %s  correlation with Nuked-OPN2 %.5f (lag %d)\n", name, chip == OPN2_YM2612 ? "YM2612" : "YM3438", c, lag);
        CHECK(c >= min_corr, "%s: correlation %.5f < %.5f", name, c, min_corr);
    }
#else
    (void)min_corr;
    (void)lag;
#endif
}

static const patch_t SINE = {7, 0, {0x01, 0x01, 0x01, 0x01}, {127, 127, 127, 0}, {0x1F, 0x1F, 0x1F, 0x1F},
                             {0, 0, 0, 0}, {0, 0, 0, 0}, {0x0F, 0x0F, 0x0F, 0x0F}, {0, 0, 0, 0}};
static const patch_t BRASS = {2, 5, {0x71, 0x0D, 0x33, 0x01}, {0x23, 0x2D, 0x26, 0x00}, {0x5F, 0x99, 0x5F, 0x94},
                              {0x05, 0x05, 0x05, 0x07}, {0x02, 0x02, 0x02, 0x02}, {0x11, 0x11, 0x11, 0xA6},
                              {0, 0, 0, 0}};
static const patch_t BASS = {0, 6, {0x71, 0x33, 0x0D, 0x01}, {0x1E, 0x1A, 0x22, 0x00}, {0x1F, 0x1F, 0x1F, 0x1F},
                             {0x0A, 0x0C, 0x0A, 0x09}, {0x00, 0x00, 0x00, 0x00}, {0x2F, 0x2F, 0x2F, 0x2F},
                             {0, 0, 0, 0}};
static const patch_t EP = {4, 3, {0x02, 0x01, 0x04, 0x01}, {0x20, 0x00, 0x24, 0x02}, {0x5F, 0x5F, 0x5F, 0x5F},
                           {0x88, 0x0B, 0x88, 0x08}, {0x02, 0x02, 0x02, 0x02}, {0x37, 0x37, 0x37, 0x37},
                           {0, 0, 0, 0}};
static const patch_t SSG = {7, 0, {0x01, 0x02, 0x03, 0x01}, {0x7F, 0x7F, 0x08, 0x00}, {0x1F, 0x1F, 0x1F, 0x1F},
                            {0, 0, 0x10, 0x14}, {0, 0, 0x08, 0x08}, {0x0F, 0x0F, 0x8F, 0x4F}, {0, 0, 0x0E, 0x08}};

static void test_opn2(void)
{
    static wr_t w[512];
    uint32_t n;
    int chip;
    printf("YM2612\n");
    for (chip = OPN2_YM3438; chip >= OPN2_YM2612; chip--) {
        /* a sine on OP4: the pitch */
        n = patch_writes(w, 0, 0, &SINE, 0xC0);
        n += note_writes(w + n, 40, 0, 4, 1083, 1);
        w[n] = (wr_t)END;
        compare("sine", w, chip, 0.999);
        if (chip == OPN2_YM3438) {
            uint32_t i, cross = 0;
            for (i = 2000; i + 1 < NS; i++)
                if (ours[2 * i] < 0 && ours[2 * i + 2] >= 0)
                    cross++;
            /* F = fnum * 2^block * fm / 2^21: 1083 * 16 * 53267 / 2097152 = 440.1 Hz */
            {
                double hz = cross * 53267.0 / (NS - 2001);
                CHECK(fabs(hz - 440.1) < 1.5, "sine: %.2f Hz, wanted 440.1", hz);
            }
        }
        /* algorithm 2 brass with feedback 5, key off half way */
        n = patch_writes(w, 0, 1, &BRASS, 0xC0);
        n += note_writes(w + n, 40, 1, 3, 0x284, 1);
        n += key_writes(w + n, 20000, 1, 0);
        w[n] = (wr_t)END;
        compare("brass", w, chip, 0.995);
        /* algorithm 0 bass, feedback 6, panned left, on channel 4 (port 1) */
        n = patch_writes(w, 0, 3, &BASS, 0x80);
        n += note_writes(w + n, 40, 3, 2, 0x2D8, 1);
        n += key_writes(w + n, 15000, 3, 0);
        w[n] = (wr_t)END;
        compare("bass", w, chip, 0.995);
        /* algorithm 4 EP with the LFO: AM and PM */
        w[0] = (wr_t){0, 0, 0x22, 0x0D};                     /* LFO on, rate 5 (9.6 Hz) */
        n = 1 + patch_writes(w + 1, 1, 2, &EP, 0xC0 | 0x30 | 0x06);
        n += note_writes(w + n, 50, 2, 4, 0x269, 1);
        w[n] = (wr_t)END;
        compare("ep_lfo", w, chip, 0.99);
        /* SSG-EG: repeating (8) and alternating-inverted (E) envelopes */
        n = patch_writes(w, 0, 4, &SSG, 0xC0);
        n += note_writes(w + n, 40, 4, 3, 0x300, 1);
        w[n] = (wr_t)END;
        compare("ssg_eg", w, chip, 0.99);
        /* channel 3 special mode: OP1-OP3 at their own pitches */
        {
            patch_t p = SINE;
            p.tl[0] = 0x10;
            p.tl[1] = 0x10;
            p.tl[2] = 0x10;
            n = patch_writes(w, 0, 2, &p, 0xC0);
            w[n++] = (wr_t){30, 0, 0x27, 0x40};
            w[n++] = (wr_t){31, 0, 0xAD, 0x22};             /* OP1 */
            w[n++] = (wr_t){32, 0, 0xA9, 0x69};
            w[n++] = (wr_t){33, 0, 0xAE, 0x23};             /* OP2 */
            w[n++] = (wr_t){34, 0, 0xAA, 0x0A};
            w[n++] = (wr_t){35, 0, 0xAC, 0x1A};             /* OP3 */
            w[n++] = (wr_t){36, 0, 0xA8, 0x8F};
            n += note_writes(w + n, 37, 2, 4, 0x200, 1);
            w[n] = (wr_t)END;
            compare("ch3_special", w, chip, 0.99);
        }
        /* the DAC on channel 6: a ramp, written every 4 samples */
        {
            uint32_t i;
            n = 0;
            w[n++] = (wr_t){0, 0, 0x2B, 0x80};
            for (i = 0; i < 400u; i++)
                w[n++] = (wr_t){10u + i * 4u, 0, 0x2A, (uint8_t)(i * 7u)};
            w[n] = (wr_t)END;
            compare("dac", w, chip, 0.99);
        }
        /* a chord on all six, three panned */
        {
            uint32_t ch;
            static const uint16_t F[6] = {0x269, 0x2D8, 0x309, 0x269, 0x2D8, 0x309};
            static const uint8_t PAN[6] = {0xC0, 0x80, 0x40, 0xC0, 0x80, 0x40};
            n = 0;
            for (ch = 0; ch < 6u; ch++)
                n += patch_writes(w + n, n, ch, ch & 1u ? &EP : &BRASS, PAN[ch]);
            for (ch = 0; ch < 6u; ch++)
                n += note_writes(w + n, n, ch, 3 + (ch > 2), F[ch], 1);
            w[n] = (wr_t)END;
            compare("chord6", w, chip, 0.99);
        }
    }
}

/* -------------------------------------------------------------- PSG --- */
static void test_psg(void)
{
    static psg_t p;
    static int32_t buf[44100];
    uint32_t i, cross = 0;
    printf("SN76489\n");
    psg_reset(&p, 3579545u, 44100u);
    /* tone 1, period 254: 3579545 / (32 * 254) = 440.4 Hz, attenuation 0 */
    psg_write(&p, 0x80 | 0x0E);
    psg_write(&p, 254 >> 4);
    psg_write(&p, 0x90);
    memset(buf, 0, sizeof(buf));
    psg_render(&p, buf, 44100);
    for (i = 100; i + 1 < 44100u; i++)
        if (buf[i] < 0 && buf[i + 1] >= 0)
            cross++;
    CHECK(abs((int)cross - 440) <= 1, "tone: %u crossings in a second, wanted 440", cross);
    {
        int32_t mx = 0;
        for (i = 100; i < 44100u; i++)
            if (abs(buf[i]) > mx)
                mx = abs(buf[i]);
        CHECK(mx >= 8000 && mx <= 8192, "tone: peak %d, wanted 8192", mx);
    }
    /* attenuation 2 dB a step: 10 steps = 20 dB */
    psg_write(&p, 0x90 | 10);
    memset(buf, 0, sizeof(buf));
    psg_render(&p, buf, 4410);
    {
        int32_t mx = 0;
        for (i = 100; i < 4410u; i++)
            if (abs(buf[i]) > mx)
                mx = abs(buf[i]);
        CHECK(mx >= 800 && mx <= 840, "attenuation 10: peak %d, wanted 819", mx);
    }
    /* period 0 and 1 hold the output: samples through the volume */
    psg_write(&p, 0x80);
    psg_write(&p, 0x00);
    psg_write(&p, 0x90);
    memset(buf, 0, sizeof(buf));
    psg_render(&p, buf, 1000);
    CHECK(buf[500] == 8192, "period 0: %d, wanted a steady 8192", buf[500]);
    /* white noise: about as many ups as downs, and not periodic at 15 bits */
    psg_reset(&p, 3579545u, 44100u);
    psg_write(&p, 0xE4);                                   /* white, clock / 512 */
    psg_write(&p, 0xF0);
    memset(buf, 0, sizeof(buf));
    psg_render(&p, buf, 44100);
    {
        int32_t pos = 0, neg = 0;
        for (i = 0; i < 44100u; i++)
            pos += buf[i] > 0, neg += buf[i] < 0;
        CHECK(pos > 15000 && neg > 15000, "white noise: %d up, %d down", pos, neg);
    }
    /* periodic noise from tone 3: a pulse every 16 shifts */
    psg_reset(&p, 3579545u, 44100u);
    psg_write(&p, 0xC0 | 0x0);
    psg_write(&p, 0x10);                                   /* tone 3 period 256 */
    psg_write(&p, 0xE3);                                   /* periodic, tone 3's rate */
    psg_write(&p, 0xF0);
    memset(buf, 0, sizeof(buf));
    psg_render(&p, buf, 44100);
    {
        uint32_t ups = 0;
        for (i = 1; i < 44100u; i++)
            if (buf[i - 1] < 0 && buf[i] >= 0)
                ups++;
        /* shifts at 3579545 / (16 * 256 * 2) = 437 Hz; one pulse in 16: 27.3 Hz */
        CHECK(ups >= 26 && ups <= 29, "periodic noise: %u pulses a second, wanted 27", ups);
    }
}

/* ------------------------------------------------------------ mixer --- */
static void test_mixer(void)
{
    static md_t m;
    static int32_t buf[44100 * 2];
    static int16_t pcm[44100 * 2];
    static uint8_t ramp[1000];
    uint32_t i;
    double e = 0;
    printf("Mixer\n");
    md_init(&m, 44100, MD_NTSC, OPN2_YM2612, MD_FILTER_MD1);
    CHECK(m.fm_hz == 53267u, "FM rate %u, wanted 53267", m.fm_hz);
    {
        wr_t w[64];
        uint32_t n = patch_writes(w, 0, 0, &BRASS, 0xC0), k;
        n += note_writes(w + n, 0, 0, 4, 0x269, 1);
        for (k = 0; k < n; k++)
            md_fm(&m, w[k].port, w[k].reg, w[k].val);
    }
    md_psg(&m, 0x80 | 0x0E);
    md_psg(&m, 254 >> 4);
    md_psg(&m, 0x94);
    memset(buf, 0, sizeof(buf));
    md_render(&m, buf, 44100);
    for (i = 0; i < 44100u * 2u; i++) {
        int32_t v = buf[i];
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        e += (double)v * v;
    }
    e = sqrt(e / (44100.0 * 2));
    CHECK(e > 500 && e < 12000, "mix level rms %.0f", e);
    wav_write("mix_brass_psg", pcm, 44100, 2, 44100);
    /* the DAC stream ends by itself */
    for (i = 0; i < sizeof(ramp); i++)
        ramp[i] = (uint8_t)(128 + 100 * sin(i * 0.2));
    md_fm(&m, 0, 0x2B, 0x80);
    md_dac_play(&m, ramp, sizeof(ramp), 0xFFFFFFFFu, 8000);
    memset(buf, 0, sizeof(buf));
    md_render(&m, buf, 6615);
    CHECK(!m.dac.on, "DAC: still playing 1000 samples at 8 kHz after 0.15 s");
    md_dac_play(&m, ramp, sizeof(ramp), 0, 8000);
    md_render(&m, buf, 4410);
    CHECK(m.dac.on, "DAC: a looped sample stopped");
    /* PAL */
    md_init(&m, 44100, MD_PAL, OPN2_YM2612, MD_FILTER_NONE);
    CHECK(m.fm_hz == 52781u, "PAL FM rate %u, wanted 52781", m.fm_hz);
}

int main(int argc, char **argv)
{
    wavdir = argc > 1 ? argv[1] : NULL;
    test_opn2();
    test_psg();
    test_mixer();
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
