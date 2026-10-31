/* SPDX-License-Identifier: GPL-3.0-only AND BSD-3-Clause */
/* FM1-TRAX: YM2612 / YM3438 emulation (opn2.h).
 *
 * A C port of the OPN2 parts of ymfm (https://github.com/aaronsgiles/ymfm), whose model of the chip
 * is checked against Nuked-OPN2. The tables and the algorithms below are ymfm's; the structure is
 * new (one chip, cached per operator, plain C for the FM-1). ymfm's licence:
 *
 *   Copyright (c) 2021, Aaron Giles. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without modification, are permitted
 *   provided that the following conditions are met:
 *   1. Redistributions of source code must retain the above copyright notice, this list of
 *      conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright notice, this list of
 *      conditions and the following disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *   3. Neither the name of the copyright holder nor the names of its contributors may be used to
 *      endorse or promote products derived from this software without specific prior written
 *      permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
 *   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 *   FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *   DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *   IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 *   OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */
#include "opn2.h"
#include "dsp_hot.h"

enum { EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };
#define EG_QUIET 0x380u

/* the chip's quarter log-sine ROM: -log2(sin) as 4.8 fixed point */
static const uint16_t SIN_ATT[256] DSP_HOT_DATA = {
    0x859,0x6c3,0x607,0x58b,0x52e,0x4e4,0x4a6,0x471,0x443,0x41a,0x3f5,0x3d3,0x3b5,0x398,0x37e,0x365,
    0x34e,0x339,0x324,0x311,0x2ff,0x2ed,0x2dc,0x2cd,0x2bd,0x2af,0x2a0,0x293,0x286,0x279,0x26d,0x261,
    0x256,0x24b,0x240,0x236,0x22c,0x222,0x218,0x20f,0x206,0x1fd,0x1f5,0x1ec,0x1e4,0x1dc,0x1d4,0x1cd,
    0x1c5,0x1be,0x1b7,0x1b0,0x1a9,0x1a2,0x19b,0x195,0x18f,0x188,0x182,0x17c,0x177,0x171,0x16b,0x166,
    0x160,0x15b,0x155,0x150,0x14b,0x146,0x141,0x13c,0x137,0x133,0x12e,0x129,0x125,0x121,0x11c,0x118,
    0x114,0x10f,0x10b,0x107,0x103,0x0ff,0x0fb,0x0f8,0x0f4,0x0f0,0x0ec,0x0e9,0x0e5,0x0e2,0x0de,0x0db,
    0x0d7,0x0d4,0x0d1,0x0cd,0x0ca,0x0c7,0x0c4,0x0c1,0x0be,0x0bb,0x0b8,0x0b5,0x0b2,0x0af,0x0ac,0x0a9,
    0x0a7,0x0a4,0x0a1,0x09f,0x09c,0x099,0x097,0x094,0x092,0x08f,0x08d,0x08a,0x088,0x086,0x083,0x081,
    0x07f,0x07d,0x07a,0x078,0x076,0x074,0x072,0x070,0x06e,0x06c,0x06a,0x068,0x066,0x064,0x062,0x060,
    0x05e,0x05c,0x05b,0x059,0x057,0x055,0x053,0x052,0x050,0x04e,0x04d,0x04b,0x04a,0x048,0x046,0x045,
    0x043,0x042,0x040,0x03f,0x03e,0x03c,0x03b,0x039,0x038,0x037,0x035,0x034,0x033,0x031,0x030,0x02f,
    0x02e,0x02d,0x02b,0x02a,0x029,0x028,0x027,0x026,0x025,0x024,0x023,0x022,0x021,0x020,0x01f,0x01e,
    0x01d,0x01c,0x01b,0x01a,0x019,0x018,0x017,0x017,0x016,0x015,0x014,0x014,0x013,0x012,0x011,0x011,
    0x010,0x00f,0x00f,0x00e,0x00d,0x00d,0x00c,0x00c,0x00b,0x00a,0x00a,0x009,0x009,0x008,0x008,0x007,
    0x007,0x007,0x006,0x006,0x005,0x005,0x005,0x004,0x004,0x004,0x003,0x003,0x003,0x002,0x002,0x002,
    0x002,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000};

