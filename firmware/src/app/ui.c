/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the screen and the panel (docs/DESIGN.md, 5). Part of the unity build after app.h; the
 * host simulator and the web build include it too.
 *
 * The screen is drawn whole, every frame (~30 a second), in three bands of 80 rows: each band is
 * drawn into one of gfx.c's two canvases while the other is still being sent. Every view's draw
 * function draws in screen coordinates and the band clips it, so a view is drawn three times a frame
 * and must not change anything while drawing. The views:
 *
 *   PATTERN (HOME)  the tracker      STAGE (HOME again)  scopes, registers, piano roll
 *   ORDERS  (SEQ)   the arrangement  SONG (SEQ again)    title, timing, chip
 *   INSTRUMENT (EDIT) FM pages / PSG macros / DAC; LIBRARY (SEL there)
 *   SONGS   (GLO)   the 256 slots    SETUP (GLO again)
 *
 * This file: themes, input, the frame, the header and footer, the eye-candy that is everywhere
 * (copper bars, CRT lines, wipes, the boot intro). The views are in ui_*.c, included at the end. */

/* --------------------------------------------------------------- themes --- */
typedef struct {
    const char *name;
    uint16_t bg, panel, line, dim, text, acc, hi, sel, play, rec;
    uint16_t cu0, cu1;                         /* the header's copper gradient, top and bottom */
    uint16_t chan[10];                         /* FM1-6, SQ1-3, NOI */
} theme_t;

