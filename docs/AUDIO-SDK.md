# Audio through the official JieLi SDK (AC79 / WL82) — development notes

Source: JieLi's official SDK `fw-AC79_AIoT_SDK` V1.2.0 (Gitee: gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK).
Apache 2.0 licence → reusable in OpenPocket (compatible with GPL-3.0 in that direction).

## What the SDK gives us
- `include_lib/driver/cpu/wl82/asm/iis.h` — I2S (ALNK) API: `iis_open`, `iis_set_sample_rate`,
  `iis_set_enc_data_handler` (input), `iis_set_dec_data_handler` (output), `iis_irq_handler`.
- `include_lib/driver/cpu/wl82/asm/WL82.h` — the official register map (ALNK0 0x12E00, ALNK1 0x15100,
  IOMAP 0x51000+, CLOCK 0x10000, ANA 0x11900).
- `cpu/wl82/liba/cpu.a` → `iis.c.o` = **LLVM bitcode** produced by clang 4.0.1 (= our toolchain).
  Compiles as is: `clang -target pi32v2 -Os -c -x ir iis.bc -o iis.o` (~5 KB).
  Dependencies we provide: malloc/free, memset, gpio_direction_input/output, gpio_set_die, gpio_set_pull_up/down,
  log_print/printf (no-op), log_tag_const_{d,e,i}_AUDIO_IIS (u8 = 0), os_current_task_rom, cpu_assert_debug,
  config_asser, P33_SYSTEM_RESET.
  The driver does not register its IRQ: we call `iis_irq_handler(0)` from the ALNK0 ISR.
- The driver sets the audio clock family (44.1k / 48k) itself through CLK_CON2 (0x10014) and ANA.

## I2S pins (AC791x datasheets in the SDK)
| Group | MCK | SCLK | LRCK | DAT0 | DAT1 | DAT2 | DAT3 |
|---|---|---|---|---|---|---|---|
| IIS_PORTA (B1) | PA4 | PA5 | PA6 | PA7 | PA8 | PA9 | PA10 |
| IIS_PORTC (A)  | PC0 | PC1 | PC2 | PC3 | PC4 | PC5 | PC6 |

Pocket X: PC0/PC1 = encoder, PC7 = screen, PA0–PA3 = buttons, PA10 = MASTER knob (analog)
→ **IIS_PORTA, data on PA7/PA8/PA9** (DAT3/PA10 not usable).

## Finding the audio path on the pedal
- **v10 (2026-11-01)**: `I2S OK`. **Guitar input = I2S ALNK0, DAT1 = PA8** (IIS_PORTA, master, MCLK on PA4,
  44.1 kHz). DAT0 (PA7) and DAT2 (PA9) idle. No sound out on DAT0/DAT2.
- Output hypothesis: **internal DAC (DACL pin)** → **SGM8905** line amp (which has a shutdown pin), like JieLi's
  QFN48 reference board (`board_7911B.c`: DACL, hw_channel 0x01, amp mute).
- **v11**: internal DAC through `sdk/dac.bc` (IRQ_AUDIO = 13) + a search for the amp enable pin (weak pull-up /
  pull-down on 23 candidate pins, one at a time).
- **v11** on the pedal: `I2S OK`, `DAC OK`, but no sound, and **the button LEDs stay off** under OpenPocket → nobody
  drives the **74HC595**. Hypothesis: it drives the LEDs **and** the SGM8905 enable.
- **v12**: automatic search for the 595's pins (DS, SHCP+STCP, OE), weak pulls only; witness = LEDs blinking.
  The Pocket X has no knob LEDs (button LEDs only); the 595 search was judged too indirect.
- **v13**: direct sweep, each candidate pin driven 3.3 V then 0 V (1.5 s each), one at a time, with a continuous
  beep on the internal DAC: listen for when the sound comes out.
- **v13** on the pedal: **a click in the output when PA9 changes state** (first sign of life of the output).
- **v14**: PA9 (AMP) and PA7 (MOD) set by hand: free / 3.3 V / 0 V / I2S data; simultaneous beep on the internal
  DAC and on I2S.

