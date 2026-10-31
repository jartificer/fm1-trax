/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the views other than the pattern, and what every view shares (navigation, transport,
 * saving, undo, the instrument on PRESETS). Included by ui.c after ui_pat.c. */

#define AUDITION_INS (MAX_INS - 2)             /* the library plays what it shows on this instrument */
static ins_t audition_keep;
static uint8_t audition_on;

static void song_loaded(void)                  /* a song was loaded or made: the view starts over */
{
    ui.ord = ui.row = ui.ch = ui.scroll = 0;
    ui.o_ord = ui.o_ch = 0;
    ui.blk = 0;
    ui.dirty = 0;
    undo.ok = 0;
    player.mute = 0;
    mute_send();
    player_post(PC_RESET, 0, 0, 0);
}

static void ins_changed(int i)
{
    player_post(PC_INS, (uint16_t)i, 0, 0);
    ui.dirty = 1;
}

static void audition_end(void)
{
    if (!audition_on)
        return;
    song.ins[AUDITION_INS] = audition_keep;
    player_post(PC_INS, AUDITION_INS, 0, 0);   /* the song's own instrument back: not an edit (no "*") */
    audition_on = 0;
}

static void save_song(void)
{
    int rc;
    lock();
    rc = song_save_slot(set.song);
    unlock();
    if (rc == 0) {
        char b[8];
        dec3(b, set.song);
        ui.dirty = 0;
        ui_say("SAVED TO", b);
    } else {
        ui_say(rc == -10 ? "NO ROOM" : "SAVE FAILED", rc == -10 ? "DELETE A SONG" : "");
    }
}

/* ---------------------------------------------------------------- orders --- */
static int pat_unused(int ch)                  /* a pattern number no position names and that is empty */
{
    int p, o;
    for (p = 0; p < MAX_PAT; p++) {
        int named = 0;
        if (!song_pat_empty(&song, ch, p))
            continue;
        for (o = 0; o < song.nord && !named; o++)
            named = song.order[o][ch] == p;
        if (!named)
            return p;
    }
    return -1;
}

static void ord_input(void)
{
    int32_t d;
    if ((d = enc(EN_SELECT)) != 0)
        ui.o_ord = clampi(ui.o_ord + d, 0, song.nord - 1);
    if ((d = enc(EN_ALGO)) != 0)
        ui.o_ch = clampi(ui.o_ch + (d > 0 ? 1 : -1), 0, nch_song() - 1);
    if ((d = enc(EN_K1)) != 0) {
        uint8_t *p = &song.order[ui.o_ord][ch_at(ui.o_ch)];
        *p = (uint8_t)clampi(*p + d, 0, MAX_PAT - 1);
        ui.dirty = 1;
    }
    if ((d = enc(EN_K2)) != 0) {
        int c;
        for (c = 0; c < NCH; c++)
            song.order[ui.o_ord][c] = (uint8_t)clampi(song.order[ui.o_ord][c] + d, 0, MAX_PAT - 1);
        ui.dirty = 1;
    }
    if ((d = enc(EN_K3)) != 0) {
        int n = clampi(song.nord + d, 1, MAX_ORD), o;
        for (o = song.nord; o < n; o++)       /* new positions: the patterns of the last one */
            memcpy(song.order[o], song.order[song.nord - 1], NCH);
        song.nord = (uint16_t)n;
        if (song.loop >= song.nord)
            song.loop = (uint16_t)(song.nord - 1);
        ui.o_ord = clampi(ui.o_ord, 0, song.nord - 1);
        ui.dirty = 1;
    }
    if ((d = enc(EN_K4)) != 0) {
        song.loop = (uint16_t)clampi(song.loop + d, 0, song.nord - 1);
        ui.dirty = 1;
    }
    if (pressed(B_ENV) && song.nord < MAX_ORD) {   /* insert a position after the cursor, the same patterns */
        int o;
        lock();
        for (o = song.nord; o > ui.o_ord + 1; o--)
            memcpy(song.order[o], song.order[o - 1], NCH);
        song.nord++;
        if (song.loop > ui.o_ord)
            song.loop++;
        unlock();
        ui.o_ord++;
        ui.dirty = 1;
        ui_say("POSITION", "INSERTED");
    }
    if (hold_once(B_LFO, 700u)) {             /* held: delete the position */
        if (song.nord > 1) {
            int o;
            lock();
            for (o = ui.o_ord; o < song.nord - 1; o++)
                memcpy(song.order[o], song.order[o + 1], NCH);
            song.nord--;
            if (song.loop > ui.o_ord || song.loop >= song.nord)
                song.loop = song.loop ? (uint16_t)(song.loop - 1) : 0;
            unlock();
            ui.o_ord = clampi(ui.o_ord, 0, song.nord - 1);
            ui.dirty = 1;
            ui_say("POSITION", "DELETED");
        }
    } else if (released(B_LFO) && !ui.hold_done[B_LFO] && song.nord < MAX_ORD) {
        /* duplicate: a new position after it, with new copies of its patterns */
        int o, c, r;
        lock();
        for (o = song.nord; o > ui.o_ord + 1; o--)
            memcpy(song.order[o], song.order[o - 1], NCH);
        song.nord++;
        if (song.loop > ui.o_ord)
            song.loop++;
        for (c = 0; c < NCH; c++) {
            int src = song.order[ui.o_ord][c], dst;
            if (song_pat_empty(&song, c, src))
                continue;
            dst = pat_unused(c);
            if (dst < 0)
                continue;
            for (r = 0; r < song.rows; r++) {
                const cell_t *s = song_cell_ro(&song, c, src, r);
                cell_t *t;
                if (cell_is_empty(s))
                    continue;
                t = song_cell_w(&song, c, dst, r);
                if (t)
                    *t = *s;
            }
            song.order[ui.o_ord + 1][c] = (uint8_t)dst;
        }
        unlock();
        ui.o_ord++;
        ui.dirty = 1;
        ui_say("POSITION", "DUPLICATED");
    }
    if (pressed(B_SEL)) {                      /* to the pattern view at this position */
        ui.ord = ui.o_ord;
        ui.row = 0;
        ui.ch = ui.o_ch;
        go(V_PAT);
    }
}

static void ord_draw(void)
{
    int nc = nch_song(), cw = nc > 10 ? 17 : 20, i, c, r0 = ui.o_ord - NROWS_VIS / 2;
    int po = player.playing ? player.ord : -1;
    if (band_has(24, 22)) {
        rect(0, 24, 240, 22, th->panel);
        for (c = 0; c < nc; c++) {
            int ch = ch_at(c);
            uint16_t cc = chan_col(ch);
            text(22 + c * cw, 25, &FONT_XS, ch_name(ch) + (cw < 20 ? 1 : 0), c == ui.o_ch ? th->hi : cc);
            rect(22 + c * cw, 38, (cw - 4) * player.ch[ch].level / 15, 2, cc);
        }
    }
    for (i = 0; i < NROWS_VIS; i++) {
        int o = r0 + i, y = ROWS_Y + i * ROW_H;
        char b[4];
        uint16_t bgc = th->bg;
        if (o < 0 || o >= song.nord || !band_has(y, ROW_H))
            continue;
        if (o == ui.o_ord)
            bgc = th->sel;
        if (o == po)
            bgc = mixc(bgc, th->play, 70);
        rect(0, y, 240, ROW_H, bgc);
        hex2(b, (uint32_t)o);
        text(1, y, &FONT_XS, b, o == ui.o_ord ? th->hi : th->dim);
        if (o == song.loop)
            text(14, y, &FONT_XS, "@", th->acc);
        for (c = 0; c < nc; c++) {
            int ch = ch_at(c), p = song.order[o][ch];
            hex2(b, (uint32_t)p);
            text(22 + c * cw, y, &FONT_XS, b, song_pat_empty(&song, ch, p) ? mixc(th->dim, th->text, 90) : chan_col(ch));
        }
    }
    {   /* the cursor */
        int32_t y = ROWS_Y + (NROWS_VIS / 2) * ROW_H;
        frame_rect(20 + ui.o_ch * cw, y - 1, 16, ROW_H + 2, rainbow(ui.frame * 24u));
    }
    if (band_has(214, 10)) {
        char b[24], n[4];
        str_cpy(b, "LENGTH ", sizeof b);
        hex2(n, song.nord);
        str_cpy(b + 7, n, 3);
        str_cpy(b + 9, "  LOOP ", 8);
        hex2(n, song.loop);
        str_cpy(b + 16, n, 3);
        text(2, 213, &FONT_XS, b, th->dim);
        text_r(238, 213, &FONT_XS, "ENV INS LFO DUP", th->dim);
    }
    footer("PATTERN", "ROW +-", "LENGTH", "LOOP");
}