static const theme_t THEMES[NTHEMES] = {
    {"MEGA BLUE", RGB(6, 8, 28), RGB(16, 20, 54), RGB(34, 42, 96), RGB(84, 96, 150), RGB(214, 220, 244),
     RGB(80, 200, 255), RGB(255, 236, 120), RGB(40, 64, 160), RGB(80, 255, 140), RGB(255, 64, 72),
     RGB(24, 40, 140), RGB(4, 6, 30),
     {RGB(255, 96, 96), RGB(255, 160, 64), RGB(255, 224, 80), RGB(150, 240, 90), RGB(80, 220, 200), RGB(100, 150, 255),
      RGB(200, 130, 255), RGB(255, 120, 220), RGB(160, 200, 255), RGB(200, 200, 200)}},
    {"AMBER CRT", RGB(10, 5, 0), RGB(36, 18, 0), RGB(70, 36, 0), RGB(130, 70, 10), RGB(255, 176, 60),
     RGB(255, 210, 120), RGB(255, 240, 200), RGB(110, 56, 0), RGB(255, 220, 140), RGB(255, 90, 30),
     RGB(120, 54, 0), RGB(14, 6, 0),
     {RGB(255, 200, 110), RGB(255, 176, 60), RGB(230, 150, 40), RGB(255, 200, 110), RGB(255, 176, 60), RGB(230, 150, 40),
      RGB(255, 220, 160), RGB(240, 190, 120), RGB(220, 170, 100), RGB(200, 140, 70)}},
    {"PHOSPHOR", RGB(0, 8, 2), RGB(0, 28, 8), RGB(0, 56, 16), RGB(20, 110, 40), RGB(90, 255, 120),
     RGB(170, 255, 190), RGB(230, 255, 235), RGB(0, 90, 30), RGB(200, 255, 210), RGB(255, 120, 80),
     RGB(0, 90, 30), RGB(0, 10, 4),
     {RGB(140, 255, 160), RGB(100, 240, 130), RGB(70, 210, 100), RGB(140, 255, 160), RGB(100, 240, 130), RGB(70, 210, 100),
      RGB(190, 255, 200), RGB(160, 250, 180), RGB(130, 230, 150), RGB(100, 190, 120)}},
    {"SUNSET", RGB(22, 6, 34), RGB(44, 14, 64), RGB(80, 26, 104), RGB(140, 70, 160), RGB(255, 220, 240),
     RGB(255, 110, 199), RGB(255, 210, 90), RGB(120, 30, 120), RGB(90, 255, 220), RGB(255, 70, 90),
     RGB(200, 40, 120), RGB(30, 10, 70),
     {RGB(255, 90, 130), RGB(255, 140, 90), RGB(255, 200, 90), RGB(255, 110, 199), RGB(190, 120, 255), RGB(110, 160, 255),
      RGB(90, 255, 220), RGB(130, 230, 255), RGB(220, 160, 255), RGB(220, 200, 230)}},
    {"HIGH CONTRAST", RGB(0, 0, 0), RGB(24, 24, 24), RGB(90, 90, 90), RGB(150, 150, 150), RGB(255, 255, 255),
     RGB(255, 255, 0), RGB(255, 255, 255), RGB(0, 70, 200), RGB(0, 255, 0), RGB(255, 40, 40),
     RGB(60, 60, 60), RGB(0, 0, 0),
     {RGB(255, 255, 0), RGB(0, 255, 255), RGB(255, 128, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(255, 128, 255),
      RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(200, 200, 200)}},
    /* Okabe & Ito's colours, told apart with every common colour blindness */
    {"COLOUR SAFE", RGB(8, 10, 16), RGB(24, 28, 40), RGB(54, 60, 80), RGB(110, 116, 136), RGB(236, 236, 236),
     RGB(86, 180, 233), RGB(240, 228, 66), RGB(0, 70, 120), RGB(0, 158, 115), RGB(213, 94, 0),
     RGB(0, 70, 120), RGB(8, 10, 16),
     {RGB(230, 159, 0), RGB(86, 180, 233), RGB(0, 158, 115), RGB(240, 228, 66), RGB(0, 114, 178), RGB(204, 121, 167),
      RGB(213, 94, 0), RGB(230, 159, 0), RGB(86, 180, 233), RGB(200, 200, 200)}},
};
static const theme_t *th = &THEMES[0];
#define K_BG (th->bg)
#define K_ACC (th->acc)
#define K_DIM (th->dim)

static uint16_t chan_col(int ch)
{
    if (ch >= CH_OP2)
        return th->chan[CH_FM3];
    return th->chan[ch < 10 ? ch : 9];
}

/* ------------------------------------------------------------- colours --- */
static uint16_t mixc(uint16_t a, uint16_t b, int32_t t)   /* a .. b, t 0..256 */
{
    int32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    if (t < 0)
        t = 0;
    if (t > 256)
        t = 256;
    return (uint16_t)(((ar + ((br - ar) * t >> 8)) << 11) | ((ag + ((bg - ag) * t >> 8)) << 5) | (ab + ((bb - ab) * t >> 8)));
}

static uint16_t rainbow(uint32_t h)          /* h 0..767 around the wheel */
{
    uint32_t s = h % 768u, k = s % 256u;
    if (s < 256u)
        return RGB(255 - k, k, 0);
    if (s < 512u)
        return RGB(0, 255 - k, k);
    return RGB(k, 0, 255 - k);
}

static const int8_t SINE[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127, 126, 125, 122, 117, 112, 106,
    98, 90, 81, 71, 60, 49, 37, 25, 12, 0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -117,
    -122, -125, -126, -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12};
static int32_t isin(uint32_t a) { return SINE[a & 63u]; }   /* a full turn is 64 */

/* --------------------------------------------------------------- state --- */
enum { V_PAT, V_STAGE, V_ORD, V_SONG, V_INS, V_LIB, V_SONGS, V_SETUP, V_INTRO, NVIEW };
static const char *const VIEW_NAME[NVIEW] = {"PATTERN", "STAGE", "ORDERS", "SONG", "INSTRUMENT", "LIBRARY",
                                             "SONGS", "SETUP", ""};
enum { PG_TONE, PG_ENV, PG_LFO, PG_MAC };      /* the FM editor's pages; PSG: the macro shown */

#define HDR_H 24
#define FTR_Y 224
#define NROWS_VIS 13
#define ROW_H 12
#define ROWS_Y 46
#define WIPE_N 8
#define FRAME_MS 33u

static struct {
    uint8_t view, last_view;
    uint32_t frame, now, drawn_at;
    /* the pattern cursor */
    int ord, row, ch, scroll;
    uint8_t ins;                               /* the instrument the keys play and enter */
    uint8_t edit;                              /* REC: the keys write */
    uint8_t fxmode;                            /* FX: the knobs edit the effect columns */
    uint8_t last_note;
    /* the block (SEL held and turned) */
    uint8_t blk, marking;
    int b_r0, b_r1, b_c0, b_c1;
    /* order view, lists */
    int o_ord, o_ch, song_row, setup_row;
    /* instrument editor */
    int i_page, i_row, i_step, i_mac;
    /* library */
    int lib_src, lib_pos;
    /* songs */
    int sl_pos;
    /* a message over the screen */
    char msg[2][32];
    uint32_t msg_until;
    /* an action that wants its button pressed again */
    int pend;
    uint32_t pend_until;
    int wipe;
    /* panel */
    uint32_t held, t_down[NB];
    uint8_t hold_done[NB];
    uint32_t keys;
    int16_t jam_key;                           /* the key sounding (a panel key, or 100 + a MIDI note), -1 none,
                                                * JAM_TIMED a knob's preview, which stops at jam_until */
    uint32_t jam_until;
    uint16_t cpu_peak;                         /* the slowest audio block lately, % of its time */
    uint32_t batt_low_t, batt_said;            /* the battery: since when it reads empty; the last warning */
    uint16_t batt_warns;                       /* low-battery warnings shown */
    uint32_t cpu_peak_t;
    uint8_t jam_ch, jam_ins;
    int16_t keys_held[16];                     /* the keys down, oldest first, as jam_key counts them */
    uint8_t keys_held_n;
    uint32_t enc_t[NE];
    uint8_t dirty;                             /* the song changed since it was loaded or saved */
    /* the stage view's history */
    uint8_t roll[10][120];
    uint8_t roll_pos;
    uint8_t heat[2][256];                      /* YM2612 registers written lately */
    /* the boot intro */
    uint32_t intro_t0;
    uint8_t chime;
} ui;

int accel_off;                                 /* the host's "spin": exact detents */

static int nch_song(void) { return song.ext3 ? NCH : NCH_STD; }
static int ch_at(int i)                        /* the i-th channel left to right: FM3's operators after FM3 */
{
    static const uint8_t EXT[NCH] = {CH_FM1, CH_FM2, CH_FM3, CH_OP2, CH_OP3, CH_OP4, CH_FM4, CH_FM5, CH_FM6,
                                     CH_SQ1, CH_SQ2, CH_SQ3, CH_NOI};
    return song.ext3 ? EXT[i] : i;
}
static const char *ch_name(int ch)
{
    static const char *const N[NCH] = {"FM1", "FM2", "FM3", "FM4", "FM5", "FM6", "SQ1", "SQ2", "SQ3", "NOI",
                                       "OP2", "OP3", "OP4"};
    if (song.ext3 && ch == CH_FM3)
        return "OP1";
    return N[ch];
}

static void hex2(char *b, uint32_t v)
{
    b[0] = "0123456789ABCDEF"[(v >> 4) & 15u];
    b[1] = "0123456789ABCDEF"[v & 15u];
    b[2] = 0;
}
static void dec3(char *b, uint32_t v)
{
    b[0] = (char)('0' + v / 100u % 10u);
    b[1] = (char)('0' + v / 10u % 10u);
    b[2] = (char)('0' + v % 10u);
    b[3] = 0;
}

void ui_say(const char *a, const char *b)
{
    str_cpy(ui.msg[0], a ? a : "", sizeof ui.msg[0]);
    str_cpy(ui.msg[1], b ? b : "", sizeof ui.msg[1]);
    ui.msg_until = plat_ms() + 1400u;
}

/* press again within 2 s to do it: 1 = do it now */
static int confirm(int what, const char *a, const char *b)
{
    if (ui.pend == what && plat_ms() < ui.pend_until) {
        ui.pend = 0;
        return 1;
    }
    ui.pend = what;
    ui.pend_until = plat_ms() + 2000u;
    ui_say(a, b);
    return 0;
}

static void view_leave(int v);                 /* ui_views.c: what a view does as it is left */

static void go(int v)
{
    if (v == ui.view)
        return;
    view_leave(ui.view);
    ui.last_view = ui.view;
    ui.view = (uint8_t)v;
    ui.wipe = set.wipes ? 1 : 0;
    ui.pend = 0;
}

/* --------------------------------------------------------------- drawing --- */
static int32_t band_y0;                        /* the band being drawn: rows band_y0 .. +79 */

static void band_begin(int32_t y0)
{
    cv_begin(240, 80, th->bg);
    band_y0 = y0;
    cv_oy = -y0;
    cv_y0 = 0;
    cv_y1 = 80;
}
static int band_has(int32_t y, int32_t h) { return y + h > band_y0 && y < band_y0 + 80; }

static void rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (band_has(y, h))
        cv_rect(x, y, w, h, c);
}
static void frame_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    rect(x, y, w, 1, c);
    rect(x, y + h - 1, w, 1, c);
    rect(x, y, 1, h, c);
    rect(x + w - 1, y, 1, h, c);
}
static int32_t text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    return cv_text(x, y, f, s, c);
}
static int32_t text_sh(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    cv_text(x + 1, y + 1, f, s, RGB(0, 0, 0));
    return cv_text(x, y, f, s, c);
}
static void text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void text_r(int32_t rx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    text(rx - text_w(f, s), y, f, s, c);
}

