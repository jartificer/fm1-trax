/* SPDX-License-Identifier: GPL-3.0-only */
/* The battery's bars from the divider's ADC reading (0..1023, smoothed), Felucca's thresholds, with
 * hysteresis: a level is left only once the reading is BATT_HYST counts past its threshold, so the
 * dip as the audio draws, or the ADC's noise, does not make the icon flicker or the warning come
 * and go. Pure, so the host tests it (tests/host/battery_test.c). */
#pragma once
#include <stdint.h>

#define BATT_HYST 4
static const int16_t BATT_TH[3] = {531, 561, 591};   /* 1, 2, 3 bars from here up */

static inline int batt_bars(int32_t raw)
{
    int l = 0;
    while (l < 3 && raw >= BATT_TH[l])
        l++;
    return l;
}

/* prev: the level shown so far (-1: none yet) */
static inline int batt_level(int32_t raw, int prev)
{
    int up = batt_bars(raw - BATT_HYST), down = batt_bars(raw + BATT_HYST);
    if (prev < 0 || prev > 3)
        return batt_bars(raw);
    if (prev < up)
        return up;
    if (prev > down)
        return down;
    return prev;
}
