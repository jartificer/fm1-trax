/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the Mega Drive's sound: the YM2612 at its own rate, resampled, the PSG, the DAC
 * stream a sound driver would feed from the Z80, the console's output filter, and scopes.
 *
 * Everything that makes sound goes through md_fm / md_psg (register writes, as a driver makes them)
 * and md_dac_* (samples on the DAC); those are also what a .vgm records. */
#pragma once
#include <stdint.h>
#include "opn2.h"
#include "psg.h"

#define MD_NCH 10                      /* FM1-6, SQ1-3, NOISE */
#define MD_SCOPE 128                   /* samples kept per channel for the scopes */

enum { MD_NTSC, MD_PAL };
enum { MD_FILTER_MD1, MD_FILTER_MD2, MD_FILTER_NONE };

typedef struct {
    const uint8_t *data;               /* 8-bit unsigned PCM */
    uint32_t len, loop;                /* loop: where a looped sample starts again; >= len: no loop */
    uint32_t pos, step;                /* Q16, in samples per FM sample */
    uint8_t on;
} md_dac_t;

typedef struct {
    opn2_t fm;
    psg_t psg;
    md_dac_t dac;
    uint32_t out_hz, fm_hz;
    uint32_t fm_step;                  /* FM samples per output sample, Q16 */
    uint32_t fm_pos;                   /* Q16: how far between fm_prev and fm_next */
    int32_t fm_prev[2], fm_next[2];
    int32_t lp[2], lp_a;               /* output low-pass: state, coefficient Q14 */
    int32_t dc[2], dc_x[2];            /* DC blocker: state (x256), last input */
    uint8_t region, filter;
    uint32_t vgm_samples;              /* 44.1 kHz samples rendered (for the VGM log's waits) */
    int8_t scope[MD_NCH][MD_SCOPE];
    uint8_t scope_pos, scope_div;
    uint16_t peak[MD_NCH];             /* per channel, decays; for meters */
    void (*log)(void *ctx, int kind, int port, uint8_t reg, uint8_t val);   /* a VGM writer (or NULL) */
    void *log_ctx;
    const uint8_t *dac_at;             /* the sample byte the DAC was last given (MD_LOG_DAC) */
} md_t;

enum { MD_LOG_FM, MD_LOG_PSG, MD_LOG_DAC };

/* (n << 16) / d without 64-bit arithmetic; n < d << 15 */
uint32_t q16_div(uint32_t n, uint32_t d);

void md_init(md_t *m, uint32_t out_hz, int region, int chip, int filter);
void md_set_filter(md_t *m, int filter);
void md_fm(md_t *m, int port, uint8_t reg, uint8_t val);
void md_psg(md_t *m, uint8_t val);
/* play a sample on the DAC (it is turned on by register 0x2B, as on the console); rate in Hz */
void md_dac_play(md_t *m, const uint8_t *data, uint32_t len, uint32_t loop, uint32_t rate_hz);
void md_dac_stop(md_t *m);
void md_mute(md_t *m, uint32_t mask);  /* bit per channel, MD_NCH of them */
/* n stereo frames, interleaved, added to out[]; one FM channel at full level is about 5,500 */
void md_render(md_t *m, int32_t *out, uint32_t n);
/* md_render from code in the XIP flash: on the FM-1 it runs from RAM (dsp_hot.h), out of a direct
 * call's reach, so through a pointer the compiler cannot fold back */
static inline void md_render_far(md_t *m, int32_t *out, uint32_t n)
{
#ifdef __PI32V2__
    void (*volatile f)(md_t *, int32_t *, uint32_t) = md_render;
    f(m, out, n);
#else
    md_render(m, out, n);
#endif
}