/* text whose colour runs down its rows (the chrome of 16-bit logos): grad[0..f->h-1] */
static int32_t text_grad(int32_t x, int32_t y, const felucca_font_t *f, const char *s, const uint16_t *grad)
{
    if (!band_has(y, f->h)) {
        for (; *s; s++)
            x += f->adv[glyph(f, (uint8_t)*s)];
        return x;
    }
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w, ni = 0, run = 0, a = 0;
        const uint8_t *gd;
        w = f->bw[gi];
        gd = f->data + f->off[gi];
#define NIB() ((ni & 1u) ? gd[ni++ >> 1] & 15u : gd[ni++ >> 1] >> 4)
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                if (!run) {
                    a = NIB();
                    run = a && a < 15u ? 1u : NIB() + 1u;
                }
                run--;
                if (a >= 8u)
                    cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, grad[gy]);
            }
#undef NIB
        x += f->adv[gi];
    }
    return x;
}

static void hline_grad(int32_t x, int32_t y, int32_t w, uint16_t a, uint16_t b)
{
    int32_t i;
    if (!band_has(y, 1))
        return;
    for (i = 0; i < w; i++)
        cv_pset(x + i, y, mixc(a, b, i * 256 / (w > 1 ? w - 1 : 1)));
}

/* copper bars: a gradient and three bright bars bouncing through it (the header, the stage) */
static void copper(int32_t y, int32_t h, uint32_t t)
{
    int32_t r;
    if (!band_has(y, h))
        return;
    for (r = 0; r < h; r++) {
        uint16_t c = mixc(th->cu0, th->cu1, r * 256 / h);
        int k;
        for (k = 0; k < 3; k++) {
            int32_t centre = h / 2 + isin(t / 2u + (uint32_t)k * 21u) * (h / 2) / 127, d = r - centre;
            if (d < 0)
                d = -d;
            if (d < 4)
                c = mixc(c, k == 0 ? th->acc : k == 1 ? th->hi : th->play, (4 - d) * 48);
        }
        rect(0, y + r, 240, 1, c);
    }
}

