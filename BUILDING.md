# Building OpenPocket

One container, one set of commands, on macOS (Apple silicon and Intel), Linux and Windows.

## Quick start

```
make package      # build the firmware + installer packages, print their SHA-256
make test         # run the host tests
```

Every command is `node tools/op.mjs <command>` (Node 18 or newer); `make` is a shortcut for it on macOS and Linux, and the `.bat` files are shortcuts for it on Windows (double-click **`build.bat`**). The Docker command lines live only in `tools/op.mjs`.

## Requirements

| System | You need |
| --- | --- |
| macOS | Docker Desktop (or Colima), Node 18+, `make` optional (Xcode command line tools) |
| Linux | Docker, Node 18+, `make` optional |
| Windows | Docker Desktop (WSL 2 backend), Node 18+ (nodejs.org). `make` is not needed. Without Docker: WSL with the toolchain installed in it once (`wsl sh firmware/setup.sh`); `build.bat` then builds there |

The toolchain is JieLi's compiler, and it only exists for x86-64 Linux. The container is therefore always `linux/amd64`. On
Apple silicon and other ARM machines Docker emulates it (Rosetta or QEMU); you change nothing.

## Commands

| Command | What it does |
| --- | --- |
| `make doctor` | check Node, Docker and the image, one line each (exit 1 when the build cannot work) |
| `make build` | `firmware/build.sh` in the container: `firmware/openpocket.bin`, the update loader, `docs/firmware/PocketX_9NN.fwsc`, `BlackBox_9NN.fwsc`, `manifest.json` |
| `make ble` | the Bluetooth MIDI test image (`OP_BLE=1 firmware/build.sh`): `firmware/openpocket-ble.bin`, `build/ble/*.fwsc`. Own target, own output folders; [BLE-TEST.md](BLE-TEST.md) |
| `make serve-ble` | `make serve` with `build/ble/` as the installer's package folder, to flash the test image |
| `make package` | `build`, then `build/SHA256SUMS` |
| `make package-test` | the MIDI test channel (`OP_CHANNEL=midi-test`): `firmware/openpocket-midi-test.bin` and `docs/firmware/midi-test/` (v37 MIDI TEST, its own `manifest.json`), with `build/SHA256SUMS-midi-test`. The release files are not touched and the installer offers it only with `?channel=midi-test` |
| `make test` | `tests/run_tests.sh` (engine, preset store, screen, simulated pedal, installer, editor; the browser page tests also run when Playwright is installed) |
| `make clean` | remove every build output (on the host; needs neither Docker nor the image) |
| `make shell` | a shell in the container (as your user) |
| `make serve` | the installer and editor on `http://localhost:8765/` (offline install; needs Python 3 on the host) |
| `make flash ARGS="--dry-run"` | write a built package to a connected pedal over USB MIDI, no browser (`--confirm-flash` really writes; see below) |
| `make sim [ARGS="--port 9000"]` | the simulated pedal on `http://127.0.0.1:8766/`: screen, controls, state, an HTTP API, the installer and editor pages on a virtual MIDI port (`docs/SIMULATOR.md`; needs a C compiler on the host, no Docker) |
| `make log ARGS="--follow"` | read a connected pedal's state over USB MIDI, with the fields explained and a verdict (`docs/BLE-TEST.md`, "Reading the pedal from a computer") |
| `make render IN=guitar.wav [OUT=..] [PRESET=1]` | a DI recording through the engine |
| `make appimage DUMP=fm1-1.bin` | `fm1-300.bin` for the SD-card recovery |
| `make rescue` / `make rescue-write` | restore a looping BlackBox from its UBOOT state from a Mac (read-only first; asks for sudo; `ARGS="--package FILE"` for options): `docs/RESCUE.md` |
| `make rescue-check` | the UBOOT rescue tool (`tools/uboot_rescue.py`, see `docs/RESCUE.md`) against a simulated pedal; needs only Python 3 |

`make NATIVE=1 build` skips Docker and uses a toolchain installed on the machine (`sh firmware/setup.sh` installs it in
`~/.jieli`; Linux x86-64 only). `tests/run_tests.sh` also runs directly on macOS and Linux with only a C compiler, Node and
Python 3. Its NAM check (`tests/test_nam.py`, the fixed-point network against the float reference) also needs NumPy
and is skipped without it (the container has none).

The NAM factory models (`engine/px_nam_factory.h`, v37) are generated, not hand-written: `tools/train_a2.py` trains each
A2-Lite network from a capture (needs `pip install jax optax numpy scipy`, about 15 minutes a model on two CPU cores),
then `tools/mk_nam_factory.py LIST.txt DIR docs/nam_di.wav` writes the header with each model's level trim
([NAM.md](NAM.md)).

