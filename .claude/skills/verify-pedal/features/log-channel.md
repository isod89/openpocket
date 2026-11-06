# Log channel

The pedal's state as text over USB MIDI: this boot, the last boot, the status line the screen scrolls.

## Sub-features

- `DIAG`, command 19 (`docs/EDITOR-PROTOCOL.md`), read only; `firmware/px_editor.h`, `firmware/ble/ble_diag.h`.
- `tools/log.mjs`: parsing, meanings, verdict. It falls back to `INFO` and `PING`, then to the identity query.

## How to get to it (user POV)

Plug the pedal in over USB and run the command. No screen, no browser.

## Driving it with tools/op.mjs

`node tools/op.mjs log --count 2 --interval 2`: `i` rises between the readings.
`--json` gives the same as objects, with `raw` holding each line as the pedal sent it.

## Gotchas

- The pedal clock in `this boot` wraps every 179 s, so it is not the uptime after that; the event rate uses this computer's clock.
- Do not run it during a flash: both talk to the same port.
- The line is the screen's own text (`ble_test_line`), so a new field shows in `raw` before the parser knows it.
- `tests/test_log.mjs` runs this against the real editor code; it says nothing about USB on hardware.
