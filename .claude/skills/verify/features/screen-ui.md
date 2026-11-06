# Pedal screen

## Sub-features
Play page (preset, tiles of the 12 blocks), block page, edit page, save page, tuner, boot and error screens.

## How to get to it (user POV)
Knob, PUSH, HOME, footswitches and block buttons on the pedal.

## Driving it with uisim
`node tools/op.mjs test` builds and runs `tests/uisim.c` (the real `ui.h`), writing every page to `build/ui/*.ppm` and a log in which `never drawn` must not appear. View a PPM by converting it (`sips -s format png file.ppm --out file.png` on macOS).

## Driving it live with the hosted simulator
`node tools/op.mjs sim --port 0 --reset` runs the same `ui.h` on a real pedal state (`tests/edsim.c`). Press buttons (`POST /api/press/NAME`, `/api/knob/N`), then `GET /api/screen.png?scale=2` and look at it; `GET /api/state` gives the page (`mode`), the focused block and the sound. This reaches what `uisim`'s script does not: any sequence a user can play, the save flow into a slot and the flash file after it, the tuner with `POST /api/tuner/HZ`, the battery and volume. Skill: `openpocket-simulator`.

## Gotchas
- Colours and fonts are checked as drawn pixels, not on the real LCD.
- New screens must be added to `tests/uisim.c` or they are never checked.
