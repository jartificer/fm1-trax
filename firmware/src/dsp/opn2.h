/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the Yamaha YM2612 (OPN2) and YM3438 (OPN2C), at register level and sample accuracy.
 *
 * One call of opn2_clock is one of the chip's own samples (its master clock / 144: 53,267 Hz on an
 * NTSC Mega Drive). The model is ymfm's (Aaron Giles, BSD-3-Clause, see opn2.c), which is checked
 * against Nuked-OPN2's die-level emulation: the chip's log-sine and exponent ROMs, its envelope
 * rate tables, detune, LFO, SSG-EG, channel 3's special mode, CSM, the DAC and the 9-bit output with
 * the YM2612's ladder effect. */
#pragma once
#include <stdint.h>

#define OPN2_NCH 6

enum { OPN2_YM2612, OPN2_YM3438 };

typedef struct {
    uint32_t phase;                    /* 10.10 */
    uint32_t step;                     /* phase step without PM (cached) */
    uint16_t env;                      /* attenuation, 10 bits */
    uint8_t state;                     /* EG_* */
    uint8_t key, keyon;                /* the key as the envelope saw it; the key requests (bit 0 normal, 1 CSM) */
    uint8_t ssg_inv;
    /* cached from the registers */
    uint8_t rate[4];
    uint16_t sustain, tl;
    uint16_t block_freq;
    int8_t detune;
    uint8_t mul2;                      /* multiple as x.1 (0 means 1/2) */
    uint8_t ssg;                       /* bit 3 enable, 0-2 mode */
    uint8_t am;
} opn2_op_t;

typedef struct {
    opn2_op_t op[4];                   /* in algorithm order: OP1, OP2, OP3, OP4 */
    int16_t fb[2], fb_in;
} opn2_ch_t;

typedef struct {
    uint8_t reg[0x200];
    opn2_ch_t ch[OPN2_NCH];
    uint32_t env_counter;
    uint32_t lfo_counter;
    uint8_t lfo_am;
    int8_t lfo_pm;
    uint8_t dirty;                     /* channels whose registers changed (bit per channel) */
    uint8_t chip;                      /* OPN2_YM2612 / OPN2_YM3438 */
    uint8_t mute;                      /* channels left out of the output (bit per channel; not the chip's) */
    uint8_t dac_en;
    uint16_t dac;                      /* 9 bits, signed as the chip's */
    uint16_t addr[2];
    uint8_t status;
    int32_t ta_count, tb_count;        /* samples to the timers' next overflow */
    uint8_t ta_run, tb_run;
    int16_t out[OPN2_NCH];             /* each channel's last output, 9 bits, for scopes and meters */
} opn2_t;

void opn2_reset(opn2_t *c, int chip);
/* a register write as a sound driver makes it: port 0 (0x000..0x0FF: global and channels 1-3) or 1
 * (channels 4-6); the DAC is register 0x2A, its enable 0x2B */
void opn2_write(opn2_t *c, int port, uint8_t reg, uint8_t val);
/* one sample at the chip's rate; the left and right output in the chip's 9-bit units summed over
 * the six channels (-1536..+1530 for the YM2612) */
void opn2_clock(opn2_t *c, int32_t *l, int32_t *r);
uint8_t opn2_status(const opn2_t *c);
