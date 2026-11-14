# Base firmware and the products on it

## Sub-features
- The base (`firmware/base/`, `docs/BASE-FIRMWARE.md`): the code every image shares.
- OpenPocket built on it: the same bytes as the pinned release (`tests/release.sha256`: v42 with the bass tuner fix).
- STARTER (`firmware/apps/starter`, `docs/ALT-FIRMWARE.md`): an alternative firmware, a product of its own.
  `build/apps/starter/starter.bin`, its two packages, its manifest.
- The product identity: an app's INFO reply, the installer's separate card, the flasher's `--app`, the editor's notice.

## How to get to it (user POV)
A maintainer changes a file under `firmware/base/`, or `px_usb.h`, `app.ld`, `build.sh`. A user of OpenPocket sees no
difference. A user of an alternative firmware installs it from `index.html?app=NAME` or with `flash --app NAME`, and
goes back with the button "Back to OpenPocket" or with `flash` without `--app`.

## Driving it with op.mjs
1. `node tools/op.mjs all`: OpenPocket (it fails at the release pin when its bytes changed), each app, then the host
   tests with the packages just built. Read the lines "two products from one base" and "an app on the base firmware
   in the simulator".
2. `node tools/op.mjs sim --app starter --port 0 --reset`: the app's own C files on this computer. `GET /api/state`,
   `POST /api/press/NAME`, `/api/knob/N`, `/api/sound/440`, `/api/screen.png` (docs/SIMULATOR.md).
3. `sh .claude/skills/verify/scripts/pages.sh`: the installer page with `?app=starter` (install, the product named,
   Back to OpenPocket) and the editor's notice for a pedal on an alternative firmware.
4. After a change to a base file, the fast check is the object hash of `firmware/openpocket.c` compiled with
   `build.sh`'s flags, before and after. The full check is `node tools/op.mjs package`.

## Gotchas
- The simulators do not run an app's compiled image. `sim --app` compiles the app's C files for this computer;
  `devsim` runs the update code and the loader on the package's bytes.
- STARTER's start, its sound, and its USB on real hardware are verifiable only on a pedal (`verify-pedal`, the
  captain's call). `docs/ALT-FIRMWARE.md`, section 7, lists what was not checked.
- Extracting a function from `app_main`, or a loop written once over the channels in `base/audio.h`, can change
  OpenPocket's bytes even when the statements are the same. Check after every step.
- `node tools/op.mjs test` leaves Linux binaries in `build/`. `pages.sh` compiles its simulators again for the host.
