# What's new in OpenPocket v40: the effects run the stock firmware's way

The stock BlackBox firmware, read down to its boot code ([STOCK-FIRMWARE.md](STOCK-FIRMWARE.md), sections 7.3, 8.2,
8.6 and 18.1), does three things OpenPocket did not. v40 does all three. The aim is no `BYP` and no crackle when the
reverb is on.

## 1. The audio path runs from RAM

- **Until v39** the whole engine ran from the flash, through an instruction cache that the two cores share. A heavy
  preset needs more code than that cache holds: the amp or the NAM network on one core and the delay and reverb on the
  other kept pushing each other out of it. Every miss is a slow read from the flash, and that time showed up as load,
  then as `BYP`.
- **The stock firmware** copies its whole per-sample library into RAM at boot (66 KB) and runs it from there.
- **Now OpenPocket does the same.** The functions that run every block (the chain, the delay and reverb loops, the NAM
  network and its hand-written routines, the oversampling, the cabinet IR, the audio interrupt's own code) are copied to
  RAM at boot, about 75 KB. The code is exactly what the compiler made before, moved and not rewritten.
- **Why v37's try failed and this one does not:** in v37 the RAM code sat at the bottom of the RAM, just out of the
  call instruction's reach of the flash (+-4 MiB), and the linker refused 478 calls. The RAM code window now sits at the
  top of the free RAM (`0x01C5E000`), within reach of all the flash code. The link checks it (`firmware/app.ld`).

## 2. The delays run at half rate, as the stock delays do

- The delay line now turns at **22.05 kHz** (one tick every two samples), as the stock firmware's five delays do. That
  halves the delay's work, and its memory: **2 s of delay in 88 KB instead of 175 KB**. The freed RAM is what makes room
  for point 1.
- **Same echoes:** the same times (within 0.1 ms), the same feedback and modulation (every depth converted). Measured on
  the bundled lead loop through an amp and cabinet, the echoes are within 0.1 dB in level and 0.4 dB up to 6 kHz. Above
  6 kHz, where a cabinet leaves very little, they are about 2 dB darker. A small fixed lift on the echoes makes up most of
  what the half rate takes off.
- The wet lanes of v39 are unchanged: the delay through its lane is still bit for bit the delay inline
  (`tests/engine/wet_test.c`).

## 3. The reverb's low cut runs at the tank's rate

The reverb's tank already ran at half rate. Its low cut now does too, as the stock reverbs' filters do: one filter fewer
per sample. TONE stays at the full rate, because it also removes the half rate's images. The reverb's output is the same
as v39 to within 0.0002 on every type.

## On the pedal

- Nothing to change in your presets. The delay's TIME knob, tap tempo and MIDI clock give the same times.
- **Please report**, on a preset that went to `BYP` with v39: the CPU per block (on the tuner page's footer in v39, on
  the diagnostics page now: keep holding HOME past the tuner) with v39 and with v40, and whether `BYP`, `LATE` or `CUT`
  still appear.

## For developers

- `tools/ramcode.py` renames a compiled object's function sections (`.text.NAME` to `.ram_text.dsp.NAME`), so that the
  linker puts them in RAM. `--plain` also moves an object's plain `.text` (px_dsp.o's hand-written routines).
  `firmware/build.sh` lists the functions (`RAM_DSP`, `RAM_FW`). A name the compiler inlined is reported and skipped.
- `firmware/app.ld`:
  - `POOL` is `0x01C20000..0x01C5E000` (248 KB);
  - `RAMTEXT` is `0x01C5E000..0x01C74000` (88 KB);
  - an `ASSERT` refuses the link if the flash code ever grows out of the RAM code's call range;
  - `0x01C00000..0x01C08000` is free.
- `tools/check_calls.py`: the audio path's RAM code (`.ram_text.dsp.*`) may call the flash and be called from it. The
  flash driver and the second core's loop still may not, because they run while the flash cannot be read.
- Engine: `PX_DLY_N` is 45,056 ticks. Delay times and depths are in ticks (`rc_delay`). The delay keeps
  `dly_tick`, `dly_xa`, `dly_y0`, `dly_y1` and `dly_dp`, as the reverb keeps its own half-rate state.
