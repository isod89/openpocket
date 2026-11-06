# OpenPocket: the same commands on macOS and Linux. Every target calls tools/op.mjs (Node 18+), which holds all the
# Docker command lines; Windows calls it from the .bat files. See docs/BUILDING.md.
#   make doctor     check the prerequisites (Node, Docker, the image)
#   make build      firmware/openpocket.bin, the update loader and docs/firmware/*.fwsc + manifest.json
#   make test       the host tests (tests/run_tests.sh)
#   make package    build + build/SHA256SUMS of the outputs
#   make clean      remove every build output (needs no Docker)
#   make shell      a shell in the build container
#   make image      (re)build the container image
#   make serve      the installer and editor on http://localhost:8765/ (offline install)
#   make render IN=guitar.wav [OUT=out.wav] [PRESET=1]   a WAV through the engine
#   make appimage [DUMP=fm1-1.bin]   fm1-300.bin for the SD-card recovery
#   make rescue [ARGS=..]   restore a looping BlackBox from its UBOOT state, read-only (needs sudo and libusb: docs/RESCUE.md)
#   make rescue-write [ARGS=..]   the same, writing the official firmware back (run `make rescue` first)
#   make rescue-check   the UBOOT rescue tool (tools/uboot_rescue.py) against a simulated pedal; needs only Python 3
# `make NATIVE=1 build` skips Docker (toolchain installed with firmware/setup.sh).
.PHONY: doctor build test package clean shell image serve render appimage rescue rescue-write rescue-check
doctor build test package clean shell image serve rescue-check:
	@node tools/op.mjs $@
render:
	@node tools/op.mjs render "$(IN)" "$(OUT)" "$(PRESET)"
appimage:
	@node tools/op.mjs appimage "$(DUMP)"
rescue:
	@node tools/op.mjs rescue $(ARGS)
rescue-write:
	@node tools/op.mjs rescue --write $(ARGS)