/* the exponent ROM: 10-bit mantissas, implicit 0x400, reversed and shifted up by 2 (ymfm's form) */
#define X(a) ((((a) | 0x400) << 2))
static const uint16_t POWER[256] DSP_HOT_DATA = {
    X(0x3fa),X(0x3f5),X(0x3ef),X(0x3ea),X(0x3e4),X(0x3df),X(0x3da),X(0x3d4),X(0x3cf),X(0x3c9),X(0x3c4),X(0x3bf),X(0x3b9),X(0x3b4),X(0x3ae),X(0x3a9),
    X(0x3a4),X(0x39f),X(0x399),X(0x394),X(0x38f),X(0x38a),X(0x384),X(0x37f),X(0x37a),X(0x375),X(0x370),X(0x36a),X(0x365),X(0x360),X(0x35b),X(0x356),
    X(0x351),X(0x34c),X(0x347),X(0x342),X(0x33d),X(0x338),X(0x333),X(0x32e),X(0x329),X(0x324),X(0x31f),X(0x31a),X(0x315),X(0x310),X(0x30b),X(0x306),
    X(0x302),X(0x2fd),X(0x2f8),X(0x2f3),X(0x2ee),X(0x2e9),X(0x2e5),X(0x2e0),X(0x2db),X(0x2d6),X(0x2d2),X(0x2cd),X(0x2c8),X(0x2c4),X(0x2bf),X(0x2ba),
    X(0x2b5),X(0x2b1),X(0x2ac),X(0x2a8),X(0x2a3),X(0x29e),X(0x29a),X(0x295),X(0x291),X(0x28c),X(0x288),X(0x283),X(0x27f),X(0x27a),X(0x276),X(0x271),
    X(0x26d),X(0x268),X(0x264),X(0x25f),X(0x25b),X(0x257),X(0x252),X(0x24e),X(0x249),X(0x245),X(0x241),X(0x23c),X(0x238),X(0x234),X(0x230),X(0x22b),
    X(0x227),X(0x223),X(0x21e),X(0x21a),X(0x216),X(0x212),X(0x20e),X(0x209),X(0x205),X(0x201),X(0x1fd),X(0x1f9),X(0x1f5),X(0x1f0),X(0x1ec),X(0x1e8),
    X(0x1e4),X(0x1e0),X(0x1dc),X(0x1d8),X(0x1d4),X(0x1d0),X(0x1cc),X(0x1c8),X(0x1c4),X(0x1c0),X(0x1bc),X(0x1b8),X(0x1b4),X(0x1b0),X(0x1ac),X(0x1a8),
    X(0x1a4),X(0x1a0),X(0x19c),X(0x199),X(0x195),X(0x191),X(0x18d),X(0x189),X(0x185),X(0x181),X(0x17e),X(0x17a),X(0x176),X(0x172),X(0x16f),X(0x16b),
    X(0x167),X(0x163),X(0x160),X(0x15c),X(0x158),X(0x154),X(0x151),X(0x14d),X(0x149),X(0x146),X(0x142),X(0x13e),X(0x13b),X(0x137),X(0x134),X(0x130),
    X(0x12c),X(0x129),X(0x125),X(0x122),X(0x11e),X(0x11b),X(0x117),X(0x114),X(0x110),X(0x10c),X(0x109),X(0x106),X(0x102),X(0x0ff),X(0x0fb),X(0x0f8),
    X(0x0f4),X(0x0f1),X(0x0ed),X(0x0ea),X(0x0e7),X(0x0e3),X(0x0e0),X(0x0dc),X(0x0d9),X(0x0d6),X(0x0d2),X(0x0cf),X(0x0cc),X(0x0c8),X(0x0c5),X(0x0c2),
    X(0x0be),X(0x0bb),X(0x0b8),X(0x0b5),X(0x0b1),X(0x0ae),X(0x0ab),X(0x0a8),X(0x0a4),X(0x0a1),X(0x09e),X(0x09b),X(0x098),X(0x094),X(0x091),X(0x08e),
    X(0x08b),X(0x088),X(0x085),X(0x082),X(0x07e),X(0x07b),X(0x078),X(0x075),X(0x072),X(0x06f),X(0x06c),X(0x069),X(0x066),X(0x063),X(0x060),X(0x05d),
    X(0x05a),X(0x057),X(0x054),X(0x051),X(0x04e),X(0x04b),X(0x048),X(0x045),X(0x042),X(0x03f),X(0x03c),X(0x039),X(0x036),X(0x033),X(0x030),X(0x02d),
    X(0x02a),X(0x028),X(0x025),X(0x022),X(0x01f),X(0x01c),X(0x019),X(0x016),X(0x014),X(0x011),X(0x00e),X(0x00b),X(0x008),X(0x006),X(0x003),X(0x000)};
#undef X

/* the envelope's increments: 8 steps of 4 bits per rate */
static const uint32_t EG_INC[64] DSP_HOT_DATA = {
    0x00000000, 0x00000000, 0x10101010, 0x10101010, 0x10101010, 0x10101010, 0x11101110, 0x11101110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x10101010, 0x10111010, 0x11101110, 0x11111110, 0x10101010, 0x10111010, 0x11101110, 0x11111110,
    0x11111111, 0x21112111, 0x21212121, 0x22212221, 0x22222222, 0x42224222, 0x42424242, 0x44424442,
    0x44444444, 0x84448444, 0x84848484, 0x88848884, 0x88888888, 0x88888888, 0x88888888, 0x88888888};

static const uint8_t DETUNE[32][4] = {
    {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 1, 2, 2}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3},
    {0, 1, 2, 4}, {0, 1, 3, 4}, {0, 1, 3, 4}, {0, 1, 3, 5}, {0, 2, 4, 5}, {0, 2, 4, 6}, {0, 2, 4, 6}, {0, 2, 5, 7},
    {0, 2, 5, 8}, {0, 3, 6, 8}, {0, 3, 6, 9}, {0, 3, 7, 10}, {0, 4, 8, 11}, {0, 4, 8, 12}, {0, 4, 9, 13}, {0, 5, 10, 14},
    {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}};

