# The base firmware

The firmware tree builds more than one image for the pedal. The **base** is the code that every image has, whatever
the image does with the sound. An **app** is one image built on the base, and each app is a product of its own.
OpenPocket is the first app. STARTER (`firmware/apps/starter`) is the second: a small alternative firmware that exists
to be copied.

The base exists so that an experiment that does not fit in OpenPocket can be its own image. OpenPocket's engine
region has 40 bytes free, so a different engine needs a different memory layout. An app is installed with the same
installer as OpenPocket, and OpenPocket is put back the same way.

This document is the reference for the base. To start an alternative firmware, read
[ALT-FIRMWARE.md](ALT-FIRMWARE.md) first. It also covers the install, the way back and the recovery.

OpenPocket itself does not change when the base changes. `firmware/openpocket.bin` and both packages are the same
bytes as the pinned release, upstream's published v42 with the bass tuner fix, and the build fails when they are not
(section 9).

## 1. What the base holds

| Part | Files |
| --- | --- |
| The chip: start-up code, vectors, clocks, flash, USB, the second core | `firmware/bsp/`, `firmware/inc/`, `firmware/px_math.c`, `firmware/sdk/` |
| The board: pins, time, the boot guard's record, the screen, rectangles, text | `firmware/base/board.h`, `ui_text.h`, `fonts.h`, `font8x8.h` |
| The controls: the pins of the encoder, buttons, and footswitches, the ADC, the battery, the debounce | `firmware/base/controls.h` |
| The controls as events: down, hold, click, release, a detent, the volume | `firmware/base/input.h` |
| Diagnostics kept across a reset: the crash screen, the boot stages, the stacks' depth | `firmware/base/diag.h` |
| The audio's way in and out: the I2S, the block's stages for a mono or a stereo sound, the load and its bypass, the tuner | `firmware/base/audio.h`, `px_aorder.h`, `px_tuner.h` |
| A schedule for an app without one: the sound made in the interrupt, on one core; the reset after a NaN; a change made in silence | `firmware/base/audio_plain.h` |
| The start: `fm1_cstart`, the boot guard, the boot screen, the report of a start that failed, safe mode | `firmware/base/start.h`, `firmware/base/boot_screens.h` |
| The start and the main loop's duties for an app on the plain schedule | `firmware/base/loop.h` |
| USB-MIDI, the update entry for the installer, the UBOOT soft key, USB audio | `firmware/px_usb.h`, `px_uaudio.h`, `px_uaconf.h` |
| Musical MIDI: the ring read, the messages decoded for an app's map, the clock's tempo | `firmware/base/midi.h` |
| An app's own data in flash, and the flash an app may write | `firmware/base/store.h`, `firmware/base/flashmap.h` |
| Which product an image is: the descriptor, the INFO reply, an app's own editor requests | `firmware/base/product.h` |
| The button lights' driver, the cache RAM | `firmware/px_leds.h`, `firmware/px_cacheram.h` |
| The link and the build: sections, fixed regions, RAM code, the memory report | `firmware/app.ld`, `firmware/build.sh`, `tools/ramcode.py`, `tools/check_calls.py` |
| The package and the update loader | `tools/pxpkg.py`, `tools/mkloader.py`, `loader/` |

OpenPocket's own files are `firmware/openpocket.c`, `px_audio.h`, `ui.h`, `ui_colors.h`, `px_lights.h`,
`px_store.h`, `px_models.h`, `px_irs.h`, `px_editor.h`, `px_midi.h`, `px_midi_io.h`, `px_ble_midi.h`,
`firmware/ble/`, `firmware/memory.ld`, and `engine/`.

A base file that was already a file of its own, such as `px_usb.h`, stays at its path. The tests include those files
by path, and a file that does not move merges with upstream changes as before.

STARTER is the check on the boundary. It includes the base's files and none of OpenPocket's. A base file that needs a
name from OpenPocket's engine, presets, or screens stops the STARTER build.

## 2. What an app is

An app is one C file that includes the base's files in order and puts its own code between them. The firmware is one
translation unit, and every function is `static`. There are two ways to write that file.

