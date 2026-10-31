/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the PSG presets, the DAC kit, a new song's instruments (presets.h). */
#include "presets.h"
#include "player.h"
#include "song.h"
#include "drums.h"                     /* build/gen: tools/gen_drums.py */

const char *const GROUP_PSG[NGROUP_PSG] = {"SQUARE", "ARP", "NOISE", "FX"};

/* a PSG preset: name, then its macros as strings of values with '|' the loop point and '/' the
 * release point, e.g. "15 14 12 | 10 9 / 6 3 0" */
typedef struct {
    const char *name;
    const char *vol, *arp, *pitch, *noise;
} psg_preset_t;

static const psg_preset_t PSG[NPRESET_PSG] = {
    /* SQUARE */
    {"PSG SQUARE", "15", 0, 0, 0},
    {"PSG SOFT", "8 11 13 | 13 / 10 7 4 2 0", 0, 0, 0},
    {"PSG PLUCK", "15 13 11 10 9 8 7 6 5 5 4 4 3 3 2 2 1 1 0", 0, 0, 0},
    {"PSG DECAY", "15 15 14 14 13 13 12 12 11 11 10 | 10 / 8 6 4 2 0", 0, 0, 0},
    {"PSG VIBRATO", "13 | 13 / 9 6 3 0", 0, "0 0 0 0 0 0 | 3 3 3 -3 -3 -3 -3 -3 -3 3 3 3", 0},
    {"PSG ECHO", "15 12 9 6 3 0 10 8 6 4 2 0 6 5 4 3 2 1 0", 0, 0, 0},
    {"PSG SWELL", "2 4 6 8 10 12 13 14 | 15 / 12 9 6 3 0", 0, 0, 0},
    {"PSG TREMOLO", "| 15 15 15 12 9 9 9 12 / 8 4 0", 0, 0, 0},
    /* ARP */
    {"ARP MAJOR", "| 13 / 8 4 0", "| 0 4 7", 0, 0},
    {"ARP MINOR", "| 13 / 8 4 0", "| 0 3 7", 0, 0},
    {"ARP OCTAVE", "| 13 / 8 4 0", "| 0 12", 0, 0},
    {"ARP MAJ7", "| 13 / 8 4 0", "| 0 4 7 11", 0, 0},
    {"ARP POWER", "| 13 / 8 4 0", "| 0 7 12", 0, 0},
    {"ARP SUS4", "| 13 / 8 4 0", "| 0 5 7", 0, 0},
    {"ARP DIM", "| 13 / 8 4 0", "| 0 3 6 9", 0, 0},
    {"ARP UP-DOWN", "| 13 / 8 4 0", "| 0 4 7 12 7 4", 0, 0},
    /* NOISE (for NOI: the noise mode macro is 4-7 white, 0-3 periodic; 3 / 7 follow SQ3's pitch) */
    {"HAT CLOSED", "14 9 5 2 0", 0, 0, "4"},
    {"HAT OPEN", "14 13 12 11 10 9 8 7 6 5 4 3 2 1 0", 0, 0, "4"},
    {"SNARE NOISE", "15 13 11 9 8 7 6 5 4 3 2 1 0", 0, 0, "5"},
    {"KICK NOISE", "15 12 8 4 0", 0, 0, "6 6 6 7"},
    {"CRASH NOISE", "15 14 14 13 13 12 12 11 11 10 10 9 9 8 8 7 7 6 6 5 5 4 4 3 3 2 2 1 1 0", 0, 0, "4"},
    {"SHAKER", "6 10 8 5 3 1 0", 0, 0, "4"},
    {"BUZZ BASS", "15 | 15 / 10 5 0", 0, 0, "3"},
    {"WHITE PITCHED", "15 | 14 / 10 5 0", 0, 0, "7"},
    /* FX */
    {"BLIP", "15 12 8 0", "0 12", 0, 0},
    {"COIN", "14 14 14 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0", "0 0 0 5 5 5 5 5 5 5 5 5 5 5 5 5 5", 0, 0},
    {"LASER DOWN", "15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0", 0, "0 -40 -40 -40 -40 -40 -40 -40 -40 -40 -40 -40 -40 -40 -40 -40", 0},
    {"LASER UP", "15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0", 0, "0 48 48 48 48 48 48 48 48 48 48 48 48 48 48 48", 0},
    {"JUMP", "14 14 13 13 12 11 10 9 7 5 3 0", 0, "-30 20 20 20 20 20 20 20 20 20 20 20", 0},
    {"POWERUP", "| 13 / 0", "| 0 4 7 12 16 19 24", "| 4", 0},
    {"SIREN", "| 12 / 8 4 0", 0, "| 6 6 6 6 6 6 6 6 -6 -6 -6 -6 -6 -6 -6 -6", 0},
    {"EXPLODE", "15 15 14 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0", 0, 0, "7 7 6 6 6 6 6 6 6"},
};

