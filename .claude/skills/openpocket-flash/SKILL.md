---
name: openpocket-flash
description: Build, dry-run and flash an OpenPocket package to a connected Donner Pocket X or M-VAVE BlackBox from the command line (node tools/op.mjs flash, over USB MIDI, no browser). Use to check that a built package fits the connected pedal, or, only when the captain explicitly instructs it, to write it. Covers the safety rules and the SD-card recovery.
---

# Flash a pedal from the command line

`node tools/op.mjs flash` (or `make flash ARGS="..."`) runs the same update protocol as the browser installer
(`docs/index.html`): the code is `docs/pxota.js` and `docs/pxpkg.js`, shared with the page; `tools/flash.mjs` only adds the
command line. Run everything from the repository root. The command runs on the host, not in the container: the pedal is a USB
device.

## Safety rules (read first)

1. **Never flash without the captain's explicit instruction** for this pedal and this package. A task that says "build" or
   "test" is not that instruction. Without it, stop after the dry run and report.
2. **Always dry-run first**, and read its output: pedal, model and package must be the ones intended.
3. Never put `--confirm-flash` in a script, alias or default. It is typed for one flash, on the captain's say-so.
4. Keep the pedal plugged in and powered until the command prints `done`. Close the browser installer and any other MIDI
   program first; they hold the port.
5. Only one pedal on the bus. The command refuses when none or several answer.
6. A failure prints the recovery pointer. Do not retry in a loop: report the output to the captain.

## 1. Set up once

```
node tools/op.mjs doctor          # Node 18+ (Docker only for building)
npm ci --prefix tools             # the one dependency, @julusian/midi (prebuilt RtMidi), into tools/node_modules
```

On Linux the MIDI library needs ALSA (`libasound2`); the pedal's USB MIDI port must be visible to your user.

## 2. Build the package

```
node tools/op.mjs package         # docs/firmware/PocketX_9NN.fwsc, BlackBox_9NN.fwsc, manifest.json, build/SHA256SUMS
```

Details and failures: the `openpocket-toolchain` skill. To flash a package built elsewhere (for example a test image from
another branch), keep its `.fwsc` next to the `manifest.json` that was built with it and pass both.

## 3. Dry run (writes nothing)

```
node tools/op.mjs flash --dry-run
node tools/op.mjs flash --dry-run --package build-x/PocketX_925.fwsc --manifest build-x/manifest.json
```

It checks the package against the manifest (size, SHA-256, identity, UFW and CRC structure), finds the pedal by asking each
MIDI input for its identity (a read-only query), and refuses a package for the other model. The pedal's flash is not touched.
Success ends with `dry run ok: would update PocketX_923 -> PocketX_924`.

Refusals (exit 2, nothing sent to the pedal): no `--confirm-flash` and no `--dry-run`; package not listed in the manifest;
SHA-256, size or identity differs from the manifest; wrong model; no pedal; several pedals; pedal already in update mode
(power-cycle it).

## 4. Flash (only on the captain's explicit instruction)

```
node tools/op.mjs flash --confirm-flash [--package FILE] [--manifest FILE]
```

Same checks as the dry run, then: the pedal stages its update loader (step 1), the loader writes the app area (step 2), the
pedal restarts, and the command reads the version it reports. `done: the pedal reports PocketX_924.` is success (exit 0); a
version that differs from the package is a failure. It takes about a minute. Report the full output to the captain.

## 4b. A pedal that restarts in a loop (only on the captain's explicit instruction)

```
node tools/op.mjs flash --rescue --confirm-flash [--package FILE] [--manifest FILE] [--timeout SECONDS]
make flash-rescue ARGS="--confirm-flash"
```

The installer page's rescue mode, on the command line. Start it first, then plug the pedal in or let it restart. It polls every
MIDI port every 100 ms; the moment the pedal answers its identity query it sends the upgrade command (which stops the sound
processing, so the restarts stop) and installs the package of the pedal's model. `--package` pins one package: a pedal of
the other model is refused before anything is sent. Without `--package` the manifest's package for the pedal's model is used.
It waits `--timeout` seconds (default 180); an install already running is never cut short. A pedal that vanishes before the
update finished has had nothing written: it is waited for again. `--rescue --dry-run` waits for the pedal and checks the model,
writing nothing. Reached states: the running firmware's MIDI port, and an OpenPocket update loader left half-done (finished only with `--package`: a loader does not report its model; a foreign loader is refused). Not reached
(same as the page): a firmware that crashes before it answers, and the chip's mask-ROM loader `WL80UBOOT` (USB 4C4A:8057, no
MIDI); both need the SD-card recovery. Timeout is exit 1 with the recovery text. One pedal on the bus: the first to answer is used.

## 5. Recovery

A failure (exit 1) prints `FAILED: ...` and the recovery text. In order:

1. Unplug USB, power-cycle the pedal, run `--dry-run` again to see what it reports. A pedal in update mode (`ota-...`) is
   finished by the browser installer's resume button, or by this command after a power-cycle.
2. If it stays dead, the SD-card recovery in the README (`flash/`: fm1buddy on an ESP32-S3; the image comes from your factory
   backup with `make appimage DUMP=fm1-1.bin` or `makeimage.bat`). Only the captain does this.

Never publish or commit a pedal backup (`fm1-1.bin`, `fm1-300.bin`): they hold the official firmware.

## Tests

`node tests/test_flash.mjs` (part of `node tools/op.mjs test`) drives the real command against the simulated pedal that runs
the pedal's own `ota.c` and loader code: update, dry run writes nothing, wrong model, damaged package, missing confirmation,
no pedal, two pedals, failure with the recovery text, and `--rescue` against a pedal that restarts in a loop (answers on the
Nth restart, never answers, vanishes mid-transfer, wrong model, missing confirmation). Real hardware is not covered by any test.