## ✅ Audio path complete (v14, 2026-11-01)
| Role | Pin | Detail |
|---|---|---|
| MCLK | PA4 | output (mclk_output = 1) |
| SCLK / LRCK | PA5 / PA6 | JieLi is master |
| **Guitar input** | **PA8 = DAT1** | I2S ALNK0, 24-bit stereo (16-bit up to v25), 44.1 kHz (external ADC); guitar on the **left** channel only |
| **Output** | **PA7 = DAT0** | I2S ALNK0 → external DAC "344C" → SGM8905 amp → OUT + headphones |
| **Amp enable** | **PA9 = 3.3 V** | GPIO output high (the click = the amp powering up) |

The internal DAC (DACL) is NOT used by the Pocket X. Guitar → output passthrough: clean.
Driver setup: `iis_open` IIS_PORTA, channel_in = BIT(1), channel_out = BIT(0), master, MCLK, then
`iis_set_sample_rate(44100)`, `iis_channel_on`, ALNK0 IRQ = 11 → `iis_irq_handler(0)`.

## v15: the DSP engine in the audio path
- `firmware/base/audio.h` and `px_audio.h`: I2S (in PA8 / out PA7), amp PA9, engine `engine/px_dsp.c` in the ALNK0 IRQ (blocks of 128
  frames), guard: > 85 % load for 8 blocks in a row → BYPASS. Load shown as "CPU xx%".
