/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the demo song every FM-1 starts with (and the tests play): "Sunset Beach Galaxy" by
 * Tobikomi, from the YM2017 cart (GPL-3.0), converted from its VGM by tools/vgm2fm1d.mjs (see
 * assets/songs/README.md). Shared by the host tests, the app and the web build. */
#pragma once
#include "song.h"
#include "demo_song_data.h"

__attribute__((unused)) static int demo_note(const char *s)        /* "A-4" / "C#5" -> 1..96 */
{
    static const char *const N = "C-C#D-D#E-F-F#G-G#A-A#B-";
    int i;
    for (i = 0; i < 12; i++)
        if (N[2 * i] == s[0] && N[2 * i + 1] == s[1])
            return (s[2] - '0') * 12 + i + 1;
    return 0;
}

static int demo_get(void *ctx)
{
    uint32_t *pos = (uint32_t *)ctx;
    return *pos < sizeof DEMO_SONG ? DEMO_SONG[(*pos)++] : -1;
}

static void demo_song(song_t *s)
{
    uint32_t pos = 0;
    if (song_read(s, demo_get, &pos) != 0)
        song_init(s);
}
