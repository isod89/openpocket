# The stock firmware, read for OpenPocket (BlackBox_021)

What M-VAVE's own firmware for the BlackBox (`BlackBox_1.fwsc`, tag `BlackBox_021`) does, read from its code, and
what OpenPocket can take from it, especially on the audio side. The firmware was unpacked with `tools/pocketx_fw.py`
and disassembled with JieLi's own objdump (`--mcpu=r3`, which decodes the FPU instructions), with Ghidra for the
call graph. Addresses are those of the stock image: flash code at `0x2000120`, RAM code at `0x1C00000`.

Each finding says how sure it is: **read** (seen in the code), **likely** (strong evidence, one step not seen), or
**guess**.

## 1. The image

| Part | Where | What |
| --- | --- | --- |
| Flash code | `0x2000120`, 630 KB | the application, run from flash through the cache |
| RAM code | `0x1C00000`, 66 KB (`0x107E4`), copied at boot | the hot DSP: FFT, filters, resamplers, the amp's process stages |
| Overlay window | `0x1C22340` to `0x1C23F80`, 7 KB | one of two code sets copied in on demand (`f_0200774c`): 7,232 B from flash `0x2093AE8` or 6,756 B from `0x2092084` |
| Effect code | 20 blocks after the overlays (flash `0x2095728`) | float routines of the time effects (chorus, delays, reverb...), copied to RAM from `0x1C23F80` on |

**Read.** The DSP that runs every sample is not executed from flash: the library and the per-sample stages live in RAM,
and the less used code is swapped into one small RAM window. On this chip a flash fetch that misses the cache costs
many cycles and a taken branch about 8, so this is where the stock firmware gets its speed.

## 2. Tasks and interrupts

**Read** (interrupt table and `request_irq` calls):

| Source | IRQ | Priority | Core | Handler |
| --- | --- | --- | --- | --- |
| ALNK0 (the I2S audio) | 11 | 4 | CPU0 | `0x202A230` |
| BLE radio (two lines) | 45, 29 | 2 | CPU0 | the BT controller |
| SOFT3 (software interrupt) | | 6 | both | scheduler wake-up |
| TIMER4, SBC decode | | | CPU0 (19.1: asked for CPU1, forced to CPU0) | |

Named tasks (table at `0x208CF00`): `wet_audio`, `tuner_task`, `USB-Audio`, `bt_sbc_dec`, `midi_route`, `sys_timer`.

What it means for OpenPocket's Bluetooth: in the stock firmware the radio's interrupts sit **below** the audio
interrupt (2 against 4) on the same core, so audio always preempts the radio and never the other way round.
**Read (third pass, section 7):** the dry chain runs inside the audio interrupt itself; only the delay and the reverb
run in the `wet_audio` task, one block behind. In our v37 attempt
the radio's work could hold up the audio block, which fits the "BYP 150%" we saw. Any return of Bluetooth should keep
that order: audio interrupt highest, radio below it, radio work on the core that
does not run the network.

## 3. Audio input and output

**Read** (ALNK ISR `0x202A230`):

- **64-frame blocks**, 24-bit samples, stereo I2S on ALNK0 (the same pins OpenPocket uses).
- The input gain is applied as an integer multiply and `>> 15`; peak meters are kept per block.
- Two counters, **early** and **late**, count blocks that arrive before or after the expected time: the stock firmware
  watches its own real-time margin the way our `LATE` counter (v37) now does.

**Read (fourth pass, corrected): 44.1 kHz, not 48.** The ALNK0 set-up (`f_02023a20`, at `0x2024A6C`) clears bits 8..11
of CLK_CON2 (`0x10014`), which selects the 44.1 kHz clock family (the JieLi driver sets bit 8 for 48 kHz), and writes
the same dividers as the SDK's 44.1 kHz case (`ALNK0 + 0x0C`: bits [4:2] = 0, [7:5] = 4). Every model's set-up is
then called with **fs = 44118** (`0x472C5600`, eleven places: the amp, gate, compressor, drives, reverbs, delays,
modulation): the stock firmware takes its real rate to be 44,118 Hz, 0.04 % above 44,100 (**guess**: the audio clock is
not an exact 44.1 kHz multiple, as 240 MHz / 5440 = 44,117.6 Hz would be). The `48000.0` seen earlier is a fast path:
the coefficient functions accept 44,118, 44,100 or 48,000 and skip the table work when fs is 48,000. The delays and
the reverbs run at half of it, 22,059 Hz (the constant is in their set-up). So the stock firmware and OpenPocket run at
the same rate; the stock firmware uses 64-frame blocks (1.45 ms) where OpenPocket uses 128 (2.9 ms).
- The DMA: LEN = 128 words (64 frames, 24-bit stereo), two 1 KB buffers at `0x1C20E20` (out) and `0x1C21220` (in),
  channel 0 and 1 each set to 24-bit in CON1.

## 4. How the stock effects are built

### Module descriptors

**Read.** Each effect is a small descriptor in flash: an init function, a parameter function (UI value to
coefficients, all in float at 48 kHz, run outside the audio path), a process function, and the instance size. Tables
at `0x204F54C` (5 modules), `0x20509D4` (7 modules), `0x204FC30` (the amp family) and `0x2054F0C` (six amp
setups: `f_0202cfd0`, `cbfc`, `c962`, `ce20`, `d4d0`, `c874`).

### Parameters glide, they never jump