**On the plain schedule.** The app makes its sound in the audio interrupt, on one core, and the base does the start
and the main loop. `firmware/apps/starter/starter.c` is the example, and section 3 lists what such an app declares.

**With its own schedule.** OpenPocket shares a block between two cores, so it provides the audio callbacks itself
(`px_audio.h`) and writes its own start and main loop (`openpocket.c`). The table at the end of this section is the
contract for that case.

The app's file defines these names before `base/board.h`:

| Name | Meaning |
| --- | --- |
| `AUDIO_PROBE` | `0`. `1` builds the audio test in place of the app. |
| `DSP_CPU1` | `1` starts the second core for the DSP. `build.sh` reads this line to link the second core's entry. |
| `OP_NAME` | The product's name: the boot screen, the installer. |
| `OP_VERSION_NUM`, `OP_VERSION` | The package's number (`"42"` gives `PocketX_942`) and the text on the screens. |
| `OP_APP_ID` | An app other than OpenPocket: its folder's name under `firmware/apps`. OpenPocket defines none. |
| `A_CH` | `2` when the app's sound has a left and a right side. The default is `1`. |

An app that writes its own start and loop provides these functions and calls the base in this order:

| Function | When the base calls it |
| --- | --- |
| `app_main()` | Once, from `fm1_cstart`. It does not return. |
| `a_block(data, len)` | In the I2S interrupt, with a guitar block. It calls `a_input`, then makes the sound from `a_in` to `a_out`, or starts it on another core. |
| `a_send(d, n)` | In the I2S interrupt, with an output block to fill. It finishes that sound, then calls `a_output`. |
| `a_collect()` | Before a flash write or an update. The sound in progress outside the interrupt is finished when it returns. |
| `ed_service(now)` | In every main-loop pass, before the installer's service. It takes the SysEx frames that are the app's. |

1. `lcd_init()`, then `#include "base/boot_screens.h"` in the function's body. The included statements draw the boot
   screen and the report of a failed start, and they declare `int safe`.
2. `input_init()`, `adc_pins_analog()`, `input_baseline()`.
3. `a_hold = 1`, `px_flash_boot()`, `px_usb_start()`, then `audio_io_start()`.
4. In the loop: `fm1_wdt_feed()`, `px_usb_service(now)`, and the MIDI ring read (section 7).
5. After 6 seconds in the loop: `guard.pending = 0`, `guard.failed = 0`, `op_diag.stage = ST_OK`. The start then
   counts as finished.
6. After 1.5 seconds, and never when `safe` is set: `a_hold = 0`. The app's sound starts.

Steps 4 and 5 keep the pedal recoverable. A start that does not reach step 5 counts as failed. The next start runs in
safe mode with USB up, and two failed starts in a row enter the chip's UBOOT. `base/loop.h` is these six steps as
three functions, so an app on the plain schedule cannot get them wrong.

## 3. What an app on the plain schedule declares

Each row is one declaration. The app supplies the left column, and the base does the rest.

| The app supplies | In | The base's part |
| --- | --- | --- |
| Its memory regions: `RAM`, `POOL`, `RAMTEXT` | `memory.ld` | `app.ld` checks them and places the sections (section 4). |
| Its sources and its RAM code | `app.sh` | `build.sh` compiles, moves the listed functions to RAM, links, packages. |
| Its engine: `app_sound(in, out)` and `app_sound_reset()` | its C files | `base/audio_plain.h` calls them in the interrupt, measures the load, bypasses an overload. |
| A stereo sound: `#define A_CH 2` | its C file | `base/audio.h` treats the two sides alike in every output stage (section 5). |
| What its controls do: `app_turn`, `app_button`, `app_master`, and each button's hold time | its C files | `base/input.h` reads the pins and the ADC and gives events (section 6). |
| Its screen | its C files | `base/board.h` and `ui_text.h` draw rectangles and text. The app draws its own tiles. |
| Its MIDI map: `#define APP_MIDI name` | its C files | `base/midi.h` reads the ring and decodes the messages (section 7). |
| Its data in flash: a magic number, a format version, one block | its C files | `base/store.h` keeps two copies in the app's own flash range (section 8). |
| Who it is: `OP_APP_ID`, `OP_NAME`, `OP_VERSION` | its C file | `base/product.h` puts the descriptor in the image and answers INFO (section 10). |
| Its own editor requests: `#define APP_EDITOR name` | its C files | `base/product.h` passes it the frames that are not INFO. |
| Its start and its loop: `app_init()`, `app_step(now)` | its C files | `base/loop.h`: `base_begin()`, `base_go()`, `base_loop(now)`. |

