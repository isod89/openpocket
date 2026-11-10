# What's new in OpenPocket v41: NAM presets much lighter, every chain fits

Most of v41 is **VinSpee's work** ([pull request #22](https://github.com/isod89/openpocket/pull/22), first reviewed step by
step in [VinSpee/openpocket #16, #18, #19 and #21](https://github.com/VinSpee/openpocket/pull/16)). Thank you, Vince.

Every output sample is the same as v40's: VinSpee checked it bit for bit on the PC (every block, every type, every factory
preset) and on the pedal at boot. Nothing sounds different. The pedal just has much more room.

## 1. NAM presets use much less of the processor

- **Before:** the two cores split each block of the NAM network by frames. They ran every layer on half the frames,
  the second core waited for the first at every layer, and the end of the network ran on one core.
- **Now:** they split it **by layers**. The first core runs the first layers, the second core the rest, and neither waits
  for the other at each layer. The second core keeps working through the audio interrupts, and the split between the
  cores is set in one step from what is measured.
- New hand-written routines for the network (`rdm`, `post4`, `post5`), and six effect blocks (gate, compressor, EQ, two
  pitch types, two synth types, the cabinet) now work on a whole sub-block at a time.

Measured on a pedal, all 24 factory presets:

| | v40 | v41 |
| --- | --- | --- |
| NAM presets (DSP load) | 68 to 82 % | about 51 to 63 % (on average 18 %, then 6 to 11 % lower again) |
| ORGAN | 82 % | 63 % |
| TIGHT METAL | 68 % | 51 % |

**A NAM amp with all 12 blocks on and every knob at 100 now runs.** On v40, four to seven of eleven such worst-case
chains switched the effects off (`BYP`). On v41 none do: they run at 71 to 83 % on average, at most 86 %, against the
93 % at which the safety bypass engages.

## 2. More free memory

24 KB of RAM freed (27 KB free instead of 3 KB), with the same features: smaller tuner buffers, buffers moved to unused
memory, the preset store and the update code without their own copies.

## 3. Preset changes sound sooner

- **A preset now sounds in 16 to 27 ms** instead of 57 to 96 ms (footswitch, MIDI Program Change, editor).
- When the MODEL knob loads another NAM model, the corner of the screen shows **LOADING** until it plays.

## 4. The screen

- A red **OVERLOAD: SOUND BYPASSED** bar when the safety bypass engages, with what to do (turn a block off) and the peak.
- **LOADING** in amber.
- A footswitch on a block page goes back to the PLAY page, where the new preset shows.
- Blocks that are off are easier to read.
- While a preset is edited, the PLAY page's footer says `HOLD: SAVE`.

## 5. Safer

- **After a failed boot**, safe mode also skips the NAM routines' start-up check: a fault there costs one boot, not a
  boot loop, and USB stays reachable for the installer's rescue.
- **If the second core stops answering**, the first one now gives up waiting after 8 ms, sets the second core aside and
  goes on alone, as v40 did. VinSpee's second core runs for as long as the split is on, so v40's time limit no longer
  applied there. Without this, a stuck second core would have held the audio until the watchdog restarted the pedal.
- **The MIDI page says when the second core was set aside**, and why, in red: `CORE 2 OFF: MISREAD`, `LATE`, `FAULT`...
  Until the pedal is switched off, presets that need both cores can then go to `BYP`. The corner shows `C1` both for a
  preset that runs on one core by design and for this case; the MIDI page tells them apart.

## 6. For developers

- **PING** (editor command 18) reports the peak load, late blocks, clipped blocks, the stacks' free bytes and a DSP
  profile. `node tools/op.mjs dsp --presets 1,2,3 --seconds 6` prints them per preset over USB.
- New host tests: `tests/engine/lp_test.c` (the layer split against one core), `tests/engine/hand_test.c` (the hand
  routines against C, under the sanitizers), `tests/engine/lp_stuck.c` (a second core that never moves: the waits give
  up), `tests/test_post5.py`, `tests/test_ping.mjs`, and `tests/equiv.sh REF` (this engine against another commit's, bit
  for bit, 852 runs).
- The Bluetooth test image fits again. Its radio now scans for the stored radio records from `0xA1000` (where a Pocket
  X keeps them) and gives defaults when installing OpenPocket erased them (`firmware/ble/ble_rf.c`).

## Bluetooth MIDI: set aside

Bluetooth MIDI in the normal firmware (the work after v40) is kept on the branch **`ble-v41`**, not in this release. On it,
a Pocket X was seen and connected from Android, and a phone's slider set the volume. But the sound crackled while a
phone sent a stream of messages, and VinSpee's work uses the memory that branch gave the radio. Bluetooth will come back
on top of v41. What was learned is in [BLUETOOTH.md](BLUETOOTH.md) and
[STOCK-FIRMWARE.md, section 19](STOCK-FIRMWARE.md#19-fourteenth-pass-bluetooth-read-to-the-end).

## Please try on the pedal

Not yet tried on a real pedal: your own NAM models, the tuner, the looper, the footswitches' timing, the LOADING
indicator on the screen, and a real guitar into the heaviest chains. If something sounds different from v40 or a
`CORE 2 OFF` line appears on the MIDI page, please report it with the preset.
