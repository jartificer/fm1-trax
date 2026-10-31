/* SPDX-License-Identifier: GPL-3.0-only */
/* The battery's bars (firmware/src/app/battery.h) and the device's smoothing (plat_fm1.c's
 * batt_sample, copied here): the right level for each reading, no flicker on noise or on the dip as
 * the audio draws, and a smoothed reading that reaches the true one (the old whole-count /32 stuck
 * up to 31 counts away). */
#include <stdio.h>
#include <stdlib.h>
#include "../../firmware/src/app/battery.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int32_t q4;
static int lvl = -1;
static void sample(int32_t a)                  /* as plat_fm1.c's batt_sample */
{
    if (a <= 0)
        return;
    q4 = q4 ? q4 + (a * 16 - q4) / 16 : a * 16;
    lvl = batt_level(q4 / 16, lvl);
}

int main(void)
{
    int32_t r;
    int i, prev, changes;
    /* the plain levels */
    CHECK(batt_bars(0) == 0 && batt_bars(530) == 0 && batt_bars(531) == 1 && batt_bars(560) == 1 &&
          batt_bars(561) == 2 && batt_bars(591) == 3 && batt_bars(1023) == 3, "bars");
    /* from nothing: the plain level */
    for (r = 0; r < 1024; r++)
        CHECK(batt_level(r, -1) == batt_bars(r), "first level at %d", (int)r);
    /* a level is never more than one hysteresis away from the plain one, and always 0..3 */
    for (prev = -1; prev <= 4; prev++)
        for (r = 0; r < 1024; r++) {
            int l = batt_level(r, prev);
            CHECK(l >= 0 && l <= 3, "range %d %d", (int)r, prev);
            CHECK(l >= batt_bars(r - BATT_HYST) && l <= batt_bars(r + BATT_HYST), "near %d %d", (int)r, prev);
        }
    /* noise of +-3 counts around every threshold: the level does not change */
    for (i = 0; i < 3; i++) {
        int l = batt_level(BATT_TH[i], -1);
        changes = 0;
        for (r = 0; r < 2000; r++) {
            int n = batt_level(BATT_TH[i] + (rand() % 7) - 3, l);
            changes += n != l;
            l = n;
        }
        CHECK(changes <= 1, "flicker at threshold %d: %d changes", BATT_TH[i], changes);
    }
    /* the smoothing reaches the true reading, whole counts, from above and below */
    q4 = 0;
    lvl = -1;
    sample(600);
    for (i = 0; i < 400; i++)
        sample(570);
    CHECK(q4 / 16 >= 569 && q4 / 16 <= 571 && lvl == 2, "smoothing down: %d, level %d", (int)(q4 / 16), lvl);
    for (i = 0; i < 400; i++)
        sample(520);
    CHECK(q4 / 16 >= 519 && q4 / 16 <= 521 && lvl == 0, "smoothing to empty: %d, level %d", (int)(q4 / 16), lvl);
    for (i = 0; i < 400; i++)
        sample(610);
    CHECK(q4 / 16 >= 609 && q4 / 16 <= 611 && lvl == 3, "charged: %d, level %d", (int)(q4 / 16), lvl);
    /* a failed read (-1, 0) changes nothing */
    sample(-1);
    sample(0);
    CHECK(lvl == 3, "a failed read");
    /* 2 bars, just above the 1-bar threshold, the audio pulling it 15 counts down every other second
     * (20 samples a second): no level change for a short dip, and never the empty level */
    q4 = 0;
    lvl = -1;
    for (i = 0; i < 200; i++)
        sample(566);
    changes = 0;
    prev = lvl;
    for (i = 0; i < 20 * 60; i++) {
        sample((i / 20) & 1 ? 551 : 566);
        CHECK(lvl != 0, "the dip reads empty");
        changes += lvl != prev;
        prev = lvl;
    }
    CHECK(changes <= 60, "the dip: %d changes a minute", changes);
    if (fails)
        return 1;
    printf("battery ok\n");
    return 0;
}