## 4. Memory regions

`firmware/app.ld` holds the regions that are the chip's or the boot chain's. They are the same for every app.

| Region | Address | Size | Use |
| --- | --- | --- | --- |
| `XIP` | `0x02000120` | 581,564 | Code and constants, run from the flash. |
| `LOWRAM` | `0x01C00000` | 24,576 | Section `.bss.lo.*`, set to zero at the start. |
| `CRAM` | `0x01F28000` | 28,672 | The cache RAM, section `.cram*`. |
| Stacks | `0x01C74000` | 32,768 | Two stacks with guard bands. |
| `NOINIT` | `0x01C7C000` | 15,696 | Section `.noinit`. It survives a reset. |

The app declares the rest in a file `memory.ld`: the 432 KiB of SRAM from `0x01C08000` up to the stacks, as three
regions in this order:

| Region | Use |
| --- | --- |
| `RAM` | `.data` and `.bss`. |
| `POOL` | Section `.pool`, the engine's memory (`OP_POOL` in the source). The start does not set it to zero. Section `.pool.tail.*` follows it and is set to zero. |
| `RAMTEXT` | Code that runs from RAM. Section `.bss.hi.*` uses its free end and is set to zero. |

`app.ld` includes `memory.ld` and refuses the link when the three regions overlap, are out of order, or reach the
stacks. It also refuses a `RAMTEXT` that the flash code cannot call. `firmware/memory.ld` is OpenPocket's.

| | `RAM` | `POOL` | `RAMTEXT` |
| --- | --- | --- | --- |
| OpenPocket | 96 KiB | 248 KiB | 88 KiB |
| STARTER | 64 KiB | 320 KiB | 48 KiB |

`build.sh` prints how much of each region the image uses. It reads the regions from the linker's map, so the report
follows the app's `memory.ld`. STARTER uses 8 KiB of its `POOL` and 4.6 KiB of its `RAMTEXT`.

Code runs from RAM when its function is named in a list. `tools/ramcode.py` renames the function's section, and the
code itself does not change. The base's audio path is in the list for every app. The app adds its own names in
`app.sh`.

## 5. Audio

A block is 128 frames at 44.1 kHz. The guitar is mono: the converter's right input carries noise only. The base
turns each guitar block into `a_in`, and it sends `a_out` to the output through these stages: the NaN guard, the
mutes, the change fade, the MASTER volume, the soft knee.

An app's sound is mono by default, and the base sends it to both outputs alike. An app that defines `A_CH 2` fills a
left block and then a right block in `a_out`. Every stage then treats the two sides alike, and the I2S block gets one
side on each channel. The pedal's converter and headphone path are stereo. Whether the main output jack carries two
channels is not known.

`base/audio_plain.h` adds two things for an engine that keeps state:

- **A NaN.** The base silences the output when the app's sound gives a NaN or an infinity. `a_nan_poll()` then calls
  `app_sound_reset()` with the interrupt held and fades the sound back in. `base_loop` calls it.
- **A change in silence.** `a_change(fn)` fades the output out over 5.8 ms, runs `fn` with the interrupt held, and
  fades the output back in. Use it to swap an effect, change an order, or clear a buffer. A setting that only moves
  needs no such call: the engine glides to it.

The base's schedule uses one core. The schedule that shares a block between two cores is OpenPocket's, in
`px_audio.h`.

## 6. Controls

`base/controls.h` gives the pins (`PA_FX`, `PA_HOME`, `PA_FS_UP`, `PA_FS_DN`, `PH_PUSH`, `PH_MOD`, `PH_AMP`, the
encoder on `PC0` and `PC1`), the idle state taken at the start, the debounce, the analog keypad for CAB, DLY, and REV,
the encoder's table, the MASTER knob's smoothing, and the battery gauge. It reads. It does not decide what a press
means.

