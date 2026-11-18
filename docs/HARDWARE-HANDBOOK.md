# AC791N handbook: what OpenPocket learnt about the chip (for FM-1 firmware work)

This document collects what the OpenPocket project found about the JieLi AC791N, the chip in the FM-1, the Donner Pocket X
and the M-VAVE BlackBox. It covers the CPU, memory, flash, second core, audio, accelerators and radio, the methods that
worked for DSP on this chip, the mistakes that cost us builds, and what M-VAVE's own firmware does. It is written for a
developer, or an assistant, building FM-1 firmware on felucca-bsp: OpenPocket uses the same BSP (`firmware/bsp/`), so the
register names and helpers below are the ones you already have.

**How sure each fact is.** **Measured**: seen on a real unit (pedal or FM-1). **Read**: seen in code (ours, the JieLi
SDK, or the disassembled stock firmware) but not measured. **Likely / guess**: reasoned, one step unproven.

**What carries over to the FM-1.** Everything about the chip carries over: CPU, memory, flash controller, second core,
ALNK (I2S), FFT block, cache, power, reset and USB. Board facts (which pin does what, the external ADC/DAC, the button lights)
are the Pocket X / BlackBox's and are marked **[pedal]**. Use them only as examples of how to find your own.

Sources in this repository: `docs/AUDIO-SDK.md` (the audio bring-up, v10..v37), `docs/STOCK-FIRMWARE.md` (the stock
firmware read in three passes), `docs/NAM.md` (the fixed-point network), `docs/BLE-TEST.md` (Bluetooth),
`docs/RESCUE.md` (mask-ROM recovery), `firmware/bsp/chip/*.h` (felucca-bsp, with our additions).

---

## 1. CPU and clocks