/* LFO PM: two shifts of the F-number's top 7 bits per depth and step */
static const uint8_t PM_SHIFTS[8][8] DSP_HOT_DATA = {
    {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77}, {0x77, 0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x72},
    {0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x17, 0x17}, {0x77, 0x77, 0x72, 0x72, 0x17, 0x17, 0x12, 0x12},
    {0x77, 0x77, 0x72, 0x17, 0x17, 0x17, 0x12, 0x07}, {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01},
    {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01}, {0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01}};

static const uint8_t LFO_MAX[8] DSP_HOT_DATA = {109, 78, 72, 68, 63, 45, 9, 6};

/* each algorithm: bit 0 OP2's input (0 none, 1 OP1), bits 1-3 OP3's, 4-6 OP4's (0 none, 1 OP1, 2 OP2,
 * 5 OP1+OP2, 6 OP1+OP3, 7 OP2+OP3), 7-9 OP1, OP2, OP3 sound too (OP4 always does) */
#define ALG(o2, o3, o4, c1, c2, c3) ((o2) | ((o3) << 1) | ((o4) << 4) | ((c1) << 7) | ((c2) << 8) | ((c3) << 9))
static const uint16_t ALGS[8] DSP_HOT_DATA = {
    ALG(1, 2, 3, 0, 0, 0), ALG(0, 5, 3, 0, 0, 0), ALG(0, 2, 6, 0, 0, 0), ALG(1, 0, 7, 0, 0, 0),
    ALG(1, 0, 3, 0, 1, 0), ALG(1, 1, 1, 0, 1, 1), ALG(1, 0, 0, 0, 1, 1), ALG(0, 0, 0, 1, 1, 1)};

/* the operators' register offsets in algorithm order: OP1 0x30, OP2 0x38, OP3 0x34, OP4 0x3C */
static const uint8_t OPREG[4] = {0, 8, 4, 12};

static const uint16_t CHBASE[OPN2_NCH] DSP_HOT_DATA = {0x000, 0x001, 0x002, 0x100, 0x101, 0x102};
DSP_INLINE uint32_t chbase(uint32_t ch) { return CHBASE[ch]; }

DSP_INLINE uint32_t eff_rate(uint32_t raw, uint32_t ks) { return raw ? (raw + ks > 63u ? 63u : raw + ks) : 0u; }

DSP_HOT static uint32_t phase_step(const opn2_t *c, uint32_t ch, const opn2_op_t *o, int32_t pm)
{
    uint32_t fnum = (o->block_freq & 0x7FFu) << 1, block, s;
    uint32_t pms = c->reg[0xB4u + chbase(ch)] & 7u;
    if (pms && pm) {
        uint32_t ap = (uint32_t)(pm < 0 ? -pm : pm), sh = PM_SHIFTS[pms][ap & 7u];
        uint32_t top = (o->block_freq >> 4) & 0x7Fu;
        int32_t adj = (int32_t)((top >> (sh & 15u)) + (top >> (sh >> 4)));
        if (pms > 5u)
            adj <<= pms - 5u;
        adj >>= 2;
        fnum = (uint32_t)((int32_t)fnum + (pm < 0 ? -adj : adj)) & 0xFFFu;
    }
    block = (o->block_freq >> 11) & 7u;
    s = (fnum << block) >> 2;
    s = (uint32_t)((int32_t)s + o->detune) & 0x1FFFFu;
    return (s * o->mul2) >> 1;
}

/* the operators' cached values from the registers */
DSP_HOT static void prepare_ch(opn2_t *c, uint32_t ch)
{
    uint32_t b = chbase(ch), i;
    opn2_ch_t *h = &c->ch[ch];
    h->alg = ALGS[c->reg[0xB0u + b] & 7u];
    h->fbk = (c->reg[0xB0u + b] >> 3) & 7u;
    h->pan = c->reg[0xB4u + b];
    h->pms = h->pan & 7u;
    h->am_sh = (uint8_t)((1u << (((h->pan >> 4) & 3u) ^ 3u)) - 1u);
    for (i = 0; i < 4u; i++) {
        opn2_op_t *o = &c->ch[ch].op[i];
        uint32_t r = b + OPREG[i], bf, kc, ks, dt, sl;
        bf = ((uint32_t)(c->reg[0xA4u + b] & 0x3Fu) << 8) | c->reg[0xA0u + b];
        if (ch == 2u && (c->reg[0x27] & 0xC0u)) {             /* channel 3's special mode: OP1-3 their own */
            static const uint8_t SPECIAL[3] = {1, 2, 0};      /* OP1: A9/AD, OP2: AA/AE, OP3: A8/AC */
            if (i < 3u) {
                uint32_t n = SPECIAL[i];
                bf = ((uint32_t)(c->reg[0xACu + n] & 0x3Fu) << 8) | c->reg[0xA8u + n];
            }
        }
        o->block_freq = (uint16_t)bf;
        kc = ((bf >> 10) & 15u) << 1;
        kc |= (0xFE80u >> ((bf >> 7) & 15u)) & 1u;
        dt = (c->reg[0x30u + r] >> 4) & 7u;
        o->detune = (int8_t)((dt & 4u) ? -(int32_t)DETUNE[kc][dt & 3u] : (int32_t)DETUNE[kc][dt & 3u]);
        o->mul2 = (uint8_t)((c->reg[0x30u + r] & 15u) * 2u);
        if (!o->mul2)
            o->mul2 = 1;
        o->step = phase_step(c, ch, o, 0);
        o->tl = (uint16_t)((c->reg[0x40u + r] & 0x7Fu) << 3);
        sl = c->reg[0x80u + r] >> 4;
        sl |= (sl + 1u) & 0x10u;
        o->sustain = (uint16_t)(sl << 5);
        ks = kc >> ((c->reg[0x50u + r] >> 6) ^ 3u);
        o->rate[EG_ATTACK] = (uint8_t)eff_rate((c->reg[0x50u + r] & 31u) * 2u, ks);
        o->rate[EG_DECAY] = (uint8_t)eff_rate((c->reg[0x60u + r] & 31u) * 2u, ks);
        o->rate[EG_SUSTAIN] = (uint8_t)eff_rate((c->reg[0x70u + r] & 31u) * 2u, ks);
        o->rate[EG_RELEASE] = (uint8_t)eff_rate((c->reg[0x80u + r] & 15u) * 4u + 2u, ks);
        o->ssg = c->reg[0x90u + r] & 15u;
        o->am = c->reg[0x60u + r] >> 7;
        o->pm_at = -128;
    }
    c->ssg_fix |= (uint8_t)(1u << ch);                      /* SSG-EG may be off now: see opn2_clock */
}

