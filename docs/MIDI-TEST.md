# MIDI over USB-C: hardware test plan

For the first run of **OpenPocket v37 MIDI TEST** on a real pedal. Nothing here was tried on hardware yet: the pedal is the only
way to check the USB behaviour and the sound. The design is in [MIDI.md](MIDI.md). Takes about 45 minutes. Tick as you go and
send back the "report" block at the end.

**Do this on one pedal first.** The Donner Pocket X is the better first choice: the M-VAVE BlackBox install is still being
tested (issue #1: a restart loop after power cycles in some versions, rescue mode on the installer page is the cure).

## 0. Before you start

- [ ] Know your way back (section 7). Have your official firmware file for the pedal at hand, and the SD-card recovery
      (`flash/`, `fm1-1.bin`) if you made one.
- [ ] Chrome or Edge on a computer, a USB **data** cable, the pedal on and charged.
- [ ] A MIDI monitor: macOS: Audio MIDI Setup (Window > Show MIDI Studio) and the free MIDI Monitor; Windows: MIDI-OX or
      Bome MIDI Translator; Linux: `aseqdump -l` then `aseqdump -p "OpenPocket"`.
- [ ] A DAW or a MIDI controller that can send Program Change and Control Change (Ableton, Logic, Reaper, GarageBand with a
      MIDI app, or a free web tool such as the Web MIDI Monitor and sender pages).
- [ ] The package: a build of the pull request gives `docs/firmware/midi-test/` (`node tools/op.mjs package-test`), or the CI
      artifact. Serve it with `node tools/op.mjs serve` (offline) and open `http://localhost:8765/index.html?channel=midi-test`,
      or use the page you were given.

## 1. Install the test build

- [ ] Open the installer with `?channel=midi-test`. A warning banner says "TEST CHANNEL" and the package line says `v37 (midi-test)`.
- [ ] Connect the pedal, check that it is found, press Install, wait for "Done". Do not unplug while it works.
- [ ] The pedal restarts. The boot screen shows **v37 MIDI TEST**.
- [ ] The sound is normal: play a few presets, each block of one preset, the tuner (hold HOME). Note the number at the bottom
      right ("C1 42%"): baseline DSP load with the same preset: ______ %.
- [ ] Open the web editor (`editor.html`): it connects, shows the presets, and a knob turned on the pedal moves on the page.

## 2. The MIDI page

- [ ] Hold FX for about a second: the **MIDI** page opens. Rows: CHANNEL OMNI, SEND ON, CLOCK > DELAY OFF. "USB: NO HOST"
      or "HOST CONNECTED" is right.
- [ ] A short press of FX (not held) still cycles the effects (DRV, SFX, SYN...). It acts when you let go.
- [ ] Knob changes the row, PUSH goes to the next row, after the last row it leaves. HOME leaves too.
- [ ] Change CHANNEL to 3, leave. The screen shows "SAVING..." briefly, the sound mutes for a fraction of a second. Power
      the pedal off and on: the page still says 3.

Set it back to OMNI for the next steps.

## 3. Over USB-C: the pedal receives

Plug the pedal into the computer (the pedal should appear as a MIDI port named **OpenPocket**) and send:

- [ ] **Program Change 0, 1, 2, 11**: the pedal shows preset 01, 02, 03, 12 each time, the sound follows. Program Change 60 does nothing.
- [ ] **CC 25 = 0 / 127** (AMP off / on): the AMP tile on the screen goes dark and back, the sound loses and gets its amp.
- [ ] **CC 77 sweep 0 to 127** (AMP setting 1, GAIN): the gain bar moves and the sound changes smoothly. No crackle, no
      glitch, no mute. Also try CC 102 to 105 (DELAY settings), CC 106 (REVERB).
- [ ] **CC 57 = 0, 64, 127** (AMP model): the model changes (first, middle, last).
- [ ] **CC 7 sweep** (volume): the VOL number at the top follows.
- [ ] The MIDI page's "LAST:" line shows what you just sent (open it with FX held; it is read-only for the host while open).
- [ ] CHANNEL 3: a message on channel 1 does nothing, on channel 3 works. Back to OMNI.
- [ ] While the **SAVE page** (hold PUSH) is open, a Program Change from the computer is ignored. HOME to leave.
- [ ] **Clock:** CLOCK > DELAY ON, select a preset with the delay on (press DLY), start the DAW playing at 120 BPM: the delay
      TIME shows about 500 ms (the nearest step is 498 ms) and follows a change of tempo to 90 BPM (about 670 ms). With
      CLOCK > DELAY OFF nothing changes.

## 4. Over USB-C: the pedal sends

With the MIDI monitor listening to the pedal's port and SEND ON:

- [ ] Footswitch up / down: one **Program Change** per press, with the slot number minus 1.
- [ ] Turn the knob on a block's page: **Control Change** messages with the numbers of [MIDI.md](MIDI.md). Switch a block: 127 / 0.
- [ ] Nothing is sent at power-on, and nothing when the computer was unplugged and is plugged back in.
- [ ] SEND OFF: the pedal sends nothing; it still receives.
- [ ] A change sent **to** the pedal is not sent back (watch the monitor while moving a DAW fader: no echo).
- [ ] Loop test: a DAW track that echoes everything it receives (MIDI through) back to the pedal does not make it chatter.

## 5. Audio with MIDI flowing

- [ ] Play a high-gain preset continuously for 5 minutes while the DAW sends a CC sweep and clock: no crackle, no dropout, no
      mute, no "BYP" at the bottom right. Note the load: ______ % (baseline above). A rise of more than a few percent is worth reporting.
- [ ] Same with a long note flood (a keyboard or a DAW sending 100 notes a second): the pedal ignores the notes and the sound
      stays clean.
- [ ] While the flood runs, use the **web editor** (change a preset, rename, save): it keeps working. (Before this release a
      busy MIDI port could stop the editor.)

## 6. The update path still works

- [ ] On the normal installer page (without `?channel=`), Connect: the pedal is found and shows `OpenPocket v37`. Press Install
      (this puts **v36** back) and wait for "Done": the boot screen shows v36, the sound is normal.
- [ ] Open the test page again and install v37 MIDI TEST once more: it also works in this direction.
- [ ] The editor still connects on both versions.

## 7. Way back and recovery

1. **To the previous OpenPocket:** the normal installer page (no `?channel=`), Install (v36).
2. **To the official firmware:** the installer page, step 3 "Back to the official firmware", with your official `.fwsc`.
3. **A pedal that restarts in a loop:** the installer's rescue mode (step 2), plug the pedal in; it installs by itself.
4. **A pedal that does not start at all:** the SD-card recovery (`flash/`, `makeimage.bat` / `node tools/op.mjs appimage`).

This build does not change the boot, update-entry or recovery code, so all four work as for v36.

## 8. Report back

Please send, even if everything works:

```
Pedal: Pocket X / BlackBox            Computer + OS:
Installed v37 MIDI TEST: yes/no       Boot screen showed v37 MIDI TEST: yes/no
USB port name seen:                   Apps tried (DAW, monitor):
Section 2 (page):  ok / what failed
Section 3 (receive): ok / what failed   (which CC or PC numbers)
Section 4 (send): ok / what failed
Section 5: DSP load baseline ___ % with MIDI ___ %, any crackle / dropout / BYP: 
Section 6 (update, both ways): ok / what failed
Anything strange on screen or in sound:
```

For a failure, add what the MIDI page's "LAST:" line showed and the exact bytes your monitor showed. A photo of the screen helps.
