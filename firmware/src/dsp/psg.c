/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: SN76489 (psg.h). Behaviour as documented by SMS Power! ("SN76489 notes", Maxim) for
 * the Sega VDP's PSG. */
#include "psg.h"

/* 2 dB a step, 8192 full scale, 15 off */
static const int16_t VOL[16] = {8192, 6507, 5168, 4105, 3261, 2590, 2057, 1634,
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
    p->step = (uint32_t)(((uint64_t)(clock_hz / 16u) << 16) / out_hz);
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

static inline void tick(psg_t *p)                           /* one step of the chip: clock / 16 */
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

void psg_render(psg_t *p, int32_t *out, uint32_t n)
{
    uint32_t s, i;
    for (s = 0; s < n; s++) {
        int32_t acc[PSG_NCH] = {0, 0, 0, 0};
        uint32_t need = p->step, sum = 0;
        int32_t den = 0;
        while (need) {
            uint32_t left = 0x10000u - p->frac, take = need < left ? need : left;
            for (i = 0; i < PSG_NCH; i++)
                acc[i] += p->out[i] * (int32_t)(take >> 4);
            den += (int32_t)(take >> 4);
            p->frac += take;
            need -= take;
            if (p->frac >= 0x10000u) {
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
