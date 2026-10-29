/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the instruments in the firmware: 128 FM patches and 32 PSG instruments in groups, and
 * the DAC kit's samples (made by tools/gen_drums.py). */
#pragma once
#include <stdint.h>
#include "ins.h"

#define NPRESET_FM 128
#define NPRESET_PSG 32
#define NGROUP_FM 16
#define NGROUP_PSG 4

typedef struct {
    const char *name;
    fm_patch_t p;
} fm_preset_t;

extern const fm_preset_t PRESET_FM[NPRESET_FM];
extern const char *const GROUP_FM[NGROUP_FM];      /* 8 patches each, in order */
extern const char *const GROUP_PSG[NGROUP_PSG];

/* the PSG and DAC presets as whole instruments */
void preset_psg(int i, ins_t *out);
void preset_fm(int i, ins_t *out);
void preset_dac(int i, ins_t *out);
int preset_dac_count(void);

void presets_default_song(void *song);  /* a new song's instruments */
void presets_bank_init(void);          /* the DAC kit as the player's sample bank */
