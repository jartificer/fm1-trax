/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX app: the settings (one flash object), the song slots and the instrument library (objects
 * in the chunk store), and the app's entry points. Included by the unity build after gfx.c (drawing
 * helpers in scope) and by the host simulator. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "../trk/song.h"
#include "../trk/player.h"
#include "../trk/presets.h"

#ifndef OM_VERSION                       /* a release build passes its own (tools/build.py --release) */
#define OM_VERSION "DEV"
#endif
#ifndef OM_REV                           /* the build number on the boot screen (tools/build.py) */
#define OM_REV ""
#endif
#define SET_MAGIC 0x56524446u            /* "FDRV" */
#define SET_FORMAT 1u

typedef struct {
    uint32_t magic, format;
    uint8_t song;                        /* the slot in use */
    uint8_t theme;                       /* the screen's colours (ui.c THEMES) */
    uint8_t crt;                         /* CRT scanlines */
    uint8_t intro;                       /* the boot intro */
    uint8_t leds;                        /* key and button lights: 0 off, 1 on */
    uint8_t step;                        /* rows the cursor moves after a note is entered, 0..16 */
    uint8_t oct;                         /* the keys' octave, 0..7 */
    uint8_t follow;                      /* the pattern view follows the play position */
    uint8_t midi_in;                     /* 0 off, 1 the keys (any channel plays the cursor's channel) */
    uint8_t wipes;                       /* the views change with a wipe */
    uint8_t rsv[22];
} settings_t;

extern settings_t set;

#define NTHEMES 6

void settings_defaults(void);
int settings_load(void);                 /* 0 = loaded; else the defaults are in place */
int settings_save(void);                 /* 0 = saved and verified */

/* the chunk store's objects: the songs, then the library's banks of 16 instruments */
#define NSLOTS 256
#define OBJ_SONG(n) (n)
#define LIB_NFM 256                      /* user FM instruments */
#define LIB_NPSG 128                     /* user PSG (and DAC) instruments */
#define LIB_N (LIB_NFM + LIB_NPSG)
#define LIB_BANK 16
#define OBJ_LIB(bank) (NSLOTS + (bank))  /* banks 0..15: FM; 16..23: PSG */

/* the songs in flash: what each slot holds (titles read at boot, kept up to date by saves) */
typedef struct {
    uint8_t used;
    char title[24];
    uint32_t bytes;                      /* in flash (compressed) */
} slot_info_t;
extern slot_info_t slot_info[NSLOTS];
void slots_scan(void);
/* with player.lock set (the render leaves the song alone): */
int song_load_slot(int slot);            /* 0 loaded, 1 empty slot (a new song), -1 unreadable (a new song) */
int song_save_slot(int slot);            /* 0 = saved and verified, -10 = no room */
int song_delete_slot(int slot);          /* 0 = deleted */
void song_boot_load(void);               /* boot, before the audio starts: the slot in use */
uint32_t song_packed(const song_t *s, void (*put)(void *ctx, const uint8_t *b, uint32_t n), void *ctx);

/* the library: slot 0..LIB_N-1; an empty slot reads as INS_EMPTY */
void lib_get(int slot, ins_t *out);
int lib_put(int slot, const ins_t *in);  /* 0 = saved (its bank rewritten) */
int lib_used(int slot);
void lib_forget(void);                   /* the banks in flash changed (the link) */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
void ui_say(const char *a, const char *b);
/* the link (link.c) tells the UI */
void ui_song_replaced(int slot);         /* a song came in (saved to slot; -1: only in memory) */
void ui_song_deleted(int slot);
void ui_stop(void);
int ui_song_dirty(void);