/* CRT: every other row a quarter darker, after a band is drawn */
static void crt_lines(void)
{
    uint32_t y, x;
    for (y = (uint32_t)(band_y0 & 1); y < 80u; y += 2u) {
        uint16_t *p = cv_px + y * 240u;
        for (x = 0; x < 240u; x++) {
            uint32_t c = swap16(p[x]);
            c = ((c & 0xF7DEu) >> 1) + ((c & 0xE79Cu) >> 2);
            p[x] = swap16(c);
        }
    }
}

/* a view change: the new view comes in from the left behind a bright edge */
static void wipe_mask(void)
{
    int32_t edge = ui.wipe * 240 / WIPE_N, y;
    if (ui.wipe <= 0 || ui.wipe >= WIPE_N)
        return;
    for (y = band_y0; y < band_y0 + 80; y++) {
        if (y < HDR_H)
            continue;
        cv_rect(edge, y, 240 - edge, 1, th->bg);
        cv_rect(edge, y, 3, 1, rainbow((uint32_t)(y * 6 + (int32_t)ui.frame * 24)));
        cv_rect(edge + 3, y, 2, 1, th->line);
    }
}

/* ------------------------------------------------------- header, footer --- */
static void song_pos_text(char *b)             /* "03:1F" */
{
    int o = player.playing ? player.ord : ui.ord, r = player.playing ? player.row : ui.row;
    hex2(b, (uint32_t)o);
    b[2] = ':';
    hex2(b + 3, (uint32_t)r);
}

