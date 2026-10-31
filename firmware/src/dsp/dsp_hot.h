/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: where the sound's inner loops live on the FM-1. Code run from the XIP flash stalls on
 * every cache miss; these functions and their tables are copied to RAM at boot with the flash
 * driver's (.ram_text, app.ld), and the audio interrupt runs them from there. Elsewhere (the host,
 * the web) the attributes are empty. */
#pragma once
#ifdef __PI32V2__
#define DSP_HOT __attribute__((section(".ram_hot")))
#define DSP_HOT_DATA __attribute__((section(".ram_hotd")))
#else
#define DSP_HOT
#define DSP_HOT_DATA
#endif
/* small helpers the hot code uses: always inlined, so that no call leaves RAM for the flash */
#define DSP_INLINE static inline __attribute__((always_inline))
