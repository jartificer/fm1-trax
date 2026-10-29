#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# FM1-DRIVE host tests. The YM2612 is compared with Nuked-OPN2 (LGPL-2.1, fetched into build/ref at a
# fixed commit, used by the tests only, never built into the firmware); without it (offline, or
# NO_NUKED=1) the comparison is skipped and the rest still runs.
#   tests/run_tests.sh [WAVDIR]
set -e
cd "$(dirname "$0")/.."
B=build/host
mkdir -p "$B" build/ref ${1:+"$1"}
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
        echo "Nuked-OPN2 not available: the reference comparison is skipped"
    fi
fi
DSP="firmware/src/dsp/opn2.c firmware/src/dsp/psg.c firmware/src/dsp/mdsnd.c"
# the FM-1 has no fused multiply-add; the host build must compute the same samples
CFLAGS="-O2 -Wall -Wextra -ffp-contract=off"
# shellcheck disable=SC2086
cc $CFLAGS -o "$B/engine_test" tests/host/engine_test.c $DSP $REF -lm
"$B/engine_test" ${1:+"$1"}
echo "all host tests passed"
