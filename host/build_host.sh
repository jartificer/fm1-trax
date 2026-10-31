#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the FM1-TRAX host simulator (the whole app on this machine) into build/host/drive_host.
# Same sources as the firmware, -ffp-contract=off like the device (no fused multiply-add).
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
mkdir -p build/gen build/host
[ -f build/gen/felucca_font.h ] || python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
[ -f build/gen/drums.h ] || python3 tools/gen_drums.py build/gen/drums.h >/dev/null
$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter ${HOST_CFLAGS:-} \
    -DOM_HOST -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen -o build/host/drive_host host/drive_host.c \
    firmware/src/dsp/opn2.c firmware/src/dsp/psg.c firmware/src/dsp/mdsnd.c \
    firmware/src/trk/song.c firmware/src/trk/player.c firmware/src/trk/presets.c firmware/src/trk/presets_fm.c \
    firmware/src/trk/vgm.c -lm
echo "host: build/host/drive_host"
