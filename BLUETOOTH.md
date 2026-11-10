# Bluetooth MIDI (branch `ble-v41`, not in the v41 release)

> **Status.** This page describes the Bluetooth work kept on the branch **`ble-v41`**, not the v41 release. The v41
> release is built without Bluetooth: VinSpee's two-core work (pull request #22) uses the RAM this branch gave the radio
> (`0x01C00000..0x01C08000`, and the top of the RAM code window), and Bluetooth is set aside until it is rebuilt on top
> of it (the 27 KB of main RAM that work freed are the likely home). On the branch, a Pocket X was seen and connected
> from Android, and BLE-MIDI control worked; a phone sending a stream of Control Change (a slider) made the sound crackle.
> The findings below hold for any later Bluetooth build: the stock firmware's way of running the radio, the radio
> records lost at install on a Pocket X (VM at `0xA1000`) and the defaults now given for them (in v41's `ble_rf.c`, for
> the Bluetooth test image too).

The normal firmware can be a **Bluetooth MIDI device** named `OpenPocket`. A Mac, an iPhone or iPad, an Android phone or a
Bluetooth MIDI controller (a Chocolate Plus, for example) connects to it. Program Change and Control Change then do exactly
what they do over USB ([MIDI.md](MIDI.md): presets, blocks on and off, models, settings, the clock).

It is **off by default**: a pedal that never uses it starts no radio and pays nothing.

