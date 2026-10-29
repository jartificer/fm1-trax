/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the song (song.h): patterns in the row pool, the defaults, the byte stream. */
#include "song.h"
#include "presets.h"

static void mem0(void *p, uint32_t n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

/* ------------------------------------------------------------ effects --- */
static const struct {
    uint8_t fx;
    const char *name;
} FX_NAMES[] = {
    {FX_ARP, "ARPEGGIO"}, {FX_PORTA_UP, "PORTA UP"}, {FX_PORTA_DN, "PORTA DOWN"}, {FX_TONE_PORTA, "TONE PORTA"},
    {FX_VIBRATO, "VIBRATO"}, {FX_PAN, "PAN L/R"}, {FX_SPEED1, "SPEED ODD"}, {FX_VOL_SLIDE, "VOL SLIDE"},
    {FX_JUMP, "JUMP TO ORD"}, {FX_RETRIG, "RETRIGGER"}, {FX_BREAK, "BREAK TO ROW"}, {FX_SPEED, "SPEED"},
    {FX_LFO, "LFO ON/RATE"}, {FX_FB, "FEEDBACK"}, {FX_TL1, "OP1 LEVEL"}, {FX_TL2, "OP2 LEVEL"},
    {FX_TL3, "OP3 LEVEL"}, {FX_TL4, "OP4 LEVEL"}, {FX_MUL, "OP MULTIPLE"}, {FX_DAC, "DAC ON/OFF"},
    {FX_EXT3, "FM3 EXTENDED"}, {FX_AR, "ATTACK ALL"}, {FX_AR1, "OP1 ATTACK"}, {FX_AR2, "OP2 ATTACK"},
    {FX_AR3, "OP3 ATTACK"}, {FX_AR4, "OP4 ATTACK"}, {FX_NOISE, "NOISE MODE"}, {FX_SLIDE_UP, "NOTE UP"},
    {FX_SLIDE_DN, "NOTE DOWN"}, {FX_FINE, "FINE TUNE"}, {FX_CUT, "NOTE CUT"}, {FX_DELAY, "NOTE DELAY"},
};
#define NFX (sizeof(FX_NAMES) / sizeof(FX_NAMES[0]))

const char *fx_name(uint8_t fx)
{
    uint32_t i;
    for (i = 0; i < NFX; i++)
        if (FX_NAMES[i].fx == fx)
            return FX_NAMES[i].name;
    return 0;
}

uint8_t fx_next(uint8_t fx, int dir)
{
    int i, at = -1;
    for (i = 0; i < (int)NFX; i++)
        if (FX_NAMES[i].fx == fx)
            at = i;
    if (at < 0)
        return dir > 0 ? FX_NAMES[0].fx : FX_NAMES[NFX - 1u].fx;
    at += dir > 0 ? 1 : -1;
    if (at < 0 || at >= (int)NFX)
        return FX_NONE;
    return FX_NAMES[at].fx;
}

/* -------------------------------------------------------- instruments --- */
void ins_init(ins_t *in, int type)
{
    int m;
    mem0(in, sizeof(*in));
    in->type = (uint8_t)type;
    for (m = 0; m < NMAC; m++)
        in->mac[m].loop = in->mac[m].rel = MACRO_NONE;
    if (type == INS_FM) {
        int i;
        in->u.fm.alg = 4;
        in->u.fm.fb = 5;
        for (i = 0; i < 4; i++) {
            fm_op_t *o = &in->u.fm.op[i];
            o->ar = 31;
            o->d1r = 8;
            o->d2r = 2;
            o->rr = 6;
            o->sl = 3;
            o->mul = 1;
            o->tl = (i & 1) ? 0 : 30;
        }
        in->u.fm.op[0].mul = 2;
    } else if (type == INS_DAC) {
        in->u.dac.root = 49;               /* C-4 */
    }
}

/* ------------------------------------------------------------- patterns --- */
int song_pat_empty(const song_t *s, int ch, int pat)
{
    uint32_t r;
    const cell_t *c;
    if (s->pat[ch][pat] == PAT_NONE)
        return 1;
    c = &s->pool[s->pat[ch][pat]];
    for (r = 0; r < s->rows; r++, c++)
        if (c->note || c->ins != VAL_NONE || c->vol != VAL_NONE || c->fx[0] || c->fx[1])
            return 0;
    return 1;
}

const cell_t *song_cell_ro(const song_t *s, int ch, int pat, int row)
{
    if (ch < 0 || ch >= NCH || pat < 0 || pat >= MAX_PAT || row < 0 || row >= s->rows || s->pat[ch][pat] == PAT_NONE)
        return 0;
    return &s->pool[s->pat[ch][pat] + (uint32_t)row];
}

cell_t *song_cell(song_t *s, int ch, int pat, int row) { return (cell_t *)song_cell_ro(s, ch, pat, row); }

static void cells_clear(cell_t *c, uint32_t n)
{
    while (n--) {
        c->note = NOTE_NONE;
        c->ins = c->vol = VAL_NONE;
        c->fx[0] = c->fx[1] = 0;
        c->fxv[0] = c->fxv[1] = 0;
        c++;
    }
}

void song_compact(song_t *s)
{
    /* every pattern with something in it, moved down in order of where it was */
    uint32_t used = 0;
    for (;;) {
        int bc = -1, bp = -1, ch, p;
        uint32_t best = POOL_ROWS;
        for (ch = 0; ch < NCH; ch++)
            for (p = 0; p < MAX_PAT; p++) {
                uint16_t at = s->pat[ch][p];
                if (at != PAT_NONE && at >= used && at < best) {
                    best = at;
                    bc = ch;
                    bp = p;
                }
            }
        if (bc < 0)
            break;
        if (song_pat_empty(s, bc, bp)) {
            s->pat[bc][bp] = PAT_NONE;
            continue;
        }
        if (best != used) {
            uint32_t r;
            for (r = 0; r < s->rows; r++)
                s->pool[used + r] = s->pool[best + r];
            s->pat[bc][bp] = (uint16_t)used;
        }
        used += s->rows;
    }
    s->pool_used = (uint16_t)used;
}

uint32_t song_pool_free(const song_t *s) { return (POOL_ROWS - s->pool_used) / s->rows; }

cell_t *song_cell_w(song_t *s, int ch, int pat, int row)
{
    if (ch < 0 || ch >= NCH || pat < 0 || pat >= MAX_PAT || row < 0 || row >= s->rows)
        return 0;
    if (s->pat[ch][pat] == PAT_NONE) {
        if (s->pool_used + s->rows > POOL_ROWS)
            song_compact(s);
        if (s->pool_used + s->rows > POOL_ROWS)
            return 0;
        s->pat[ch][pat] = s->pool_used;
        cells_clear(&s->pool[s->pool_used], s->rows);
        s->pool_used = (uint16_t)(s->pool_used + s->rows);
    }
    return &s->pool[s->pat[ch][pat] + (uint32_t)row];
}

int song_set_rows(song_t *s, int rows)
{
    /* after song_compact the patterns sit at k * old for k = 0..n-1; each moves to k * rows, front
     * to back when they shrink, back to front when they grow (so a move never lands on one not yet
     * moved) */
    static uint16_t at[NCH * MAX_PAT];     /* the pattern (ch << 8 | p) at k * old */
    static cell_t tmp[MAX_ROWS];
    int ch, p, k, n = 0, old = s->rows;
    if (rows < 1 || rows > MAX_ROWS)
        return -1;
    if (rows == old)
        return 0;
    song_compact(s);
    n = s->pool_used / old;
    if ((uint32_t)n * (uint32_t)rows > POOL_ROWS)
        return -1;
    for (ch = 0; ch < NCH; ch++)
        for (p = 0; p < MAX_PAT; p++)
            if (s->pat[ch][p] != PAT_NONE)
                at[s->pat[ch][p] / old] = (uint16_t)(ch << 8 | p);
    for (k = rows < old ? 0 : n - 1; k >= 0 && k < n; k += rows < old ? 1 : -1) {
        uint32_t from = (uint32_t)k * (uint32_t)old, to = (uint32_t)k * (uint32_t)rows, r;
        uint32_t keep = (uint32_t)(rows < old ? rows : old);
        for (r = 0; r < keep; r++)
            tmp[r] = s->pool[from + r];
        for (r = 0; r < keep; r++)
            s->pool[to + r] = tmp[r];
        if (rows > old)
            cells_clear(&s->pool[to + keep], (uint32_t)(rows - old));
        s->pat[at[k] >> 8][at[k] & 0xFFu] = (uint16_t)to;
    }
    s->rows = (uint8_t)rows;
    s->pool_used = (uint16_t)((uint32_t)n * (uint32_t)rows);
    return 0;
}

int song_ins_used(const song_t *s, int i)
{
    uint32_t r;
    for (r = 0; r < s->pool_used; r++)
        if (s->pool[r].ins == i)
            return 1;
    return 0;
}

void song_init(song_t *s)
{
    int ch, p, i;
    mem0(s, sizeof(*s) - sizeof(s->pool));
    s->region = 0;
    s->chip = 0;
    s->filter = 0;
    s->speed[0] = s->speed[1] = 6;
    s->rows = 64;
    s->hl[0] = 4;
    s->hl[1] = 16;
    s->nord = 1;
    for (ch = 0; ch < NCH; ch++)
        for (p = 0; p < MAX_PAT; p++)
            s->pat[ch][p] = PAT_NONE;
    for (i = 0; i < MAX_INS; i++)
        ins_init(&s->ins[i], INS_EMPTY);
    presets_default_song(s);
    s->pool_used = 0;
}

/* --------------------------------------------------------- the stream --- */
typedef struct {
    void (*put)(void *, const uint8_t *, uint32_t);
    void *ctx;
    uint32_t n;
} wr_t;

static void w8(wr_t *w, uint32_t v)
{
    uint8_t b = (uint8_t)v;
    w->put(w->ctx, &b, 1);
    w->n++;
}
static void w16(wr_t *w, uint32_t v)
{
    w8(w, v);
    w8(w, v >> 8);
}
static void wbytes(wr_t *w, const void *p, uint32_t n)
{
    w->put(w->ctx, p, n);
    w->n += n;
}

static void w_ins(wr_t *w, const ins_t *in)
{
    int m;
    w8(w, in->type);
    wbytes(w, in->name, sizeof(in->name));
    if (in->type == INS_FM) {
        int i;
        const fm_patch_t *f = &in->u.fm;
        w8(w, f->alg);
        w8(w, f->fb);
        w8(w, f->ams);
        w8(w, f->pms);
        for (i = 0; i < 4; i++) {
            const fm_op_t *o = &f->op[i];
            uint8_t b[11] = {o->ar, o->d1r, o->d2r, o->rr, o->sl, o->tl, o->ks, o->mul, o->dt, o->am, o->ssg};
            wbytes(w, b, sizeof(b));
        }
    } else if (in->type == INS_DAC) {
        w8(w, in->u.dac.sample);
        w8(w, in->u.dac.root);
        w8(w, in->u.dac.loop);
        w8(w, in->u.dac.pitched);
    }
    for (m = 0; m < NMAC; m++) {
        const macro_t *mc = &in->mac[m];
        w8(w, mc->len);
        if (mc->len) {
            w8(w, mc->loop);
            w8(w, mc->rel);
            wbytes(w, mc->v, mc->len);
        }
    }
}

uint32_t song_write(const song_t *s, void (*put)(void *, const uint8_t *, uint32_t), void *ctx)
{
    wr_t w = {put, ctx, 0};
    int ch, p, i, n;
    wbytes(&w, SONG_MAGIC, 4);
    w8(&w, SONG_FORMAT);
    wbytes(&w, s->title, sizeof(s->title));
    wbytes(&w, s->author, sizeof(s->author));
    w8(&w, s->region);
    w8(&w, s->chip);
    w8(&w, s->filter);
    w8(&w, s->ext3);
    w16(&w, s->tick_hz);
    w8(&w, s->speed[0]);
    w8(&w, s->speed[1]);
    w8(&w, s->rows);
    w8(&w, s->hl[0]);
    w8(&w, s->hl[1]);
    w8(&w, s->lfo);
    w16(&w, s->nord);
    w16(&w, s->loop);
    w8(&w, NCH);
    /* the instruments that are not empty */
    for (i = 0, n = 0; i < MAX_INS; i++)
        n += s->ins[i].type != INS_EMPTY;
    w16(&w, (uint32_t)n);
    for (i = 0; i < MAX_INS; i++)
        if (s->ins[i].type != INS_EMPTY) {
            w8(&w, (uint32_t)i);
            w_ins(&w, &s->ins[i]);
        }
    for (i = 0; i < s->nord; i++)
        wbytes(&w, s->order[i], NCH);
    for (ch = 0; ch < NCH; ch++) {
        for (p = 0, n = 0; p < MAX_PAT; p++)
            n += !song_pat_empty(s, ch, p);
        w16(&w, (uint32_t)n);
        for (p = 0; p < MAX_PAT; p++) {
            const cell_t *c;
            uint32_t r, skip = 0;
            if (song_pat_empty(s, ch, p))
                continue;
            w8(&w, (uint32_t)p);
            c = &s->pool[s->pat[ch][p]];
            for (r = 0; r < s->rows; r++, c++) {
                uint32_t m = (c->note ? 1u : 0u) | (c->ins != VAL_NONE ? 2u : 0u) | (c->vol != VAL_NONE ? 4u : 0u) |
                             (c->fx[0] ? 8u : 0u) | (c->fx[1] ? 16u : 0u);
                if (!m) {
                    skip++;
                    continue;
                }
                if (skip)
                    w8(&w, 0x80u | (skip - 1u));
                skip = 0;
                w8(&w, m);
                if (m & 1u)
                    w8(&w, c->note);
                if (m & 2u)
                    w8(&w, c->ins);
                if (m & 4u)
                    w8(&w, c->vol);
                if (m & 8u) {
                    w8(&w, c->fx[0]);
                    w8(&w, c->fxv[0]);
                }
                if (m & 16u) {
                    w8(&w, c->fx[1]);
                    w8(&w, c->fxv[1]);
                }
            }
            w8(&w, 0x40);                  /* the end of the pattern */
        }
    }
    w8(&w, 0xEE);
    return w.n;
}

typedef struct {
    int (*get)(void *);
    void *ctx;
    int bad;
} rd_t;

static uint32_t r8(rd_t *r)
{
    int v = r->get(r->ctx);
    if (v < 0) {
        r->bad = 1;
        return 0;
    }
    return (uint32_t)v;
}
static uint32_t r16(rd_t *r)
{
    uint32_t a = r8(r);
    return a | r8(r) << 8;
}
static void rbytes(rd_t *r, void *p, uint32_t n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = (uint8_t)r8(r);
}

static void r_ins(rd_t *r, ins_t *in)
{
    int m, type = (int)r8(r);
    ins_init(in, type > INS_DAC ? INS_EMPTY : type);
    if (type > INS_DAC)
        r->bad = 1;
    rbytes(r, in->name, sizeof(in->name));
    in->name[sizeof(in->name) - 1u] = 0;
    if (type == INS_FM) {
        int i;
        fm_patch_t *f = &in->u.fm;
        f->alg = r8(r) & 7u;
        f->fb = r8(r) & 7u;
        f->ams = r8(r) & 3u;
        f->pms = r8(r) & 7u;
        for (i = 0; i < 4; i++) {
            fm_op_t *o = &f->op[i];
            o->ar = r8(r) & 31u;
            o->d1r = r8(r) & 31u;
            o->d2r = r8(r) & 31u;
            o->rr = r8(r) & 15u;
            o->sl = r8(r) & 15u;
            o->tl = r8(r) & 127u;
            o->ks = r8(r) & 3u;
            o->mul = r8(r) & 15u;
            o->dt = r8(r) & 7u;
            o->am = r8(r) & 1u;
            o->ssg = r8(r) & 15u;
        }
    } else if (type == INS_DAC) {
        in->u.dac.sample = (uint8_t)r8(r);
        in->u.dac.root = (uint8_t)r8(r);
        in->u.dac.loop = (uint8_t)r8(r);
        in->u.dac.pitched = (uint8_t)r8(r);
    }
    for (m = 0; m < NMAC; m++) {
        macro_t *mc = &in->mac[m];
        mc->len = (uint8_t)r8(r);
        if (mc->len > MACRO_LEN) {
            r->bad = 1;
            mc->len = 0;
        }
        if (mc->len) {
            mc->loop = (uint8_t)r8(r);
            mc->rel = (uint8_t)r8(r);
            rbytes(r, mc->v, mc->len);
        }
    }
}

uint32_t ins_write(const ins_t *in, void (*put)(void *, const uint8_t *, uint32_t), void *ctx)
{
    wr_t w = {put, ctx, 0};
    w_ins(&w, in);
    return w.n;
}

int ins_read(ins_t *in, int (*get)(void *), void *ctx)
{
    rd_t r = {get, ctx, 0};
    r_ins(&r, in);
    if (r.bad)
        ins_init(in, INS_EMPTY);
    return r.bad ? -1 : 0;
}

int song_read(song_t *s, int (*get)(void *), void *ctx)
{
    rd_t r = {get, ctx, 0};
    char magic[4];
    int ch, i, n, nch;
    rbytes(&r, magic, 4);
    if (magic[0] != 'F' || magic[1] != 'M' || magic[2] != '1' || magic[3] != 'D' || r8(&r) != SONG_FORMAT) {
        song_init(s);
        return -1;
    }
    song_init(s);
    for (i = 0; i < MAX_INS; i++)
        ins_init(&s->ins[i], INS_EMPTY);
    rbytes(&r, s->title, sizeof(s->title));
    rbytes(&r, s->author, sizeof(s->author));
    s->title[sizeof(s->title) - 1u] = s->author[sizeof(s->author) - 1u] = 0;
    s->region = r8(&r) & 1u;
    s->chip = r8(&r) & 1u;
    s->filter = (uint8_t)(r8(&r) % 3u);
    s->ext3 = r8(&r) & 1u;
    s->tick_hz = (uint16_t)r16(&r);
    s->speed[0] = (uint8_t)r8(&r);
    s->speed[1] = (uint8_t)r8(&r);
    s->rows = (uint8_t)r8(&r);
    s->hl[0] = (uint8_t)r8(&r);
    s->hl[1] = (uint8_t)r8(&r);
    s->lfo = r8(&r) & 15u;
    s->nord = (uint16_t)r16(&r);
    s->loop = (uint16_t)r16(&r);
    nch = (int)r8(&r);
    if (r.bad || s->rows < 1u || s->rows > MAX_ROWS || s->nord < 1u || s->nord > MAX_ORD || nch < 1 || nch > NCH) {
        song_init(s);
        return -2;
    }
    if (!s->speed[0])
        s->speed[0] = 6;
    if (!s->speed[1])
        s->speed[1] = s->speed[0];
    n = (int)r16(&r);
    for (i = 0; i < n && !r.bad; i++) {
        int k = (int)r8(&r);
        r_ins(&r, &s->ins[k]);
    }
    for (i = 0; i < s->nord && !r.bad; i++) {
        rbytes(&r, s->order[i], (uint32_t)nch);
    }
    for (ch = 0; ch < nch && !r.bad; ch++) {
        int np = (int)r16(&r), k;
        for (k = 0; k < np && !r.bad; k++) {
            int p = (int)r8(&r);
            uint32_t row = 0;
            if (!song_cell_w(s, ch, p, 0)) {
                r.bad = 2;
                break;
            }
            for (;;) {
                uint32_t m = r8(&r);
                cell_t *c;
                if (r.bad || m == 0x40u)
                    break;
                if (m & 0x80u) {
                    row += (m & 0x7Fu) + 1u;
                    continue;
                }
                if (row >= s->rows || (m & ~31u)) {
                    r.bad = 1;
                    break;
                }
                c = song_cell(s, ch, p, (int)row);
                if (m & 1u)
                    c->note = (uint8_t)r8(&r);
                if (m & 2u)
                    c->ins = (uint8_t)r8(&r);
                if (m & 4u)
                    c->vol = (uint8_t)r8(&r);
                if (m & 8u) {
                    c->fx[0] = (uint8_t)r8(&r);
                    c->fxv[0] = (uint8_t)r8(&r);
                }
                if (m & 16u) {
                    c->fx[1] = (uint8_t)r8(&r);
                    c->fxv[1] = (uint8_t)r8(&r);
                }
                row++;
            }
        }
    }
    if (!r.bad && r8(&r) != 0xEEu)
        r.bad = 1;
    if (r.bad) {
        int e = r.bad == 2 ? -3 : -2;
        song_init(s);
        return e;
    }
    if (s->loop >= s->nord)
        s->loop = 0;
    return 0;
}
