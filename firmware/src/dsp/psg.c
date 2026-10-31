/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: SN76489 (psg.h). Behaviour as documented by SMS Power! ("SN76489 notes", Maxim) for
 * the Sega VDP's PSG. */
#include "psg.h"
#include "dsp_hot.h"

/* 2 dB a step, 8192 full scale, 15 off */
static const int16_t VOL[16] DSP_HOT_DATA = {8192, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
                                1298, 1031, 819, 650, 516, 410, 326, 0};

void psg_reset(psg_t *p, uint32_t clock_hz, uint32_t out_hz)
{
    uint32_t i;
    uint8_t *b = (uint8_t *)p;
    for (i = 0; i < sizeof(*p); i++)
        b[i] = 0;
    for (i = 0; i < PSG_NCH; i++) {
        p->vol[i] = 15;
        p->out[i] = 1;
    }
    p->lfsr = 0x8000;
    p->step = (clock_hz / 16u / out_hz) << 16;              /* (clock / 16 << 16) / out, in 32 bits */
    {
        uint32_t r = clock_hz / 16u % out_hz, i;
        for (i = 0; i < 16u; i++) {
            r <<= 1;
            if (r >= out_hz) {
                r -= out_hz;
                p->step |= 1u << (15u - i);
            }
        }
    }
}

void psg_write(psg_t *p, uint8_t v)
{
    uint32_t r;
    if (v & 0x80u) {                                       /* latch + data */
        p->latch = (v >> 4) & 7u;
        r = p->latch;
        if (r & 1u)
            p->vol[r >> 1] = v & 15u;
        else if (r == 6u) {
            p->noise = v & 7u;
            p->lfsr = 0x8000;                              /* a write to the noise control resets it */
        } else
            p->period[r >> 1] = (uint16_t)((p->period[r >> 1] & 0x3F0u) | (v & 15u));
        return;
    }
    r = p->latch;                                           /* data byte: the latched register */
    if (r & 1u)
        p->vol[r >> 1] = v & 15u;
    else if (r == 6u) {
        p->noise = v & 7u;
        p->lfsr = 0x8000;
    } else
        p->period[r >> 1] = (uint16_t)((p->period[r >> 1] & 15u) | ((uint32_t)(v & 0x3Fu) << 4));
}

DSP_INLINE void tick(psg_t *p)                           /* one step of the chip: clock / 16 */
{
    uint32_t i;
    for (i = 0; i < 3u; i++) {
        if (p->period[i] <= 1u) {                           /* 0 and 1: the output stays high */
            p->out[i] = 1;
            continue;
        }
        if (p->count[i])
            p->count[i]--;
        if (!p->count[i]) {
            p->count[i] = p->period[i];
            p->out[i] = (int8_t)-p->out[i];
        }
    }
    if (p->count[3])
        p->count[3]--;
    if (!p->count[3]) {
        uint32_t rate = p->noise & 3u;
        p->count[3] = rate == 3u ? (p->period[2] ? p->period[2] : 1u) : (uint16_t)(0x10u << rate);
        p->nflip ^= 1u;
        if (p->nflip) {                                     /* the rising edge shifts the LFSR */
            uint32_t fb = (p->noise & 4u) ? ((p->lfsr ^ (p->lfsr >> 3)) & 1u) : (p->lfsr & 1u);
            p->lfsr = (uint16_t)((p->lfsr >> 1) | (fb << 15));
        }
    }
    p->out[3] = (p->lfsr & 1u) ? 1 : -1;
}

/* the reference: every chip step, one at a time (for sample rates far below the chip's) */
DSP_HOT static void render_steps(psg_t *p, int32_t *out, uint32_t n)
{
    uint32_t s, i;
    for (s = 0; s < n; s++) {
        int32_t acc[PSG_NCH];
        uint32_t need = p->step, sum = 0;
        int32_t den = 0;
        for (i = 0; i < PSG_NCH; i++)
            acc[i] = 0;
        while (need) {
            uint32_t left = (1u << 16) - p->frac, take = need < left ? need : left;
            for (i = 0; i < PSG_NCH; i++)
                acc[i] += p->out[i] * (int32_t)(take >> 4);
            den += (int32_t)(take >> 4);
            p->frac += take;
            need -= take;
            if (p->frac >= (1u << 16)) {
                p->frac = 0;
                tick(p);
            }
        }
        for (i = 0; i < PSG_NCH; i++) {
            int32_t v = acc[i] * VOL[p->vol[i]] / den;   /* < 2^31: |acc| <= den <= step / 16 */
            if (p->mute & (1u << i))
                v = 0;
            p->level[i] = (int16_t)v;
            sum += (uint32_t)v;
        }
        out[s] += (int32_t)sum;
    }
}

