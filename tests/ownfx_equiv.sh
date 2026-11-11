#!/bin/sh
# OpenPocket: the Bluetooth audio image's engine (engine/px_dsp.h PX_OWN_FX) gives every existing model's sound bit for bit
# (tests/engine/ownfx_equiv.c: 5,460 runs hashed). This tree's engine is built four times, as the pedal builds it
# (-fno-builtin -ffp-contract=off, firmware/px_math.c as the libm: see tests/equiv.sh), without and with -DPX_OWN_FX=1, with
# the Bluetooth image's flags (firmware/build.sh) and with the host's defaults, and the lists are compared. No git needed.
#   tests/ownfx_equiv.sh [SECTIONS]      (default ABCDFGHKL; about a minute, the four runs at once). Exit 0: identical.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
B=build/ownfx; mkdir -p "$B"
BT="-DPXN_LEAN=1 -DPXN_HOT_RAM=0 -DPX_RUNS=0 -DPX_LOOP_GUARD=1"
for cfg in bt host; do
  F=""; [ "$cfg" = bt ] && F="$BT"
  $CC -O2 -w -fno-builtin -ffp-contract=off $F -Iengine -o "$B/off-$cfg" tests/engine/ownfx_equiv.c firmware/px_math.c -lm
  $CC -O2 -w -fno-builtin -ffp-contract=off $F -DPX_OWN_FX=1 -Iengine -o "$B/on-$cfg" tests/engine/ownfx_equiv.c firmware/px_math.c -lm
done
st=0
for r in off-bt on-bt off-host on-host; do ("$B/$r" $1 > "$B/$r.txt" || echo "$r: a check failed" > "$B/$r.fail") & done
wait
for cfg in bt host; do
  if [ -f "$B/off-$cfg.fail" ] || [ -f "$B/on-$cfg.fail" ]; then
    cat "$B/off-$cfg.fail" "$B/on-$cfg.fail" 2>/dev/null; grep -h -m5 'FLAGS\|DV row' "$B/off-$cfg.txt" "$B/on-$cfg.txt"; st=1
  elif ! cmp -s "$B/off-$cfg.txt" "$B/on-$cfg.txt"; then
    echo "ownfx ($cfg flags): DIFFERS"; diff "$B/off-$cfg.txt" "$B/on-$cfg.txt" | head -8; st=1
  else echo "ownfx ($cfg flags): PX_OWN_FX identical, $(tail -1 "$B/on-$cfg.txt")"; fi
done
rm -f "$B"/*.fail
exit $st