DSP_HOT static void start_attack(opn2_op_t *o, int restart)
{
    if (o->state == EG_ATTACK)
        return;
    o->state = EG_ATTACK;
    if (!restart) {
        o->ssg_inv = (o->ssg & 8u) && (o->ssg & 4u);
        o->phase = 0;
    }
    if (o->rate[EG_ATTACK] >= 62u)
        o->env = 0;
}

DSP_HOT static void start_release(opn2_op_t *o)
{
    if (o->state >= EG_RELEASE)
        return;
    o->state = EG_RELEASE;
    if (o->ssg_inv) {
        o->env = (uint16_t)((0x200u - o->env) & 0x3FFu);
        o->ssg_inv = 0;
    }
}

DSP_HOT static void clock_ssg(opn2_op_t *o)
{
    uint32_t mode = o->ssg & 7u;
    if (!(o->env & 0x200u))
        return;
    if (mode & 1u) {                                        /* hold */
        o->ssg_inv = (uint8_t)(((mode >> 2) ^ (mode >> 1)) & 1u);
        if (o->state != EG_ATTACK)
            o->env = o->ssg_inv ? 0x200u : 0x3FFu;
    } else {                                                /* repeat (alternating: inverts) */
        o->ssg_inv ^= (uint8_t)((mode >> 1) & 1u);
        if (o->state == EG_DECAY || o->state == EG_SUSTAIN)
            start_attack(o, 1);
        if (!(mode & 2u))
            o->phase = 0;
    }
    if (o->state == EG_RELEASE)
        o->env = 0x3FF;
}

/* go: bit s set when a rate of shift s steps on this count, ((cnt << s) & 0x7FF) == 0 (env_go) */
DSP_INLINE void clock_env(opn2_op_t *o, uint32_t cnt, uint32_t go)
{
    uint32_t rate, shift, inc;
    if (o->state == EG_ATTACK && o->env == 0)
        o->state = EG_DECAY;
    if (o->state == EG_DECAY && o->env >= o->sustain)
        o->state = EG_SUSTAIN;
    rate = o->rate[o->state];
    shift = rate >> 2;
    if (!((go >> shift) & 1u))
        return;
    cnt <<= shift;
    inc = (EG_INC[rate] >> (4u * ((cnt >> (shift <= 11u ? 11u : shift)) & 7u))) & 15u;
    if (o->state == EG_ATTACK) {
        if (rate < 62u)
            o->env = (uint16_t)(o->env + (((uint32_t)~o->env * inc) >> 4));
        o->env &= 0x3FFu;
    } else {
        uint32_t e = o->env;
        if (!(o->ssg & 8u))
            e += inc;
        else if (e < 0x200u)
            e += 4u * inc;
        if (e >= 0x400u)
            e = 0x3FF;
        o->env = (uint16_t)e;
    }
}

DSP_INLINE uint32_t env_go(uint32_t cnt)
{
    uint32_t tz = 0;
    while (tz < 11u && !((cnt >> tz) & 1u))
        tz++;
    return (0xFFFFu << (11u - tz)) & 0xFFFFu;
}

DSP_INLINE uint32_t env_att(const opn2_op_t *o, uint32_t am)
{
    uint32_t r = o->env;
    if (o->ssg_inv)
        r = (0x200u - r) & 0x3FFu;
    if (o->am)
        r += am;
    r += o->tl;
    return r > 0x3FFu ? 0x3FFu : r;
}

DSP_INLINE int32_t op_out(const opn2_op_t *o, uint32_t phase, uint32_t am)
{
    uint32_t s, idx, v;
    if (o->env > EG_QUIET)
        return 0;
    idx = phase & 0x3FFu;
    s = SIN_ATT[((idx & 0x100u) ? ~idx : idx) & 0xFFu];
    v = s + (env_att(o, am) << 2);
    v = (uint32_t)POWER[v & 0xFFu] >> (v >> 8);
    return (idx & 0x200u) ? -(int32_t)v : (int32_t)v;
}