/* the battery, small, between the transport and the position: 0..3 bars; nothing on USB power (or
 * before the first reading). Empty: a red outline that blinks (ui_frame warns once as well) */
static void batt_icon(int32_t x, int32_t y)
{
    int b = plat_battery(), i;
    uint16_t c = b == 1 ? th->hi : th->text;
    if (b < 0 || b > 3)
        return;
    if (b == 0) {
        if (ui.frame / 16u & 1u)
            return;
        c = th->rec;
    }
    frame_rect(x, y, 14, 8, c);
    rect(x + 14, y + 2, 2, 4, c);              /* the terminal */
    for (i = 0; i < b; i++)
        rect(x + 2 + i * 4, y + 2, 3, 4, c);
}

static void draw_header(void)
{
    char b[32];
    int32_t x;
    if (!band_has(0, HDR_H))
        return;
    copper(0, HDR_H, ui.frame);
    if (plat_ms() < ui.msg_until && ui.view != V_INTRO) {   /* a message takes the header's place */
        rect(0, 0, 240, HDR_H, th->sel);
        x = text_sh(6, 4, &FONT_B, ui.msg[0], th->hi);
        text_sh(x + 8, 4, &FONT_B, ui.msg[1], th->text);
        rect(0, HDR_H - 1, 240, 1, th->acc);
        return;
    }
    x = text_sh(4, 4, &FONT_B, song.title[0] ? song.title : "UNTITLED", ui.dirty ? th->hi : th->text);
    if (ui.dirty)
        x = text_sh(x, 4, &FONT_B, "*", th->hi);
    (void)x;
    /* right: transport, position, octave */
    song_pos_text(b);
    text_r(236, 6, &FONT_XS, b, player.playing ? th->play : th->text);
    if (player.playing && ui.edit) {
        if (ui.frame / 8u & 1u)
            rect(170, 8, 8, 8, th->rec);
    } else if (player.playing) {
        int i;
        for (i = 0; i < 4; i++) {              /* a play triangle */
            rect(170, 8 + i, 2 + i * 2, 1, th->play);
            rect(170, 15 - i, 2 + i * 2, 1, th->play);
        }
    } else {
        rect(171, 9, 6, 6, ui.edit ? th->rec : th->dim);
    }
    batt_icon(184, 8);
    rect(0, HDR_H - 1, 240, 1, mixc(th->acc, th->bg, 96));
}

/* the footer: what KNOB 1..4 do now, each over its knob's quarter of the screen */
static void footer(const char *k1, const char *k2, const char *k3, const char *k4)
{
    const char *k[4] = {k1, k2, k3, k4};
    int i;
    if (!band_has(FTR_Y, 16))
        return;
    rect(0, FTR_Y, 240, 16, th->panel);
    rect(0, FTR_Y, 240, 1, th->line);
    for (i = 0; i < 4; i++) {
        if (i)
            rect(i * 60, FTR_Y + 3, 1, 10, th->line);
        if (k[i])
            text_c(i * 60 + 30, FTR_Y + 2, &FONT_XS, k[i], i == 0 ? th->acc : th->text);
    }
}

/* a horizontal meter */
static void meter(int32_t x, int32_t y, int32_t w, int32_t h, int32_t v, int32_t max, uint16_t c)
{
    int32_t f = max > 0 ? v * w / max : 0;
    rect(x, y, w, h, th->line);
    rect(x, y, f < 0 ? 0 : f > w ? w : f, h, c);
}

/* ------------------------------------------------------------- editing --- */
static void lock(void) { player.lock = 1; }
static void unlock(void) { player.lock = 0; }

static int cur_pat(int ch) { return song.order[ui.ord][ch]; }

/* undo: the pattern the last edits changed, as it was before them; ARP swaps it back (and again) */
static struct {
    int ch, pat, ok;
    uint32_t t;
    cell_t cells[MAX_ROWS];
} undo __attribute__((section(".pool")));

