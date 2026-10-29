/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: a song, as the tracker edits and plays it (docs/DESIGN.md, 3).
 *
 *   Channels: FM1-FM6, SQ1-SQ3, NOI, and with FM3 extended, FM3's operators 2-4 as channels of their
 *   own (CH_OP2..CH_OP4; FM3 is then its operator 1). An order list of up to 256 positions, each
 *   naming a pattern per channel; patterns of song->rows rows (1..128), up to 256 per channel,
 *   allocated from one pool of rows. A cell: note, instrument, volume, two effects.
 *
 *   In flash and in files a song is a byte stream (song_write / song_read): the header, the
 *   instruments it uses, the orders, and each pattern's rows with the empty ones skipped. */
#pragma once
#include <stdint.h>
#include "ins.h"

enum {
    CH_FM1, CH_FM2, CH_FM3, CH_FM4, CH_FM5, CH_FM6,
    CH_SQ1, CH_SQ2, CH_SQ3, CH_NOI,
    CH_OP2, CH_OP3, CH_OP4,                /* FM3 extended: its operators 2-4 */
    NCH
};
#define NCH_STD 10
static inline int ch_is_fm(int ch) { return ch < CH_SQ1 || ch >= CH_OP2; }
static inline int ch_is_psg(int ch) { return ch >= CH_SQ1 && ch <= CH_NOI; }

#define MAX_ORD 256
#define MAX_PAT 256                        /* per channel */
#define MAX_ROWS 128
#define POOL_ROWS 16384                    /* rows in all patterns of a song */
#define MAX_INS 256

/* a cell */
#define NOTE_NONE 0
#define NOTE_OFF 0xFE                      /* === key off: the envelopes' release */
#define NOTE_CUT 0xFD                      /* ^^^ silence at once */
#define NOTE_MAX 96                        /* notes 1..96: C-0 .. B-7 */
#define VAL_NONE 0xFF                      /* no instrument / volume */
typedef struct {
    uint8_t note, ins, vol;
    uint8_t fx[2], fxv[2];                 /* effect 0 = none: commands are stored + 1 (FX_*) */
} cell_t;

/* effects: the command's value is stored + 1 so that 0 means none; FX_ARP is 00xy */
enum {
    FX_NONE = 0,
    FX_ARP = 0x01,         /* 00xy */
    FX_PORTA_UP,           /* 01xx */
    FX_PORTA_DN,           /* 02xx */
    FX_TONE_PORTA,         /* 03xx */
    FX_VIBRATO,            /* 04xy */
    FX_PAN = 0x09,         /* 08xy */
    FX_SPEED1,             /* 09xx */
    FX_VOL_SLIDE,          /* 0Axy */
    FX_JUMP,               /* 0Bxx */
    FX_RETRIG,             /* 0Cxx */
    FX_BREAK,              /* 0Dxx */
    FX_SPEED = 0x10,       /* 0Fxx */
    FX_LFO,                /* 10xy */
    FX_FB,                 /* 11xx */
    FX_TL1, FX_TL2, FX_TL3, FX_TL4,   /* 12xx-15xx */
    FX_MUL,                /* 16xy */
    FX_DAC,                /* 17xx */
    FX_EXT3,               /* 18xx */
    FX_AR,                 /* 19xx */
    FX_AR1, FX_AR2, FX_AR3, FX_AR4,   /* 1Axx-1Dxx */
    FX_NOISE = 0x21,       /* 20xy */
    FX_SLIDE_UP = 0xE2,    /* E1xy */
    FX_SLIDE_DN,           /* E2xy */
    FX_FINE = 0xE6,        /* E5xx */
    FX_CUT = 0xED,         /* ECxx */
    FX_DELAY,              /* EDxx */
};
const char *fx_name(uint8_t fx);           /* "ARP", "PORTA UP", ... or NULL: not an effect */
uint8_t fx_next(uint8_t fx, int dir);      /* the next effect in the list (for the knob), FX_NONE between */

typedef struct {
    char title[24], author[24];
    uint8_t region;                        /* MD_NTSC / MD_PAL: clocks, and the tick at 60 / 50 Hz */
    uint8_t chip;                          /* OPN2_YM2612 / OPN2_YM3438 */
    uint8_t filter;                        /* MD_FILTER_* */
    uint8_t ext3;                          /* FM3 extended */
    uint16_t tick_hz;                      /* 0: the region's frame rate; else ticks a second x 10 */
    uint8_t speed[2];                      /* ticks per row, alternating (swing); equal: no swing */
    uint8_t rows;                          /* rows per pattern, 1..MAX_ROWS */
    uint8_t hl[2];                         /* row highlights: beat, bar */
    uint8_t lfo;                           /* register 0x22 at the start: bit 3 on, bits 0-2 rate */
    uint16_t nord;                         /* positions in the order list, 1..MAX_ORD */
    uint16_t loop;                         /* where the song goes after its last position */
    uint8_t order[MAX_ORD][NCH];           /* pattern numbers */
    uint16_t pat[NCH][MAX_PAT];            /* each pattern's first row in pool[], PAT_NONE: empty */
    uint16_t pool_used;                    /* rows of pool[] in use */
    ins_t ins[MAX_INS];
    cell_t pool[POOL_ROWS];
} song_t;
#define PAT_NONE 0xFFFFu

extern song_t song;

void song_init(song_t *s);                 /* a new song: one empty pattern, the default instruments */
/* the cell at a row of a channel's pattern; NULL for an empty (unallocated) pattern */
cell_t *song_cell(song_t *s, int ch, int pat, int row);
const cell_t *song_cell_ro(const song_t *s, int ch, int pat, int row);
/* the same, allocating the pattern; NULL when the pool is full */
cell_t *song_cell_w(song_t *s, int ch, int pat, int row);
int song_pat_empty(const song_t *s, int ch, int pat);
void song_compact(song_t *s);              /* frees empty patterns' rows */
int song_set_rows(song_t *s, int rows);    /* 0 = done (patterns cut or extended); -1 no room */
int song_ins_used(const song_t *s, int i); /* an instrument any cell names */
uint32_t song_pool_free(const song_t *s);

/* the byte stream: song_write calls put for each run of bytes, returns the length */
#define SONG_MAGIC "FM1D"
#define SONG_FORMAT 1u
uint32_t song_write(const song_t *s, void (*put)(void *ctx, const uint8_t *b, uint32_t n), void *ctx);
/* get returns the next byte or -1; returns 0 = read, < 0 = not a song / damaged (s is then a new song) */
int song_read(song_t *s, int (*get)(void *ctx), void *ctx);