The `.bat` files `makeimage.bat` and `render.bat` only check that Node is installed and call `tools/op.mjs`. `build.bat` does the same when Node and Docker are there, and otherwise builds in WSL (`build.bat docker` or `build.bat wsl` forces one way). `INSTALL-OFFLINE.bat` needs nothing installed: it serves `docs\` with PowerShell (`tools/serve_offline.ps1`), and falls back to Python (`tools/serve.py`) or WSL only if PowerShell cannot open a port. `tests/test_op.mjs` (part of `make test`) checks the Docker command lines, the friendly errors and `make clean` without Docker, using a fake `docker`. `PUBLISH-GITHUB.bat` and `PUBLISH-SOURCE.bat` (PowerShell
`tools/publish_github.ps1`) are the maintainer's release tool and still need Windows; nothing else does.

## Flashing from the command line

`make flash ARGS="--dry-run"` (or `node tools/op.mjs flash --dry-run`) finds the pedal over USB MIDI and checks the built package without writing; `--confirm-flash` instead of `--dry-run` writes it. It runs on the host (not in the container), speaks the same update protocol as the installer page (`docs/pxota.js`, shared with the page), and checks the package against `docs/firmware/manifest.json` (size, SHA-256, identity), refuses the wrong model, and verifies the version the pedal reports afterwards. `--package FILE --manifest FILE` flash a package built elsewhere. Install its one dependency once with `npm ci --prefix tools`. The rules for agents (dry run first, never flash without the captain's explicit instruction, recovery) are in `.claude/skills/openpocket-flash/SKILL.md`. On failure the command prints the SD-card recovery pointer (README, `flash/`). `tests/test_flash.mjs` runs it against the simulated pedal; no test touches real hardware.

**A pedal that restarts in a loop.** `make flash-rescue ARGS="--confirm-flash"` (or `node tools/op.mjs flash --rescue --confirm-flash [--package FILE] [--timeout SECONDS]`) is the installer page's rescue mode on the command line, and uses the same code (`Updater.rescueWithin` in `docs/pxota.js`). Start it, then plug the pedal in or let it restart. It asks every MIDI port for the pedal's identity every 100 ms; the moment one answers, it stops the pedal's sound processing (which ends the restarts) and installs the package of that pedal's model, printing `pedal answered: ...` and one line per step. If the pedal disappears before the update finished, nothing has been written yet: it waits for the pedal again, and an OpenPocket update loader left behind is finished when `--package` names the package (a loader does not report its model; without `--package` the timeout message says so). It waits 180 s by default (`--timeout`); an install that has started is never cut short. `--rescue --dry-run` only waits for the pedal and checks the model. The same refusals apply (no `--confirm-flash`, wrong model, damaged package). Only a pedal whose firmware answers the identity query for a moment is caught, as on the page; a pedal that crashes before it answers, or sits in the chip's mask-ROM boot loader (`WL80UBOOT`, USB 4C4A:8057, no MIDI), is out of reach and needs the SD-card recovery. Connect only one pedal: the first to answer is used.

## What is pinned

- The base image `debian:stable-slim`, by digest (`Dockerfile`).
- The JieLi toolchain `jieli-linux-toolchains-20260730.1.tar.xz`, by URL and SHA-256 (`firmware/setup.sh`). The build fails if
  the checksum differs.

To update either pin, change it in one commit, run `make clean package test`, and compare the new `build/SHA256SUMS` with the
old one.

## Licence of the toolchain

The toolchain is JieLi's proprietary software. It is **not** in this repository and is not redistributed by it: the image
build downloads it from JieLi's own server (`jl-update.oss-cn-shenzhen.aliyuncs.com`, the file `https://pkgman.jieliapp.com/s/linux-toolchain`
redirects to). If you publish a container image, do not include the toolchain layer. The Apache-2.0 SDK parts in
`firmware/sdk/` and `third_party/jieli-ble/` are covered by `NOTICE.md`.

## Reproducibility

The build is deterministic: two clean builds in the container give byte-identical `openpocket.bin`, `.fwsc` and
`manifest.json` (checked on Apple silicon under emulation, SHA-256 equal). No build output is committed to the repository
(`.gitignore`), so there is no committed binary to compare with; release packages are published as GitHub release assets,
and `build/SHA256SUMS` of a given commit is the reference. CI (`.github/workflows/build.yml`) builds and tests every pull
request on a native x86-64 runner and uploads the packages with their checksums.

## Troubleshooting

- **`Cannot connect to the Docker daemon`**: start Docker Desktop (or `colima start`).
- **`exec format error` on Apple silicon**: Docker's emulation is off. In Docker Desktop, turn on *Use Rosetta* (or install QEMU
  with `docker run --privileged --rm tonistiigi/binfmt --install amd64`).
- **`checksum mismatch` while building the image**: JieLi replaced the file at that URL. Do not bypass the check; open an
  issue with the new SHA-256.
- **No network for the toolchain download**: build once on a connected machine; the image `openpocket-build` keeps it.
- **Files owned by root on Linux**: `tools/op.mjs` runs the container as your user; if you ran `docker run` by hand, `sudo chown -R $USER .`.

## Time

On an Apple M-series Mac under emulation: first image build about 1 min (download of 25 MB), `make build` about 10 s,
`make test` about 1 min. Native x86-64 is faster.

## Not covered

The Docker path on native Windows was not tested (no Windows machine here); `tools/op.mjs` is tested on macOS and Linux with a fake `docker`. The WSL build and the PowerShell offline server are the ones v36 was built and installed with. The page
tests that need Playwright are skipped unless it is installed.
