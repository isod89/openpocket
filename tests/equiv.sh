#!/bin/sh
# OpenPocket: is this tree's engine bit-identical to commit REF's? (tests/engine/equiv.c: 852 runs hashed)
#   tests/equiv.sh REF        e.g. tests/equiv.sh origin/openpocket-v40-ram-dsp
# Both engines are built as the pedal builds them: -fno-builtin (the pedal has no square-root instruction: px_math.c's
# sqrtf) and -ffp-contract=off (it does not fuse a multiply with an add), with firmware/px_math.c as the libm. A PC
# compiler left to itself does both, and its fused multiply-adds follow the shape of the code: two engines that are
# identical on the pedal can then differ on the PC in the last bit.
# EQUIV_CFLAGS: more flags for both builds, e.g. the Bluetooth audio image's engine with its own types
#   EQUIV_CFLAGS="-DPX_OWN_FX=1 -DPX_RUNS=0 -DPXN_LEAN=1 -DPX_LOOP_GUARD=1" tests/equiv.sh HEAD~1 (equiv.c leaves those types out)
# Needs git and a C compiler; writes to build/equiv. Exit 0: identical.
set -e
cd "$(dirname "$0")/.."
REF=${1:?usage: tests/equiv.sh REF}
CC="${CC:-cc}"
B=build/equiv
rm -rf "$B/ref"
mkdir -p "$B/ref"
git archive "$REF" engine firmware/px_math.c | tar -x -C "$B/ref"
$CC -O2 -w -fno-builtin -ffp-contract=off $EQUIV_CFLAGS -I"$B/ref/engine" -o "$B/ref-run" tests/engine/equiv.c "$B/ref/firmware/px_math.c" -lm
$CC -O2 -w -fno-builtin -ffp-contract=off $EQUIV_CFLAGS -Iengine -o "$B/new-run" tests/engine/equiv.c firmware/px_math.c -lm
"$B/ref-run" > "$B/ref.txt"
"$B/new-run" > "$B/new.txt"
if cmp -s "$B/ref.txt" "$B/new.txt"; then echo "equiv: identical to $REF, $(tail -1 "$B/new.txt")"; exit 0; fi
echo "equiv: DIFFERS from $REF"
diff "$B/ref.txt" "$B/new.txt" | head -8
exit 1
