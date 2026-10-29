/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the sound driver (player.h). */
#include "player.h"

player_t player;
const sample_t *bank;
uint32_t bank_n;

static fm_patch_t cur[NCH];            /* each FM channel's patch as the effects have changed it */
static uint8_t cur_ins[NCH];           /* the instrument cur[] was loaded from (0xFF: none) */
static uint8_t ext_keys;               /* FM3 extended: its operators' keys */
static uint32_t keyon_mask;            /* channels to key on after the next sample */
static uint8_t last_tl[NCH][4];
static uint16_t last_freq[NCH];        /* FM: block << 11 | fnum; PSG: period (0xFFFF: none written) */
static uint8_t last_att[NCH];

/* ------------------------------------------------------------- pitch --- */
static float exp2f_frac(float x)       /* 2^x for 0 <= x < 1 (no libm on the FM-1) */
{
    float t = 1.0f, s = 1.0f, y = x * 0.69314718f;
    int k;
    for (k = 1; k < 12; k++) {
        t *= y / (float)k;
        s += t;
    }
    return s;
}

#define C0_HZ 16.351598f
#define PSG_DIV 32.0f

static float pitch_hz0(int32_t p, int32_t *oct)   /* the pitch's octave and its frequency an octave 0 */
{
    if (p < 0)
        p = 0;
    *oct = p / 768;
    return C0_HZ * exp2f_frac((float)(p % 768) / 768.0f);
}

static uint16_t fm_freq(int32_t p)     /* block << 11 | fnum */
{
    int32_t oct;
    float f = pitch_hz0(p, &oct);
    float fn = f * 2097152.0f / (float)player.md.fm_hz;
    uint32_t n;
    while (oct > 7) {
        fn *= 2.0f;
        oct--;
    }
    n = (uint32_t)(fn + 0.5f);
    if (n > 2047u)
        n = 2047u;
    return (uint16_t)((uint32_t)oct << 11 | n);
}

static uint16_t psg_period(int32_t p)
{
    static const float CLK[2] = {3579545.0f, 3546893.0f};
    int32_t oct;
    float f = pitch_hz0(p, &oct);
    float per = CLK[player.md.region] / (PSG_DIV * f * (float)(1u << (oct > 20 ? 20 : oct)));
    uint32_t n = (uint32_t)(per + 0.5f);
    return (uint16_t)(n < 1u ? 1u : n > 1023u ? 1023u : n);
}

