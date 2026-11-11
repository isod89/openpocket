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

Only the control dispatch is mirrored: `press()` and the `button_*` functions in `tests/edsim.c` follow what the main
loop of `firmware/openpocket.c` does with a debounced press (PUSH, HOME, footswitches, the block buttons, a long press of
PUSH or HOME, a double click on PUSH, and their 800, 700 and 350 ms). If that loop changes, change them with it.

**Not simulated:** the LEDs and any bypass footswitch (the firmware does not drive any), USB and Bluetooth, the debounce. `POST press` has no timing: a double click or a long press
is one request; `POST down` and `POST up` have the firmware's.

## In a browser

`http://127.0.0.1:8766/` is the pedal: its front panel, drawn in code (`tools/sim/pedal.html`), with the live screen in
the LCD. It works as the pedal does, through `POST down` and `POST up`: a button acts when the firmware's does, so hold
HOME 0.7 s for the tuner (1.4 s for the diagnostics), press both footswitches together for the tuner, hold PARA 0.8 s
for SAVE, click PARA twice for MOVE.

| Control | Mouse or touch | Keys |
| --- | --- | --- |
| Footswitches | press, hold, release | Up (right, next preset), Down (left, previous) |
| HOME | press, hold | Escape or H |
| FX, AMP, MOD, DLY, REV, CAB | press | F, A, M, D, R, C |
| PARA, as a knob | drag (up or right: clockwise), wheel | Left, Right |
| PARA, as PUSH | click, hold | Enter |
| MASTER | drag, wheel | `[`, `]` |

Tab reaches each control. On a button, Enter and Space press it. On MASTER, the arrows move it by 2, Page Up and Down by
10, Home and End go to 0 and 100. On PARA, the four arrows turn it and Space presses it.

A knob follows the firmware: PARA counts detents, and detents less than 45 ms apart are the fast turn (steps of 5).
MASTER is 0..100 with the firmware's hysteresis of 2, so its smallest move is 2, and it always reaches 0 and 100. PARA
waits 120 ms to tell a press from a turn, so its hold starts that much late. The skin draws 24 detents a turn: the
firmware only counts them, so that number is an assumption.

Scripts on the page have `window.pedal`: `down(name)`, `up(name)`, `tap(name, ms)`, `turn(detents, fast)`, `master(volume)`
and `state()`. Each control has `data-control` (or the id `master` or `para`), and a button has `data-lit`.

The firmware drives no LEDs, so the backlights are the skin's own reading of the state (`backlights()` in
`tools/sim/pedal-input.js`). `data-lit` is `off`, `on` or `focus`. A block button is on when its block is on; FX when one
of its seven blocks is on; a button is `focus` on its own page; HOME is `focus` on the chain page. A footswitch's ring
brightens while it is held.

The panel's Sound section plays guitar through the pedal: see "The sound".

The TUNER line on the panel holds: both footswitches together open the tuner, as does a long press of HOME.

`http://127.0.0.1:8766/plain` is the earlier plain page: the screen, every control as a button, the battery, the tuner
pitch and the state.

The installer and the editor pages work against the simulated pedal without a change and without a driver:
`/site/index.html` and `/site/editor.html` serve `docs/` with the virtual Web MIDI (`/sim/webmidi-shim.js`) injected.
The shim replaces `navigator.requestMIDIAccess` with one input and one output port, "OpenPocket", and works in any
browser (also those without Web MIDI). For another copy of the pages, run once in the page's console:

```js
document.head.append(Object.assign(document.createElement("script"), { src: "http://127.0.0.1:8766/sim/webmidi-shim.js" }))
```

## The sound

The skin plays guitar through the pedal's real chain: `engine/px_dsp.c`, the firmware's own engine and maths
(`firmware/px_math.c`), built to WebAssembly and run in an AudioWorklet of the page. The preset is the one on the screen:
the page follows `GET state` (10 times a second, and after every control) and hands the engine the same bytes the
firmware's `audio_poll()` hands it, so footswitches, PARA, the block buttons, an editor page and `curl` all change the
sound. MASTER is the firmware's output gain, so it sets the level through the same curve.

