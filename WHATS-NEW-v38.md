# What's new in OpenPocket v38: changes without clicks, cabinet IRs

Three of the ideas taken from the stock BlackBox firmware ([STOCK-FIRMWARE.md](STOCK-FIRMWARE.md)).

## Turning a knob no longer clicks
- **Settings glide.** A knob's new value is reached over 23 ms instead of at once: gains, mixes, depths, the drive,
  the amp's gain, the delay's mix and feedback, the reverb's mix, the preset level, and the rest
  (`engine/px_dsp.c`, `GL_F`). The filters change at once, as before: a small step in a filter does not click.
- **The audio is no longer stopped to apply a knob.** Until v37 every change held the audio interrupt off while the
  whole engine was recomputed (the cabinet's normalisation alone runs 4608 samples). On a NAM preset, which leaves
  little margin, that most likely made a block come late: a crackle. Now the main loop prepares the heavy part while
  the audio plays, and the audio side applies the change at the start of its next block, in a few microseconds.
- **While a knob turns, the sound follows it** every 40 ms (before, it waited until the knob stopped, then jumped).
- **A block switched on or off fades** in or out over 23 ms instead of cutting.
- **A change of preset, of type, of order or of NAM model** fades the output out (6 ms), applies the change and fades it
  back in, instead of a jump.
- **The volume knob glides** too.
- Measured on the PC (`tests/engine/change_test.c`, 124 knob changes and 49 blocks on / off over the 24 factory
  presets): the clicks are 30 dB lower than with v37's way, and every setting ends exactly where a full recompute
  puts it.

## Cleaner oversampling for the drives and amps
- The drive and the fitted amps run at 4x (the drive before a NAM amp at 2x). The filters that go up and down are now
  polyphase half-band filters, as in the stock firmware: two all-pass branches, one multiply a section, each branch at
  the lower rate. Their stop band is 78 dB (stage 1) and 66 dB (stage 2) down; v37's Butterworth filters let 24..30 kHz
  through at only 6..13 dB down.
- On the pedal they take about 40 % fewer instructions than v37's filters (counted in the compiled code).
- The aliasing measured on the drives is 1 to 3 dB lower: most of what is left comes from the clipping itself at 4x,
  not from the filters.

## USER IR: your own cabinet impulse responses
- **A new CAB type, USER IR**, plays a cabinet impulse response: its first 23 ms (1024 samples at 44.1 kHz), convolved
  exactly (uniform partitioned FFT, no added latency), at the same loudness as the modelled cabinets.
- **Settings:** IR (the slot; its name shows on the knob and on the PLAY page), LOW CUT (20..400 Hz), HIGH CUT
  (20 kHz..2 kHz).
- **The web editor has a Cabinet IRs panel:** pick a slot, choose a `.wav` file (16, 24 or 32-bit, float, any rate,
  mono or stereo). The editor drops the silence before the onset, resamples to 44.1 kHz and sends it (8 slots, kept
  through firmware updates, in the flash at 0xF4000..0xFC000).
- Cost on the pedal: about 420 instructions a sample (two FFTs of 128 and 16 x 64 complex multiply-adds a block,
  counted in the compiled code), against about 150 for a modelled cabinet; 17 KB of RAM.

## Not done: the time effects at half rate
The stock firmware runs its modulation and delay effects at 24 kHz to save time. Measured here first: MOD and DELAY cost
1 to 3 % of what the NAM network costs, so halving them would free about 1 %, and their echoes and chorus would lose
everything above 11 kHz. Not worth it for now; the reverb already runs at half rate.

## Delay and reverb: lighter loops, and the CPU per block on screen
- The delay and the reverb run over each block in a loop made for their type, as the stock firmware's effects do: the
  type's tests are made once a block instead of once a sample (on the pedal a taken branch costs about 8 cycles). The
  sound is unchanged, bit for bit (checked on every factory preset and every delay and reverb type).
- **The tuner page's footer shows where the CPU goes:** the 4 costliest blocks, in % of an audio block's time, over the
  last second (for example `NAM 64 REV 7 DLY 5 CAB 3 %`; with a NAM amp the blocks after it run on the second core,
  beside the network). "ANY KEY: EXIT" moves to the title bar. If a sound crackles, a photo of this page (with the
  LATE count) says which block to make lighter.

## For developers
- Engine: `px_engine_change`, `px_engine_knobs`, `px_engine_prepare` (`engine/px_dsp.h`); the firmware's side in
  `firmware/px_audio.h` (`audio_apply`, `a_commit`).
- Editor protocol 3: `IR_LIST`, `IR_WRITE`, `IR_COMMIT`, `IR_ERASE` ([EDITOR-PROTOCOL.md](EDITOR-PROTOCOL.md)).
- New tests: `tests/engine/change_test.c`, `tests/engine/ir_test.c`, and the IR part of `tests/test_editor.mjs`.