static void undo_mark(int ch, int pat)
{
    int r;
    if (undo.ok && undo.ch == ch && undo.pat == pat && plat_ms() - undo.t < 1500u) {
        undo.t = plat_ms();
        return;                                /* the same burst of edits */
    }
    for (r = 0; r < song.rows; r++) {
        const cell_t *c = song_cell_ro(&song, ch, pat, r);
        if (c)
            undo.cells[r] = *c;
        else
            memset(&undo.cells[r], 0, sizeof(cell_t)), undo.cells[r].ins = undo.cells[r].vol = VAL_NONE;
    }
    undo.ch = ch;
    undo.pat = pat;
    undo.ok = 1;
    undo.t = plat_ms();
}

static void undo_swap(void)
{
    int r;
    if (!undo.ok) {
        ui_say("NOTHING TO UNDO", "");
        return;
    }
    lock();
    for (r = 0; r < song.rows; r++) {
        cell_t *c = song_cell_w(&song, undo.ch, undo.pat, r), t;
        if (!c)
            break;
        t = *c;
        *c = undo.cells[r];
        undo.cells[r] = t;
    }
    unlock();
    undo.t = 0;                                /* the next edit starts a new burst */
    ui.dirty = 1;
    ui_say("UNDO", "ARP AGAIN: REDO");
}

/* the cell at the cursor, for writing (allocating its pattern); NULL: the song is full */
static cell_t *edit_cell(int ch, int pat, int row)
{
    cell_t *c;
    undo_mark(ch, pat);
    lock();
    c = song_cell_w(&song, ch, pat, row);
    unlock();
    if (!c)
        ui_say("SONG FULL", "NO ROOM FOR ROWS");
    else
        ui.dirty = 1;
    return c;
}

static int ins_vol_max(int ch) { return ch_is_psg(ch) ? 15 : 127; }

static void mute_send(void)
{
    player_post(PC_MUTE, (uint16_t)(player.mute & 0xFFFFu), (uint8_t)(player.mute >> 16), 0);
}

/* ------------------------------------------------------------- input --- */
static uint32_t btn_now, btn_prev;
static int pressed(int b) { return (btn_now >> b & 1u) && !(btn_prev >> b & 1u); }
static int released(int b) { return !(btn_now >> b & 1u) && (btn_prev >> b & 1u); }
static int held(int b) { return (int)(btn_now >> b & 1u); }
static uint32_t held_ms(int b) { return held(b) ? plat_ms() - ui.t_down[b] : 0u; }

/* a hold: once, when b has been down ms (and was not used for something else meanwhile) */
static int hold_once(int b, uint32_t ms)
{
    if (held(b) && !ui.hold_done[b] && held_ms(b) >= ms) {
        ui.hold_done[b] = 1;
        return 1;
    }
    return 0;
}

