---
name: openpocket-simulator
description: Start and drive the hosted OpenPocket simulated pedal (node tools/op.mjs sim, make sim): the firmware's real screen, editor protocol and preset store running natively behind a local HTTP API. Use to press footswitches and buttons, turn the knob, read the screen as a PNG, read the pedal state, send SysEx, dump or reset the flash, or to use the installer and editor pages against a simulated pedal in a browser. Use for verifying UI, editor-protocol or preset-store changes without a pedal, and for trying a preset or a screen. Not for sound, USB or Bluetooth (not simulated).
---

# OpenPocket simulator

The pedal's own `ui.h`, `px_editor.h` and `px_store.h` run natively (`tests/edsim.c`) behind HTTP. Full reference:
`docs/SIMULATOR.md` (every endpoint, the state fields, what is not simulated). Run from the repository root. Needs Node 18+
and a C compiler (`node tools/op.mjs doctor` says whether `cc` is there); no Docker.

## Start

```
node tools/op.mjs sim --port 0 --reset &      # prints: OpenPocket simulator (pedal) listening on http://127.0.0.1:PORT/
```

`--port 0` picks a free port: read the URL from the first line of output. Without `--port` it is 8766. The flash lives in
`build/sim/flash.bin` (`--flash FILE`); without `--reset` a session continues where it stopped. The first start compiles
the simulator (a few seconds). Stop it with Ctrl-C or by killing the process **you** started.

## Drive (with `U=http://127.0.0.1:PORT`)

| Do | Request |
| --- | --- |
| A footswitch or button | `curl -X POST $U/api/press/NAME`: `fs_up` `fs_dn` `fx` `home` `push` `mod` `amp` `cab` `dly` `rev`; `push_long` (SAVE), `dpush` (MOVE), `home_long` (tuner) |
| Turn the knob | `curl -X POST $U/api/knob/3` (negative: other way; `?fast` for steps of 5) |
| Volume, battery, tuner pitch | `POST $U/api/volume/55`, `/api/battery/3640[/USB_MV]`, `/api/tuner/110` |
| Read the state | `curl -s $U/api/state` (slot, name, mode, focused block, the whole sound, `frame`) |
| Read the screen | `curl -s "$U/api/screen.png?scale=2" -o screen.png`, then look at it; `screen.rgb565` is the raw pixels |
| SysEx | `curl -s -X POST $U/api/sysex -d 'f0 7d 4f 50 01 f7'` returns `{"reply":"hex"}` (`docs/EDITOR-PROTOCOL.md`) |
| Dump / reset the flash | `curl -s $U/api/flash -o flash.bin`; `curl -X POST $U/api/reset` |

Every request is applied before it is answered. The pedal's pages: `chain` (PLAY) -> `block` -> `edit`; `push_long` opens
`save` (knob picks the slot, `push` saves); `home_long` opens the tuner, any control leaves it. A worked path to change a
setting: `press/amp`, `knob/2`, `press/push`, `knob/8`, then `state` shows `preset.p[5][0]` up by 8 and `dirty: 1`.
Prove a screen claim by reading `screen.png`, not only the state.

## Browser

`$U/` is a page with the screen, the controls and the state. `$U/site/index.html` (installer) and `$U/site/editor.html`
(editor) are `docs/` with a virtual Web MIDI injected, so Connect finds the simulated pedal in any browser; drive them with
`chrome-devtools-axi` or Playwright. For the installer use the update flow instead of the pedal:
`node tools/op.mjs sim --update FLASH.bin` (an image such as `flash_old.bin` from `python3 tests/fixtures.py DIR`).

## Limits (say so when verifying)

No sound or DSP (the load shown is fixed), no LEDs or bypass footswitch (the firmware has none), no USB or Bluetooth, no
real timing (a long press or a double click is one request). `press()` in `tests/edsim.c` mirrors the control dispatch of
`firmware/openpocket.c`: if that loop changes, change it too. The proof is the firmware's real code on a PC, not the pedal.

## Tests

`node tests/test_sim.mjs` (also in `node tools/op.mjs test`) covers the API, the flash file and the shim.
