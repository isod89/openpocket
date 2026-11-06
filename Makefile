# OpenPocket: the same commands on macOS and Linux. Every target calls tools/op.mjs (Node 18+), which holds all the
# Docker command lines; Windows calls it from the .bat files. See docs/BUILDING.md.
#   make doctor     check the prerequisites (Node, Docker, the image)
#   make build      firmware/openpocket.bin, the update loader and docs/firmware/*.fwsc + manifest.json
#   make test       the host tests (tests/run_tests.sh)
#   make package    build + build/SHA256SUMS of the outputs
#   make package-test   the MIDI test channel package (docs/firmware/midi-test/, never the installer default)
#   make clean      remove every build output (needs no Docker)
#   make shell      a shell in the build container
#   make image      (re)build the container image
#   make serve      the installer and editor on http://localhost:8765/ (offline install)
#   make render IN=guitar.wav [OUT=out.wav] [PRESET=1]   a WAV through the engine
#   make appimage [DUMP=fm1-1.bin]   fm1-300.bin for the SD-card recovery
# `make NATIVE=1 build` skips Docker (toolchain installed with firmware/setup.sh).
.PHONY: doctor build test package package-test clean shell image serve render appimage
doctor build test package package-test clean shell image serve:
	@node tools/op.mjs $@
render:
	@node tools/op.mjs render "$(IN)" "$(OUT)" "$(PRESET)"
appimage:
	@node tools/op.mjs appimage "$(DUMP)"
