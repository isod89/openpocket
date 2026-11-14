# What's new in OpenPocket v42: a new editor, reworked pedal controls, USB audio

v42 brings together four pull requests from the community. Thank you, **Vince Speelman
([VinSpee](https://github.com/VinSpee))**, for #23, #24 and #25, and **Ivan Shevchuk
([Testing7B](https://github.com/Testing7B))**, for #26.

The sound engine is unchanged from v41: every output sample is the same.

## 1. The pedal's controls and screens (pull request #24)

A design review of the pedal's interface. [SOUNDS-AND-CONTROLS.md](SOUNDS-AND-CONTROLS.md) has the full list.

- **Both footswitches open the tuner**, as the panel always said. The first one still changes the preset at once; the
  second one, within about 0.15 s, changes it back and opens the tuner.
- **The tuner reads a bass.** Its lowest note was 61 Hz, so a bass's E (41 Hz) and A (55 Hz) strings showed nothing or
  a wrong note. It now reads from about 28 Hz (a five-string bass's low B is 31 Hz); a guitar reads as before.
- **PUSH on PLAY opens the block at once.** It used to wait 350 ms for a possible second click. A double click still
  starts MOVE.
- **An unsaved edit is kept** when a footswitch moves on: come back to that preset and the edit is there, still marked
  `EDITED`. One edit is kept at a time.
- **The preset list**: HOME on PLAY lists the presets by name; the knob browses without loading, PUSH loads.
- **NAM MODEL and USER IR move one model a click**, skip the empty slots and show their place (`MODEL 6/14`). Presets do
  not change.
- On a block page the knob goes down the page in the order drawn: ON, the model, the settings. FX steps through its
  blocks in the order of the preset's chain.
- The corner reads `DSP 42%`, amber above 80 %, red when a block ran late. The tuner page shows the tuner only, with an
  arrow toward in tune.
- **Diagnostics pages** (keep holding HOME past the tuner): bars and meters for the load, each core, the costliest blocks,
  the inputs and the battery, every figure written beside its picture. Take a photo of both pages for a bug report.
- Smaller things: a copy saved to another slot keeps its name (the page says so), RESTORE FACTORY says `PUSH: RESTORE`
  and does nothing on a factory sound, SYN's page is SYNTH, USER IR's cuts show `HZ`, a failed start or update says what
  to do first.
- The installer: rescue mode moves to the advanced section, and a finished install names the version and links to the
  editor and these notes.
- v41's `CORE 2 OFF` line stays on the MIDI page: it says when the second core was set aside, and why.

## 2. The pedal as a USB sound card (pull request #25)

With the new **USB AUDIO** setting on (the MIDI page, hold FX; or the editor's MIDI section), a computer sees a USB sound
card named OpenPocket beside the MIDI port, with no driver: stereo, 16 bit, 44.1 kHz, both ways.

- **Recorded:** the pedal's sound before MASTER, so the knob does not change a recording.
- **Played:** what the computer plays comes out of the pedal's output, at MASTER's level. It is not in the recording.
- A change takes the pedal off USB for a second; it comes back as the other device.
- **Off by default.** Off, the pedal is the same USB device as before, with the same sound and DSP load. On, it costs
  about 1 % of the DSP.
- Tried on a BlackBox with macOS 26 only (10 minutes without a gap, no frame lost). Windows, iOS, Linux and older macOS
  are still to be tried. [USB-AUDIO.md](USB-AUDIO.md) has the details.

## 3. The web editor (pull request #23)

The [web editor](https://isod89.github.io/openpocket/editor.html) is now four sections:

- **Presets** show what each one is made of, can be searched, copied and moved, and every write says what it replaces
  and can be undone.
- **Sound** shows the chain in one line, uses lists and buttons for settings with named values, works from the keyboard,
  adds Compare, and keeps the DSP load in the top bar.
- **Models** takes several files at once, shows which presets use a slot, and renames or moves a capture with its
  presets following. **TONE3000** has its own panel with your favorites and the trending list.
- **MIDI** is new: the pedal's Control Change numbers named for the current preset; controls learned from another MIDI
  device, which the page passes on while it is open; a monitor; and the USB AUDIO setting.

## 4. BlackBox V21: check and restore the factory sounds (pull request #26)

The official BlackBox V21 download installs the firmware but contains no sound bank, and OpenPocket uses part of the
flash where the stock sound bank lives. After going back to V21, the installer can now **verify the factory sounds**:
all 20 amps, 20 cabinets and 80 presets, over MIDI. If they are missing, [RESCUE.md](RESCUE.md#blackbox-v21-factory-sounds)
has a recovery tool for macOS that restores only the sound bank from the M-EFCS app's own files, after a full flash backup.

## Please try on the pedal

Not yet tried on this exact build:

- the new gestures: both footswitches for the tuner, PUSH opening at once, the double click for MOVE, holding HOME;
- the preset list, the kept edit, the diagnostics pages on the real screen;
- USB audio on Windows;
- the new editor with real USB (reading all 52 presets) and a real TONE3000 sign-in.

## For developers

- Editor protocol 4: one new command, `USB_AUDIO` (28). Editors for protocols 1 to 3 work as before.
- The simulator's API gains `press/fx_long` and `press/feet`, and the modes `midi`, `list` and `diag`.
- The editor's code is in modules (`editor.js`, `edcore.js`, `edpresets.js`, `edsound.js`, `edmodels.js`, `edmidi.js`,
  `pxmidi.js`, `pxlib.js`), with `tests/test_editor_logic.mjs`.
- The Bluetooth test image (`node tools/op.mjs ble`) does not link: it was already about 1.2 KB over its flash region in
  v41, and the new screens add about 6 KB. The release image is not affected.
