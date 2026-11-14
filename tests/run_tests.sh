#!/bin/sh
# OpenPocket host tests (Linux / macOS, or in the container: tools/op.mjs test; no pedal, no JieLi toolchain needed):
#   tests/run_tests.sh [PocketX_016.fwsc]
#  - engine: every block x model x extreme knob, signal / silence / overload, with the pedal's own libm (px_math.c):
#    no NaN; long runs (3 s pluck + 8 s silence): nothing runs away (only delays at FDBK 100 hold on, by design)
#  - the two-core pipeline (PRE / NAM / POST stages) gives the plain engine's output one block later
#  - NAM hand routines: their start-up check and dispatch with C stand-ins under the sanitizers; post5's assembly text
#    against the C post through a model of its instructions (tests/test_post5.py)
#  - (not run here, it needs the git history: tests/equiv.sh REF, this tree's engine against commit REF's, bit for bit)
#  - changes without clicks (v38): knob changes glide to exactly where a full set goes, blocks on / off fade, fewer clicks
#  - USER IR (v38): the partitioned FFT convolution against the direct FIR, slots, names
#  - wet lanes (v39): the delay through its lane as inline bit for bit, the reverb's tail lag frames later, the pipeline
#  - NAM: engine/px_nam.c against tools/nam_a2.py with every factory model (needs numpy)
#  - MIDI: the musical-MIDI code (firmware/px_midi.h) on a PC: decoder, CC map, messages sent, settings byte, clock
#  - preset store: save / reload / power cut at every step of a save (firmware/px_store.h on a simulated flash)
#  - the audio callbacks' order (v42, firmware/px_aorder.h): the I2S driver calls the output's callback before the
#    input's when its interrupt enters late; every output block must still be the sound of that interrupt's guitar block
#  - tuner (firmware/px_tuner.h): bass and guitar notes, sine and plucked, in tune and off it, read every 100 ms: the
#    note and its octave, within 2 cents from a five-string bass's low B (31 Hz) to the high E string
#  - base audio (firmware/base/audio.h, audio_plain.h): the audio stages every app shares, for a mono and for a stereo
#    app: the guitar through an app's sound to the output channels, MASTER, the mutes, the fades, the soft knee, the
#    load's bypass
#  - base controls (firmware/base/input.h): each button's pins and the keypad's voltages to its event, the debounce, a
#    hold and its release, two buttons together, the encoder's detents, the MASTER knob
#  - base store (firmware/base/store.h): save / reload, a power cut at every step of a save, nothing written outside its
#    ten sectors, no sector ending like an update record whatever the app saves
#  - USB audio (v42): the USB device code and the audio glue (bsp/chip/usb.c, firmware/px_uaudio.h) against a model of
#    the controller and a computer: with the setting off the descriptors are the earlier device's byte for byte; with
#    it on, the descriptors, the requests, 10 minutes of each stream with the clocks apart (no gap, no repeat)
#    and the MIDI input ring with an app's reader of it (firmware/base/midi.h): unread, it blocks the installer's SysEx
#  - LED map + lights driver (led_probe.h, px_leds.h): PC7 enable after the first byte, the 595 timing, the table
#  - button lights (px_lights.h): each block lights its button, FX for its group, HOME blinks in the tuner
#  - packages: tools/pxpkg.py rebuilds the official PocketX_016 app area byte for byte (needs the official file)
#  - installer: docs/pxota.js + docs/pxpkg.js against a simulated pedal running the real update code (ota.c, ldr_core.c)
#  - flasher: tools/flash.mjs (node tools/op.mjs flash) against the same simulated pedal: update, dry run, wrong model, damaged package, rescue of a pedal that restarts in a loop
#  - products: an alternative firmware (an app on the base firmware) on the same simulated pedal: what it says it is, the
#    flasher's --app guard, the install, the way back to OpenPocket (also by rescue), what the flash keeps through both.
#    With OP_BUILT=1 (node tools/op.mjs all) also with the packages the build made: each product's image byte for byte
#  - rescue: tools/uboot_rescue.py (protocol, guards, resume) against a simulated UBOOT pedal, tests/uboot_sim.py
#  - hosted simulator: tools/sim.mjs driven over HTTP (controls, held buttons, screen, flash file, SysEx, the Web MIDI shim)
#  - an app in the hosted simulator: firmware/apps/starter's own C files with tests/host as the chip, over the same HTTP
#    interface: its controls, its sound at the two outputs, MIDI, its store, safe mode, the boot guard
#  - pedal skin: its input mapping (tools/sim/pedal-input.js)
#  - sound: the WebAssembly engine on known signals, against the native build; the bundled loops against their manifest
#  - Chocolate Plus files: tools/chocolate/fcp.py against a made-up export
#  - editor: docs/pxedit.js against the simulated pedal (tests/edsim.c), PING's figures, and the editor's own logic
#    (docs/pxmidi.js: the Control Change map against docs/MIDI.md, learned controls; docs/pxlib.js: the model library)
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
# v40.5: CPU0's waits for a second core that never moves end after the limit (a stuck CPU1: LATE, not the watchdog)
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Iengine tests/engine/lp_stuck.c firmware/px_math.c -lm -o build/lp_stuck && build/lp_stuck | tail -1
# v40.2: the NAM hand routines' start-up check, timing and dispatch, with C stand-ins under the sanitizers (a fault there
# would be a boot loop on the pedal)
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DPXN_HAND=1 -Iengine tests/engine/hand_test.c firmware/px_math.c -lm -o build/hand_test && build/hand_test | tail -1
$CC -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -DPXN_HAND=1 -DPXN_LEAN=1 -Iengine tests/engine/hand_test.c firmware/px_math.c -lm -o build/hand_test_lean && build/hand_test_lean | tail -1   # (the Bluetooth image's)
# v40.5: post5's assembly text itself, run by a model of its instructions against the C post (tests/test_post5.py), and
# the same with one offset changed, which must fail
python3 tests/test_post5.py
if python3 tests/test_post5.py "r6 = [sp+40]" "r6 = [sp+44]" > /dev/null 2>&1; then echo "test_post5 accepts a wrong routine"; exit 1; fi
$CC -O2 -Iengine -Ifirmware tests/engine/change_test.c firmware/px_math.c -lm -o build/change_test && build/change_test | tail -1
$CC -O2 -Iengine -Ifirmware tests/engine/ir_test.c firmware/px_math.c -lm -o build/ir_test && build/ir_test | tail -1
$CC -O2 -Iengine tests/engine/wet_test.c engine/px_dsp.c firmware/px_math.c -lm -o build/wet_test && build/wet_test | tail -1
echo "== NAM (fixed point against the float reference, every factory model)"
$CC -O2 -DPXN_EMUVEC -Iengine tests/engine/nam_test.c engine/px_nam.c -o build/nam_test
if python3 -c "import numpy" 2>/dev/null; then mkdir -p build/nam; python3 tests/test_nam.py build/nam_test > build/nam/log.txt || { cat build/nam/log.txt; exit 1; }; tail -1 build/nam/log.txt; else echo "(numpy not installed: NAM comparison skipped)"; fi
echo "== preset store"
$CC -O1 -Iengine tests/test_store.c engine/px_dsp.c firmware/px_math.c -lm -o build/test_store && build/test_store | tail -2
echo "== audio callbacks' order (the output block is its interrupt's, whichever callback the driver calls first)"
$CC -O1 -Wall -Ifirmware tests/test_aorder.c -o build/test_aorder && build/test_aorder | tail -1
echo "== tuner (bass and guitar notes through the pedal's capture and estimate)"
$CC -O1 -Wall -Ifirmware tests/test_tuner.c -lm -o build/test_tuner && build/test_tuner | tail -1
echo "== base audio (the stages every app shares, under the plain schedule: firmware/base/audio.h, audio_plain.h)"
# (a base test's last line is its result; its whole output is shown when it fails)
base_test() { t="$1"; shift; "$@" -o "build/$t"; "build/$t" > "build/$t.log" || { cat "build/$t.log"; exit 1; }; tail -1 "build/$t.log"; }
BAF="-O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-int-to-pointer-cast -Ifirmware"
base_test test_base_audio $CC $BAF tests/test_base_audio.c firmware/px_math.c -lm
base_test test_base_audio2 $CC $BAF -DA_CH=2 tests/test_base_audio.c firmware/px_math.c -lm   # an app with a stereo sound
echo "== base controls (firmware/base/input.h over base/controls.h: the pins and the ADC to an app's events)"
base_test test_base_input $CC -O1 -Wall -Wno-unused-function -Wno-unused-variable -Itests/host -Ifirmware tests/test_base_input.c
echo "== base store (firmware/base/store.h: an app's own data in flash, kept by every update loader)"
base_test test_base_store $CC -O1 -Wall -Wno-unused-function -Ifirmware tests/test_base_store.c
echo "== USB audio (the device code and the audio glue against a model of the controller and a computer)"
UAF="-O1 -Wall -Wno-unused-function -Wno-int-to-pointer-cast -Iengine -Ifirmware -Ifirmware/bsp/chip"
$CC $UAF tests/test_usb_audio.c -lm -o build/test_usb_audio
$CC $UAF -DT_REF tests/test_usb_audio.c -lm -o build/test_usb_audio_ref      # the USB device as it was before v42
build/test_usb_audio desc > build/usb_desc_off.txt
build/test_usb_audio_ref > build/usb_desc_ref.txt
if cmp -s build/usb_desc_off.txt build/usb_desc_ref.txt; then echo "USB AUDIO off: the descriptors are the earlier device's, byte for byte"
else echo "USB AUDIO off: the descriptors differ from the earlier device's"; diff build/usb_desc_ref.txt build/usb_desc_off.txt; exit 1; fi
build/test_usb_audio > build/usb_audio.log || { cat build/usb_audio.log; exit 1; }
tail -1 build/usb_audio.log
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
echo "== packages of two products (tools/pxpkg.py: an app's product descriptor, its packages' names, the manifest)"
python3 tests/test_pxpkg_app.py > build/pxpkg_app.log || { cat build/pxpkg_app.log; exit 1; }; tail -1 build/pxpkg_app.log
echo "== installer"
node tests/test_installer.mjs "$1"
echo "== flasher (tools/flash.mjs)"
node tests/test_flash.mjs
echo "== two products from one base (an alternative firmware installed, told from OpenPocket, and OpenPocket put back)"
node tests/test_products.mjs ${OP_BUILT:+--built}
echo "== build entry point (tools/op.mjs)"
node tests/test_op.mjs
echo "== release pin (tests/release_pin.sh: the same sums pass, one changed byte's sum fails)"
sh tests/release_pin.sh tests/release.sha256 > /dev/null || { echo "release pin: refuses its own sums"; exit 1; }
sed '1s/^./0/;1s/^00/01/' tests/release.sha256 > build/release_pin_wrong.sums
if sh tests/release_pin.sh build/release_pin_wrong.sums > /dev/null 2>&1; then echo "release pin: accepts a changed image"; exit 1; fi
if sh tests/release_pin.sh build/no-such.sums > /dev/null 2>&1; then echo "release pin: accepts a missing build"; exit 1; fi
echo "release pin: ok"
echo "== rescue tool (tools/uboot_rescue.py on a simulated UBOOT pedal)"
python3 tests/test_uboot_rescue.py $1
echo "== hosted simulator (tools/sim.mjs)"
node tests/test_sim.mjs
echo "== an app on the base firmware in the simulator (firmware/apps/starter, from its own C files: tools/sim.mjs --app)"
node tests/test_app_sim.mjs
echo "== Bluetooth test image sources (third_party/jieli-ble)"
node tests/test_ble_sources.mjs
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
node tests/test_editor_logic.mjs
echo "== log channel (tools/op.mjs log)"
$CC -O1 -Wall -Wno-unused-function -DOP_BLE_TEST=1 -o build/edsim_diag tests/edsim.c engine/px_dsp.c firmware/px_math.c -lm
node tests/test_log.mjs
if node -e "require('playwright')" 2>/dev/null; then echo "== page"; node tests/test_page.mjs build/page; echo "== editor page"; node tests/test_editor_page.mjs build/editor; echo "== pedal skin"; node tests/test_pedal_page.mjs build/pedal; echo "== pedal sound"; node tests/test_audio_page.mjs build/audio; else echo "(Playwright not installed: page tests skipped)"; fi