DSP_INLINE int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

void opn2_reset(opn2_t *c, int chip)
{
    uint8_t *p = (uint8_t *)c;
    uint32_t i, ch;
    for (i = 0; i < sizeof(*c); i++)
        p[i] = 0;
    c->chip = (uint8_t)chip;
    c->reg[0xB4] = c->reg[0xB5] = c->reg[0xB6] = 0xC0;
    c->reg[0x1B4] = c->reg[0x1B5] = c->reg[0x1B6] = 0xC0;
    c->lfo_am = 0x3F;
    for (ch = 0; ch < OPN2_NCH; ch++)
        for (i = 0; i < 4u; i++) {
            c->ch[ch].op[i].env = 0x3FF;
            c->ch[ch].op[i].state = EG_RELEASE;
        }
    c->dac = 0;
    c->dirty = 0x3F;
}

static void timer_load(opn2_t *c)
{
    uint8_t m = c->reg[0x27];
    if (m & 1u) {
        if (!c->ta_run)
            c->ta_count = 1024 - (int32_t)(((uint32_t)c->reg[0x24] << 2) | (c->reg[0x25] & 3u));
        c->ta_run = 1;
    } else
        c->ta_run = 0;
    if (m & 2u) {
        if (!c->tb_run)
            c->tb_count = 16 * (256 - (int32_t)c->reg[0x26]);
        c->tb_run = 1;
    } else
        c->tb_run = 0;
    if (m & 0x10u)
        c->status &= (uint8_t)~1u;
    if (m & 0x20u)
        c->status &= (uint8_t)~2u;
}

void opn2_write(opn2_t *c, int port, uint8_t reg, uint8_t val)
{
    uint32_t idx = ((uint32_t)port & 1u) << 8 | reg;
    if (!port) {
        if (reg == 0x2A) {
            c->dac = (uint16_t)((c->dac & 1u) | ((uint32_t)(val ^ 0x80u) << 1));
            return;
        }
        if (reg == 0x2B) {
            c->dac_en = val >> 7;
            return;
        }
        if (reg == 0x2C) {
            c->dac = (uint16_t)((c->dac & ~1u) | ((val >> 3) & 1u));
            return;
        }
    }
    if (reg < 0x20u || (port && reg < 0x30u))
        return;
    if ((reg & 0xF0u) == 0xA0u) {                           /* frequencies: the high byte latches */
        uint32_t latch;
        if ((reg & 3u) == 3u)
            return;
        latch = 0xB8u | ((reg >> 3) & 1u);                  /* B8: A4-A6's latch, B9: AC-AE's (both ports) */
        if (reg & 4u)
            c->reg[latch] = val & 0x3Fu;
        else {
            c->reg[idx] = val;
            c->reg[idx | 4u] = c->reg[latch];
        }
        /* the channel's frequency; A8-AE: channel 3's special-mode ones */
        c->dirty |= (uint8_t)((reg & 8u) ? 1u << 2 : 1u << ((reg & 3u) + 3u * ((uint32_t)port & 1u)));
        return;
    }
    if ((reg & 0xF8u) == 0xB8u)
        return;
    c->reg[idx] = val;
    /* what the channels cache from the registers: their own registers, and channel 3's mode */
    if (reg >= 0x30u) {
        if ((reg & 3u) != 3u)
            c->dirty |= (uint8_t)(1u << ((reg & 3u) + 3u * ((uint32_t)port & 1u)));
    } else if (idx == 0x27u)
        c->dirty |= 1u << 2;
    if (idx == 0x28u) {                                     /* key on / off */
        uint32_t ch = val & 3u, i;
        if (ch == 3u)
            return;
        ch += ((val >> 2) & 1u) * 3u;
        for (i = 0; i < 4u; i++) {
            opn2_op_t *o = &c->ch[ch].op[i];               /* bits 4-7: OP1-OP4 */
            o->keyon = (uint8_t)((o->keyon & ~1u) | ((val >> (4u + i)) & 1u));
        }
        c->keys_dirty = 1;
    } else if (idx == 0x27u || idx == 0x24u || idx == 0x25u || idx == 0x26u)
        timer_load(c);
}

uint8_t opn2_status(const opn2_t *c) { return c->status; }

DSP_HOT static int32_t lfo_clock(opn2_t *c)
{
    uint32_t sub;
    int32_t pm;
    if (!(c->reg[0x22] & 8u)) {
        c->lfo_counter = 0;
        c->lfo_am = 0x3F;
        return 0;
    }
    sub = c->lfo_counter++ & 0xFFu;
    if (sub >= LFO_MAX[c->reg[0x22] & 7u])
        c->lfo_counter += 0x101u - sub;
    c->lfo_am = (uint8_t)((c->lfo_counter >> 8) & 0x3Fu);
    if (!((c->lfo_counter >> 14) & 1u))
        c->lfo_am ^= 0x3Fu;
    pm = (int32_t)((c->lfo_counter >> 10) & 7u);
    if ((c->lfo_counter >> 13) & 1u)
        pm ^= 7;
    return ((c->lfo_counter >> 14) & 1u) ? -pm : pm;
}