void note_name(uint8_t note, char out[4])
{
    static const char N[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
    if (note == NOTE_OFF) {
        out[0] = out[1] = out[2] = '=';
    } else if (note == NOTE_CUT) {
        out[0] = out[1] = out[2] = '^';
    } else if (note == 0 || note > NOTE_MAX) {
        out[0] = out[1] = out[2] = '-';
    } else {
        uint32_t n = note - 1u;
        out[0] = N[(n % 12u) * 2u];
        out[1] = N[(n % 12u) * 2u + 1u];
        out[2] = (char)('0' + n / 12u);
    }
    out[3] = 0;
}

/* ---------------------------------------------------------- registers --- */
static const uint8_t OPREG[4] = {0, 8, 4, 12};

static int hw_of(int ch)               /* the YM2612 channel a tracker channel plays on */
{
    return ch >= CH_OP2 ? 2 : ch;
}
static int op_of(int ch)               /* FM3 extended: the operator a channel is (0..3); else -1 */
{
    if (!player.ext3)
        return -1;
    if (ch == CH_FM3)
        return 0;
    return ch >= CH_OP2 ? ch - CH_OP2 + 1 : -1;
}

static void fmw(int hw, uint8_t base, uint8_t v) { md_fm(&player.md, hw >= 3, (uint8_t)(base + hw % 3), v); }
static void fmw_op(int hw, int op, uint8_t base, uint8_t v)
{
    md_fm(&player.md, hw >= 3, (uint8_t)(base + hw % 3 + OPREG[op]), v);
}

static void fm_key(int ch, int on)
{
    int hw = hw_of(ch), op = op_of(ch);
    uint8_t code = (uint8_t)(hw < 3 ? hw : hw + 1), mask;
    if (op >= 0) {
        if (on)
            ext_keys |= (uint8_t)(1u << op);
        else
            ext_keys &= (uint8_t)~(1u << op);
        mask = ext_keys;
    } else
        mask = on ? 15u : 0u;
    md_fm(&player.md, 0, 0x28, (uint8_t)(mask << 4 | code));
}

static void fm_write_op(int ch, int op)
{
    int hw = hw_of(ch);
    const fm_op_t *o = &cur[ch].op[op];
    fmw_op(hw, op, 0x30, (uint8_t)((o->dt & 7u) << 4 | (o->mul & 15u)));
    fmw_op(hw, op, 0x50, (uint8_t)((o->ks & 3u) << 6 | (o->ar & 31u)));
    fmw_op(hw, op, 0x60, (uint8_t)((o->am & 1u) << 7 | (o->d1r & 31u)));
    fmw_op(hw, op, 0x70, (uint8_t)(o->d2r & 31u));
    fmw_op(hw, op, 0x80, (uint8_t)((o->sl & 15u) << 4 | (o->rr & 15u)));
    fmw_op(hw, op, 0x90, (uint8_t)(o->ssg & 15u));
    last_tl[ch][op] = 0xFF;            /* written with the volume */
}

static void fm_write_chan(int ch)      /* B0 / B4: algorithm, feedback, pan, LFO sensitivities */
{
    int hw = hw_of(ch);
    const fm_patch_t *p = &cur[ch];
    fmw(hw, 0xB0, (uint8_t)((p->fb & 7u) << 3 | (p->alg & 7u)));
    fmw(hw, 0xB4, (uint8_t)((player.ch[ch].pan & 0xC0u) | (p->ams & 3u) << 4 | (p->pms & 7u)));
}

static void fm_load(int ch, int ins)   /* the instrument's patch into the channel */
{
    int op = op_of(ch), i;
    const ins_t *in = &song.ins[ins];
    if (in->type != INS_FM)
        return;
    cur[ch] = in->u.fm;
    cur_ins[ch] = (uint8_t)ins;
    if (op >= 0) {                     /* FM3 extended: one operator; FM3 itself also the channel's */
        if (op > 0)
            cur[ch].op[op] = in->u.fm.op[0];
        fm_write_op(ch, op);
        if (op == 0)
            fm_write_chan(ch);
        return;
    }
    for (i = 0; i < 4; i++)
        fm_write_op(ch, i);
    fm_write_chan(ch);
}

int ins_is_carrier(int alg, int op)
{
    static const uint8_t CARRIERS[8] = {8, 8, 8, 8, 10, 14, 14, 15};   /* bit per operator */
    return (CARRIERS[alg & 7] >> op) & 1;
}

static int macro_val(const chan_t *c, const ins_t *in, int m, int dflt)
{
    const macro_t *mc = &in->mac[m];
    if (!mc->len)
        return dflt;
    return mc->v[c->mpos[m] < mc->len ? c->mpos[m] : mc->len - 1u];
}

static void fm_volume(int ch)
{
    chan_t *c = &player.ch[ch];
    const ins_t *in = &song.ins[c->ins];
    int op = op_of(ch), i, vol = c->vol, mv;
    mv = in->type == INS_FM ? macro_val(c, in, MAC_VOL, 127) : 127;
    vol = vol * mv / 127;
    for (i = 0; i < 4; i++) {
        uint32_t tl = cur[ch].op[i].tl;
        if (op >= 0 && op != i)
            continue;
        if (op >= 0 || ins_is_carrier(cur[ch].alg, i))
            tl += (uint32_t)(127 - vol);
        if (tl > 127u)
            tl = 127u;
        if (tl != last_tl[ch][i]) {
            last_tl[ch][i] = (uint8_t)tl;
            fmw_op(hw_of(ch), i, 0x40, (uint8_t)tl);
        }
    }
    c->level = (uint8_t)(c->key ? vol >> 3 : 0);
}

static void fm_pitch(int ch, int32_t p)
{
    uint16_t f = fm_freq(p);
    int hw = hw_of(ch), op = op_of(ch);
    player.ch[ch].out_fnum = f;
    if (f == last_freq[ch])
        return;
    last_freq[ch] = f;
    if (op >= 0) {
        /* FM3 extended: OP1 A9/AD, OP2 AA/AE, OP3 A8/AC, OP4 the channel's A2/A6 */
        static const uint8_t LO[4] = {0xA9, 0xAA, 0xA8, 0xA2};
        uint8_t lo = LO[op];
        md_fm(&player.md, 0, (uint8_t)(lo + 4u), (uint8_t)(f >> 8));
        md_fm(&player.md, 0, lo, (uint8_t)f);
        return;
    }
    fmw(hw, 0xA4, (uint8_t)(f >> 8));
    fmw(hw, 0xA0, (uint8_t)f);
}

static void psg_out(int ch, int32_t p, int att)
{
    chan_t *c = &player.ch[ch];
    int n = ch - CH_SQ1;
    uint16_t per;
    if (ch == CH_NOI) {
        if ((c->noise & 3u) == 3u) {   /* noise at tone 3's pitch: the noise channel sets SQ3's period */
            per = psg_period(p);
            if (per != last_freq[CH_SQ3]) {
                last_freq[CH_SQ3] = per;
                md_psg(&player.md, (uint8_t)(0xC0 | (per & 15u)));
                md_psg(&player.md, (uint8_t)(per >> 4));
            }
            c->out_period = per;
        }
    } else {
        per = psg_period(p);
        c->out_period = per;
        if (per != last_freq[ch]) {
            last_freq[ch] = per;
            md_psg(&player.md, (uint8_t)(0x80 | n << 5 | (per & 15u)));
            md_psg(&player.md, (uint8_t)(per >> 4));
        }
    }
    if (att != last_att[ch]) {
        last_att[ch] = (uint8_t)att;
        md_psg(&player.md, (uint8_t)(0x90 | n << 5 | att));
    }
    c->level = (uint8_t)(15 - att);
}

static void noise_mode(int m)
{
    player.ch[CH_NOI].noise = (uint8_t)(m & 7);
    md_psg(&player.md, (uint8_t)(0xE0 | (m & 7)));
}

static void dac_enable(int on)
{
    if (player.dac == on)
        return;
    player.dac = (uint8_t)on;
    md_fm(&player.md, 0, 0x2B, on ? 0x80 : 0x00);
    if (!on)
        md_dac_stop(&player.md);
}

static void ext3_set(int on)
{
    player.ext3 = (uint8_t)on;
    md_fm(&player.md, 0, 0x27, on ? 0x40 : 0x00);
    last_freq[CH_FM3] = 0xFFFF;
}

/* --------------------------------------------------------------- notes --- */
static void macros_start(chan_t *c)
{
    int m;
    for (m = 0; m < NMAC; m++)
        c->mpos[m] = 0;
    c->released = 0;
    c->mpitch = 0;
}

static void macros_release(chan_t *c, const ins_t *in)
{
    int m;
    c->released = 1;
    for (m = 0; m < NMAC; m++)
        if (in->mac[m].len && in->mac[m].rel != MACRO_NONE && c->mpos[m] <= in->mac[m].rel)
            c->mpos[m] = (uint8_t)(in->mac[m].rel + 1u);
}

static void macros_step(chan_t *c, const ins_t *in)
{
    int m;
    for (m = 0; m < NMAC; m++) {
        const macro_t *mc = &in->mac[m];
        uint32_t end;
        if (!mc->len)
            continue;
        if (m == MAC_PITCH && c->mpos[m] < mc->len)
            c->mpitch = (int16_t)(c->mpitch + mc->v[c->mpos[m]]);
        c->mpos[m]++;
        end = (!c->released && mc->rel != MACRO_NONE) ? mc->rel + 1u : mc->len;
        if (c->mpos[m] >= end) {
            /* the loop repeats while held; after the release only a loop past the release point */
            int rel_done = c->released && mc->rel != MACRO_NONE;
            if (mc->loop != MACRO_NONE && mc->loop < end && (!rel_done || mc->loop > mc->rel))
                c->mpos[m] = mc->loop;
            else
                c->mpos[m] = (uint8_t)(end - 1u);
        }
    }
}

static void note_off(int ch)
{
    chan_t *c = &player.ch[ch];
    const ins_t *in = &song.ins[c->ins];
    if (ch_is_fm(ch)) {
        if (ch == CH_FM6 && in->type == INS_DAC) {
            md_dac_stop(&player.md);
        } else if (c->key)
            fm_key(ch, 0);
        keyon_mask &= ~(1u << ch);
    }
    macros_release(c, in);
    c->key = 0;
}

static void note_cut(int ch)
{
    chan_t *c = &player.ch[ch];
    note_off(ch);
    c->note = 0;
    if (ch_is_psg(ch))
        psg_out(ch, c->pitch, 15);
}

static void note_on(int ch, uint8_t note)
{
    chan_t *c = &player.ch[ch];
    const ins_t *in = &song.ins[c->ins];
    c->note = note;
    c->pitch = (int32_t)(note - 1u) * 64;
    c->nslide_n = 0;
    c->vib_pos = 0;
    c->arp_pos = 0;
    macros_start(c);
    if (ch == CH_FM6 && in->type == INS_DAC) {
        if (in->u.dac.sample < bank_n) {
            const sample_t *s = &bank[in->u.dac.sample];
            uint32_t rate = s->rate;
            if (in->u.dac.pitched) {
                int32_t d = ((int32_t)note - in->u.dac.root) * 64;
                int32_t oct = 0;
                float r;
                while (d < 0) {
                    d += 768;
                    oct--;
                }
                oct += d / 768;
                r = exp2f_frac((float)(d % 768) / 768.0f);
                r = oct >= 0 ? r * (float)(1u << (oct > 4 ? 4 : oct)) : r / (float)(1u << (-oct > 8 ? 8 : -oct));
                rate = (uint32_t)((float)rate * r);
            }
            dac_enable(1);
            md_dac_play(&player.md, s->data, s->len, in->u.dac.loop ? 0u : 0xFFFFFFFFu, rate);
        }
        c->key = 1;
        c->level = 15;
        return;
    }
    if (ch_is_fm(ch)) {
        if (ch == CH_FM6)
            dac_enable(0);
        if (in->type == INS_FM && cur_ins[ch] != c->ins)
            fm_load(ch, c->ins);
        if (c->key)
            fm_key(ch, 0);
        keyon_mask |= 1u << ch;
    } else if (ch == CH_NOI && in->mac[MAC_NOISE].len == 0) {
        noise_mode(c->noise);          /* a new note restarts the noise, as a driver writing its mode */
    }
    c->key = 1;
}

/* ------------------------------------------------------------- effects --- */
static void set_speed(int i, uint8_t v)
{
    if (!v)
        return;
    player.speed[i] = v;
}

static void cell_fx(int ch, uint8_t fx, uint8_t v, int first)
{
    chan_t *c = &player.ch[ch];
    int fm = ch_is_fm(ch);
    (void)first;
    switch (fx) {
    case FX_ARP: c->arp = v; break;
    case FX_PORTA_UP: c->slide = (int16_t)v; break;
    case FX_PORTA_DN: c->slide = (int16_t)-v; break;
    case FX_TONE_PORTA: if (v) c->porta = v; break;
    case FX_VIBRATO:
        c->vib_speed = v >> 4;
        c->vib_depth = v & 15u;
        break;
    case FX_PAN:
        if (fm) {
            c->pan = (uint8_t)((v & 0xF0u ? 0x80u : 0u) | (v & 0x0Fu ? 0x40u : 0u));
            fm_write_chan(ch == CH_OP2 || ch == CH_OP3 || ch == CH_OP4 ? CH_FM3 : ch);
        }
        break;
    case FX_SPEED1: set_speed(0, v); break;
    case FX_SPEED: set_speed(0, v); set_speed(1, v); break;
    case FX_VOL_SLIDE: c->vslide = (int8_t)((v >> 4) ? (int)(v >> 4) : -(int)(v & 15u)); break;
    case FX_JUMP: player.jump = v; break;
    case FX_BREAK: player.brk = v; break;
    case FX_RETRIG: c->retrig = v; break;
    case FX_LFO:
        player.lfo = (uint8_t)((v & 0xF0u ? 8u : 0u) | (v & 7u));
        md_fm(&player.md, 0, 0x22, player.lfo);
        break;
    case FX_FB:
        if (fm) {
            cur[ch].fb = v & 7u;
            fm_write_chan(ch);
        }
        break;
    case FX_TL1: case FX_TL2: case FX_TL3: case FX_TL4:
        if (fm) {
            cur[ch].op[fx - FX_TL1].tl = v & 0x7Fu;
            last_tl[ch][fx - FX_TL1] = 0xFF;
        }
        break;
    case FX_MUL:
        if (fm && (v >> 4) >= 1u && (v >> 4) <= 4u) {
            cur[ch].op[(v >> 4) - 1u].mul = v & 15u;
            fm_write_op(ch, (v >> 4) - 1);
        }
        break;
    case FX_DAC: dac_enable(v != 0); break;
    case FX_EXT3: ext3_set(v != 0); break;
    case FX_AR: case FX_AR1: case FX_AR2: case FX_AR3: case FX_AR4:
        if (fm) {
            int i;
            for (i = 0; i < 4; i++)
                if (fx == FX_AR || fx - FX_AR1 == i) {
                    cur[ch].op[i].ar = v & 31u;
                    fm_write_op(ch, i);
                }
        }
        break;
    case FX_NOISE:
        if (ch == CH_NOI)
            noise_mode((v & 0xF0u ? 4 : 0) | (v & 3u));
        break;
    case FX_SLIDE_UP: case FX_SLIDE_DN:
        c->nslide_speed = (uint8_t)((v >> 4) * 8u + 4u);
        c->nslide_n = (int8_t)(fx == FX_SLIDE_UP ? (int)(v & 15u) : -(int)(v & 15u));
        c->target = c->pitch + c->nslide_n * 64;
        break;
    case FX_FINE: c->fine = (int8_t)((int)v - 0x80); break;
    case FX_CUT: c->cut = v; break;
    default: break;
    }
}

static void apply_cell(int ch, const cell_t *cl)
{
    chan_t *c = &player.ch[ch];
    int k, porta = 0;
    for (k = 0; k < 2; k++)
        if (cl->fx[k] == FX_TONE_PORTA)
            porta = 1;
    if (cl->ins != VAL_NONE && song.ins[cl->ins].type != INS_EMPTY) {
        if (cl->ins != c->ins)
            cur_ins[ch] = 0xFF;        /* reloaded on the next note */
        c->ins = cl->ins;
    }
    if (cl->vol != VAL_NONE)
        c->vol = ch_is_psg(ch) ? (cl->vol > 15u ? 15u : cl->vol) : (cl->vol > 127u ? 127u : cl->vol);
    c->retrig = c->cut = 0;
    for (k = 0; k < 2; k++)
        if (cl->fx[k])
            cell_fx(ch, cl->fx[k], cl->fxv[k], 1);
    if (cl->note == NOTE_OFF)
        note_off(ch);
    else if (cl->note == NOTE_CUT)
        note_cut(ch);
    else if (cl->note >= 1u && cl->note <= NOTE_MAX) {
        if (porta && c->key && c->note) {
            c->target = (int32_t)(cl->note - 1u) * 64;
            c->note = cl->note;
        } else
            note_on(ch, cl->note);
    }
}

/* ---------------------------------------------------------------- ticks --- */
static const int8_t SINE[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127, 126, 125, 122, 117, 112,
    106, 98, 90, 81, 71, 60, 49, 37, 25, 12, 0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112,
    -117, -122, -125, -126, -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12};

static int chan_on(int ch)             /* the channel exists in the song's setup */
{
    return ch < NCH_STD || player.ext3;
}

static void chan_tick(int ch)
{
    chan_t *c = &player.ch[ch];
    const ins_t *in = &song.ins[c->ins];
    int32_t p;
    int t = player.tick;

    if (c->delay && t == c->delay) {
        c->delay = 0;
        apply_cell(ch, &c->delayed);
    }
    if (c->cut && t == c->cut) {
        c->cut = 0;
        note_cut(ch);
    }
    if (c->retrig && t && t % c->retrig == 0 && c->note)
        note_on(ch, c->note);

    /* pitch slides */
    if (c->slide && t)
        c->pitch += c->slide;
    if (c->porta && c->target != c->pitch && c->note) {
        if (c->pitch < c->target) {
            c->pitch += c->porta;
            if (c->pitch > c->target)
                c->pitch = c->target;
        } else {
            c->pitch -= c->porta;
            if (c->pitch < c->target)
                c->pitch = c->target;
        }
    }
    if (c->nslide_n) {
        if (c->nslide_n > 0) {
            c->pitch += c->nslide_speed;
            if (c->pitch >= c->target) {
                c->pitch = c->target;
                c->nslide_n = 0;
            }
        } else {
            c->pitch -= c->nslide_speed;
            if (c->pitch <= c->target) {
                c->pitch = c->target;
                c->nslide_n = 0;
            }
        }
    }
    if (c->pitch < 0)
        c->pitch = 0;
    if (c->pitch > 96 * 64)
        c->pitch = 96 * 64;
    /* the volume slide (from the second tick, as trackers do) */
    if (c->vslide && t) {
        int max = ch_is_psg(ch) ? 15 : 127, v;
        c->vacc = (int16_t)(c->vacc + c->vslide * (ch_is_psg(ch) ? 4 : 16));
        v = c->vol + c->vacc / 16;
        c->vacc %= 16;
        c->vol = (uint8_t)(v < 0 ? 0 : v > max ? max : v);
    }

    p = c->pitch + c->fine + c->mpitch;
    if (c->vib_depth) {
        p += SINE[c->vib_pos & 63u] * c->vib_depth / 16;
        c->vib_pos = (uint8_t)(c->vib_pos + c->vib_speed);
    }
    if (c->arp) {
        uint32_t step = c->arp_pos % 3u;
        p += step == 1u ? (c->arp >> 4) * 64 : step == 2u ? (c->arp & 15u) * 64 : 0;
        c->arp_pos++;
    }
    if (in->mac[MAC_ARP].len)
        p += macro_val(c, in, MAC_ARP, 0) * 64;

    if (ch_is_fm(ch)) {
        if (c->note && !(ch == CH_FM6 && player.dac)) {
            fm_pitch(ch, p);
            fm_volume(ch);
        }
    } else {
        int att = 15;
        /* a PSG note sounds while held, and after it is let go only through a volume macro's release */
        if (c->note && (c->key || (in->mac[MAC_VOL].len && in->mac[MAC_VOL].rel != MACRO_NONE)))
            att = 15 - c->vol * macro_val(c, in, MAC_VOL, 15) / 15;
        if (ch == CH_NOI && in->mac[MAC_NOISE].len && c->note) {
            int m = macro_val(c, in, MAC_NOISE, c->noise) & 7;
            if (m != c->noise || c->mpos[MAC_NOISE] == 0)
                noise_mode(m);
        }
        psg_out(ch, p, att);
    }
    if (c->note)
        macros_step(c, in);
}

static void advance_row(void)
{
    song_t *s = &song;
    player.tick = 0;
    if (s->speed[0] != s->speed[1] || player.speed[0] != player.speed[1])
        player.speed_i ^= 1u;
    if (player.jump >= 0 || player.brk >= 0) {
        if (!player.loop_pat) {
            if (player.jump >= 0)
                player.ord = (uint16_t)player.jump;
            else
                player.ord++;
        }
        player.row = (uint16_t)(player.brk >= 0 ? player.brk : 0);
        if (player.row >= s->rows)
            player.row = 0;
        player.jump = player.brk = -1;
    } else if (++player.row >= s->rows) {
        player.row = 0;
        if (!player.loop_pat)
            player.ord++;
    }
    if (player.ord >= s->nord) {
        player.ord = s->loop < s->nord ? s->loop : 0;
        player.loops++;
        if (player.stop_at_end)
            player.playing = 0;
    }
}

static void tick(void)
{
    int ch;
    if (player.playing && player.tick == 0) {
        if (player.row_hook)
            player.row_hook();
        for (ch = 0; ch < NCH; ch++) {
            const cell_t *cl;
            int k, d = 0;
            if (!chan_on(ch) || player.ch[ch].jam)
                continue;
            cl = song_cell_ro(&song, ch, song.order[player.ord][ch], player.row);
            if (!cl)
                continue;
            for (k = 0; k < 2; k++)
                if (cl->fx[k] == FX_DELAY && cl->fxv[k])
                    d = cl->fxv[k];
            if (d) {
                player.ch[ch].delayed = *cl;
                player.ch[ch].delayed.fx[0] = player.ch[ch].delayed.fx[0] == FX_DELAY ? 0 : player.ch[ch].delayed.fx[0];
                player.ch[ch].delayed.fx[1] = player.ch[ch].delayed.fx[1] == FX_DELAY ? 0 : player.ch[ch].delayed.fx[1];
                player.ch[ch].delay = (uint8_t)d;
            } else
                apply_cell(ch, cl);
        }
    }
    for (ch = 0; ch < NCH; ch++)
        if (chan_on(ch))
            chan_tick(ch);
    if (player.playing) {
        player.frames++;
        if (++player.tick >= player.speed[player.speed_i])
            advance_row();
    } else
        player.tick = (uint8_t)(player.tick + 1u) % 64u;
}

static void keyons(void)
{
    int ch;
    for (ch = 0; ch < NCH; ch++)
        if (keyon_mask & (1u << ch))
            fm_key(ch, 1);
    keyon_mask = 0;
}

/* -------------------------------------------------------------- control --- */
static void tick_rate(void)
{
    /* the frame rate of the console's video: NTSC 59.92 Hz, PAL 49.70 Hz */
    static const float FRAME_HZ[2] = {59.922743f, 49.701459f};
    float hz = song.tick_hz ? (float)song.tick_hz * 0.1f : FRAME_HZ[song.region & 1u];
    player.tick_len = (uint32_t)((float)player.md.out_hz * 65536.0f / hz);
}

static void silence(void)
{
    int ch, i;
    for (ch = 0; ch < 6; ch++) {
        md_fm(&player.md, 0, 0x28, (uint8_t)(ch < 3 ? ch : ch + 1));
        for (i = 0; i < 4; i++)
            fmw_op(ch, i, 0x40, 0x7F);
    }
    for (i = 0; i < 4; i++)
        md_psg(&player.md, (uint8_t)(0x9F | i << 5));
    md_dac_stop(&player.md);
}

void player_reset_chips(void)
{
    int ch, i;
    uint32_t mute = player.mute, t = player.md.vgm_samples;
    void (*log)(void *, int, int, uint8_t, uint8_t) = player.md.log;
    void *log_ctx = player.md.log_ctx;
    md_init(&player.md, player.md.out_hz ? player.md.out_hz : 44100u, song.region & 1, song.chip & 1, song.filter % 3u);
    player.md.log = log;               /* a VGM being written goes on through the reset */
    player.md.log_ctx = log_ctx;
    player.md.vgm_samples = t;
    md_mute(&player.md, mute);
    player.dac = 0;
    player.ext3 = 0;
    ext_keys = 0;
    keyon_mask = 0;
    for (ch = 0; ch < NCH; ch++) {
        chan_t *c = &player.ch[ch];
        uint8_t *b = (uint8_t *)c;
        for (i = 0; i < (int)sizeof(*c); i++)
            b[i] = 0;
        c->pan = 0xC0;
        c->vol = ch_is_psg(ch) ? 15 : 127;
        c->ins = 0;
        cur_ins[ch] = 0xFF;
        last_freq[ch] = 0xFFFF;
        last_att[ch] = 0xFF;
        for (i = 0; i < 4; i++)
            last_tl[ch][i] = 0xFF;
    }
    player.lfo = song.lfo;
    md_fm(&player.md, 0, 0x22, song.lfo);
    md_fm(&player.md, 0, 0x27, 0x00);
    md_fm(&player.md, 0, 0x2B, 0x00);
    if (song.ext3)
        ext3_set(1);
    silence();
    noise_mode(4);
    tick_rate();
}

void player_init(uint32_t out_hz)
{
    player.md.out_hz = out_hz;
    player.jump = player.brk = -1;
    player_reset_chips();
}

#define RING 64u
static struct {
    uint8_t cmd, b, c;
    uint16_t a;
} ring[RING];
static volatile uint32_t r_head, r_tail;

int player_post(uint8_t cmd, uint16_t a, uint8_t b, uint8_t c)
{
    uint32_t h = r_head;
    if (h - r_tail >= RING)
        return -1;
    ring[h % RING].cmd = cmd;
    ring[h % RING].a = a;
    ring[h % RING].b = b;
    ring[h % RING].c = c;
    __asm__ volatile("" ::: "memory");
    r_head = h + 1u;
    return 0;
}

static void do_cmd(uint8_t cmd, uint16_t a, uint8_t b, uint8_t c)
{
    switch (cmd) {
    case PC_PLAY: {
        int ch;
        player_reset_chips();
        player.ord = a < song.nord ? a : 0;
        player.row = b < song.rows ? b : 0;
        player.loop_pat = c;
        player.tick = 0;
        player.speed_i = 0;
        player.speed[0] = song.speed[0] ? song.speed[0] : 6;
        player.speed[1] = song.speed[1] ? song.speed[1] : player.speed[0];
        player.jump = player.brk = -1;
        player.frames = player.loops = 0;
        player.tick_left = 0;
        for (ch = 0; ch < NCH; ch++)
            player.ch[ch].jam = 0;
        player.playing = 1;
        break;
    }
    case PC_STOP: {
        int ch;
        player.playing = 0;
        for (ch = 0; ch < NCH; ch++)
            if (player.ch[ch].key)
                note_off(ch);
        silence();
        break;
    }
    case PC_JAM_ON:
        if (a < NCH && b >= 1u && b <= NOTE_MAX) {
            cell_t cl = {b, c, VAL_NONE, {0, 0}, {0, 0}};
            player.ch[a].jam = 1;
            apply_cell(a, &cl);
        }
        break;
    case PC_JAM_OFF:
        if (a < NCH) {
            note_off(a);
            player.ch[a].jam = 0;
        }
        break;
    case PC_MUTE:
        player.mute = (uint32_t)a | (uint32_t)b << 16;
        md_mute(&player.md, player.mute);
        break;
    case PC_RESET:
        player.playing = 0;
        player_reset_chips();
        break;
    case PC_INS: {
        int ch;
        for (ch = 0; ch < NCH; ch++)
            if (cur_ins[ch] == a)
                cur_ins[ch] = 0xFF;            /* reloaded on its next note */
        break;
    }
    default:
        break;
    }
}

void player_run_commands(void)
{
    while (r_tail != r_head) {
        uint32_t t = r_tail % RING;
        do_cmd(ring[t].cmd, ring[t].a, ring[t].b, ring[t].c);
        r_tail++;
    }
}

void player_render(int32_t *out, uint32_t n)
{
    if (player.lock) {                  /* the song is being loaded or saved: only the chips run */
        md_render(&player.md, out, n);
        return;
    }
    player_run_commands();
    while (n) {
        uint32_t k;
        if (keyon_mask) {               /* key on a sample after the key off, so that it retriggers */
            md_render(&player.md, out, 1);
            keyons();
            out += 2;
            n--;
            continue;
        }
        if (player.tick_left < (1u << 16)) {
            tick();
            player.tick_left += player.tick_len;
            if (player.stop_at_end && !player.playing)
                return;                 /* an export: it ends exactly at the loop */
            continue;
        }
        k = player.tick_left >> 16;
        if (k > n)
            k = n;
        md_render(&player.md, out, k);
        player.tick_left -= k << 16;
        out += 2u * k;
        n -= k;
    }
}