| Control | Does |
| --- | --- |
| Input | a bundled loop, a file (drop one on the page, or choose one), or a device of the computer |
| Play, Stop | start and stop the source. The first click also starts the audio, because browsers require a gesture |
| Loop | repeat the loop or file |
| Bypass | the firmware's bypass: the dry input at MASTER, and the overload banner on the screen. It stays on across presets, unlike the pedal's, which is an overload guard and has no switch |
| Input level | the level into the pedal: 100 % puts a full-scale recording at guitar level (below) |
| Input, Output meters | peak dBFS going into and out of the engine, before the output volume |
| Output volume | the computer's master volume, after the pedal; MASTER is the pedal's own |

A device starts muted, so that a microphone next to the speakers cannot howl. Its meters move, and "Hear this input" lets
it through. Echo cancelling, noise suppression and automatic gain are turned off. The picker shows names after the
browser has been allowed to use the input, which it asks for at the first Play. Scripts have `window.pedal.audio()`:
`{running, sampleRate, playing, device, muted, heard, latencyMs}`, where `heard` is the name of the preset the engine runs.

The level matters to the sound: the amps are voiced for a guitar at the pedal's input, about 0.15 of full scale at the
engine (`tools/calib_levels.py`), which is what 100 % gives a full-scale recording. Drive it harder with Input level.

**Why WebAssembly in a worklet, and not the native engine streamed over a WebSocket.** Latency: the worklet adds one
block of 128 frames (2.9 ms, the firmware's own block) to what the browser's output costs, and a stream would add a round
trip, two jitter buffers and the page's main thread. In Chrome on a Mac the context reported 5.4 ms of its own, the same
at 44.1 kHz as at the sound card's 48 kHz; the output device adds what it adds (168 ms reported on that Mac's output),
and the page shows the sum. Fidelity:
it is the same code with the same 128-frame blocks and the firmware's own `px_math.c`; `tests/test_audio.mjs` holds it to
the native build exactly, on all 52 factory presets (the pedal's MASTER is scaled by 0.25 there so nothing clips, which is exact). Effort: Node has no WebSocket server, and the repository has no dependencies.
The price is a committed binary: `tools/sim/px_engine.wasm` (82 KB), so that the simulator still needs only a C
compiler. `node tools/op.mjs wasm` (`make wasm`) rebuilds it in the container with Debian's clang and lld. Next to it,
`px_engine.wasm.sources` holds the sources' checksum, and the test fails when the engine changes without a rebuild.

`engine/px_wasm.c` mirrors `dsp_block()`, `audio_apply()`, `audio_reset()` and `audio_volume()` of `firmware/px_audio.h`:
if those change, change it with them. Left out: the 24-bit output rounding, and the overload guard that drops to bypass,
because a computer's load says nothing about the pedal's.

**Sample rates.** The audio context runs at the engine's 44.1 kHz, and the browser converts everything: files when it
decodes them, devices at the source, and the output to the sound card. Firefox refuses to connect a device whose rate
differs from the context's, and the page then says so; loops and files are not affected. This path was tested only in
Chromium.

**The load.** The status text ("DSP 3%") and `dsp.load` show the time the engine took for a block as a share of 2.9 ms,
as the firmware measures it, but on this computer, so it is no prediction of the pedal's load. It stays 42 until the
sound first runs.

**Pages need a secure origin.** AudioWorklets and devices work on `localhost` and `127.0.0.1`. From another computer
(`--host`), the page needs https, or the Sound section says that it has no worklet.

**The loops** are in `assets/loops/`: five dry, direct-input takes from the Guitar-TECHS dataset, CC BY 4.0, so they
are not under the repository's GPL. They are mono 16-bit WAV at 44.1 kHz, peaking at -3 dBFS, 2.6 MB in all, so they are
committed rather than fetched; WAV because every browser and the tests read it without a decoder. `manifest.json` and
`ATTRIBUTION.md` record each one's source, licence and checksum, and `prepare.py` how it was made. Each runs from a note
to the same note some bars later, with a 20 ms crossfade, and the browser loops it gaplessly. `node tools/sim/render.mjs OUTDIR [preset]` renders each one through the engine, twice around, to
WAV files.

## The API

All paths start with `/api/`. Answers are JSON unless stated; an error is `{"error": "..."}` with status 400 (bad
request), 405 (wrong method), 409 (not in this mode) or 503 (the program did not answer). Every request is applied
before it is answered, so a state read right after a press shows the press.

| Request | Does |
| --- | --- |
| `GET info` | `{kind: "pedal" \| "update", port, flash, controls, log}`; `port` is the MIDI port name, `null` when none |
| `GET state` | the pedal's state (below) |
| `POST press/NAME` | a control: `fs_up` `fs_dn` `fx` `home` `push` `mod` `amp` `cab` `dly` `rev`; `push_long` (hold PUSH: SAVE), `dpush` (double click PUSH: MOVE), `home_long` (hold HOME: tuner; on the tuner: diagnostics), `fx_long` (hold FX: MIDI), `feet` (both footswitches: tuner) |
| `POST down/NAME`, `POST up/NAME` | a button goes down, comes up: `fs_up` `fs_dn` `fx` `home` `push` `mod` `amp` `cab` `dly` `rev`. Timed as the firmware does (`ui_buttons` in `firmware/ui.h`): HOME, PUSH and FX act at a short release, or after 0.7 s, 0.8 s and 0.8 s held (HOME held on as long again: the diagnostics); a PUSH click on the chain opens the block at once and a second one within 350 ms makes it MOVE; the others act when they go down; the second footswitch down within 150 ms of the first, while the first is held, takes the first one's step back and opens the tuner. A button already down, or not down, is left as it is |
| `POST knob/N[?fast]` | the PARA knob N detents (negative: the other way); `fast` is the quick-turn step of 5 |
| `POST volume/N` | the MASTER knob, 0..100 |
| `POST battery/MV[/USB_MV]` | the battery voltage the screen shows; `USB_MV` above 4300 shows "USB" |
| `POST tuner/HZ` | the pitch the tuner page hears (0: none) |
| `POST bypass/0`, `POST bypass/1` | the bypass: `dsp.bypass`, the overload banner on the screen, the dry input in the sound |
| `POST load/N` | the load in the status text and `dsp.load`, 0..999. The page sets it while the sound runs |
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
 "dsp": {"load": 42, "bypass": 0, "core1": 1}, "last_press": "fs_up", "presses": 1, "knob": 0, "held": [],
 "preset": {"name": "TWIN CLEAN", "on": [..12], "type": [..12], "order": [..12], "p": [[..5] x 12]}}