- Float on the hardware FPU: `-mcpu=r3 -mfprev1` (the JieLi SDK's options for the WL82). Without them clang emits
  software calls (`__addsf3`...). `firmware/px_math.c` = a small libm (sin/cos/exp/log/pow/sqrt, error ~1e-6).
- Engine (~250 KB) in the RAM area `POOL` 0x01C20000 (0x54000 bytes), like CLIPPER. PX_OVERSAMPLE = 2 to begin with.
- **v15** on the pedal: **BYPASS** (engine > 85 % of real time).
- **v16**: engine optimised without changing the sound (−120 dB difference checked on the PC, 8 presets): cabinet as
  a linear dot product (doubled history, unrolled ×4), no more modulo in delay/reverb, anti-denormal ±1e-15, plus a
  measurement screen at boot (INT/FLOAT speed, % of real time per engine configuration).
- **v16** on the pedal: **CPU 52 %**, real time OK. But the sound was too bright, lacking lows.
- **v17** (sound): measured on the PC (plucked string, octave bands) → the 100–200 Hz band at −20 dB on 5150/Djent.
  Causes: (1) cumulated low cuts before the distortion (200 Hz tightness + 90/110/130 Hz stages); (2) cabinet = the
  response of 6 filters TRUNCATED at 256 taps (5.8 ms) → the ~100 Hz low bump disappeared.
  Fixes: cabinet run directly as 6 biquads (exact response, ~8× less work), lower stage low cuts and tightness
  (5150: 35/50/70 Hz, tightness 100 Hz), 5150/Uber low shelf raised to 170–180 Hz, resonance ×10 dB, presence
  ×6 dB, cabinet presence peaks reduced. Result: +10 dB at 100–200 Hz. Amp oversampling back to ×4 (CPU freed by
  the cabinet).

## v18–v20: hardware SPI screen + DSP on the second core
- **Screen**: hardware SPI1, group A (PB6 = CLK, PB7 = DO, IOMAP_CON1 bit 4 = 0, from the SDK's `spi.h`), registers
  and CON = 0x4021 taken from `fm1_lcd_hw.h` (Felucca), 20 MHz clock (BAUD 2), pixels sent by DMA with a double
  buffer of 1024 pixels. `LCD_HW 0` in openpocket.c = back to software SPI.
- **Second core**: Felucca's `fm1_cpu1.h`, started at the beginning of app_main. Block N goes to CPU1 at the end of
  IRQ N, IRQ N+1 collects it (same latency). `FM1_CPU1_FALLBACK 1`: if it fails, CPU0 takes over alone.
  Bottom right: `C1 xx%` (second core) or `C0 xx%` (fallback). `DSP_CPU1 0` = everything on CPU0.
- The SDK's CORDIC (`tanh_float`) was studied: a `copex` instruction + a division → no faster than our rational tanh.
- **v17 on the pedal: NO SOUND.** Cause found by simulating on the PC with OUR libm (px_math.c): `cosf` = sin(x+pi/2)
  with ~4e-6 error; the lowered low cuts (30–35 Hz) computed at 176.4 kHz (×4) need cos(w) with w ~ 1e-3: unstable
  biquads → NaN → permanent silence (no BYPASS: the computing time stays normal). Invisible on the PC with the
  standard libm.
- **v19**: sin/cos rewritten (reduction to [−pi/4, pi/4], pi/2 in two parts, separate polynomials); 4,860
  amp × cabinet × extreme-setting combinations tested with px_math.c: 0 NaN. Safety net: NaN/Inf detected at the
  output → silence + engine reset by the main loop. LESSON: always simulate with px_math.c, not the system libm.
- **v20** = v19 + hardware SPI screen (LCD_HW 1) + DSP on the second core (DSP_CPU1 1). Fallback switches in
  openpocket.c: `LCD_HW 0` (software screen), `DSP_CPU1 0` (everything on CPU0); build.sh only assembles fm1_cpu1.S
  when DSP_CPU1 is 1.

## v21–v23: content
- **v21**: engine v2 (10 blocks, 13 amps), "chain + block page" UI without flicker. **v22**: Inter font, no more
  measurement screen at boot.
- **v23**: engine v3, 11 blocks (PITCH and SPECIAL FX added), 118 models voiced from circuit analyses; English UI;
  readable values (`+5TH`, ms, dB). Fixes found on the PC: denormal numbers in digital silence (the amp stages' bias
  swallowed the anti-denormal offset), phase-aligned pitch shifters (SOLA), normalised saturating delays
  (self-oscillation only above FDBK ≈ 90). Benchmark (`BENCH_AT_BOOT 1`) updated: GROOVE METAL preset (5150 III,
  4 stages) and the heaviest blocks.

## v24: USB update
- USB-MIDI (felucca-bsp `chip/usb.c`, polled by TIMER5 at 2 kHz) and the M-UPGRADE update entry (`chip/ota.c`): the
  package's loader is staged at 0xE0000 (5 × 4 KiB), then the pedal restarts into it.
- Loader (`loader/`): felucca-bsp adapted to the Pocket X: app area [0x4000, 0xA3000) like PocketX_016, flash
  header never written, update records erased between 0xA3000 and 0xFF000.
- Packages (`tools/pxpkg.py`): rebuild the official PocketX_016 app area byte for byte.

## v25: presets in flash, finer cabinets and amps
- Presets (`firmware/px_store.h`): 42 slots of 96 bytes, two A/B copies (0xF0000 / 0xF2000, two 4 KiB sectors
  each), "OPPS" header written last (sequence + CRC16), read back after writing. Bytes 0xF00..0xF7F of each sector
  always left erased (the loader reads that area for its records). While writing: output muted, ALNK0 IRQ masked,
  second core between two blocks (its loop runs from RAM).
- Cabinet: 128-tap FIR after the biquads (cone resonances 1.2–6 kHz drawn per model, off-axis mic comb, rear wave of
  open-back cabinets), normalised to unity at low frequencies. (Replaced in v27, see below.)
- Amp: asymmetric triode `u≥0 ? 0.7·tanh(u/0.7) : tanh(u)`, bias shift driven by an envelope (100 ms, 0.45 for
  ≥4 stages, 0.35 otherwise), sag on the preamp too (×0.3). `px_trim.h` recalibrated (29 amps at 0.160 RMS).
- Memory: 327,724 of 344,064 bytes in POOL.

## v26: 24-bit input and output
- Symptom: constant hiss ("shhh") on high-gain amps, even with no cable plugged in, absent with the official
  firmware. First hypothesis: the 16-bit input (quantization noise × the amp's ~60 dB of gain). It did not fix the
  hiss (the real cause was found in v28), but 24 bits is what the official firmware uses and is kept.
- Official firmware (PocketX_016, disassembled): same ALNK0, same pins, master, 64 SCLK per frame, but
  - every channel is set to **24 bits** (function 0x0200664A: `CON1 |= 1 << (4*ch + 2)`, the SDK's `data_width` bit);
  - LEN = 128 and a 512-byte half-buffer processed as 64 frames of 8 bytes: in 24-bit mode LEN counts 32-bit words;
  - input read as `(word << 8) >> 7`: 24 bits right-justified (bit 23 = sign), **left channel only**, ×2;
  - output written as 24-bit integers clipped at ±8,388,600 into both words of the frame.
- OpenPocket v26: `data_width = BIT(0) | BIT(1)`, `sr_points = 256` (still 128 frames per block), 4,096-byte heap for
  the driver's buffers (2 channels × 8 × 256). Output in 24 bits (the MASTER volume no longer costs resolution).
- Diagnostic: the tuner screen shows `IN L -xx R -xx dB`, the level of each input channel (dBFS).

## v27: CPU load (DOOM SLUDGE in BYP at 89 % on v26)
- v25's 128-tap cabinet FIR was expensive on the pedal's FPU (128 multiplications per sample). Replaced by the same
  response computed another way: a few sparse taps (direct sound, mic comb, rear wave) + the cone's resonances as
  2-pole resonators (h[k] = A·dec^k·sin(kw), exactly what the FIR summed), the 6 strongest of the 10 draws:
  0.5 dB RMS from v26, 1.9 dB at worst on the small peaks.
- Amp: the triode curve folded into a single rational function (identical to 2e-7), the bias envelope pre-scaled
  by its depth, and the two supply-sag divisions computed once per 64-sample block instead of every sample.
- Instructions per sample (PC, no vectorisation): DOOM SLUDGE v24 1,936 / v26 2,655 / v27 2,240;
  GROOVE METAL 1,430 / 2,151 / 1,744.
- Engine: 326,612 bytes in POOL.

## v28: the guitar from the left channel only (the hiss came from the right channel)
- Measured on the pedal (tuner screen, v26): nothing plugged in, L −76 to −91 dBFS, R −77 dBFS steady; strumming
  hard, L −17 dBFS, R still −77 dBFS. The guitar only reaches the left channel; the right one only carries a
  constant noise, 14 dB above the left channel's floor.
- The (L + R) / 2 mix put that noise into the sound: signal-to-noise 60 dB (−17 vs −77). Left channel only, like the
  official firmware: 74 dB (−17 vs −91), up to 14 dB less hiss, amplified just as much by the high-gain amps.
  Input = L × 0.5: same level as before, settings unchanged. Confirmed on the pedal: the hiss is gone.

## v29: SYN block, block order per preset
- 12th block **SYN** (appended to the block indices, so presets keep theirs): ORGAN, CHURCH ORGAN, STRINGS, CHOIR
  (delay-line octave shifters on a level-held signal + key envelope; ensemble; vocal formants), MONO SYNTH (pitch
  tracker: 50 Hz high-pass + fixed 110 Hz 4th-order low-pass so the fundamental always dominates, armed zero
  crossings with hysteresis, sub-sample timing, median of 3 periods; polyBLEP saw/pulse; 24 dB ladder with tanh
  feedback), PWM SYNTH and BASS SYNTH (comparator / flip-flop dividers, no tracking delay), 12-STRING, ACOUSTIC (body
  EQ + early reflections), SITAR (one-sided bridge contact + 6 damped comb strings), BARITONE.
  Tracker on single plucked notes E2..A3 (synthetic): locks in 30–80 ms, within 2–9 cents.
- **Block order**: `px_preset_t.order[12]`, a permutation (invalid → default). The engine builds the list of active
  blocks once per call and runs it per sample; DRIVE and AMP share oversampling set A when AMP follows DRIVE,
  otherwise a separate DRIVE uses set B. Default order checked bit-exact against v28 on all 42 earlier presets.
- **SOLA search** (pitch shifters) made ~3× cheaper: coarse pass every 8 offsets on every 4th sample, fine pass ±7;
  energy on the target note unchanged or better (440 Hz octave up: 94.6 % → 99.8 %).
- **Preset store format 2**: 52 slots of 112 bytes (sector 0: 34 slots, 0xEF0 bytes; sector 1: 18). A format-1 copy
  (v25..v28) is converted at boot; the first save writes format 2 to the other copy.
- Instructions per sample (PC, scalar): B3 ORGAN 2,329 (the heaviest new preset, as heavy as DOOM SLUDGE 2,311);
  others 709–1,795. Engine 339,660 bytes in POOL (344,064).

## v30: the power supplies at start (issue #1, BlackBox boot loop)
- Report: OpenPocket on a BlackBox worked for an hour after the install, then, after the power switch, restarted every
  ~4.3 s (USB up 2.9 s, down 1.3 s), on battery and on USB alike.
- BlackBox_020 and PocketX_016 drive the hardware identically (same pins, same register values, same `isd_config`);
  both run the same power init at start (BB `f_0200698a`, PX `f_0200664a`): P3_ANA_CON0 LDO on → DC-DC on → LDO off
  (1 ms apart), CON1 0x20, CON5 0x40 → [3:0] 4 → [5:4] 3 (0x74), CON6 12 → [2:0] 7, CON7 0xA5, CON8 0x96,
  CON9 |= 0x10 (SYSVDD_VOL_HD_SEL) → [3:0] 15, CON0 |= 0x80, ANA_KEEP [5:0] 0, CON2 |= 0x40.
- OpenPocket did none of it (the BSP had `fm1_vdc14_raise`, `fm1_sysvdd_raise`, `fm1_power_stock`, never called). The
  always-on P33 keeps these across a warm reset, so after an install (the official firmware ran last) the levels
  were the stock ones; after a power cycle, the boot loader's (VDC14 3 = 1.40 V, SYSVDD 11 = 1.26 V, LDO), too low
  for both cores at full load on some units.
- `fm1_cstart` now raises VDC14 to 7 and SYSVDD to 15 (a level per 100 us), sets the stock CON6 / CON9 bits, moves
  the 1.3 V supply onto the DC-DC, CON5 0x74, before anything else runs. The boot screen shows `PWR a>b c>d DCDC ...`
  (as found > as set).
- Boot OK now at 6 s (4.5 s of DSP), so a pedal that resets under load is caught by the guard and the safe mode.
- Installer: rescue mode (packages fetched beforehand; the install starts the moment a pedal answers; the upgrade
  command halts the DSP and the second core at once). BlackBox install allowed again, with a note.

## v31: the long-press reset (issue #1, the real cause)
- v30 on the BlackBox: `PWR 3>7 11>15 DCDC OFF>ON` (supplies set), and still a reset: "LAST BOOT FAILED, STOPPED AT
  FIRST 6 S", no crash, BLOCKS 976 (2.8 s of audio), LOAD 40 %, NAN 0. The .noinit diagnostics survived, so a chip
  reset, not a power loss; no CPU exception.
- The period, ~4.3 s, matches the long-press reset (P3_PINR_CON, 0x94: the SDK's `struct long_press` offers 4 s or
  8 s, rising or falling edge, on one wake-up pin) + the boot. BlackBox_020's app entry (`f_0200698a`) writes
  PINR_CON = 0 before anything else; OpenPocket never touched it. The always-on P33 keeps 0 across a warm reset (an
  install), not across a power cycle: then the boot loader / power-on value applies, and on the BlackBox the pin
  sits at its active level (the Pocket X's does not).
- `fm1_cstart` now reads P3_RST_SRC and PINR_CON, then writes PINR_CON = 0, first thing. The boot screen shows
  `RST <source> PINR <found>`, and "LAST BOOT FAILED" shows "RESET BY: ..." (P3_RST_SRC bits: 0 power-on,
  1 VDDIO low, 2 watchdog, 3 VCM, 4 long press, 5 1.2 V, 6 soft).

## v32: the web editor
- `firmware/px_editor.h`: SysEx `F0 7D 4F 50 cmd ... F7` (docs/EDITOR-PROTOCOL.md), frames from usb.c's `sx_frame`
  (`ota_frame_get`), handled in the main loop before `ota_service` (which takes any frame), replies through
  `ota_wire_send`, as Felucca's editor.c. It edits `PRESET` directly: `audio_poll` applies, `ui_render` redraws.
- Pushes: while watched, a hash of PRESET + slot + focused block + `st_gen`; a change made by the editor itself updates
  the hash, so only the pedal's own changes are pushed (40 ms apart at most). The watch ends 5 s after the last request
  (the page pings every second; PING also returns the DSP load).
- Slots: `SLOT_PUT` changes `st_slot` in RAM; `COMMIT` (or bit 1) writes the whole store once (`st_commit`): an import
  of 52 presets is one flash write. `BLK_COL` moved to `ui_colors.h` for `CAT_BLOCK`.
- `tests/edsim.c` compiles px_editor.h + px_store.h + the engine catalogue on the PC (stdin / stdout like devsim).

## v33: the supplies back to the boot loader's levels
- On a Pocket X with v32: DOOM SLUDGE C1 77 % (61 % on v28/v29, same engine), SMOOTH LEAD in BYP (92 %), crackles on
  nearly every sound. The engine code has not changed since v29; v30 added the stock power settings (VDC14 7,
  SYSVDD 15, CON6 / CON9 bits, the 1.3 V supply on the DC-DC, CON5 0x74). They were not what fixed issue #1 (the
  long-press reset, v31, was), so v33 removes them: the boot screen only shows the levels found (`PWR 3 11`).
- Not proven yet: the Pocket X test of v33 (C1 % back to ~61 %, no crackle) decides. If it does not, the next suspect is
  the code cache (the DSP runs from XIP: px_engine_process 17 KB, the hot path ~20 KB, shared with CPU0).
- Confirmed on both pedals with v31's boot line: a Pocket X starts with `PINR 08` (bit 0, the enable, clear: the
  long-press reset off), Megal's BlackBox with `PINR 09` (bit 0 set: on). Bit 0 is the enable in the SDK's
  `long_press_reset_enable` (cpu.a power_wakeup.c: P33 0x94 bit 0, and 0x95 bit 0).

## v34: amps, drives and the V30 cab fitted to captures
- The 29 amps / 27 drives / 15 cabinets voiced by ear make way for 8 amps, 4 drives and 7 cabinets whose numbers are
  fitted by computer to Neural Amp Modeler captures of the real units, on real DI guitar
  ([FITTED-MODELS.md](FITTED-MODELS.md)). Same engine, same CPU (+3 % on average, the heaviest preset a little
  lighter), only the tables change.
- Presets saved before v34 are converted at boot to the nearest new type (store format 3, `px_type_v34`); the other
  blocks are untouched. Six factory presets are renamed (their old amp no longer exists).

## v35: new reverbs, 2 s delay, 16-bit delay lines (89 KB of RAM freed)
- RAM: the engine filled 332 of the 336 KB POOL. The delay line (1 s in 32-bit floats: 188 KB) and the reverb
  (107 KB) are now 16-bit (rounded; once a tail is down to the last bits it is cut towards zero, so a loop cannot
  hold a tiny pattern for ever). Engine: 247 KB, 89 KB free (for BLE / MIDI later), with a delay twice as long.
- Reverb: v34's was a Freeverb (8 combs + 4 all-pass): metallic on long tails, the same decay at every frequency
  whatever DECAY said. v35: an 8-line FDN (Hadamard, per-line decay at low frequencies and at 4 kHz, 4 slowly
  modulated lines, early reflections) and Dattorro's plate, at 22.05 kHz in one 16-bit ring (32 KB). Measured with
  a noise burst: the decay time now matches DECAY (HALL at noon 2.6 s target: 2.60 s), and the tail's fine spectrum
  is 1.5 to 2.5 dB closer to a dense random field on HALL, CATHEDRAL, PLATE, SHIMMER, AMBIENT (less ringing).
  Each type's level matches v34's (on guitar, within 0.2 dB); SHIMMER's octave builds up about as in v34.
- Delay: TIME up to 2 s (0–80 = 20–1000 ms, 80–100 = 1–2 s), cubic (Hermite) reads in the delay, chorus,
  flanger, vibrato, rotary. Presets saved before v35 are converted at boot (store format 4) and on import in the
  editor (files without `"v": 35`). The read position is now split into whole samples and a fraction before
  wrapping round the line, so it can never land past the end.
- CPU (PC, instructions per sample): reverb presets +80 to +100, delay + reverb presets up to +200, 1,889 on average
  over the 52 presets (v34: 1,807); the heaviest, SMOOTH LEAD, 2,582 (v34: 2,418). To be checked on the pedal.

## v36: levels, 24 factory presets, a new screen
- Levels, measured as loudness (ITU-R BS.1770, gated) on real DI guitar at the pedal's level, by
  `tools/calib_levels.py` (writes `engine/px_trim.h`): each amp has a level curve over GAIN (11 points: noon at
  -23 LUFS for every amp, +-3 dB over the knob; v35 spread 46 dB between JCM900 at 0 and full), every cabinet sits
  with the 1x12, and every other type (knobs at noon) keeps the loudness when switched on. The 52 v35 presets
  spanned -53 to -10 LUFS; the 24 v36 presets are all at -23.0 +-0.2 LUFS (`tools/level_presets.py`).
- The preset LEVEL: GT block, 3rd setting "PRESET LVL" (-15..+15 dB, 0.3 dB a step). Store format 5 and the editor
  (files without `"v": 36`) set it to 0 dB on older presets.
- Output: a soft limiter (transparent to 0.8, rounds off to 1.2) instead of the hard clip at 1.2.
- SLOW SWELL: 2.9 ms look-ahead and a more sensitive attack detector, so a pick attack is not heard before the swell.
- 24 factory presets round the fitted amps (near noon); 25..52: USER (clean TWIN + 1x12 + room).
- Screen: PLAY (number and name large, amp / cab, the chain in colour, the selected block) and EDIT (the block on its
  colour, model, 5 settings with value + bar), new Inter fonts (26 and 58 px). `tests/uisim.c` draws every page on
  the PC and checks that each full page paints every pixel.

## v37: NAM A2-Lite, user models
- AMP type 8, NAM: a Neural Amp Modeler A2-Lite network (3 channels, 23 dilated layers, LeakyReLU, 16-tap head;
  1871 weights, about 1,730 multiply-adds a sample) in fixed point, `engine/px_nam.c` ([NAM.md](NAM.md)). Activations
  Q20, weights Q24, 64-bit sums shifted by a constant. Against the float reference (`tools/nam_a2.py`): 83 to 100 dB
  signal to error on the factory models (`tests/test_nam.py`).
- This CPU has no data cache in front of the SRAM, a taken branch costs about 8 cycles, and it issues two
  instructions together (a 32-bit multiply-add with a 16-bit load). A first version (a loop over taps per frame,
  16-frame chunks) measured 223 % of real time on the pedal. Now: blocks of 32 frames, layer by layer. Gather: each
  frame's inputs to the layer into one row of X (`[u, 1.0, taps x 3 channels]`, 3 words a move), from the layer's
  ring (interleaved [time][channel], (kernel - 1) x dilation + 32 frames). Dot: the layer's 3 weight rows against 4 X
  rows at a time (`pxn_dot3x4_asm`: a weight read once for 4 frames, 4 multiply-adds paired with 4 loads, 5 issues,
  in the hardware loop). Post: LeakyReLU (predicated, no branch), the head sum, the residual 1x1 written straight
  into the next layer's ring. The head is a 50-long row (bias, 16 taps x 3) over the head's ring. The hand routines
  are in the flash with their callers; `px_nam_check()` compares them with the C ones at boot (pattern with both
  signs, rows of 20 and 47, with and without the residual) and keeps C on any difference.
- Memory: the rings (83.4 KB) in the POOL region beside the engine, the model (weights converted, 8 KB) and the
  block buffers (7 KB) in RAM. A direct call between the flash (0x02xxxxxx) and the RAM code (0x01C0xxxx) is out of
  the call instruction's +-4 MiB: `tools/check_calls.py`, run by `firmware/build.sh` before linking, refuses any such
  call and names it (a first v37 build put the audio path in RAM: the linker refused 478 calls, naming only one).
- User models: 16 slots of 8 KiB at 0xC0000 (`firmware/px_models.h`), written by the editor (protocol 2); the update
  loader keeps that range (`loader/ldr_core.c`, `LDR_NAM_LO/HI`). The weights are read from the flash (SPI, with the
  audio held) when a preset picks the model.
- Level: every model plays at the same loudness: a trim per model (-20 LUFS on `docs/nam_di.wav`, measured by the
  editor for user models and by `tools/mk_nam_factory.py` for the factory ones), then one constant that puts NAM +
  1x12 at GAIN noon at the fitted amps' -23 LUFS.
- 24 factory presets rebuilt on the 12 factory NAM models, levelled by `tools/level_presets.py`; USER slots: NAM TWIN.

## v40: the audio path in RAM, the delays at half rate (the stock firmware's way)
- The per-block code runs from RAM, as the stock firmware's per-sample library does (66 KB copied at boot). The 478
  refused calls of the first v37 try came from the RAM code's place: at `0x01C0xxxx` it is just out of the call
  instruction's +-4 MiB of the flash. `RAMTEXT` now sits at `0x01C5E000..0x01C74000`, within reach of every flash
  address up to `0x0205E000` (`app.ld` asserts it). So the flash and the audio path call each other directly, and no
  pointer is needed.
- How the code gets there: `px_dsp.c` is compiled with `-ffunction-sections`, then `tools/ramcode.py` renames the listed
  functions' sections (`.text.NAME` to `.ram_text.dsp.NAME`) and px_dsp.o's plain `.text` (the hand-written routines).
  `openpocket.o` gets the same for the audio interrupt's own functions. The source carries no attribute, so the
  compiler inlines exactly as before (this compiler does not inline into a function that has a section of its own:
  v37's note). About 75 KB moves.
- `check_calls.py` lets `.ram_text.dsp.*` call the flash and be called from it, and still refuses it for the code that
  runs with the flash off (`.ram_text` proper, `.c1_text`).
- The room for it: the delay line at 22.05 kHz, as the stock delays (88 KB for 2 s instead of 175 KB). `POOL` goes from
  336 to 248 KB.
- Delay at half rate: one tick every two samples, with the pair averaged in, the output interpolated between the last two
  ticks, and a fixed lift `1.15 d - 0.15 d_prev` on the wet output. Glide and envelope coefficients are doubled, and
  depths and LFO steps are converted to ticks; the lane's lag is 128 ticks. On the lead loop through an amp and a
  cabinet, the wet part is within 0.1 dB, and within 0.4 dB up to 6 kHz. `tests/engine/wet_test.c` still checks the
  lane against the inline delay, bit for bit.
- Reverb: the low cut after the tank runs at the tank's rate. TONE stays at the full rate, because it takes down the
  half rate's images. The output is within 2e-4 of v39 on every type.