`base/input.h` turns that into events for an app. `in_poll(now)` runs in every main-loop pass and calls the app:

| Call | Meaning |
| --- | --- |
| `app_turn(dd, fast)` | The encoder moved one detent. `fast` is set when the detent before was less than 45 ms ago. |
| `app_button(b, IN_DOWN, held)` | Button `b` went down. Every press gives this, at once. |
| `app_button(b, IN_HOLD, held)` | It has been down for its hold time. The app gives each button's time to `in_start`. |
| `app_button(b, IN_CLICK, held)` | It came up before its hold time, or it has none. |
| `app_button(b, IN_RELEASE, held)` | It came up after a hold. |
| `app_master(vol)` | The MASTER knob is at `vol`, 0 to 100. |

`held` is the set of buttons that are down, so the second button of a pair sees the first one. The buttons are
`IN_FSUP`, `IN_FSDN`, `IN_PUSH`, `IN_HOME`, `IN_FX`, `IN_MOD`, `IN_AMP`, `IN_CAB`, `IN_DLY`, and `IN_REV`.

OpenPocket polls the pins in its own main loop (`openpocket.c`) and times its gestures in `ui.h`.

## 7. MIDI

`usb.c` puts the host's Program Changes, Control Changes, clock, and transport bytes into a ring. SysEx does not take
this road.

**Every app must read the ring.** While it is nearly full, `usb.c` leaves the host's packets unread, and the
installer's SysEx comes in the same packets. A pedal whose ring nobody reads stops answering the installer after 41 to 48
messages from a host. `midi_poll()` in `base/midi.h` reads it, and `base_loop` calls `midi_poll()` in every pass.

An app with a MIDI map names it with `#define APP_MIDI name` and gets each message as a `midi_msg_t`: its type
(`MIDI_PC`, `MIDI_CC`, `MIDI_CLOCK`, `MIDI_START`, `MIDI_STOP`), its channel, and its data, with the millisecond it
arrived. `midi_tempo` turns the clock into a quarter note's time. Without `APP_MIDI` the messages are dropped.

OpenPocket has its own reader and map, and its own copy of the decoder (`px_midi.h`, `px_midi_io.h`).

## 8. An app's data in flash

| Range | Whose |
| --- | --- |
| `0x04000..0xA3000` | The firmware. An update rewrites it. |
| `0xC0000..0xE0000` | OpenPocket's NAM models. |
| `0xE0000..0xE5000` | An update's staging area. |
| `0xE5000..0xEF000` | An app's own data (`base/store.h`), ten sectors of 4 KiB. |
| `0xF0000..0xFC000` | OpenPocket's presets and cabinet IRs. |

An app never writes OpenPocket's ranges. They must be there, untouched, when OpenPocket is installed again. The
base's flash guard refuses a write outside the update's staging area and the range the app declares with
`OP_FLASH_OWN`. An app that uses the store declares `BS_FLASH_OWN`.

`base/store.h` keeps one block of the app's data, at most 19,184 bytes, with the app's own magic number and format
version:

- `bs_load(magic, data, max, &len, &ver)` at the start. It leaves `data` untouched when it finds no whole copy.
- `bs_save(magic, ver, data, len)`. It mutes the sound for the write and reads the copy back.

There are two copies of five sectors, written in turn. A copy's header goes last, with a sequence number and a CRC16.
A power cut during a save leaves the copy before it.

The last 256 bytes of every sector stay erased. The boot code and every update loader look there, at each 4 KiB
boundary minus 256, for the record of an update in progress, and a loader erases a sector that ends like one.
OpenPocket's loader skips OpenPocket's ranges by address. It does not know this range. Those erased bytes are what
keeps an app's data through a stay on OpenPocket.

Nothing is written before the first save. On a pedal that came from the official firmware this range holds part of
the official sound bank (one amp model and eight cabinets), which OpenPocket's own areas already overlap elsewhere.
The first save of an app overwrites it.

## 9. Build both products, and the release pin

```
node tools/op.mjs all             # or: make all
```

`all` builds OpenPocket, then each app of `firmware/apps`, then runs the host tests with the packages it just built.
The parts can be run alone:

