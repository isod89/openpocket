#!/bin/sh
# OpenPocket host tests (Linux / macOS, or in the container: tools/op.mjs test; no pedal, no JieLi toolchain needed):
#   tests/run_tests.sh [PocketX_016.fwsc]
#  - engine: every block x model x extreme knob, signal / silence / overload, with the pedal's own libm (px_math.c):
#    no NaN; long runs (3 s pluck + 8 s silence): nothing runs away (only delays at FDBK 100 hold on, by design)
#  - the two-core pipeline (PRE / NAM / POST stages) gives the plain engine's output one block later
#  - changes without clicks (v38): knob changes glide to exactly where a full set goes, blocks on / off fade, fewer clicks
#  - USER IR (v38): the partitioned FFT convolution against the direct FIR, slots, names
#  - wet lanes (v39): the delay through its lane as inline bit for bit, the reverb's tail lag frames later, the pipeline
#  - NAM: engine/px_nam.c against tools/nam_a2.py with every factory model (needs numpy)
#  - MIDI: the musical-MIDI code (firmware/px_midi.h) on a PC: decoder, CC map, messages sent, settings byte, clock
#  - preset store: save / reload / power cut at every step of a save (firmware/px_store.h on a simulated flash)
#  - LED map + lights driver (led_probe.h, px_leds.h): PC7 enable after the first byte, the 595 timing, the table
#  - button lights (px_lights.h): each block lights its button, FX for its group, HOME blinks in the tuner
#  - packages: tools/pxpkg.py rebuilds the official PocketX_016 app area byte for byte (needs the official file)
#  - installer: docs/pxota.js + docs/pxpkg.js against a simulated pedal running the real update code (ota.c, ldr_core.c)
#  - flasher: tools/flash.mjs (node tools/op.mjs flash) against the same simulated pedal: update, dry run, wrong model, damaged package, rescue of a pedal that restarts in a loop
#  - rescue: tools/uboot_rescue.py (protocol, guards, resume) against a simulated UBOOT pedal, tests/uboot_sim.py
#  - hosted simulator: tools/sim.mjs driven over HTTP (controls, held buttons, screen, flash file, SysEx, the Web MIDI shim)
#  - pedal skin: its input mapping (tools/sim/pedal-input.js)
#  - sound: the WebAssembly engine on known signals, against the native build; the bundled loops against their manifest
#  - Chocolate Plus files: tools/chocolate/fcp.py against a made-up export
#  - Bluetooth audio (docs/BT-AUDIO-TEST.md): the ring and its rate (tests/test_bt_ring.c), the radio pools' allocator
#    (test_bt_lbuf.c), the compiler runtime it adds (test_bt_rt.c), SBC through Android's encoder and decoder (test_bt_sbc.c), the third-party sources (test_bt_sources.mjs)
#  - pages: docs/index.html, docs/editor.html and the pedal skin in headless Chromium (needs Playwright: npm i playwright
#    && npx playwright install chromium)
set -e
cd "$(dirname "$0")/.."
mkdir -p build
CC="${CC:-cc}"
echo "== engine"
$CC -O2 -Iengine tests/engine/sweep2.c engine/px_dsp.c firmware/px_math.c -lm -o build/sweep && build/sweep | tail -1
$CC -O2 -Iengine tests/engine/long.c engine/px_dsp.c firmware/px_math.c -lm -o build/long && build/long | tail -1
$CC -O2 -Iengine tests/engine/pipe_test.c engine/px_dsp.c firmware/px_math.c -lm -o build/pipe_test && build/pipe_test | tail -1
# v40.2: the network split between the cores by layers, against one core and through the pipeline
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DPXN_EMUVEC -Iengine tests/engine/lp_test.c firmware/px_math.c -lm -o build/lp_test && build/lp_test | tail -1
# v40.2: the NAM hand routines' start-up check, timing and dispatch, with C stand-ins under the sanitizers (a fault there
# would be a boot loop on the pedal)
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DPXN_HAND=1 -Iengine tests/engine/hand_test.c firmware/px_math.c -lm -o build/hand_test && build/hand_test | tail -1
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DPXN_HAND=1 -DPXN_LEAN=1 -Iengine tests/engine/hand_test.c firmware/px_math.c -lm -o build/hand_test_lean && build/hand_test_lean | tail -1   # (the Bluetooth image's)
$CC -O2 -Iengine -Ifirmware tests/engine/change_test.c firmware/px_math.c -lm -o build/change_test && build/change_test | tail -1
$CC -O2 -Iengine -Ifirmware tests/engine/ir_test.c firmware/px_math.c -lm -o build/ir_test && build/ir_test | tail -1
$CC -O2 -Iengine tests/engine/wet_test.c engine/px_dsp.c firmware/px_math.c -lm -o build/wet_test && build/wet_test | tail -1
echo "== NAM (fixed point against the float reference, every factory model)"
$CC -O2 -DPXN_EMUVEC -Iengine tests/engine/nam_test.c engine/px_nam.c -o build/nam_test
if python3 -c "import numpy" 2>/dev/null; then mkdir -p build/nam; python3 tests/test_nam.py build/nam_test > build/nam/log.txt || { cat build/nam/log.txt; exit 1; }; tail -1 build/nam/log.txt; else echo "(numpy not installed: NAM comparison skipped)"; fi
echo "== preset store"
$CC -O1 -Iengine tests/test_store.c engine/px_dsp.c firmware/px_math.c -lm -o build/test_store && build/test_store | tail -2
echo "== screen (every page fully drawn)"
mkdir -p build/ui && $CC -O1 -Wall -Wno-unused-function -Iengine -Ifirmware -o build/uisim tests/uisim.c engine/px_dsp.c firmware/px_math.c -lm
build/uisim build/ui > build/ui/log.txt
if grep -q "never drawn" build/ui/log.txt; then grep "never drawn" build/ui/log.txt; echo "screen: FAIL"; exit 1; else echo "screen: $(wc -l < build/ui/log.txt) pages ok (build/ui/*.ppm)"; fi
echo "== MIDI (firmware/px_midi.h: decoder, Control Change map, what the pedal sends, settings, clock)"
for t in midisim midiio; do
  $CC -O1 -Wall -Wno-unused-function -Iengine -Ifirmware -o build/$t tests/$t.c engine/px_dsp.c firmware/px_math.c -lm
  build/$t > build/$t.log || { grep FAIL build/$t.log; exit 1; }
  tail -1 build/$t.log
