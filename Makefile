# OpenPocket: the same commands on macOS and Linux. Every target calls tools/op.mjs (Node 18+), which holds all the
# Docker command lines; Windows calls it from the .bat files. See docs/BUILDING.md.
#   make doctor     check the prerequisites (Node, Docker, the image)
#   make build      firmware/openpocket.bin, the update loader and docs/firmware/*.fwsc + manifest.json
#   make ble        the Bluetooth MIDI test image: firmware/openpocket-ble.bin, build/ble/*.fwsc (docs/BLE-TEST.md)
#   make test       the host tests (tests/run_tests.sh)
#   make package    build + build/SHA256SUMS of the outputs
#   make package-test   the MIDI test channel package (docs/firmware/midi-test/, never the installer default)
#   make clean      remove every build output (needs no Docker)
#   make shell      a shell in the build container
#   make image      (re)build the container image
#   make serve      the installer and editor on http://localhost:8765/ (offline install)
#   make serve-ble  the same, with the Bluetooth test packages (build/ble) to install; builds them first if missing
#   make flash ARGS="--dry-run"   write a built package to a connected pedal (ARGS="--confirm-flash" to really write)
#   make log ARGS="--follow"   read a connected pedal's state over USB MIDI, with the fields explained
#   make render IN=guitar.wav [OUT=out.wav] [PRESET=1]   a WAV through the engine
#   make appimage [DUMP=fm1-1.bin]   fm1-300.bin for the SD-card recovery
# `make NATIVE=1 build` skips Docker (toolchain installed with firmware/setup.sh).
.PHONY: doctor build ble test package package-test clean shell image serve serve-ble render appimage flash log
doctor build ble test package package-test clean shell image serve:
	@node tools/op.mjs $@
serve-ble:
	@node tools/op.mjs serve ble
render:
	@node tools/op.mjs render "$(IN)" "$(OUT)" "$(PRESET)"
appimage:
	@node tools/op.mjs appimage "$(DUMP)"
flash log:
	@node tools/op.mjs $@ $(ARGS)
