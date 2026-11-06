# Installer page (docs/index.html)

## Sub-features
Connect to the pedal over Web MIDI, detect the running firmware (OpenPocket vN or official), Install, Rescue mode (pedal in a boot loop), return to the official firmware.

## How to get to it (user POV)
Open the page in Chrome/Edge, press Connect, plug the pedal in, press Install.

## Driving it with Playwright
`sh .claude/skills/verify/scripts/pages.sh`; `tests/test_page.mjs` serves `docs/` plus fixture packages, bridges a fake Web MIDI to `build/devsim` (the pedal's real `ota.c` and `ldr_core.c`) and clicks Connect, Install, rescue. End state: "Done: the pedal now runs PocketX_924", the simulated flash holds the new app, screenshots `build/page/*.png`.

## Driving it against the hosted simulator
`node tools/op.mjs sim --update FLASH.bin --port 0` serves `/site/index.html` with a virtual Web MIDI over `tests/devsim.c` on that image (make one and fake packages with `python3 tests/fixtures.py DIR`; the page also needs its packages in `docs/firmware/`). `GET /api/info` shows the steps the pedal took in `log`. The image is never written; the result goes to `build/sim/update-dump.bin`. Skill: `openpocket-simulator`.

## Gotchas
- Fixtures are version 924 packages made by `tests/fixtures.py`, not the real build.
- Nothing proves a real pedal: only the captain installs, by hand.
- The harmless `Failed to load resource` line is the page's web-font request being refused offline.