static void parse_macro(const char *s, macro_t *m)
{
    int neg = 0, have = 0, v = 0;
    m->len = 0;
    m->loop = m->rel = MACRO_NONE;
    if (!s)
        return;
    for (;; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            have = 1;
            continue;
        }
        if (have && m->len < MACRO_LEN)
            m->v[m->len++] = (int8_t)(neg ? -v : v);
        have = neg = v = 0;
        if (c == '-')
            neg = 1;
        else if (c == '|')
            m->loop = m->len;
        else if (c == '/' && m->len)
            m->rel = (uint8_t)(m->len - 1u);
        if (!c)
            break;
    }
}

static void copy_name(char *d, const char *s)
{
    int i;
    for (i = 0; i < 14 && s[i]; i++)
        d[i] = s[i];
    for (; i < 15; i++)
        d[i] = 0;
}

void preset_fm(int i, ins_t *out)
{
    ins_init(out, INS_FM);
    out->u.fm = PRESET_FM[i % NPRESET_FM].p;
    copy_name(out->name, PRESET_FM[i % NPRESET_FM].name);
}

void preset_psg(int i, ins_t *out)
{
    const psg_preset_t *p = &PSG[i % NPRESET_PSG];
    ins_init(out, INS_PSG);
    copy_name(out->name, p->name);
    parse_macro(p->vol, &out->mac[MAC_VOL]);
    parse_macro(p->arp, &out->mac[MAC_ARP]);
    parse_macro(p->pitch, &out->mac[MAC_PITCH]);
    parse_macro(p->noise, &out->mac[MAC_NOISE]);
}

int preset_dac_count(void) { return NDRUM; }

void preset_dac(int i, ins_t *out)
{
    ins_init(out, INS_DAC);
    out->u.dac.sample = (uint8_t)(i % NDRUM);
    out->u.dac.root = 49;
    copy_name(out->name, DRUM_NAME[i % NDRUM]);
}

static sample_t kit[NDRUM];

void presets_bank_init(void)
{
    int i;
    for (i = 0; i < NDRUM; i++) {
        kit[i].name = DRUM_NAME[i];
        kit[i].data = DRUM_DATA[i];
        kit[i].len = DRUM_LEN[i];
        kit[i].rate = DRUM_RATE;
    }
    bank = kit;
    bank_n = NDRUM;
}

/* a new song's instruments: a band to start from */
void presets_default_song(void *sp)
{
    song_t *s = sp;
    static const uint8_t FM[8] = {8, 16, 24, 40, 56, 32, 64, 120};      /* SYN BASS 1, BRASS, E.PIANO, SQUARE LEAD, STRINGS, DRAWBAR, HARP, POWER UP */
    static const uint8_t PSGI[4] = {0, 2, 16, 18};                    /* SQUARE, PLUCK, HAT, SNARE */
    static const uint8_t DAC[4] = {0, 6, 12, 14};                     /* 808 KICK, SNARE, CLAP, HAT */
    int i;
    for (i = 0; i < 8; i++)
        preset_fm(FM[i], &s->ins[i]);
    for (i = 0; i < 4; i++)
        preset_psg(PSGI[i], &s->ins[8 + i]);
    for (i = 0; i < 4; i++)
        preset_dac(DAC[i], &s->ins[12 + i]);
}
