/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the sound driver. It plays the song (song.h) on the Mega Drive's chips (mdsnd.h) as a
 * Z80 / 68000 driver would: once a tick (the frame: 60 Hz NTSC, 50 Hz PAL, or the song's rate) it
 * reads the rows, runs the effects and the instruments' macros, and writes the chips' registers.
 *
 * Threads (as FM1-GM's sequencer): player_render() runs in the audio interrupt and owns the chips
 * and the play state. The main loop changes them through player_post() (a command ring) and reads
 * the state below for the screen. It edits the song's cells directly (a cell is read once a row);
 * anything that moves patterns or instruments around waits for player.lock. */
#pragma once
#include <stdint.h>
#include "../dsp/mdsnd.h"
#include "song.h"

/* sample bank (the DAC kit and the song's samples) */
typedef struct {
    const char *name;
    const uint8_t *data;               /* 8-bit unsigned */
    uint32_t len;
    uint16_t rate;                     /* Hz */
} sample_t;
extern const sample_t *bank;
extern uint32_t bank_n;

typedef struct {
    uint8_t ins, note;                 /* the instrument and note it plays (note 0: none) */
    uint8_t vol;                       /* the volume column's value: FM 0-127, PSG 0-15 */
    uint8_t key;                       /* keyed on */
    int32_t pitch;                     /* 64ths of a semitone from C-0, before vibrato, arpeggio, macros */
    int32_t target;                    /* tone portamento's */
    int16_t slide;                     /* 01xx / 02xx: per tick */
    uint8_t porta;                     /* 03xx speed */
    uint8_t vib_speed, vib_depth, vib_pos;
    uint8_t arp, arp_pos;
    int8_t vslide;                     /* 0Axy: per tick, x16 */
    int16_t vacc;                      /* the volume slide's fraction */
    uint8_t retrig, cut, delay;        /* 0Cxx, ECxx, EDxx: ticks; 0 none */
    cell_t delayed;
    int8_t fine;                       /* E5xx, 64ths */
    int8_t nslide_n;                   /* E1xy / E2xy: semitones to go (sign: direction) */
    uint8_t nslide_speed;
    uint8_t pan;                       /* FM: bit 7 left, bit 6 right */
    uint8_t mpos[NMAC], released;
    uint8_t rel_ticks;                 /* ticks since the release (stops at 255) */
    int16_t mpitch;                    /* the pitch macro's sum */
    uint8_t noise;                     /* NOI: bit 2 white, bits 0-1 rate */
    uint8_t tl[4];                     /* FM: 12xx-15xx overrides, 0xFF none */
    uint8_t retrigger;                 /* key off then on this tick */
    uint8_t jam;                       /* played from the keys, not the song */
    /* for the screen */
    uint8_t level;                     /* 0..15, the sound's loudness now */
    uint16_t out_fnum, out_period;
} chan_t;

typedef struct {
    md_t md;
    volatile uint8_t lock;             /* set by the main loop: the render leaves the song alone */
    uint8_t playing, loop_pat;         /* loop_pat: play the one order position */
    uint16_t ord, row;                 /* the row playing (or last played) */
    uint8_t tick, speed_i;
    uint32_t tick_left;                /* output samples to the next tick, Q16 */
    uint32_t tick_len;                 /* Q16 */
    int16_t jump, brk;                 /* 0Bxx / 0Dxx pending: -1 none */
    uint8_t speed[2];
    uint8_t ext3, dac, lfo;            /* the chips' state the driver keeps */
    uint32_t mute;                     /* bit per channel */
    uint32_t frames;                   /* ticks since play started */
    uint32_t loops;                    /* times the song has looped */
    uint8_t stop_at_end;               /* stop when the song loops (VGM / WAV export) */
    void (*row_hook)(void);            /* called as each row starts (VGM export: the loop point) */
    chan_t ch[NCH];
} player_t;

extern player_t player;

void player_init(uint32_t out_hz);
void player_reset_chips(void);         /* the song's region, chip, filter; silence */

/* commands from the main loop */
enum {
    PC_PLAY,        /* a: order, b: row, c: 1 = loop that order position */
    PC_STOP,
    PC_JAM_ON,      /* a: channel, b: note, c: instrument */
    PC_JAM_OFF,     /* a: channel */
    PC_MUTE,        /* a | b << 8 | c << 16: the mask */
    PC_RESET,       /* the song changed: chips to its settings */
    PC_INS,         /* a: an instrument that was edited (its next note loads it again) */
};
int player_post(uint8_t cmd, uint16_t a, uint8_t b, uint8_t c);   /* 0 = queued */

void player_render(int32_t *out, uint32_t n);   /* the audio interrupt: n stereo frames into out (zeroed) */
/* the safety net in player_render: samples clipped to PLAYER_CLIP; a level over PLAYER_GUARD drops the
 * block and resets the player and the chips (player_guard_trips counts it) */
#define PLAYER_CLIP 32767
#define PLAYER_GUARD 262144
#define PSG_REL_MAX 240        /* ticks a PSG volume macro's loop after the release may go on (4 s at 60 Hz) */
extern volatile uint32_t player_guard_trips;
extern volatile int32_t player_raw_peak;   /* the last block's peak before the clip */
#ifdef OM_HOST
void player_test_poison(void);
uint32_t player_hanging(void);
#endif

/* for exports and tests: run the commands now, play the song to its loop */
void player_run_commands(void);

/* note names: "C-4", "C#4", "===", "^^^", "---" */
void note_name(uint8_t note, char out[4]);