DSP_HOT static void timers_clock(opn2_t *c)
{
    if (c->ta_run && --c->ta_count <= 0) {
        if (c->reg[0x27] & 4u)
            c->status |= 1u;
        if ((c->reg[0x27] & 0xC0u) == 0x80u) {              /* CSM: timer A keys channel 3 on */
            uint32_t i;
            for (i = 0; i < 4u; i++)
                c->ch[2].op[i].keyon |= 2u;
            c->keys_dirty = 1;
        }
        c->ta_count = 1024 - (int32_t)(((uint32_t)c->reg[0x24] << 2) | (c->reg[0x25] & 3u));
    }
    if (c->tb_run && --c->tb_count <= 0) {
        if (c->reg[0x27] & 8u)
            c->status |= 2u;
        c->tb_count = 16 * (256 - (int32_t)c->reg[0x26]);
    }
}

DSP_INLINE int32_t ladder(int32_t v) { return v < 0 ? v - 3 : v + 4; }

/* an operator at rest: released and fully attenuated. Nothing it does can be heard until a key-on,
 * and a key-on starts its phase over: its envelope, SSG-EG and phase need not run meanwhile */
DSP_INLINE int op_idle(const opn2_op_t *o) { return o->env == 0x3FFu && o->state == EG_RELEASE; }

/* a sample's start, after the timers: the registers' effects, then the key states (only after a key
 * request), then SSG-EG switched off */
DSP_INLINE void begin(opn2_t *c)
{
    uint32_t ch, i;
    /* prepare: the registers' effects, then the key states (only after a key request) */
    if (c->dirty) {
        for (ch = 0; ch < OPN2_NCH; ch++)
            if (c->dirty & (1u << ch))
                prepare_ch(c, ch);
        c->dirty = 0;
    }
    if (c->keys_dirty) {
        uint32_t csm = 0;
        for (ch = 0; ch < OPN2_NCH; ch++)
            for (i = 0; i < 4u; i++) {
                opn2_op_t *o = &c->ch[ch].op[i];
                uint8_t k = o->keyon != 0;
                if (k != o->key) {
                    o->key = k;
                    if (k)
                        start_attack(o, 0);
                    else
                        start_release(o);
                }
                csm |= o->keyon & 2u;
                o->keyon &= 1u;                             /* a CSM key-on lasts one sample */
            }
        c->keys_dirty = csm != 0;                           /* its end is looked at next sample */
    }
    if (c->ssg_fix) {                                       /* an operator without SSG-EG is never inverted */
        for (ch = 0; ch < OPN2_NCH; ch++)
            if (c->ssg_fix & (1u << ch))
                for (i = 0; i < 4u; i++)
                    if (!(c->ch[ch].op[i].ssg & 8u))
                        c->ch[ch].op[i].ssg_inv = 0;
        c->ssg_fix = 0;
    }
}

/* a sample after its start (begin) */
DSP_HOT static void clock_rest(opn2_t *c, int32_t *l, int32_t *r)
{
    uint32_t ch, i, lfo_on, envclk, envcnt, envgo;
    int32_t pm, sl = 0, sr = 0;

    if (((++c->env_counter) & 3u) == 3u)
        c->env_counter++;
    envclk = (c->env_counter & 3u) == 0;
    envcnt = c->env_counter >> 2;
    envgo = envclk ? env_go(envcnt) : 0;
    pm = lfo_clock(c);
    c->lfo_pm = (int8_t)pm;
    lfo_on = c->reg[0x22] & 8u;

    for (ch = 0; ch < OPN2_NCH; ch++) {
        opn2_ch_t *h = &c->ch[ch];
        uint32_t fbk, alg, am, busy = 0, pm_on = lfo_on && h->pms;
        int32_t out[8], res, mod;
        h->fb[0] = h->fb[1];
        h->fb[1] = h->fb_in;
        for (i = 0; i < 4u; i++) {
            opn2_op_t *o = &h->op[i];
            if (op_idle(o))
                continue;
            busy = 1;
            if (o->ssg & 8u)                                /* (without, ssg_inv stays 0: ssg_fix) */
                clock_ssg(o);
            if (envclk)
                clock_env(o, envcnt, envgo);
            if (pm_on) {
                if (o->pm_at != pm) {
                    o->pm_step = phase_step(c, ch, o, pm);
                    o->pm_at = (int8_t)pm;
                }
                o->phase += o->pm_step;
            } else
                o->phase += o->step;
        }
        if (!busy) {                                        /* all four at rest: silent */
            res = 0;
            h->fb_in = 0;
        } else {
            fbk = h->fbk;
            alg = h->alg;
            am = (uint32_t)(c->lfo_am << 1) >> h->am_sh;
            mod = fbk ? (h->fb[0] + h->fb[1]) >> (10u - fbk) : 0;
            out[0] = 0;
            out[1] = h->fb_in = (int16_t)op_out(&h->op[0], (h->op[0].phase >> 10) + (uint32_t)mod, am);
            if (ch == 5u && c->dac_en)
                res = 0;                                    /* OP1 only, for its feedback */
            else {
                out[2] = op_out(&h->op[1], (h->op[1].phase >> 10) + (uint32_t)(out[alg & 1u] >> 1), am);
                out[5] = out[1] + out[2];
                out[3] = op_out(&h->op[2], (h->op[2].phase >> 10) + (uint32_t)(out[(alg >> 1) & 7u] >> 1), am);
                out[6] = out[1] + out[3];
                out[7] = out[2] + out[3];
                res = op_out(&h->op[3], (h->op[3].phase >> 10) + (uint32_t)(out[(alg >> 4) & 7u] >> 1), am) >> 5;
                if (alg & 0x080u)
                    res = clampi(res + (out[1] >> 5), -257, 256);
                if (alg & 0x100u)
                    res = clampi(res + (out[2] >> 5), -257, 256);
                if (alg & 0x200u)
                    res = clampi(res + (out[3] >> 5), -257, 256);
            }
        }
        if (ch == 5u && c->dac_en)
            res = (int32_t)(int16_t)(c->dac << 7) >> 7;     /* the DAC replaces channel 6 */
        if (c->mute & (1u << ch))
            res = 0;
        c->out[ch] = (int16_t)res;
        if (c->chip == OPN2_YM2612) {
            sl += ladder((h->pan & 0x80u) ? res : 0);
            sr += ladder((h->pan & 0x40u) ? res : 0);
        } else {
            sl += (h->pan & 0x80u) ? res : 0;
            sr += (h->pan & 0x40u) ? res : 0;
        }
    }
    *l = sl;
    *r = sr;
}