| Command | Result |
| --- | --- |
| `node tools/op.mjs package` | OpenPocket: `firmware/openpocket.bin`, `docs/firmware/*.fwsc`, `manifest.json`, `build/SHA256SUMS`. It ends with the release pin. |
| `node tools/op.mjs app starter` | STARTER: `build/apps/starter/starter.bin`, `starter-PocketX_901.fwsc`, `starter-BlackBox_901.fwsc`, `manifest.json`, `SHA256SUMS`. |
| `node tools/op.mjs test --built` | The host tests, also with those packages. |

An app's outputs go to `build/apps/<name>/`. The OpenPocket build's outputs are not touched, and the public installer
never offers an app.

**The release pin.** `tests/release.sha256` holds the SHA-256 of OpenPocket's image, both packages, and the
manifest. They are the sums of upstream's published v42 packages with the bass tuner fix (the tuner reads a bass's
low strings). `package` ends with `tests/release_pin.sh`, which fails when a sum differs. The build is deterministic: CI and a local build give the same bytes.

The same sums mean that nothing in OpenPocket needs a check on a pedal. A change to a base file changes every app,
so run `package` after one. A change that only moves code, or that adds lines for a case OpenPocket does not
compile, leaves the image the same bytes.

To pin a new OpenPocket release after a merge of upstream's next version, compare the packages with upstream's
published ones, then copy `build/SHA256SUMS` over `tests/release.sha256` in the same commit. A change made to
OpenPocket on purpose, such as the bass tuner fix, is pinned the same way once its own tests pass.

## 10. Which product an image is

Every image on the base reports the identity `PocketX_9NN` or `BlackBox_9NN` in the update handshake. That is on
purpose. An installer of any age finds the pedal by it and can install OpenPocket over it. The identity does not say
which product runs. Three other things do.

| Where | OpenPocket | An app |
| --- | --- | --- |
| In the image | No product descriptor. | A descriptor found by its mark `OP-APP:`: id, name, version, what it can do. |
| In the package | `PocketX_942.fwsc`. The manifest has no `app`. | `starter-PocketX_901.fwsc`. The manifest's `app` holds the descriptor. |
| On the pedal | INFO answers protocol 1 to 4. | INFO answers protocol 0, then the descriptor. |

INFO is the first request of the editor protocol, `F0 7D 4F 50 01 F7` ([EDITOR-PROTOCOL.md](EDITOR-PROTOCOL.md)).
OpenPocket has answered it since v32. No OpenPocket answers protocol 0, and the published web editor refuses it, so
an old editor does not open on an app.

`tools/pxpkg.py` reads the descriptor from the image it packages. It refuses to package an app's image as
OpenPocket, and OpenPocket's image as an app.

The capabilities come from what the app includes: `stereo` (`A_CH 2`), `usb-audio` (`OP_UAUDIO`), `midi`
(`APP_MIDI`), `store` (`base/store.h`). The descriptor also holds the protocol number of the app's own editor
requests, or 0.

The tools use this as follows:

- **The installer page** names the product a pedal runs. It offers an app only with `index.html?app=<name>`, in a
  card of its own beside OpenPocket. While a pedal runs an app, the button of step 2 reads "Back to OpenPocket".
- **The flasher** installs an app only with `--app <name>`. It names the product a pedal runs, checks after an
  install that the pedal runs the product that was installed, and ends with the way back.
- **The web editor** says which product answered and how to put OpenPocket back. It does not open.
- **`node tools/op.mjs log`** names the product.

The USB device's name is "OpenPocket" for every app, because the installer finds the pedal by that name.

## 11. The simulator

`node tools/op.mjs sim --app <name>` runs an app on this computer ([SIMULATOR.md](SIMULATOR.md)). It compiles the
app's own C file and its engine with `tests/host` as the chip. Three files are stood in for: `base/board.h`,
`px_usb.h`, and `base/start.h`. Every other part of the base is the pedal's own code: the start and the loop, the
controls from the pins up, the audio stages, the store, MIDI, and INFO.

The simulator gives the app's screen, its controls, MIDI and SysEx, its flash, and the level of the two outputs for a
test tone. It does not play the app's sound in the browser. The WebAssembly engine of the OpenPocket simulator is
OpenPocket's.

## 12. What the base leaves out

