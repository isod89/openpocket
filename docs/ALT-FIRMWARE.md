# An alternative firmware

An alternative firmware is another product for the same pedal, built from this tree on the base firmware
([BASE-FIRMWARE.md](BASE-FIRMWARE.md)). It is not OpenPocket and not a version of it. The pedal runs one firmware or
the other. It is installed with the same installer, and OpenPocket is put back the same way.

STARTER (`firmware/apps/starter`) is a small alternative firmware that exists to be copied. This document says how to
start your own from it, how it is installed and replaced, and what the recovery paths are.

Nothing here has run on a pedal yet. Section 7 lists what was checked without one and what still needs one.

## 1. What STARTER is

STARTER turns the mono guitar into a left and a right side, with three settings, eight presets, and the pedal's
controls. It is small on purpose. It uses every part of the base that a larger product needs.

| File | Content |
| --- | --- |
| `starter.c` | The pedal's side: what the app declares to the base, and the order of the base's parts. About 50 lines. |
| `app.h` | The app itself: its sounds, presets, screen, controls, MIDI map, and its own editor requests. |
| `engine.c`, `engine.h` | Its engine: LEVEL, WIDTH (the right side comes up to 30 ms later), PAN. |
| `memory.ld` | Its three memory regions. |
| `app.sh` | Its sources and the functions that run from RAM, for `firmware/build.sh`. |

| Control | What it does in STARTER |
| --- | --- |
| The knob | The selected setting. Turned quickly, it goes in steps of 5. |
| PUSH | The next setting. Held for 0.8 s, it saves the sound to its slot. |
| MOD, AMP, CAB | LEVEL, WIDTH, PAN. |
| HOME | The effect off and on. |
| The footswitches | The preset before, the preset after. |
| MASTER | The volume. |
| MIDI | Program Change 0 to 7 picks a preset. Control Change 7 is LEVEL, 12 WIDTH, 10 PAN, 80 off and on. The clock's tempo is shown. |

## 2. Start your own

1. Copy the folder and rename its C file. The folder's name is the product's id: lowercase letters, digits, and `-`.

   ```
   cp -R firmware/apps/starter firmware/apps/mine
   mv firmware/apps/mine/starter.c firmware/apps/mine/mine.c
   ```

2. In `mine.c`, set `OP_APP_ID` to the folder's name, `OP_NAME` to the product's name, and `OP_VERSION_NUM` to `"01"`.
   The packager refuses an image whose `OP_APP_ID` is not its folder's name.
3. In `app.sh`, set `APP_MAIN=mine.c`.
4. In `app.h`, change `AP_MAGIC` to four letters of your own. The magic number is what keeps your data apart from
   another app's in the same flash range.
5. Build it, and run it on this computer:

   ```
   node tools/op.mjs app mine
   node tools/op.mjs sim --app mine
   ```

   The first command needs Docker ([BUILDING.md](BUILDING.md)). The second needs only a C compiler. It prints the
   address of a page with the app's screen and the pedal's controls.
6. Replace `engine.c` and `app.h` with your own, a part at a time. Keep `mine.c` short: it is the list of what the
   app declares to the base. Section 3 of [BASE-FIRMWARE.md](BASE-FIRMWARE.md) has that list.
7. Copy `tests/test_app_sim.mjs` for your app and change it as the app changes. It drives the simulator over HTTP.

Rules that keep the pedal recoverable and OpenPocket's data safe:

- Call `base_begin()`, `base_go()`, and `base_loop(now)` as STARTER does. They bring USB up before your sound runs,
  feed the watchdog, tell the boot guard that the start finished, and read the MIDI ring.
- Write flash only through `base/store.h`. The base refuses a write outside the range the app declares.
- Do not write `0xC0000..0xE0000` or `0xF0000..0xFC000`. OpenPocket's NAM models, presets, and cabinet IRs are there.
- Clear your engine's state at the start. The `POOL` region is not set to zero.
- List your per-sample functions in `app.sh` so that they run from RAM.
- Check what the flash held when you load it. `app_init` in STARTER shows how.

## 3. How a product is told from OpenPocket

Every image on the base reports the identity `PocketX_9NN` or `BlackBox_9NN`. An installer of any age finds the
pedal by it. The identity does not say which product runs. The product does, in three places:

- **The image** holds a product descriptor: id, name, version, and what the app can do.
- **The package** is named after the app (`mine-PocketX_901.fwsc`), and its manifest has an `app` entry.
- **The pedal** answers the INFO request with protocol 0, then the descriptor. OpenPocket answers protocol 1 to 4.

Section 10 of [BASE-FIRMWARE.md](BASE-FIRMWARE.md) has the details. The boot screen shows `OP_NAME` and the version.

## 4. Install, and go back

Install only on a pedal you can afford to recover. Read section 6 first.

**With the installer page:**

