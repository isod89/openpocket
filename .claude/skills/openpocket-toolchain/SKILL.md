---
name: openpocket-toolchain
description: Set up and use the OpenPocket firmware toolchain (JieLi clang in a pinned linux/amd64 Docker container, driven by tools/op.mjs). Use to check prerequisites, build the firmware and installer packages, run the host tests and simulators, clean, update the pinned toolchain, or diagnose a build failure (Docker not running, slow emulation on Apple silicon, checksum mismatch).
---

# OpenPocket toolchain

The compiler is JieLi's, x86-64 Linux only. Everything runs in one container (`openpocket-build`, base image pinned by
digest, toolchain pinned by SHA-256, see `Dockerfile` and `firmware/setup.sh`). The only entry point is
`node tools/op.mjs <command>` (Node 18+); `make <command>` and the `.bat` files call it. Details: `docs/BUILDING.md`.
Run commands from the repository root.

## Set up

1. `node tools/op.mjs doctor` checks Node and Docker and says whether the image exists. Fix every `FAIL` line first
   (start Docker Desktop, or `colima start`).
2. `node tools/op.mjs image` builds the image the first time (about 1 minute; it downloads the 25 MB toolchain from
   JieLi's server and checks its SHA-256). `build` and `test` do this themselves when needed.

## Use

| Goal | Command |
| --- | --- |
| Build the firmware, the update loader and the installer packages | `node tools/op.mjs build` |
| Build, then write `build/SHA256SUMS` | `node tools/op.mjs package` |
| Host tests (engine, preset store, screen, simulated pedal, installer, editor) | `node tools/op.mjs test` |
| Remove every build output (needs no Docker) | `node tools/op.mjs clean` |
| A shell in the container | `node tools/op.mjs shell` |
| Dry-run or flash a built package to a connected pedal (host, USB MIDI) | `node tools/op.mjs flash`: see the `openpocket-flash` skill |

Outputs: `firmware/openpocket.bin`, `docs/firmware/PocketX_9NN.fwsc`, `BlackBox_9NN.fwsc`, `manifest.json`. All of them
are gitignored. Two clean builds give identical bytes; a changed checksum means the source or a pin changed.

The simulators are C programs that run the pedal's own code on the PC; `test` builds and drives them in the
container (so `build/` then holds Linux binaries). To run one on the host, compile it with `cc` first (these are the
lines from `tests/run_tests.sh`):

```
cc -O1 -Wall -Wno-unused-function -o build/devsim tests/devsim.c && node tests/test_installer.mjs
cc -O1 -Wall -Wno-unused-function -o build/edsim tests/edsim.c engine/px_dsp.c firmware/px_math.c -lm && node tests/test_editor.mjs
mkdir -p build/ui && cc -O1 -Wall -Wno-unused-function -Iengine -Ifirmware -o build/uisim tests/uisim.c engine/px_dsp.c firmware/px_math.c -lm && build/uisim build/ui
```

`devsim` is the update flow, `edsim` the editor protocol, `uisim` writes every screen page to `build/ui/*.ppm`.
`sh tests/run_tests.sh` runs the whole suite on the host with only `cc`, Node and Python 3 (the browser page tests need
Playwright and are skipped without it). `node tools/op.mjs shell` opens an interactive shell (needs a terminal).

## Updating the pinned toolchain or base image

Follow `references/updating-pins.md`. Never bypass the checksum check.

## Safety rules of this repository

- **Never flash a device unless the captain explicitly instructs it.** `node tools/op.mjs flash --dry-run` is allowed; `--confirm-flash`
  and any other write to a real pedal only on the captain's say-so, following the `openpocket-flash` skill. Builds and tests never
  touch a pedal. Hardware checks are in `docs/MIDI-TEST.md` when present.
- **Never publish an unreviewed package as the installer default.** A test package is a separate, labelled file and is
  not listed in the default `docs/firmware/manifest.json`. `PUBLISH-*.bat` is the maintainer's tool: do not run it.
- **Keep the official-firmware analysis out of the repository.** Facts about the vendor's firmware (offsets, tables,
  disassembly, bytes) stay in the captain's private notes. Write behaviour in your own words; never commit vendor code.
- Do not commit the JieLi toolchain or any proprietary binary; it is downloaded at image build time.

## Failures

`references/troubleshooting.md`: Docker not running, `exec format error`, slow emulation, checksum mismatch, root-owned
files, no network.
