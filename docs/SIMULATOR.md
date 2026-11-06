# The simulated pedal

`node tools/op.mjs sim` (or `make sim`) runs the pedal's own code natively on your computer and puts it behind a local
HTTP interface. A person plays with it in a browser; a script or an agent drives it with `curl`. It needs Node 18+ and a
C compiler (`cc`: Xcode command line tools on macOS, `build-essential` on Linux); no Docker, no toolchain, no pedal.

```
node tools/op.mjs sim                    # http://127.0.0.1:8766/
node tools/op.mjs sim --port 9000 --reset
node tools/op.mjs sim --update FLASH.bin # the update flow, for the installer (see below)
```

| Option | Meaning |
| --- | --- |
| `--port N` | the port (default 8766; `0` picks a free one, the URL is printed) |
| `--host H` | the address to listen on (default `127.0.0.1`: this computer only) |
| `--flash FILE` | the pedal's flash, kept between runs (default `build/sim/flash.bin`) |
| `--reset` | delete the flash file first: factory presets only |
| `--update FILE` | run the update flow on this 1 MiB flash image (it is never written), see "The installer" |

## What runs

`tests/edsim.c`, built into `build/sim/edsim`, compiles the firmware's own code: the editor protocol
(`firmware/px_editor.h`), the preset store (`firmware/px_store.h`) on a 1 MiB flash array, the screen
(`firmware/ui.h`, `ui_text.h`, drawn into the 240 x 240 frame buffer of `tests/simlcd.h`) and the engine's catalogue
(`engine/px_dsp.c`). `tests/devsim.c` (`--update`) runs `firmware/bsp/chip/ota.c` and `loader/ldr_core.c`. The programs
are the ones the tests use; `tools/sim.mjs` only talks to them on stdin/stdout. Their line protocol is documented in the
header of each `.c` file; `tests/devsim.c` and the old `edsim` commands are unchanged.

Only the control dispatch is mirrored: `press()` in `tests/edsim.c` follows what the main loop of
`firmware/openpocket.c` does with a debounced press (PUSH, HOME, footswitches, the block buttons, a long press of PUSH
or HOME, a double click on PUSH). If that loop changes, change `press()` with it.

**Not simulated:** sound (the DSP never runs; "DSP 42 %" is a fixed number), real time (a double click is one request,
a long press is one request), the LEDs and any bypass footswitch (the firmware does not drive any), USB and Bluetooth,
the debounce.

## In a browser

`http://127.0.0.1:8766/` shows the screen, the controls (footswitches, FX, HOME, PUSH, MOD AMP CAB DLY REV, the knob,
long and double presses, the volume knob, the battery, the tuner pitch) and the state. Keys: arrows are the
footswitches (up and down) and the knob (left and right), Enter is PUSH, Escape is HOME.

The installer and the editor pages work against the simulated pedal without a change and without a driver:
`/site/index.html` and `/site/editor.html` serve `docs/` with the virtual Web MIDI (`/sim/webmidi-shim.js`) injected.
The shim replaces `navigator.requestMIDIAccess` with one input and one output port, "OpenPocket", and works in any
browser (also those without Web MIDI). For another copy of the pages, run once in the page's console:

```js
document.head.append(Object.assign(document.createElement("script"), { src: "http://127.0.0.1:8766/sim/webmidi-shim.js" }))
```

## The API

All paths start with `/api/`. Answers are JSON unless stated; an error is `{"error": "..."}` with status 400 (bad
request), 405 (wrong method), 409 (not in this mode) or 503 (the program did not answer). Every request is applied
before it is answered, so a state read right after a press shows the press.

