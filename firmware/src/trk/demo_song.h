/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: "DRIVE ON", the demo song every FM-1 starts with (and the tests play): 8 bars in
 * A minor at 150 BPM, bass, lead, stabs, the DAC kit, PSG arpeggios and hats. Built from text so
 * that it reads like a tracker's page. Shared by the host tests, the app and the web build. */
#pragma once
#include "song.h"

static int demo_note(const char *s)        /* "A-4" / "C#5" -> 1..96 */
{
    static const char *const N = "C-C#D-D#E-F-F#G-G#A-A#B-";
    int i;
    for (i = 0; i < 12; i++)
        if (N[2 * i] == s[0] && N[2 * i + 1] == s[1])
            return (s[2] - '0') * 12 + i + 1;
    return 0;
}

/* a channel's notes over 64 rows: tokens "A-4" (note), "..." (empty), "===" (off), separated by
 * spaces, one per row, cycling when shorter than the pattern */
static void demo_line(song_t *s, int ch, int pat, int ins, int vol, const char *line, int step)
{
    int row = 0;
    const char *p = line;
    while (row < s->rows) {
        char tok[4] = {0, 0, 0, 0};
        int k;
        while (*p == ' ')
            p++;
        if (!*p) {
            p = line;
            continue;
        }
        for (k = 0; k < 3 && p[k]; k++)
            tok[k] = p[k];
        p += 3;
        {
            cell_t *c = song_cell_w(s, ch, pat, row);
            if (tok[0] == '=') {
                c->note = NOTE_OFF;
            } else if (tok[0] != '.') {
                c->note = (uint8_t)demo_note(tok);
                c->ins = (uint8_t)ins;
                if (vol >= 0)
                    c->vol = (uint8_t)vol;
            }
        }
        row += step;
    }
}

static void demo_fx(song_t *s, int ch, int pat, int row, int k, uint8_t fx, uint8_t v)
{
    cell_t *c = song_cell_w(s, ch, pat, row);
    c->fx[k] = fx;
    c->fxv[k] = v;
}

