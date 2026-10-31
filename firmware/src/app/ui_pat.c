/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the pattern view (HOME), included by ui.c.
 *
 * Six channels fit across: the cursor's channel shows its whole cell (note, instrument, volume, two
 * effects), the five around it their notes, so the "lens" moves with ALGORITHM. Fourteen rows, the
 * cursor's in the middle; the row playing glows in the channels' colours; a strip of the order
 * list runs along the bottom, and each channel's header has its scope and its level. */

#define PAT_X 14                               /* the row numbers' column */
#define W_WIDE 104
#define W_NARROW 22
#define NVISCOL 6

static int cur_ch(void) { return ch_at(ui.ch); }

/* the channel an instrument plays on from the keys, whatever the cursor's channel, by one rule for
 * every kind (the pattern view, the instrument editor, the library and the web editor all use it):
 *   the DAC kit                           FM6
 *   a PSG instrument with a noise macro   NOI (hats, snares, the noise presets)
 *   any other PSG instrument              the cursor's SQ1-SQ3, else SQ1
 *   an FM patch                           the cursor's FM channel, else FM1 */
static int ins_ch_for(const ins_t *in, int ch)
{
    switch (in->type) {
    case INS_DAC: return CH_FM6;
    case INS_PSG:
        if (in->mac[MAC_NOISE].len)
            return CH_NOI;
        return ch_is_psg(ch) && ch != CH_NOI ? ch : CH_SQ1;
    case INS_FM: return ch_is_fm(ch) ? ch : CH_FM1;
    default: return ch;
    }
}
static int ins_ch(int ins) { return ins_ch_for(&song.ins[ins], cur_ch()); }

/* why a note from the keys was not written: the instrument does not play on the cursor's channel */
static void ins_ch_say(int ins)
{
    const ins_t *in = &song.ins[ins];
    ui_say(in->type == INS_DAC ? "DRUMS PLAY ON FM6"
           : in->type == INS_PSG ? (in->mac[MAC_NOISE].len ? "NOISE PLAYS ON NOI" : "TONES PLAY ON SQ1-SQ3")
                                 : "FM PLAYS ON FM1-FM6",
           "NOT WRITTEN HERE");
}

/* the clipboard: a block of cells */
static struct {
    int rows, cols;
    cell_t c[MAX_ROWS][NCH];
} clip __attribute__((section(".pool")));

static void cell_empty(cell_t *c)
{
    memset(c, 0, sizeof *c);
    c->ins = c->vol = VAL_NONE;
}

static int cell_is_empty(const cell_t *c)
{
    return !c || (!c->note && c->ins == VAL_NONE && c->vol == VAL_NONE && !c->fx[0] && !c->fx[1]);
}

/* the position the view shows: the cursor's, or (following) the player's */
static int view_ord(void) { return player.playing && set.follow ? player.ord : ui.ord; }
static int view_row(void) { return player.playing && set.follow ? player.row : ui.row; }

static void cursor_follow(void)
{
    if (player.playing && set.follow) {
        ui.ord = player.ord < song.nord ? player.ord : 0;
        ui.row = player.row < song.rows ? player.row : 0;
    }
}

static void move_row(int d)
{
    int r = ui.row + d;
    while (r < 0) {                            /* past the top: the position before */
        if (ui.ord == 0) {
            r = 0;
            break;
        }
        ui.ord--;
        r += song.rows;
    }
    while (r >= song.rows) {
        if (ui.ord + 1 >= song.nord) {
            r = song.rows - 1;
            break;
        }
        ui.ord++;
        r -= song.rows;
    }
    ui.row = r;
}

static void blk_norm(int *r0, int *r1, int *c0, int *c1)
{
    *r0 = ui.b_r0 < ui.b_r1 ? ui.b_r0 : ui.b_r1;
    *r1 = ui.b_r0 < ui.b_r1 ? ui.b_r1 : ui.b_r0;
    *c0 = ui.b_c0 < ui.b_c1 ? ui.b_c0 : ui.b_c1;
    *c1 = ui.b_c0 < ui.b_c1 ? ui.b_c1 : ui.b_c0;
}

