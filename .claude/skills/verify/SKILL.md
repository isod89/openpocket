---
name: verify
description: Verify OpenPocket (guitar multi-effects firmware for the Donner Pocket X / M-VAVE BlackBox) without a pedal: build the firmware and installer packages in the pinned container, run the host tests and simulators, and drive the web installer (docs/index.html) and editor (docs/editor.html) in headless Chromium against simulated pedals. Use after any change to firmware/, engine/, loader/, tools/, tests/ or docs/*.html|js, and before claiming a change works. For a connected pedal, use the verify-pedal skill; only a person can verify sound.
---

# Verify OpenPocket

No user session exists to drive: the product is firmware that runs on a pedal, plus two static web pages. "Driving it as a
user" means (1) building what a user installs, (2) running the pedal's own code on the PC in simulators, and (3) clicking
through the installer and editor pages with a mocked Web MIDI device. Never flash or contact a real device from here: that
is the `verify-pedal` skill.

## Launch

Nothing stays running except short-lived test servers, which the scripts start and stop themselves, and the hosted
simulator when you start it (`node tools/op.mjs sim`, skill `openpocket-simulator`): stop the one you started.

```
node tools/op.mjs doctor      # Node, Docker, image (toolchain skill: .claude/skills/openpocket-toolchain)
node tools/op.mjs package     # firmware/openpocket.bin, docs/firmware/*.fwsc, manifest.json, build/SHA256SUMS
node tools/op.mjs test        # engine, store, screen, simulated pedal, installer, editor (in the container)
sh .claude/skills/verify/scripts/pages.sh   # installer + editor pages in headless Chromium
```

First `image` takes about 1 minute, `package` 10 s, `test` about 1 minute (emulated on Apple silicon). `pages.sh`
installs Playwright and Chromium once into `~/.cache/openpocket-pw` (about 100 MB, never into the repository).

## Doctor

`sh .claude/skills/verify/scripts/doctor.sh` is read-only: Node, Docker, `cc`, Python, Playwright, uncommitted changes.
Run it first whenever anything looks off.

## Drive

| Feature | Command | Proof |
| --- | --- | --- |
| Build and packages | `node tools/op.mjs package` | `build/SHA256SUMS`: same sources give the same bytes |
| Engine, store, screen, update protocol, editor protocol | `node tools/op.mjs test` | every section ends `ok` / `passed`; `build/ui/*.ppm` are the drawn pages |
| Installer page, update flow | `pages.sh` (runs `tests/test_page.mjs`) | `build/page/page_1..4*.png`, last line `page test passed` |
| Editor page | `pages.sh` (runs `tests/test_editor_page.mjs`) | `build/editor/editor_1..6*.png`, last line `editor page test passed` |
| Screen layout | `cc ... tests/uisim.c` (see toolchain skill), `build/uisim build/ui` | PPM pages, `never drawn` absent from the log |
| The pedal as a user plays it: footswitches, knob, pages, save, tuner, the live screen | `node tools/op.mjs sim`, then the HTTP API (skill `openpocket-simulator`, `docs/SIMULATOR.md`) | `GET /api/state` and `/api/screen.png` after each press; the flash file `build/sim/flash.bin` survives a restart |
| The pedal skin: every control by mouse, touch and keyboard, the backlights, the live LCD | `pages.sh` (runs `tests/test_pedal_page.mjs`) | `build/pedal/pedal-skin*.png`, last line `pedal page: ok` |
| The sound in the simulator: the engine on known signals, against its native build; the input picker, meters, bypass, the preset heard | `node tests/test_audio.mjs`, `pages.sh` (runs `tests/test_audio_page.mjs`) | every line `ok`; `build/audio/sound.png`; `node tools/sim/render.mjs DIR` renders WAV files to listen to |
| Editor and installer pages by hand or with `chrome-devtools-axi` | `sim` serves them at `/site/editor.html` and `/site/index.html` with a virtual Web MIDI | Connect finds "OpenPocket"; an edit shows in `/api/state` |
| MIDI musical messages | see `features/midi-usb.md` | host simulator tests when the USB-MIDI feature is in; hardware: `docs/MIDI-TEST.md` |

Features: `features/README.md` and one file per feature there. A proof that drives only one entry point is incomplete when
the map lists more.

## Evidence

Screenshots and logs go to `build/` (gitignored); copy what you cite to a folder outside the repo. Proof standards: run the
real code (the simulators compile the firmware's own `px_editor.h`, `px_store.h`, `ui.h`, `ota.c`), not a re-implementation;
a mocked boundary is only Web MIDI/USB (already isolated by the tests); state what was exercised and what was not.
**Not verifiable here:** audio on the pedal, its DSP load and glitches, USB enumeration on a real host, Bluetooth, flashing, the screen on
the real LCD. Say so, and point to `docs/MIDI-TEST.md` (hardware checklist) when it exists.

## Cleanup

`node tools/op.mjs clean` removes build outputs (including `build/`: copy evidence first). The scripts leave no process
behind; if one is interrupted, `ps` for `devsim`/`edsim`/`chrome-headless-shell` that YOU started and kill those PIDs only.

## Maintain

`/maintain-verification-skill` keeps this skill and `features/` honest. Re-run `doctor.sh` and `pages.sh` after changing the
pages, the simulators or `tools/op.mjs`, and update the feature files when behaviour moves.