**Read** (`f_01c0796c`, `f_01c07aee`). A gain is never set: it moves towards its new value by a one-pole ramp, one
step per sample (`g = g*a + target*(1-a)`), and the routine falls back to a plain multiply loop as soon as the value
has arrived (it compares the ramp's next value with the current one). Mixes (wet/dry, the two inputs of a crossfade)
move the same way, two gains at once. On the control side the setters keep a smoothed copy too
(`0.97 * old + 0.03 * new` per block, `f_0202d7d8`).

### A change of type is a crossfade

**Read** (`f_01c05350`, used by the 12 modules of the first two tables). Each slot holds an old and a new instance.
When the user changes the type or the model, both run for a while and the outputs are crossfaded with the ramped
gains above; when they are the same, only one runs. No mute, no click.

### Time effects run at half rate

**Read** (`f_01c05350`, `f_01c0b0f6`, `f_01c0b02a`). The delays and the reverbs take the block, sum it to mono,
**decimate it 2:1** with a polyphase IIR half-band filter (two all-pass sections per branch), run the effect on
**32 samples at 24 kHz**, and **interpolate 1:2** back, with the crossfade gain applied in the interpolator. A delay or
a reverb loses nothing audible above 12 kHz, and costs half.

**Correction (third pass, read):** the modulation effects do **not** go through this wrapper. Their process functions
(`0x1C059E6`..`0x1C05A04`) all call `f_01c05924`, which runs the effect at full rate on 64 samples, once per channel
with two states (left, right), and mixes it with the dry signal (`f_01c08d08`, the mix ramped by a one-pole of
0.0183 a block).

### The MW-808 drive (first taken for the amp): 6x oversampled, one division per sample

**Read** (`f_0202d236`). **Corrected in the fifth pass (section 10.2):** this is the process function of the **MW-808**
drive (category 0), not of the amp; the amp's own process is `f_0202d7d8` (section 10.1). Per 16-frame sub-block:

1. first-order input filter (`f_01c08cba`: `b0, b1, a1`, three multiply-adds a sample);
2. the preamp stage (`f_01c05754`): input conditioning with a soft limiter (`fmin`/`fmax`), three more first-order
   filters, a ramped gain;
3. **upsample 2x** (`f_01c0758a`: polyphase all-pass half-band, 6 coefficients:
   0.3195, 0.0958, 0.7420, 0.5567, 0.9600, 0.8695) **then 3x** (`f_01c076d0`: three all-pass branches,
   0.7396, 0.3513, 0.1013): 16 samples become 96, at 288 kHz;
4. the waveshaper at 288 kHz: bias added, then a rational sigmoid, asymmetric, with one division per sample instead
   of `tanh` (`y = c - 1/(k*x + d)` on each side of a knee, with `k = 75.86`, `d = 1.0133`, `c = 0.2163`, knee
   `-0.1014`);
5. **down 3x** (`f_01c07750`, the same three branches, `x 1/3`) and **down 2x** (`f_01c077e6`);
6. tone stages (ramped gains, a two-way mix), two more first-order filters, out.

Where a `tanh` is needed it is a **512-entry table with linear interpolation** in RAM (`0x1C0E408`, used by
`f_0202d7d8` for its input soft clip and by the effect blocks).

### Cabinets: partitioned FFT convolution, on the FFT block

**Read** (`f_0202f844`, RAM library `f_01c04f9a`...; **corrected in the ninth pass, 14.5**: `f_0202f844` is the
`AM2`/`AM3` amp stage, which uses the same convolution; the cabinet's own process is `f_0202bafa`). Cabinet IRs are convolved with a uniform partitioned FFT
convolution: 64-frame partitions, FFT of 128, overlap-save, complex multiply-accumulate in float (`f_01c0a676`, two
loads per complex product, unrolled). The FFTs themselves run either in software (radix routines in RAM for 32, 64
and other sizes) or **on the chip's FFT block at `0x1EE0400`** (the same block OpenPocket's NAM uses as a vector unit):
the code writes a 3-word job (`[config | 1, in, out]` at `0x1C1001C`), points `CADR` (`0x1EE0404`) at it, starts it with
`CON = 0x101`, and later waits for bit 7 of `CON` and clears it with bit 6. The block works in fixed point, so the
code normalises the input first (it finds the block's headroom, `f_02044de2`, clamps the shift to -24..+18) and
rebuilds a float scale `2^e` for the result (`f_01c08c74`). It checks which core it runs on (`cnum`) and uses the
hardware only on CPU1.

### Captured models

**Read** (the loader). The stock firmware loads models of six formats from its own flash: `CAB` (an IR), `AM2`,
`AM3`, `AM4` (its own amp captures: **read in the fifth pass**, section 10.1, a FIR, a 16-unit GRU and a FIR), `NAM` (type 16, 1,871 float weights, 7.5 KB) and `LMT`. **Likely:**
the model inference uses the integer kernels of the overlay window: overlay A is built of 64-bit integer
multiply-accumulate loops with three to five accumulators, one input broadcast against a row of weights read with a
stride (`r_acc += [w ++= stride] * x`, inside hardware loops), the shape of a dense or 1-D convolution layer; the
loader that swaps A and B is called from the model module's init (`f_02008692`). So the stock firmware runs its
networks in **fixed point** from RAM, as OpenPocket does since v37. Its network is of the same order as A2-Lite: if
each weight is used once per sample, as in a WaveNet, 1,871 weights are about 1,871 multiply-adds per sample, against
A2-Lite's ~1,730. **Read (fourth pass): it is A2-Lite.** The firmware holds the A2-Lite layer table at `0x2055A74`:
kernel sizes 6 (x14), 15, 15, 6 (x7) and dilations 1, 3, 7, 17, 41, 101, 239 (twice), 1, 13, then 1..239 again,
exactly the 23 layers OpenPocket's `engine/px_nam.c` runs, and 1,871 is A2-Lite's weight count. So the stock `NAM`
type plays Neural Amp Modeler A2-Lite captures, in fixed point, at 44.1 kHz like OpenPocket.

## 5. What OpenPocket can take from it

v38 has taken items 1, 3 and 5 ([WHATS-NEW-v38.md](WHATS-NEW-v38.md)). Item 2 was measured first and left out: MOD and
DELAY cost 1 to 3 % of the NAM network, so running them at half rate would free about 1 %.

In order of value for the sound, with what it would cost.

1. **Glide every parameter, crossfade every change.** Today `audio_apply` (`firmware/px_audio.h`) masks the audio
   interrupt, recomputes the whole engine (`px_engine_set`) and swaps the new coefficients in at once. That makes two
   kinds of crackle: a step in the sound when a coefficient jumps, and a late block if the recompute takes longer
   than the block margin (the interrupt is held off meanwhile). The stock way: compute the new set outside the
   interrupt into a second copy without masking, swap a pointer, and let gains and mixes ramp per sample; for a type
   or model change, run the old and the new instance together for ~20 ms and crossfade. For the NAM amp this means
   two network states for the fade time, or a short fade out/in of the amp block (cheaper, almost as clean).
2. **Halve the cost of the time effects.** Run MOD, DELAY and the reverb at half rate behind a polyphase half-band
   decimator and interpolator (two all-pass sections a branch, about 6 multiply-adds per output pair). That frees CPU
   for the NAM network, which is the limit today.
3. **Cheaper oversampling.** OpenPocket oversamples the drive and amp 4x with Butterworth biquads (two up, two down,
   at the oversampled rate). The stock firmware gets 6x for less with polyphase all-pass half-band filters (2x) and a
   three-branch all-pass stage (3x): each all-pass section is one multiply-add, and each branch runs at the lower
   rate. The coefficients are in section 4 and can be used as they are.
4. **Waveshapers without `tanh`.** A rational curve (one division) or the 512-entry table with linear interpolation,
   both in RAM, instead of a polynomial or a library call per oversampled sample.
5. **Real cabinet IRs.** OpenPocket's cabinets are filters and sparse taps. The stock firmware shows that a 64-frame
   uniform partitioned convolution fits beside its amp, with the FFTs on the FFT block of CPU1, with block floating
   point. OpenPocket already drives that block (`engine/px_nam.c`); the FFT mode is the same `CON`/`CADR` protocol
   with a different job word. This would let the pedal load user IRs (and the `CAB` files of the stock firmware).
6. **Sample rate 48 kHz?** Most NAM captures, TONE3000 included, are made at 48 kHz; at 44.1 kHz they play about 1.5
   semitones low (`docs/NAM.md`). (Corrected in section 8.1: the stock firmware runs at 44.1 kHz too.) Moving OpenPocket to 48 kHz fixes the
   user models without resampling, but costs 9% more CPU per second of audio and needs the factory models retrained
   (`tools/train_a2.py`). Worth it only together with items 1 to 3, which free more than that.
7. **Smaller blocks later.** 64 frames at 48 kHz (1.33 ms) against our 128 at 44.1 (2.9 ms): lower latency, but the
   per-block overheads (the network's layer-by-layer pipeline, the two-core hand-off) count twice as often. Not before
   the NAM pipeline has been reworked for it.
8. **Bluetooth, if it comes back:** radio interrupts below the audio interrupt, on CPU0, and no radio work on the core
   that runs the network at the time it runs it (section 2).

## 6. Second pass (v38): how the effects' code is kept fast

**Read.** The effects' per-sample code lives in 20 blocks of flash (`0x2095728..0x209A0F4`, a table of {address, size}
at `0x2055DA0`), each linked to run at one of four RAM slots, and copied there when its type is chosen: the code that
runs is always in RAM, and only the chosen types' code is there. Found by the slot address that makes each block's calls
land on known functions:

**Corrected in the fourth pass** (section 8: the loader and each block's caller were found, so this is read, not
guessed):

| RAM slot | Blocks | What they are |
| --- | --- | --- |
| `0x1C23F80` (1,456 B) | 0..5 | the six reverbs: Room, Hall, Swell, Spring, Shimmer, Cloud |
| `0x1C24540` (2,244 B) | 6..10 | the five delays: Analog, Duck, dTape, Dual, Lofi |
| `0x1C24E20` (980 B) | 11..17 | the seven modulations: Chorus, Phaser, Tremolo, Flanger, Vibrato, Univibe, Autofilter |
| `0x1C25200` (1,172 B) | 18, 19 | the process routines of the MW-GOLD and MW-RAT drives (set up by `f_0202cbfc`, `f_0202ce20`; **corrected in the fifth pass**: these are drives, not amps, section 10.2) |

Each block is one type's own routine: no test of the type inside the sample loop. **Read:** their reverbs keep their
lines in 16 bits, as OpenPocket's do since v35.

What OpenPocket took from it in v38: the delay and the reverb run over the block in a loop made for their type (the
type's tests done once a block, not once a sample: on this CPU a taken branch costs about 8 cycles), with the output
unchanged bit for bit. Moving the effects' code into RAM as well would need the engine split so that no call crosses
between flash and RAM code (out of the CPU's call range): left for when the per-block times on the pedal (the tuner
page's footer, v38) show the effects are worth it.

## 7. Third pass: from the audio interrupt to the effects

This pass followed the audio from the I2S interrupt to each effect, instruction by instruction, instead of starting
from the effects. It corrects section 2 (where the DSP runs) and section 4 (the modulation's rate).

### 7.1 The whole dry chain runs in the audio interrupt

**Read.** `request_irq(11, 4, 0x202A230, CPU0)` is the only audio interrupt (`f_02023a20`, at `0x2024B62`). Its
handler, for each 64-frame block:

1. copies the 128 words out of ALNK0 (`f_01c06f1e`), sign-extends the 24-bit samples with an input shift (x1, x2 or x4)
   and keeps peak meters (`f_01c06f2c`, `f_01c06f6e`, `f_01c06fba`);
2. calls `f_01c01364(in, out, 64)`, which is either a plain copy (effects off) or **`f_02001326`, the effect engine**;
3. mixes in the computer's and the phone's audio: USB playback comes in through a ring of 1,024 packed 24-bit frames
   at `0x1C17FC0`, written by the USB side with a linear-interpolation resampler (`f_0202ab58`) and read in the
   interrupt (`f_020296bc`, `f_020298d8`); Bluetooth (A2DP) through a ring of 6,144 16-bit samples at `0x1F29EE0`
   (`f_02029a6e`), each with its own gain; several weak hooks in this path are linked to address 0 (unused here);
4. times itself: it reads the free-running timer at `0x1EEF0F0` at the start and at the end, and keeps the sum and the
   maximum over 512 blocks (`0x1C1AF50 + 620..660`, the average at `+2060`, the maximum at `+2064`), plus the early and
   late counters of section 3.

`f_02001326`, inside the interrupt:

- converts the block to float, **two planar channels of 64** (`0x1C14320`: left, then right at `+256`, `f_01c0b2e2`);
- applies the volume with a **linear ramp over the block** when it changes (step `(new - old) / 64` a sample);
- runs a small state machine for mute, bypass and the tuner (states 0..6 at `0x1C1AF50 + 53/54`), whose fades follow a
  256-entry curve (`0x1C0EC0C`, 0 to 1);
- runs the chain in two calls, `f_0200126a` (the slots before a split point) and `f_0200127e` (the rest);
- converts back to 24-bit with round-to-even (`f_01c0b31c`, a hardware loop of 24 cycles per stereo pair).

### 7.2 The chain runner

**Read** (`f_02000d4c`, data at `0x1C1AF50`):

- six slots, each with a category (`+1010 + i`), an on flag (`+1004 + i`), an instance (`+1428 + 4i`), a descriptor
  (`+1084 + 4i`) and its parameters (`+2528 + 12i`, six 16-bit values); the active slots are listed in order at
  `+1016` before each block;
- each slot runs through `f_01c0130e(instance, buffer, 64, process, ...)`, in place;
- when a slot is switched on or off, its output is **crossfaded over 320 samples** (`f_0200088a`: a linear ramp of
  `1/320` a sample, about 7 ms), never cut;
- the slots of category 3 (delay) and 4 (reverb) are not run here (section 7.3);
- the same function also feeds the tuner: the block is decimated by 16 with a 26-tap symmetric FIR (13 coefficient
  pairs) into a 1,024-sample ring, about 3 kHz, read by `tuner_task`.

### 7.3 The delay and the reverb run beside the chain, one block late

**Read.** This is how the stock firmware keeps its time effects from ever making the dry sound late:

- at a delay or reverb slot, the interrupt calls `f_02000c36(lane, ...)`: lane 0 is the delay, lane 1 the reverb;
- `f_01c00e32` does the exchange in one pass: the output is `dry x gain + the lane's wet output of the previous block`
  (stereo, left at `+48` and right at `+304` of the lane's buffer), and the lane's next input is the **mono sum**
  `(L + R) / 2` of this block. Two buffers a lane, used in turn;
- then it posts a semaphore (`ctx + 920`, `f_0205ca82`) to wake **`wet_audio`**, a task at priority 24 with a
  1.5 KB stack (`f_0205d870` at `0x2023606`, entry `0x202A910`);
- `wet_audio` waits on that semaphore, runs each lane that has a block (`process(handle, out, in)`), swaps the buffers
  (`f_02001b10`), and times each lane with the same timer (`ctx + 2700 + 4 x lane`: count, last, maximum);
- **likely:** if a lane is not ready when the interrupt needs it, the block passes with the dry part only and a
  counter goes up (`[lane + 28]`), instead of the whole output being late.

So the wet sound comes 64 samples (1.3 ms) late, which no one hears in an echo or a reverb tail, and the delays and
reverbs (at half rate, section 4) run in a task that the interrupt never waits for. The dry path (gate, drive, amp,
modulation, cabinet) is the only thing that must finish inside the block.

### 7.4 Presets and the effect catalogue

**Read** (`f_02001efc` checks a preset, `f_02001e04` repairs one):

| Offset | Size | What |
| --- | --- | --- |
| 0 | 2 | volume, 0..100 |
| 2 | 6 | the order: a permutation of 0..5 (checked bit by bit, `f_02001d74`); the default is 0,1,2,3,4,5 |
| 8 | 6 | on / off, one byte a slot |
| 14 | 6 | the model in each slot |
| 20 | 72 | six 16-bit parameters a slot, each clamped to its descriptor's min and max (0..100 in the UI) |

92 bytes, then a "changed" flag. The categories (table of six pointers at `0x2054EDC`, entry `{index, count, list,
single descriptor}`):

| Category | Models | Descriptor(s) |
| --- | --- | --- |
| 0 | 10 | Noise Gate++, Boost, Compress, AI Gate MS, MW-808, MW-GOLD, MW-OCD, MW-RAT, MW-MUFF, MW-FACE (list `0x2046E30`) |
| 1 | 20 | one AMP descriptor (`0x2046D38`) for all 20 amps, the model passed to its init |
| 2 | 7 | Chorus, Phaser, Tremolo, Flanger, Vibrato, Univibe, Autofilter (`0x204F81C`) |
| 3 | 5 | Analog, Duck, dTape, Dual, Lofi (`0x204F5E8`) |
| 4 | 6 | Room, Hall, Swell, Spring, Shimmer, Cloud (`0x2046FEC`) |
| 5 | 20 | one CAB descriptor (`0x2046DB0`) for all 20 cabinets (Lvl, Lcut, Hcut) |

### 7.5 Shared tables and curves

**Read** (RAM data, values checked):

- `0x1C0E408`: `tanh(i / 64)` for i = 0..511, linear interpolation between entries: the soft clip of the amp's input,
  the chorus's input and one of the five LFO shapes at `0x2054EB4` (**corrected in the fifth pass**: those five functions
  turn a 32-bit LFO phase into a 0..1 wave, sine, soft square and others; they are not waveshapers);
- `0x1C0DE00` (square root of the mantissa, 0.707..1) and `0x1C0F414` (log2 of the mantissa, -1..0): together they make
  `asinh(x) = ln(|x| + sqrt(x^2 + 1))` without a library call (`f_01c05a18`), another soft clipper;
- `f_01c05b52`..`f_01c05d32` are those five LFO shapes (above), not knob mappings as first written.

### 7.6 Every interrupt

**Read** (all 20 `request_irq` calls; number, priority, core):

| IRQ | Priority | Core | Handler | What, where read |
| --- | --- | --- | --- | --- |
| 11 | 4 | CPU0 | `0x202A230` | ALNK0, the audio (7.1) |
| 9 | 6 | CPU0 | `0x20294EC` | USB device (works on `0x51000`) |
| 62 | 5 | CPU0 (asked: CPU1; 19.1) | `0x2030AA8` | completion of a hardware job: clears `0x10800` bit 14, calls the job's callback |
| 46 | 3 | CPU0 (asked: CPU1; 19.1) | `0x204303A` | a peripheral at `0x13D00` that posts a semaphore |
| 123 | 6 | both | | software interrupt (scheduler) |
| 124, 125 | 7 | CPU1, CPU0 | | software interrupts between the cores |
| 44 | 2 | CPU0 | `0x2030DC2` | the LRC timer, not the radio (19.1) |
| 45, 29 | 2 | CPU0 | `0x2063BC2`, `0x20642DC` | the BLE baseband: end of event, reception (19.1) |
| 40, 41 | 2 | CPU0 | `0x2072600`, `0x2074112` | the classic baseband and its clock (A2DP) |
| 3, 5 | 1 | CPU0 | `0x205DF6E`, `0x2030C02` | timers |
| 2, 6, 16, 24, 28, 49, 50 | 0..7 | CPU0 | | not traced |

The audio interrupt is above everything that does real work on CPU0 except the timers (1) and the radio (2): the
radio interrupt is short and its work is done in tasks.

### 7.7 What OpenPocket can take from this

1. **Take the delay and the reverb out of the critical path** (the largest gain for crackles; **done in v39**,
   [WHATS-NEW-v39.md](WHATS-NEW-v39.md), with a lag of two 128-frame blocks that the delay's taps make up for). Today they run inside the
   block that must finish before the next interrupt, in series. As in the stock firmware: send a mono copy at their slot,
   return their wet output one block later, and let the dry path finish without them. A late wet block then costs an
   echo's 2.9 ms, not a crackle. With a NAM amp, CPU1 already runs everything after the amp; the wet part could go to
   whichever core has time left at the end of the block.
2. **Time the interrupt the stock way:** sum and maximum over 512 blocks from a free-running timer, plus the wet task's
   own times per lane. v38's footer shows the averages; the maximum is what predicts a crackle.
3. **Ramp the volume over the block** (v38 glides it already) and **crossfade on/off over ~7 ms** (v38: 23 ms).
4. **One `tanh` table for every soft clip**, as above, instead of polynomial approximations per sample.

## 8. Fourth pass: the rate, the code blocks, the network, the overlays

### 8.1 The sample rate

44.1 kHz (fs = 44,118 in the code), not 48: section 3. This corrects section 5's item 6, which rested on the 48,000
constant: the stock firmware plays its captures at 44.1 kHz as OpenPocket does.

### 8.2 How an effect's code gets into RAM (read)

`f_0201c0a8(block)` loads block 0..20 into its slot: blocks 0..5 into slot 0, 6..10 into slot 1, 11..17 into slot 2,
18..20 into slot 3. The slot table `{start, end}` is at `0x2055150` and the block table `{flash address, size}` at
`0x2055DA0`, both reached from one base pointer (`0x2054870`), which is why no code holds their addresses. It skips the
copy when the slot already holds that block (`0x1C0B3A8`, one byte a slot), checks the sizes and alignment, copies,
`csync; ssync`, and compares the copy with the flash before it records the block (`memcmp`). Each effect's init
calls it with its own block, so only the four chosen types' code is ever in RAM. The process function then runs the
slot's address through a pointer (the modulation's `f_01c05924` calls `[instance + 8]`, set to `0x1C24E20`).

| Block | Effect (init) | What its set-up shows |
| --- | --- | --- |
| 0 | Room (`f_0202e9cc`) | 8 all-passes of 29, 113, 209, 308, 68, 156, 254, 363 samples, gains alternately -0.6 and +0.6; one 5,132-sample line read at 16 taps (1,386..4,790), at 22,059 Hz (63..217 ms) |
| 1 | Hall (`f_0202ec0c`) | **likely** the same structure: a line of 2,470 and 16 taps from 2,036 to 4,937 |
| 2 | Swell (`f_0202eed8`) | an 8,196-sample buffer and the RiseT envelope (rates 22,059 / 441 / 17,647) |
| 3 | Spring (`f_0202f080`) | lines of 5,440 and 12,592, two of 640, a 9.6 kHz filter: the "Combs" knob |
| 4 | Shimmer (`f_0202f2a8`) | six short lines (111..220) and a 4,241-sample pitch-shifter line |
| 5 | Cloud (`f_0202f46e`) | a 5,224 line tapped at 862..3,969, a 3,969 line, all-passes of 328, 990, 1,384, 2,042, 2,455 |
| 6 | Analog (`f_0201d7e6`, type 11, max 781) | 32 Hz, 300 Hz, 1.2 kHz and 4 kHz filters (the bucket brigade's tone) |
| 7 | Duck (type 12, max 789) | envelope times from `ln(0.001)` (the ducker) |
| 8 | dTape (type 13, max 790) | wow and flutter (`Speed`, `Depth`), `Grit` |
| 9 | Dual (type 14, max 478) | two heads (20 16-bit reads a pass) |
| 10 | Lofi (type 15, max 782) | an EQ curve, 9 biquad sections (16.2) (615 Hz -8.4 dB, 738 -6.2, 749 +3.1, 1.6 k +2.4, 4.1 k -1.0, 6.2 k -5.5, 6.7 k -3.8, 7.2 k ...) |
| 11..17 | Chorus, Phaser, Tremolo, Flanger, Vibrato, Univibe, Autofilter | full rate, two states (left, right), mixed by `f_01c08d08` |
| 18, 19 | MW-GOLD, MW-RAT drives | their oversampled clipping stages (section 10.2; first taken for amp set-ups) |

All five delays share one init (`f_0201cc56`, the type 11..15 passed in, and a maximum time 478..790, **likely** in
ms) and run at 22,059 Hz in 16-bit lines.

### 8.3 The cabinet

CAB init (`f_02009da6`): 20 models, IRs of 1,024 taps convolved in 64-frame partitions (the same sizes as OpenPocket's
USER IR, v38), low cut from 20 Hz, high cut to 18 kHz.

### 8.4 The two overlays

Overlay A (7,232 B, the fixed-point kernels) is loaded by the AMP's init: the A2-Lite network (section 4).
**Corrected in the sixth pass (section 11.4):** overlay B (6,756 B) is not the tuner. The loader `f_0200774c(n)` takes
n = 1 for A (from flash `0x2093AE8`) and n = 0 for B (`0x2092084`), both into the same window at `0x1C22340`; the AMP's
init asks for A when the model is a `NAM` file and for **B for every other model: B holds the `AM2`..`AM5` (GRU)
kernels**. The tuner page (`f_0200f0ca`) calls the loader with n = 2, which only releases the window: the 7 KB are then
free for the tuner while the amp is silent.

### 8.5 Which core runs `wet_audio`

The task is created without a core: JieLi's task creation pins a task only when its name starts with `#C0` or `#C1`
(`f_0205d0f6` reads the digit), and no task name in the firmware does (only `IDLE0` / `IDLE1` are per core). So
`wet_audio` runs on whichever core is free: with the audio interrupt on CPU0, **likely** mostly CPU1.

### 8.6 The reverbs and the delays, read instruction by instruction

**The reverb core, shared by five of the six reverbs** (`f_01c08260` in the RAM library, called through `f_01c05ae4`;
set up by `f_0202e826`). Per sample, at 22,059 Hz, in float:

1. one LFO: a 32-bit phase accumulator read through a 256-entry quarter-wave sine table (`0x1C0E204`), interpolated;
   its depth is smoothed every sample (one-pole, 1.44e-4). Two offsets come out of it, `(1 + sin) x depth` and
   `(1 - sin) x depth`;
2. **eight delay lines**, each read at its length plus one of the two offsets (alternately), with a 4-point Hermite
   (cubic) interpolation, and each followed by **its own Schroeder all-pass** (a gain per line): every line is
   modulated and diffused, not only four;
3. an optional per-sample hook on the eight outputs (`[ctx + 404]`): the Shimmer passes its pitch shifter here
   (`f_02030500`), the others pass nothing;
4. an **8 x 8 Hadamard** mix (fast Walsh-Hadamard butterflies, x 0.35355 = 1/sqrt 8);
5. the outputs, **in stereo**: left = the sum of the eight, right = (first four) - (last four), both x 0.35355;
6. each line is written with `input x its input gain + its mixed output x its feedback gain` (the decay).

Each type puts its own front end before it and the same post after it (two one-pole filters a channel, high-pass then
low-pass, `f_01c07f22`, set by HPass and LPass):

| Type | Before the core |
| --- | --- |
| Room | a 5,132-sample line read at 16 taps with their own gains: 12 taps make one signal, 4 another; each goes through 4 all-passes (29, 113, 209, 308 and 68, 156, 254, 363, gains +-0.6, `f_01c07f8e`); their sum feeds the core, and is also added to both outputs (the early reflections) |
| Hall | a pre-stage of its own (`f_01c081ca`, `f_020303c6`), then the core |
| Swell | an input envelope (RiseT, `f_02030850`) and **two** cores |
| Spring | the input **convolved with a spring impulse response** by the same partitioned FFT convolution as the cabinets (`f_01c04f9a`), then the core |
| Shimmer | the core with the pitch shifter in its hook (above), three all-pass pairs |
| Cloud | no core: four modulated lines read with a Hermite and a fixed 4th-order low-pass each (`f_01c0804c`), with `asinh` saturation (`f_01c05a18`) in the loop |

**The delays** (block 9, Dual, read in full; the others call the same LFO, `f_01c05098`). Per sample at 22,059 Hz:

1. each head's length moves to its target by a one-pole (1.44e-4 a sample): turning TIME never jumps;
2. each head is read from a **16-bit** line with a 4-point Hermite whose differences are taken **in integers**
   (`3 x (b - c) - ...` on the int16 samples) and only then converted to float; a read on a whole sample skips it;
3. the heads are summed (0.9 and a set gain, x 1/32768) and **soft-clipped through the `tanh` table** (x 64.7, the
   512-entry table of section 7.5): that is the wet output;
4. the feedback goes through **two biquads** (the tone) and a short **modulated float delay** (Speed, Depth: the
   LFO above, Hermite read): the wow sits inside the loop, so it builds up on each repeat;
5. the line is written with `input x gain + feedback x gain`, scaled by 32768 and **saturated to 16 bits** (`sat16`).

So the stock delays and reverbs are float DSP on 16-bit (delays) or float (reverbs) memory, at half rate, with
every time and depth smoothed per sample and every read a cubic. OpenPocket's (v35) are of the same family: an 8-line
Hadamard FDN at half rate, a 16-bit delay line, Hermite reads. What the stock ones do that OpenPocket's do not:

- all eight lines modulated, each with a nested all-pass (OpenPocket: four modulated, diffusion only before the tank);
- **stereo** reverb output (sum and difference); OpenPocket's output is mono on both channels;
- the delay's output soft-clipped by `tanh` and a modulated delay inside the feedback loop;
- the Spring as a convolution with a real spring's impulse response, ahead of the tank.

## 9. Not done yet

- The meaning of the order bytes in a preset (slot to category or the reverse: the default is the identity).
- The modulation blocks' per-sample routines (8.6 covers the reverbs and the delays), and Hall's and Cloud's front ends in
  full.
- ~~The six amp set-ups' circuits, and the `AM2` / `AM3` / `AM4` / `LMT` model formats.~~ Done in the fifth pass (sections
  10.1 and 10.2): the six "amp set-ups" are the six MW drives, and the formats are read.

## 10. Fifth pass: the amp, the model files, the drives, the mods, the flash, MIDI

Read instruction by instruction unless marked **likely**. Two earlier claims were wrong and are corrected in place: the
"6x oversampled amp" of section 4 is the **MW-808 drive**, and the "six amp set-ups" (table `0x205502C`) and code blocks
18 and 19 are the **six MW drives**.

### 10.1 The amp and its model files

**The AMP descriptor** (`0x2046D38`; knobs Gain, Level, Bass, Middle, Treble) has init `f_02008692`, set `f_02009c66`,
process `f_0202d7d8`. All 20 amps are **captured models read from the flash**: there is no circuit simulation of an amp
in the firmware.

**Process** (`f_0202d7d8`), per block:
1. input gain, smoothed (`0.97 / 0.03` one-pole);
2. a soft knee: `|x| <= 0.5` passes, above it `0.5 + 0.5 * tanh(2 * (|x| - 0.5))` (the 512-entry table), sign kept, so the
   input never exceeds 1.0;
3. three stage functions through pointers (`ctx+156/160/164`), filled by the init from the model's type; unused stages
   point at an empty routine;
4. a dry/processed blend from byte 18 of the model's header (0..100: below 50 the processed part is scaled by `2 x b/100`,
   above it the dry part by `2 - 2 x b/100`); forced to 100 for NAM files;
5. the tone stack (`f_02007d68` designs it with a shelf designer `f_02007abc` and a peak designer `f_02007c4e`, Q 0.707;
   `f_01c057e4` runs it): three biquads (**likely** low shelf, mid peak, high shelf), each described by `{frequency, min dB, max dB}` **taken from the model's header** (bytes 24..59). The default, used for NAM
   files, is `{150 Hz, -20, +20}`, `{425 Hz, -15, +15}`, `{1800 Hz, -10, +10}` (`0x205553C`);
6. Level (`f_01c0942e`).

**The model file validator** (`f_0200216e`) recognises five formats by their first four bytes; every float is checked
for NaN and infinity. All have a 60-byte header:

| Magic | Type | Body after the header | Size |
| --- | --- | --- | --- |
| `AM2`, `AM3` | 2 | 1,536 floats | 6,208 B, or 6,272 with an `LMT` trailer |
| `AM4`, `AM5` | 4 | 1,952 floats | 7,872 B, or 7,936 with an `LMT` trailer |
| `NAM` | 16 | 1,871 floats (A2-Lite, section 8) | 7,548 B, or 7,836 with a `BEND` trailer |
| `CAB` | 32 | 1,024 taps in **signed 24-bit**, little endian, scaled by 2^-23, header 20 B, byte 18 = 1 or 2 | 3,092 B |

**What `AM2`/`AM3`/`AM4`/`AM5` are** (the init's type-specific set-ups): a **Wiener-Hammerstein model with a small GRU** in
the middle, run in fixed point.

| Part | `AM2` / `AM3` | `AM4` / `AM5` |
| --- | --- | --- |
| input FIR | 256 taps | 512 taps |
| GRU | 16 units, 1 input: 48 input weights, 3 x 16 x 16 recurrent, 16 output weights, **no biases** (832 floats) | the same with the two bias vectors (`b_ih`, `b_hh`, 3 x 16 each; 928 floats) |
| output FIR | 256 taps | 512 taps |
| more | three 64-tap filters (reversed, FFT'd) | — |

The FIRs are turned into 64-frame FFT partitions (`f_02008098`) like the cabinets. The GRU's weights are converted to
**Q21** (`x 2,097,151`) into rows of 17 words (`f_02008114`); the gate order is r, z, n (the n gate keeps `b_hn` apart,
as PyTorch's GRU). The `LMT` trailer is `"LMT\0"` and 16 floats: `f_02008380` stores them as a second 16-weight output
row and takes their dot product with the first (**likely** a limit or level head; its use was not followed further).
An `AM4` with `LMT` gets one more stage (`f_020302e2`) and two filters at about 15 Hz and 500 Hz (`f_020084a2`,
`f_020084f0`).

**NAM's `BEND` trailer**: `"BEND"` and 72 floats, read as 24 triplets scaled by 2^20 into each layer's record (48-byte
records). **Likely** per-layer fixed-point ranges (calibration); without it the init uses defaults.

### 10.2 The drives (category 0)

The ten models of category 0: Noise Gate++ (init `f_0200a18e`), Boost (`f_0200ae00`), Compress (`f_0200b1fa`), AI Gate
MS (`f_0200acf4`), and six **MW drives**. Each drive's init is a stub calling `f_0201da90(i)`, which checks the RAM
(module registry, below) and calls `0x205502C[i](44118.0)`; all six share the set function `f_02020790` and the process
`f_02020b24`, which calls `0x204FD70[i]`:

| i | Drive (knobs) | Set-up | Process | State |
| --- | --- | --- | --- | --- |
| 0 | MW-808 (4) | `f_0202cfd0` | `f_0202d236`: 2x then 3x up, a rational asymmetric diode curve, down (section 4) | 1,312 B |
| 1 | MW-GOLD (4) | `f_0202cbfc` | code block 18 at `0x1C25200` | 2,560 B |
| 2 | MW-OCD (6) | `f_0202c962` | `f_0202ca82` | 1,792 B |
| 3 | MW-RAT (4) | `f_0202ce20` | code block 19 at `0x1C252AE` | 2,192 B |
| 4 | MW-MUFF (4) | `f_0202d4d0` | `f_0202d60e` | 1,952 B |
| 5 | MW-FACE (3) | `f_0202c874` | `f_0202c8de` → `f_01c0556c` | 3,904 B |

GOLD, OCD, RAT and MUFF go through one oversampled shaper, `f_01c0549a`: up 2x (`f_01c0758a`) and 3x (`f_01c076d0`),
three curves (`f_01c07e10`, `f_01c07e92`, `f_01c07edc`), down 3x and 2x; around it, biquads (`f_01c0944a`) and ramped
gains (`f_01c0796c`). FACE has a chain of its own (`f_01c079c8`, `f_01c07a1e`, `f_01c07bbc`, `f_01c07d8c`). So every
stock drive clips at **6x the rate (264.7 kHz)**. The circuits' coefficients were not tabulated.

**The module registry** (`0x2050018`, 53 entries, checked by `f_0200210c` against `{44,118 Hz, 64 frames}`): the RAM
each module needs, so the firmware can refuse a model it has no room for. Gates/dynamics 400..624 B; delays (ids 11..15)
50..82 KB; reverbs (16..21) Room 56 KB, Hall 57 KB, Swell 51 KB, Spring 73 KB, Shimmer 63 KB, Cloud 79 KB; mods (38..44)
Chorus 3.7 KB, Phaser 1.1 KB, Tremolo 0.7 KB, Flanger 1.6 KB, Vibrato 4 KB, Univibe 1.1 KB, Autofilter 128 B; drives
(45..50) 1.7..4.3 KB; id 51 (**likely** NAM) 70 KB; id 24 (**likely** the cabinet) 133 KB.

### 10.3 The modulations

All seven run at full rate in stereo through one wrapper (`f_01c05924`): each channel is copied to a scratch buffer, run
through the type's kernel (code blocks 11..17), and mixed with the dry signal by `f_01c08d08`. The **Mix** knob is
smoothed (`0.0183` a block) and becomes **equal-sum gains**: below 50 % dry 1 and wet `2 x mix`; above it dry
`2 - 2 x mix` and wet 1.

**Chorus** (block 11, read in full):
- a 32-bit phase accumulator LFO into a 512-entry sine table (`0x1C0F818`, linear interpolation);
- depth smoothed per sample (`1.44e-4`), the delay position smoothed again (`0.0143`);
- the line read with a 4-point cubic interpolation;
- the input is coloured before it enters the line, as a bucket brigade would: `10 x tanh(0.1 x + 0.1) - 0.987`, a gentle
  asymmetric saturation with its DC removed, then a two-pole low-pass;
- the kernel outputs the wet signal only.

**Phaser** (block 12): works in 16-sample chunks: LFO (`f_01c05dca`), all-pass stages (`f_01c05e2c`), coefficient tables
chosen by the stage count (up to 4 tables at `0x2054870 + 1604`); a change of rate, depth or stages re-designs it
(`f_020308fc`); two output filters. **Univibe** (block 16) is the same frame with its own LFO (`f_01c05eaa`), the
classic four-stage phaser with an unequal, lamp-like sweep (**likely**). **Tremolo** (block 13) and **Autofilter**
(block 17, with the table sine and cosine `f_02007992`, `f_02007a22`) are short kernels. **Flanger** (block 14)
and **Vibrato** (block 15) are the long ones (284 and 260 instructions): modulated lines with feedback (Flanger: gains
0.6..0.95, the five LFO shapes of `0x2054EB4`), Vibrato with the `tanh` input colour of the Chorus (`6.4`) and a
512-entry table. They were not read line by line.

### 10.4 What the stock firmware keeps in the flash

**Read** (`f_02022e82`, at boot): the user data is in two files of the flash filesystem, **refused unless they sit at
these addresses**:

| File | Flash | Size | What |
| --- | --- | --- | --- |
| `EXT_RESERVED/usr` | `0xBA000` | 12 KB | not followed |
| `EXT_RESERVED/pd40j` | `0xBF000` | 256 KB | the user area, below |

Inside `pd40j`:

| Offset | Size | What |
| --- | --- | --- |
| `0x00000` | 20 x 8 KB | the 20 amp models, each validated: only types 2, 4 or 16 (`AM2/3`, `AM4/5`, `NAM`) |
| `0x28000` | 20 x 4 KB | the 20 cabinets, type 32 (`CAB`) |
| `0x3C000` | 8 KB | the preset bank: 80 presets x 92 B (section 7.4), then `PATCHEND`: 7,370 B in all |
| `0x3E000` | 4 KB | `GLB1`: the global settings, as a log |

So the stock amps and cabinets are **files the user can replace** (the editor's "PC AMP", "PC NAN" and "PC CAB" pages
name them); the firmware has no built-in amp or IR.

**`GLB1`** (`f_0200c578`): the magic, then up to 511 records of 8 bytes appended one after the other (wear levelling);
the last record that is not all `0xFF` wins. A record: byte 0 = 0; bytes 1 and 2 = two preset numbers 0..79 (**likely**
the current preset and the other one of the A/B pair); byte 3 = 0/1; byte 4 = the input gain, -10..+10 dB (shown as
`%s%d dB`); byte 5 = the MIDI channel 0..15, or 255 for OMNI; byte 6 = BT on/off; byte 7 = Loopback on/off (**corrected in the
sixth pass**: the settings page reads them so, section 11.6; the tails are not in this record).

### 10.5 MIDI

**Read** (`f_02014dfe`, the `midi_route` task's handler): incoming messages wait in a 32-entry ring (3 bytes each, the
channel already filtered by the OMNI/CH setting).
- **Program Change n**: loads preset n (`f_0200c8d0`).
- **Control Change**: the CC number indexes a 128-entry table (`0x2046704`, 12 bytes an entry: handler, name, argument),
  the same table as the footswitch functions. The value goes to the handler.

| CC | What | Value |
| --- | --- | --- |
| 0..5 | NS, AMP, MOD, DLY, REV, CAB on/off | toggles; as read, the handler acts only on value 0 |
| 20..27 | virtual keys 0..7 (the footswitches) | non-zero = press, 0 = release |
| 28 | tuner mode | |
| 40, 41, 42 | MOD tap, DLY tap, MOD+DLY tap | |
| 48..53 | NS, AMP, MOD, DLY, REV, CAB **model** | the model number, clamped to the category's count |
| 54..89 | **effect parameters**: `54 + 6 x unit + param` (units NS, AMP, MOD, DLY, REV, CAB; params 0..5) | 0..127, scaled to 0..100 with rounding |

### 10.6 Bluetooth, small things

- The advertising data (`f_020155f6`) carries the flags, the **BLE-MIDI** service UUID and the 5 bytes `sinco` (**likely**
  a maker tag: the string next to it is `sincoCAB`); the device name goes in the scan response (up to 29 bytes).
- Tasks `bt_sbc_dec` and the string `JL_A2DP`: the stock firmware also **receives Bluetooth audio** (A2DP, SBC), mixed
  into the output by the audio interrupt (section 9 of the handbook).
- OTA file names are present (`ble_app_ota.bin`, `spp_app_ota.bin`): updates over BLE and over SPP.

### 10.7 What remains not done

- Flanger, Vibrato, Tremolo, Autofilter line by line; the drives' curves and filter values; Hall's and Cloud's front ends.
- The `LMT` and `BEND` trailers' exact use; the third and fourth bytes of `GLB1`; the `usr` file.
- The tuner's pitch algorithm (`tuner_task`), the USB Audio loopback, the battery thresholds, the preset order bytes.

**Section 11 (sixth pass) takes up this list.**
- The BLE app protocol beyond BLE-MIDI (SysEx used by M-VAVE's app).

## 11. Sixth pass: the list of section 10.7, read

Two more corrections, made in place: overlay B is the `AM` models' kernels, not the tuner (8.4); bytes 6 and 7 of a
`GLB1` record are BT and Loopback, not the tails (10.4).

### 11.1 The modulations, line by line

**Tremolo** (block 13, read in full; runs in pieces of at most 64 samples):
- a 32-bit phase into the 512-step sine of `f_01c03562` (a 256-entry quarter table, `-1..+1`), linearly interpolated;
- `gain = 1 - 0.499 x depth x (1 + sin)`: from 1 down to 0.002 at full depth;
- the gain itself goes through a one-pole of 0.1 a sample (it softens the edges; the wave is a sine anyway);
- depth and level are smoothed by `0.00143` a sample; level is clamped to 0..2 and multiplies the output.

**Autofilter** (block 17, read in full): a **band-pass biquad swept by a sine LFO, its coefficients recomputed every
sample**:
- `f = fmin + (fmax - fmin) x (1 + sin) / 2`; `K = tan(pi f / fs)` from the table sine and cosine (`f_02007992`,
  `f_02007a22`);
- `b0 = K x norm`, `b2 = -b0`, `a1 = 2 Q (K^2 - 1) x norm`, `norm = 1 / (K + Q + K^2 Q)` (`Q` from the knob), direct form I;
- output = `5 x mix x band-pass + (1 - mix) x dry`: the band-pass is made up by 5x (+14 dB).

**Flanger** (block 14, read in full), per 16-sample piece then per sample:
- knobs: rate `0.17..16 Hz`; depth `0..0.8`; feedback `0.2..0.8`; base delay **2..7 ms**; one of the **five LFO shapes**
  (`0x2054EB4`, phase to 0..1);
- every value is smoothed (`0.0046`, `0.00183`, `0.0143` a sample);
- `delay = base x (1 - depth x (1 - LFO))`; the line is **16-bit**, read with the integer-difference Hermite of the
  delays (8.6);
- the line's input is `x + feedback x delayed`, then: a first-order filter; a gentle asymmetric cubic,
  `0.95 y - (y^2 + y^3) / 64`; the `tanh` table; x 0.5; two biquads; `x 32768`, `sat16`;
- the kernel outputs `line input + delayed x 0.6 (1 - feedback)` (wet only, mixed by the wrapper).

**Vibrato** (block 15, read in full): a **tape-style vibrato**, 100 % wet, with a lo-fi knob:
- the LFO is **not a sine**: a fixed 513-point waveform at `0x2053748`, smooth but irregular (two shallow dips to 0.70
  and 0.73, one deep dip to 0, flat at 1 for the last quarter), a wow curve;
- `delay = depth x (line length - 2) x LFO`, read with the 4-point cubic of the Chorus;
- before the line: the Chorus's input colour (`tanh(0.1 x + 0.1)`), then the **bit depth** cut by a knob `b` = 0..20
  (`b` least significant bits of a 23-bit word cleared), a filter, and a crossfade from clean to reduced of
  `(1 - sin(2 pi (0.25 + 0.025 b))) / 2` (0 at `b` = 0, 1 at `b` = 20), with a make-up gain of `b/3` dB (up to 5 dB at
  `b` = 15, back to 0 at 20).

**Phaser and Univibe** (blocks 12 and 16, the coefficient designer `f_01c08b7a` and the section runner `f_01c05e2c`
read): **not all-pass stages but peaking EQs**, a model of a phaser's response:
- 3 sections (a switch: 5), each an RBJ-style peaking biquad (`A = 10^(dB/40)`, `alpha` from `1 / (2Q)`);
- centres **245, 550, 1234 Hz** (5 sections: **200, 319, 550, 950, 1640 Hz**), all multiplied together by
  `1 + 3 x LFO` (a sweep of two octaves), the LFO one of the five shapes;
- gains and Qs blended by a knob between two sets: notch/peak/notch **-16 / +11.5 / -16 dB, Q 2 / 0.6 / 2** and
  **-3 / +15 / -3 dB, Q 0.4 / 4 / 0.4** (5 sections: -16, +11.5, -17, +10.5, -16 and -3, +21, -3, +20, -3);
- the coefficients glide to their new values every 16 samples;
- the Univibe uses the same tables with **the gains' sign inverted** (`f_02030986`: `-0.08305` against `+0.08305`, so
  peaks become notches) and its own LFO routine.

### 11.2 The drives' shared clipper

`f_01c0549a` (GOLD, OCD, RAT, MUFF), read in full:
1. the bias glides to its target;
2. up 2x and 3x, then `f_01c07e10` on the 96 samples, an **asymmetric rational clipper with two knees**:
   - `s = (x + bias) x g`; below the upper knee `T1` the sample passes;
   - above it `y = (x + bias) x k2 + c2 - 1 / (s x k3 + d1)`;
   - below the lower knee `-T2`, `y = (x + bias) x k2 - c3 - 1 / (y x k4 - d2)`;
   - ten constants per drive (`g, k2, k3, d1, c2, T1, k4, d2, c3, T2`), one division a sample;
3. down 3x and 2x;
4. optional, by flag: a one-pole **DC blocker** (`f_01c07e92`) and a one-pole **low-pass** (`f_01c07edc`).

The ten constants and the filters come from each drive's knob functions, which are **circuit fits**, not tables:
- MW-808: `2.6104`, `0.1149`, 17.8 kHz and 22 kHz poles, `6.94`, `189.2`, `21.07`, `210.26`;
- MW-OCD: a table of 16 numbers that read as frequency and level pairs (103, 32.69, 28.78, 257, 36.32, 132, 33.74, 89,
  27.33, 89, 22.38, 79, 32.69, 186, 28.78, 146; the pairing was not confirmed), `173.78`, `-12.99`;
- MW-RAT: `3.65`, `1.876`, `4.123`, `1.758`, `-11.52`, `27.13`, `7.45`, `13.62`;
- MW-MUFF: `2.49`, `1.061`, 250 Hz and 525 Hz (`1570.8` and `3298.7` rad/s), `40.5`, `-26.6`, `0.8`, 350 Hz;
- MW-FACE (**likely**: these two functions are called from the shared set routine): two **4th-order polynomials** of the knob (`3909 - 23084 k + 52295 k^2 - 50424 k^3 + 18304 k^4`, and
  `0.7345 - 4.364 k + 9.830 k^2 - 9.386 k^3 + 3.285 k^4`), the shape of a Fuzz Face's pots (**likely**).

### 11.3 Hall and Cloud, before the core

**Hall** (block 1 and its set-up `f_0202ec0c`, read): lengths are given at 44.1 kHz and scaled by `0.5002` to the
22,059 Hz of the reverbs.
- A line of 2,470 samples (112 ms) read at **17 taps**: 2036, 2357, 2397, 2555, 2722, 2893, 2924, 3266, 3496, 3639,
  3873, 3887, 4250, 4268, 4479, 4649, 4937 (44.1 kHz samples: 46..112 ms).
- Each tap has its own signed gain (0.022, 0.056, -0.032, 0.079, -0.089, 0.112, -0.126, -0.112, 0.112, 0.079, -0.178,
  0.126, -0.089, -0.178, -0.141, -0.089, -0.178).
- Each tap feeds **its own all-pass of 114 samples (44.1 kHz), gain 0.75**; the 17 outputs are summed into the core.
- When every tap is longer than the block, the block is done tap by tap (a fast path); otherwise sample by sample.

**Cloud** (block 5, read in outline): two sine LFOs; two modulated lines read with Hermite; cross gains in dB steps
(0.891, -0.944, 0.841, 0.798, 0.891, -0.708, 0.631); then the four modulated lines of `f_01c0804c` (fixed 4th-order
low-pass each); `x 0.707`; `asinh` on both outputs; the same post as the others. Lengths: a 5,224 line tapped at 862,
1126, 2326, 2913, 3169; a 3,969 line; all-passes of 328, 990, 1384, 2042, 2455.

### 11.4 The AM kernels (overlay B), `LMT` and `BEND`

**Overlay B** holds the `AM` models' code (section 8.4 corrected).

**The per-sample GRU** (`0x1C22340`, read):
- the input is converted to Q21;
- **three jobs on the FFT block** (`0x1EE0400`: a job word `2721`, `CON = 0x149`, wait for bit 7, clear with bit 6)
  compute the 16 x 16 recurrent products;
- four CPU routines do the gates;
- the output is converted back (`x 2^-21`).

The stock firmware uses the FFT block as a matrix-vector engine **for its GRU**, the same trick as OpenPocket's NAM.

**The AM4/AM5 stage** (`0x1C224D0`, read): the input FIR, then the GRU per sample, then the output FIR.
- **Warm-up after loading**: for the first 1,102 samples the GRU is fed a decaying exponential and its output is
  muted (`x 1e-16`). The last output is kept as the model's DC offset and subtracted from then on.
- **With `LMT`**:
  - an envelope follower on the input (smoothed by `0.0183`) sets a gain applied around the network;
  - two filters are designed at 2 kHz and 3 kHz;
  - the init adds a first-order high-pass at 15 Hz and a first-order filter at 500 Hz;
  - a set routine (`f_020302e2`) maps three knob bytes to `0..3`, `-6..0 dB` and `-24..0 dB`;
  - the `LMT` weights are a second 16-weight output row; their dot product with the first is precomputed.
  - So `LMT` is a level-dependent stage (a limiter or a level-dependent tone); its exact law was not reduced to a
    formula.

**`BEND`**:
- 72 floats: one triplet for each of the 23 A2-Lite layers and one for the head;
- each triplet is converted to Q20 and copied to **every channel of its layer**;
- **corrected in the seventh pass (12.1)**: the triplets are not activation parameters; they pre-fill each layer's history
  ring (3 channels a frame) with its steady state.

### 11.5 The tuner

`f_0200f0ca` sets it up: the overlay window released, a pitch detector with `A = 440 Hz`, 0.01 thresholds, a window
from the sine table. `tuner_task` (`f_0202bc8c`, priority 3, 512-word stack) reads the 3 kHz ring of section 7.2.
**Read in outline**:
1. a 2,048-sample frame, windowed;
2. a spectrum through the FFT helpers; the magnitude squared (`re^2 + im^2`) of each bin;
3. the peak bin, refined with a 3-point interpolation;
4. **nine refinement passes** of a single-frequency DFT (sums of `x sin` and `x cos` at the candidate frequency)
   around the peak;
5. the result folded into an octave (doubled or halved until it fits);
6. the cents: `1200 x log2(f / ref)`, with a window of `2^(+-1/24)` (a quarter tone) around the note.

### 11.6 Settings, tails, loopback, battery

**The settings page** (`f_0200de68`) reads a struct whose bytes are the `GLB1` record:
- byte 4: MIDI channel (`CH n` or `OMNI`);
- byte 5: **BT** ON/OFF;
- byte 6: **Loopback** ON/OFF;
- the **input gain** (`%s%d dB`, signed): a state byte.

**The tails** (one state byte, set by `f_0200de08`): bit 0 Delay Tail, bit 1 Reverb Tail, bits 2 and 3 their mode;
each is **Off, Auto or Force** (strings at `0x204FF24`); a mode bit is refused without its on bit.

**Loopback**: the audio interrupt has two output mixers, `f_02029ede` and `f_02029b50` (USB record and playback paths),
chosen by a switch. What Loopback routes was not followed to the register.

**Battery**: the "Low Battery" screen is built by `f_0201adb0`. The level comes from the SDK's `adc_scan` /
`sys_power` services, not from application code; no threshold was found in the app.

### 11.7 The preset's order bytes

`f_020031f0` copies the six order bytes to the slots' categories (`state + 1010 + i`), then initialises each slot from
the category it holds. So **order[i] is the category that runs in slot i**: the chain runs slot 0 to 5, and the
default `0,1,2,3,4,5` is gate/drive, amp, mod, delay, reverb, cab. A preset with `0,1,5,2,3,4` would put the cabinet
before the modulation (**likely** as the editor offers it; the check `f_02001d74` only requires a permutation).

### 11.8 M-VAVE's SysEx protocol

**Read** (`f_02025b12` assembles `F0 .. F7` from USB or BLE MIDI; `f_02025100` handles it).

A message, between `F0` and `F7`:

| Byte | What |
| --- | --- |
| 0, 1 | `00 59` (the header) |
| 2 | command |
| 3..5 | payload length, 24-bit little endian |
| 6.. | payload |
| last | checksum: `~(sum of the payload bytes)`, 8 bits |

The total must equal the length + 7. The payload is **8-bit**, not packed to 7 bits.

| Command | What |
| --- | --- |
| `0x11` | identify: the reply carries 27 bytes, the name `BlackBox_021` |
| `0x12` | a 6-byte status reply (all zero) |
| `0x15` | stream bytes into a ring buffer and wake `midi_route` (a data channel) |
| `0x21` | object `payload[0]` (0..7), 32-bit argument: its handler 0 |
| `0x22` | object, 24-bit address: its handler 2, then an 8-byte reply |
| `0x23` | object, 24-bit address, up to 1,024 bytes: its handler 1, reply with the data |
| `0x30` | address and data: copied into a buffer and **written by the flash task** (`f_0205ca82`), with a busy handshake |

Errors get an 8-byte reply with status 1. Only objects 4 and 5 are registered (section 12.2).

### 11.9 What is still not done after this pass

- The exact law of `LMT`, and which kernel reads `BEND`'s triplets.
- The coefficient formulas behind each drive's knobs (the constants are above, not the circuits).
- Cloud and the Phaser's designer in full detail; the tuner's thresholds.
- Loopback's routing; the battery threshold (in the SDK's power service); the SysEx object list.
- The 12 KB `usr` file.

## 12. Seventh pass: the five open points

### 12.1 `BEND`: the layers' resting state

**Read.** The init (`f_02008692`, from `0x2009A1E`) takes each of the 72 floats of the trailer in threes, converts them to
Q20, and **writes the triplet into every frame of the layer's history ring** (`[layer + 56]`, `count + 72` frames of
12 bytes). A2-Lite's layers have 3 channels and their rings are interleaved `[time][channel]`, 12 bytes a frame, as in
OpenPocket's `engine/px_nam.c`. The `+ 72` is the runner's largest piece: `0x1C23464` runs the network in pieces of at
most 72 samples.

So `BEND` is **each layer's output at silence, used to pre-fill its history**: 23 triplets for the 23 layers, one for
the head. A model loaded with it starts at rest, with no thump and no warm-up. Without it the init clears the rings
(`memset` of 72,000 and 30,156 bytes) and the network has to settle by itself, as the `AM` models do with their
1,102-sample warm-up (11.4).

The input of the network is also read: `x 2^20` to Q20, with saturation at `+-2^31`, and every clipped sample counted
(`0x1C2389C`).

### 12.2 The SysEx objects

**Read.** Only two objects are registered (`f_020226e4`, at boot): **4** and **5** (`[state + 459 x 4]` and
`[state + 460 x 4]`, i.e. `0x1C1B67C` and `0x1C1B680`). No instruction writes the other six slots, so commands `0x21`
to `0x23` on objects 0..3, 6 and 7 get the error reply. Each object is three handlers: `+0` for command `0x21`, `+4`
for `0x23` (read), `+8` for `0x22` (write).

**Object 5, a memory-mapped file service** (ops at `0x2054C8C`: `f_02006F16`, `f_02007400`, `f_02007534`). Reads and
writes take a 32-bit address; the top nibble picks the space.
- **`0x7xxxxxxx` = the `pd40j` file, by offset**: amp models `0x00000..0x27FFF` (8 KB slots), cabinets
  `0x28000..0x3BFFF` (4 KB slots). Reads and writes are queued to the main loop (`f_02013260`), which validates a
  model with `f_0200216e` before it is accepted.
- **`0xD0000000`** = a 32-byte descriptor of the map, `PMV1` (`0x20583E0`): version 1, then sizes `0x3000` (12 KB, the
  `usr` file), `0x2000` (an amp slot), `0x2000`, `0x1000` (a cabinet slot), and 7 (the `pd40j` nibble).
- **`0xE0000300`** = a 36-byte status block, `PDB1`: version 1, size 36, three state bytes, a counter, a time, a pointer.
- **`0xE00004xx`** = a 28-byte radio record (the advertising buffer of 10.6).
- **`0xE0000500..0xE00006FF`** = a 512-byte buffer.
- **`0xE0000700..0xE000071F`** = task statistics (`app_core`).
- Other spaces (`0x0..0x1`, `0x2`, `0x8`, `0xA0000000`, `0xF`) are handled in `f_02013260` and were not mapped one by
  one.

**Object 4** (ops at `0x2054C98`: no `0x21`, `f_02006E3E` for reads, `f_02006EAA` for writes) queues requests of types
4 and 5 to the same main-loop engine: a second stream (**likely** the live preset and settings).

### 12.3 Loopback

**Read up to the mixer.**
- The setting (`state + 1052`) is copied by the main loop (`f_02016150`) to byte 7 of a 10-byte routing struct at
  `0x1C0B510` (`0x1C0B4A0 + 112`), **inverted**: Loopback ON writes 0, OFF writes 1. Bytes 0 and 1 of that struct are
  127, two volumes.
- The audio interrupt reads the struct every block and passes byte 7 as the **mode** (0..3) to the output mixer
  `f_02029ede`. Modes 2 and 3 are set by other states (2 resets the USB rings, `f_020296bc`; 3 copies the block to a
  meter, `f_0202978c`).
- In modes 1 and 2 (Loopback OFF), with the computer's playback active, the processed block is sent to the USB record
  ring (`f_0202978c` through `f_020298ce`): **the computer records the pedal's sound**.
- In mode 0 (Loopback ON), the mixer first copies the output block (512 bytes) and then mixes the computer's playback
  into it (`f_01c071b0`).

Settled in the eleventh pass (16.3): with Loopback ON the computer records the pedal **plus** its own playback; OFF, the
pedal alone.

### 12.4 The battery

**Read.**
- **The measurement**: the SDK's `adc_scan` (`f_02020E96`, ADC at `0x13100`) samples a channel list every tick and,
  every 100 ticks, the reference and the battery. It keeps the last 20 battery samples (`state + 2020`);
  `adc_get_value(0x5000F)` returns their average (`f_0200B7D4`).
- **The percentage** (`f_0201ADB0`, the UI loop): the raw value of ADC channel 8, **0 % below 528, 100 % above 635,
  linear between**: `(raw x 100 - 52,700) / 109`, clamped to 0..100.
- **The icon** (`f_02010BB8`): green while charging, red below **21 %**, white otherwise. Charging is detected on GPIO
  151, read by `f_02016E76`.
- **At boot** (`f_02023A20`): the supply is measured in mV, `(1023 x trim + 818,400) / (average / 10)`. Below
  **3,400 mV** the firmware changes a power setting (`f_01c0128e(5, 0, 3, 7)`).
- **The hardware low-voltage reset** is `VLVD = 4` in the package's `isd_config.ini` (read from BlackBox_020's package).
- **The "Low Battery" label**: built by the UI (`state + 444`) and hidden right after it is built (`f_02004d58(obj, 1)`).
  No other instruction reaches that object: **in this build it is never shown** (**likely**: an object could still be
  reached through a path that was not found).

### 12.5 The `usr` file: a journal for model uploads

**Read.** `EXT_RESERVED/usr` (12 KB at `0xBA000`):
- **`0xBA000..0xBBFFF`** (8 KB): the staging buffer;
- **`0xBC000`**: a 20-byte journal record:

| Bytes | What |
| --- | --- |
| 0..3 | `P4JR` |
| 4, 5 | version 2 |
| 6 | type: 0 = an amp model (8 KB), 1 = a cabinet (4 KB) |
| 7 | the slot, 0..19 |
| 8..11 | the size (8,192 or 4,096) |
| 16..19 | CRC-32 (`0xEDB88320`) of bytes 0..15 |

At boot `f_02012874` (from `f_02012B2E`) checks the record and, if valid, **copies the staged model into its `pd40j`
slot**. So an upload survives a power cut: the model is written to `usr` first, the record last, and the copy into the
real slot is redone at the next boot until it completes (**likely** for the order of the writes; the boot-side commit
is read).

### 12.6 What is still not done

- The address spaces `0x0..0x2`, `0x8`, `0xA` and `0xF` of object 5, and the exact content of object 4's stream.
- Loopback's two USB rings down to their DMA (a measurement would settle it).
- The SDK's power service beyond the boot check (no other threshold was found in the application).

## 13. Eighth pass: the SysEx address map, the USB record path, the power module

### 13.1 The full SysEx address map

**Read** (the main-loop engine `f_02013260`; request types 2..5, then a jump table on the top nibble of the address).
Object 5 queues types 2 (read) and 3 (write); object 4 queues types 4 (read) and 5 (write).

**Object 5: storage.**

| Space | Read | Write |
| --- | --- | --- |
| `0x0` | the `usr` file, 12 KB | yes (the staging buffer and journal of 12.5) |
| `0x1` | the header of the **amp model now loaded** (60 bytes: name, blend, tone-stack bands...), through `f_0201150c` | yes, applied live (the slot's set routine is called) |
| `0x2` | the header of the **cabinet now loaded** (20 bytes) | yes, applied live |
| `0x7` | the `pd40j` file by offset (12.2) | yes, each model validated |
| `0x8` | the **names of the 20 cabinets**: 20 x 14 bytes, bytes 4..17 of each `CAB` file (`f_02009cfc`: `pd40j + 0x28000 + i x 4 KB`) | — |
| `0x9` | the **names of the 20 amp models**, 14 bytes each (`f_02002d18`) | — |
| `0xA` | — | a **one-byte command**: 1, 2 or 3 starts a commit from the staging buffer (1 = an 8 KB amp model) |
| `0xD` | the `PMV1` map descriptor | — |
| `0xE` | `0x300` `PDB1` status, `0x4xx` radio record, `0x500..0x6FF` buffer, `0x700` task statistics; `0x100`, `0x200` and `0x400` are other small blocks | — |
| `0xF` | one status byte (`0x1C0B4A4`) | up to 511 bytes: **install a model into a slot** (the address's low byte is the slot 0..19, bit 8 picks amp (8 KB) or cabinet (4 KB)); the model is validated (`f_0200216e`, types 2, 4 or 16 for amps) before it is written |

So every model in the stock firmware is a **14-byte-named file**: the name sits at bytes 4..17 of the header (`AM`,
`NAM` and `CAB` alike), right after the four-byte magic.

**Object 4: the live state.**

| Space | Read | Write |
| --- | --- | --- |
| `0x0` | the **whole preset bank**, 7,370 bytes (`pd40j + 0x3C000`, 10.4) | yes |
| `0x1` | the **current preset**, 92 bytes (`state + 4076`, or the edited copy at `+ 4260`) | yes: the preset as edited |
| `0x2` | the **settings record**, 7 bytes (the `GLB1` record of 10.4, `state + 1046`) | yes |
| `0x7` | sub-spaces by bits 20..27: `7` = the tails byte and two values (`f_02012274`); `8` = a live block (input gain...) | yes |
| `0xD` | one byte, Loopback (`state + 1052`) and other flags | yes |
| `0xE`, `0xF` | — | other live controls (not followed one by one) |

That is the whole protocol of M-VAVE's app: **every preset, setting and model is read and written as memory**, through
two objects and an address space; there is no other command set.

### 13.2 The USB record path, and Loopback

**Read.** The USB Audio class's callbacks are a table at `0x205505C`. In it, `f_0202ad78` is the **record callback**:
- it reads the **1,024-frame record ring** at `0x1C18BC0` (stereo, packed 24-bit, 6 bytes a frame; write index
  `state + 118`, read index `state + 120`);
- it resamples by a fraction to follow the host's clock: below 256 frames of fill it slows down, above 768 it speeds
  up, by at most 48 steps at a time;
- `f_0202af5e` empties the ring.

The audio interrupt fills that ring through `f_0202978c`:
- with a volume from a 101-entry table (`0x204FF30`, index 0..100);
- overruns are counted at `state + 2092`.

The output mixer `f_02029ede` sends a block to the ring at **two possible points**:
- **before** the computer's playback is mixed into the pedal's output (`0x2029FC4`, modes 1 and 2);
- or **after** the mix, from a copy (`0x202A0DA`), chosen by the mode byte (Loopback) and two stream flags (bytes 8 and
  9 of the routing struct).

So Loopback chooses **whether the computer records the pedal alone or the pedal plus its own playback**. Every flag
combination is worked out in 16.3.

### 13.3 The power module

**Read.** The SDK's module table (`0x2080A10`) pairs each module with an "idle?" query used before sleep:
- `adc_scan` → `0x2020E0E` (is a conversion running);
- `sys_power` → `0x2030E7A` (one flag).

Neither holds a voltage threshold. With 12.4 that settles the battery: the only thresholds are the boot check (3.4 V)
and the icon's 21 %; the hardware LVD (`VLVD = 4`) is the only cut-off; the "Low Battery" label is never shown.

### 13.4 What is left

- Object 4's spaces `0xE` and `0xF` and object 5's `0xE0000100/200/400`, one by one.
- Loopback's two flag bytes in practice (a recording on the pedal settles it).
- Nothing else of the list in 9, 10.7, 11.9 and 12.6 remains open.

## 14. Ninth pass: the dynamics, the other delays, Shimmer, Swell, Spring, the cabinet's filters

Read instruction by instruction where it says **read**; **read in outline** means the structure and the constants,
not every line.

### 14.1 The knobs of every delay and reverb

**Read** (the descriptors' parameter tables):

| Type | Knobs (defaults) |
| --- | --- |
| Analog | Time 43, Fb 20, Mix 12, **Phaser** 0, **Pitch** 0 |
| Duck | Time 66, Fb 40, Mix 50, **Unpack** 50, Speed 50, Depth 50 |
| dTape | Time 51, Fb 14, Mix 15, **Grit** 5, Speed 50, Depth 50 |
| Dual | Time 64, Fb 30, Mix 50, **T-Mode** 7, Speed 50, Depth 50 |
| Lofi | Time 58, Fb 25, Mix 15, **Grit** 5, Speed 50, Depth 50 |
| Room, Hall | Decay 40, Mix 13, HPass 80, LPass 80, Depth 40 |
| Swell | Decay 40, Mix 30, HPass 60, LPass 60, **RiseT** 50 |
| Spring | Decay 40, Mix 13, HPass 80, LPass 80, **Combs** 40 |
| Shimmer | Decay 75, Mix 25, **Tone** 75, **Pitch** 100, **Amt** 80 |
| Cloud | Decay 40, Mix 30, HPass 60, LPass 60, **Diff** 50 |
| Noise Gate++ | Gate 50 |
| AI Gate MS | Gate 50, **Bias** 50 |
| Boost | Gate 50, Gain 50 |
| Compress | Gate 50, Sustain 50, Attack 50, Level 50 |

The time knob of every delay is scaled by 9 (`900 / 100`): 0..900 ms, clamped to the type's maximum (478..790 ms,
8.6).

### 14.2 The gate inside every dynamics model (and every drive)

**Read in outline.** Noise Gate++, AI Gate MS, Boost, Compress and the six MW drives all create the same gate object
(`f_02009ed0`, run by `f_0200a384` → the RAM kernel `f_01c027ec`, 779 instructions). The "Gate" knob of each of them
drives it. Every MW drive and every dynamics model therefore has a noise gate built in.

**What the set routines give it** (read). With `g = Gate / 100`:
- **amount** (`+460`) `= (2 - g) x g`: rises fast at first and is flat near the top;
- **bias** (`+464`): Noise Gate++ `0.5 + 0.25 g`; **AI Gate MS** `b + 0.5 g (1 - b)` with `b = Bias / 100`, the one
  difference between the two gates: the bias is a knob;
- a threshold, default **-60 dB** (0.001), and an exponent clamped to 1..1.333 (`f_0200a292`). Boost passes
  `1 + Gain / 300` there, the others 1.

**The kernel** (outline):
- a one-pole high-pass on the detector (~18 Hz, `0.99751`);
- several envelope followers and a noise-floor estimate (counters up to 100 blocks, factors 0.65 and 0.4);
- a state machine (`f_0200a426`) with hold counters;
- gains computed in the log2 domain (`dB / 6.0206`), with constants of 14 dB, 22, -8, 0.12, 0.045, 0.05 and 0.06.

It is an adaptive gate that tracks the noise floor and opens relative to it, not a fixed threshold; that is the
**likely** meaning of "AI". The full state machine was not reduced to equations.

### 14.3 Boost and Compress

**Boost** (`f_0200aed6`, read):
- gain = `2^(1.794e-4 x Gain^2)`: **0 dB at 0 to +10.8 dB at 100**, quadratic in the knob;
- plus the gate;
- the process applies the gain (`f_01c0942e`) and runs the gate.

**Compress**: read in full in the eleventh pass (16.1), which **corrects** this paragraph's first reading: the
`1.5 + 0.09 x Sustain` value is the make-up gain in dB (3..12 dB, the constant is 3, not 1.5), the ratio is
`1 + 5 S^4`, and the shelf is fixed at 0 dB.

### 14.4 The four other delays

All five share the Dual's skeleton (8.6): the time glides (1.44e-4), the line is 16-bit, read with an integer-difference
Hermite, and written saturated to 16 bits. What each adds:

- **Analog** (block 6, **read**):
  - the repeat goes through a two-pole low-pass (the bucket brigade's tone);
  - then a blend between clean and **`tanh` saturated** (`tanh(1.122 x)`, the 512-entry table), by a smoothed amount,
    before it is fed back;
  - after the line, a **short modulated float delay** (sine LFO, `f_01c03562`, Hermite read) whose output can be
    **phase-inverted** (a sign chosen by a flag): the **Phaser** knob;
  - the **Pitch** knob **likely** sets that LFO's depth (its set routine scales it by `0.025` and by 22.059 samples per ms).
- **Duck** (block 7, read):
  - an envelope follower on the dry input; when the input is above **-30 dB** (`0.0316`) the echoes are pulled down to
    **0.125 (-18 dB)**, otherwise back to 1, through a smoothed gain;
  - **Unpack** sets the release (`x 7 + 1`, through `exp(-6,907.76 / ...)`);
  - Speed and Depth modulate the line (`f_01c05098`).
- **dTape** (block 8, read in outline):
  - in the feedback, two one-poles (`0.98056`, `0.99028`) and a Butterworth section (`1 - 1/sqrt 2`, `sqrt 2 - 1`);
  - **`asinh` saturation** driven by **Grit** (`asinh(Grit x)`, built from the sqrt and log2 tables, 7.5);
  - the `tanh` table on the output (`x 71.81 / 64`);
  - wow and flutter from Speed and Depth.
- **Lofi** (block 10): read in full in the eleventh pass (16.2).

### 14.5 Shimmer, Swell, Spring

- **Shimmer's pitch shifter** (`f_02030500`, in the FDN's per-sample hook, **read**): a **two-tap rotating-delay
  ("Doppler") shifter**.
  - A 32-bit phase ramp moves a read point through a line.
  - A second tap is 180° away (`phase ^ 0x80000000`); both are read with Hermite.
  - Each tap is weighted by a **window from a 512-entry table** (`0x2050B8C`, the second tap 256 entries later), so
    the two crossfade where the jump happens.
  - The line is fed with the FDN's outputs x 0.235; the shifted sound goes back into the lines (Amt), so each pass
    is shifted again: the shimmer's rising cascade.
  - Pitch sets the ramp's speed.
- **Swell** (set `f_02030850`, read):
  - **RiseT** sets a rise of `(15.2 x RiseT + 0.8)` time units at the reverb's rate, turned into a one-pole that
    reaches -60 dB over it (`1 - 6.908 / N`);
  - the input envelope opens the reverb's input along it; Decay maps through `10^(-5 (1 - x))`.
- **Spring** (`f_0202f080`, read):
  - the impulse response is **640 taps at 22,059 Hz (29 ms)** in flash at `0x20579A0`, cut into **20 partitions of 32**,
    each FFT'd (64 points) at init;
  - its shape: it builds to a peak at 3 ms (tap 66, 0.051), decays slowly, and is faded out over its last partition;
    136 zero crossings, a dispersive, chirpy spring;
  - the convolution runs ahead of the tank, with lines of 5,440 and 12,592 and a 9.6 kHz filter (the Combs knob,
    8.6).

### 14.6 The cabinet's Lvl, Lcut and Hcut

**Read** (set `f_02009e86`, process `f_0202bafa`):
- **Lcut** = `20 + Lcut x 280 / 100` Hz: **20..300 Hz**, a **first-order high-pass** (`f_020084a2`);
- **Hcut** = `5,000 + Hcut x 130` Hz rounded down to 100 Hz: **5..18 kHz**, a **first-order low-pass** (`f_020084f0`);
- **Lvl** 0..100 (default 100);
- defaults Lvl 100, Lcut 0 (20 Hz), Hcut 100 (18 kHz): flat;
- the cabinet's process (`f_0202bafa`): level, the two filters, the partitioned convolution (`f_01c04f9a`).

**Corrected**: `f_0202f844`, which section 4 called the cabinet, is the **`AM2`/`AM3` amp stage**: the AMP init installs
it for those models (`0x2008D5C`). It convolves the model's FIR with the same library and adds two **first-order high
shelves at 2 kHz and 3 kHz** whose gains come from the model.

### 14.7 Earlier findings not yet written down

- **Tap tempo** (`f_02006d26`, read):
  - the tap period must be 1..3,000 ms;
  - **DLY TAP** gives the period to the delay's descriptor function `+20`, which turns it into the Time value of the
    delay's slot;
  - **MOD TAP** gives `1,000,000 / period` (the rate in mHz) to the modulation's, which turns it into its Rate;
  - **MOD+DLY TAP** does both;
  - the preset is marked changed only if the value moved.
- **The settings page's entries** (table at `0x2055014`): MIDI Ch, BT, Loopback, Input Gain, Delay Tail, Reverb Tail.
- **The tuner page** (`f_0200f534`): `Tuner`, `Ref %dHz` (A = 440 by default), `Mode`: **Mute** or **Bypass** (the
  output muted or passed while tuning).
- **The amp names on screen** (`f_02007620`): `AMP nn`; `PC AMP` for an uploaded `AM` file; `PC NAN` (sic) for an
  uploaded `NAM` file (**likely** from the strings and the type test).
- **Saving a preset** (`f_020102e2`): the screen shows `Saving...`, `Swapping...` (moving a preset to another place) or
  `Save failed`.
- `pd40_ui` is **the name of the UI task**, not a file (**corrected in the tenth pass**): `f_0200c236` compares the
  current task's name with it to decide whether it may take the UI lock directly.
- **The tails** (`f_0200127e`, read):
  - while the spill-over flag (`state + 2`) is set, the interrupt keeps exchanging blocks with the delay lane and/or
    the reverb lane (bits 0 and 1 of `state + 3`) at their old places in the chain (`state + 172..175`: position and
    lane of each);
  - it does so before running the rest of the chain (from the split point `0x1C0B4A2`), so the echoes and the reverb
    ring out across a preset change;
  - Off, Auto and Force (11.6) decide when that flag is set (the deciding code was not followed).

### 14.8 What is still not done

- The gate kernel's state machine as equations (14.2) and the compressor's per-sample loop line by line.
- Lofi's 664 instructions and dTape's tail line by line.
- USB descriptors, Bluetooth (A2DP, OTA), footswitch timing, the 020 / 021 difference: done in section 15.

## 15. Tenth pass: keys, USB, the AI gate, the last SysEx spaces, 020 against 021

### 15.1 The keys and the footswitches

**Read** (`f_0200b9c2`, a system timer every **20 ms**, registered by the init with `f_02010b96(…, 20)`).

**Inputs:**
- GPIO bits from `0x501C4` and `0x50004`;
- **three buttons on one ADC ladder** (channel 9). The raw value gives the combination pressed: below 430 = all
  three (mask 7); below 490 = 5; below 525 = 6; below 590 = 3; below 680 = 4; below 720 = 1; below 820 = 2; above =
  none. Each ladder bit must be seen on **3 successive scans (60 ms)** before it counts;
- the eight MIDI virtual keys (CC 20..27, 10.5), merged into the same mask.

**Each of 18 keys** runs a state machine:

| Event | When | Code |
| --- | --- | --- |
| press | 1 scan after the key goes down | `key << 16 \| 1` |
| **double press** | a press within **12 scans (240 ms)** of the last release | `key << 16 \| 2` |
| **long press** | held **37 scans (740 ms)** | `key << 16 \| 3` |
| repeat while held | every **7 scans (140 ms)** after that, counting up to 19 | `key << 16 \| 4..20` |
| release | after a short press | `key << 16 \| 0` |
| release | after a long press | `key << 16 \| (state + 20)` |

**A two-key combination**: keys 0 and 1 together send their own event (`0x3000000`) and reset both keys' states:
**likely** the tuner or bypass shortcut.

Events go to the UI through `f_0200b8f0`.

### 15.2 USB

**Read** (descriptors in flash):
- **Device**: USB 1.1, **VID `0x3654`, PID `0x7756`**, bcdDevice 1.00, 64-byte control endpoint, strings 1..3 (the
  product name is `BlackBox`).
- **Audio** (USB Audio Class 1), two streaming interfaces:
  - **playback** (computer → pedal), endpoint `0x03` OUT, isochronous **adaptive**, 274-byte packets;
  - **record** (pedal → computer), endpoint `0x83` IN, isochronous **asynchronous**, 270-byte packets (45 frames);
  - both are **2 channels, 24-bit in 3-byte subframes, 44,100 Hz only**.
- **MIDI**: a MIDI Streaming interface (class 1, subclass 3) with two bulk endpoints.
- The tasks `USB-Audio` and `USB-Midi` serve them. The record path is in 13.2: a ring and a resampler that follows the
  host clock, since the pedal runs at 44,118 Hz and the host at 44,100 Hz.

### 15.3 The "AI" gate, decoded

**Read** (`f_01c027ec` per sample, `f_0200a426` per analysis block).

**What the kernel accumulates** while it gates, on the high-passed detector signal:
- the sum and sum of squares of the signal and of its **first difference** (how bright or noisy it is);
- the **intervals between zero crossings**: their count, sum and sum of squares, i.e. the mean and variance of the
  period;
- peak levels, kept as maxima.

**What the analysis does with them** (`f_0200a426`):
- the period is called **steady** when the variance is under 4 % of the squared mean (`x 1.04`);
- a steady period is compared, within **1 %**, to **10.0 ms, 8.33 ms, 5.0 ms and 4.17 ms**: the **100, 120, 200 and
  240 Hz** of rectified **50 and 60 Hz mains hum** (`0.01`, `0.00833`, `0.005`, `0.004167` times the rate, tolerances
  `1e-4`, `8.3e-5`, `5e-5`, `4.2e-5`);
- the level is then sorted into classes at **-50, -60 and -100 dB**;
- the gate's open and close decisions use these, with hold counters (up to 100 blocks) and factors 0.65, 0.4, 0.97 and
  0.7.

So the "AI" gate **recognises steady mains hum and does not let it hold the gate open**, while a played note (not a
mains period, or not steady) opens it. That is the difference from a threshold gate. The same object is in every
dynamics model and every MW drive (14.2); AI Gate MS only exposes its bias.

### 15.4 The last SysEx spaces

**Read** (object 4, writes):

| Space | Write |
| --- | --- |
| `0xD` | Loopback (1 byte): applied, and the settings record saved (`f_0200c6e4`) |
| `0xE` | **load preset n** (the low byte, 0..79; `f_0200c8d0`) |
| `0xF` | **save the current preset** (`f_020124f4`, the `PATCHEND` writer) |
| `0x7`, sub-commands `0x300000`, `0x700001`, `0x800001` | preset-bank operations (`f_020122de`; **likely** the swap and copy behind `Swapping...`) |

The `0xE` blocks of object 5 (`0x100`, `0x200`, `0x400`) are small read-only records next to `PDB1` (13.1).

### 15.5 Bluetooth

**Read**: BLE advertising with the BLE-MIDI service (10.6); the classic link's A2DP sink (`JL_A2DP`, `bt_sbc_dec`,
SBC and mSBC), mixed into the output from a 6,144-sample 16-bit ring (7.x); the pairing records in the BTIF file.

The `*_ota.bin` names (`ble_app_ota.bin`, `spp_app_ota.bin`, `edr_ota2.bin`, `nor_ota.bin`, `usb_hid_ota.bin`,
`lcflash_ota.bin`, ...) are JieLi's generic list of update loaders, compiled in from the SDK. Which of them this
product uses is decided by the update tool, not by the application.

### 15.6 BlackBox_020 against BlackBox_021

**Read** (the two application images, their strings and tasks):

| | 020 | 021 |
| --- | --- | --- |
| Audio | JieLi's generic audio framework (`audio_server`, `audio_encoder`, `sbc_encoder`, opus, flac, cvsd, spdif, linein tasks) | its own interrupt-driven chain, and **the `wet_audio` task (the delay and reverb lanes) is new** |
| Drives | none of the MW drives | **MW-808, GOLD, OCD, RAT, MUFF, FACE** added |
| Models | `AMP %d` / `CAB %d` names | `AMP 00`, `PC AMP`, `PC NAN`; the `AM5` format and the NAM `BEND` trailer added |
| Storage | `mnt/sdfile/app/usr` | `EXT_RESERVED/pd40j` + `usr` (the layout of 10.4), the `P4JR` upload journal, the `GLB1` settings log |
| MIDI and keys | `midi_route` | the CC map and the footswitch functions (NS..CAB ON/OFF, VIRTUAL KEY 0..7, MOD/DLY TAP, TUNER MODE) |
| Settings | — | MIDI Ch / OMNI, BT, Loopback, Input Gain, Delay Tail, Reverb Tail (Off / Auto / Force), "Auto-save on exit" |
| Tasks | `screen_task`, `usr_app_task`, `usr_audio_task`, `eff_mute_task` | `pd40_ui`, `wet_audio`, `USB-Audio`, `USB-Midi`, `bt_sbc_dec`, `adc_scan` |
| Messages | — | `Effect change failed`, `Save failed`, `Saving...`, `Swapping...` |

Both have the same 41 effect names (gates, mods, delays, reverbs) and the tuner. So 021 is a **rewrite of the audio
engine and of the system around the same effects**: what this document describes is 021 throughout.

### 15.7 What is left

Done in the eleventh pass (section 16): the compressor's loop, Lofi, Loopback's flags and the screens' drawing code.

## 16. Eleventh pass: the last unread code

### 16.1 Compress, line by line

**Read** (set `f_0200b25e`, process `f_0200b38e`, defaults `f_0200b100`). `S`, `A`, `L` are Sustain, Attack and
Level over 100; Gate is the shared gate (14.2).

**Per sample, in this order:**
1. **Detector high-pass**: a one-pole high-pass on the input, `y = c (y + x - x_prev)`, with `c = 0.9 - 0.3 A`
   (0.9 at A = 0, 0.6 at A = 1): its corner moves from about **740 Hz to 3.6 kHz**. The compressor listens to the
   pick and the treble, not to the low strings.
2. **Envelope**: one one-pole on `|y|`, the same coefficient for attack and release:
   `k = 1 - 6,907.76 / (fs x t)` with `t = 80 + 56.25 (A - 1)` ms (**23.75..80 ms**, the time to fall by 60 dB;
   a time constant of 3.4..11.6 ms). The level is `20 log10(env)` (computed as `3.0103 x log2(env^2 + 1e-12)` with the
   log2 table).
3. **Gain computer**, in dB, the sum of two soft-knee stages:
   - a **compressor**: threshold **-48 dB**, knee **30 dB** wide, ratio **`1 + 5 S^4`** (1:1 at S = 0, 1.31:1 at 0.5,
     6:1 at 1). Above `T + W/2`: `(L - T)(1/R - 1)`; in the knee: `(L - T + W/2)^2 / (2W) x (1/R - 1)`;
   - a **downward expander**: threshold -90 dB, knee 30 dB, its ratio starts at 12 but the set routine always sends
     it to **1.0, so it is off**;
   - plus a **make-up gain of `3 + 9 S` dB** (3..12 dB).
4. **Gain**: `x x 10^(dB / 20)` (`exp2(dB x 0.1661)`), every sample.
5. **Output**: a one-pole DC blocker (`1 - 5e-5 x Sustain`: none at 0, about 35 Hz at 100); a first-order **1 kHz high
   shelf** (`f_0200b080`, `0.0227 x fs`) whose gain `20 p - 10` dB has `p = 0.5` from the defaults and **is never
   set**, so it stays flat; then **`tanh`** (the 512-entry table, `|x| x 64`), so the output soft-clips;
   then **x `0.014 x Level`** (0.7 at 50, 1.4 at 100). The dry path's gain is 0: the output is fully compressed.
6. Every parameter glides by 0.1 a block toward its target. The gate runs after, on the block.

### 16.2 Lofi, line by line

**Read** (process: block 10, `f_01c24540`, 664 instructions; set `f_0201cfce`; the stereo wrapper `+0x5BE`).

**Knobs** (each 0..100, over 100): Time `t`, Fb, Mix, Grit `g`, Speed `s`, Depth.
- delay time = **`100 + 682 t^2` ms** (100..782 ms, quadratic), in samples at 22,059 Hz; it glides by `1.44e-4` a
  sample;
- feedback = Fb, glided by 0.01 a block;
- drive `d = 3.5 + 2.5 g` (glided by 0.0183 a block);
- LFO rate = **`0.5 + 9.5 s`** (**likely** Hz; `f_01c05098`, the delays' LFO); Depth sets the length of the
  modulated pre-delay below, so how far the wow swings.

**Per 32-sample chunk:**
1. **Saturation**: `asinh(x sinh(d)) / (6 d)` (built from the sqrt and log2 tables; `sinh(d)` is recomputed only when
   `d` moves). Small signals get a gain of `sinh(d) / 6d` (0.79 at Grit 0, 5.6 at Grit 100); large ones are
   compressed logarithmically.
2. **The lo-fi EQ**: **9 biquad sections** in cascade (`f_01c0b246`, 324 bytes of state: the curve of 8.2).
3. **A modulated pre-delay**: the chunk is written into a float ring and read back with a 4-point Hermite at
   `(1 + k x LFO) x D` samples, `D` growing with Depth (wow).
4. **The main line** (16-bit, 22,059 Hz): read at the delay time with the integer-difference Hermite; the output is
   that read (**wet only**: the Mix is the wrapper's, 8.6); the line is written with
   `input_fade x modulated + feedback x read`, scaled by 32,768 and saturated to 16 bits.

**The stereo wrapper** compares the two channels' states (128 + 324 + 12 bytes); when they match, it saturates and
filters the input once and copies the result, saving half the work.

### 16.3 Loopback, settled

**Read** (`f_02029ede` in full, and `f_020298d8`, the USB playback mixer). The flags:
- `m` = the routing mode, byte 7 of `0x1C0B510` (**Loopback ON = 0, OFF = 1**; 2 and 3 are internal);
- `P` = byte 8 of that struct;
- `R` = the computer is recording (`state + 58`), `A` = the computer is playing (`state + 57`), `B` = Bluetooth audio
  is streaming (`state + 61 == 2`).

What reaches the computer (`R` set):

| Loopback | `P` | The computer records |
| --- | --- | --- |
| OFF | 1 | the pedal's processed block, **before** the output volume and before any mixing |
| OFF | 0 | the pedal's block **after** the output volume and balance, before the computer's playback and Bluetooth are mixed in |
| ON | 1 | a copy of the pedal's block, taken before the output volume, into which **the computer's playback** is mixed (**likely** Bluetooth audio too, `f_02029a6e`) |
| ON | 0 | **exactly what the pedal outputs**: the pedal, plus the computer's playback, plus Bluetooth audio |

So **Loopback OFF = the pedal alone; ON = the pedal plus the computer's own sound** (and Bluetooth audio when
`P = 0`). In every case the output is clipped to 24 bits (`f_01c070ee`) and its peak is kept for a meter
(`state + 636`).

The mixer also shows **the Bluetooth audio path**: a **6,144-sample** ring at `0x1F29EE0` (indices `state + 122` /
`+ 124`), mixed into the output with the same volume and polarity bytes (bytes 5 and 6 of the struct invert the
two sources' signs), underruns counted at `state + 2096`. The USB playback ring is the 1,024-frame one at
`0x1C17FC0` (`state + 114` / `+ 116`, underruns at `state + 2080`), scaled by the host's volume (`f_0202967e`).

### 16.4 A "hold" in every delay

**Read.** All five delays (blocks 6..10) carry a flag (byte `+20` of their state). When it is set:
- the input fades out (0.01 a block);
- the **feedback glides to 1.0**;
so the repeats **hold forever** (an infinite-repeat "hold" or "freeze"). The flag is set for both channels through
the last function of each delay's descriptor (`f_0202bbec`, `f_0202bc14`, `f_0202bc3c`, `f_0202bc64`, ... at
descriptor `+36`). **No code in the application calls that slot**: no footswitch function, CC or menu reaches it, so
in 021 the hold exists in the engine but is not used.

### 16.5 How the screens are drawn

**Read in outline** (the `pd40_ui` task; `f_0201adb0` its loop; `f_02018920` and `f_020178ee` the drawing).
- A **retained widget tree**: each object has a parent, a first child (`+4`) and a next sibling (`+8`), bounds
  clipped to **240 x 240** (`f_02004b72`), a dirty flag (`+162`) and a global redraw flag (`state + 388`, a frame
  counter at `state + 4548`). Only dirty objects are redrawn.
- **Styles per state**: a property lookup (`f_02004abe`) picks a value by property id and object state (a pressed or
  focused bit), the design of LVGL, written small.
- **Text**: a pool of 32 strings of 96 bytes (`0x1C7F120`; overflow counted at `state + 2285..2287`); strings carry
  LVGL-style colour tags (`#ffffff P#%s %02d#`).
- **Drawing**: a software rasteriser into **RGB565** with alpha (`f_02016e8e` fills a clipped span, blending each 5/6-bit
  channel by the opacity); knobs are drawn as **anti-aliased arcs in degrees** (`f_020178ee`, 360); the screens are
  built by one function each (main `f_0200dac8`, edit `f_0200d47c`, settings `f_0200de68` / `f_0200df42`, tuner
  `f_0200ed68` / `f_0200f534`, save and swap `f_0200e59a` / `f_0200fbda`, low battery `f_0201adb0`).
- The frame goes to the panel by SPI DMA, which OpenPocket already drives (handbook, display).

Nothing here changes OpenPocket: its UI is its own. The pixel code is read in 17.1.

### 16.6 What is left

The UI's pixel code and JieLi's libraries: done in section 17.

## 17. Twelfth pass: the pixel code and JieLi's libraries

### 17.1 How a pixel is drawn

**Read** (`f_02018920` and the routines it calls). The UI is a small **LVGL-style** library: an object tree, styles
read by property id, a draw pass per object, a flush to the panel.

**The draw pass** (`f_02018920`, recursive over the children):
1. Skip the object if it is hidden (flag bit 0 of `+155`) or outside the area being redrawn (its bounds, widened by
   the shadow and outline sizes, against the clip rectangle).
2. Draw its **background**: `f_0201716a`, a filled rectangle with a **corner radius** (prop 14) and an optional
   **vertical gradient** between two colours (props 0 and 1, opacity prop 8).
3. Draw the **type's content**, a switch on the type byte (`+122`):

| Type | Object | Drawn by |
| --- | --- | --- |
| 2 | **label** (text) | `f_0201745e` per glyph; the font from the object or its parents (`f_02015c78`, default `0x2054F0C`), style props 18, 2, 9 and 22 |
| 3 | **image** | inline: its pixels drawn as spans through a 16-bit palette, with a recolour (props 12, 5) |
| 4 | **line** | inline: a polyline (points at `+20`, count at `+28`), width prop 21, colour prop 7 |
| 5 | **bar** | the value mapped to pixels (`f_0200fc96`, a linear map), then a rounded rectangle |
| 6 | **arc** | `f_020178ee` twice: the background ring and the value's ring, angles in degrees (props 20, 6, 13, 24) |
| 7 | **gauge / knob** | 674 inline instructions: value mapped to an angle, then `f_020178ee` |
| others | plain object | border and outline (`f_0201858a`), a run-length image (`f_020177d8`, **likely** a background image), then the children (recursion) |

**The primitives:**
- **Span fill** (`f_02016e8e`): one row from `x0` to `x1` in the **RGB565** buffer. At full opacity it stores the colour;
  otherwise it blends each channel: `(src x a + dst x (255 - a) + 127) / 255`, with a cache of the last blend.
- **Rounded corners** (`f_02016f8e`): the coverage of a corner pixel is measured on a **4 x 4 sub-pixel grid**
  (offsets 1, 3, 5, 7 eighths) against the radius, giving an opacity of 0..255: **anti-aliased corners**.
- **Rectangles** (`f_0201716a`): the radius is found with an integer square root; each row is a span, the gradient
  colour interpolated per row; the corner rows use the coverage above.
- **Arcs** (`f_020178ee`): between two angles in degrees, on spans, edge pixels blended by opacity (**likely**
  anti-aliased like the corners).
- **Text** (`f_0201745e`): each glyph is a bitmap of `bpp` bits a pixel (1, 2, 4 or 8), each value scaled to an
  opacity and drawn as spans. A font can instead store glyphs **run-length coded**: each byte is a run (high nibble)
  of a 4-bit opacity (low nibble, x opacity / 15).
- **Images** (`f_020177d8`): 8-bit **palette indices**, each row **run-length coded** (a byte with bit 7 set repeats
  the next index, otherwise it counts literal indices), a row offset table, and an **RGB565 palette**.

**The fonts** (font records at `0x2054F0C..0x2054F84`, 24 bytes each: bitmaps, glyph table, kerning table, counts,
line height, baseline, `bpp`, compression; glyph records of 10 bytes: bitmap offset, code point, advance in 1/16 px,
width, height, x and y offsets; kerning records of 3 bytes: left, right, adjustment in 1/16 px):

| Record | Glyphs | Height | Format | Use |
| --- | --- | --- | --- | --- |
| `0x2054F0C` | ASCII 32..126 (95), 380 kerning pairs | 20 px | 4 bpp | all text |
| `0x2054F24` | `0123456789P` | 54 px | 4 bpp, run-length | the big preset number (`P05`) |
| `0x2054F3C` | `ABCDEFG` | 38 px | 4 bpp, run-length | the tuner's note |
| `0x2054F84` | ` #0123456789?BHMRTadefnoprstuyz` | 22 px | 4 bpp | the tuner page (`Ref 440Hz`, `Mute`, `Bypass`, `#`) |
| `0x2054F54`, `0x2054F6C` | one glyph each (`A`) | 16 / 10 px | 1 bpp | **likely** placeholders or icons |

**The flush** (`f_0201bccc`, registered as the display driver's flush function `f_0201be1c`):
- the window is set with the panel's standard commands, **CASET `0x2A`, RASET `0x2B`, RAMWR `0x2C`** (`f_0201b9ce`;
  data/command on the GPIO at `0x50040` bit 5, chip select bit 1);
- the pixels go by **SPI1 DMA** (`0x11D0C` address, `0x11D10` count), at most 3,840 bytes (1,920 pixels) a transfer;
- every SPI wait has a **2 ms timeout** (`f_020105da`); on a timeout the SPI is reset (`CON = 0x4021`) and the
  command retried once; counters at `state + 3736..3772` keep the failures.

### 17.2 The map of the image

**Read** (strings, peripheral addresses, call graph). What each part of the application image is:

| Addresses | Instructions | What it is |
| --- | --- | --- |
| `0x1C00000..0x1C11000` (RAM) | 24,841 | interrupt entry, flash and cache helpers, the multi-core lock (`f_01c01196` / `f_01c011ae`), the audio interrupt and the DSP kernels (sections 2..8) |
| `0x2000000..0x2022000` | 44,619 | **BlackBox**: presets, effects' set-up, UI, keys, MIDI, settings (sections 3..16) |
| `0x2022000..0x2025B00` | 4,915 | JieLi's app framework: system events (`sys_event`), the file-system switch (`jlfat`), UTF-8, the app's start-up (`f_02023a20`), the driver version (`INCLUDE_DRIVER-$a5f5fbc`) |
| `0x2025B00..0x202AE00` | 7,116 | **USB device** (registers at `0x16000..0x16800`): the USB-MIDI receiver, Audio Class, the record and playback rings and mixer (16.3) |
| `0x202AE00..0x2030A00` | 7,371 | BlackBox's DSP glue: amp, cabinet, reverbs, Shimmer (sections 4..14) |
| `0x2030A00..0x2035D00` | 7,572 | system services: timers (`sys_timer`, `usr_systimer`), clocks and power domains (`ble`, `edr`, `wifi`), the flash file system (`sdfile`, `EXT_RESERVED`, `app_area_head`, `res`), the Bluetooth pairing file (`btif`) |
| `0x2035D00..0x203F000` | 13,398 | a **FAT** file system (12/16/32, long names in UTF-8, 8.3 names), partition tables (MBR, GPT `EFI PART`) |
| `0x203F000..0x2043000` | 5,434 | clock (`osc`), environment (`getenv`), the BLE GATT application (`f_020420e0`), the **SBC/mSBC decoder** (`f_02042caa`, on a hardware block at `0x13D00`, **likely**) |
| `0x2043000..0x2046100` | 4,086 | C library: `memcpy` `f_0204534a`, `memset` `f_0204540c`, `memcmp` `f_02045004`, `strcmp` `f_0204503c`, `strlen` `f_0204514a`, software double-precision floating point |
| `0x2046100..0x2058000` | — | read-only data: strings, effect and model tables, fonts, impulse responses, descriptors |
| `0x2058000..0x2059900` | 1,524 | the **update library** (`INCLUDE_UPDATE`, `app_dir_head`, the `*_ota.bin` names, `flash.bin`, `LOADER.BIN`) |
| `0x2059900..0x205C000` | 3,272 | the **heap** (`MEM_HEAP`): `malloc` `f_0205a17e`, `free` `f_02059b36`, `calloc` `f_0205b24c`; chunks with size and flag bits, bins, a spin lock shared by both cores |
| `0x205C000..0x2060000` | 5,558 | the **RTOS** (FreeRTOS, 18.2) and, from `0x205EF00`, the **pairing crypto** (SHA-256, HMAC, ECDH P-192, 18.5) |
| `0x2060000..0x2075000` | 29,160 | the **Bluetooth controller**: radio (`0x28000`), baseband (`0x2FC00`, `0x2FD00`), the `btctrler` task, the BLE link layer (`link_layer`, `ble_slave_mult`), the virtual HCI between host and controller (`H4_Controller`), low power (`btstack_lowpwer_deal`), the radio test mode (`autotest`) |
| `0x2075000..0x2080000` | 16,057 | the **Bluetooth host** (`btstack`): L2CAP, SDP, AVDTP / A2DP, ATT / GATT |
| `0x2080000..0x2080738` | — | Bluetooth host code (the HCI event side) |
| `0x2080738..0x2081880` | — | tables: the file-system operations, the init calls (`0x2080990`), the module table (`0x2080A10`) |
| `0x2081880..0x2091F64` | — | the **RAM code's load image** (67,556 bytes copied to `0x1C00000` at boot, 18.1), with the task table (`0x208CF00`) |
| `0x2091F64..0x2092084` | — | a 256-byte configuration block (**likely** the Bluetooth defaults, 18.5) and the 32 bytes of initial data of the second RAM bank |
| `0x2092084..0x209A0F4` | — | the two overlays and the 20 effect blocks (sections 8.2, 8.4) |

### 17.3 What the libraries do for the BlackBox

**USB** (read):
- the **USB-MIDI receiver** (`f_02025b12`) recognises two 8-byte packets `04 F0 22 24` + `07 35 xx F7`, the SysEx
  `F0 22 24 35 xx F7`:
  - `xx = 7D` writes `usb_update_mode` at `0x1C7FD80` and resets the chip (`f_020253d6`): **the boot loader's USB
    update mode** (the same key OpenPocket uses, `firmware/bsp/chip/usb.c`);
  - `xx = 7F` sets the update request flag (OpenPocket's `ota_req`, the M-UPGRADE path);
- the same reset is reached from a second handler (`f_0202710a`) on a command packet `FC 00 FE 00 00 00`
  (**likely** JieLi's PC update tool);
- Audio Class and the rings: 15.2, 13.2, 16.3.

**Bluetooth** (read):
- **one classic profile: A2DP sink**. The only SDP record (`0x2075950`) is: Audio Sink `0x110B`, L2CAP PSM `0x19`
  (AVDTP) version 1.3, Advanced Audio `0x110D` version 1.3, features `0x0001`, name `JL_A2DP`. **No AVRCP, no
  HFP, no SPP record**;
- the audio is decoded as SBC (`bt_sbc_dec` task) and mixed into the output through the 6,144-sample ring (16.3);
- **BLE**: a GATT server with the BLE-MIDI service, advertised as `sinco...` (10.6);
- the pairing records live in the `btif` file.

**Files** (read): every file the BlackBox opens is on the **`sdfile`** flash file system (`mnt/sdfile/...`: `app`,
`res`, `EXT_RESERVED/pd40j`, `EXT_RESERVED/usr`, `app/btif`, `app/cfg_tool.bin`), resolved through the flash
directory (`app_area_head`). The FAT driver is linked in through the file-system table but **no code path the
BlackBox uses mounts a FAT volume** (its mount, `f_020365f8`, is reached only through that table).

**Update** (read): the update library knows JieLi's loader names (`edr_ota2.bin`, `ble_app_ota.bin`, ...) and loads
`LOADER.BIN` from flash (`f_0202567e`). The update itself runs in the boot loader after the reset above, not in the
application.

**RTOS and heap** (read): FreeRTOS-style tasks on both cores (`wet_audio`, `pd40_ui`, `tuner_task`, `USB-Audio`,
`USB-Midi`, `bt_sbc_dec`, `midi_route`, `sys_timer`, `btctrler`, `btstack`, ...), a heap with a lock shared by the
two cores. The audio itself runs in interrupts (section 2), not in tasks.

### 17.4 What is left

Read in the thirteenth pass (section 18), which also **corrects** two rows of 17.2: `0x2080000..0x2088000` is
not a driver library but tables and the RAM code's load image, and `f_0205ca82` gives a semaphore, it does not
create tasks (`f_0205d93a` does).

## 18. Thirteenth pass: boot, RTOS, heap, FAT and Bluetooth

### 18.1 Boot

**Read** (`f_02000120` → `f_0200018a` → `f_02021564`).
1. **Entry** `0x2000120` jumps to `f_0200018a`: stack `0x1C1EB10`, supervisor stack `0x1C1FB10`.
2. `f_0200042e` keeps the **boot loader's hand-over** (flash geometry, boot flags, 16 bytes of keys) at `0x1C7FD4C..`
   (the same area as the `usb_update_mode` word at `0x1C7FD80`, 17.3).
3. **`.bss`** cleared: 72,532 bytes from `0x1C107E4`.
4. **RAM code** copied: **67,556 bytes from flash `0x2081880` to `0x1C00000`** (code and initialised data).
5. An external-RAM section (`0x4000120`) would be copied and cleared: its size is 0, so the BlackBox uses **no SDRAM**.
6. `f_01c00bb6`: cache set-up (`0x1EEE008`), the cache tag RAM cleared, and the **second RAM bank** at `0x1F28000`
   prepared (32 bytes of data from `0x2092064`, then 20,160 bytes zeroed; the UI and the Bluetooth audio ring live
   there, 16.3, 17.1).
7. **`main`** (`f_02021564`):
   - the **reset cause** read from the power unit and kept at `state + 508` (power-on, watchdog, low voltage, ...);
   - the clocks named `hsb`, `sys`, `sdram`, `timer`, `app_core` reported;
   - the **exception handler** installed (`0x2000238`: all registers pushed, then `f_02020ba2` prints them and
     halts), the system timer interrupt (28, `f_02020df6`);
   - **memory protection per core**: the flash code, the RAM code (`0x1C00000..0x1C0B38C`) and each core's stack
     (core 0 `0x1C1EB10..0x1C1FB10`, core 1 `0x1C1FE10..0x1C20E10`) are fenced, so a stack overflow or a write to code
     traps;
   - the RTOS initialised (`f_0205d82e`, `f_0205d85a`), the task `app_core` created (`f_0202150e`), the scheduler
     started (`f_0205da8e`).
8. **Core 1** starts at `0x2000224` with its own stacks and jumps to the RAM code at `0x1C035C0`.

### 18.2 The RTOS

**Read.** It is **FreeRTOS (version 10 layout) made SMP by JieLi**:
- the queue structure is FreeRTOS's to the byte: `pcHead +0`, `pcWriteTo +4`, waiting-to-send list `+16`,
  waiting-to-receive list `+36`, `uxMessagesWaiting +56`, `uxLength +60`, `uxItemSize +64`, the locks `+68/+69`;
  queue types 1 mutex, 2 counting semaphore (`f_0205da94`), 4 recursive mutex (`f_0205d0cc`);
- **one current task per core** (`cnum` indexes `0x1C10054`), two idle tasks **`IDLE0`, `IDLE1`** (256-word stacks);
- the context switch is a **software interrupt per core** (number `0x7F - core`, handler `0x204519A`);
- **the tick is 100 Hz (10 ms)**: core 0's timer (`0x1EEF0EC`) reloads `sys clock / 100`, interrupt 3, handler
  `f_0205df6e` (counts jiffies at `0x1C0B4C8`, then `xTaskIncrementTick` `f_0205c79c`, yields if needed);
- API used by the BlackBox: take / give with timeout `f_0205cdba` / `f_0205ceb8`, semaphore give `f_0205ca82`
  (with its from-interrupt path), task creation `f_0205d93a` (wrappers `f_0202150e` by name from the task table,
  `f_0205d37e` by name and arguments), tasks found by name `f_0205e896`.

**The task table** (`0x208CF00` in the RAM image; name, priority, stack in words, message queue in words):

| Task | Priority | Stack | Queue |
| --- | --- | --- | --- |
| `app_core` | 15 | 1,024 | 128 |
| `sys_event` | 29 | 512 | 0 |
| `systimer` | 14 | 256 | 0 |
| `sys_timer` | 9 | 512 | 64 |
| `btencry` | 16 | 512 | 64 |
| `btctrler` | 19 | 512 | 384 |
| `btstack` | 18 | 512 | 384 |
| `update` | 21 | 384 | 32 |
| `tuner_task` | 3 | 512 | 0 |
| `bt_sbc_dec` | 27 | 384 | 0 |

The BlackBox's own tasks (`wet_audio`, `pd40_ui`, `USB-Audio`, `USB-Midi`, `midi_route`) are created from code with
their own values. The audio path itself runs in interrupts (section 2), above every task.

### 18.3 Memory allocation

**Read.** Three allocators:
- **`malloc` / `free` / `calloc`** (`f_0205a17e`, `f_02059b36`, `f_0205b24c`) are **dlmalloc 2.8**: its parameter
  block to the field (granularity 4 KB, trim threshold 2 MB, no `mmap`, the magic `0x55AA55A8` mixed with the tick
  count), chunks with size and in-use bits, small and tree bins, chunks rounded to **32 bytes** (cache lines, so
  buffers are safe for DMA). A **spin lock** (`testset` on `state + 8524`) makes it safe on both cores. A corrupted
  heap makes it **loop forever** (dlmalloc's `CORRUPTION_ERROR_ACTION`), which shows up as a hang, not a crash
  screen.
- **`MEM_HEAP`** (`f_0205be6c`): a small pool for the Bluetooth controller with guard words `0xA5ABCDEE` /
  `0x5A5AEAAE` around blocks.
- A **block allocator** for buffers (`f_0205b3e6` / `f_0205b87a`, through `f_0205b9c4`), used by the UI and
  `wet_audio` (**likely** JieLi's `lbuf`).

### 18.4 The FAT driver

**Read** (mount `f_020365f8`, the rest through the file-system table `0x20808A0`).
- **FAT12, FAT16 and FAT32**, chosen by cluster count (the 65,525 limit); sectors of 512 bytes, clusters of 512 B to
  64 KB; no exFAT;
- long file names in **UTF-8** (`f_020228ca` decodes it), 8.3 names, wildcards (`*`) in searches, `/` and `\`
  separators;
- the usual operations through the table: open, read, write, seek, length, position, rename, delete, directory
  scan; a FAT32 boot-sector template (`MSDOS5.0`, `NO NAME`, `FAT32`) for formatting; MBR and GPT (`EFI PART`)
  partition tables read;
- **unused by the BlackBox**: no code path mounts a FAT volume (17.3). It is there because JieLi's file-system layer
  links it in; the pedal has no card and does not expose a disk over USB.

### 18.5 The Bluetooth stack

**Read** (its tasks, its configuration, its interfaces and its fixed tables). Both halves run on the chip, joined by
a **virtual HCI** (`H4_Controller`): the host sends standard HCI commands (`f_02061aae(type, length, opcode, ...)`),
the controller answers with events.

**Controller** (`0x2060000..0x2075000`, tasks `btctrler` and `btencry`):
- the **radio** (registers `0x28000..`) and the **baseband** (`0x2FC00`, `0x2FD00`), driven by the controller's
  scheduler;
- **classic**: page and inquiry scan, connection accept, link keys, and **Secure Simple Pairing (Bluetooth 2.1)**:
  ECDH on **P-192** (the curve's constants at `0x205FEE4`) and SHA-256 / HMAC (`0x205EF00..`) computed in the
  `btencry` task; the user confirmation is answered **automatically** (`f_0206e146`), so pairing is **Just Works**
  with no PIN. There is **no P-256**, so no Secure Connections;
- **BLE**: a link layer with several slave links (`ble_slave_mult`), advertising set through the LE HCI commands
  (`0x2006` parameters, `0x200A` enable);
- the radio test mode (`autotest`) and low-power handling (`btstack_lowpwer_deal`);
- its own memory pool (`MEM_HEAP`, 18.3).

**Host** (`0x2075000..0x2080738`, task `btstack`):
- **L2CAP**, **SDP** with one record (A2DP sink, 17.3), **AVDTP / A2DP sink** with SBC decoded by the `bt_sbc_dec`
  task (priority 27) into the 6,144-sample ring (16.3);
- **ATT / GATT** with the BLE-MIDI service (`f_020420e0`), advertised as `sinco...`;
- the BlackBox configures it once at start-up (`f_020221ac`): a set of feature bits, two timeouts of **8,000 slots
  (5 s)**, the **Bluetooth address** (from the `btif` record, with a 16-bit check `f_02077256`), the event callbacks
  (`f_0202865a`, `f_02028664`), then starts the two tasks (`f_020772e6`);
- the default block at `0x2091F64` (**likely** the stack's defaults) holds a class of device **`0x240404`**:
  Audio/Video, wearable headset, with the Audio and Rendering service bits.

**What it means for the BlackBox**: Bluetooth is only a **receiver** of music (A2DP sink) and a **BLE-MIDI**
peripheral; no remote control (AVRCP), no calls (HFP), no serial port (SPP). Its tasks (priorities 16..27) all run
below the audio interrupts, but its radio work shares the CPU, which fits the extra load seen when Bluetooth is on
(section 2).

### 18.6 What is left

Every part of the image is now identified and the parts that matter are read. What was not taken instruction by
instruction is the inside of JieLi's generic state machines (the Bluetooth controller's scheduler, LMP and link
layer, L2CAP/AVDTP/ATT details, FAT's cluster allocation, FreeRTOS's lists, dlmalloc's bins): they are standard
designs (FreeRTOS, dlmalloc, the Bluetooth specification) or chip-specific radio code, the same in every JieLi
product, and OpenPocket neither uses nor replaces them line by line.

## 19. Fourteenth pass: Bluetooth, read to the end

Why: Bluetooth in the v37 release sent nearly every preset to `BYP` at 150 % and more, and was taken out again. This pass
reads everything that decides how the stock firmware lives with its radio: where the radio's interrupts and tasks run,
what the library does per packet, and the BlackBox's own Bluetooth layer. The library's inside was read in JieLi's LLVM
bitcode from the AC79 SDK (`third_party/jieli-ble/`), which keeps its function names, its structures' field names and its
section names, and was matched against the stock image.

### 19.1 Interrupts, cores and the critical sections

**Read.**
- **`request_irq` (`f_02010694`) does not obey its core argument.** It looks the interrupt up in a table at `0x204FE18`
  (`{irq, priority, core}`, three bytes each). The table holds `{125, 7, CPU0}`, `{124, 7, CPU1}`, then the marker
  `{254, 254, 254}`. The SDK's own copy of the table explains the marker: "if this row is present, only the interrupts
  before it go to their core; every other interrupt is forced onto CPU0". So **every interrupt but SOFT4 is served by
  CPU0**: the audio (11, priority 4), the BLE baseband (45 and 29, priority 2), the classic baseband (40, 41), the hardware
  jobs (62) and the SBC decoder (46). This corrects sections 2 and 7.6, which took the core arguments at their word.
  Interrupt 44, called "radio" there, is the LRC timer (`0x2030DC2`).
- The priorities are 0 to 7, **7 the highest**: the audio (4) is above the radio (2), so the audio interrupt can preempt
  the radio's interrupt's caller, but not the radio's interrupt itself (next point).
- **`local_irq_disable`** (`f_01C01196`, in RAM) is `cli` plus a per-core counter at `0x1C0BDF4`. `local_irq_enable`
  (`f_01C011AE`) is `sti` when the counter returns to 0. `f_01C011CA` adds a spin lock (`testset` on `0x1C0BBB8`): the
  critical section shared by the two cores. The SDK's other variant, which keeps priority-7 interrupts alive through a
  critical section with the core's `IPMASK` register (`CONFIG_IPMASK_ENABLE`), is **not** built in: a critical section
  masks everything, the audio too.
- **The BLE event interrupt** (`0x2063BC2`) is the library's `ble_event_irq_handler`. It opens with `local_irq_disable`
  and a spin lock on the baseband (`testset` on `bb_hdl + 33`) and keeps them for its whole body. **The radio's
  interrupt is never interrupted, not even by the audio**; in exchange it is short.
- `cpu_in_irq` (the SDK's `apps/common/system/init.c`) is `icfg & 0xFF`: not 0 while an interrupt is served.

### 19.2 The library is JieLi's, built with LTO

**Read.**
- The 64 bitcode objects of `third_party/jieli-ble/` were compiled with JieLi's clang and compared with all 3,005
  functions of the stock image (instruction sequences with their operands blanked):
  - 121 are identical;
  - 223 more match closely (60 % or more).

  Examples: `__set_le_prio` at `0x2062134`, `ble_hw_tx_update_header` at `0x206223E`, `ble_pll_comp_reset` at `0x2063ACE`.
  The two baseband interrupt handlers are those `le_hw_init` registers (`request_irq(45, 2, ble_event_irq_handler)`,
  `request_irq(29, 2, ble_rx_irq_handler)`).
- The stock copy was **linked with LTO** (the SDK's `lto-wrapper`): the configuration constants are folded in (the loop
  over the baseband's hardware entities is gone, and so is every log string). So most functions differ in detail and match
  in structure. The whole of Bluetooth, classic and LE, takes 130 KB in the stock image (`0x2060000..0x2080738`). Without
  LTO, the LE part alone takes OpenPocket 154 KB of code and 60 KB of constants.
- The SDK's linker script puts all of the controller's and host's code (`.ble_rf_code`, `.ble_ll_code`,
  `.bt_stack_code`, ...) **in flash**. Only the DSP library runs from RAM (8.2).

### 19.3 How a packet travels

**Read** (the library's IR):
1. **IRQ 29** (reception) or **IRQ 45** (end of a radio event), on CPU0, everything masked.
   - For each baseband entity with a pending flag (`0x28028 & 0x28030`), the received buffers are queued (`hw_dma_push`),
     the next packet to send is set up (`ble_hw_tx`), and the next event is planned (`mult_slave_or_master_event_enter`:
     anchor, window widening, slave latency).
   - The planner adds and removes timeouts from inside the interrupt (`sys_timeout_add`, `usr_timeout_add`).
   - It stops the baseband with `le_hw_suspend`, which asks `cpu_in_irq`: inside an interrupt it uses the version that
     does not wait on the busy flag (`ble_hw_disable_isr`).
2. **The interrupt wakes the controller's task**: `thread_resume` → `bt_task_resume` → `os_taskq_post_type("btctrler", 255)`,
   an empty message.
3. **Task `btctrler`** (stock: priority 19, 2 KB of stack, a 384-word queue) waits in `os_taskq_pend`. It dispatches its
   messages and then runs its "threads" (`hci_controller_run_loop` → `thread_run`): the link layer's procedures,
   control PDUs and encryption.
   - Message types: `0x400005` link-layer event, `0x400006` HCI command from the host, `0x40000C` a semaphore to post.
4. **To the host**: `hci_packet_handler` copies each HCI event into a message for task `btstack` (type `0x400001`, the
   event's words) and passes ACL data by pointer (`0x400002`). If the queue is full, it sleeps 20 ms and tries again
   (not from an interrupt or from `btstack` itself).
5. **Task `btstack`** (priority 18, 2 KB, 384 words): `hci_event_handler` / `hci_acl_handler` → L2CAP → ATT server → the
   application's write callback.
6. **From the application**: `ble_user_cmd_prepare(cmd, argc, ...)` copies the command into a ring that the host's own
   thread reads (`__ble_thread_loop_handler`), so it may be called from any task. The commands used here:

   | Command | Meaning |
   | --- | --- |
   | 1 | advertising on/off |
   | 2 | advertising parameters |
   | 3, 4 | advertising and scan-response data |
   | 7 | ATT send buffer |
   | 8 | MTU |
   | 9 | free room |
   | 10 | send (notify) |
   | 11 | connection parameter request: `gap_request_connection_parameter_update(handle, min, max, latency, timeout)`, read through a pointer **later**, so the parameters must not live on the caller's stack |

The radio itself (timing, retransmission, empty packets, acknowledgements) is done by the baseband in hardware. The
interrupt only moves buffers and plans the next event, so its time is tens of microseconds, not milliseconds.

### 19.4 The BlackBox's own Bluetooth layer

**Read.**
- **GATT table** at `0x204FE2A`, given to the ATT server at `0x2080662` with the read callback `f_02028684`, the write
  callback `f_02028716` and the event callback `f_020288FC`:

  | Handle | What |
  | --- | --- |
  | `0x0001`.. | GAP `0x1800`, device name `0x2A00` (read and write) |
  | `0x0060` | service `0xAE40`, JieLi's data service |
  | `0x0062` | `0xAE41`, write without response: what is written goes into **the same SysEx receiver as USB** (`f_02025B12`, port 1) |
  | `0x0064`, `0x0065` | `0xAE42`, notify, and its CCC: the receiver's replies (port 2), sent by `f_020268A4` |
  | `0x0070` | service BLE-MIDI `03B80E5A-EDE8-4B33-A751-6CE34EC4C700` |
  | `0x0072`, `0x0073` | the MIDI characteristic (read, write without response, notify), and its CCC |

  The `AE40` service is the M-VAVE app's channel: the app talks the SysEx protocol of 11.8 and 13.1 over Bluetooth.
- **A BLE-MIDI write** (`0x0072`): the header and the timestamps (bytes with bit 7 set that come before a status byte)
  are dropped and running status is kept (`b[0x1C0B4A0 + 168]`). The MIDI bytes go into a ring (`[0x1C0B4A0 + 160]`, mask
  `+164`, write index `+156`) that the MIDI routing drains, the same way as USB MIDI.
- **MIDI out**: `f_020268A4` sends the pedal's own MIDI as BLE-MIDI notifications on `0x0072`, with a header and a timestamp
  from the tick count, and the SysEx replies on `0xAE42`.
- **The link's parameters.** When the central switches notifications on (a CCC write), the BlackBox asks for new
  parameters from the table at `0x204FF08`:
  1. first `{6, 9, 0, 100}`: 7.5 to 11.25 ms, no slave latency, 1 s supervision timeout;
  2. if the central refuses (L2CAP event `0x77` with a non-zero result), `{12, 12, 0, 100}`: 15 ms.
- **The event callback**:
  - a connection (`0x3E/01` or `0x3E/0A`) keeps the handle, sets up a 900-byte send buffer (`0x1C17724`) and a payload of 20
    bytes;
  - the MTU exchange (`0xB5`) raises the payload to MTU - 3, at most 197;
  - a disconnection (`0x05`) clears all this and advertises again (`f_020155F6`) if Bluetooth is on.
- **Advertising** (`f_020155F6`): `ADV_IND` every **800 × 0.625 ms = 500 ms** on channels 37 to 39.
  - Data: the flags (`06`), the BLE-MIDI UUID, and manufacturer data `"sinco"` (AD type `0xFF`).
  - Scan response: the name, up to 29 bytes.
- **Radio upkeep**: a 3 s timer (`f_020219F4(..., f_0203FC42, 3000)`) reads the chip's temperature sensor (ADC channel
  `0x3000F`, through `f_0200B7D4`). When the reading has moved by more than about 2,200 since the last calibration, it
  runs the radio calibration again (`f_0203F702` … `f_0203FC16`, the Wi-Fi/RF common library).

### 19.5 What it means for OpenPocket

**Read** (the stock firmware) and **likely** (the v37 diagnosis; v41 is its test).
- The stock firmware runs some 130 KB of Bluetooth code **from flash**, through the instruction cache the two cores share,
  and its sound does not suffer, because **its whole per-sample library runs from RAM** (8.2). In v37, OpenPocket's audio
  code still ran from flash. The radio's code and the DSP code pushed each other out of the cache, and the second core,
  which does no Bluetooth at all, slowed down by half. That fits the reading: about 70 % before the radio started, 150 %
  after. v37 made it worse by putting the controller's task stacks in the data cache used as RAM, which the same cache
  controller serves.
- v40 moved the audio path to RAM. v41 brings Bluetooth back on top of it:
  - the radio's data, heap and task stacks in plain RAM;
  - the controller run only when something waits for it;
  - the stock firmware's advertising interval and link-parameter requests.

  [BLUETOOTH.md](BLUETOOTH.md) lists the choices.
- **The supplies.** The stock firmware always runs at VDC14 1.60 V and SYSVDD 1.38 V (AUDIO-SDK.md, v30), and its radio
  with them. OpenPocket runs at the boot loader's 1.40 V and 1.26 V (v33). On the pedal, v41's first builds with the radio
  on lost the second core to misreads (`BYP` everywhere) and crashed once on a bad pointer: the radio now starts only
  after raising the supplies to the stock levels (**likely**; the next test decides).

### 19.6 What is left

- The `AE40` service's own framing, beyond "the bytes go to the SysEx receiver".
- The radio re-calibration routine's inside (`f_0203F702` …): it belongs to the Wi-Fi/RF library OpenPocket does not
  link.
- The classic A2DP path (`JL_A2DP`, `bt_sbc_dec`), which OpenPocket does not use.