static void blk_copy(void)
{
    int r0, r1, c0, c1, r, c;
    blk_norm(&r0, &r1, &c0, &c1);
    clip.rows = r1 - r0 + 1;
    clip.cols = c1 - c0 + 1;
    for (c = c0; c <= c1; c++)
        for (r = r0; r <= r1; r++) {
            const cell_t *s = song_cell_ro(&song, ch_at(c), cur_pat(ch_at(c)), r);
            if (s)
                clip.c[r - r0][c - c0] = *s;
            else
                cell_empty(&clip.c[r - r0][c - c0]);
        }
}

static void blk_paste(void)
{
    int r, c;
    if (!clip.rows) {
        ui_say("CLIPBOARD EMPTY", "SEL: COPY");
        return;
    }
    for (c = 0; c < clip.cols && ui.ch + c < nch_song(); c++) {
        int ch = ch_at(ui.ch + c);
        for (r = 0; r < clip.rows && ui.row + r < song.rows; r++) {
            cell_t *d = edit_cell(ch, cur_pat(ch), ui.row + r);
            if (!d)
                return;
            *d = clip.c[r][c];
        }
    }
    ui_say("PASTED", "");
}

static void blk_clear(void)
{
    int r0, r1, c0, c1, r, c;
    blk_norm(&r0, &r1, &c0, &c1);
    for (c = c0; c <= c1; c++)
        for (r = r0; r <= r1; r++) {
            cell_t *d = song_cell(&song, ch_at(c), cur_pat(ch_at(c)), r);
            if (d) {
                undo_mark(ch_at(c), cur_pat(ch_at(c)));
                cell_empty(d);
                ui.dirty = 1;
            }
        }
    ui.blk = 0;
}

static void blk_transpose(int d)
{
    int r0, r1, c0, c1, r, c;
    blk_norm(&r0, &r1, &c0, &c1);
    for (c = c0; c <= c1; c++)
        for (r = r0; r <= r1; r++) {
            cell_t *x = song_cell(&song, ch_at(c), cur_pat(ch_at(c)), r);
            if (x && x->note >= 1 && x->note <= NOTE_MAX) {
                undo_mark(ch_at(c), cur_pat(ch_at(c)));
                x->note = (uint8_t)clampi(x->note + d, 1, NOTE_MAX);
                ui.dirty = 1;
            }
        }
}

/* a note from the keys (or MIDI): sound it on the cursor's channel; in edit mode also write it */
static void pat_key(int note)
{
    int ch = cur_ch(), ins = ui.ins;
    jam_on(ins_ch(ins), note, ins);
    ui.last_note = (uint8_t)note;
    if (!ui.edit)
        return;
    if (ins_ch(ins) != ch) {                   /* it would not sound in this column */
        ins_ch_say(ins);
        return;
    }
    if (player.playing) {                      /* live: quantised to the nearest row */
        int o = player.ord, r = player.row;
        uint32_t sp = player.speed[player.speed_i & 1u];
        if (player.tick * 2u >= sp && ++r >= song.rows) {
            r = 0;
            o = o + 1 < song.nord ? o + 1 : song.loop;
        }
        {
            cell_t *c = edit_cell(ch, song.order[o][ch], r);
            if (c) {
                c->note = (uint8_t)note;
                c->ins = (uint8_t)ins;
            }
        }
        return;
    }
    {
        cell_t *c = edit_cell(ch, cur_pat(ch), ui.row);
        if (c) {
            c->note = (uint8_t)note;
            c->ins = (uint8_t)ins;
        }
        move_row(set.step);
    }
}