```
node tools/op.mjs serve app mine          # or: make serve-app APP=mine
```

The page opens as `index.html?app=mine`. Step 2 still installs OpenPocket. The alternative firmware has a card of
its own below it, with its name, what it replaces, and the way back. While the pedal runs an alternative firmware,
step 1 names it, and the button of step 2 reads "Back to OpenPocket".

The public installer never offers an alternative firmware. Opened on a pedal that runs one, it shows the pedal as an
OpenPocket of that number and offers to install OpenPocket. That is the way back from any computer.

**With the flasher:**

```
node tools/op.mjs flash --dry-run --app mine          # what would replace what; writes nothing
node tools/op.mjs flash --confirm-flash --app mine    # install the alternative firmware
node tools/op.mjs flash --confirm-flash               # the way back: install OpenPocket
```

The flasher installs an alternative firmware only with `--app`. Without it, a manifest or a package of an
alternative firmware is refused before anything is sent. After an install the flasher asks the pedal which product
it runs, and it reports a mismatch.

**What the flash keeps.** An install rewrites the firmware's area only. OpenPocket's presets, NAM models, and cabinet
IRs stay through a stay on an alternative firmware. The alternative firmware's own data stays through a stay on
OpenPocket.

**The web editor** edits OpenPocket only. On an alternative firmware it says which product answered and how to put
OpenPocket back.

## 5. Going back to the official firmware

Step 3 of the installer page works from an alternative firmware as it does from OpenPocket.

The official firmware keeps its sound bank in `0xBF000..0xFD000`. OpenPocket's own areas are inside that range, and
so is an alternative firmware's data (`0xE5000..0xEF000`: one amp model and eight cabinets of the official bank). A
pedal that has saved a sound on either firmware needs its official sound bank restored after it goes back.
[RESCUE.md](RESCUE.md) has the steps for the BlackBox V21.

## 6. Recovery

An alternative firmware on the base has the same recovery paths as OpenPocket, because the start, the boot guard,
USB, and the update entry are the base's code, not the app's.

| What happened | What recovers it | Needs |
| --- | --- | --- |
| The app's sound crashes or overloads | The base bypasses a sound that stays above 85 % load. After a crash the next start is in safe mode: the app's sound is off, the guitar passes, USB answers. Install OpenPocket from there. | The installer or the flasher |
| The pedal restarts in a loop, and answers USB for a moment each time | Rescue mode: the installer page's "Rescue and advanced options", or `node tools/op.mjs flash --rescue --confirm-flash`. It installs OpenPocket the moment the pedal answers. | The installer or the flasher |
| Two starts in a row did not finish | The boot guard enters the chip's UBOOT state by itself. | The UBOOT rescue tool |
| An install was interrupted | The pedal stays in update mode. Press Resume on the installer page, or run `flash --rescue --package FILE`. | The installer or the flasher |
| The firmware does not answer USB at all | The chip's UBOOT state. On a BlackBox: `make rescue`, then `make rescue-write` ([RESCUE.md](RESCUE.md)). That document covers the BlackBox only. On a Pocket X the documented route is the SD card (README.md, "Install"). | A Mac or Linux computer, libusb, the official package; or the SD-card kit |

The UBOOT rescue tool (`tools/uboot_rescue.py`) does not care which product was on the pedal. It writes the official
firmware's application area and leaves the rest of the flash, so OpenPocket's data and the app's own data stay. It
refuses an image whose first `0x4000` bytes differ from the pedal's, so it writes the official package, not an
OpenPocket package or an app's. After it, install OpenPocket or the app again with the installer.

The rescue image is `fm1-1.bin`, the backup of one pedal's whole flash, made before its first install of OpenPocket
(README.md, "Install"). `make appimage DUMP=fm1-1.bin` makes `fm1-300.bin` from it: the same flash with OpenPocket as
its firmware. Either one is a 1 MiB image written through the SD-card kit (`flash/`, fm1buddy on an ESP32-S3). They
cover a pedal on an alternative firmware as they cover one on OpenPocket, because they do not depend on what the
flash held. The first restores the official firmware with its sounds, the second OpenPocket. Both also replace the
alternative firmware's own data with what the backup held there. A rescue image belongs to one pedal: do not write
another pedal's.

Before the first install of an alternative firmware on a pedal:

1. Have the official package for that pedal on the computer.
2. Run `make rescue` once on a BlackBox, with the pedal in its UBOOT state (the installer's "Enter UBOOT mode"
   button). It is read-only, it saves a backup of the whole flash, and it shows that the tool reaches that pedal.
3. Keep the SD-card backup at hand, as for the first install of OpenPocket.

What an app can break: its own sound, screen, and controls. What it cannot break without changing the base: the
start, the boot guard, the update entry, and the flash outside its own range. The exceptions are a fault before USB
is up and a fault that also stops the main loop from answering USB. The boot guard and the UBOOT state cover those.