> **v41: Bluetooth is back in the release.** v37 had it and took it out again: with the radio on, nearly every preset went
> to `BYP` at 150 % and more. The fourteenth reading of the stock firmware
> ([STOCK-FIRMWARE.md, section 19](STOCK-FIRMWARE.md#19-fourteenth-pass-bluetooth-read-to-the-end)) showed why, and v41 is
> built the stock firmware's way. See [How it works](#how-it-works).

## Switch it on

1. Hold **FX** for 0.8 s: the **MIDI** page opens.
2. Press **PUSH** until the **BLUETOOTH** row is selected, then turn the **KNOB** to **ON**.
3. Press **HOME**. The setting is saved, and the radio starts as the page closes.

Open the MIDI page again to see what the radio is doing:

| The page says | Meaning |
| --- | --- |
| `BT: STARTING` | the radio is starting (a second or two) |
| `BT: VISIBLE AS "OPENPOCKET"` | ready: a device can connect |
| `BT: DEVICE CONNECTED` | a device is connected; the status corner of the PLAY page shows `BT` before the load (`BT C1 42%`) |
| `BT: ON WHEN YOU LEAVE` | switched on here: it starts when you press HOME |
| `BT: OFF AFTER A RESTART` | switched off here: the radio stops at the next power-on (until then, what it receives is ignored) |
| `BT: FAILED TO START` | the radio did not start this time (the pedal works as usual; the log channel says why, see below) |
| `BT: STOPPED - SET OFF, THEN ON` | the pedal restarted while the radio was starting (or after a failed boot). It waits for you: turn the row OFF, then ON again, and leave the page to try again (`BT: TRIES AGAIN WHEN YOU LEAVE`) |

The line under it, `IRQ 31 RX 0 TIMERS 12 /S`, is the radio's activity per second: its event and receive interrupts and the
library's timers. With a link at 15 ms it reads about 67 events a second; while only visible, two to ten.

The setting is kept with the MIDI settings (the preset store's settings byte, bit 7) and survives firmware updates.

## Connect

- **Mac:** *Audio MIDI Setup* → *Window* → *Show MIDI Studio* → the Bluetooth button → `OpenPocket` → *Connect*. It then
  appears as a MIDI device in every app.
- **The phone's own Bluetooth settings never list it.** That is normal for every Bluetooth MIDI device. To check that the
  pedal is visible, *nRF Connect* or *LightBlue* (both free) show `OpenPocket` in their scan.
- **iPhone / iPad:** in an app that has a Bluetooth MIDI page (GarageBand: *Settings* → *Advanced* → *Bluetooth MIDI
  Devices*; many MIDI apps have the same page).
- **Android:** an app such as *MIDI BLE Connect*, then any MIDI app.
- **Chocolate Plus:** switch it to **H**; it connects by itself a few seconds after the pedal starts. Its footswitches are set
  up in [tools/chocolate/README.md](../tools/chocolate/README.md). To reach the pedal from a Mac instead, put the Chocolate
  on **O**.

No PIN is asked. One device at a time: while one is connected, the pedal is not visible to others.

For the first 30 s after the radio starts, or after a device disconnects, the pedal advertises every 100 ms, so it is found
at once. After that it advertises every 500 ms, as the stock firmware does. A device that searches still finds it, within a
second.

## What it does not do (yet)

- The pedal only **receives** over Bluetooth: its own changes are sent over USB, not over Bluetooth. The stock firmware
  sends them over both.
- **Pairing is not remembered** across a power cycle. Devices that connect without pairing (the Chocolate, a Mac) do not
  notice. One that insists on its bond may need to forget `OpenPocket` and connect again.
- Every OpenPocket pedal has the same Bluetooth address: two of them in one room get in each other's way.
- Switching it off takes effect at the next power-on.
- The stock firmware's second service (JieLi's `AE40`, used by the M-VAVE app) is not offered. OpenPocket's editor uses USB.
- The stock firmware re-calibrates the radio every 3 s when the chip's temperature drifts. OpenPocket calibrates it once,
  at start-up: a very long session in a hot place may lose range.

## Please report

What is not measured yet is the sound **while a device is connected**. Please report:
- the status corner (`BT C1 nn%`) on a few presets, with a device connected and without;
- whether you hear any crackle, and whether `BYP`, `LATE` or `CUT` appear.

If the radio does not start, send a photo of the MIDI page and the output of `node tools/op.mjs log` (USB cable, pedal
running). The log's trace line ends with `HEAP n STK n CPR n`: the least heap and task stack that were ever free, in bytes,
and how many connection-parameter requests the central refused.

## How it works

### Why v37 fell to `BYP`, and what v41 does instead

v37 ran the radio while the audio code still ran from the flash. **The two cores share one instruction cache in front of
the flash**, and the radio's code (about 150 KB, also in flash) kept pushing the DSP code out of it. The second core, which
does no radio work at all, lost its time to cache misses: 70 % before the radio started, 150 % and more after.
v37 also placed the controller's task stacks in the data cache used as RAM, which the same cache controller serves.

The stock firmware has the same radio code in flash and never suffers from it, because **its whole audio path runs from
RAM**. v40 did the same for OpenPocket (`tools/ramcode.py`), and v41 builds on it:

| | v37 | stock firmware | v41 |
| --- | --- | --- | --- |
| audio code | flash | RAM | RAM (v40) |
| radio code | flash | flash | flash |
| radio data, heap, queues | RAM, cache RAM | RAM | RAM (`BTRAM`, the 32 KB v40 freed) |
| controller task stacks | cache RAM | RAM | RAM (`BTSTK`, below the main stacks) |
| radio interrupt priority | 2, below audio (3) | 2, below audio (4) | 2, below audio (3): the stock order |
| controller tasks run | every main-loop pass, then every ms | FreeRTOS tasks | only when an interrupt, a timer or a message asks |
| advertising | 100 ms | 500 ms | 100 ms for 30 s, then 500 ms |
| link interval | the central's | asks 7.5 to 11.25 ms, then 15 ms | the same requests as stock |

### The parts

- **JieLi's own Bluetooth LE controller and host** (Apache-2.0, LLVM bitcode from the AC79 SDK, `third_party/jieli-ble/`):
  the same library family the stock firmware links (section 19.2 of STOCK-FIRMWARE.md compares them function by function).
  OpenPocket's code around it:
  - `firmware/ble/ble_os.c`: a small cooperative scheduler for its tasks, its message queues, semaphores, timers and heap;
  - `firmware/ble/ble_rf.c`: the radio start-up, replayed from the stock firmware's;
  - `firmware/ble/ble_midi.c`: the GATT table, the advertising, the link's parameters;
  - `firmware/px_ble_midi.h`: the MIDI packet reader.

  What the pedal receives goes through the same `px_midi_in` as USB.
- **How a message travels**, as in the stock firmware:
  1. The radio's interrupt (IRQ 45, or 29 on reception) copies the packet out of the baseband and plans the next radio
     event. It masks all interrupts while it runs, as the library always does; this takes tens of microseconds.
  2. It posts a message to the controller's task (`btctrler`), which runs the link layer and turns the packet into an HCI
     event.
  3. The event goes to the host's task (`btstack`): L2CAP, then ATT, then the write callback, then `px_midi_ble_write`.
  4. Both tasks run in the main loop's turn (`ble_poll`), and only when something posted to them, or one of their timers
     or waits is due (`ble_poll_due`). Between two radio events, none of the library runs.
- **Interrupt priority 2, below the audio interrupt (3)**, as the library asks and as the stock firmware does: the audio
  interrupt is never interrupted by the radio. The radio's work may wait for one audio interrupt (up to 2.7 ms with a NAM
  amp), well inside its 7.5 ms and longer link intervals.
- **Found on the pedal (v41 test builds):**
  1. With the radio at priority 4 (above the audio), the pedal crashed within seconds of the radio starting, on CPU0,
     inside the NAM network's hand-written routine (`pxn_rdl6_asm`, a misaligned access, `EMU 00000001`): a pointer read
     wrong from RAM.
  2. With the radio at priority 2, every preset went to `BYP`, as in v37.
  3. With VDC14 and SYSVDD raised to the stock levels, CPU0 crashed again in the same NAM routine, this time on a bus
     error (`DBG 00100000`): an SRAM address with bit 21 set, the exact misread pattern the FM-1 projects documented
     (`0x00200000` read where RAM holds 0).

  All three fit one cause better than the priority: **the core supplies**. OpenPocket runs on the boot loader's levels
  (VDC14 1.40 V, SYSVDD 1.26 V; v33), where some units read RAM wrong under load, "both cores, or the radio" (the FM-1
  projects found it with WiFi, `fm1_sys.h`). The stock firmware always runs at VDC14 1.60 V and SYSVDD 1.38 V, on the DC-DC. A bad read
  on the second core makes it refuse its job: it is retired, every preset runs on one core, and the load doubles (`BYP`).
  A bad read on CPU0 is a crash.
- **So the radio now starts only after the stock firmware's whole power setup** (`ble_supplies` in
  `firmware/openpocket.c`), as M-VAVE's start-up does it (AUDIO-SDK.md, v30):
  - VDC14 to 7 and SYSVDD to 15, one level per 100 µs, with the core supply's strong drive (`CON9` bit 4) and `CON6`
    bit 3;
  - the 1.3 V supply moved from its linear regulator onto the DC-DC, tuned as stock (`CON7` 0xA5, `CON8` 0x96);
  - VDDIO `0x74`, and the radio's own 1.2 V regulator on automatic (`WLDO12_AUTO`).

  The audio is held (muted, its interrupt masked, the second core between jobs) for the regulator's switch-over. A pedal
  with Bluetooth off keeps v33's supplies. v30 had this setup for everything and a Pocket X then measured slower and
  crackled; that was never proven to come from the supplies (the DSP still ran from flash then), but listen for it.
