# Bluetooth audio test image (A2DP: the phone's music through the pedal)

An optional build of OpenPocket that makes the pedal a **Bluetooth speaker**, as the stock BlackBox firmware is: it shows up on a
phone as **`OpenPocket`**, pairs with no PIN, and the phone's music plays **mixed into the pedal's output**, with the guitar.

It is a test image, its own build target (`make bt`). The normal firmware and the installer's packages are unchanged.

> **Status: built, not yet run on a pedal.** Every part that can be checked on a computer is checked (see "Checked on a
> computer"). What only a pedal can tell (does the classic radio start, does a phone see it, pair, play, and what the radio
> costs the DSP) is what the first runs are for. Expect a few rounds, as the BLE image needed.

## What is in it, and what is not

- **In:** classic Bluetooth (BR/EDR) with Secure Simple Pairing ("Just Works", no PIN), the A2DP sink with SBC, as the stock
  firmware. The music is summed to **mono** (the pedal's output is mono), at **-6 dB**, added **after** the MASTER volume and
  **before** the output's soft knee. The phone's own volume sets its level. It keeps playing in bypass and in the tuner.
- **Not in:** Bluetooth MIDI (the BLE image's: there is not the room for both), stereo, play/pause from the pedal (AVRCP),
  remembering a pairing after a power cycle, a level setting on the pedal.

## Build

```
make bt           # firmware/openpocket-bt.bin and build/bt/PocketX_940.fwsc, BlackBox_940.fwsc, manifest.json
```

(`node tools/op.mjs bt` is the same; `OP_BT=1 sh firmware/build.sh` inside the container.) CI builds it on every pull request:
the artifact `openpocket-bt-audio` holds the packages (`build/bt/`), the image, its symbol table and its link map.

## Flash it

Your pedal must already run OpenPocket.

1. `make serve-bt` opens the offline installer on `http://localhost:8765/` with `build/bt/` as its package folder (Chrome or Edge,
   USB cable). Or copy the artifact's `build/bt/*.fwsc` and `manifest.json` into `docs/firmware/` and use `python3 tools/serve.py`.
2. **Connect**, **Install**. The start-up screen reads `v40 BT AUDIO TEST 3` (the number is the test build's).
3. **Back to normal:** `make serve` (the release packages) and **Install** again. The SD-card recovery (`make appimage`) works
   as before. Nothing outside the application area is written.

## What you should see

| Step | On the pedal (status area, bottom right) | On the phone |
| --- | --- | --- |
| 1. start-up, a few seconds | `BT START 2`, `3`, `4`: the radio starting (the stages are the BLE image's) | |
| 2. ready | `BT VISIBLE`, then the visible line scrolls (below) | **Settings > Bluetooth**: `OpenPocket` appears under other devices |
| 3. tap it | `BT LINK`, the link line scrolls | paired and connected, **no PIN asked** |
| 4. play music | `BT PLAY` | the music plays **from the pedal's output**, with the guitar |
| 5. pause / disconnect | `BT LINK` / `BT VISIBLE` again | |

`FAILED s c` means the start failed: the line that scrolls says which step and stage (the BLE image's codes,
[BLE-TEST.md](BLE-TEST.md#reading-the-status-text)). The stall watch, the crash screen and the `LAST BOOT FAILED` report work as in
the BLE image, and after a failed boot Bluetooth stays off for that boot, so the pedal can always be re-flashed.

### If it stalls (`VEC 3F`, then a safe-mode boot)

The stall watch ends a Bluetooth task that runs for 4 s without giving the processor back: the crash screen, `VEC 3F`, then a
boot in safe mode with Bluetooth off. The `PC` and `RETS` on that crash screen are whatever the watch's 10 kHz tick interrupted,
which with sound running is mostly the audio interrupt (`fm1_cpu1_wait`, `a_collect`): not the stuck code. This image keeps a
**stall record** instead: during the last second before the watch fires, the tick samples the stuck task's own PC and RETS (from
the interrupt stack), and when it fires the record takes the rest. The safe-mode boot shows it, on the log's last three lines,
until Bluetooth starts again:

```
bluetooth  STALL btctrler S8 BB 00000107 0000FE02 00000000 0000FFFF 00000001 CLKN 00001234 00001240 L 3/3 PW 00000400 00000000 0000C000
link       STALL HEAP 9120 MIN 8800 MF 0 0 IRQ 0/0 EV 0 STUB 0 CR rf replay, rf replay done, btctrler, request_irq 40, request_irq 41, low_power_get
trace      STALL PC 02012345/02011111*9876 N 10000+0 D 28-160 SP 01C7BF80
```

| Field | Meaning |
| --- | --- |
| `STALL btctrler S8` | the task that was running and the start-up stage (the BLE image's stages: 3 = the radio replay, 7 = after it, 8 = the baseband asked for its interrupts) |
| `BB` | the classic baseband's registers 0x20000, 0x2000C, 0x20014, 0x20018 and 0x2001C (1 there: a clock latch the library asked for never completed) |
| `CLKN a b` `L x/y` | the classic baseband's native clock latched twice, 1 ms apart, and the reads each latch took (65535: it never completed: the clock does not run) |
| `PW` | 0x2FC40, 0x2FC6C (the Bluetooth domain's power and clock words the classic code waits on) and 0x10010 |
| `HEAP` `MIN` `MF n size` | the Bluetooth heap free now and at its lowest, failed allocations and the last one's size (the library stops in a loop on one) |
| `CR` | the last calls the library made into the firmware, oldest first |
| `PC pc/rets*n` | the stuck code: each PC and RETS sampled, and how often (look them up in `build-bt/symbols.txt` of the same CI run) |
| `N` `D` `SP` | samples taken (+ those that fit no slot), the interrupt stack's depth at the samples, the crash's stack pointer |

### The visible line

`BT VIS IRQ 40:1234 41:56789 EV 4 RDY SCAN 1 V F BD 0B0B ADDR 024F1A2B3C4D UID 1 HEAP 9120 STUB 0`

| Field | Meaning |
| --- | --- |
| `IRQ 40` `41` | the classic baseband's interrupts (packets, the alarm) and the slot timers: they rise when the radio runs |
| `EV` | events from the stack; `RDY`: it said it is ready (`NORDY`: it did not, the pedal made itself visible anyway after 2 s) |
| `SCAN` | times "visible and connectable" was sent to the controller |
| `V` | the stored radio calibration: found records (`F` = all four), the VM area (flash / 4096), the crystal trim (as the BLE image) |
| `ADDR` | this pedal's Bluetooth address, made from the flash chip's unique ID (`UID 1`; `UID 0`: not read, the fixed BLE address is used) |
| `HEAP` | the Bluetooth heap's lowest free bytes |
| `STUB` | calls of the stand-ins (functions this image leaves out) and the last one's name: should stay 0 |

### The link line

`BT PLAY n1 ENC 1 A2DP 1 PLAY 1 SBC 44100 2CH BP53 FR 51234 ER 0 PK 7345 BUF 148+52 MS UND 0 RATE -0.04% DISC 0`

| Field | Meaning |
| --- | --- |
| `n` `ENC` `A2DP` `PLAY` | links so far, encrypted links, A2DP channels, times the music started |
| `SBC 44100 2CH BP53` | the stream: its rate, channels, bitpool (53: the usual "high quality") |
| `FR` `ER` `PK` | SBC frames decoded, frames refused, packets received |
| `BUF a+b MS` | the music waiting: still encoded (a) and decoded (b); it keeps about 150 ms in all |
| `UND` | times the decoded music ran out (an underrun: a short gap) |
| `RATE` | how much the pedal is speeding up or slowing the music to follow the phone's clock (about -0.04 % is normal) |
| `DISC` | disconnections |

## Please report

- What happened at each step of the table, the phone (model, iOS / Android version), and a photo of the status area at the
  furthest step reached (or of `FAILED ...`).
- `node tools/op.mjs log --follow` while you try it (USB cable): it reads the same lines over USB, so no one has to read the
  scrolling text. Send its output.
- With the music playing: `node tools/op.mjs dsp --presets 1,14,18,19,20,21,24 --seconds 8`, and the same with the music
  paused. The question this image is for: does a streaming radio push heavy presets into `BYP` or late blocks?
- By ear: clicks or gaps in the music (and whether `UND` rises), hum or buzz in the guitar sound while the music plays.

## Known limits and risks

1. **The pairing is forgotten at power-off.** Afterwards the phone says it cannot connect: forget `OpenPocket` in its Bluetooth
   settings and pair again.
2. **Saving a preset while the music plays** gives a short gap (interrupts are off while the flash is written).
3. **The decoded music, the SBC decoder's state and the mix buffer sit in 7.5 KB of SRAM** (`0x01C06000..0x01C07E00`) that
   OpenPocket never used before. If anything else writes there, you would hear clicks or noise in the music, nothing worse.
4. **The flash window is larger in this image** (the package's whole app slot, `0x9D740` bytes, instead of `0x8DFBC`): the classic
   stack is bigger than the LE one. The packages and the update loader already used that slot size.
5. **Pedals that ran BlackBox_021** before OpenPocket may have lost part of their stored radio calibration (021 kept it at
   `0x9F000`, inside the area OpenPocket rewrites); the `V` field shows what was found. Classic Bluetooth is stricter about the
   crystal trim than BLE.
6. **Regulation:** the pedal was certified with the stock firmware. This image uses the library's own power table; it is a hobby
   experiment, not a product.

## How it works

```
openpocket.c (OP_BT_AUDIO)   the BLE test image's hooks: ble_test_start() after the screen, ble_test_poll() in the main loop
firmware/bt/bt_a2dp.c        the app: start (a task runs btstack_init), the stack's events, the music: packets -> SBC -> ring
firmware/bt/bt_sbc.c         Android's SBC decoder behind three calls: the packet's first frame, one frame, mono
firmware/px_bt_ring.h        the decoded music's ring and its rate, read by px_audio.h dsp_out (in RAM, on either core)
firmware/bt/bt_glue.c        what the stack needs from the system: the radio pools' allocator (bt_lbuf.h), low power, the
                             address (the flash's unique ID), the name, the SDP record (the library's own A2DP sink record)
firmware/bt/bt_cfg.c         the stack's configuration: classic only, one link, an A2DP sink (the SDK's values, Apache-2.0)
firmware/bt/bt_stubs.c       empty stand-ins for what the image leaves out (LE, TWS, calls, other profiles), counted
firmware/bt/app-bt.ld        the memory map: the classic stack's sections, its pools, the ring
firmware/bt/members.txt      the JieLi objects compiled (third_party/jieli-ble + third_party/jieli-bt)
firmware/ble/                shared with the BLE image: the scheduler and heap, the radio start-up, the stall watch, DIAG
```

- **The Bluetooth stack is the stock firmware's:** JieLi's classic controller and host from the AC79 SDK (Apache-2.0, LLVM
  bitcode, the archives `third_party/jieli-ble` already pins). The SDP record the phone reads is the library's own, byte for byte
  the stock firmware's (`JL_A2DP`).
- **The SBC decoder is Android's** (`third_party/sbc-android`, Apache-2.0), fixed point, about 2 % of one core.
- **The music path:** the radio's receive pool (12 KB) holds the phone's packets, still encoded; the main loop decodes them into a
  58 ms ring of mono samples; the audio path reads the ring at a rate the main loop steers, so the pedal follows the phone's clock
  (the two differ by about 0.04 %) with a backlog of 150 ms, by linear interpolation. Nothing of the stack runs in the audio
  interrupt: it only reads the ring.
- **Memory** (beyond the BLE image's): four task stacks of 2.75 KB in NOINIT, the baseband's table (4.2 KB) and the receive pool in
  the SRAM below the RAM, the ring, the decoder's state and the mix buffer above it.
- **The baseband's 64 KB window.** The classic baseband reaches memory as 16-bit offsets from its table (`bd_base`, register
  0x2FC44): everything the library hands it must lie less than 64 KB above `bd_base`, and the library asserts it (a failed
  assertion stops in a loop). So the send pool (2 KB, also the page and inquiry scan buffers), the controller's own variables
  and the Bluetooth heap come first in the RAM, as the SDK keeps them next to `bd_base` (its `btctler_lib_bss.ld`), and the link
  checks it (`firmware/bt/app-bt.ld`). Test build 1 had the send pool at the end of the RAM, 118 KB above: its assertion was
  the stall at 5.5 s.

## Checked on a computer

- `tests/test_bt_ring.c`: a phone at 44.1 kHz with 15 ms of jitter, the pedal at its real 44,117.6 Hz, 60 s: no underrun, the rate
  settles at the clocks' ratio, a 1 kHz sine comes out 52 dB clean, the pitch wanders 0.7 cents; a 300 ms pause stops it once,
  without a click, and it starts again.
- `tests/test_bt_sbc.c`: Android's encoder makes the frames a phone sends (joint stereo, 328 kb/s), packed as A2DP packets
  (contributors, an extension, padding): every frame decodes, the output is the mono mix 64 dB above the difference; mono
  streams; a frame whose CRC fails is refused and the stream goes on.
- `tests/test_bt_lbuf.c`: the radio pools' allocator through 100,000 random operations (no overlap, no corruption, no leak).
- `tests/test_bt_sources.mjs`: the vendor objects are as published and listed, the licences are there.
- The member set was chosen from the archives' symbol tables: what it references and does not define is supplied
  (`bt_glue.c`, `bt_cfg.c`) or stood in for (`bt_stubs.c`), with the library's own types.
