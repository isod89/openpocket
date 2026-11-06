# Bluetooth test image (BLE MIDI peripheral)

An optional build of OpenPocket that turns the pedal's Bluetooth radio on and makes it a connectable BLE MIDI peripheral named
**`OpenPocket`**. A Mac, a phone or a Chocolate Plus connects to it, and Program Change and Control Change then do what they do over
USB ([MIDI.md](MIDI.md)). USB MIDI is not changed.

The normal firmware and the installer's packages in `docs/firmware/` are built exactly as before: `make build` gives the
same bytes with or without this feature in the tree. The test image is its own build target, `make ble`.

> **Status: runs on a real BlackBox.** A Mac connected, found the service, and a Control Change switched a block and a Program Change
> loaded a preset, with the state read back over USB. A Chocolate Plus on H paired by itself and its footswitches A to D changed
> DRIVE, MOD, DELAY and REVERB. The audio under a link is not measured: read
> [What is not verified](#what-is-not-verified) before you flash it.

**First hardware run (fixed).** The first image crashed on every boot, before any advertisement: the hardware stack-limit
guard (`fm1_guard.h`, EMU bit 3) faults any stack pointer outside `[_ustack_lo, _sstack_top]`, and the controller's task
stacks (`ble_os.c`) are in ordinary RAM below that window. The test image is now built with that one guard off
(`OP_GUARD_STACK=0` in `firmware/build.sh`); the write, bus and code-range guards stay on. The normal firmware keeps all four.
`firmware/app.ld` now refuses to link the image if the stack guard is on while the task stacks lie outside the window, so the
mistake cannot come back unnoticed (`OP_BLE=1 OP_GUARD_STACK=1 sh firmware/build.sh` shows the refusal).

## Build

```
make ble          # firmware/openpocket-ble.bin and build/ble/PocketX_936.fwsc, BlackBox_936.fwsc, manifest.json
```

(`node tools/op.mjs ble` is the same; `OP_BLE=1 sh firmware/build.sh` inside the container.) It prints the SHA-256 of the image and the two packages. Build outputs
are `firmware/build-ble/` and `build/ble/`; `make clean` removes them.

## Flash it

This is the pedal's normal update path, with the test packages instead of the release ones. Your pedal must already run
OpenPocket (the stock-firmware scan was skipped on purpose).

1. `make serve-ble` builds the test packages if they are missing and opens the offline installer on
   `http://localhost:8765/` with `build/ble/` as its package folder. (Chrome or Edge, USB cable to the pedal.)
2. **Connect**, pick the pedal, **Install**, as for any update. The pedal restarts into the test image; its start-up
   screen reads `v36 BLE TEST`.
3. To go back to the normal firmware: run `make serve` (the release packages) and **Install** again, or use the
   **Back to the official firmware** step. The SD-card recovery (`make appimage`) works as before.

The status text at the bottom right of the screen shows the Bluetooth state: `STARTING` with the stage and the pedal's
usual `C1 42%`, then `ADVERTISING` (the proof counters, scrolling) or `FAILED` (the failure, scrolling, with its short
code first so that it is always on screen). See [Reading the status text](#reading-the-status-text).

## What you should see

Open a scanner (nRF Connect, LightBlue, or macOS *Audio MIDI Setup*; use active scanning, which phones do by default):

| In the scanner | Value |
| --- | --- |
| Name | `OpenPocket` (in the scan response) |
| Service UUID | BLE-MIDI `03B80E5A-EDE8-4B33-A751-6CE34EC4C700` (in the advertising data) |
| Type | connectable undirected (`ADV_IND`), 100 ms interval, channels 37, 38, 39 |
| Address | `02:4F:50:42:4C:54`, a fixed locally administered address set by this image (not the stock firmware's) |

The pedal has one link and stops advertising while it is in use, so a scanner sees nothing then. A Chocolate Plus with its switch on
H connects by itself within seconds of the pedal starting. To reach the pedal from a Mac, put the Chocolate on O.
`tools/chocolate/README.md` sets its footswitches up to switch the effects.

If the screen reads `FAILED`, or nothing appears within a few seconds, please report the screen text, the pedal model,
what the scanner shows and whether the pedal still works as a normal OpenPocket (audio, screen, USB MIDI).

## The link

| Part | What it is |
| --- | --- |
| GATT | GAP (`0x1800`, device name), GATT (`0x1801`), and the MIDI service with its characteristic `7772E5DB-3868-4112-A1A9-F2669D106BF3`: read (empty), write without response, notify |
| MIDI in | `firmware/px_ble_midi.h` reads the header, timestamps, running status and several messages in one write, and ignores the timestamps. SysEx and system common messages are skipped |
| MIDI out | Nothing yet: notify can be enabled, but the pedal sends nothing |
| Pairing | Just Works with bonding, accepted when the central asks. The pedal never asks, and the characteristic does not need it. Keys stay in RAM, so a bond ends at a power cycle |
| Connection | The central's parameters are kept. The pedal does not ask for others: the Chocolate's 60 ms, latency 0, 4 s timeout held |

The log channel (`node tools/op.mjs log`) adds two lines, and the status area scrolls the first one while connected:

`link       LINK 1 n2 A A1B2C3D4E5F6 I48 L0 T400 W7 P1 D0 LAST 2 24 127 STUB 3 low_power_request`

| Field | Meaning |
| --- | --- |
| `LINK` | 1 while connected |
| `n` | links so far, counting ones that failed to establish |
| `A` | the central's address |
| `I` `L` `T` | interval (1.25 ms units), latency, timeout (10 ms units) |
| `W` | writes to the MIDI characteristic |
| `P` | Just Works pairings accepted |
| `D` | the last disconnection reason in hex: `13` the central closed it, `8` timeout, `3E` the link never established |
| `LAST` | the last message: type (1 Program Change, 2 Control Change), data 1, data 2 |
| `STUB` | calls of the stand-ins (`ble_stubs.c`), and the last one's name |

`trace      EV 43E0100 4050013 ST name name` lists the last host events (packet type, event code, subevent or status, status; without Number Of Completed Packets) and the stand-ins called. While a link is up, `i` in the `ADV` line counts connection events, so about 16.7 a second means a 60 ms link.

## Reading the status text

| Text | Meaning |
| --- | --- |
| `STARTING n` | the last stage reached: 1 task created, 2 controller init called, 3 radio front end replay entered, 4 waiting for the controller task, 5 setting the advertisement, 6 enabling it, 7 the replay returned, 8 the baseband asked for its interrupts. The markers are set by two tasks, so 3 and 7 can appear after 4; the number shown is the last one set, not the highest |
| `FAILED s c` | step `s` failed with return value `c`. The status area is only 11 characters wide, so the whole line scrolls like a marquee (2 characters a second, resting 2 s on the start, then looping): `FAILED 2 -7  step btctrler_task_init stage init`. The short code is first, so it can be read before the scroll starts. The usual `C1 42%` is not shown while it has failed. |
| `ADVERTISING` | the controller accepted the command to advertise; it does not mean a packet was sent. The status shows the proof counters instead, as a marquee (below) |

Failing steps: `1` `task_create` failed (`c` is -1); `2` `btctrler_task_init` returned `c` (not 0); `3`
`btctrler_task_ready` returned `c` (0: the controller task did not answer); `4` `ll_hci_adv_enable` returned `c` (not
0); `5` still `STARTING` after 5 s, and `c` is the stage it stalled in (the list above). At a timeout the line ends with `wait XXXXXXXX`: the address of the semaphore or mutex the controller task is blocked on (look it up in `firmware/build-ble/symbols.txt`; a value below 0x10 is a task state instead). Example: `FAILED 5 3` only means the last marker passed was the entry of the radio replay; it does not say the replay hung. (In the second hardware run it was a deadlock after the radio start-up: see "Found on hardware" below.) The codes and the text come from `firmware/ble/ble_status.h`, checked on the PC by
`tests/test_ble_status.c`. Please report the whole `FAILED s c` text.

### The advertising line (proof counters)

The fourth flash showed `ADVERTISING` but neither a Mac nor a phone scanner saw the pedal, so while advertising the status area scrolls a line of counters and registers, with the numbers first: `ADV i12 e5 r0 t340 s0 CON 2001 IEN 101 IPND 0 STAT 80 IC45 3 IC29 3 BEN 1`.

| Field | Meaning | What it tells |
| --- | --- | --- |
| `i` | baseband event interrupts (IRQ 45), counted by `ble_isr.S` before the library's own handler runs | above 0 and rising: the radio runs and events happen. 0: no event ever reaches the CPU |
| `r` | receive interrupts (IRQ 29) | above 0: a scanner's request was heard, so the antenna works both ways |
| `e` | HCI packets the controller returned (`hci_packet_handler`) | any controller answer |
| `t` | library software timers that fired | the controller's tick source runs |
| `s` | calls of the slot-timer stand-ins (`slot_timer_*`) | above 0: the link scheduler asked for a timer this image does not have (see below) |
| `CON IEN IPND STAT` | baseband registers `0x28000`, `0x28028`, `0x28030`, `0x28038` in hex | enable, interrupt enables, pending, busy flags |
| `IC45 IC29` | the interrupt configuration of the two lines (bit 0 enabled, bits 3:1 priority) | 0: the line is off |
| `BEN` | BLE block enable `0x2FC80` | bit 0 clear: the block is off |

Reading the next flash: `i` and `t` rising means the radio runs and the cause is on the air side (power, trims, address). `i` 0 with `IC45` 0 means the interrupt was never enabled; `i` 0 with `IC45` 3 and `IEN` 0 means the baseband never enabled its event; `t` 0 means the library's timers never fire.

**Checked against the Core Specification** (`firmware/ble/ble_adv.h`, tested on the PC): the advertising data is two whole AD structures (flags 0x06, the complete 128-bit UUID list, little endian), 21 of 31 bytes; the scan response is the complete local name, 21 bytes; the PDU is ADV_SCAN_IND at 100 ms on channels 37, 38 and 39 with filter policy 0. One fault was found and fixed: the controller's public address was never set (the library reads it from a stored item this image does not have), so it was all zeros, which is not a valid device address. It is now `02:4F:50:42:4C:54` (locally administered, unicast; not an IEEE-assigned address). The library's own-address type is fixed at public in its HCI layer, so a random static address cannot be used from here.

**Open suspicion, not a fix:** the link scheduler (`link_task.c`) arms its timer through `slot_timer_set` and its siblings, which are among the empty stand-ins in `ble_stubs.c` (the library's slot timer is not part of the linked archive). Whether a plain advertising event depends on them is not known. `s` counts the calls: if it is above 0 the next step is a real slot timer.

## Reading the pedal from a computer

`node tools/op.mjs log` reads all of this over USB MIDI, so nobody has to read the scrolling text: it prints each field with its meaning and a verdict. It needs `npm ci --prefix tools` once.

```
node tools/op.mjs log --wait 40 --scan 30    # after a flash: wait for the boot, read, scan from this Mac, read, judge
node tools/op.mjs log --follow --json        # keep reading, one JSON object per reading
```

The request is `DIAG` ([EDITOR-PROTOCOL.md](EDITOR-PROTOCOL.md)), which only this image answers, so the normal firmware is byte for byte what it was. It reads memory and the same registers as the status text, and writes nothing. Its three lines are:

| Line | Example | Meaning |
| --- | --- | --- |
| this boot | `UP 137120 SAFE 0 STAGE 8 RST 1 GUARD 0` | the pedal's clock in ms, which wraps every 179 s, so it is the uptime only in the first 179 s; safe mode; boot stage (8 = OK); reset source bits (`POWER-ON`, `VDDIO LOW`, `WATCHDOG`, `VCM`, `LONG PRESS`, `1.2V`, `SOFT`, from bit 0); failed boots counted |
| last boot | `LAST OK`, or `LAST FAILED STAGE 7 BLOCKS 9 LOAD 42 PEAK 61 NAN 0 CPU1 1 LOOPS 0 CRASH VEC 3F PC 2000DC8 RETS 2001234 DBG 0 EMU 0 UP 4100` | the fields of the `LAST BOOT FAILED` screen; `CRASH ...` only after a crash |
| status | `ADV i12 e5 ...`, `FAILED 5 3  step timeout ...`, `STARTING 4`, `OFF` | the text the status area scrolls, from the same function |

It cannot read a pedal that is not running its main loop. While the red crash screen or the 12 s `LAST BOOT FAILED` report is up, USB is down; the next boot gives the same report through this channel. A pedal stuck before USB service stays silent: only the screen shows what it is doing. With firmware that has no `DIAG`, the command gives the version and the DSP load and says what is missing. Exit codes: 0 pass or running, 1 fail, 3 undecided. `RUNNING` means the radio events rise; only a sighting by the scan is `PASS`.

## Found on hardware

The second flash showed `FAILED 5 3`. The cause, found by reading the vendor code against the linked image: the controller's link-layer start-up calls `thread_create`, which calls `crc_get_32bit`, which waits for ever on a lock that the vendor function `__crc16_mutex_init()` creates and the SDK's own start-up calls. This image never called it, so the lock stayed "taken" and the controller task slept for ever. `ble_test_start` now calls it. Two more defects were fixed in the same change, because the first one would have exposed them: `ll_hci_adv_set_params` reads six bytes from its direct-address argument (it was NULL, which the bus guard is documented to fault on; now a zero array), and the context switch saved `r4`-`r9` while the compiler preserves `r4`-`r15` (now all of them, the list the compiler's own prologues use). None of the three has run on a pedal yet.

The third flash froze: the screen was drawn but the controls were dead, and the pedal reset by watchdog after about 8 s (`LAST BOOT FAILED`, `RESET BY: WATCHDOG`, `LOOPS 0`). The scheduler is cooperative, so a vendor call that never returns holds the whole main loop. The scout report (data/openpocket-ble-hang-diagnosis/report.md) found two such places on the advertising path: a vendor assertion that fails because the empty stand-in for `low_power_get` returned a null handle (a failed assertion parks in `P33_SYSTEM_RESET`, an endless loop), and, earlier, an unbounded wait on baseband status bit 1 (`0x28038`) that the repository's own baseband header says can stay set. `low_power_get` now returns a non-null handle. The second wait is untested; if it is the one, the crash screen below names it.

**Stall watch.** The 10 kHz timer interrupt counts while a `ble_poll()` runs. After 4 s it makes the next tick take the fatal path, so the pedal shows the normal crash screen with `VEC 3F` and the `PC` and `RETS` of the code that did not return (look them up in `firmware/build-ble/symbols.txt`; `PC 02000DC8` is a vendor assertion and `RETS` names it; a `PC` inside `ble_hw_disable` is the status-bit wait). It cannot fire while interrupts are off, and it watches only `ble_poll()`; the radio replay is excluded because it is long and its waits are bounded.

**After a failed boot Bluetooth stays off.** The next boot shows `LAST BOOT FAILED` and runs the normal firmware (status `OFF SAFE nn%`), so the boot guard never counts two failures in a row and the pedal does not fall back to the loader. The flasher works in that state. The boot after that one tries Bluetooth again.

## How it works

```
openpocket.c (OP_BLE_TEST)  ble_test_start() after the screen is up, ble_test_poll() in the main loop
firmware/ble/ble_test.c     a task that starts the controller and the host
firmware/ble/ble_midi.c     the GATT table, the host's callbacks, pairing, advertising
firmware/px_ble_midi.h      a BLE MIDI write -> USB-MIDI event packets (tests/midisim.c)
firmware/ble/ble_os.c       the operating-system services the library calls (below)
firmware/ble/ble_rf.c       the Wi-Fi front-end half of the radio start-up (below)
firmware/ble/ble_cfg.c      configuration: LE only, advertising, one slave link, a GATT server
firmware/ble/ble_stubs.c    empty stand-ins for the parts that are not built
third_party/jieli-ble/      JieLi's controller, as published (Apache-2.0): see its README.md
```

* **The controller is JieLi's**, from the Apache-2.0 SDK libraries (`btctrler.a`, 33 of its objects plus 3 helpers from
  `cpu.a`): HCI front end, link layer, advertising, the LE radio driver. Only the objects the image reaches are linked.
  The LE part of the host stack (`btstack.a`) runs as its own task; the classic profiles are stand-ins.
  The OpenPocket code around it is small and separate from the vendor objects (`firmware/ble/`), which are kept
  unmodified under `third_party/jieli-ble/` with the licence text and the source URL.
* **No RTOS.** The library expects tasks, queues, semaphores and timers. `ble_os.c` provides a cooperative scheduler:
  the controller's one task runs from `ble_poll()` in the main loop, switches only where it waits (queue, semaphore,
  delay), and is woken by the two BLE interrupts (IRQ 45 and 29, priority 2: the numbers and priority the stock firmware
  uses). The heap is a 24 KB static pool; each task has a 4 KB stack. Against the normal image it needs about 44 KB more RAM (of which the heap
  and stacks are 36 KB) and 131 KB more flash; the link fits with about 15 KB of the 96 KB RAM region free.
* **Radio start-up.** The library's `bt_rf_init` calls the Wi-Fi library's `wf_rf_common_init` first. Linking that would
  pull in the Wi-Fi MAC/SDIO stack, so `ble_rf.c` runs the already committed capture of the stock start-up instead
  (`firmware/bsp/chip/ble_rf_tables_v15.h`, run by `fm1_ble_rf.h`), with this unit's trims measured by `fm1_rf_cal.h`.
  The BLE baseband itself is initialised by the library (`le_hw_init`), which makes the same register changes as the stock
  firmware's baseband start-up in the analysis (bits 13 and 14 set, bit 4 cleared, IRQs 45 and 29 at priority 2).
* **Nothing is saved.** The library asks for stored items (`syscfg_read`): none are returned. It tries to store calibration
  results (`syscfg_write`): the call is accepted and ignored. The VM (`0xA5000`) and BTIF (`0xBE000`) areas are not read
  and not written by this image.

## What is not verified

On a real pedal the radio, the link, the GATT table, pairing with the Chocolate and the MIDI path have run. These have not:

1. **Sound under a link.** No one has listened for crackle or measured the DSP load with a link up, at the Chocolate's 60 ms or a Mac's 30 ms.
2. **Long runs.** The longest link so far is minutes. The stock firmware's 3 s radio maintenance timer is not here, so a link may degrade as the pedal warms.
3. **Reconnection after a power cycle.** Keys are in RAM, so a central that keeps its bond finds a pedal that has forgotten it.
4. **Failed first connections.** A Mac's first attempts can end with reason `3E` before one holds; it retries by itself. The cause is not known.
5. **Flash writes while Bluetooth runs.** The library's interrupt code runs from flash (execute in place). Saving a preset during a link could fetch code while the flash is busy. Do not save presets while testing.
6. **Regulation.** The pedal was certified with the stock firmware. This image uses the standard channels, the 1M PHY and the library's own power table; it is a hobby experiment, not a product.

## Recovery

If the image does not start, the existing safety net applies: two failed boots go back to the update loader
(`UBOOT`), and the web installer, the SD-card recovery (`make appimage`) and the official firmware's own updater all work as
before. The image does not touch the flash outside the application area.

## Found on hardware: the radio was silent for two reasons (fixed)

The fifth flash showed `ADVERTISING` and event interrupts at the advertising rate, but no scanner saw the pedal. The cause was in our glue, found by comparing the stock BlackBox_016 start-up with this image:

1. The library writes the modem's transmit I/Q trims (`keep_BT_CMPUK1/2/3`, `keep_BT_OFSI`, `keep_BT_OFSQ`, which stock fills from the Wi-Fi front-end calibration) into `0x2FC10` and `0x2FC08`. Here nothing set them, so the I and Q gains were zero. `ble_rf.c` now sets the neutral values (`0x80` for the two gains).
2. `wf_rf_common_init` was `void`, so the library read a stray register value as its result and switched on the Wi-Fi/Bluetooth coexistence arbiter (`0x14000` bit 1). It now returns 0, as stock does on a first start.

After the fix a Mac scan saw `OpenPocket-BLE-TEST` with the BLE-MIDI service UUID (206 sightings in 60 s, RSSI -51 to -36 dBm, not connectable) and the scan-request counter `r` rose. The stored radio records are also read now: the VM is plain flash (the XIP window decrypts it, so it must be read raw, `ble_vm.h`), VM 106 to 108 go to the replay, and VM 187 (the unit's calibration) replaces the measured one. The status line shows them as `V <mask> <area/4096> <first two bytes of VM 106>` (`V F BD B0B` on the test pedal). `bt_max_pwr_set(10, 5, 8, 9)` is called after the controller's own init. Which of these beyond the two fixes matters was not isolated. `t` and `e` stay 0 by design here (no software timer on the advertising path, no host packet handler), so they are not health signs.

## Found on hardware: the link

Connectable advertising needed the real link-layer members (`RF_ble5`, `multilink_schedule`, `ll_data_extension`, `ll_multiple_phys`) in place of their stand-ins. Two more faults showed up on the pedal:

1. `dev_hci_set_event_mask` was a stand-in, so the host never received LE meta events: a central connected and the pedal looked idle, with `i` at 16.7 a second and no advertisement on the air. The real one is linked now.
2. After a disconnection the advertisement must be set up again, as the SDK does. Enabling it alone left the pedal nearly silent.