- The radio's interrupts stay at priority 2, below the audio, as in the stock firmware (`OP_BLE_IRQ_PRIO`).
- **When the second core stops**, the MIDI page says so in red (`CORE 2 OFF: MISREAD`, `LATE`, `FAULT`). It then stays
  off **until the pedal is switched off and on again** (a restart is not enough).
- **`cpu_in_irq` says no, always**, as in the test image that advertised and connected on a BlackBox. The first v41 builds
  answered as the SDK does (`icfg & 0xFF`), and no phone saw them: what that byte holds outside an interrupt after this
  pedal's boot loader was never checked. Back to the proven answer until the radio is seen again.
- **Seen on the pedal (v41, heap fixed):** the MIDI page said `VISIBLE AS "OPENPOCKET"` with `IRQ 2 RX 0 TIMERS 0 /S`,
  and nRF Connect on Android did not list it. Two events a second is the 500 ms advertising: the baseband runs. `RX 0`
  means no scanner's request was ever heard. The radio ran but nothing reached the air (or came back), as in the test
  image's fourth flash before its transmit trims were fixed.
- **Then, with the long line below:** `i361 e0 r0 t0 s0 V 0 0 0 CON 6001 IEN 101 IPND 0 STAT 0 IC45 5 IC29 5 BEN 10001`.
  `V 0 0 0`: **no stored radio record found**. The test BlackBox that a Mac saw had all four (`V F BD B0B`). Without them
  the replay wrote zeros into the crystal trim (VM 106: the oscillator, so every channel, off its frequency) and the
  radio trims (VM 107). The cause, found in the user's own backup (`fm1-1.bin`): that Pocket X keeps its VM at `0xA1000`,
  not `0xA3000`. The scan started too high, and in any case the area lies inside OpenPocket's application area
  (`[0x4000, 0xA3000)`), so the first OpenPocket install overwrote it. The backup still has it: VM 106 `0B 0B` (the test
  BlackBox's too), 107 `01 07 04 07 0B 01 07`, 108 zeros, 187 the unit's calibration.
  - The scan now starts at `0xA1000`.
  - A missing 106 or 107 gets those values (`ble_rf.c`); a missing 187 is measured, as before. The `V` mask says which
    (BLE-TEST.md).
  - **Result on the pedal: seen and connected.** nRF Connect on Android listed `OpenPocket` and connected:
    `i2291 e23 r2158 t0 s0 V 430 0 B0B`, `BT: DEVICE CONNECTED`. `V 430`: 106 and 107 defaulted, 187 measured, the
    measurement's scan and trim both succeeded. `r` rising: the pedal hears the phone. The first Bluetooth connection to
    an OpenPocket Pocket X. Then, through Android's MIDI (MIDI BLE Connect) and a controller app, a slider on the phone set the pedal's
    volume: BLE-MIDI received and applied.
- **The radio's long line on the MIDI page:** put the cursor on `BLUETOOTH` (PUSH) once the radio has started. The three
  lines around `BT: ...` then show the line the test image scrolled ([BLE-TEST.md](BLE-TEST.md#the-advertising-line-proof-counters)):
  `i e r t s` counters since the start, `V` the stored radio records found (bit mask, area, crystal trim), the baseband
  registers `CON IEN IPND STAT`, the interrupt lines `IC45 IC29`, the block enable `BEN`. While it is starting or has
  failed, the step and stage instead.
- **Timers** added from the interrupt (the link scheduler's timeouts) are handled under a lock (`ble_os.c`).

### Memory

| What | Where | Size |
| --- | --- | --- |
| the library's zeroed data (its named `.*_bss` and its objects' plain `.bss`, renamed `.btram.lib` by `build.sh`), the tasks' queues, and the heap: all the rest, 19.1 KB (one link and the audio driver take about 17 KB of it; the first v41 build had 16 KB, too little, and the radio never started) | `BTRAM`, `0x01C00000..0x01C08000` | 32 KB |
| the three task stacks (4 KB each) | `BTSTK`, `0x01C71000..0x01C74000`, right below the main stacks | 12 KB |
| the library's initialised data | `RAM` (`.data`) | 2.3 KB |
| the library's code | flash, right after OpenPocket's code, within the RAM code's call range (`_ecode`) | 154 KB |
| the library's constants and strings | flash, after the read-only data | 60 KB |

The RAM code window (`RAMTEXT`) went from 88 KB to 76 KB to make room for `BTSTK`; the audio path uses 74 KB of it. The data
cache used as RAM is all the NAM vector unit's again, as before v37. The build prints `RAM`, `BTRAM` and `RAM code`.

### Safety

- **The stack guard** stays on. Before the controller's tasks first run, the firmware lowers the bottom of the guard's
  window to `BTSTK` (`ble_guard_window` in `firmware/openpocket.c`). `app.ld` refuses the link if the stacks are anywhere
  else.
- **Start-up safety.** A reset while the radio is starting (a hang the watchdog ends, a crash) is noted in memory that
  survives a reset. The following boots leave the radio off until a power cycle, or until you turn the row off and on again,
  so a radio that will not start on some unit costs one restart, never a loop. After any failed boot (the
  `LAST BOOT FAILED` screen), the next boot waits for you the same way.
- **The radio's start-up runs on the main stack.** Its crystal ramp keeps data on the caller's stack, and in v37 it crashed
  on a task stack in the cache RAM.
- **Flash writes** (saving a preset) mask every interrupt while the flash is busy, the radio's too. A link survives the
  pause: the supervision timeout is 4 s.

### Building

- The log channel (`node tools/op.mjs log`, editor command 23 `DIAG`) answers in every firmware with Bluetooth built in: the
  boot, the last boot, the radio's state, the link and the last events, as described in [BLE-TEST.md](BLE-TEST.md).
- `OP_NOBLE=1 sh firmware/build.sh` builds the firmware without Bluetooth. `make ble` builds the test image: the same
  firmware with the radio always on and its state scrolling in the status area.
- **Flash is nearly full:** with Bluetooth, the image uses about 577 of the 581 KB the application area allows.