| Item | Value | How sure |
| --- | --- | --- |
| Cores | two pi32v2 cores (JieLi's own ISA), CPU0 and CPU1, shared memory | read, measured |
| Core clock | **360 MHz** under M-VAVE's bootloader (PLL1 = 24 MHz / 12 x 270 = 540 MHz, system clock 2/3 of it); **320 MHz** under m-bootloader 1.2 (PLL1 off), so every firmware ran 11 % slower and the peripheral clock was 53 MHz instead of 60 | measured (fm1-x0x debug build; `fm1_clock.h` puts the stock clock back) |
| Bus clocks | HSB /2 = 180 MHz, LSB /3 = 60 MHz (0x10008 = 0x00010200) | measured |
| Time base | **TIMER4 free-running at 24 MHz** from the crystal (`0x10804`, `fm1_ticks()`, `FM1_TICKS_PER_US 24`) | measured |
| FPU | single-precision float in hardware, **only with `-mcpu=r3 -mfprev1`** (the SDK's options for the WL82). Without them clang emits soft-float calls (`__addsf3`...) | measured |
| Division | integer and float divisions are slow: take them out of per-sample loops (compute `1/x` once a block) | measured (v27: two divisions a sample moved to once a block) |
| 64-bit | 64-bit multiply is native (`r11_r10 = r1 * r2`); **64-bit division is not: `__udivdi3` is missing from the toolchain's runtime**, the link fails | measured (v38 build error) |

### The performance model that held on the pedal (measured)

- **A taken branch costs about 8 cycles.** A per-sample `switch` or `if` on a block's type, inside the loop, costs more
  than the arithmetic. Make one loop per type (C `always_inline` with constant parameters) and decide once a block.
- **There is no data cache in front of the SRAM.** The data cache serves the flash only. Every load from RAM pays the bus.
  Keep inner-loop data in registers, read each weight once for several frames, and avoid scattered reads.
- **Two instructions can issue together**: a 32-bit multiply-add with a 16-bit load (5 issues for 4 MACs + 4 loads in
  our hand routine).
- **Load-use stall:** a multiply-add paired with the load of the value it needs next waits (3.2 cycles against 2.2
  without the wait). Load each value 2 to 4 steps before its use (`tools/gen_rdq.py` unrolls that way).
- **Hardware loops** (`rep N { ... }`) have no branch cost. The compiler uses them for simple counted loops. The stock
  firmware's sample loops are all hardware loops.
- **Flash code runs through the instruction cache** (XIP), shared by both cores. A cache miss on flash is expensive. The
  stock firmware keeps every per-sample routine in RAM (section 9), and so does OpenPocket since v40, with one
  exception (next point).
- **Code in RAM is fetched more slowly than code in the cache** (measured, v40.5, sixteen copies of one instruction in a
  counted loop): a 6-byte instruction that works for 1 cycle takes 2.14 cycles from RAM and 1.49 from the flash; a
  6-byte `smax` paired with a load 2.27 against 2.08. RAM delivers an instruction word in about 1.5 cycles, the cache
  one every cycle, and a hardware loop (`rep`, bodies up to 32 bytes) fetches nothing after its first turn. Compiled
  float code is not affected (a float operation is 4 bytes for 2 cycles), and the effect loops run no faster from the
  flash (tried: the same load, more jitter). The NAM network's hand loops are multiply-adds paired with loads, 6 bytes
  for 2 cycles: from RAM they wait for their own instructions. `rdm6`, `rdm15` and `post5` (6.6 KB) therefore lie in
  the flash (`PXN_HOT`, `engine/px_nam.c`): the whole network takes 6 % less time, and editor traffic and knob turns
  over USB (flash code on CPU0, 100 requests a second) do not change a heavy chain's load or its peaks.
- **The cycle costs, in a hardware loop** (measured, v40.4 and v40.5; a timer tick is 15 cycles): a load 2, a store 1.5,
  an ALU instruction 1, a 32x32 multiply or a 64-bit multiply-add 2, a float add or multiply 2 each, a float division
  4, a taken branch 6.4, a call and return about 90. A 16-bit load or store written with a multiply or multiply-add in
  one instruction (`#`) is free; written with a 1-cycle instruction it costs its own time (the instruction is then
  free). A value used by the instruction after its load costs 1 more cycle. Multiply-adds into the same accumulator
  one after the other take 3 cycles each (2 when two or more accumulators take turns), and 64-bit shifts of the same
  pair 2 each (1 when they alternate). Loads and stores in turn cost less than the same number grouped (a load and a
  store 2.5 to 3 cycles together). The two cores do not slow each other's memory accesses.
- **libm precision matters.** Our first `cosf` (sin(x + pi/2), error 4e-6) made biquads at 30 Hz with 4x oversampling
  unstable, giving NaN and then permanent silence on the pedal while the PC (system libm) was fine. Always simulate with
  the libm you ship (`firmware/px_math.c`).
- **Denormals:** digital silence through feedback filters falls into denormals and the FPU slows down. We add +-1e-15,
  alternating sign per sample, at the chain input and after stages that can swallow it (bias, gates).

### The second core (felucca-bsp `fm1_cpu1.h`; measured)

- **Start it at power-on**, before audio and timers run, with interrupts off (Melodee's and X0X's finding). The entry address
  goes in the word at `0x01C7FFF8`.
- **One job at a time:** `fm1_cpu1_run(fn, arg)` → do your own share → `fm1_cpu1_wait()`. CPU1's idle loop is in RAM
  (`.c1_text`). The job itself runs from flash (v40: mostly from RAM, but it still calls flash code), so never switch
  the flash off (a flash write) with a job outstanding.
- **Some units misread shared RAM on CPU1** (0x00200000 where 0 is stored, more often under load). The BSP guards
  every job with a check word, and `FM1_CPU1_FALLBACK 1` retires CPU1 on any misread or fault, so CPU0 continues alone.
  OpenPocket builds with the fallback on and always keeps a one-core path that works.
- **Sharing:** a single writer per word and `csync` around the hand-over. We saw no coherence problem with buffers
  written on one core and read on the other after the job's completion.
- **Patterns that worked:**
  1. **Whole chain on CPU1, one block late:** block N goes to CPU1 at the end of IRQ N, and IRQ N+1 collects it before
     output. CPU0 is free between interrupts (UI, display) and never touches audio.
  2. **Work split inside a block:** a causal network (each layer's frame t depends only on frames <= t of the layer
     before). CPU1 takes the first 12 or 16 frames of each 32 through all layers without waiting. CPU0 takes the rest,
     waiting per layer on one counter.
  3. **Pipeline:** CPU1 runs the effects after the amp on the previous block while CPU0 runs the effects before the amp
     on this block, then both share the network. This costs one extra block of latency (`tests/engine/pipe_test.c`: same
     output, one block later).
  4. **v39 "wet lanes"**: delay and reverb run beside the chain, two blocks late (section 8).
- **Lesson from v40.4 (measured):** a job per interrupt that must end before the next one puts the two cores' work in
  series. With the network split by layers, CPU1's last network block could only start when CPU0 had finished its own
  share, so a block needed CPU0's share plus one network block on CPU1 (12 % of the block) within 2.9 ms: loads above
  about 88 % a core made the interrupt wait. After the interrupt has been held off (a preset change holds it off for
  10 ms), it runs block after block until it has caught up with the converters, and every such wait then counts as
  load. Let CPU1 run one worker across the interrupts, ask it for the output-critical work by a counter, and wait only
  for that.
- **Lesson from v39 (measured as a defect):** with pattern 3 we gave CPU1 the delay and reverb as a job in the gap
  after the block, before the next interrupt. The gap was too short, because the network shares its work between the
  cores and only half of the freed time came back to the gap. The job was cut every block and the wet sound came out
  chopped at the block rate (a whistle). **Never put work in "the time left before the next interrupt" without
  measuring that time under the heaviest load.** Put it where the work-sharing accounts for it.

---

## 2. Memory map

| Range | Size | What | How sure |
| --- | --- | --- | --- |
| `0x02000120..` | up to ~580 KB (`XIP` in `app.ld`) | flash code, executed in place; **app.bin file offset 0 = 0x02000120** (not 0x02000000: several of our early address readings were off by 0x120 for that reason) | measured |
| `0x01C00000..0x01C08000` | 32 KB | free since v40 (until v39: `RAMTEXT`, 24 KB) | read (`app.ld`) |
| `0x01C08000..` | 96 KB | `RAM`: `.data`, `.bss` | read |
| `0x01C20000..0x01C5E000` | 248 KB | `POOL`: free SRAM, OpenPocket's DSP engine (~246 KB used in v40: the delay at half rate freed 87 KB) | read |
| `0x01C5E000..0x01C74000` | 88 KB | `RAMTEXT` (v40): code copied to RAM at boot. The flash driver and CPU1's loop (`.ram_text`, run with the flash off), and the audio path (`.ram_text.dsp.*`, about 75 KB, `tools/ramcode.py`). High enough to be within the call range of all the flash code | read (`app.ld`) |
| `0x01C74000..0x01C7C000` | | user and supervisor stacks with 256-byte guard bands | read |
| `0x01C7C000..0x01C7FD50` | | `NOINIT`: survives resets (crash records, boot counters) | read |
| `0x01C7FD50..0x01C7FFFF` | | reserved: boot info, the CPU1 mailbox/entry word (`0x01C7FFF8`), vector table | read |
| `0x01F28000..0x01F2F000` | 28 KB | **seven of the eight 4 KB data-cache ways turned into plain RAM** at boot (`firmware/px_cacheram.h`, from melodee). The data cache serves only the flash, so nothing is lost. OpenPocket puts the vector unit's inputs there, away from the RAM the CPUs use | measured |
| `0x01EE0400` | | FFT block / vector unit (section 6) | measured |
| `0x01EEE008` / `00C` / `010` | | cache control: CON (bit 14 = idle), data ways, instruction ways | read (melodee) |
| `0x01EEF0F0` | | a free-running timer the stock firmware reads to time its audio interrupt | read (stock) |
| `0x00050000 + port x 0x40` | | GPIO: OUT +0x00, DIR +0x08 (1 = input), DIE +0x0C, PU +0x10, PD +0x14. Port D = the flash (never touch) | measured |
| `0x00012E00` / `0x00015100` | | ALNK0 / ALNK1 (I2S) | read (SDK `WL82.h`) |
| `0x00010000`, `0x00011900`, `0x00051000+` | | CLOCK, ANA, IOMAP | read (SDK) |

**Stock firmware for comparison (read):** RAM code `0x1C00000..0x1C107E4` (66 KB, the whole per-sample DSP library),
copied at boot from app.bin offset 0x81760; an overlay window at `0x1C22340` (7 KB, two code sets swapped on demand);
four RAM slots from `0x1C23F80` where each effect type's code is copied when chosen. It also keeps the Bluetooth A2DP
PCM ring at `0x1F29EE0`, inside the cache-way range above (**likely**: the stock firmware also uses cache ways as RAM).

### Rules learnt the hard way

- **Call range:** a direct `call` reaches +-4 MiB. RAM code at the bottom of the RAM (`0x01C0xxxx`) is out of reach
  of the flash (`0x02000120..`): the linker refuses such a call, but names only one of the offending calls (a v37 build
  had 478). **v40's way out:** put the RAM code high in the RAM (`0x01C5E000..0x01C74000`). Every flash address up to
  `0x0205E000` is then within reach both ways, and `app.ld` asserts it. The audio path is moved there without touching
  the source (`tools/ramcode.py` renames `.text.NAME` to `.ram_text.dsp.NAME`, so the compiler's inlining is unchanged).
  `tools/check_calls.py` still refuses calls between the flash and the code that runs with the flash off (`.ram_text`,
  `.c1_text`); for those, call through a pointer the compiler cannot fold (`FL_FAR`, `fm1_flash.h`).
- **RAM is the scarce resource.** Count `.data + .bss + CPU1 stack (3 KB) + the SDK driver's bss` against 96 KB, and
  `.pool` against 248 KB and the RAM code against `RAMTEXT` (88 KB) since v40, on every change. We compile the objects for the target and add up the section sizes even when
  we cannot link.
- **Reading a crash screen's `EMU`** (the core's `EMU_MSG`, bit names from the SDK's `cpu/wl82/debug.c`): bit 0 misaligned
  access, 1 illegal instruction, 2 division by zero, 3 stack overflow (the stack guard), 8 ETM watchpoint, 16 to 20 FPU
  (invalid, infinity, tiny, huge, inexact). `DBG` is the bus side (writes into guarded ranges, unmapped addresses, the PC
  limit).
- **Stack guard:** felucca's EMU stack limit faults any `sp` outside `[_ustack_lo, _sstack_top]`. Task stacks placed in
  ordinary RAM (an RTOS, the BLE controller) crash at once with the guard on (section 11).

---

## 3. Flash (1 MiB SPI NOR)

**[pedal] layout as OpenPocket uses it. The FM-1's own layout differs: felucca's main store is `0x97000..0xE0000`.**

| Range | What |
| --- | --- |
| `0x00000..0x04000` | flash header, boot config: **never written** |
| `0x04000..0xA3000` | the application area (the update loader rewrites only this) |
| `0xA3000..0xFF000` | where the bootloader looks for update records: anything that ends like a record can be erased by an update, so user data there must be skipped by the loader (`loader/ldr_core.c`) |
| `0xA5000` | stock "VM" (settings; holds the radio calibration records, VM 106..108, 187). On a Pocket X backup (`fm1-1.bin`, its official firmware) the active VM area is at **`0xA1000`**, inside OpenPocket's application area: installing OpenPocket overwrites it, and that pedal's radio records are gone (`ble_rf.c` gives defaults then) |
| `0xBA000..0xBD000` | stock file `EXT_RESERVED/usr` (12 KB) |
| `0xBE000` | stock BTIF (Bluetooth pairing) |
| `0xBF000..0xFF000` | stock file `EXT_RESERVED/pd40j` (256 KB): 20 amp models x 8 KB, 20 cabinets x 4 KB at `+0x28000`, the preset bank at `+0x3C000` (80 x 92 B + `PATCHEND`), the `GLB1` settings log at `+0x3E000` (`STOCK-FIRMWARE.md` 10.4). OpenPocket's own areas below reuse this range: installing OpenPocket overwrites the stock user data |
| `0xC0000..0xE0000` | OpenPocket: NAM user models (16 x 8 KB) |
| `0xE0000..` | update loader staging (5 x 4 KB) |
| `0xEE000..0xF0000` | the Bluetooth audio test image's pairing store (test build 14, `firmware/bt/bt_ps.h`): two sectors of 5 copies of the library's pairing records each, sequence + CRC-32, the mark written last, read back after writing; bytes `0xF00..0xFFF` of each sector never written. Only that image writes it (its `FL_RANGE_OK`, `firmware/px_usb.h`); the release firmware and the update loader leave it as it is |
| `0xF0000..0xF4000` | OpenPocket presets, A/B copies, header written last (sequence + CRC16), read back after writing; bytes `0xF00..0xF7F` of each sector left erased (the loader reads that area) |
| `0xF4000..0xFC000` | OpenPocket: cabinet IRs (8 x 4 KB) |
| near `0xFF000` | key/MAC area |

**Writing the flash while audio runs (measured, v25 on):** the SFC (XIP) is switched off during erase and program, so
everything that runs then must be in `.ram_text` and touch only registers, the stack and RAM: no `.rodata`, no library
calls, no flash-resident helper. Interrupts off, and CPU1 idle between two jobs (its job code is in flash). We mute the
output, mask the ALNK0 interrupt, write, unmask, and fade back in.

**Reading raw:** the XIP window decrypts. Data written by another tool (the stock VM) must be read through the plain
window (`SFCENC_CON`, `fm1_flash.h`, `ble_vm.h`), not through `0x02xxxxxx`.

---

## 4. Power, reset, boot (measured on pedals)

- **Long-press reset:** P33 `PINR_CON` (`0x94`, bit 0 = enable) resets the chip when one wake-up pin is held for 4 or 8 s.
  The always-on P33 domain keeps it across a warm reset but not across a power cycle. The boot loader's value applies
  after power-on. On one BlackBox the pin sits at its active level, so the unit reset every ~4.3 s after power-on
  (issue #1). The stock app writes `PINR_CON = 0` first thing; **do the same**. Pocket X boots with `PINR 08`
  (off), a BlackBox with `PINR 09` (on).
- **Reset source:** `P3_RST_SRC` bits: 0 power-on, 1 VDDIO low, 2 watchdog, 3 VCM, 4 long press, 5 1.2 V, 6 soft. Show
  it on the boot screen; it named the long-press cause above.
- **Supplies:** the stock firmware sets VDC14 7, SYSVDD 15, the 1.3 V supply on the DC-DC and several P3_ANA_CON bits.
  We copied that in v30. On a Pocket X the same engine then measured **77 % instead of 61 %** CPU, with crackles
  (v32), so v33 went back to the boot loader's levels (the link was not proven). Do not change the supplies without
  measuring the DSP load before and after.
  **The Bluetooth work (branch `ble-v41`): with the radio on, the boot loader's levels are not enough**: the second core misread its jobs and was retired
  (`BYP` on every preset), and CPU0 read bad pointers (crashes; one a bus error at an address with bit 21 set, the
  misread pattern). Raising VDC14 and SYSVDD alone did not stop it: the radio now starts after the stock firmware's whole
  power setup (v30's: the levels, the DC-DC, VDDIO, `WLDO12_AUTO`), with the audio held for the switch-over.
- **Crash and boot diagnostics that paid off:** a `.noinit` record of the last boot (stage reached, blocks of audio
  played, load, NaN count, reset source, crash PC/RETS); a boot guard (two failed starts → safe mode, then back to the
  loader); the crash screen with `VEC`, `PC`, `RETS` looked up in `symbols.txt`.
- **Mask-ROM rescue:** a looping unit spends ~44 s between crashes in JieLi's USB boot ROM ("UBOOT", USB `4C4A:8057`,
  `WL82 UBOOT1.00`). With JieLi's `wl82loader.bin` in RAM the whole flash can be read and rewritten (chip key `0x980F`).
  The ROM cannot be damaged (`tools/uboot_rescue.py`, `docs/RESCUE.md`).

---

## 5. Audio (ALNK / I2S)

- **Driver:** JieLi's own `iis.c` from the AC79 SDK is shipped as LLVM bitcode (`cpu.a`). Our toolchain compiles it as
  is: `clang -target pi32v2 -Os -c -x ir iis.bc` (~5 KB). You provide malloc/free, gpio_* helpers and a few log symbols.
  It does not register its interrupt: call `iis_irq_handler(0)` from the ALNK0 ISR (**IRQ 11**). It sets the 44.1/48 kHz
  clock family itself (CLK_CON2 `0x10014`, ANA).
- **Sample rate (read in the driver's `iis_set_sample_rate`, not probed on hardware):** two settings.
  1. **The clock family** in CLK_CON2 `0x10014`: bit 8 for ALNK0 (bit 10 for ALNK1), **set for the 48 kHz family**
     (48 / 24 / 12 / 96 / 192 kHz...), **cleared for the 44.1 kHz family**; for the 48 kHz family the driver also sets
     bit 25 of `0x119A4` (**likely** an audio PLL output). It clears bit 9 (ALNK1: bit 11) at the end.
  2. **Dividers** in the byte at offset `0x0C` of the ALNK register block as the driver addresses it: bits [7:5] = 4 and
     [4:2] = 0 for both 44.1 and 48 kHz; [7:5] = 2 for 88.2/96, 1 for 176.4/192, 6 for 22.05/24, and [4:2] = 1 with
     [7:5] = 6 for 11.025/12. 8 kHz is refused.
  On the pedal `iis_set_sample_rate(44100)` gives 44.1 kHz (**measured**: the tuner reads notes in tune, and the NAM models
  trained at 44.1 kHz play at their pitch). **The stock firmware sets the same 44.1 kHz bits** (read: it clears bits 8..11
  of `0x10014` and writes [7:5] = 4, [4:2] = 0), and passes **fs = 44 118** to every effect and model set-up: it takes
  its real rate to be 44 118 Hz (0.04 % above 44 100; **guess**: the audio clock is not an exact 44.1 kHz multiple).
- **Callback order (read in the driver):** `iis_irq_handler` walks the channels in order and calls the output
  callback for channel 0 (TX), then the input callback for channel 1 (RX), in the same interrupt.
- **24-bit mode (read in the stock firmware, measured):** set each channel's 24-bit bit (`CON1 |= 1 << (4*ch + 2)`, the
  SDK's `data_width`). The DMA then stores one sample per 32-bit word, right-aligned (bit 23 = sign), and LEN counts
  words: `sr_points = 2 x frames`. Read input as `(int32_t)(w << 8) >> 8`. 24-bit is what the stock firmware uses (a hiss
  we first blamed on 16-bit input turned out to be the right channel's noise, below).
- **[pedal]** I2S on IIS_PORTA: MCLK PA4, SCLK PA5, LRCK PA6, output DAT0 PA7 → external DAC → SGM8905 amp (enable PA9
  high), input DAT1 PA8 from an external ADC, guitar on the **left channel only** (the right one carries steady noise
  14 dB above the left floor; mixing L+R added it). The chip's internal DAC is not used on the pedal.
- **Blocks:** OpenPocket uses 128 frames at 44.1 kHz (2.9 ms). The stock firmware uses **64-frame blocks** at the same
  rate (its coefficient code also has a 48 kHz fast path, which misled an earlier reading).
- **Real-time accounting:** time every block with TIMER4 against its budget (`frames / fs x 24 MHz`). Count blocks over
  100 % (`LATE`), blocks that reach the output clipper (`CLIP`), the peak load, and a per-effect time (v38 profiler). The
  stock firmware keeps the sum and the **maximum over 512 blocks** of its interrupt's time: the maximum is what predicts
  a crackle.

---

## 6. The FFT block / vector unit at `0x1EE0400` (measured)

- **Registers:** CON `0x1EE0400`, CADR `0x1EE0404` (address of a command), CONST `0x1EE0408`. Start a command with
  `CON = 0x149` (stock FFT jobs: `0x101`). Done = CON bit 7, cleared by setting bit 6. It runs beside the CPU.
- **Dot-product command** (layout from the AC79 SDK `apps/common/jl_math`, Apache-2.0), 8 words, 32-byte aligned:
  `[0] 0xAA1` (dot product, x y z 32-bit), `[1] x`, `[2] y`, `[3] z`, `[4] q << 24 | nloop << 12 | nlen`,
  `[5] z step << 20 | y step << 10 | x step` (bytes), `[6] 0`, `[7] 0`. It computes `nloop` dot products of `nlen` pairs,
  `z = sum(x*y) >> q`. We use it as a matrix-vector engine for the network layers (a weight row against many X rows).
- **Speed:** about **4.8 cycles per multiply-add**, about as fast as a good CPU loop. The gain comes from running it
  beside the CPU (split each layer in two halves: while the unit does one, the CPU gathers or post-processes the other).
- **Memory contention:** its inputs went into the cache-way RAM (`0x1F28000`), so it does not fight the CPUs for SRAM.
- **Check it at boot:** run it against the C result on a pattern. If it is absent, unclocked, or off by more than 1 LSB,
  do not use it (we time out after 400 000 polls).
- **FFT mode (read in the stock firmware):** a 3-word job `[config | 1, in, out]`, CADR pointed at it, `CON = 0x101`,
  wait for bit 7, clear with bit 6. It is **fixed point**: the stock code finds the block's headroom, clamps the shift to
  -24..+18, and rebuilds a float scale `2^e` for the result (block floating point). It uses the hardware **only when
  `cnum` says it runs on CPU1**, and software radix routines in RAM otherwise.
- **CORDIC** (`copex`, the SDK's `tanh_float`): studied, no faster than a rational tanh.

---

## 7. Peripherals and I/O (registers generic, pins **[pedal]**)

- **Display:** hardware SPI1 group A (PB6 CLK, PB7 DO, IOMAP_CON1 bit 4 = 0), CON = 0x4021, 20 MHz (BAUD 2), pixels by
  DMA from a double buffer of 1024 pixels (`fm1_lcd_hw.h`).
- **Button lights [pedal]:** one 74HC595 on port C: DS PC8, SHCP PC5, STCP PC6; **PC7 = output enable, low = on** (not a
  screen line, as we long assumed: with it high the lights stay dark). Byte MSB first, ~5 us per edge, sent inverted.
  Read-modify-write of PORTC from the main loop only.
- **Inputs [pedal]:** buttons PA0..PA3, encoder PC0/PC1, MASTER knob on PA10 (ADC), battery divider on PC9 (ADC);
  `0x16A10` bit 16 is read by the stock firmware beside the battery gauge (**guess**: charging).
- **USB device:** felucca `usb.c`, polled from TIMER5 at 2 kHz (10 kHz tick). USB-MIDI and SysEx (our editor
  protocol: `F0 7D 4F 50 cmd ... F7`). The M-UPGRADE update entry (`ota.c`) stages a loader at `0xE0000` and restarts
  into it.
- **Watchdog:** feed it from the main loop. A vendor call that never returns, or a cooperative scheduler stuck, shows up
  as `RESET BY: WATCHDOG` after ~8 s.

---

## 8. DSP methods that worked on this chip

Measured on the pedal or proven on the PC against a reference. The engine is `engine/px_dsp.c`, the network `engine/px_nam.c`.

1. **Fixed point for the heavy network:** NAM A2-Lite (23 dilated layers, 3 channels, 1 871 weights, ~1 730 MAC/sample).
   Activations Q20, weights Q24, 64-bit accumulators shifted by a constant: 83..100 dB SNR against float. Blocks of 32
   frames, layer by layer, each layer's ring interleaved `[time][channel]` with its first 3 frames mirrored past the end
   (4 frames read straight from any position). Hand assembly for the inner loops, **checked against C at boot** (falls
   back to C on any 1-bit difference). The first, per-frame version ran at 223 % of real time; the layer-by-layer one fits.
2. **Pick the fastest way at boot:** time CPU alone, vector unit, both, one or two cores on the first model, and keep the
   fastest (`pxn_tune`). Units differ, and so do bootloaders (320 versus 360 MHz).
3. **No per-sample branches:** one loop per effect type (v38: delay and reverb, bit-identical). Block-wise processing:
   each stage over the whole block keeps its state in registers.
4. **Oversampling with polyphase IIR half-band filters** (two all-pass branches, one multiply per section, each branch
   at the lower rate). Coefficients: stage 1 (6) 0.062889286, 0.224015205, 0.424695820, 0.616864442, 0.782113520,
   0.927573355; stage 2 (2) 0.132097462, 0.574998436. Stop band 78/66 dB, about 40 % fewer instructions than Butterworth
   biquads. The stock firmware's amp uses 2x (0.3195, 0.0958, 0.7420, 0.5567, 0.9600, 0.8695) then a 3x three-branch
   stage (0.7396, 0.3513, 0.1013) for 6x.
5. **Waveshapers without `tanh()`:** a rational curve (one division) or a **512-entry table of `tanh(i/64)`** with linear
   interpolation (the stock firmware's, RAM `0x1C0E408`). The stock firmware builds `asinh` from 256-entry sqrt and log2
   mantissa tables.
6. **16-bit delay and reverb lines** (rounded; once a tail is down to the last bits it is cut towards zero, so a loop
   cannot hold a tiny pattern for ever). The reverb tank runs at half rate (22.05 kHz) in one 16-bit ring of 32 KB. The
   stock reverbs also keep 16-bit lines.
7. **Cabinet impulse responses by uniform partitioned FFT convolution:** 64-frame partitions, a real FFT of 128 built from
   a complex FFT of 64, 1024 taps. It is exact to -124 dB against direct FIR, costs ~420 instructions/sample, and uses
   17 KB of RAM. The stock firmware does the same on the FFT block.
8. **Changes without clicks:** prepare heavy recomputation outside the interrupt, hand a flag to the audio side, apply at
   the start of the next block. Glide gains and mixes (~23 ms), fade blocks on/off, and fade the output out and in
   (~6 ms) around a full rebuild. **Never mask the audio interrupt for a long recompute** (v37 did, for up to a whole
   engine rebuild: most likely a late block, heard as a crackle, on heavy presets).
9. **The wet lanes (v39, from the stock firmware):** the delay and the reverb no longer run in series in the critical
   path. At their slot the chain sends the signal and mixes in the lane's wet output of two blocks before. The lane runs
   beside the chain (the other core). The delay's taps that are *heard* read the lag less far back; the taps *fed back*
   keep the delay time, so echo timing and repeat spacing are exact, bit for bit (`tests/engine/wet_test.c`). Mind the
   gap lesson in section 1.
10. **The audio path from RAM, the delays at half rate (v40, from the stock firmware):** the per-block code is copied to
    RAM at boot (no misses in the instruction cache the two cores share), placed within call range of the flash
    (section 2). The delay line ticks at 22.05 kHz (pairs averaged in, output interpolated, a small lift of the top):
    half the work and half the memory, echoes within 0.1 dB on guitar through a cabinet.
11. **Safety nets:** NaN/Inf at the output → silence + engine reset by the main loop. Sustained overload (> 85 % for 8
    blocks) → bypass. A soft knee at 0.8 of full scale instead of a hard clip.
12. **Test on the PC with the pedal's own code and libm:** bit-exact regression hashes over every preset and type, a
    two-core pipeline test, a vector-unit emulator that completes commands late at random, a simulated second core.
    Almost every pedal-only bug we had would have shown there.

---

## 9. What M-VAVE's stock firmware does (BlackBox_021, read; details in `docs/STOCK-FIRMWARE.md`)

- **OS:** FreeRTOS SMP from JieLi's SDK. Tasks include `app_core`, `sys_event`, `systimer`, `btctrler`, `btstack`,
  `tuner_task`, `bt_sbc_dec`, `USB-Audio`, and **`wet_audio` (priority 24, created with a 384-word stack)**.
- **Audio interrupt:** `request_irq(11, prio 4, 0x202A230, CPU0)`. Per 64-frame block it copies ALNK0, sign-extends with
  an input shift, keeps peaks, runs the **whole dry chain inside the interrupt** (float, two planar channels of 64),
  mixes in USB playback (a 1 024-frame packed 24-bit ring with a linear-interpolation resampler) and A2DP (a 16-bit ring),
  converts back with round-to-even, and times itself.
- **Chain runner:** six slots, each with a category, on flag, instance, descriptor and six 16-bit parameters. A slot
  switched on/off is crossfaded over 320 samples. **Delay and reverb are not run in the chain:** at their slot the
  interrupt outputs `dry x gain + the lane's wet output of the previous block` and gives the lane a mono `(L+R)/2` copy.
  It then posts a semaphore to `wet_audio`, which processes the lane, swaps the double buffer, and times itself. That is
  where our v39 came from.
- **Effects:** module descriptors `{name, params {count, names, defaults, min, max}, funcs {init, set, process, ...}}`.
  Six categories (gates/dynamics/drives 10, one AMP descriptor for 20 amps, modulation 7, delay 5, reverb 6, one CAB
  descriptor for 20 cabinets). Delays and reverbs run at **half rate** (22 059 Hz) behind a 2:1 polyphase
  decimator/interpolator.
  Modulation runs at full rate, stereo, with an equal-sum Mix law. **The drives** (six MW models) clip at 6x the rate
  (264.7 kHz, polyphase all-pass half-bands 2x then 3x). Cabinets are FFT convolution on the FFT block.
- **Code placement:** every per-sample routine in RAM. Per-type effect code copied into one of four RAM slots when chosen
  (`f_0201c0a8`: 20 blocks; reverbs, delays, modulations, amp; the copy is checked against the flash), so the per-sample
  loop has no type test.
- **The amp has no circuit model: all 20 amps are model files in the flash** (`AM2`/`AM3`: a 256-tap FIR, a 16-unit GRU
  without biases, a 256-tap FIR; `AM4`/`AM5`: 512 taps, the GRU with biases; GRU weights in Q21 fixed point), behind a
  soft knee at 0.5 and before a 3-band tone stack whose frequencies and ranges come from the model's header.
- **Its NAM type is A2-Lite** (read: the 23-layer kernel/dilation table is in the firmware, 1 871 weights), in fixed
  point from a 7 KB RAM overlay (A). The `AM` models' GRU kernels are a second overlay (B) in the same window, and
  the GRU's three 16 x 16 matrix-vector products a sample run **on the FFT block as a vector unit**, as OpenPocket's NAM
  does (read, `STOCK-FIRMWARE.md` 11.4).
- **Its cabinets:** 20 IRs of 1 024 taps (files, signed 24-bit), 64-frame partitions.
- **Keys (read):** scanned every 20 ms; three footswitches on one ADC ladder (channel 9, combinations decoded,
  60 ms debounce); long press 740 ms, double press within 240 ms, repeat every 140 ms.
- **USB (read):** VID 0x3654 / PID 0x7756, USB 1.1; UAC1 playback (adaptive) and record (asynchronous), 2 ch 24-bit
  44.1 kHz only; a MIDI Streaming interface.
- **AI gate (read):** it measures the mean and variance of the zero-crossing period and treats a steady period of
  100, 120, 200 or 240 Hz (+-1 %, rectified 50/60 Hz mains hum) as noise, so hum does not hold the gate open.
- **020 to 021:** the wet_audio lanes, the MW drives, the CC map, the pd40j storage and most settings are new in 021.
- **Dynamics (read):** every dynamics model and every MW drive has the same adaptive noise gate built in (default
  threshold -60 dB, tracks the noise floor); Boost is 0..+10.8 dB (quadratic knob); Compress (read in full): detector
  high-passed at ~0.7..3.6 kHz by Attack, one envelope time 23.75..80 ms, threshold -48 dB, 30 dB soft knee, ratio
  1 + 5 (Sustain/100)^4 (up to 6:1), make-up 3..12 dB, then tanh soft clip and Level x 0.014; its tone shelf stays flat.
- **Lofi (read in full):** asinh drive (3.5 + 2.5 Grit), 9 biquads, a wow pre-delay (LFO 0.5..10 Hz), then a 16-bit
  line of 100..782 ms (quadratic Time knob).
- **Loopback (read):** OFF = the computer records the pedal alone; ON = the pedal plus the computer's own playback.
- **Hold:** every stock delay has an unused infinite-repeat hold (feedback to 1, input faded out).
- **Stock UI (read):** an LVGL-style library; RGB565 with alpha-blended spans, anti-aliased rounded corners (4 x 4
  sub-pixels), fonts of 4 bpp (20 px ASCII with kerning, 54 px run-length digits for the preset number, 38 px tuner
  notes); flush by CASET / RASET / RAMWR and SPI1 DMA of up to 3,840 bytes, 2 ms timeout with SPI reset.
- **Stock libraries (mapped):** FreeRTOS-style RTOS on both cores, a locked heap, sdfile flash file system (FAT
  linked but unused), USB device (MIDI + UAC1), Bluetooth controller + host with **only an A2DP sink** profile (no
  AVRCP, HFP, SPP) and a BLE GATT server; the SysEx `F0 22 24 35 7D F7` reboots to the boot loader's USB update mode.
- **Stock boot (read):** RAM code (67,556 B) copied from flash `0x2081880` to `0x1C00000`, `.bss` 72,532 B at
  `0x1C107E4`, no SDRAM, second RAM bank at `0x1F28000`; per-core memory protection (stacks fenced).
- **Stock RTOS (read):** FreeRTOS 10 made SMP (IDLE0/IDLE1, a software interrupt per core for switches), **100 Hz
  tick**; dlmalloc 2.8 heap with 32-byte chunks and a cross-core spin lock (a corrupt heap hangs).
- **Stock Bluetooth (read):** SSP with ECDH P-192, auto-confirmed (Just Works, no PIN), no Secure Connections;
  class of device 0x240404 (likely); tasks btctrler 19, btstack 18, btencry 16, bt_sbc_dec 27.
- **Cabinet filters (read):** Lcut = first-order high-pass 20..300 Hz, Hcut = first-order low-pass 5..18 kHz.
- **Spring (read):** a 640-tap (29 ms at 22,059 Hz) spring impulse response convolved ahead of the reverb tank.
- **Shimmer (read):** a two-tap rotating-delay pitch shifter with a 512-entry crossfade window, inside the FDN loop.
- **Battery (read):** percentage from ADC channel 8, raw 528..635 mapped linearly to 0..100 %; icon red below 21 %,
  charging on GPIO 151; at boot a supply below 3.4 V changes a power setting; hardware LVD level `VLVD=4`.
- **Uploads (read):** a model is staged in the 12 KB `usr` file (`0xBA000`) with a `P4JR` journal record (CRC-32) at
  `0xBC000`, and copied into its `pd40j` slot at boot: power-cut safe.
- **SysEx (read):** `F0 00 59 cmd len24 payload ~sum F7`, 8-bit payload. Everything is memory access through two
  objects: object 5 = storage (`pd40j` by offset, `usr`, the loaded models' headers, model names, install command),
  object 4 = live state (preset bank, current preset, settings, flags). Full map in `STOCK-FIRMWARE.md` 13.1.
- **USB record (read):** a 1,024-frame packed 24-bit ring at `0x1C18BC0`, drained by the class callback with a
  fractional resampler that follows the host clock (slows below 256 frames of fill, speeds up above 768).
- **MIDI:** Program Change loads a preset; CC 0..5 switch the six units, 20..27 are the footswitches, 28 the tuner,
  40..42 tap tempo, 48..53 choose each unit's model, 54..89 set the 36 parameters (`54 + 6 x unit + param`, 0..127 to
  0..100). The channel (or OMNI) and the input gain (-10..+10 dB) are kept in the `GLB1` log.
- **`wet_audio` is not pinned to a core** (JieLi pins a task only when its name starts with `#C0`/`#C1`).
- **Preset (92 bytes):** volume 0..100, the order (a permutation of 0..5), on[6], model[6], 6 x 6 parameters.
- **Interrupt priorities:** audio 4, radio 2, timers 1 (7 is the highest). **Every interrupt runs on CPU0** but SOFT4:
  `request_irq` forces all others there, whatever core is asked (`STOCK-FIRMWARE.md` 19.1). The radio sits below the
  audio, but its interrupt masks everything while it runs, so the audio waits for it, never the other way round.

---

## 10. Toolchain notes

- JieLi's clang for pi32v2 (`clang -target pi32v2`, LLVM 4-era) + its `ld`, `objdump`, `objcopy`. **Disassemble with
  `objdump --mcpu=r3`**, or the FPU instructions do not decode.
- Flags that matter: `-mcpu=r3 -mfprev1` (hardware float), `-fno-builtin`, `-ffunction-sections`. `-O2` for DSP, `-Os` for
  the rest. Our clang needed `-isystem <host clang>/include -ffreestanding` for `stdint.h` when used outside the SDK.
- SDK libraries are often LLVM bitcode: compile them with `-x ir` instead of reimplementing them (I2S driver, BLE
  controller, DAC).
- Avoid `uint64_t` division and modulo (no `__udivdi3`). Keep 64-bit for multiply-accumulate only.
- Engine code is portable C: it builds for the PC (tests) and as WebAssembly (`engine/wasm.sh`, the simulator's sound)
  from the same sources.

---

## 11. Bluetooth LE on this chip (measured on a BlackBox; `docs/BLE-TEST.md`)

What it took to get JieLi's controller (from the SDK's `btctrler.a` / `btstack.a` bitcode) running without their RTOS:

- **Call `__crc16_mutex_init()`** before starting the controller, or `thread_create → crc_get_32bit` waits for ever.
- **`low_power_get` must return a non-null handle**, or a vendor assertion parks the CPU in `P33_SYSTEM_RESET`
  (looks like a hang, then the watchdog fires).
- **The context switch must save r4..r15**, the registers the compiler's prologues preserve, not only r4..r9.
- **Radio silent until two fixes:** the modem's TX I/Q trims (`0x2FC10`, `0x2FC08`) must be set (neutral gains `0x80`;
  stock fills them from the Wi-Fi front-end calibration), and the stand-in for `wf_rf_common_init` must **return 0**,
  or a stray value switches on the Wi-Fi/BT coexistence arbiter (`0x14000` bit 1).
- **The link:** link `dev_hci_set_event_mask` for real (a stand-in means the host never sees LE meta events), and set
  advertising up again after each disconnection.
- **Interrupts:** BLE IRQ 45 and 29 at priority 2, below the audio interrupt (3), as the library asks and as the stock
  firmware has them. **Never above the audio:** a v41 test build with them at 4 crashed within seconds, inside the NAM
  network's hand-written routine in the audio interrupt (a misaligned access, `EMU` bit 0). The library's interrupt
  handlers mask everything for their whole run (`cli` + a spin lock), as in the stock firmware.
- **`cpu_in_irq`:** the SDK's is `icfg & 0xFF`, and the link scheduler calls `le_hw_suspend` from the interrupt, where a
  "no" takes the task's way (it waits on the baseband's busy flag). But the build that advertised and connected on a
  BlackBox always said no, and the first v41 builds that used `icfg & 0xFF` were never seen by a phone: OpenPocket says no
  again until the low byte is read on the pedal outside an interrupt.
- **Timers from the interrupt:** the link scheduler adds and removes timeouts inside the interrupt; the timer table needs
  a lock.
- **Stack guard:** the controller's task stacks in ordinary RAM trip felucca's stack-limit guard. The Bluetooth release (branch
  `ble-v41`) puts them right below the main stacks and lowers the guard window's bottom to them; the test image keeps
  them in NOINIT with the guard off (the linker script refuses any other combination).
- **Never put the controller's data or stacks in the data cache used as RAM** (v37 did): the cache controller serves it,
  the same one that feeds the two cores' instructions from the flash.
- **The audio path must run from RAM when the radio runs** (v40): the radio's code is in flash and would push flash DSP
  code out of the instruction cache both cores share. This was v37's `BYP` at 150 %.
- **Radio calibration:** read the stock VM records raw (VM 106..108 to the radio replay, VM 187 the unit's calibration).
- **Not verified:** sound under a link (the `ble-v41` branch's release: no crackle while idle, crackles while a phone sends a stream of CC), long runs (the stock firmware re-calibrates
  the radio when the temperature drifts; OpenPocket does not), bonding across a power cycle. Flash writes mask every
  interrupt while the flash is busy, the radio's too, so the library's flash code is never fetched then.

---

## 12. Open questions

- Whether CPU1's shared-RAM misreads depend on supply level, clock or temperature.
- The code-cache cost of running the DSP from flash on both cores (suspected once, never measured in isolation).
- The stock firmware's `LMT` and `BEND` trailers (the model formats themselves are read: `STOCK-FIRMWARE.md` 10.1), its
  drives' curve values, its tuner's pitch algorithm.
