# OpenPocket: the same commands on macOS and Linux. Every target calls tools/op.mjs (Node 18+), which holds all the
# Docker command lines; Windows calls it from the .bat files. See docs/BUILDING.md.
#   make doctor     check the prerequisites (Node, Docker, the image)
#   make build      firmware/openpocket.bin, the update loader and docs/firmware/*.fwsc + manifest.json
#   make ble        the Bluetooth MIDI test image: firmware/openpocket-ble.bin, build/ble/*.fwsc (docs/BLE-TEST.md)
#   make app APP=starter   another app on the base firmware (firmware/apps/starter): build/apps/starter/ (docs/ALT-FIRMWARE.md)
#   make all        every product (OpenPocket with its release pin, each app of firmware/apps), then the tests on what was built
#   make test       the host tests (tests/run_tests.sh)
#   make package    build + build/SHA256SUMS of the outputs
#   make package-test   the MIDI test channel package (docs/firmware/midi-test/, never the installer default)
#   make clean      remove every build output (needs no Docker)
#   make shell      a shell in the build container
#   make image      (re)build the container image
#   make serve      the installer and editor on http://localhost:8765/ (offline install)
#   make serve-ble  the same, with the Bluetooth test packages (build/ble) to install; builds them first if missing
#   make serve-app APP=starter   the same, with that alternative firmware offered beside OpenPocket; builds it first if missing
#   make flash ARGS="--dry-run"   write a built package to a connected pedal (ARGS="--confirm-flash" to really write)
#   make flash-rescue ARGS="--confirm-flash"   wait for a pedal that restarts in a loop and install (docs/BUILDING.md)
#   make log ARGS="--follow"   read a connected pedal's state over USB MIDI, with the fields explained
#   make sim [ARGS="--port 9000 --reset"]   the hosted simulated pedal on http://127.0.0.1:8766/ (docs/SIMULATOR.md)
#   make sim ARGS="--app starter"   the same for an app of firmware/apps, from its own C files
#   make wasm       the engine as WebAssembly for the simulator's sound (tools/sim/px_engine.wasm, committed)
#   make render IN=guitar.wav [OUT=out.wav] [PRESET=1]   a WAV through the engine
#   make appimage [DUMP=fm1-1.bin]   fm1-300.bin for the SD-card recovery
#   make rescue [ARGS=..]   restore a looping BlackBox from its UBOOT state, read-only (needs sudo and libusb: docs/RESCUE.md)
#   make rescue-write [ARGS=..]   the same, writing the official firmware back (run `make rescue` first)
#   make rescue-check   the UBOOT rescue tool (tools/uboot_rescue.py) against a simulated pedal; needs only Python 3
# `make NATIVE=1 build` skips Docker (toolchain installed with firmware/setup.sh).
.PHONY: doctor build ble app all test package package-test clean shell image serve serve-ble serve-app wasm render appimage flash flash-rescue log rescue rescue-write rescue-check sim
doctor build ble all test package package-test clean shell image serve wasm rescue-check:
	@node tools/op.mjs $@
app:
	@node tools/op.mjs app "$(APP)"
serve-ble:
	@node tools/op.mjs serve ble
serve-app:
	@node tools/op.mjs serve app "$(APP)"
render:
	@node tools/op.mjs render "$(IN)" "$(OUT)" "$(PRESET)"
appimage:
	@node tools/op.mjs appimage "$(DUMP)"
flash log:
	@node tools/op.mjs $@ $(ARGS)
rescue:
	@node tools/op.mjs rescue $(ARGS)
rescue-write:
	@node tools/op.mjs rescue --write $(ARGS)
flash-rescue:
	@node tools/op.mjs flash --rescue $(ARGS)
sim:
	@node tools/op.mjs sim $(ARGS)