Known cases that nothing recovers are the same as for OpenPocket: a damaged flash head, and the Pocket X radio
calibration that the first OpenPocket install overwrites.

## 7. What was checked, and what needs a pedal

Checked without a pedal, by `node tools/op.mjs all`:

- The base's parts, each by a host test that runs the pedal's own code: the audio stages for a mono and a stereo
  app, the controls from the pins to the events, the MIDI ring and decoder, the store with a power cut at every step
  of a save, the plain schedule's NaN reset and silent change.
- STARTER in the simulator, from its own C files: its controls, its sound at the two outputs, MIDI, its store, safe
  mode, the boot guard's mark.
- Both products on the simulated update pedal, which runs the pedal's own update code and loader: the package the
  build made for STARTER installs over OpenPocket, the flash then holds `starter.bin` byte for byte, and OpenPocket's
  release package installs back. OpenPocket's data and the app's own data are kept through both.
- The installer page and the web editor in a browser against those simulated pedals.
- The UBOOT rescue tool against a simulated pedal that holds an alternative firmware.
- OpenPocket's image and packages: the same bytes as the release.

Not checked, because it needs a pedal: everything in section 8.

## 8. Checks on a pedal

Flashing a pedal is the owner's call. Run this list once on a Pocket X and once on a BlackBox, in order. Stop at the
first step that fails and note the screen. OpenPocket itself needs none of this: its image is the release's bytes.

Before you start:

1. Have the pedal's official package and its SD-card backup at hand (section 6).
2. Run `node tools/op.mjs all`. It must exit with 0, and its output must hold the release pin's line and "all product
   tests passed".
3. With the pedal on OpenPocket, save one preset under a name you will recognise, and note it.

Install:

4. Run `node tools/op.mjs flash --dry-run --app starter`. It names the pedal's firmware as OpenPocket and STARTER
   v01 as what would replace it. It writes nothing.
5. Run `node tools/op.mjs serve app starter`. On the page, press Connect. Step 1 shows OpenPocket. The card
   "Alternative firmware" shows STARTER v01.
6. Press "Install STARTER v01". The pedal shows UPDATE, restarts, and shows STARTER v01 on its boot screen. The card
   says that the pedal now runs STARTER v01.

On STARTER:

7. Play. The guitar reaches the output. MASTER changes the volume. The screen shows a load of a few percent.
8. Press the right footswitch: preset 2, WIDE. With headphones the guitar is wider than on preset 1. Listen at the
   output jack too, and note whether it carries two channels.
9. Press CAB, turn the knob left to PAN 0: the sound is on the left only. Turn it back to 50.
10. Press HOME: the effect is off. Press it again.
11. Hold PUSH for a second: SAVED shows. Switch the pedal off and on: it starts on that preset, with PAN as saved.
12. Send a Program Change 3 and a Control Change 7 from a computer: the preset and LEVEL follow.
13. On the installer page, press Connect again. Step 1 shows "STARTER v01: an alternative firmware, not OpenPocket".
    The button of step 2 reads "Back to OpenPocket".
14. Open the public installer in another tab and press Connect. It shows the pedal as OpenPocket v1 and offers to
    install OpenPocket. Do not install from there. Close the tab.
15. Open `editor.html` and press Connect. It says that the pedal runs STARTER v01 and does not open.

Safe mode and the boot guard:

16. Switch the pedal off, on, and off again within 3 seconds, then on. The record of an unfinished start is kept in
    RAM across a reset. Whether it survives this pedal's power switch is not known. If it does, the screen shows
    LAST START FAILED, then STARTER with SAFE MODE, the guitar passes, and the installer page still finds the pedal.
    If the pedal starts as usual, note that and go on.
17. Switch it off and on normally. STARTER starts as usual.
18. On the installer page, open "Rescue and advanced options" and press "Enter UBOOT mode". The pedal shows UBOOT
    (USB). On a BlackBox, run `make rescue` now: it is read-only and must end with its summary. Switch the pedal off
    and on.

The way back:

19. On the installer page, press "Back to OpenPocket". The pedal restarts into OpenPocket. Step 1 shows OpenPocket.
20. The preset saved in step 3 is there, and so are the NAM models and the cabinet IRs.
21. Install STARTER once more with `node tools/op.mjs flash --confirm-flash --app starter`. It starts on the preset
    saved in step 11: its own data stayed through the stay on OpenPocket.
22. Go back with `node tools/op.mjs flash --confirm-flash`. It ends with "the pedal reports PocketX_942 (OpenPocket
    v42)" or the BlackBox's line.

Rescue, on one pedal only, after everything above has passed:

23. With STARTER installed, run `node tools/op.mjs flash --rescue --confirm-flash` and then plug the pedal in. It
    installs OpenPocket the moment the pedal answers.