static void pat_knob(int k, int32_t d)
{
    int ch = cur_ch();
    cell_t *c;
    if (ui.blk && k == 0 && !ui.fxmode) {
        blk_transpose(d);
        return;
    }
    c = edit_cell(ch, cur_pat(ch), ui.row);
    if (!c)
        return;
    if (!ui.fxmode) {
        switch (k) {
        case 0:                                /* the note: semitones (from the last one, on an empty cell) */
            if (c->note < 1 || c->note > NOTE_MAX) {
                c->note = ui.last_note;
                if (c->ins == VAL_NONE)
                    c->ins = ui.ins;
            } else {
                c->note = (uint8_t)clampi(c->note + d, 1, NOTE_MAX);
            }
            ui.last_note = c->note;
            jam_preview(ch, c->note, c->ins == VAL_NONE ? ui.ins : c->ins);
            break;
        case 1: {                              /* the instrument; below 00: none */
            int v = c->ins == VAL_NONE ? -1 : c->ins;
            v = clampi(v + d, -1, MAX_INS - 2);
            c->ins = (uint8_t)(v < 0 ? VAL_NONE : v);
            if (v >= 0)
                ui.ins = (uint8_t)v;
            break;
        }
        case 2: {                              /* the volume; below 0: none */
            int v = c->vol == VAL_NONE ? (d > 0 ? -1 : ins_vol_max(ch) + 1) : c->vol;
            v = clampi(v + d, -1, ins_vol_max(ch));
            c->vol = (uint8_t)(v < 0 ? VAL_NONE : v);
            break;
        }
        default:                               /* effect 1's value */
            if (!c->fx[0])
                c->fx[0] = FX_ARP;
            c->fxv[0] = (uint8_t)clampi(c->fxv[0] + d, 0, 255);
            break;
        }
        return;
    }
    {
        int f = k / 2;
        if (k & 1) {
            if (!c->fx[f])
                c->fx[f] = FX_ARP;
            c->fxv[f] = (uint8_t)clampi(c->fxv[f] + d, 0, 255);
        } else {
            int i, n = d < 0 ? -d : d;
            for (i = 0; i < n; i++)
                c->fx[f] = fx_next(c->fx[f], d < 0 ? -1 : 1);
            {
                const char *nm = fx_name(c->fx[f]);
                ui_say(f ? "FX2" : "FX1", nm ? nm : "NONE");
            }
        }
    }
}

static void pat_input(void)
{
    int32_t d;
    int k;
    cursor_follow();
    if ((d = enc(EN_SELECT)) != 0) {
        if (held(B_SEL)) {
            if (!ui.marking) {
                ui.marking = ui.blk = 1;
                ui.b_r0 = ui.b_r1 = ui.row;
                ui.b_c0 = ui.b_c1 = ui.ch;
            }
            ui.hold_done[B_SEL] = 1;
            move_row(d > 0 ? 1 : -1);
            ui.b_r1 = ui.row;
        } else {
            if (player.playing && set.follow)
                player_post(PC_PLAY, (uint16_t)ui.ord, (uint8_t)clampi(ui.row + d, 0, song.rows - 1), 0);
            move_row(d);
        }
    }
    if ((d = enc(EN_ALGO)) != 0) {
        if (held(B_SEL)) {
            if (!ui.marking) {
                ui.marking = ui.blk = 1;
                ui.b_r0 = ui.b_r1 = ui.row;
                ui.b_c0 = ui.b_c1 = ui.ch;
            }
            ui.hold_done[B_SEL] = 1;
            ui.ch = clampi(ui.ch + (d > 0 ? 1 : -1), 0, nch_song() - 1);
            ui.b_c1 = ui.ch;
        } else {
            ui.ch = clampi(ui.ch + (d > 0 ? 1 : -1), 0, nch_song() - 1);
        }
    }
    for (k = 0; k < 4; k++)
        if ((d = enc(EN_K1 + k)) != 0)
            pat_knob(k, d);
    if (pressed(B_FX))
        ui.fxmode ^= 1u;
    if (pressed(B_ENV)) {
        if (ui.blk) {
            blk_clear();
            ui_say("BLOCK CLEARED", "");
        } else {
            cell_t *c = edit_cell(cur_ch(), cur_pat(cur_ch()), ui.row);
            if (c) {
                if (c->note == NOTE_OFF)
                    cell_empty(c);
                else
                    c->note = NOTE_OFF;
                move_row(set.step);
            }
        }
    }
    /* LFO: mute (tap) / solo (held) */
    if (hold_once(B_LFO, 600u)) {
        uint32_t me = 1u << cur_ch(), all = (1u << NCH) - 1u;
        player.mute = (player.mute == (all & ~me)) ? 0u : (all & ~me);
        mute_send();
        ui_say(player.mute ? "SOLO" : "ALL ON", ch_name(cur_ch()));
    } else if (released(B_LFO) && !ui.hold_done[B_LFO]) {
        player.mute ^= 1u << cur_ch();
        mute_send();
    }
    /* SEL: the block */
    if (released(B_SEL)) {
        if (ui.marking) {
            ui.marking = 0;                    /* the block stays marked */
        } else if (!ui.hold_done[B_SEL]) {
            if (!ui.blk) {                     /* nothing marked: the cursor's pattern */
                ui.b_r0 = 0;
                ui.b_r1 = song.rows - 1;
                ui.b_c0 = ui.b_c1 = ui.ch;
            }
            blk_copy();
            ui.blk = 0;
            ui_say("COPIED", ui.b_c0 == ui.b_c1 && ui.b_r1 - ui.b_r0 + 1 == song.rows ? "THE PATTERN" : "THE BLOCK");
        }
    } else if (!ui.marking && hold_once(B_SEL, 1000u)) {
        blk_paste();
    }
}

