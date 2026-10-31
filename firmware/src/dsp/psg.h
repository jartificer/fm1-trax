/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the SN76489 as in the Mega Drive's VDP (Sega's variant).
 *
 * Three square tone channels and a noise channel at 3.579545 MHz / 16. 10-bit periods (0 and 1 hold
 * the output high, which drivers use to play samples through the volume), 4-bit attenuation in 2 dB
 * steps, a 16-bit noise LFSR tapped at bits 0 and 3 (white) or 0 (periodic) at clock/512, /1024,
 * /2048 or tone 3's rate. Rendered with an exact box filter: each output sample is the average of the
 * chip's steps it covers. */
#pragma once
#include <stdint.h>

#define PSG_NCH 4

typedef struct {
    uint16_t period[3];
    uint8_t vol[PSG_NCH];              /* attenuation, 15 = off */
    uint8_t noise;                     /* bit 2 white, bits 0-1 rate (3: tone 3's) */
    uint8_t latch;                     /* the register the next data byte goes to: 0-7 */
    uint16_t count[PSG_NCH];
    int8_t out[PSG_NCH];               /* each channel's level: +1 / -1 (noise: 1 / 0 as its LFSR bit) */
    uint8_t nflip;                     /* noise counter's flip-flop: the LFSR shifts every second flip */
    uint16_t lfsr;
    uint32_t step;                     /* chip steps per output sample, Q16 */
    uint32_t frac;                     /* how far into the current step, Q16 */
    uint8_t mute;                      /* channels left out (bit per channel; not the chip's) */
    int16_t level[PSG_NCH];            /* each channel's last output sample, for scopes and meters */
} psg_t;

void psg_reset(psg_t *p, uint32_t clock_hz, uint32_t out_hz);
void psg_write(psg_t *p, uint8_t val);
/* n samples, mono, added to out[]; full scale per channel is 8192 */
void psg_render(psg_t *p, int32_t *out, uint32_t n);
