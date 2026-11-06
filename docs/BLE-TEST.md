# Bluetooth test image (advertise only)

An optional build of OpenPocket that turns the pedal's Bluetooth radio on and sends **one advertisement**, so the name
**`OpenPocket-BLE-TEST`** shows up in a phone or Mac Bluetooth scanner. It is stage 2 of the Bluetooth plan: the radio comes up, nothing
more. No connection is ever accepted, there is no pairing, no GATT table and no MIDI over Bluetooth. USB MIDI is not
changed.

The normal firmware and the installer's packages in `docs/firmware/` are built exactly as before: `make build` gives the
same bytes with or without this feature in the tree. The test image is its own build target, `make ble`.

> **Status: built and linked. The first flash on a pedal crash-looped, the second stalled at `FAILED 5 3` and the third
> froze in the first `ble_poll`; those faults are fixed. The fourth flash reached `ADVERTISING`, but neither a Mac nor a phone scanner saw the pedal. This build adds proof counters and a valid controller address; it has not been run on a pedal yet, and the scanner check is still pending.** There is no emulator of the radio, so everything below about what the pedal does is the plan, not an
> observation. Read [What is not verified](#what-is-not-verified) before you flash it.

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

Open a scanner (nRF Connect, LightBlue, or macOS *Bluetooth Explorer* / *Audio MIDI Setup* are all fine; use active
scanning, which phones do by default):

| In the scanner | Value |
| --- | --- |
| Name | `OpenPocket-BLE-TEST` (in the scan response) |
| Service UUID | BLE-MIDI `03B80E5A-EDE8-4B33-A751-6CE34EC4C700` (in the advertising data) |
| Type | scannable, not connectable (`ADV_SCAN_IND`), 100 ms interval, channels 37, 38, 39 |
| Address | `02:4F:50:42:4C:54`, a fixed locally administered address set by this image (not the stock firmware's) |

The name appearing in a scanner is the whole success test. Connecting to it will fail by design.

If the screen reads `FAILED`, or nothing appears within a few seconds, please report the screen text, the pedal model,
what the scanner shows and whether the pedal still works as a normal OpenPocket (audio, screen, USB MIDI).

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

## Found on hardware

The second flash showed `FAILED 5 3`. The cause, found by reading the vendor code against the linked image: the controller's link-layer start-up calls `thread_create`, which calls `crc_get_32bit`, which waits for ever on a lock that the vendor function `__crc16_mutex_init()` creates and the SDK's own start-up calls. This image never called it, so the lock stayed "taken" and the controller task slept for ever. `ble_test_start` now calls it. Two more defects were fixed in the same change, because the first one would have exposed them: `ll_hci_adv_set_params` reads six bytes from its direct-address argument (it was NULL, which the bus guard is documented to fault on; now a zero array), and the context switch saved `r4`-`r9` while the compiler preserves `r4`-`r15` (now all of them, the list the compiler's own prologues use). None of the three has run on a pedal yet.

The third flash froze: the screen was drawn but the controls were dead, and the pedal reset by watchdog after about 8 s (`LAST BOOT FAILED`, `RESET BY: WATCHDOG`, `LOOPS 0`). The scheduler is cooperative, so a vendor call that never returns holds the whole main loop. The scout report (data/openpocket-ble-hang-diagnosis/report.md) found two such places on the advertising path: a vendor assertion that fails because the empty stand-in for `low_power_get` returned a null handle (a failed assertion parks in `P33_SYSTEM_RESET`, an endless loop), and, earlier, an unbounded wait on baseband status bit 1 (`0x28038`) that the repository's own baseband header says can stay set. `low_power_get` now returns a non-null handle. The second wait is untested; if it is the one, the crash screen below names it.

**Stall watch.** The 10 kHz timer interrupt counts while a `ble_poll()` runs. After 4 s it makes the next tick take the fatal path, so the pedal shows the normal crash screen with `VEC 3F` and the `PC` and `RETS` of the code that did not return (look them up in `firmware/build-ble/symbols.txt`; `PC 02000DC8` is a vendor assertion and `RETS` names it; a `PC` inside `ble_hw_disable` is the status-bit wait). It cannot fire while interrupts are off, and it watches only `ble_poll()`; the radio replay is excluded because it is long and its waits are bounded.

**After a failed boot Bluetooth stays off.** The next boot shows `LAST BOOT FAILED` and runs the normal firmware (status `OFF SAFE nn%`), so the boot guard never counts two failures in a row and the pedal does not fall back to the loader. The flasher works in that state. The boot after that one tries Bluetooth again.

## How it works

```
openpocket.c (OP_BLE_TEST)  ble_test_start() after the screen is up, ble_test_poll() in the main loop
firmware/ble/ble_test.c     a task that starts the controller, sets the advertising data and enables advertising
firmware/ble/ble_os.c       the operating-system services the library calls (below)
firmware/ble/ble_rf.c       the Wi-Fi front-end half of the radio start-up (below)
firmware/ble/ble_cfg.c      controller configuration: LE only, advertising + one slave link
firmware/ble/ble_stubs.c    empty stand-ins for the parts that are not built
third_party/jieli-ble/      JieLi's controller, as published (Apache-2.0): see its README.md
```

* **The controller is JieLi's**, from the Apache-2.0 SDK libraries (`btctrler.a`, 33 of its objects plus 3 helpers from
  `cpu.a`): HCI front end, link layer, advertising, the LE radio driver. Only the objects the image reaches are linked.
  The host stack (`btstack.a`) is not used; `ble_test.c` calls the controller's own `ll_hci_adv_*` functions.
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

All of it was built, linked and size-checked; none of it ran on hardware. The things most likely to go wrong, in the
order I would suspect them:

1. **The radio start-up.** It replays a capture taken from another firmware, with the stored crystal trim and the other
   saved radio records **not applied** (the VM is not read, so those fields stay at the capture's defaults). The carrier
   may therefore be off by some kHz. A scanner usually still sees it; a wrong value that stops the PLL would hang the
   start-up and the screen would stay on `STARTING`.
   The brief asked for the stock TX trims to be left as found. This image does **not** meet that exactly: it neither
   reads nor writes them, and the transmit level is whatever the library's power table gives with no saved trim.
   Reading them needs the VM item format, which is not documented in this repository; that would be the next task.
2. **Interrupt return.** The library's two interrupt functions end in `rti` without the `csync` the stock ones have
   (our compiler does not add it to bitcode; `fm1_isr.S` explains it for our own interrupts). The risk is an interrupt
   taken twice. Suspect this if the pedal resets or hangs a moment after `ADVERTISING` appears.
3. **The scheduler.** The context switch (`ble_ctx.S`) is eight instructions assembled from the compiler's own prologue
   and epilogue forms, and was not run. The queue, semaphore and timer code is plain C but also unrun.
4. **Flash writes while Bluetooth runs.** The library's interrupt code runs from flash (execute in place). Saving a
   preset or an update while advertising could, in principle, fetch code while the flash is busy. Do not save presets
   while testing.
5. **Real-time load.** Starting the radio blocks the main loop for the length of the start-up (screen and knobs freeze,
   audio keeps running from its interrupt). Afterwards the cost is the two radio interrupts every 100 ms.
6. **Other guards, checked on paper only.** After the stack-limit fault, the library's start-up path was read for the other
   guards (`fm1_guard.h`). *Write limits* cover only the two 256-byte stack bands and `0x01C7FD50` upward (boot info,
   mailbox, vectors); the controller's data, heap and task stacks are in `.bss`/`.data` between `0x01C08000` and about
   `0x01C1C000`, so none of its writes should land there. *Code range*: all of its code is linked into `.text` (`_etext`
   bounds it) and none runs from RAM. *Bus-invalid*: only the radio's own registers are touched. *Interrupt stacks*: the two
   BLE interrupts run on the system stack as every interrupt does, which is inside the window. None of this has run on
   hardware; a fault on the next flash is still possible, and the crash screen's `EMU` and `DBG` values say which guard it was.
7. **Regulation.** The pedal was certified with the stock firmware. This image uses the standard advertising channels,
   the 1M PHY and the library's own power table; it is a hobby experiment, not a product.

## Recovery

If the image does not start, the existing safety net applies: two failed boots go back to the update loader
(`UBOOT`), and the web installer, the SD-card recovery (`make appimage`) and the official firmware's own updater all work as
before. The image does not touch the flash outside the application area.
