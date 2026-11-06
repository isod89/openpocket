# OpenPocket: the same commands on macOS and Linux. Every target calls tools/op.mjs (Node 18+), which holds all the
# Docker command lines; Windows calls it from the .bat files. See docs/BUILDING.md.
#   make doctor     check the prerequisites (Node, Docker, the image)
#   make build      firmware/openpocket.bin, the update loader and docs/firmware/*.fwsc + manifest.json
#   make ble        the advertise-only Bluetooth test image: firmware/openpocket-ble.bin, build/ble/*.fwsc (docs/BLE-TEST.md)
#   make test       the host tests (tests/run_tests.sh)
#   make package    build + build/SHA256SUMS of the outputs
#   make clean      remove every build output (needs no Docker)
#   make shell      a shell in the build container
#   make image      (re)build the container image
#   make serve      the installer and editor on http://localhost:8765/ (offline install)
#   make serve-ble  the same, with the Bluetooth test packages (build/ble) to install; builds them first if missing
#   make flash ARGS="--dry-run"   write a built package to a connected pedal (ARGS="--confirm-flash" to really write)
#   make flash-rescue ARGS="--confirm-flash"   wait for a pedal that restarts in a loop and install (docs/BUILDING.md)
#   make log ARGS="--follow"   read a connected pedal's state over USB MIDI, with the fields explained
#   make sim [ARGS="--port 9000 --reset"]   the hosted simulated pedal on http://127.0.0.1:8766/ (docs/SIMULATOR.md)
#   make wasm       the engine as WebAssembly for the simulator's sound (tools/sim/px_engine.wasm, committed)
#   make render IN=guitar.wav [OUT=out.wav] [PRESET=1]   a WAV through the engine
#   make appimage [DUMP=fm1-1.bin]   fm1-300.bin for the SD-card recovery
# `make NATIVE=1 build` skips Docker (toolchain installed with firmware/setup.sh).
.PHONY: doctor build ble test package clean shell image serve serve-ble wasm render appimage flash flash-rescue log sim
doctor build ble test package clean shell image serve wasm:
	@node tools/op.mjs $@
serve-ble:
	@node tools/op.mjs serve ble
render:
	@node tools/op.mjs render "$(IN)" "$(OUT)" "$(PRESET)"
appimage:
	@node tools/op.mjs appimage "$(DUMP)"
flash log:
	@node tools/op.mjs $@ $(ARGS)
flash-rescue:
	@node tools/op.mjs flash --rescue $(ARGS)
sim:
	@node tools/op.mjs sim $(ARGS)
