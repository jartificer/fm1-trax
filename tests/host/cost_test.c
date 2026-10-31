/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1-DRIVE: the sound engine's cost, for the time budget (tests/cost_check.sh counts the host
 * instructions with valgrind). The FM-1 renders 256 frames in an interrupt every 5.8 ms, and the
 * screen gets what is left: a block that takes longer is heard as a stuck buffer.
 *
 *   cost_test stress|demo BLOCKS
 *
 * stress: the worst case the chips can be put in, every FM operator sounding (algorithm 7, LFO with
 * full AM and PM, SSG-EG on half of them) and the four PSG channels on; demo: the demo song played
 * through the player, at its densest from the start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "trk/player.h"
#include "trk/presets.h"
#include "trk/demo_song.h"

song_t song;

static void stress(void)
{
    md_t *m = &player.md;
    int ch, op;
    md_init(m, 44100, 0, 0, 0);
    md_fm(m, 0, 0x22, 0x0F);                                /* LFO on, fastest */
    for (ch = 0; ch < 6; ch++) {
        int port = ch / 3, c = ch % 3;
        md_fm(m, port, (uint8_t)(0xB0 + c), 0x07);          /* algorithm 7: four carriers */
        md_fm(m, port, (uint8_t)(0xB4 + c), 0xF7);          /* both sides, AMS 3, PMS 7 */
        for (op = 0; op < 4; op++) {
            uint8_t r = (uint8_t)(c + 4 * op);
            md_fm(m, port, (uint8_t)(0x30 + r), (uint8_t)(1 + op));
            md_fm(m, port, (uint8_t)(0x40 + r), 0x10);
            md_fm(m, port, (uint8_t)(0x50 + r), 0x1F);
            md_fm(m, port, (uint8_t)(0x60 + r), 0x80);      /* AM on, no decay */
            md_fm(m, port, (uint8_t)(0x70 + r), 0x00);
            md_fm(m, port, (uint8_t)(0x80 + r), 0x0F);
            md_fm(m, port, (uint8_t)(0x90 + r), (uint8_t)(op & 1 ? 0x08 : 0x00));
        }
        md_fm(m, port, (uint8_t)(0xA4 + c), (uint8_t)(0x20 + ch));
        md_fm(m, port, (uint8_t)(0xA0 + c), 0x6A);
        md_fm(m, 0, 0x28, (uint8_t)(0xF0 | (port << 2) | c));
    }
    for (ch = 0; ch < 3; ch++) {
        md_psg(m, (uint8_t)(0x80 | ch << 5 | 0x0A));
        md_psg(m, (uint8_t)(0x08 + ch));
        md_psg(m, (uint8_t)(0x90 | ch << 5));
    }
    md_psg(m, 0xE4);                                        /* white noise */
    md_psg(m, 0xF0);
}

int main(int argc, char **argv)
{
    static int32_t b[2 * 256];
    int n = argc > 2 ? atoi(argv[2]) : 100, i, k;
    if (argc < 2) {
        fprintf(stderr, "usage: cost_test stress|demo BLOCKS\n");
        return 2;
    }
    presets_bank_init();
    player_init(44100);
    if (!strcmp(argv[1], "stress"))
        stress();
    else {
        demo_song(&song);
        player_post(PC_PLAY, 5, 0, 0);                      /* order 5 on: all channels busy */
    }
    for (i = 0; i < n; i++) {
        for (k = 0; k < 2 * 256; k++)
            b[k] = 0;
        player_render(b, 256);
    }
    return 0;
}