static int32_t enc(int e)                      /* detents, faster when turned fast */
{
    int32_t d = plat_enc(e);
    uint32_t now = plat_ms(), dt;
    if (!d)
        return 0;
    dt = now - ui.enc_t[e];
    ui.enc_t[e] = now;
    if (accel_off)
        return d;
    if (dt < 25u)
        return d * 4;
    if (dt < 50u)
        return d * 2;
    return d;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int wrapi(int v, int n) { return ((v % n) + n) % n; }

/* the keys' note (1..96) for key k at the octave; 0 out of range */
static int key_note(int k)
{
    int n = set.oct * 12 + 6 + k;
    return n >= 1 && n <= NOTE_MAX ? n : 0;
}

#define JAM_TIMED (-2)
#define JAM_PREVIEW_MS 350u

static void jam_on(int ch, int note, int ins)
{
    if (ui.jam_key != -1 && ui.jam_ch != ch)
        player_post(PC_JAM_OFF, ui.jam_ch, 0, 0);
    player_post(PC_JAM_ON, (uint16_t)ch, (uint8_t)note, (uint8_t)ins);
    ui.jam_ch = (uint8_t)ch;
    ui.jam_ins = (uint8_t)ins;
}

/* a note heard without a key held (a knob changing a cell's note): it stops by itself shortly, or
 * when a key takes the channel over, so nothing is left sounding with no key to let go */
static void jam_preview(int ch, int note, int ins)
{
    jam_on(ch, note, ins);
    ui.jam_key = JAM_TIMED;
    ui.jam_until = plat_ms() + JAM_PREVIEW_MS;
}
static void jam_resume(void);
static void jam_timeout(void)
{
    if (ui.jam_key == JAM_TIMED && (int32_t)(plat_ms() - ui.jam_until) >= 0)
        jam_resume();                          /* a key still down plays on */
}

/* the keys held, panel and MIDI: a key let go stops the note only if it is the one sounding, and
 * then the newest key still down plays again (one note a channel, the last pressed, as a synth's
 * mono mode), so overlapping notes neither cut each other off nor leave a held key silent */
static int held_note(int id) { return id >= 100 ? id - 100 : key_note(id); }
static void keys_held_drop(int id)
{
    int i, j;
    for (i = j = 0; i < ui.keys_held_n; i++)
        if (ui.keys_held[i] != id)
            ui.keys_held[j++] = ui.keys_held[i];
    ui.keys_held_n = (uint8_t)j;
}
static void key_down(int id)
{
    keys_held_drop(id);
    if (ui.keys_held_n == 16)
        keys_held_drop(ui.keys_held[0]);
    ui.keys_held[ui.keys_held_n++] = (int16_t)id;
    ui.jam_key = (int16_t)id;
}
/* the newest key still down sounds again on the jam channel; none: the channel stops */
static void jam_resume(void)
{
    while (ui.keys_held_n) {
        int top = ui.keys_held[ui.keys_held_n - 1], n = held_note(top);
        if (n) {
            player_post(PC_JAM_ON, ui.jam_ch, (uint8_t)n, ui.jam_ins);
            ui.jam_key = (int16_t)top;
            return;
        }
        keys_held_drop(top);
    }
    player_post(PC_JAM_OFF, ui.jam_ch, 0, 0);
    ui.jam_key = -1;
}
static void key_up(int id)
{
    keys_held_drop(id);
    if (ui.jam_key == id)
        jam_resume();
}

static void view_key(int note);                /* the views: a key went down (note) */
static void views_input(void);
static void views_draw(void);
static void stage_tick(void);
static void intro_start(void);
static void stage_log(void *ctx, int kind, int port, uint8_t reg, uint8_t val);

static void midi_poll(void)
{
    uint32_t pkt;
    while (plat_midi_in(&pkt)) {
        uint32_t st = pkt >> 8 & 0xFFu, d1 = pkt >> 16 & 0x7Fu, d2 = pkt >> 24 & 0x7Fu;
        int note = (int)d1 - 11;
        if (note < 1 || note > NOTE_MAX)
            continue;
        if ((st & 0xF0u) == 0x90u && d2) {
            if (!set.midi_in)                  /* MIDI in off: no new notes, but held ones still end */
                continue;
            key_down(100 + note);
            view_key(note);
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            key_up(100 + note);
        }
    }
}

static void input(void)
{
    uint32_t keys = plat_keys(), down, up;
    int b, k;
    btn_prev = btn_now;
    btn_now = plat_buttons();
    for (b = 0; b < NB; b++)
        if (pressed(b)) {
            ui.t_down[b] = plat_ms();
            ui.hold_done[b] = 0;
        }
    down = keys & ~ui.keys;
    up = ui.keys & ~keys;
    ui.keys = keys;
    if (ui.view == V_INTRO) {
        if (down || (btn_now & ~btn_prev)) {
            go(V_PAT);
            ui.wipe = 1;
        } else {
            views_input();                     /* the chime, the timeout */
        }
        return;
    }
    /* the octave, everywhere */
    if (pressed(B_OCTDN) && set.oct > 0) {
        set.oct--;
        ui_say("OCTAVE", (const char[]){(char)('0' + set.oct), 0});
    }
    if (pressed(B_OCTUP) && set.oct < 7) {
        set.oct++;
        ui_say("OCTAVE", (const char[]){(char)('0' + set.oct), 0});
    }
    for (k = 0; k < NKEYS; k++) {
        if (down >> k & 1u) {
            int n = key_note(k);
            if (n) {
                key_down(k);
                view_key(n);
            }
        }
        if (up >> k & 1u)
            key_up(k);
    }
    midi_poll();
    views_input();
    jam_timeout();
}

/* the lights: the view's button, PLAY, REC, FX, the keys held */
static void leds(void)
{
    uint32_t b = 0;
    if (ui.view == V_PAT || ui.view == V_STAGE)
        b |= 1u << B_HOME;
    if (ui.view == V_ORD || ui.view == V_SONG)
        b |= 1u << B_SEQ;
    if (ui.view == V_INS || ui.view == V_LIB)
        b |= 1u << B_EDIT;
    if (ui.view == V_SONGS || ui.view == V_SETUP)
        b |= 1u << B_GLO;
    if (player.playing)
        b |= 1u << B_PLAY;
    if (ui.edit)
        b |= 1u << B_REC;
    if (ui.fxmode && ui.view == V_PAT)
        b |= 1u << B_FX;
    if (ui.blk)
        b |= 1u << B_SEL;
    if (ui.view == V_INS) {                    /* the editor's pages light their buttons */
        static const uint8_t PB[4] = {B_EDIT, B_ENV, B_LFO, B_FX};
        b |= 1u << PB[ui.i_page & 3];
    }
    if (ui.view == V_LIB)
        b |= 1u << B_SEL;
    plat_play_red(ui.edit);
    plat_glow(set.leds);
    plat_leds(set.leds ? b : 0u, set.leds ? ui.keys : 0u);
}

/* --------------------------------------------------------------- frame --- */
void ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    th = &THEMES[set.theme % NTHEMES];
    ui.ins = 0;
    ui.jam_key = -1;
    ui.last_note = 49;
    ui.view = V_PAT;
    btn_now = btn_prev = plat_buttons();
    ui.keys = plat_keys();
    player.md.log = stage_log;                 /* the stage view's register lights (kept through resets) */
    if (set.intro)
        intro_start();
}