```

`slot` is 0..51 (the pedal shows 1..52); `dirty`: the sound differs from its slot (the pedal's "EDITED"); `saved`: the
slot holds a saved preset (0: factory); `mode`: the page, one of `chain` `block` `edit` `tuner` `save` `move` `midi` `list` (the preset list) `diag` (diagnostics); `block`:
the focused block (0 GATE, 1 COMP, 2 PITCH, 3 SFX, 4 DRIVE, 5 AMP, 6 CAB, 7 EQ, 8 MOD, 9 DELAY, 10 REVERB, 11 SYN);
`writes_ok` is 0 once the firmware tried to write outside the preset area of the flash; `frame` counts the drawing
calls: it changes when the screen does; `knob`: the detents PARA has turned since the start; `held`: the buttons that are
down (`POST down`). `preset` is the current sound, as in `docs/EDITOR-PROTOCOL.md`.

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

`tests/test_audio.mjs` feeds a test tone and each loop through the WebAssembly engine and the native build for all 52 presets (identical, all finite), checks each loop against `manifest.json` and `ATTRIBUTION.md`, and
(with `EVIDENCE_DIR` set) writes the largest difference per preset and signal to `audio-parity.tsv`;
`tests/test_audio_page.mjs` drives the Sound section in Chromium with a fake audio input (with Playwright installed).
`tests/test_sim.mjs` (part of `make test`) starts the simulator and checks every row above, then runs the real editor
(`docs/pxedit.js`) and installer (`docs/pxota.js`) code over the shim. `tests/test_pedal_input.mjs` checks the skin's input
mapping, and `tests/test_pedal_page.mjs` drives the skin in Chromium when Playwright is installed.
