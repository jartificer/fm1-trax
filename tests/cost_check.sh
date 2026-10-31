#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The sound engine's time budget, in host instructions per 256-frame block (valgrind's count): a
# stand-in for the FM-1's time, which no host test can measure, that catches the engine getting
# slower. The limits sit about 10% above today's cost with clang -O2 (as the FM-1's compiler; gcc
# counts differently, so run_tests.sh builds it with clang and skips it without); the FM-1 itself shows its load in SETUP (AUDIO
# LOAD, the average and the slowest block) and says AUDIO TOO SLOW when a block comes late.
#   tests/cost_check.sh COST_TEST_BINARY
set -eu
BIN="$1"
command -v valgrind >/dev/null 2>&1 || { echo "skip: no valgrind"; exit 0; }
ir() {   # instructions for $2 blocks of case $1
    valgrind --tool=callgrind --callgrind-out-file=/dev/null "$BIN" "$1" "$2" 2>&1 |
        sed -n 's/.*refs: *//p' | tr -d ,
}
fail=0
for c in "stress 1130000" "demo 690000"; do
    set -- $c
    base=$(ir "$1" 0)
    total=$(ir "$1" 400)
    per=$(( (total - base) / 400 ))
    if [ "$per" -gt "$2" ]; then
        echo "FAIL $1: $per instructions a block (limit $2)"
        fail=1
    else
        echo "  ok $1: $per instructions a block (limit $2)"
    fi
done
exit $fail