static void draw_all(void)
{
    int32_t y;
    for (y = 0; y < 240; y += 80) {
        band_begin(y);
        if (ui.view != V_INTRO)
            draw_header();
        views_draw();
        wipe_mask();
        if (set.crt)
            crt_lines();
        cv_blit(0, (uint32_t)y);
    }
}

void ui_input_only(void) { input(); }

void ui_frame(void)
{
    static uint32_t trips;
    input();
    if (player_guard_trips != trips) {          /* the sound's safety net went off (player_render) */
        trips = player_guard_trips;
        ui_say("SOUND RESET", "SAFETY NET");
    }
    {                                           /* the audio's time budget: the slowest block, held 2 s */
        static uint32_t xruns, said;
        uint32_t p = plat_cpu_peak_pct(), now = plat_ms();
        if (p >= ui.cpu_peak || now - ui.cpu_peak_t > 2000u) {
            ui.cpu_peak = (uint16_t)(p > 999u ? 999u : p);
            ui.cpu_peak_t = now;
        }
        if (plat_xruns() != xruns) {            /* a block came too late: the DMA played an old one */
            xruns = plat_xruns();
            if (now - said > 3000u) {
                said = now;
                ui_say("AUDIO", "TOO SLOW");
            }
        }
    }
    {   /* the battery reads empty for 5 s (not a dip as the audio draws): say so, again every 10 minutes */
        uint32_t now = plat_ms();
        if (plat_battery() != 0)
            ui.batt_low_t = 0;
        else if (!ui.batt_low_t)
            ui.batt_low_t = now | 1u;
        else if (now - ui.batt_low_t >= 5000u && (!ui.batt_warns || now - ui.batt_said >= 600000u)) {
            ui.batt_said = now;
            ui.batt_warns++;
            ui_say("BATTERY LOW", ui.dirty ? "SAVE SONG" : "CHARGE IT");
            ui.msg_until = now + 4000u;
        }
    }
    th = &THEMES[set.theme % NTHEMES];
    if (plat_ms() - ui.drawn_at < FRAME_MS)
        return;
    ui.drawn_at = plat_ms();
    ui.frame++;
    if (ui.wipe > 0 && ++ui.wipe > WIPE_N)
        ui.wipe = 0;
    stage_tick();
    draw_all();
    leds();
}

#include "ui_pat.c"
#include "ui_views.c"