/* ----------------------------------------------------------- song setup --- */
enum { SS_TITLE, SS_AUTHOR, SS_REGION, SS_CHIP, SS_FILTER, SS_TICK, SS_SPEED1, SS_SPEED2, SS_ROWS, SS_HL1,
       SS_HL2, SS_LFO, SS_EXT3, NSS };
static const char *const SS_NAME[NSS] = {"TITLE", "AUTHOR", "REGION", "CHIP", "FILTER", "TICK RATE", "SPEED",
                                         "SPEED 2 (SWING)", "ROWS", "BEAT", "BAR", "LFO", "FM3 EXTENDED"};
static int ss_char;                            /* the title's character the knob changes */
static const char CHARSET[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-!?.#&'+*/:()";

static void text_edit(char *s, int max, int32_t d)
{
    int i, k = 0, n = (int)sizeof CHARSET - 1;
    if (ss_char >= max - 1)
        ss_char = max - 2;
    for (i = 0; i < ss_char; i++)              /* the string reaches the cursor */
        if (!s[i])
            s[i] = ' ', s[i + 1] = 0;
    for (i = 0; i < n; i++)
        if (CHARSET[i] == s[ss_char])
            k = i;
    s[ss_char] = CHARSET[wrapi(k + d, n)];
    if (!s[ss_char + 1] && s[ss_char] == ' ')
        s[ss_char] = 0;
    ui.dirty = 1;
}

static void ss_value(int row, char *b)
{
    static const char *const FILT[3] = {"MODEL 1", "MODEL 2", "NONE"};
    char n[8];
    b[0] = 0;
    switch (row) {
    case SS_TITLE: str_cpy(b, song.title, 24); break;
    case SS_AUTHOR: str_cpy(b, song.author, 24); break;
    case SS_REGION: str_cpy(b, song.region ? "PAL 50 HZ" : "NTSC 60 HZ", 16); break;
    case SS_CHIP: str_cpy(b, song.chip ? "YM3438 (CLEAN)" : "YM2612 (LADDER)", 20); break;
    case SS_FILTER: str_cpy(b, FILT[song.filter % 3u], 16); break;
    case SS_TICK:
        if (!song.tick_hz)
            str_cpy(b, "VIDEO FRAME", 16);
        else
            fmt_fix(b, song.tick_hz, 1), str_cpy(b + str_len(b), " HZ", 4);
        break;
    case SS_SPEED1: fmt_int(b, song.speed[0]); break;
    case SS_SPEED2: fmt_int(b, song.speed[1]); break;
    case SS_ROWS: fmt_int(b, song.rows); break;
    case SS_HL1: fmt_int(b, song.hl[0]); break;
    case SS_HL2: fmt_int(b, song.hl[1]); break;
    case SS_LFO: {
        static const char *const R[8] = {"3.98", "5.56", "6.02", "6.37", "6.88", "9.63", "48.1", "72.2"};
        if (!(song.lfo & 8u))
            str_cpy(b, "OFF", 4);
        else
            str_cpy(b, R[song.lfo & 7u], 8), str_cpy(b + str_len(b), " HZ", 4);
        break;
    }
    case SS_EXT3: str_cpy(b, song.ext3 ? "ON: OP1-OP4" : "OFF", 16); break;
    default: break;
    }
    (void)n;
}

static void ss_change(int row, int32_t d)
{
    int reset = 0;
    switch (row) {
    case SS_TITLE: text_edit(song.title, sizeof song.title, d); return;
    case SS_AUTHOR: text_edit(song.author, sizeof song.author, d); return;
    case SS_REGION: song.region ^= 1u; reset = 1; break;
    case SS_CHIP: song.chip ^= 1u; reset = 1; break;
    case SS_FILTER: song.filter = (uint8_t)wrapi(song.filter + d, 3); reset = 1; break;
    case SS_TICK: {
        int v = song.tick_hz ? song.tick_hz : (song.region ? 500 : 600);
        v = clampi(v + d * 5, 99, 3000);
        song.tick_hz = (uint16_t)(v == 99 ? 0 : v);
        reset = 1;
        break;
    }
    case SS_SPEED1: song.speed[0] = (uint8_t)clampi(song.speed[0] + d, 1, 31); break;
    case SS_SPEED2: song.speed[1] = (uint8_t)clampi(song.speed[1] + d, 1, 31); break;
    case SS_ROWS: {
        int n = clampi(song.rows + d, 1, MAX_ROWS);
        lock();
        if (song_set_rows(&song, n) != 0)
            ui_say("NO ROOM", "FOR LONGER PATTERNS");
        unlock();
        ui.row = clampi(ui.row, 0, song.rows - 1);
        break;
    }
    case SS_HL1: song.hl[0] = (uint8_t)clampi(song.hl[0] + d, 0, 64); break;
    case SS_HL2: song.hl[1] = (uint8_t)clampi(song.hl[1] + d, 0, 128); break;
    case SS_LFO: {
        int v = song.lfo & 8u ? (song.lfo & 7) + 1 : 0;
        v = clampi(v + d, 0, 8);
        song.lfo = (uint8_t)(v ? 8 | (v - 1) : 0);
        reset = 1;
        break;
    }
    case SS_EXT3:
        song.ext3 ^= 1u;
        ui.ch = clampi(ui.ch, 0, nch_song() - 1);
        ui.o_ch = clampi(ui.o_ch, 0, nch_song() - 1);
        reset = 1;
        break;
    default: break;
    }
    ui.dirty = 1;
    if (reset && !player.playing)
        player_post(PC_RESET, 0, 0, 0);
}

static void list_draw(const char *const *names, int n, int cur, void (*value)(int, char *), int y0, int vis)
{
    int i, first = cur - vis / 2;
    if (first > n - vis)
        first = n - vis;
    if (first < 0)
        first = 0;
    for (i = first; i < n && i < first + vis; i++) {
        int y = y0 + (i - first) * 14;
        char b[32];
        if (!band_has(y, 14))
            continue;
        if (i == cur) {
            rect(0, y, 240, 14, th->sel);
            rect(0, y, 3, 14, rainbow(ui.frame * 24u));
        }
        text(8, y + 1, &FONT_XS, names[i], i == cur ? th->hi : th->text);
        value(i, b);
        text_r(234, y + 1, &FONT_XS, b, i == cur ? th->hi : th->acc);
    }
}

static void song_view_input(void)
{
    int32_t d;
    if ((d = enc(EN_SELECT)) != 0) {
        ui.song_row = clampi(ui.song_row + d, 0, NSS - 1);
        ss_char = 0;
    }
    if ((d = enc(EN_ALGO)) != 0 && ui.song_row <= SS_AUTHOR)
        ss_char = clampi(ss_char + (d > 0 ? 1 : -1), 0, 22);
    if ((d = enc(EN_K1)) != 0)
        ss_change(ui.song_row, d > 0 ? 1 : -1);
}

static void song_view_draw(void)
{
    list_draw(SS_NAME, NSS, ui.song_row, ss_value, 30, 13);
    if (ui.song_row <= SS_AUTHOR && band_has(30, 190)) {   /* the character being edited */
        int i, first = ui.song_row - 6 < 0 ? 0 : ui.song_row - 6;
        const char *s = ui.song_row ? song.author : song.title;
        int32_t y = 30 + (ui.song_row - first) * 14, x = 234 - text_w(&FONT_XS, s);
        for (i = 0; i < ss_char && s[i]; i++)
            x += 6;
        if (ss_char >= (int)str_len(s))
            x = 234 - text_w(&FONT_XS, s) + 6 * ss_char;
        rect(x, y + 12, 6, 2, rainbow(ui.frame * 24u));
    }
    footer(ui.song_row <= SS_AUTHOR ? "CHAR" : "VALUE", ui.song_row <= SS_AUTHOR ? "ALGO:POS" : "", "", "");
}

/* ------------------------------------------------------ instrument editor --- */
/* the algorithms, drawn: each operator's place (column, row of 3) and who modulates whom */
static const uint8_t ALG_POS[8][4][2] = {
    {{0, 1}, {1, 1}, {2, 1}, {3, 1}}, {{0, 0}, {0, 2}, {1, 1}, {2, 1}}, {{1, 0}, {0, 2}, {1, 2}, {2, 1}},
    {{0, 0}, {1, 0}, {1, 2}, {2, 1}}, {{0, 0}, {1, 0}, {0, 2}, {1, 2}}, {{0, 1}, {1, 0}, {1, 1}, {1, 2}},
    {{0, 0}, {1, 0}, {1, 1}, {1, 2}}, {{0, 1}, {1, 1}, {2, 1}, {3, 1}}};
static const uint8_t ALG_EDGE[8][3][2] = {
    {{1, 2}, {2, 3}, {3, 4}}, {{1, 3}, {2, 3}, {3, 4}}, {{2, 3}, {1, 4}, {3, 4}}, {{1, 2}, {2, 4}, {3, 4}},
    {{1, 2}, {3, 4}, {0, 0}}, {{1, 2}, {1, 3}, {1, 4}}, {{1, 2}, {0, 0}, {0, 0}}, {{0, 0}, {0, 0}, {0, 0}}};

static ins_t *cur_ins(void) { return &song.ins[ui.ins]; }

static void alg_draw(int alg, int32_t x0, int32_t y0, int lit)
{
    int i;
    int32_t ox[4], oy[4];
    for (i = 0; i < 4; i++) {
        ox[i] = x0 + ALG_POS[alg][i][0] * 34;
        oy[i] = y0 + ALG_POS[alg][i][1] * 15;
    }
    for (i = 0; i < 3; i++) {                  /* the modulation paths */
        int a = ALG_EDGE[alg][i][0], b = ALG_EDGE[alg][i][1];
        if (!a)
            continue;
        if (band_has(y0, 50))
            cv_line(ox[a - 1] + 20, oy[a - 1] + 6, ox[b - 1], oy[b - 1] + 6, th->dim);
    }
    if (band_has(y0, 50)) {
        int32_t xo = x0 + 4 * 34 - 4;          /* the output bus */
        rect(xo, y0 + 2, 2, 38, th->acc);
        for (i = 0; i < 4; i++)
            if (ins_is_carrier(alg, i))
                cv_line(ox[i] + 20, oy[i] + 6, xo, oy[i] + 6, th->acc);
        /* OP1's feedback: a loop over it */
        cv_line(ox[0] + 4, oy[0] - 2, ox[0] + 16, oy[0] - 2, th->hi);
        cv_line(ox[0] + 4, oy[0] - 2, ox[0] + 4, oy[0], th->hi);
        cv_line(ox[0] + 16, oy[0] - 2, ox[0] + 16, oy[0], th->hi);
    }
    for (i = 0; i < 4; i++) {
        int car = ins_is_carrier(alg, i);
        uint16_t c = car ? mixc(th->acc, th->hi, lit * 17) : th->line;
        char b[2] = {(char)('1' + i), 0};
        rect(ox[i], oy[i], 20, 12, car ? c : th->panel);
        frame_rect(ox[i], oy[i], 20, 12, car ? th->hi : th->dim);
        text(ox[i] + 7, oy[i], &FONT_XS, b, car ? th->bg : th->text);
    }
}

/* the FM pages' rows: name, the field's limits, its value in an operator */
enum { R_MUL, R_DT, R_TL, R_KS, R_AM, NTONE };
enum { R_AR, R_D1R, R_SL, R_D2R, R_RR, R_SSG, NENV };
static const char *const TONE_N[NTONE] = {"MUL", "DT", "TL", "RS", "AM"};
static const char *const ENV_N[NENV] = {"AR", "D1R", "D1L", "D2R", "RR", "SSG"};

static uint8_t *op_field(fm_op_t *o, int page, int row)
{
    if (page == PG_TONE) {                     /* (a row left from another page: its last) */
        uint8_t *f[NTONE] = {&o->mul, &o->dt, &o->tl, &o->ks, &o->am};
        return f[clampi(row, 0, NTONE - 1)];
    } else {
        uint8_t *f[NENV] = {&o->ar, &o->d1r, &o->sl, &o->d2r, &o->rr, &o->ssg};
        return f[clampi(row, 0, NENV - 1)];
    }
}
static int op_max(int page, int row)
{
    static const uint8_t T[NTONE] = {15, 7, 127, 3, 1}, E[NENV] = {31, 31, 15, 31, 15, 15};
    return page == PG_TONE ? T[clampi(row, 0, NTONE - 1)] : E[clampi(row, 0, NENV - 1)];
}
static void op_text(int page, int row, int v, char *b)
{
    if (page == PG_TONE && row == R_DT) {      /* detune: 0-3 up, 4-7 down */
        static const char *const D[8] = {"0", "+1", "+2", "+3", "-0", "-1", "-2", "-3"};
        str_cpy(b, D[v & 7], 4);
    } else if (page == PG_TONE && row == R_AM) {
        str_cpy(b, v ? "ON" : "--", 4);
    } else if (page == PG_ENV && row == R_SSG) {
        if (v < 8)
            str_cpy(b, "--", 4);
        else
            b[0] = (char)('0' + (v & 7)), b[1] = 0;
    } else {
        fmt_int(b, v);
    }
}

/* the macro editor: PSG instruments, and FM's arpeggio / pitch */
static macro_t *cur_macro(void) { return &cur_ins()->mac[ui.i_mac]; }
static int mac_lo(int m) { return m == MAC_VOL ? 0 : m == MAC_ARP ? -48 : m == MAC_PITCH ? -64 : 0; }
static int mac_hi(int m) { return m == MAC_VOL ? 15 : m == MAC_ARP ? 48 : m == MAC_PITCH ? 63 : 7; }

static void ins_input(void)
{
    ins_t *in = cur_ins();
    int32_t d;
    int k;
    if ((d = enc(EN_PRESET)) != 0) {           /* the instrument */
        audition_end();
        ui.ins = (uint8_t)clampi(ui.ins + d, 0, MAX_INS - 3);
        in = cur_ins();
    }
    if (pressed(B_SEL)) {
        go(V_LIB);
        return;
    }
    if (in->type == INS_FM) {
        if (pressed(B_EDIT))
            ui.i_page = PG_TONE, ui.i_row = clampi(ui.i_row, 0, NTONE - 1);
        if (pressed(B_ENV))
            ui.i_page = PG_ENV, ui.i_row = clampi(ui.i_row, 0, NENV - 1);
        if (pressed(B_LFO))
            ui.i_page = PG_LFO;
        if (pressed(B_FX)) {
            ui.i_mac = ui.i_page == PG_MAC && ui.i_mac == MAC_ARP ? MAC_PITCH : MAC_ARP;
            ui.i_page = PG_MAC;
        }
        if ((d = enc(EN_ALGO)) != 0) {
            in->u.fm.alg = (uint8_t)wrapi(in->u.fm.alg + (d > 0 ? 1 : -1), 8);
            ins_changed(ui.ins);
        }
        if (ui.i_page == PG_TONE || ui.i_page == PG_ENV) {
            int nr = ui.i_page == PG_TONE ? NTONE : NENV;
            if ((d = enc(EN_SELECT)) != 0)
                ui.i_row = clampi(ui.i_row + d, 0, nr - 1);
            for (k = 0; k < 4; k++)
                if ((d = enc(EN_K1 + k)) != 0) {
                    uint8_t *f = op_field(&in->u.fm.op[k], ui.i_page, ui.i_row);
                    int v = *f, mx = op_max(ui.i_page, ui.i_row);
                    if (ui.i_page == PG_ENV && ui.i_row == R_SSG)
                        v = v < 8 ? clampi(d > 0 ? 8 : 0, 0, 15) : (v + d < 8 ? 0 : clampi(v + d, 8, 15));
                    else
                        v = clampi(v + d, 0, mx);
                    *f = (uint8_t)v;
                    ins_changed(ui.ins);
                }
        } else if (ui.i_page == PG_LFO) {
            if ((d = enc(EN_K1)) != 0) {
                int v = song.lfo & 8u ? (song.lfo & 7) + 1 : 0;
                v = clampi(v + d, 0, 8);
                song.lfo = (uint8_t)(v ? 8 | (v - 1) : 0);
                if (!player.playing)
                    player_post(PC_RESET, 0, 0, 0);
                ui.dirty = 1;
            }
            if ((d = enc(EN_K2)) != 0)
                in->u.fm.ams = (uint8_t)clampi(in->u.fm.ams + d, 0, 3), ins_changed(ui.ins);
            if ((d = enc(EN_K3)) != 0)
                in->u.fm.pms = (uint8_t)clampi(in->u.fm.pms + d, 0, 7), ins_changed(ui.ins);
            if ((d = enc(EN_K4)) != 0)
                in->u.fm.fb = (uint8_t)clampi(in->u.fm.fb + d, 0, 7), ins_changed(ui.ins);
        }
    } else if (in->type == INS_PSG) {
        if (pressed(B_ENV))
            ui.i_mac = MAC_VOL;
        if (pressed(B_LFO))
            ui.i_mac = MAC_ARP;
        if (pressed(B_FX))
            ui.i_mac = MAC_PITCH;
        if (pressed(B_EDIT))
            ui.i_mac = MAC_NOISE;
        ui.i_page = PG_MAC;
    } else if (in->type == INS_DAC) {
        dac_ins_t *dc = &in->u.dac;
        if ((d = enc(EN_K1)) != 0 && bank_n)
            dc->sample = (uint8_t)clampi(dc->sample + d, 0, (int)bank_n - 1), ins_changed(ui.ins);
        if ((d = enc(EN_K2)) != 0)
            dc->root = (uint8_t)clampi(dc->root + d, 1, NOTE_MAX), ins_changed(ui.ins);
        if ((d = enc(EN_K3)) != 0)
            dc->pitched = d > 0, ins_changed(ui.ins);
        if ((d = enc(EN_K4)) != 0)
            dc->loop = d > 0, ins_changed(ui.ins);
        return;
    } else {
        return;
    }
    if (ui.i_page == PG_MAC) {
        macro_t *m = cur_macro();
        if ((d = enc(EN_SELECT)) != 0)
            ui.i_step = clampi(ui.i_step + d, 0, MACRO_LEN - 1);
        if ((d = enc(EN_K1)) != 0) {
            if (ui.i_step >= m->len) {         /* writing past the end makes it longer */
                int s;
                for (s = m->len; s <= ui.i_step; s++)
                    m->v[s] = s ? m->v[s - 1] : (int8_t)(ui.i_mac == MAC_VOL ? 15 : 0);
                if (!m->len)
                    m->loop = m->rel = MACRO_NONE;
                m->len = (uint8_t)(ui.i_step + 1);
            }
            m->v[ui.i_step] = (int8_t)clampi(m->v[ui.i_step] + d, mac_lo(ui.i_mac), mac_hi(ui.i_mac));
            ins_changed(ui.ins);
        }
        if ((d = enc(EN_K2)) != 0) {
            int n = clampi(m->len + d, 0, MACRO_LEN), s;
            for (s = m->len; s < n; s++)
                m->v[s] = s ? m->v[s - 1] : (int8_t)(ui.i_mac == MAC_VOL ? 15 : 0);
            if (!m->len && n)
                m->loop = m->rel = MACRO_NONE;
            m->len = (uint8_t)n;
            if (m->loop != MACRO_NONE && m->loop >= m->len)
                m->loop = MACRO_NONE;
            if (m->rel != MACRO_NONE && m->rel >= m->len)
                m->rel = MACRO_NONE;
            ins_changed(ui.ins);
        }
        if ((d = enc(EN_K3)) != 0 && m->len) {
            int v = m->loop == MACRO_NONE ? -1 : m->loop;
            v = clampi(v + d, -1, m->len - 1);
            m->loop = (uint8_t)(v < 0 ? MACRO_NONE : v);
            ins_changed(ui.ins);
        }
        if ((d = enc(EN_K4)) != 0 && m->len) {
            int v = m->rel == MACRO_NONE ? -1 : m->rel;
            v = clampi(v + d, -1, m->len - 1);
            m->rel = (uint8_t)(v < 0 ? MACRO_NONE : v);
            ins_changed(ui.ins);
        }
    }
}

static void macro_draw(const macro_t *m, int mi, int32_t y0, int32_t h)
{
    int s, lo = mac_lo(mi), hi = mac_hi(mi);
    int32_t zero = lo < 0 ? y0 + h * hi / (hi - lo) : y0 + h;
    if (!band_has(y0 - 2, h + 16))
        return;
    rect(4, y0, 232, h, th->panel);
    rect(4, zero, 232, 1, th->line);
    for (s = 0; s < MACRO_LEN; s++) {
        int32_t x = 6 + s * 7, v, top, bot;
        uint16_t c = s < m->len ? (s == ui.i_step ? th->hi : th->acc) : th->line;
        if (s >= m->len) {
            rect(x, zero - 1, 5, 2, th->line);
            continue;
        }
        v = m->v[s];
        top = y0 + h - (v - lo) * h / (hi - lo);
        bot = zero;
        if (top > bot) {
            int32_t t = top;
            top = bot;
            bot = t;
        }
        rect(x, top, 5, bot - top + 1, c);
    }
    frame_rect(6 + ui.i_step * 7 - 1, y0 - 2, 7, h + 4, rainbow(ui.frame * 24u));
    if (m->len && m->loop != MACRO_NONE)
        rect(6 + m->loop * 7 - 2, y0, 1, h, th->play), text(6 + m->loop * 7 - 2, y0 + h + 1, &FONT_XS, "|", th->play);
    if (m->len && m->rel != MACRO_NONE)
        rect(6 + m->rel * 7 - 2, y0, 1, h, th->rec), text(6 + m->rel * 7 - 2, y0 + h + 1, &FONT_XS, "/", th->rec);
}

/* an operator's envelope, drawn: attack up, decay to the sustain level, the second decay, release */
static void env_draw(const fm_op_t *o, int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t a = (32 - o->ar) * w / 4 / 32, d1 = (32 - o->d1r) * w / 4 / 32, rr = (16 - o->rr) * w / 4 / 16;
    int32_t top = y + 2 + o->tl * (h - 4) / 127 / 2, sl = top + o->sl * (y + h - top) / 16, ss = w - a - d1 - rr;
    int32_t d2 = o->d2r ? (o->d2r * (y + h - sl)) / 40 : 0;
    if (ss < 4)
        ss = 4;
    if (!band_has(y, h))
        return;
    rect(x, y, w, h, th->panel);
    if (!o->ar) {
        text(x + 4, y + h / 2 - 6, &FONT_XS, "AR 0", th->rec);
        return;
    }
    cv_line(x, y + h - 1, x + a, top, c);
    cv_line(x + a, top, x + a + d1, sl, c);
    cv_line(x + a + d1, sl, x + a + d1 + ss, sl + d2, c);
    cv_line(x + a + d1 + ss, sl + d2, x + w - 1, y + h - 1, c);
}

static void ins_draw(void)
{
    ins_t *in = cur_ins();
    char b[32];
    static const char *const TYPE[4] = {"EMPTY", "FM", "PSG", "DAC"};
    int i;
    if (band_has(24, 20)) {                    /* which instrument */
        rect(0, 24, 240, 20, th->panel);
        hex2(b, ui.ins);
        text(4, 26, &FONT_B, b, th->hi);
        text(26, 28, &FONT_XS, TYPE[in->type & 3], th->acc);
        text(52, 26, &FONT_B, in->name[0] ? in->name : "-", th->text);
    }
    if (in->type == INS_EMPTY) {
        text_c(120, 100, &FONT_B, "EMPTY INSTRUMENT", th->text);
        text_c(120, 124, &FONT_XS, "SEL: PICK ONE FROM THE LIBRARY", th->dim);
        text_c(120, 140, &FONT_XS, "PRESETS: ANOTHER INSTRUMENT", th->dim);
        footer("", "", "", "");
        return;
    }
    if (in->type == INS_DAC) {
        const dac_ins_t *dc = &in->u.dac;
        if (dc->sample < bank_n && band_has(50, 110)) {
            const sample_t *s = &bank[dc->sample];
            int32_t x;
            text(8, 50, &FONT_B, s->name, th->hi);
            rect(4, 72, 232, 80, th->panel);
            rect(4, 112, 232, 1, th->line);
            for (x = 0; x < 232; x++) {        /* the sample's shape: its peak per column */
                uint32_t i0 = (uint32_t)x * s->len / 232u, i1 = (uint32_t)(x + 1) * s->len / 232u, j;
                int lo = 0, hi = 0;
                for (j = i0; j < i1 && j < s->len; j++) {
                    int v = (int)s->data[j] - 128;
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
                rect(4 + x, 112 - hi * 38 / 128, 1, (hi - lo) * 38 / 128 + 1, th->acc);
            }
            fmt_int(b, (int32_t)s->len);
            str_cpy(b + str_len(b), " SAMPLES", 10);
            text(8, 156, &FONT_XS, b, th->dim);
            fmt_int(b, s->rate);
            str_cpy(b + str_len(b), " HZ", 4);
            text_r(232, 156, &FONT_XS, b, th->dim);
        }
        {
            char n[4], r[8];
            note_name(dc->root, n);
            n[3] = 0;
            hex2(r, dc->sample);
            footer(r, n, dc->pitched ? "PITCHED" : "FIXED", dc->loop ? "LOOP" : "ONCE");
        }
        return;
    }
    if (in->type == INS_FM && ui.i_page != PG_MAC) {
        const fm_patch_t *f = &in->u.fm;
        int lit = 0, ch;
        for (ch = 0; ch < NCH; ch++)
            if (player.ch[ch].ins == ui.ins && player.ch[ch].level > lit)
                lit = player.ch[ch].level;
        alg_draw(f->alg, 10, 50, lit);
        if (band_has(46, 50)) {
            b[0] = 'A', b[1] = 'L', b[2] = 'G', b[3] = ' ', b[4] = (char)('0' + f->alg), b[5] = 0;
            text(170, 50, &FONT_B, b, th->hi);
            b[0] = 'F', b[1] = 'B', b[2] = ' ', b[3] = (char)('0' + f->fb), b[4] = 0;
            text(170, 68, &FONT_XS, b, th->text);
        }
        if (ui.i_page == PG_TONE || ui.i_page == PG_ENV) {
            int nr = ui.i_page == PG_TONE ? NTONE : NENV, r;
            const char *const *names = ui.i_page == PG_TONE ? TONE_N : ENV_N;
            int32_t y0 = 104;
            if (band_has(y0 - 10, 10)) {
                for (i = 0; i < 4; i++) {
                    char o[4] = {'O', 'P', (char)('1' + i), 0};
                    text_c(i * 60 + 30 + 8, y0 - 12, &FONT_XS, o, ins_is_carrier(f->alg, i) ? th->acc : th->dim);
                }
            }
            for (r = 0; r < nr; r++) {
                int32_t y = y0 + r * 13;
                if (!band_has(y, 13))
                    continue;
                if (r == ui.i_row)
                    rect(0, y, 240, 13, th->sel), rect(0, y, 3, 13, rainbow(ui.frame * 24u));
                text(6, y, &FONT_XS, names[r], r == ui.i_row ? th->hi : th->dim);
                for (i = 0; i < 4; i++) {
                    fm_op_t o = f->op[i];
                    int v = *op_field(&o, ui.i_page, r), mx = op_max(ui.i_page, r);
                    op_text(ui.i_page, r, v, b);
                    rect(i * 60 + 30, y + 10, 50 * v / mx, 2, chan_col(i * 2));
                    text_r(i * 60 + 60, y, &FONT_XS, b, r == ui.i_row ? th->hi : th->text);
                }
            }
            if (ui.i_page == PG_ENV)
                for (i = 0; i < 4; i++)
                    env_draw(&f->op[i], 4 + i * 59, 184, 55, 36, chan_col(i * 2));
            else if (band_has(172, 50)) {
                text(6, 176, &FONT_XS, "EDIT TONE  ENV ENVELOPE", th->dim);
                text(6, 190, &FONT_XS, "LFO MODULATION  FX MACROS", th->dim);
                text(6, 204, &FONT_XS, "SEL LIBRARY  ALGO ALGORITHM", th->dim);
            }
            {
                static const char *const L[2][4] = {{"OP1", "OP2", "OP3", "OP4"}, {"OP1", "OP2", "OP3", "OP4"}};
                footer(L[0][0], L[0][1], L[0][2], L[0][3]);
            }
            return;
        }
        /* the modulation page: four big readouts and the LFO's wave going by */
        {
            static const char *const LB[4] = {"LFO RATE", "AMS", "PMS", "FEEDBACK"};
            int v[4];
            v[0] = song.lfo & 8u ? (song.lfo & 7) + 1 : 0;
            v[1] = f->ams;
            v[2] = f->pms;
            v[3] = f->fb;
            for (i = 0; i < 4; i++) {
                int32_t x = i * 60;
                if (!band_has(110, 80))
                    break;
                rect(x + 4, 110, 52, 60, th->panel);
                text_c(x + 30, 112, &FONT_XS, LB[i], th->dim);
                b[0] = (char)('0' + v[i]), b[1] = 0;
                text_c(x + 30, 130, &FONT_M, i == 0 && !v[0] ? "-" : b, th->hi);
            }
            if (v[0] && band_has(176, 40)) {
                int32_t x, py = 196;
                for (x = 0; x < 232; x++) {
                    int32_t y = 196 - isin((uint32_t)(x * (v[0] + 1) / 6) + ui.frame * (uint32_t)v[0]) * 16 / 127;
                    if (x)
                        cv_line(4 + x - 1, py, 4 + x, y, th->acc);
                    py = y;
                }
            }
            footer("LFO", "AMS", "PMS", "FB");
        }
        return;
    }
    /* macros: PSG (and FM's arpeggio and pitch) */
    {
        static const char *const MN[NMAC] = {"VOLUME", "ARPEGGIO", "PITCH", "NOISE"};
        static const char *const BTN[NMAC] = {"ENV", "LFO", "FX", "EDIT"};
        const macro_t *m = cur_macro();
        char v[8], l[8], lp[8], rl[8];
        if (band_has(48, 14)) {
            for (i = 0; i < NMAC; i++) {
                if (in->type == INS_FM && (i == MAC_VOL || i == MAC_NOISE))
                    continue;
                rect(i * 60 + 2, 48, 56, 14, i == ui.i_mac ? th->sel : th->panel);
                text_c(i * 60 + 30, 49, &FONT_XS, MN[i], i == ui.i_mac ? th->hi : th->dim);
            }
        }
        macro_draw(m, ui.i_mac, 76, 120);
        if (band_has(204, 12)) {
            text(6, 210, &FONT_XS, in->type == INS_FM ? "FX" : BTN[ui.i_mac], th->dim);
            text_r(234, 210, &FONT_XS, in->type == INS_FM ? "FX: ARP / PITCH" : "ENV LFO FX EDIT", th->dim);
        }
        if (ui.i_step < m->len)
            fmt_int(v, m->v[ui.i_step]);
        else
            str_cpy(v, "--", 3);
        fmt_int(l, m->len);
        str_cpy(l + str_len(l), " LONG", 6);
        if (m->loop == MACRO_NONE || !m->len)
            str_cpy(lp, "NO LOOP", 8);
        else
            str_cpy(lp, "LOOP ", 6), fmt_int(lp + 5, m->loop);
        if (m->rel == MACRO_NONE || !m->len)
            str_cpy(rl, "NO REL", 8);
        else
            str_cpy(rl, "REL ", 5), fmt_int(rl + 4, m->rel);
        footer(v, l, lp, rl);
    }
}

/* ----------------------------------------------------------- the library --- */
enum { LS_FM = 0, LS_PSG = NGROUP_FM, LS_DAC = NGROUP_FM + NGROUP_PSG, LS_UFM, LS_UPSG, NLS };

static int lib_count(int src)
{
    if (src < LS_PSG)
        return 8;
    if (src < LS_DAC)
        return 8;
    if (src == LS_DAC)
        return preset_dac_count();
    return src == LS_UFM ? LIB_NFM : LIB_NPSG;
}
static const char *lib_src_name(int src)
{
    if (src < LS_PSG)
        return GROUP_FM[src];
    if (src < LS_DAC)
        return GROUP_PSG[src - LS_PSG];
    return src == LS_DAC ? "DAC KIT" : src == LS_UFM ? "USER FM" : "USER PSG";
}
static int lib_user_slot(int src, int i) { return src == LS_UFM ? i : LIB_NFM + i; }

static void lib_entry(int src, int i, ins_t *out)
{
    if (src < LS_PSG)
        preset_fm(src * 8 + i, out);
    else if (src < LS_DAC)
        preset_psg((src - LS_PSG) * 8 + i, out);
    else if (src == LS_DAC)
        preset_dac(i, out);
    else
        lib_get(lib_user_slot(src, i), out);
}

static void lib_audition(int note)
{
    ins_t e;
    lib_entry(ui.lib_src, ui.lib_pos, &e);
    if (e.type == INS_EMPTY)
        return;
    if (!audition_on) {
        audition_keep = song.ins[AUDITION_INS];
        audition_on = 1;
    }
    lock();
    song.ins[AUDITION_INS] = e;
    unlock();
    player_post(PC_INS, AUDITION_INS, 0, 0);
    jam_on(ins_ch_for(&e, cur_ch()), e.type == INS_DAC ? e.u.dac.root : note, AUDITION_INS);
}

static void lib_input(void)
{
    int32_t d;
    if ((d = enc(EN_ALGO)) != 0 || (d = enc(EN_K1)) != 0) {
        ui.lib_src = wrapi(ui.lib_src + (d > 0 ? 1 : -1), NLS);
        ui.lib_pos = 0;
    }
    if ((d = enc(EN_SELECT)) != 0)
        ui.lib_pos = clampi(ui.lib_pos + d, 0, lib_count(ui.lib_src) - 1);
    if (pressed(B_SEL) || pressed(B_EDIT)) {
        if (pressed(B_SEL)) {                  /* load it into the instrument being edited */
            ins_t e;
            lib_entry(ui.lib_src, ui.lib_pos, &e);
            if (e.type == INS_EMPTY) {
                ui_say("EMPTY SLOT", "");
                return;
            }
            audition_end();
            lock();
            song.ins[ui.ins] = e;
            unlock();
            ins_changed(ui.ins);
            ui_say("LOADED", e.name);
        }
        audition_end();
        go(V_INS);
        return;
    }
    if (pressed(B_SAVE)) {
        int slot;
        if (ui.lib_src < LS_UFM) {
            ui_say("PRESETS ARE FIXED", "PICK A USER BANK");
            return;
        }
        slot = lib_user_slot(ui.lib_src, ui.lib_pos);
        if (lib_used(slot) && !confirm(10 + slot, "OVERWRITE?", "SAVE AGAIN"))
            return;
        if (cur_ins()->type == INS_EMPTY) {
            ui_say("NOTHING TO STORE", "");
            return;
        }
        lock();
        {
            int rc = lib_put(slot, cur_ins());
            unlock();
            ui_say(rc == 0 ? "STORED" : rc == -10 ? "NO ROOM" : "STORE FAILED", rc == 0 ? cur_ins()->name : "");
        }
    }
}

static void lib_draw(void)
{
    int n = lib_count(ui.lib_src), first = ui.lib_pos - 6, i;
    if (band_has(24, 20)) {
        rect(0, 24, 240, 20, th->panel);
        text(4, 26, &FONT_B, "<", th->dim);
        text_c(120, 26, &FONT_B, lib_src_name(ui.lib_src), th->hi);
        text(228, 26, &FONT_B, ">", th->dim);
    }
    if (first > n - 13)
        first = n - 13;
    if (first < 0)
        first = 0;
    for (i = first; i < n && i < first + 13; i++) {
        int32_t y = 48 + (i - first) * 13;
        char b[24];
        ins_t e;
        if (!band_has(y, 13))
            continue;
        if (i == ui.lib_pos)
            rect(0, y, 240, 13, th->sel), rect(0, y, 3, 13, rainbow(ui.frame * 24u));
        dec3(b, (uint32_t)i);
        text(8, y, &FONT_XS, b, th->dim);
        lib_entry(ui.lib_src, i, &e);
        text(34, y, &FONT_XS, e.type == INS_EMPTY ? "- - -" : e.name, i == ui.lib_pos ? th->hi : th->text);
        if (e.type != INS_EMPTY)
            text_r(234, y, &FONT_XS, e.type == INS_FM ? "FM" : e.type == INS_PSG ? "PSG" : "DAC", th->acc);
    }
    footer("BANK", "", "", "");
    if (band_has(212, 12))
        text_c(120, 212, &FONT_XS, ui.lib_src >= LS_UFM ? "SEL LOAD  SAVE STORE  KEYS HEAR" : "SEL LOAD  KEYS HEAR",
               th->dim);
}

/* ---------------------------------------------------------------- songs --- */
static void songs_input(void)
{
    int32_t d;
    if ((d = enc(EN_SELECT)) != 0)
        ui.sl_pos = clampi(ui.sl_pos + d, 0, NSLOTS - 1);
    if (pressed(B_SEL)) {                      /* load */
        if (!slot_info[ui.sl_pos].used) {
            ui_say("EMPTY SLOT", "ENV: NEW SONG HERE");
        } else if (!ui.dirty || confirm(1, "UNSAVED CHANGES", "SEL AGAIN: LOAD")) {
            int rc;
            player_post(PC_STOP, 0, 0, 0);
            lock();
            rc = song_load_slot(ui.sl_pos);
            unlock();
            set.song = (uint8_t)ui.sl_pos;
            settings_save();
            song_loaded();
            ui_say(rc == 0 ? "LOADED" : "DAMAGED SONG", rc == 0 ? song.title : "A NEW ONE INSTEAD");
            go(V_PAT);
        }
    }
    if (pressed(B_ENV) && (!ui.dirty || confirm(2, "UNSAVED CHANGES", "ENV AGAIN: NEW"))) {
        player_post(PC_STOP, 0, 0, 0);
        lock();
        song_init(&song);
        unlock();
        set.song = (uint8_t)ui.sl_pos;
        settings_save();
        song_loaded();
        ui_say("NEW SONG", "SAVE TO KEEP IT");
        go(V_PAT);
    }
    if (pressed(B_SAVE)) {
        if (slot_info[ui.sl_pos].used && ui.sl_pos != set.song && !confirm(3, "OVERWRITE?", "SAVE AGAIN"))
            return;
        set.song = (uint8_t)ui.sl_pos;
        settings_save();
        save_song();
    }
    if (hold_once(B_LFO, 800u) && slot_info[ui.sl_pos].used) {
        if (confirm(4, "DELETE?", "HOLD LFO AGAIN")) {
            song_delete_slot(ui.sl_pos);
            ui_say("DELETED", "");
            if (ui.sl_pos == set.song)
                ui.dirty = 1;                  /* the song is still here, no longer saved */
        }
    }
}

static void songs_draw(void)
{
    int first = ui.sl_pos - 6, i;
    if (first > NSLOTS - 13)
        first = NSLOTS - 13;
    if (first < 0)
        first = 0;
    for (i = first; i < first + 13; i++) {
        int32_t y = 26 + (i - first) * 14;
        char b[12];
        const slot_info_t *s = &slot_info[i];
        if (!band_has(y, 14))
            continue;
        if (i == ui.sl_pos)
            rect(0, y, 240, 14, th->sel), rect(0, y, 3, 14, rainbow(ui.frame * 24u));
        dec3(b, (uint32_t)i);
        text(8, y + 1, &FONT_XS, b, i == set.song ? th->hi : th->dim);
        if (i == set.song)
            text(30, y + 1, &FONT_XS, ">", th->play);
        text(40, y + 1, &FONT_XS, s->used ? (s->title[0] ? s->title : "UNTITLED") : "- - -",
             s->used ? (i == ui.sl_pos ? th->hi : th->text) : th->dim);
        if (s->used) {
            uint32_t k = (s->bytes + 102u) / 1024u, t = (s->bytes * 10u + 512u) / 1024u % 10u;
            fmt_int(b, (int32_t)k);
            if (k < 10u) {
                uint32_t l = str_len(b);
                b[l] = '.', b[l + 1] = (char)('0' + t), b[l + 2] = 0;
            }
            str_cpy(b + str_len(b), "K", 2);
            text_r(234, y + 1, &FONT_XS, b, th->acc);
        }
    }
    if (band_has(210, 14)) {                   /* the room in flash */
        uint32_t cap = plat_obj_capacity(), room = plat_obj_room();
        char b[32];
        meter(8, 213, 120, 6, (int32_t)((cap - room) / 256u), (int32_t)(cap / 256u), th->acc);
        fmt_int(b, (int32_t)(room / 1024u));
        str_cpy(b + str_len(b), "K FREE", 8);
        text_r(234, 210, &FONT_XS, b, th->dim);
    }
    footer("SEL LOAD", "SAVE", "ENV NEW", "LFO DEL");
}

/* ---------------------------------------------------------------- setup --- */
enum { SU_THEME, SU_CRT, SU_INTRO, SU_WIPES, SU_LEDS, SU_STEP, SU_FOLLOW, SU_MIDI, SU_VERSION, SU_CPU, SU_BATT, NSU };
static const char *const SU_NAME[NSU] = {"THEME", "CRT LINES", "BOOT INTRO", "WIPES", "BUTTON LIGHTS",
                                         "EDIT STEP", "FOLLOW PLAY", "MIDI IN", "VERSION", "AUDIO LOAD",
                                         "BATTERY"};
static uint8_t setup_changed;

static void su_value(int row, char *b)
{
    switch (row) {
    case SU_THEME: str_cpy(b, THEMES[set.theme % NTHEMES].name, 20); break;
    case SU_CRT: str_cpy(b, set.crt ? "ON" : "OFF", 4); break;
    case SU_INTRO: str_cpy(b, set.intro ? "ON" : "OFF", 4); break;
    case SU_WIPES: str_cpy(b, set.wipes ? "ON" : "OFF", 4); break;
    case SU_LEDS: str_cpy(b, set.leds ? "ON" : "OFF", 4); break;
    case SU_STEP: fmt_int(b, set.step); break;
    case SU_FOLLOW: str_cpy(b, set.follow ? "ON" : "OFF", 4); break;
    case SU_MIDI: str_cpy(b, set.midi_in ? "THE KEYS" : "OFF", 10); break;
    case SU_VERSION:
        str_cpy(b, OM_VERSION, 12);
        if (OM_REV[0])
            str_cpy(b + str_len(b), " R", 3), str_cpy(b + str_len(b), OM_REV, 8);
        break;
    case SU_CPU:                               /* the average, and the slowest block lately */
        fmt_int(b, (int32_t)plat_cpu_pct());
        str_cpy(b + str_len(b), "% PEAK ", 8);
        fmt_int(b + str_len(b), (int32_t)ui.cpu_peak);
        str_cpy(b + str_len(b), "%", 2);
        break;
    case SU_BATT: {                            /* the bars, and the divider's reading (to check the thresholds) */
        int v = plat_battery(), a = plat_battery_adc();
        static const char *const L[4] = {"LOW", "1/3", "2/3", "3/3"};
        if (v == 4) {
            str_cpy(b, "USB POWER", 10);
            break;
        }
        str_cpy(b, v >= 0 && v < 4 ? L[v] : "--", 4);
        if (a >= 0) {
            str_cpy(b + str_len(b), "  ADC ", 7);
            fmt_int(b + str_len(b), a);
        }
        break;
    }
    default: b[0] = 0; break;
    }
}

static void setup_input(void)
{
    int32_t d;
    if ((d = enc(EN_SELECT)) != 0)
        ui.setup_row = clampi(ui.setup_row + d, 0, NSU - 1);
    if ((d = enc(EN_K1)) != 0) {
        int s = d > 0 ? 1 : -1;
        switch (ui.setup_row) {
        case SU_THEME: set.theme = (uint8_t)wrapi(set.theme + s, NTHEMES); break;
        case SU_CRT: set.crt ^= 1u; break;
        case SU_INTRO: set.intro ^= 1u; break;
        case SU_WIPES: set.wipes ^= 1u; break;
        case SU_LEDS: set.leds ^= 1u; break;
        case SU_STEP: set.step = (uint8_t)clampi(set.step + s, 0, 16); break;
        case SU_FOLLOW: set.follow ^= 1u; break;
        case SU_MIDI: set.midi_in ^= 1u; break;
        default: return;
        }
        setup_changed = 1;
    }
}

static void setup_draw(void)
{
    list_draw(SU_NAME, NSU, ui.setup_row, su_value, 30, 13);
    if (band_has(200, 24))
        text_c(120, 206, &FONT_XS, "OCT- + OCT+ AT POWER ON: CALIBRATE", th->dim);
    footer("VALUE", "", "", "");
}

/* ---------------------------------------------------------------- stage --- */
static void stage_log(void *ctx, int kind, int port, uint8_t reg, uint8_t val)
{
    (void)ctx;
    (void)val;
    if (kind == MD_LOG_FM && reg != 0x2A)      /* the DAC's stream would light one cell all the time */
        ui.heat[port & 1][reg] = 255;
}

static void stage_tick(void)                   /* once a frame: the piano roll moves, the heat fades */
{
    int c, p, r;
    ui.roll_pos = (uint8_t)((ui.roll_pos + 1u) % 120u);
    for (c = 0; c < 10; c++) {
        const chan_t *k = &player.ch[c];
        ui.roll[c][ui.roll_pos] = (uint8_t)(k->key && k->level && k->note <= NOTE_MAX ? k->note : 0);
    }
    for (p = 0; p < 2; p++)
        for (r = 0x20; r < 0xC0; r++)
            ui.heat[p][r] = (uint8_t)(ui.heat[p][r] * 15u / 16u);
}

static void stage_draw(void)
{
    int i;
    /* the title, in chrome, bouncing over copper bars */
    if (band_has(24, 40)) {
        uint16_t grad[32];
        const char *t = song.title[0] ? song.title : "FM1-TRAX";
        int32_t w = text_w(&FONT_L, t), x0, k;
        copper(24, 40, ui.frame * 2u + 20u);
        for (k = 0; k < 32; k++)               /* sky, horizon, ground: the chrome */
            grad[k] = k < 14 ? mixc(RGB(255, 255, 255), RGB(90, 160, 255), k * 18)
                             : k < 16 ? RGB(255, 255, 255) : mixc(RGB(150, 90, 40), RGB(255, 220, 160), (k - 16) * 16);
        if (w > 236) {                         /* too long: it scrolls */
            x0 = 240 - (int32_t)((ui.frame * 3u) % (uint32_t)(w + 240));
        } else {
            x0 = 120 - w / 2;
        }
        for (k = 0; t[k]; k++) {
            char ch[2] = {t[k], 0};
            int32_t y = 28 + isin(ui.frame * 2u + (uint32_t)k * 5u) * 3 / 127;
            text_grad(x0 + 1, y + 1, &FONT_L, ch, (const uint16_t[32]){0});
            text_grad(x0, y, &FONT_L, ch, grad);
            x0 += FONT_L.adv[glyph(&FONT_L, (uint8_t)t[k])];
        }
    }
    /* scopes: five by two */
    for (i = 0; i < 10; i++) {
        int32_t x = (i % 5) * 48, y = 66 + (i / 5) * 34, s;
        uint16_t cc = chan_col(i);
        int muted = (int)(player.mute >> i & 1u);
        if (!band_has(y, 34))
            continue;
        rect(x + 1, y, 46, 32, mixc(th->bg, cc, player.ch[i].level * 4));
        frame_rect(x + 1, y, 46, 32, muted ? th->line : mixc(th->line, cc, 128));
        text(x + 3, y + 1, &FONT_XS, ch_name(i), muted ? th->dim : cc);
        for (s = 0; s < 44; s++) {
            int a = player.md.scope[i][(player.md.scope_pos + (uint32_t)(s * 128 / 44 + 84)) % MD_SCOPE];
            int b = player.md.scope[i][(player.md.scope_pos + (uint32_t)((s + 1) * 128 / 44 + 84)) % MD_SCOPE];
            int ya = y + 19 - a * 24 / 128, yb = y + 19 - b * 24 / 128;
            ya = ya < y + 2 ? y + 2 : ya > y + 30 ? y + 30 : ya;
            yb = yb < y + 2 ? y + 2 : yb > y + 30 ? y + 30 : yb;
            cv_line(x + 2 + s, ya, x + 3 + s, yb, muted ? th->dim : cc);
        }
    }
    /* the piano roll: the last four seconds, notes as dashes in their channels' colours */
    if (band_has(136, 50)) {
        int c, k;
        rect(0, 136, 240, 48, th->panel);
        for (k = 0; k < 8; k++)
            rect(0, 136 + k * 6, 240, 1, mixc(th->panel, th->line, 128));
        for (k = 0; k < 120; k++) {
            int col = (ui.roll_pos + 1 + k) % 120;
            for (c = 0; c < 10; c++) {
                int n = ui.roll[c][col];
                if (n) {
                    int32_t y = 183 - (n - 24) * 47 / 60;
                    if (y >= 136 && y < 184)
                        rect(k * 2, y, 2, 2, chan_col(c));
                }
            }
        }
    }
    /* the YM2612's registers 20..BF, both ports, lit as they are written */
    if (band_has(186, 38)) {
        int p, r;
        for (p = 0; p < 2; p++)
            for (r = 0x20; r < 0xC0; r++) {
                int k = r - 0x20, col = k % 20, row = k / 20;
                int32_t x = p * 120 + col * 6, y = 187 + row * 4;
                rect(x, y, 5, 3, mixc(th->line, p ? th->hi : th->acc, ui.heat[p][r]));
            }
    }
    footer("YM2612", "SN76489", "LFO MUTE", "HOME BACK");
}

/* ---------------------------------------------------------------- intro --- */
static void intro_start(void)
{
    ui.view = V_INTRO;
    ui.intro_t0 = plat_ms();
    ui.chime = 0;
}

static void intro_chime(void)                  /* a bell arpeggio on FM1-FM3 */
{
    static const uint8_t NOTES[4] = {61, 68, 73, 80};   /* C-5 G-5 C-6 G-6 */
    uint32_t t = plat_ms() - ui.intro_t0;
    if (ui.chime == 0 && t > 300u) {
        audition_keep = song.ins[AUDITION_INS];
        audition_on = 1;
        preset_fm(10 * 8 + 1, &song.ins[AUDITION_INS]);   /* the BELL group */
        player_post(PC_INS, AUDITION_INS, 0, 0);
        ui.chime = 1;
    }
    while (ui.chime >= 1 && ui.chime <= 4 && t > 300u + (uint32_t)(ui.chime - 1) * 140u) {
        player_post(PC_JAM_ON, (uint16_t)(ui.chime - 1 < 3 ? ui.chime - 1 : 3), NOTES[ui.chime - 1], AUDITION_INS);
        ui.chime++;
    }
    if (ui.chime == 5 && t > 2600u) {
        int c;
        for (c = 0; c < 4; c++)
            player_post(PC_JAM_OFF, (uint16_t)c, 0, 0);
        ui.chime = 6;
    }
}

static void intro_draw(void)
{
    uint32_t t = plat_ms() - ui.intro_t0, f = ui.frame;
    int i;
    /* the star field: three layers, the near ones fast and bright */
    for (i = 0; i < 90; i++) {
        uint32_t h = (uint32_t)i * 2654435761u, layer = (uint32_t)i % 3u, sp = 1u << layer;
        h ^= h >> 15;                          /* mixed, so that x and y do not line up */
        h *= 2246822519u;
        h ^= h >> 13;
        int32_t x = (int32_t)((h >> 8) % 240u) - (int32_t)((f * sp) % 240u), y = (int32_t)((h >> 20) % 240u);
        uint16_t c = layer == 0 ? th->line : layer == 1 ? th->dim : RGB(255, 255, 255);
        if (x < 0)
            x += 240;
        rect(x, y, layer == 2 ? 2 : 1, 1, c);
    }
    /* the logo drops in and bounces */
    {
        static const char *const LOGO = "FM1-TRAX";
        uint16_t grad[32], out[32];
        int32_t y = 64, w = text_w(&FONT_L, LOGO), x = 120 - w / 2, k;
        if (t < 900u) {                        /* falling, then a damped bounce */
            int32_t a = (int32_t)t;
            y = a < 500 ? -40 + 104 * a * a / 250000 : 64 - (int32_t)(isin((uint32_t)(a - 500) * 16u / 100u) < 0 ? 0 : isin((uint32_t)(a - 500) * 16u / 100u)) * 20 * (900 - a) / 400 / 127;
        }
        for (k = 0; k < 32; k++) {
            grad[k] = k < 14 ? mixc(RGB(255, 255, 255), RGB(70, 140, 255), k * 18)
                             : k < 16 ? RGB(255, 255, 255) : mixc(RGB(120, 60, 30), RGB(255, 210, 140), (k - 16) * 16);
            out[k] = RGB(0, 0, 0);
        }
        text_grad(x - 1, y, &FONT_L, LOGO, out);
        text_grad(x + 1, y, &FONT_L, LOGO, out);
        text_grad(x, y + 2, &FONT_L, LOGO, out);
        text_grad(x, y, &FONT_L, LOGO, grad);
        if (t > 900u && t < 1500u)             /* a glint runs across */
            rect(x + (int32_t)(t - 900u) * w / 600, y, 3, 32, RGB(255, 255, 255));
    }
    if (t > 1000u) {
        text_c(120, 104, &FONT_B, "MEGA DRIVE SOUND TRACKER", th->acc);
        text_c(120, 124, &FONT_XS, "YM2612 FM  +  SN76489 PSG", th->dim);
    }
    if (OM_REV[0] && t > 1000u)
        text_c(120, 140, &FONT_XS, "REV " OM_REV, th->dim);
    /* copper bars and the sine scroller */
    copper(160, 40, f * 3u);
    {
        static const char *const SCROLL =
            "      WELCOME TO FM1-TRAX ... SIX FM CHANNELS, FOUR PSG, ONE DAC ... 128 FM PRESETS, 32 PSG, "
            "48 DRUMS ... 256 SONG SLOTS ... PRESS ANY BUTTON TO START TRACKING ...      ";
        int32_t n = (int32_t)str_len(SCROLL), off = (int32_t)(f * 4u) % (n * 16), x;
        for (x = -(off % 16); x < 240; x += 16) {
            int32_t ci = (off / 16 + (x + off % 16) / 16) % n;
            char ch[2] = {SCROLL[ci], 0};
            int32_t y = 164 + isin((uint32_t)(x / 5) + f * 2u) * 8 / 127;
            uint16_t g[32];
            int k;
            for (k = 0; k < 32; k++)           /* each letter its colour, lighter at the top */
                g[k] = mixc(RGB(255, 255, 255), rainbow((uint32_t)x * 2u + f * 12u), 80 + k * 5);
            text_grad(x + 2, y + 2, &FONT_L, ch, (const uint16_t[32]){0});
            text_grad(x, y, &FONT_L, ch, g);
        }
    }
    if ((f / 15u) & 1u)
        text_c(120, 216, &FONT_XS, "PRESS ANY BUTTON", th->text);
}

/* ---------------------------------------------------------- the link --- */
void ui_song_replaced(int slot)
{
    if (slot >= 0) {
        set.song = (uint8_t)slot;
        settings_save();
    }
    song_loaded();
    ui.dirty = slot < 0;
    ui_say("SONG RECEIVED", song.title);
}
void ui_song_deleted(int slot)
{
    if (slot == set.song)
        ui.dirty = 1;
}
void ui_stop(void) { player_post(PC_STOP, 0, 0, 0); }
int ui_song_dirty(void) { return ui.dirty; }

/* ------------------------------------------------------- shared input --- */
static void view_leave(int v)
{
    if (v == V_SETUP && setup_changed) {
        settings_save();
        setup_changed = 0;
    }
    if (v == V_LIB)
        audition_end();
    if (v == V_PAT)
        ui.marking = 0;
}

static void view_key(int note)
{
    if (ui.view == V_LIB) {
        lib_audition(note);
        return;
    }
    if (ui.view == V_PAT) {
        pat_key(note);
        return;
    }
    jam_on(ins_ch(ui.ins), note, ui.ins);     /* on a channel the instrument can play on */
}

static void views_input(void)
{
    int32_t d;
    int v = ui.view;
    if (v == V_INTRO) {
        intro_chime();
        if (plat_ms() - ui.intro_t0 > 7000u)
            go(V_PAT);
        return;
    }
    if (ui.chime && ui.chime < 6) {            /* the intro was cut short: its chime too */
        int c;
        for (c = 0; c < 4; c++)
            player_post(PC_JAM_OFF, (uint16_t)c, 0, 0);
        ui.chime = 6;
    }
    if (ui.chime == 6 && audition_on && v != V_LIB) {
        audition_end();
        ui.chime = 7;
    }
    /* the views' buttons */
    if (pressed(B_HOME)) {
        go(v == V_PAT ? V_STAGE : V_PAT);
        return;
    }
    if (pressed(B_SEQ)) {
        if (v == V_ORD)
            go(V_SONG);
        else {
            ui.o_ord = ui.ord;
            go(V_ORD);
        }
        return;
    }
    if (pressed(B_GLO)) {
        if (v == V_SONGS) {
            go(V_SETUP);
        } else {
            ui.sl_pos = set.song;
            go(V_SONGS);
        }
        return;
    }
    if (pressed(B_EDIT) && v != V_INS && v != V_LIB) {
        const cell_t *c = song_cell_ro(&song, cur_ch(), cur_pat(cur_ch()), ui.row);
        if (v == V_PAT && c && c->ins != VAL_NONE)
            ui.ins = c->ins;                   /* the instrument under the cursor */
        if (song.ins[ui.ins].type == INS_PSG)
            ui.i_page = PG_MAC, ui.i_mac = MAC_VOL;
        else if (ui.i_page == PG_MAC && song.ins[ui.ins].type == INS_FM && ui.i_mac == MAC_VOL)
            ui.i_page = PG_TONE, ui.i_row = clampi(ui.i_row, 0, NTONE - 1);
        go(V_INS);
        return;
    }
    /* the transport */
    if (pressed(B_PLAY)) {
        if (player.playing) {
            player_post(PC_STOP, 0, 0, 0);
        } else {
            int from = v == V_ORD ? ui.o_ord : ui.ord;
            player_post(PC_PLAY, (uint16_t)from, 0, 0);
        }
    }
    if (hold_once(B_PLAY, 1000u)) {
        player_post(PC_PLAY, 0, 0, 0);
        ui_say("PLAYING", "FROM THE TOP");
    }
    if (pressed(B_REC)) {
        ui.edit ^= 1u;
        ui_say(ui.edit ? "EDIT MODE" : "EDIT OFF", ui.edit ? "KEYS WRITE NOTES" : "");
    }
    if (pressed(B_SAVE) && v != V_SONGS && v != V_LIB)
        save_song();
    if (pressed(B_ARP) && (v == V_PAT || v == V_ORD))
        undo_swap();
    if (v != V_INS && v != V_LIB && (d = enc(EN_PRESET)) != 0) {   /* the instrument the keys play */
        char b[24];
        ui.ins = (uint8_t)clampi(ui.ins + d, 0, MAX_INS - 3);
        hex2(b, ui.ins);
        b[2] = ' ';
        str_cpy(b + 3, song.ins[ui.ins].type == INS_EMPTY ? "(EMPTY)" : song.ins[ui.ins].name, 18);
        ui_say("INSTRUMENT", b);
    }
    switch (v) {
    case V_PAT: pat_input(); break;
    case V_ORD: ord_input(); break;
    case V_SONG: song_view_input(); break;
    case V_INS: ins_input(); break;
    case V_LIB: lib_input(); break;
    case V_SONGS: songs_input(); break;
    case V_SETUP: setup_input(); break;
    case V_STAGE:
        if (released(B_LFO) && !ui.hold_done[B_LFO]) {
            player.mute ^= 1u << cur_ch();
            mute_send();
        }
        if ((d = enc(EN_ALGO)) != 0)
            ui.ch = clampi(ui.ch + (d > 0 ? 1 : -1), 0, nch_song() - 1);
        break;
    default: break;
    }
}

static void views_draw(void)
{
    switch (ui.view) {
    case V_PAT: pat_draw(); break;
    case V_STAGE: stage_draw(); break;
    case V_ORD: ord_draw(); break;
    case V_SONG: song_view_draw(); break;
    case V_INS: ins_draw(); break;
    case V_LIB: lib_draw(); break;
    case V_SONGS: songs_draw(); break;
    case V_SETUP: setup_draw(); break;
    case V_INTRO: intro_draw(); break;
    default: break;
    }
}
