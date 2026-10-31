/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-TRAX: the song as a .vgm file (VGM 1.71: YM2612 + SN76489 register writes at 44.1 kHz,
 * with the loop and a GD3 tag), the format every Mega Drive music player reads.
 *
 * The song is played from the top to its loop with every register write logged (mdsnd.h's log
 * hook), DAC samples included, so the file is exactly what the tracker plays. */
#pragma once
#include <stdint.h>

/* into buf (cap bytes); returns the file's length, 0 when it did not fit; max_s: give up after so
 * many seconds of song (a song that never reaches its end) */
uint32_t vgm_export(uint8_t *buf, uint32_t cap, uint32_t max_s);
