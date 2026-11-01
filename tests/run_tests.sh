#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# FM1-TRAX host tests: the chips (the YM2612 compared with Nuked-OPN2, LGPL-2.1, fetched into
# build/ref at a fixed commit, used by the tests only, never built into the firmware; without it,
# offline or NO_NUKED=1, the comparison is skipped), the tracker, the flash store, the installer, and
# the whole firmware app in the simulator (tests/scenarios/*.fd), with screenshots and audio in
# build/scenarios/. With a built package (./build.sh) the update path is tested too.
#   tests/run_tests.sh [WAVDIR]
set -u
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
B=build/host
FAIL=0
mkdir -p "$B" build/ref build/gen build/scenarios ${1:+"$1"}
run() {
    name="$1"; shift
    if "$@" > "$B/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $B/$name.log)"
        tail -15 "$B/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
NUKED_COMMIT=335747d78cb0abbc3b55b004e62dad9763140115
NUKED=build/ref/Nuked-OPN2
REF=""
if [ "${NO_NUKED:-0}" != 1 ]; then
    if [ ! -f "$NUKED/ym3438.c" ]; then
        rm -rf "$NUKED"
        if git clone -q https://github.com/nukeykt/Nuked-OPN2 "$NUKED" 2>/dev/null; then
            git -C "$NUKED" checkout -q "$NUKED_COMMIT"
        fi
    fi
    if [ -f "$NUKED/ym3438.c" ] && [ "$(git -C "$NUKED" rev-parse HEAD)" = "$NUKED_COMMIT" ]; then
        REF="-DWITH_NUKED -I$NUKED $NUKED/ym3438.c"
    else
        echo "  note Nuked-OPN2 not available: the reference comparison is skipped"
    fi
fi
DSP="firmware/src/dsp/opn2.c firmware/src/dsp/psg.c firmware/src/dsp/mdsnd.c"
TRK="firmware/src/trk/song.c firmware/src/trk/player.c firmware/src/trk/presets.c firmware/src/trk/presets_fm.c firmware/src/trk/vgm.c"
python3 tools/gen_drums.py build/gen/drums.h >/dev/null
python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
# the FM-1 has no fused multiply-add; the host build must compute the same samples
CFLAGS="-O2 -Wall -Wextra -Wdouble-promotion -ffp-contract=off"
# shellcheck disable=SC2086
run engine sh -c "$CC $CFLAGS -o $B/engine_test tests/host/engine_test.c $DSP $REF -lm && $B/engine_test ${1:-}"
# shellcheck disable=SC2086
run tracker sh -c "$CC $CFLAGS -Ibuild/gen -o $B/trk_test tests/host/trk_test.c $TRK $DSP -lm && $B/trk_test ${1:-}"
# fuzzing the sound path for loud noise: random songs, live commands, damaged streams, and the safety net
# shellcheck disable=SC2086
run fuzz sh -c "$CC $CFLAGS -DOM_HOST -Ibuild/gen -o $B/fuzz_test tests/host/fuzz_test.c $TRK $DSP -lm && $B/fuzz_test ${FUZZ_SEEDS:-300}"
# the engine's output, hashed sample by sample: an optimisation must not change a single sample
# shellcheck disable=SC2086
run golden sh -c "$CC $CFLAGS -DOM_HOST -Ibuild/gen -o $B/golden_test tests/host/golden_test.c $TRK $DSP -lm && $B/golden_test tests/host/golden.txt 200"
# the time budget: host instructions a block (with clang, as the FM-1's compiler)
if command -v clang >/dev/null 2>&1 && command -v valgrind >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    run cost sh -c "clang $CFLAGS -Ifirmware/src -Ibuild/gen -o $B/cost_test tests/host/cost_test.c $TRK $DSP -lm && sh tests/cost_check.sh $B/cost_test"
else
    echo "  skip cost (needs clang and valgrind)"
fi
run storage sh -c "$CC -O2 -Wall -Werror -o $B/storage_test tests/storage_test.c && $B/storage_test"
run lz4 sh -c "$CC -O2 -Wall -Werror -o $B/lz4_test tests/host/lz4_test.c && $B/lz4_test"
run battery sh -c "$CC -O2 -Wall -Wextra -Werror -o $B/battery_test tests/host/battery_test.c && $B/battery_test"
# the update path, against the firmware package (Felucca's tests; needs ./build.sh)
if [ -f build/fm1trax.fwsc ]; then
    run ota-entry sh -c "$CC -o $B/ota_test tests/ota_test.c && $B/ota_test build/fm1trax.fwsc"
    if [ -z "${AC79_SDK:-}" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
        run update-loader sh -c "head -c 100000 build/fm1trax.bin > $B/old_app.bin && \
            python3 tools/fm1pkg_make.py $B/old_app.bin build/loader/ota.bin $B/old.fwsc >/dev/null && \
            $CC -o $B/ldr_test tests/ldr_test.c && $B/ldr_test $B/old.fwsc build/fm1trax.fwsc"
    fi
else
    echo "  skip update-path tests (no build/fm1trax.fwsc: run ./build.sh)"
fi
run installer python3 tests/install_test.py
run host-build sh host/build_host.sh
for s in tests/scenarios/*.fd; do
    n=$(basename "$s" .fd)
    mkdir -p "build/scenarios/$n"
    run "scenario-$n" build/host/drive_host "$s" "build/scenarios/$n"
done
# the whole app again under AddressSanitizer / UBSan: a write past a buffer in the UI is silent otherwise
if echo 'int main(void){return 0;}' | $CC -fsanitize=address,undefined -x c -o $B/asan_probe - >/dev/null 2>&1; then
    run host-build-asan sh -c "HOST_CFLAGS='-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-sanitize=shift' \
        sh host/build_host.sh && mv build/host/drive_host $B/drive_host_asan && sh host/build_host.sh"
    # shellcheck disable=SC2086
    run fuzz-asan sh -c "$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-sanitize=shift \
        -ffp-contract=off -DOM_HOST -Ibuild/gen -o $B/fuzz_asan tests/host/fuzz_test.c $TRK $DSP -lm && $B/fuzz_asan 60 5000"
    for s in tests/scenarios/*.fd; do
        n=$(basename "$s" .fd)
        mkdir -p "build/scenarios-asan/$n"
        run "scenario-$n-asan" "$B/drive_host_asan" "$s" "build/scenarios-asan/$n"
    done
    command -v node >/dev/null 2>&1 && run ui-fuzz-asan env DRIVE_HOST="$B/drive_host_asan" node tests/ui_fuzz_test.mjs 40 5000
fi
# the web pages: the engine as WebAssembly (needs clang with the wasm32 target), the installer and the
# editor's link, under Node
if command -v node >/dev/null 2>&1; then
    if echo 'int f(void){return 0;}' | ${WASM_CC:-clang} --target=wasm32 -nostdlib -Wl,--no-entry -x c -o $B/wasm_probe.wasm - >/dev/null 2>&1; then
        run web-build sh host/build_web.sh
    else
        echo "  note no clang for wasm32: web/fm1trax.wasm as committed"
    fi
    run web-install node tests/web_install_test.mjs
    [ -f web/fm1trax.wasm ] && run web-editor node tests/web_editor_test.mjs
    [ -f web/fm1trax.wasm ] && run web-fuzz node tests/web_fuzz_test.mjs ${WEB_FUZZ_SEEDS:-60}
    # the whole app, random sessions on the panel and over MIDI: no hanging notes
    run ui-fuzz node tests/ui_fuzz_test.mjs ${UI_FUZZ_SEEDS:-150}
else
    echo "  skip web tests (no node)"
fi
# the card on The Firmware Hub (fmhub.json and its picture)
run fmhub python3 tools/fmhub_check.py
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
