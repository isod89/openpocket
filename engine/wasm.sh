#!/bin/sh
# The engine as WebAssembly, for the simulated pedal's sound: tools/sim/px_engine.wasm and, next to it, the SHA-256 of the
# sources it was built from (tests/test_audio.mjs fails when they differ). In the container: tools/op.mjs wasm.
set -e
cd "$(dirname "$0")/.."
SRC="engine/px_wasm.c engine/px_dsp.c firmware/px_math.c"
O=build/wasm; mkdir -p "$O"
for f in $SRC; do
  clang --target=wasm32 -O2 -fno-builtin -ffp-contract=off -nostdlibinc -Iengine -Ifirmware/inc -Wall -Wno-unused-function -c "$f" -o "$O/$(basename "$f" .c).o"
done
wasm-ld --no-entry --strip-all -z stack-size=65536 "$O"/*.o -o tools/sim/px_engine.wasm
cat $SRC engine/px_dsp.h engine/px_trim.h | sha256sum | cut -d' ' -f1 > tools/sim/px_engine.wasm.sources
chmod 644 tools/sim/px_engine.wasm
ls -l tools/sim/px_engine.wasm