#define MAX_STEPS 30u                                       /* chip steps per output sample, fast path */

/* The same samples as render_steps, a channel at a time: an output sample covers a few chip steps
 * (5 at 44.1 kHz), and a channel changes level only at its own events, so each channel's box-filter
 * sum is its level times the weights between its events. The weights: w0 up to the first step, then
 * 4096 a step (a whole step's 65536 >> 4), then the remainder's; PRE(t) is the weight covered when
 * step t happens, w the whole sample's. */
#define PRE(t) (w0 + 4096 * (int32_t)((t) - 1u))
DSP_HOT void psg_render(psg_t *p, int32_t *out, uint32_t n)
{
    uint32_t s, i;
    if ((p->step >> 16) >= MAX_STEPS) {
        render_steps(p, out, n);
        return;
    }
    for (s = 0; s < n; s++) {
        int32_t w0, w, acc[PSG_NCH];
        uint32_t need = p->step, left = (1u << 16) - p->frac, nt, sum = 0;
        if (need < left) {                                  /* no step in this sample */
            w0 = w = (int32_t)(need >> 4);
            nt = 0;
            p->frac += need;
        } else {
            uint32_t rem = need - left;
            w0 = (int32_t)(left >> 4);
            nt = 1u + (rem >> 16);
            w = w0 + 4096 * (int32_t)(rem >> 16) + (int32_t)((rem & 0xFFFFu) >> 4);
            p->frac = rem & 0xFFFFu;
        }
        for (i = 0; i < 3u; i++) {                          /* the tones */
            int32_t o = p->out[i];
            uint32_t per = p->period[i], c = p->count[i], t = c ? c : 1u;
            if (per <= 1u) {                                /* held high from the first step on */
                if (nt) {
                    acc[i] = o * w0 + (w - w0);
                    p->out[i] = 1;
                } else
                    acc[i] = o * w;
            } else if (t > nt) {                            /* no edge in this sample */
                acc[i] = o * w;
                p->count[i] = (uint16_t)(c - nt);
            } else {
                int32_t a = 0, last = 0;
                for (; t <= nt; t += per) {                 /* an edge at each step t */
                    a += o * (PRE(t) - last);
                    last = PRE(t);
                    o = -o;
                }
                acc[i] = a + o * (w - last);
                p->out[i] = (int8_t)o;
                p->count[i] = (uint16_t)(per - (nt - (t - per)));
            }
        }
        {                                                   /* the noise: its level follows the LFSR */
            int32_t o = p->out[3];
            if (!nt)
                acc[3] = o * w;
            else {
                uint32_t rate = p->noise & 3u, c = p->count[3], te = c ? c : 1u, tk = 1, fired = 0;
                uint32_t rl = rate == 3u ? (p->period[2] ? p->period[2] : 1u) : (0x10u << rate);
                int32_t a = 0, last = 0;
                for (;;) {
                    int32_t v;
                    if (tk == te) {                         /* the counter runs out */
                        p->nflip ^= 1u;
                        if (p->nflip) {
                            uint32_t fb = (p->noise & 4u) ? ((p->lfsr ^ (p->lfsr >> 3)) & 1u) : (p->lfsr & 1u);
                            p->lfsr = (uint16_t)((p->lfsr >> 1) | (fb << 15));
                        }
                        te += rl;
                        fired = 1;
                    }
                    v = (p->lfsr & 1u) ? 1 : -1;            /* every step sets the level */
                    if (v != o) {
                        a += o * (PRE(tk) - last);
                        last = PRE(tk);
                        o = v;
                    }
                    if (te > nt)
                        break;
                    tk = te;
                }
                acc[3] = a + o * (w - last);
                p->out[3] = (int8_t)o;
                p->count[3] = (uint16_t)(fired ? rl - (nt - (te - rl)) : c - nt);
            }
        }
        for (i = 0; i < PSG_NCH; i++) {
            int32_t v = 0, g = VOL[p->vol[i]];
            if (g && !(p->mute & (1u << i)))
                v = acc[i] * g / w;                         /* as render_steps: |acc| <= w */
            p->level[i] = (int16_t)v;
            sum += (uint32_t)v;
        }
        out[s] += (int32_t)sum;
    }
}