/* ------------------------------------------------------------- drawing --- */
static void cell_text(const cell_t *c, char *note, char *ins, char *vol, char *f1, char *f2)
{
    note_name(c ? c->note : 0, note);
    note[3] = 0;
    if (!c || c->note == 0)
        note[0] = note[1] = note[2] = '.';
    if (c && c->ins != VAL_NONE)
        hex2(ins, c->ins);
    else
        ins[0] = ins[1] = '.', ins[2] = 0;
    if (c && c->vol != VAL_NONE)
        hex2(vol, c->vol);
    else
        vol[0] = vol[1] = '.', vol[2] = 0;
    {
        char *f[2] = {f1, f2};
        int i;
        for (i = 0; i < 2; i++) {
            if (c && c->fx[i]) {
                hex2(f[i], (uint32_t)(c->fx[i] - 1u));
                hex2(f[i] + 2, c->fxv[i]);
            } else {
                f[i][0] = f[i][1] = f[i][2] = f[i][3] = '.';
                f[i][4] = 0;
            }
        }
    }
}

static uint16_t muted_col(int ch) { return player.mute >> ch & 1u ? th->dim : mixc(chan_col(ch), RGB(255, 255, 255), 60); }

static void chan_header(int32_t x, int32_t w, int ch)
{
    uint16_t cc = chan_col(ch), sc_c = muted_col(ch);
    int muted = (int)(player.mute >> ch & 1u);
    int lv = player.ch[ch].level;
    int sc = ch >= CH_OP2 ? CH_FM3 : ch, i, py = 0;
    if (!band_has(24, 22))
        return;
    rect(x, 24, w - 2, 22, th->panel);
    rect(x, 24, w - 2, 1, muted ? th->line : mixc(th->line, cc, 80 + lv * 11));   /* lit as it plays */
    text(x + 2, 25, &FONT_XS, ch_name(ch), muted ? th->dim : (lv > 10 ? th->hi : cc));
    if (muted)
        text(w > 30 ? x + w - 10 : x + 2, w > 30 ? 25 : 37, &FONT_XS, "M", th->rec);
    if (w > 30) {
        for (i = 0; i < 5; i++)                /* the level, as LEDs */
            rect(x + w - 18 - i * 4, 28, 3, 6, lv > i * 3 ? (i >= 4 ? th->rec : cc) : th->line);
    } else {                                   /* narrow: a bar under the name */
        int bw = lv * (w - 6) / 15;
        rect(x + 2, 33, w - 6, 2, th->line);
        rect(x + 2, 33, bw > w - 6 ? w - 6 : bw, 2, lv > 12 ? th->rec : cc);
    }
    rect(x + 1, 37, w - 4, 8, mixc(th->bg, cc, 20));
    /* the scope: the channel's last 128 samples, squeezed into its width */
    for (i = 0; i < w - 4; i++) {
        int s = player.md.scope[sc][(player.md.scope_pos + (uint32_t)(i * MD_SCOPE / (w - 4))) % MD_SCOPE];
        int y = 41 - s * 4 / 64;
        y = y < 37 ? 37 : y > 44 ? 44 : y;
        if (i)
            cv_line(x + i, py, x + 1 + i, y, sc_c);
        py = y;
    }
}

