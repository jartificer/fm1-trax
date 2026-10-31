#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build FM1-TRAX for the web pages: the app (host/drive_web.c), the chips and the tracker as one
# freestanding WebAssembly module, web/fm1trax.wasm, which both the emulator and the editor run.
# Needs clang and wasm-ld (LLVM 13 or later); no libc, no Emscripten. Same sources and
# -ffp-contract=off as the firmware.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-clang}"
mkdir -p build/gen build/web
[ -f build/gen/felucca_font.h ] || python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
[ -f build/gen/drums.h ] || python3 tools/gen_drums.py build/gen/drums.h >/dev/null
O=""
for f in host/drive_web.c firmware/src/dsp/opn2.c firmware/src/dsp/psg.c firmware/src/dsp/mdsnd.c \
         firmware/src/trk/song.c firmware/src/trk/player.c firmware/src/trk/presets.c \
         firmware/src/trk/presets_fm.c firmware/src/trk/vgm.c; do
    o="build/web/$(basename "$f" .c).o"
    $CC --target=wasm32 -O2 -ffp-contract=off -std=c99 -ffreestanding -nostdlib \
        -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
        -DOM_HOST -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen -c "$f" -o "$o"
    O="$O $o"
done
# shellcheck disable=SC2086
$CC --target=wasm32 -nostdlib -Wl,--no-entry -Wl,--export-dynamic -Wl,--initial-memory=16777216 \
    -Wl,-z,stack-size=262144 -Wl,--strip-all -o web/fm1trax.wasm $O
echo "web: web/fm1trax.wasm ($(wc -c < web/fm1trax.wasm) bytes)"
