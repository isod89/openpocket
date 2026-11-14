---
name: openpocket-simulator
description: Start and drive the hosted OpenPocket simulated pedal (node tools/op.mjs sim, make sim): the firmware's real screen, editor protocol and preset store running natively behind a local HTTP API, with a code-drawn front panel of the pedal as its page. Use to press or hold footswitches and buttons, turn the knobs, read the screen as a PNG, read the pedal state, send SysEx, dump or reset the flash, or to use the installer and editor pages against a simulated pedal in a browser. Use for verifying UI, editor-protocol or preset-store changes without a pedal, and for trying a preset or a screen. Use to hear a preset through bundled guitar loops, a file or a computer input. Not for USB or Bluetooth (not simulated).
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
| Hold a button | `curl -X POST $U/api/down/NAME`, later `$U/api/up/NAME`: the same ten names, timed as the firmware does (HOME 0.7 s, PUSH 0.8 s, double click 350 ms) |
| Turn the knob | `curl -X POST $U/api/knob/3` (negative: other way; `?fast` for steps of 5) |
| Volume, battery, tuner pitch | `POST $U/api/volume/55`, `/api/battery/3640[/USB_MV]`, `/api/tuner/110` |
| Bypass, load in the status text | `POST $U/api/bypass/1`, `/api/load/17` |
| Read the state | `curl -s $U/api/state` (slot, name, mode, focused block, the whole sound, `frame`, `held`, `knob`) |
| Read the screen | `curl -s "$U/api/screen.png?scale=2" -o screen.png`, then look at it; `screen.rgb565` is the raw pixels |
| SysEx | `curl -s -X POST $U/api/sysex -d 'f0 7d 4f 50 01 f7'` returns `{"reply":"hex"}` (`docs/EDITOR-PROTOCOL.md`) |
| Dump / reset the flash | `curl -s $U/api/flash -o flash.bin`; `curl -X POST $U/api/reset` |

Every request is applied before it is answered. The pedal's pages: `chain` (PLAY) -> `block` -> `edit`; `push_long` opens
`save` (knob picks the slot, `push` saves); `home_long` opens the tuner, any control leaves it. A worked path to change a
setting: `press/amp`, `knob/2`, `press/push`, `knob/8`, then `state` shows `preset.p[5][0]` up by 8 and `dirty: 1`.
Prove a screen claim by reading `screen.png`, not only the state.

## Browser

`$U/` is the pedal's front panel (`tools/sim/pedal.html`) with the live screen in its LCD: press and hold the buttons and
footswitches, drag or wheel the knobs, click PARA for PUSH, or use the keys listed on the page. Scripts on the page have
`window.pedal` (`down`, `up`, `tap`, `turn`, `master`, `state`); a control is `[data-control=NAME]`, `#master` or `#para`.
`$U/plain` is the plain page with the screen, every control as a button, the battery, the tuner pitch and the state.
`$U/site/index.html` (installer) and `$U/site/editor.html`
(editor) are `docs/` with a virtual Web MIDI injected, so Connect finds the simulated pedal in any browser; drive them with
`chrome-devtools-axi` or Playwright. For the installer use the update flow instead of the pedal:
`node tools/op.mjs sim --update FLASH.bin` (an image such as `flash_old.bin` from `python3 tests/fixtures.py DIR`).

An alternative firmware (an app of `firmware/apps`, `docs/ALT-FIRMWARE.md`) runs with `node tools/op.mjs sim --app starter`:
its own C files, compiled for this computer. `/api/state` then gives the product, and `/api/wait/MS`, `/api/sound/HZ` and
`NAME_long` presses exist (`docs/SIMULATOR.md`, "An app on the base firmware"). Its sound is measured, not played.

## Sound

The skin's Sound section runs the real engine (`engine/px_dsp.c`, built to `tools/sim/px_engine.wasm`) in an AudioWorklet:
pick a bundled loop (`assets/loops/`), a dropped file or a device, press Play (the first click starts the audio; a device
starts muted), and the preset on the screen is what you hear. `window.pedal.audio()` reports it (`running`, `heard`,
`muted`, ...), and `#in-meter`, `#out-meter` carry `data-db`. Without a browser, `node tools/sim/render.mjs OUTDIR [preset]`
renders the loops to WAV. After any change to `engine/`, run `node tools/op.mjs wasm` (Docker) and commit the new binary:
`tests/test_audio.mjs` fails otherwise. `docs/SIMULATOR.md` ("The sound") has the reasons and limits.

## Limits (say so when verifying)

No LEDs or bypass footswitch (the firmware has none: the skin's backlights are
its own reading of the state), no USB or Bluetooth. `press/NAME` has no timing, `down` and `up` do. The panel's TUNER line
is the stock firmware's: in OpenPocket both footswitches are two presses, and the tuner is a long press of HOME.
`press()` and the `button_*` functions in `tests/edsim.c` mirror the control dispatch of `firmware/openpocket.c`: if that
loop changes, change them too. The proof is the firmware's real code on a PC, not the pedal.

## Tests

`node tests/test_sim.mjs` (also in `node tools/op.mjs test`) covers the API, the flash file and the shim.
`node tests/test_pedal_input.mjs` covers the skin's input mapping, and `node tests/test_pedal_page.mjs OUTDIR` drives the skin
in Chromium and saves screenshots (needs Playwright). `node tests/test_audio.mjs` covers the engine and the loops, and
`node tests/test_audio_page.mjs OUTDIR` the Sound section in Chromium with a fake audio input (needs Playwright).