| Request | Does |
| --- | --- |
| `GET info` | `{kind: "pedal" \| "update", port, flash, controls, log}`; `port` is the MIDI port name, `null` when none |
| `GET state` | the pedal's state (below) |
| `POST press/NAME` | a control: `fs_up` `fs_dn` `fx` `home` `push` `mod` `amp` `cab` `dly` `rev`; `push_long` (hold PUSH: SAVE), `dpush` (double click PUSH: MOVE), `home_long` (hold HOME: tuner) |
| `POST knob/N[?fast]` | the PARA knob N detents (negative: the other way); `fast` is the quick-turn step of 5 |
| `POST volume/N` | the MASTER knob, 0..100 |
| `POST battery/MV[/USB_MV]` | the battery voltage the screen shows; `USB_MV` above 4300 shows "USB" |
| `POST tuner/HZ` | the pitch the tuner page hears (0: none) |
| `GET screen.png[?scale=N]` | the screen as a PNG, 240 x 240 times N (1..8, nearest neighbour) |
| `GET screen.rgb565` | the raw frame buffer: 240 x 240 pixels, 2 bytes each, big endian, row by row |
| `GET flash` | the 1 MiB flash, as it is now |
| `POST reset` | erase the flash (the file too) and restart the pedal: factory presets |
| `POST restart` | restart the pedal on the same flash, as a power cycle |
| `POST midi` | send one MIDI message (SysEx) and return at once. Body: hex (`f07d4f5001f7`, spaces allowed) or a JSON array of bytes |
| `POST sysex[?timeout=MS]` | send one message and wait for its answer: `{"reply": "hex", "ms": N}`, `reply` is `null` after the timeout (default 1000). An editor request (`F0 7D 4F 50 cmd ...`) is answered by the frame with the same `cmd`. One at a time |
| `GET events` | Server-Sent Events: `event: port` (`data:` the port name, empty when none) and `event: midi` (`data:` hex of each message the pedal sends) |

`state`:

```json
{"slot": 0, "name": "TWIN CLEAN", "dirty": 0, "saved": 0, "mode": "chain", "block": 5, "item": 0, "volume": 80,
 "battery_mv": 3920, "usb_mv": 0, "tuner_hz": 0, "writes_ok": 1, "frame": 35324,
 "dsp": {"load": 42, "bypass": 0, "core1": 1}, "last_press": "fs_up", "presses": 1,
 "preset": {"name": "TWIN CLEAN", "on": [..12], "type": [..12], "order": [..12], "p": [[..5] x 12]}}
```

`slot` is 0..51 (the pedal shows 1..52); `dirty`: the sound differs from its slot (the pedal's "EDITED"); `saved`: the
slot holds a saved preset (0: factory); `mode`: the page, one of `chain` `block` `edit` `tuner` `save` `move`; `block`:
the focused block (0 GATE, 1 COMP, 2 PITCH, 3 SFX, 4 DRIVE, 5 AMP, 6 CAB, 7 EQ, 8 MOD, 9 DELAY, 10 REVERB, 11 SYN);
`writes_ok` is 0 once the firmware tried to write outside the preset area of the flash; `frame` counts the drawing
calls: it changes when the screen does. `preset` is the current sound, as in `docs/EDITOR-PROTOCOL.md`.

Examples:

```sh
curl -X POST localhost:8766/api/press/fs_up                      # next preset
curl -X POST localhost:8766/api/press/amp                        # the AMP page
curl -X POST "localhost:8766/api/knob/2"                         # select the first setting
curl -X POST localhost:8766/api/press/push                       # edit it
curl -X POST "localhost:8766/api/knob/8"                         # +8
curl -s localhost:8766/api/state
curl -s "localhost:8766/api/screen.png?scale=2" -o screen.png
curl -s -X POST localhost:8766/api/sysex -d 'f0 7d 4f 50 01 f7'  # the editor protocol's INFO
```

## The flash

The flash is a file (`build/sim/flash.bin`, or `--flash`). The pedal writes it after every change, whole or not at all,
so a session survives a restart, a crash and Ctrl-C. Save a preset (hold PUSH, turn the knob, PUSH), stop the simulator,
start it again: the preset is there. `POST reset` or `--reset` starts again from the factory presets. The file is a 1 MiB image; only its preset area (0xF0000 to 0xF4000) is ever written.

## The installer

The installer needs the update code, which is a different program, so `--update FLASH.bin` starts it instead of the
pedal: `tests/devsim.c` answers the handshake and runs the update of `ota.c` and `ldr_core.c` on a copy of the image in
memory (the file is never written; the result goes to `build/sim/update-dump.bin` when the simulator stops). The
port renames itself as the real pedal's does ("OpenPocket" -> "OpenPocket Update" -> "OpenPocket"); the shim reports each
change as a Web MIDI `statechange`. Open `/site/index.html`. `GET info` shows the program's `log` (the steps it took).
Controls, screen and state answer 409 in this mode. `tests/fixtures.py DIR` builds a flash image (`flash_old.bin`) and
fake packages for a try.

## Tests

`tests/test_sim.mjs` (part of `make test`) starts the simulator and checks every row above, then runs the real editor
(`docs/pxedit.js`) and installer (`docs/pxota.js`) code over the shim.