done
echo "== LED map and the lights driver (LED_PROBE build, issue #8)"
mkdir -p build/lp && $CC -O1 -Wall -Wno-unused-function -Ifirmware -o build/lpsim tests/lpsim.c && build/lpsim build/lp
echo "== button lights in the normal firmware (px_lights.h, issue #8)"
$CC -O1 -Wall -Wno-unused-function -Iengine -Ifirmware -o build/lightsim tests/lightsim.c
build/lightsim > build/lightsim.log || { cat build/lightsim.log; exit 1; }; tail -1 build/lightsim.log
echo "== simulated pedal"
$CC -O1 -Wall -Wno-unused-function -o build/devsim tests/devsim.c
if [ -n "$1" ]; then echo "== packages"; python3 tests/test_pxpkg.py "$1"; fi
echo "== installer"
node tests/test_installer.mjs "$1"
echo "== flasher (tools/flash.mjs)"
node tests/test_flash.mjs
echo "== build entry point (tools/op.mjs)"
node tests/test_op.mjs
echo "== rescue tool (tools/uboot_rescue.py on a simulated UBOOT pedal)"
python3 tests/test_uboot_rescue.py $1
echo "== hosted simulator (tools/sim.mjs)"
node tests/test_sim.mjs
echo "== Bluetooth test image sources (third_party/jieli-ble)"
node tests/test_ble_sources.mjs
echo "== Bluetooth audio image: its sources (third_party/jieli-bt, third_party/sbc-android), the ring, the radio pools, SBC"
node tests/test_bt_sources.mjs
$CC -O1 -Wall -Wno-unused-function -o build/test_bt_ring tests/test_bt_ring.c -lm && build/test_bt_ring
$CC -O1 -Wall -Wno-unused-function -o build/test_bt_lbuf tests/test_bt_lbuf.c && build/test_bt_lbuf
$CC -O2 -Wall -Wno-unused-function -o build/test_bt_rt tests/test_bt_rt.c -lm && build/test_bt_rt
SBC=third_party/sbc-android
$CC -O1 -w -I$SBC/decoder/include -I$SBC/encoder/include -Ifirmware/bt -o build/test_bt_sbc tests/test_bt_sbc.c firmware/bt/bt_sbc.c \
    $SBC/decoder/srce/*.c $SBC/encoder/srce/*.c -lm && build/test_bt_sbc
echo "== Bluetooth test image status codes"
$CC -O1 -Wall -Wno-unused-function -o build/ble_status tests/test_ble_status.c && build/ble_status
$CC -O1 -Wall -Wno-unused-function -o build/ble_vm tests/test_ble_vm.c && build/ble_vm
echo "== Chocolate Plus files (tools/chocolate/fcp.py)"
python3 tests/test_fcp.py
echo "== pedal skin input"
node tests/test_pedal_input.mjs
echo "== simulated sound"
node tests/test_audio.mjs
echo "== editor"
$CC -O1 -Wall -Wno-unused-function -o build/edsim tests/edsim.c engine/px_dsp.c firmware/px_math.c -lm
node tests/test_editor.mjs
node tests/test_ping.mjs
echo "== log channel (tools/op.mjs log)"
$CC -O1 -Wall -Wno-unused-function -DOP_BLE_TEST=1 -o build/edsim_diag tests/edsim.c engine/px_dsp.c firmware/px_math.c -lm
node tests/test_log.mjs
if node -e "require('playwright')" 2>/dev/null; then echo "== page"; node tests/test_page.mjs build/page; echo "== editor page"; node tests/test_editor_page.mjs build/editor; echo "== pedal skin"; node tests/test_pedal_page.mjs build/pedal; echo "== pedal sound"; node tests/test_audio_page.mjs build/audio; else echo "(Playwright not installed: page tests skipped)"; fi
