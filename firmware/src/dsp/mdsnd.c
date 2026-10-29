/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the Mega Drive's sound (mdsnd.h). */
#include "mdsnd.h"

/* the console's clocks: the YM2612 runs at the 68000's clock / 144 a sample, the PSG at the Z80's */
static const uint32_t FM_CLK[2] = {7670453u, 7600489u};
static const uint32_t PSG_CLK[2] = {3579545u, 3546893u};
#define FM_GAIN 21                     /* 9-bit units to the output (ymfm's 128 * 64 / (6 * 65)) */
#define CHUNK 64u

static float exp_neg(float x)          /* e^-x for 0 <= x < 4, single precision (no libm on the FM-1) */
{
    float t = 1.0f, s = 1.0f;
    int k;
    for (k = 1; k < 24; k++) {
        t *= -x / (float)k;
        s += t;
    }
    return s;
}

void md_set_filter(md_t *m, int filter)
{
    /* one pole: Model 1 (VA0-VA6) about 3.4 kHz, Model 2 about 8 kHz, as measured on consoles */
    static const float FC[2] = {3390.0f, 8000.0f};
    m->filter = (uint8_t)filter;
    if (filter == MD_FILTER_NONE)
        m->lp_a = 16384;
    else
        m->lp_a = (int32_t)((1.0f - exp_neg(6.2831853f * FC[filter] / (float)m->out_hz)) * 16384.0f);
}

uint32_t q16_div(uint32_t n, uint32_t d)
{
    uint32_t q = n / d, r = n % d, i;
    for (i = 0; i < 16u; i++) {          /* long division, a bit at a time: r < d < 2^31 */
        r <<= 1;
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    return q;
}

void md_init(md_t *m, uint32_t out_hz, int region, int chip, int filter)
{
    uint8_t *b = (uint8_t *)m;
    uint32_t i;
    for (i = 0; i < sizeof(*m); i++)
        b[i] = 0;
    m->out_hz = out_hz;
    m->region = (uint8_t)region;
    m->fm_hz = (FM_CLK[region] + 72u) / 144u;
    m->fm_step = q16_div(FM_CLK[region] / 144u, out_hz) + q16_div(FM_CLK[region] % 144u, 144u * out_hz);
    opn2_reset(&m->fm, chip);
    psg_reset(&m->psg, PSG_CLK[region], out_hz);
    m->scope_div = 2;
    md_set_filter(m, filter);
}

void md_fm(md_t *m, int port, uint8_t reg, uint8_t val)
{
    opn2_write(&m->fm, port, reg, val);
    if (m->log)
        m->log(m->log_ctx, MD_LOG_FM, port, reg, val);
}

void md_psg(md_t *m, uint8_t val)
{
    psg_write(&m->psg, val);
    if (m->log)
        m->log(m->log_ctx, MD_LOG_PSG, 0, 0, val);
}

static void md_dac_write(md_t *m, const uint8_t *at)   /* a DAC byte from a sample (VGM: from its data bank) */
{
    opn2_write(&m->fm, 0, 0x2A, *at);
    m->dac_at = at;
    if (m->log)
        m->log(m->log_ctx, MD_LOG_DAC, 0, 0x2A, *at);
}

void md_dac_play(md_t *m, const uint8_t *data, uint32_t len, uint32_t loop, uint32_t rate_hz)
{
    m->dac.data = data;
    m->dac.len = len;
    m->dac.loop = loop;
    m->dac.pos = 0;
    m->dac.step = q16_div(rate_hz, m->fm_hz);
    m->dac.on = len != 0;
    if (m->dac.on)
        md_dac_write(m, data);
}

void md_dac_stop(md_t *m)
{
    m->dac.on = 0;
    md_fm(m, 0, 0x2A, 0x80);
}

void md_mute(md_t *m, uint32_t mask)
{
    m->fm.mute = (uint8_t)(mask & 0x3Fu);
    m->psg.mute = (uint8_t)((mask >> 6) & 15u);
}

static void fm_sample(md_t *m)
{
    int32_t l, r;
    if (m->dac.on) {                                        /* the driver's loop, as the Z80 runs it */
        uint32_t i0 = m->dac.pos >> 16, i1;
        m->dac.pos += m->dac.step;
        i1 = m->dac.pos >> 16;
        if (i1 >= m->dac.len) {
            if (m->dac.loop < m->dac.len) {
                m->dac.pos -= (m->dac.len - m->dac.loop) << 16;
                i1 = m->dac.pos >> 16;
            } else {
                m->dac.on = 0;
                i1 = i0;
            }
        }
        if (i1 != i0 && m->dac.on)
            md_dac_write(m, &m->dac.data[i1]);
    }
    m->fm_prev[0] = m->fm_next[0];
    m->fm_prev[1] = m->fm_next[1];
    opn2_clock(&m->fm, &l, &r);
    m->fm_next[0] = l * FM_GAIN;
    m->fm_next[1] = r * FM_GAIN;
}

static inline uint16_t absu(int32_t v) { return (uint16_t)(v < 0 ? -v : v); }

void md_render(md_t *m, int32_t *out, uint32_t n)
{
    int32_t psg[CHUNK];
    while (n) {
        uint32_t k = n < CHUNK ? n : CHUNK, s, c;
        for (s = 0; s < k; s++)
            psg[s] = 0;
        psg_render(&m->psg, psg, k);
        for (s = 0; s < k; s++) {
            int32_t x[2];
            uint32_t ch;
            m->fm_pos += m->fm_step;
            while (m->fm_pos >= (1u << 16)) {
                m->fm_pos -= (1u << 16);
                fm_sample(m);
            }
            for (c = 0; c < 2u; c++) {
                /* 32-bit products only (the FM-1's compiler has no 64-bit helpers): the step is
                 * 17 bits at most, the position 14, the filter's coefficient Q14 */
                int32_t f = m->fm_prev[c] + (((m->fm_next[c] - m->fm_prev[c]) * (int32_t)(m->fm_pos >> 2)) >> 14);
                int32_t v = f + psg[s] * 2 / 5, y;
                m->lp[c] += ((v - m->lp[c]) * m->lp_a) >> 14;
                /* DC blocker (the console's output capacitors): y = x - x' + (1 - 1/2048) y', its
                 * state in 1/256ths so that it settles to 0 (the ladder effect's offset goes) */
                m->dc[c] += (m->lp[c] - m->dc_x[c]) * 256 - m->dc[c] / 2048;
                m->dc_x[c] = m->lp[c];
                y = m->dc[c] / 256;
                x[c] = y;
            }
            out[2u * s] += x[0];
            out[2u * s + 1u] += x[1];
            if (++m->scope_div >= 2u) {
                m->scope_div = 0;
                for (ch = 0; ch < 6u; ch++)
                    m->scope[ch][m->scope_pos] = (int8_t)(m->fm.out[ch] >> 1);
                for (ch = 0; ch < 4u; ch++)
                    m->scope[6u + ch][m->scope_pos] = (int8_t)(m->psg.level[ch] >> 7);
                m->scope_pos = (uint8_t)((m->scope_pos + 1u) % MD_SCOPE);
            }
            for (ch = 0; ch < 6u; ch++)
                if (absu(m->fm.out[ch]) * 32u > m->peak[ch])
                    m->peak[ch] = (uint16_t)(absu(m->fm.out[ch]) * 32u);
            for (ch = 0; ch < 4u; ch++)
                if (absu(m->psg.level[ch]) > m->peak[6u + ch])
                    m->peak[6u + ch] = absu(m->psg.level[ch]);
            m->vgm_samples++;
        }
        out += 2u * k;
        n -= k;
    }
}