static void pat_draw(void)
{
    int nc = nch_song(), i, vis0, nv = nc < NVISCOL ? nc : NVISCOL;
    int o = view_ord(), cr = view_row(), r0 = cr - NROWS_VIS / 2;
    int32_t xs[NCH], ws[NCH], x;
    /* the lens: keep the cursor's channel in view */
    if (ui.ch < ui.scroll)
        ui.scroll = ui.ch;
    if (ui.ch >= ui.scroll + nv)
        ui.scroll = ui.ch - nv + 1;
    vis0 = ui.scroll;
    x = PAT_X;
    for (i = 0; i < nv; i++) {
        xs[i] = x;
        ws[i] = vis0 + i == ui.ch ? W_WIDE : W_NARROW;
        x += ws[i];
    }
    for (i = 0; i < nv; i++)
        chan_header(xs[i], ws[i], ch_at(vis0 + i));
    if (band_has(24, 22))
        text(0, 30, &FONT_XS, vis0 > 0 ? "<" : " ", th->dim), text(234, 30, &FONT_XS, vis0 + nv < nc ? ">" : " ", th->dim);
    /* the rows: each channel's column faintly in its colour */
    for (i = 0; i < nv; i++)
        rect(xs[i], ROWS_Y, ws[i] - 2, NROWS_VIS * ROW_H, mixc(th->bg, chan_col(ch_at(vis0 + i)), 14));
    for (i = 0; i < NROWS_VIS; i++) {
        int r = r0 + i, j, y = ROWS_Y + i * ROW_H;
        uint16_t rowbg = th->bg;
        char b[4];
        if (!band_has(y, ROW_H) || r < 0 || r >= song.rows)
            continue;
        if (song.hl[1] && r % song.hl[1] == 0)
            rowbg = mixc(th->bg, th->line, 200);
        else if (song.hl[0] && r % song.hl[0] == 0)
            rowbg = mixc(th->bg, th->line, 110);
        if (r == cr)
            rowbg = player.playing && set.follow ? mixc(th->sel, th->play, 50) : th->sel;
        if (rowbg != th->bg)
            rect(0, y, 240, ROW_H, rowbg);
        hex2(b, (uint32_t)r);
        text(1, y, &FONT_XS, b, r == cr ? th->hi : (song.hl[0] && r % song.hl[0] == 0) ? th->text : th->dim);
        for (j = 0; j < nv; j++) {
            int ch = ch_at(vis0 + j);
            const cell_t *c = song_cell_ro(&song, ch, song.order[o][ch], r);
            char n[4], in[3], vo[3], f1[5], f2[5];
            uint16_t cc = chan_col(ch), nc_ = c && c->note ? cc : th->dim;
            int playing_here = player.playing && player.ord == o && player.row == r;
            if (playing_here && c && c->note && c->note <= NOTE_MAX)
                rect(xs[j], y, ws[j] - 2, ROW_H, mixc(rowbg, cc, 110));   /* the note just played lights up */
            if (ui.blk) {
                int br0, br1, bc0, bc1;
                blk_norm(&br0, &br1, &bc0, &bc1);
                if (r >= br0 && r <= br1 && vis0 + j >= bc0 && vis0 + j <= bc1)
                    rect(xs[j], y, ws[j] - 2, ROW_H, mixc(rowbg, th->acc, 90));
            }
            cell_text(c, n, in, vo, f1, f2);
            if (c && (c->note == NOTE_OFF || c->note == NOTE_CUT))
                nc_ = th->text;
            if (ws[j] == W_NARROW) {
                if (c && !c->note && !cell_is_empty(c))
                    text(xs[j] + 2, y, &FONT_XS, " : ", mixc(th->dim, cc, 128));
                else
                    text(xs[j] + 2, y, &FONT_XS, n, nc_);
                continue;
            }
            text(xs[j] + 2, y, &FONT_XS, n, nc_);
            text(xs[j] + 23, y, &FONT_XS, in, c && c->ins != VAL_NONE ? th->acc : th->dim);
            text(xs[j] + 36, y, &FONT_XS, vo, c && c->vol != VAL_NONE ? th->play : th->dim);
            text(xs[j] + 52, y, &FONT_XS, f1, c && c->fx[0] ? th->hi : th->dim);
            text(xs[j] + 78, y, &FONT_XS, f2, c && c->fx[1] ? th->hi : th->dim);
        }
    }
    /* the cursor: a box whose colour runs round the wheel */
    {
        int j = ui.ch - vis0;
        int32_t y = ROWS_Y + (NROWS_VIS / 2) * ROW_H;
        uint16_t c = rainbow(ui.frame * 24u);
        if (j >= 0 && j < nv) {
            if (ui.fxmode)
                frame_rect(xs[j] + 50, y - 1, 52, ROW_H + 2, c);
            else
                frame_rect(xs[j], y - 1, 76, ROW_H + 2, c);
        }
    }
    /* the edit mode's frame */
    if (ui.edit) {
        uint16_t rc = mixc(th->rec, th->bg, player.playing ? (int32_t)(ui.frame % 16u) * 8 : 0);
        frame_rect(0, ROWS_Y - 1, 240, NROWS_VIS * ROW_H + 2, rc);
    }
    /* the instrument and octave, then the order strip */
    if (band_has(203, 21)) {
        char b[32];
        const ins_t *in = &song.ins[ui.ins];
        int32_t x;
        rect(0, 203, 240, 21, th->panel);
        b[0] = 'O', b[1] = (char)('0' + set.oct), b[2] = 0;
        x = text(4, 204, &FONT_XS, b, th->acc) + 6;
        hex2(b, ui.ins);
        x = text(x, 204, &FONT_XS, b, th->hi) + 4;
        text(x, 204, &FONT_XS, in->type == INS_EMPTY ? "(EMPTY)" : in->name, th->text);
        if (ui.edit)
            text_r(236, 204, &FONT_XS, player.playing ? "REC" : "EDIT", th->rec);
        else if (set.step != 1) {
            b[0] = 'S', b[1] = 'T', b[2] = 'E', b[3] = 'P', b[4] = ' ';
            fmt_int(b + 5, set.step);
            text_r(236, 204, &FONT_XS, b, th->dim);
        }
    }
    if (band_has(217, 6)) {
        int p, w = song.nord > 0 ? 236 / (song.nord > 59 ? 59 : song.nord) : 4;
        int first = o > 58 ? o - 58 : 0;
        if (w > 16)
            w = 16;
        for (p = first; p < song.nord && (p - first) * w < 236; p++) {
            uint16_t c = p == o ? th->hi : p == song.loop ? th->acc : th->line;
            rect(2 + (p - first) * w, 218, w - 1, 4, c);
        }
    }
    /* the footer: the knobs */
    {
        const cell_t *c = song_cell_ro(&song, cur_ch(), cur_pat(cur_ch()), ui.row);
        char n[4], in[3], vo[3], f1[5], f2[5], a[10], b[10], cc[10], dd[10];
        cell_text(c, n, in, vo, f1, f2);
        if (ui.blk) {
            footer("TRANSP", "ENV:CLR", "SEL:COPY", "");
        } else if (!ui.fxmode) {
            char nn[10];
            str_cpy(nn, "NOTE ", 6), str_cpy(nn + 5, n, 4);
            str_cpy(a, "INS ", 5), str_cpy(a + 4, in, 3);
            str_cpy(b, "VOL ", 5), str_cpy(b + 4, vo, 3);
            str_cpy(cc, "FX ", 4), cc[3] = f1[2], cc[4] = f1[3], cc[5] = 0;
            footer(nn, a, b, cc);
        } else {
            const char *n1 = c && c->fx[0] ? fx_name(c->fx[0]) : "FX1", *n2 = c && c->fx[1] ? fx_name(c->fx[1]) : "FX2";
            a[0] = f1[2], a[1] = f1[3], a[2] = 0;
            dd[0] = f2[2], dd[1] = f2[3], dd[2] = 0;
            footer(n1 ? n1 : "FX1", a, n2 ? n2 : "FX2", dd);
        }
    }
}