- **OpenPocket's own copies.** OpenPocket still has its own main loop, button polling, MIDI reader, preset store, and
  editor frames. It does not use `base/loop.h`, `input.h`, `midi.h`, `store.h`, or `product.h`. Moving it onto them
  changes its compiled code, and a changed image needs a check on a pedal (section 13).
- **Two cores.** The base's schedule uses one core.
- **The web editor for an app.** The editor edits OpenPocket only. An app's own requests reach the app, and no page
  speaks them yet.
- **The app's sound in the browser.** See section 11.
- **The Bluetooth test image and the test channel.** `OP_BLE` and `OP_CHANNEL` build OpenPocket only.
- **USB audio in STARTER.** The base has it (`OP_UAUDIO`). STARTER does not turn it on.

## 13. Compromises

They exist so that OpenPocket's compiled code stays the same.

`base/boot_screens.h` holds statements, not functions, and OpenPocket's `app_main` includes it in its body. As
functions, the same lines changed the stack layout of OpenPocket's `app_main`. `base/loop.h` wraps them in a function
for other apps.

The encoder's and the buttons' polling stays in OpenPocket's main loop for the same reason, and `base/input.h` is a
second copy of that logic for other apps. The same holds for the MIDI decoder and the SysEx reply helpers.

In `base/audio.h` the right side's lines are under `#if A_CH == 2`. A loop over the channels, written once, made the
compiler emit different code for OpenPocket's one channel.

These copies can be joined when a changed OpenPocket image can be checked on a pedal.

## 14. Merge upstream changes

OpenPocket's upstream does not have `firmware/base/`, the apps, or the product identity.

- A file that kept its path merges as before.
- A change upstream to lines that moved conflicts as "deleted by us". Apply the change by hand to the line's new
  file.

  | Moved from | Lines | To |
  | --- | --- | --- |
  | `openpocket.c` | The chip's includes, the pins, the screen, the 8x8 text | `base/board.h` |
  | `openpocket.c` | The controls' pins, the ADC, the battery, the keypad, the debounce, `QDEC`, the MASTER knob's smoothing | `base/controls.h` |
  | `openpocket.c` | The crash screen, `op_diag`, the boot stages, `stack_free`, `rst_names` | `base/diag.h` |
  | `openpocket.c` | The boot screen and the report of a failed start, from `app_main` | `base/boot_screens.h` |
  | `openpocket.c` | `fm1_cstart` | `base/start.h` |
  | `px_audio.h` | The I2S driver's glue, the audio's variables, `dsp_out`, `dsp_load`, the tuner, the input's conversion (now `a_input`), the output's copy (now `a_output`), the I2S start (now `audio_io_start`), `audio_volume` | `base/audio.h` |
  | `px_audio.h` | `b_num`, `b_str`, `b_fix2` | `base/board.h` |
  | `px_usb.h` | The three flash ranges of `FL_RANGE_OK`, the `OTA_SCRATCH` lines | `openpocket.c` |
  | `app.ld` | The regions `RAM`, `POOL`, `RAMTEXT` | `firmware/memory.ld` |

- The version line, the `app_main` loop, and the lists of RAM code in `build.sh` did not move. A release upstream
  merges into them without a conflict.
- These files that upstream also edits have additions for the second product. A conflict there is in the added
  lines, and upstream's side is kept as it is: `docs/index.html`, `docs/pxota.js`, `docs/pxedit.js`,
  `docs/editor.js`, `docs/edcore.js`, `tools/flash.mjs`, `tools/log.mjs`, `tools/op.mjs`, `tools/pxpkg.py`,
  `tools/sim.mjs`, `tools/serve.py`, `tests/devsim.c`, `tests/fixtures.py`, `tests/run_tests.sh`,
  `tests/test_usb_audio.c`, `tests/test_page.mjs`, `tests/test_editor_page.mjs`, `tests/test_log.mjs`,
  `tests/test_op.mjs`, `Makefile`, and `.github/workflows/build.yml`.
- A new file upstream is OpenPocket's until someone moves it.

After a merge of a new upstream release, `node tools/op.mjs package` fails at the release pin, because the version
changed. Compare the packages with upstream's published ones for that release, then pin them (section 9).