DSP_HOT void opn2_clock(opn2_t *c, int32_t *l, int32_t *r)
{
    timers_clock(c);
    begin(c);
    clock_rest(c, l, r);
}

/* ------------------------------------------------------------ a block --- */
#define SILENT 0xFFFFu                                      /* an operator's attenuation when quiet */

static struct {
    uint32_t go[OPN2_BLOCK];                                /* the envelope's step mask (env_go), 0: no step */
    uint32_t cnt[OPN2_BLOCK];                               /* the envelope's count */
    int8_t pm[OPN2_BLOCK];                                  /* the LFO's PM and AM */
    uint8_t am[OPN2_BLOCK];
    uint16_t ph[4][OPN2_BLOCK], att[4][OPN2_BLOCK];         /* a channel's operators: phase >> 10, attenuation */
} blk;

/* one operator through the block: its envelope, SSG-EG and phase, sample by sample as opn2_clock runs
 * them (the operators do not affect each other there), and what op_out needs of it at each sample.
 * Its attenuation (env_att) is worked out again only when the envelope or SSG-EG moved it. Returns
 * the samples it was not at rest for: at rest it stays so (no key-on within a block). */
DSP_INLINE uint32_t op_block(opn2_t *c, uint32_t ch, opn2_op_t *o, uint32_t pm_on, uint32_t am_sh,
                             uint16_t *restrict ph, uint16_t *restrict att, uint32_t n)
{
    uint32_t k = 0, j, base = 0, quiet = 0, moved = 1, ssg = o->ssg & 8u, am = o->am;
    uint32_t phase = o->phase, step = o->step;
    int32_t pm_at = o->pm_at;
    if (op_idle(o))
        goto rest;
    if (pm_on && pm_at != -128)
        step = o->pm_step;
    for (; k < n; k++) {
        if (ssg) {                                          /* (it may restart the phase) */
            o->phase = phase;
            clock_ssg(o);
            phase = o->phase;
            moved = 1;
        }
        if (blk.go[k]) {
            clock_env(o, blk.cnt[k], blk.go[k]);
            moved = 1;
        }
        if (pm_on && blk.pm[k] != pm_at) {
            pm_at = blk.pm[k];
            step = phase_step(c, ch, o, pm_at);
        }
        phase += step;
        ph[k] = (uint16_t)(phase >> 10);
        if (moved) {                                        /* env_att without the LFO's AM */
            uint32_t e = o->env;
            quiet = e > EG_QUIET;
            if (o->ssg_inv)
                e = (0x200u - e) & 0x3FFu;
            base = e + o->tl;
            if (!am && base > 0x3FFu)
                base = 0x3FF;
        }
        if (quiet)
            att[k] = SILENT;
        else if (am) {
            uint32_t a = base + ((uint32_t)(blk.am[k] << 1) >> am_sh);
            att[k] = (uint16_t)(a > 0x3FFu ? 0x3FFu : a);
        } else
            att[k] = (uint16_t)base;
        if (moved) {
            moved = 0;
            if (op_idle(o)) {                               /* at rest from the next sample on */
                k++;
                break;
            }
        }
    }
    o->phase = phase;
    if (pm_on) {
        o->pm_at = (int8_t)pm_at;
        o->pm_step = step;
    }
rest:
    for (j = k; j < n; j++)
        att[j] = SILENT;
    return k;
}

/* op_out from op_block's record */
DSP_INLINE int32_t op_rec(uint32_t phase, uint32_t att)
{
    uint32_t s, idx, v;
    if (att == SILENT)
        return 0;
    idx = phase & 0x3FFu;
    s = SIN_ATT[((idx & 0x100u) ? ~idx : idx) & 0xFFu];
    v = s + (att << 2);
    v = (uint32_t)POWER[v & 0xFFu] >> (v >> 8);
    return (idx & 0x200u) ? -(int32_t)v : (int32_t)v;
}