static void demo_song(song_t *s)
{
    int o;
    song_init(s);
    {
        const char *t = "DRIVE ON", *a = "FM1-DRIVE";
        int i;
        for (i = 0; t[i]; i++)
            s->title[i] = t[i];
        for (i = 0; a[i]; i++)
            s->author[i] = a[i];
    }
    s->speed[0] = s->speed[1] = 6;
    s->rows = 64;
    s->nord = 4;
    s->loop = 0;
    s->lfo = 0x0B;                         /* LFO on, 5.9 Hz */
    /* instruments: the default band (presets_default_song) */
    /* bass: FM1, eighths, Am F C G a bar each */
    demo_line(s, CH_FM1, 0, 0, 120,
              "A-1 ... A-2 ... A-1 ... A-2 ... A-1 ... A-2 ... G-1 ... G-2 ... "
              "F-1 ... F-2 ... F-1 ... F-2 ... C-2 ... C-3 ... C-2 ... C-3 ... "
              "G-1 ... G-2 ... G-1 ... G-2 ... ", 1);
    demo_line(s, CH_FM1, 1, 0, 120,
              "A-1 ... A-2 ... A-1 ... A-2 ... A-1 ... A-2 ... C-2 ... E-2 ... "
              "F-1 ... F-2 ... F-1 ... F-2 ... G-1 ... G-2 ... G-1 ... B-1 ... ", 1);
    /* lead: FM2, brass */
    demo_line(s, CH_FM2, 0, 1, 110,
              "A-4 ... ... ... C-5 ... E-5 ... ... ... D-5 ... C-5 ... B-4 ... "
              "C-5 ... ... ... A-4 ... ... ... === ... ... ... ... ... ... ... "
              "E-5 ... ... ... G-5 ... E-5 ... D-5 ... ... ... C-5 ... D-5 ... "
              "B-4 ... ... ... ... ... ... ... G-4 ... A-4 ... B-4 ... D-5 ... ", 1);
    demo_line(s, CH_FM2, 1, 1, 110,
              "E-5 ... ... ... A-5 ... G-5 ... E-5 ... ... ... D-5 ... C-5 ... "
              "D-5 ... ... ... ... ... E-5 ... C-5 ... ... ... A-4 ... ... ... "
              "F-5 ... ... ... E-5 ... ... ... D-5 ... ... ... C-5 ... ... ... "
              "B-4 ... ... ... D-5 ... ... ... E-5 ... ... ... ... ... === ... ", 1);
    demo_fx(s, CH_FM2, 0, 0, 0, FX_VIBRATO, 0x42);
    demo_fx(s, CH_FM2, 1, 0, 0, FX_VIBRATO, 0x52);
    demo_fx(s, CH_FM2, 1, 4, 1, FX_TONE_PORTA, 0x30);
    /* stabs: FM3-FM5, e.piano chords on the off-beats */
    {
        static const char *const CH[3][2] = {
            {"... ... A-3 ... ... ... A-3 ... ", "... ... A-3 ... ... ... A-3 ... "},
            {"... ... C-4 ... ... ... C-4 ... ", "... ... C-4 ... ... ... C-4 ... "},
            {"... ... E-4 ... ... ... E-4 ... ", "... ... E-4 ... ... ... E-4 ... "},
        };
        int k;
        for (k = 0; k < 3; k++) {
            demo_line(s, CH_FM3 + k, 0, 2, 90, CH[k][0], 1);
            demo_line(s, CH_FM3 + k, 1, 2, 90, CH[k][1], 1);
        }
        /* the chords follow the bass: F and G bars */
        for (o = 0; o < 2; o++) {
            int bar, r;
            static const char *const CHORD[4][3] = {{"A-3", "C-4", "E-4"}, {"A-3", "C-4", "F-4"},
                                                    {"G-3", "C-4", "E-4"}, {"G-3", "B-3", "D-4"}};
            for (bar = 0; bar < 4; bar++)
                for (r = bar * 16; r < bar * 16 + 16; r++)
                    for (k = 0; k < 3; k++) {
                        cell_t *c = song_cell_w(s, CH_FM3 + k, o, r);
                        if (c->note && c->note != NOTE_OFF)
                            c->note = (uint8_t)demo_note(CHORD[bar][k]);
                    }
        }
    }
    /* FM6: the DAC kit, kick and snare (one sample at a time, as on the console) */
    demo_line(s, CH_FM6, 0, 12, -1, "A-4 ... ... ... ... ... ... ... B-4 ... ... ... ... ... ... ... "
                                    "A-4 ... ... ... ... ... A-4 ... B-4 ... ... ... ... ... ... ... ", 1);
    demo_line(s, CH_FM6, 1, 12, -1, "A-4 ... ... ... ... ... ... ... B-4 ... ... ... ... ... ... ... "
                                    "A-4 ... ... ... A-4 ... ... ... B-4 ... ... ... B-4 ... B-4 ... ", 1);
    {   /* B-4 plays the snare: it is its own instrument */
        int p, r;
        for (p = 0; p < 2; p++)
            for (r = 0; r < 64; r++) {
                cell_t *c = song_cell_w(s, CH_FM6, p, r);
                if (c->note == demo_note("B-4")) {
                    c->note = (uint8_t)demo_note("C-4");
                    c->ins = 13;
                } else if (c->note == demo_note("A-4")) {
                    c->note = (uint8_t)demo_note("C-4");
                }
            }
    }
    /* SQ1: plucked arpeggio an octave up, quiet */
    demo_line(s, CH_SQ1, 0, 9, 9, "A-5 E-5 C-5 E-5 ", 2);
    demo_line(s, CH_SQ1, 1, 9, 9, "E-5 C-5 A-4 C-5 ", 2);
    /* SQ2: a long note under it */
    demo_line(s, CH_SQ2, 0, 8, 6, "A-3 ... ... ... ... ... ... ... ... ... ... ... ... ... ... ... ", 1);
    /* NOI: hats */
    demo_line(s, CH_NOI, 0, 10, 12, "C-4 ... C-4 ... C-4 ... C-4 ... ", 1);
    demo_line(s, CH_NOI, 1, 10, 12, "C-4 ... C-4 C-4 C-4 ... C-4 ... ", 1);
    /* the arrangement: AABB with the stabs and arps */
    for (o = 0; o < 4; o++) {
        int ch;
        for (ch = 0; ch < NCH; ch++)
            s->order[o][ch] = (uint8_t)(o < 2 ? 0 : 1);
        s->order[o][CH_SQ2] = 0;
    }
    demo_fx(s, CH_FM1, 1, 63, 1, FX_BREAK, 0);
}