DSP_HOT void opn2_render(opn2_t *c, uint32_t n, const uint16_t *dac, int32_t *l, int32_t *r,
                         int16_t (*outs)[OPN2_NCH])
{
    uint32_t ch, i, k, lfo_on;
    if (n > OPN2_BLOCK)
        n = OPN2_BLOCK;
    /* CSM can key channel 3 on at any sample: then a sample at a time */
    if ((c->reg[0x27] & 0xC0u) == 0x80u && c->ta_run)
        goto by_sample;
    timers_clock(c);
    begin(c);
    if (c->keys_dirty)                                      /* a CSM key-on's end still to come */
        goto by_sample_1;
    /* what the whole chip does at each sample: the timers, the envelope clock, the LFO */
    for (k = 0; k < n; k++) {
        uint32_t ec;
        if (k)
            timers_clock(c);
        if (((++c->env_counter) & 3u) == 3u)
            c->env_counter++;
        ec = c->env_counter;
        blk.cnt[k] = ec >> 2;
        blk.go[k] = (ec & 3u) == 0 ? env_go(ec >> 2) : 0;
        blk.pm[k] = (int8_t)lfo_clock(c);
        blk.am[k] = c->lfo_am;
        l[k] = r[k] = 0;
    }
    c->lfo_pm = blk.pm[n - 1];
    lfo_on = c->reg[0x22] & 8u;

    for (ch = 0; ch < OPN2_NCH; ch++) {
        opn2_ch_t *h = &c->ch[ch];
        uint32_t live = 0, alg = h->alg, fbk = h->fbk, am_sh = h->am_sh, pan = h->pan;
        uint32_t dac_ch = ch == 5u && c->dac_en, mute = (c->mute >> ch) & 1u;
        int32_t fb0 = h->fb[0], fb1 = h->fb[1], fb_in = h->fb_in, res = 0;
        for (i = 0; i < 4u; i++) {                          /* how long any operator runs */
            uint32_t m = op_block(c, ch, &h->op[i], lfo_on && h->pms, am_sh, blk.ph[i], blk.att[i], n);
            if (m > live)
                live = m;
        }
        for (k = 0; k < n; k++) {
            fb0 = fb1;
            fb1 = fb_in;
            if (k >= live) {                                /* all four at rest */
                res = 0;
                fb_in = 0;
            } else {
                int32_t out[8], mod = fbk ? (fb0 + fb1) >> (10u - fbk) : 0;
                out[0] = 0;
                out[1] = fb_in = (int16_t)op_rec(blk.ph[0][k] + (uint32_t)mod, blk.att[0][k]);
                if (dac_ch)
                    res = 0;
                else {
                    out[2] = op_rec(blk.ph[1][k] + (uint32_t)(out[alg & 1u] >> 1), blk.att[1][k]);
                    out[5] = out[1] + out[2];
                    out[3] = op_rec(blk.ph[2][k] + (uint32_t)(out[(alg >> 1) & 7u] >> 1), blk.att[2][k]);
                    out[6] = out[1] + out[3];
                    out[7] = out[2] + out[3];
                    res = op_rec(blk.ph[3][k] + (uint32_t)(out[(alg >> 4) & 7u] >> 1), blk.att[3][k]) >> 5;
                    if (alg & 0x080u)
                        res = clampi(res + (out[1] >> 5), -257, 256);
                    if (alg & 0x100u)
                        res = clampi(res + (out[2] >> 5), -257, 256);
                    if (alg & 0x200u)
                        res = clampi(res + (out[3] >> 5), -257, 256);
                }
            }
            if (dac_ch)
                res = (int32_t)(int16_t)(dac[k] << 7) >> 7;  /* the DAC replaces channel 6 */
            if (mute)
                res = 0;
            outs[k][ch] = (int16_t)res;
            if (c->chip == OPN2_YM2612) {
                l[k] += ladder((pan & 0x80u) ? res : 0);
                r[k] += ladder((pan & 0x40u) ? res : 0);
            } else {
                l[k] += (pan & 0x80u) ? res : 0;
                r[k] += (pan & 0x40u) ? res : 0;
            }
        }
        h->fb[0] = (int16_t)fb0;
        h->fb[1] = (int16_t)fb1;
        h->fb_in = (int16_t)fb_in;
        c->out[ch] = (int16_t)res;
    }
    c->dac = dac[n - 1];
    return;

by_sample:
    for (k = 0; k < n; k++) {
        c->dac = dac[k];
        opn2_clock(c, &l[k], &r[k]);
        for (ch = 0; ch < OPN2_NCH; ch++)
            outs[k][ch] = c->out[ch];
    }
    return;
by_sample_1:                                                /* the first sample's start is done */
    c->dac = dac[0];
    clock_rest(c, &l[0], &r[0]);
    for (ch = 0; ch < OPN2_NCH; ch++)
        outs[0][ch] = c->out[ch];
    for (k = 1; k < n; k++) {
        c->dac = dac[k];
        opn2_clock(c, &l[k], &r[k]);
        for (ch = 0; ch < OPN2_NCH; ch++)
            outs[k][ch] = c->out[ch];
    }
}
