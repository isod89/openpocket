# Bluetooth audio test image (A2DP: the phone's music through the pedal)

An optional build of OpenPocket that makes the pedal a **Bluetooth speaker**, as the stock BlackBox firmware is: it shows up on a
phone as **`OpenPocket`**, pairs with no PIN, and the phone's music plays **mixed into the pedal's output**, with the guitar.

It is a test image, its own build target (`make bt`). The normal firmware and the installer's packages are unchanged.

> **Status: test build 14.** Builds 1 to 10 ran on a pedal: the radio starts, a phone sees the pedal and links, but no phone
> could pair, and the guitar crackled. Build 11 paired an iPhone (below, "Test build 11"), but no music came out, and the guitar
> still crackled with Bluetooth on. Build 12 takes the phone's packets where the controller first shows them, sends the output
> in two slots against the crackles, and records the second core's fault (below, "Test build 12"); on some power-ons it
> crashed in the reverb, 2 s after power-on. Build 13 stops the reverb's loop at its block's end and counts it, and shows the
> registers of a crash (below, "Test build 13"). Build 14 is the same image on OpenPocket v42, with v42's own fix of the
> crackles' cause and the Bluetooth library folded at build time to make the room (below, "Test build 14"). Every part that
> can be checked on a computer is checked (see "Checked on a computer").

## What is in it, and what is not

- **In:** classic Bluetooth (BR/EDR) with Secure Simple Pairing ("Just Works", no PIN), the A2DP sink with SBC, as the stock
  firmware. The music is summed to **mono** (the pedal's output is mono), at **-6 dB**, added **after** the MASTER volume and
  **before** the output's soft knee. The phone's own volume sets its level. It keeps playing in bypass and in the tuner.
- **Not in:** Bluetooth MIDI (the BLE image's: there is not the room for both), stereo, play/pause from the pedal (AVRCP),
  remembering a pairing after a power cycle, a level setting on the pedal.

## Build

```
make bt           # firmware/openpocket-bt.bin and build/bt/PocketX_942.fwsc, BlackBox_942.fwsc, manifest.json
```

(`node tools/op.mjs bt` is the same; `OP_BT=1 sh firmware/build.sh` inside the container.) CI builds it on every pull request:
the artifact `openpocket-bt-audio` holds the packages (`build/bt/`), the image, its symbol table and its link map.

## Flash it

Your pedal must already run OpenPocket.

1. `make serve-bt` opens the offline installer on `http://localhost:8765/` with `build/bt/` as its package folder (Chrome or Edge,
   USB cable). Or copy the artifact's `build/bt/*.fwsc` and `manifest.json` into `docs/firmware/` and use `python3 tools/serve.py`.
2. **Connect**, **Install**. The start-up screen reads `v42 BT AUDIO TEST 14` (the number is the test build's).
   Before pairing with a phone that saw an earlier build, forget the pedal there first (iPhone: **Settings > Bluetooth**, the
   **i** next to `OpenPocket` or `Accessorio`, **Forget This Device**).
3. **Back to normal:** `make serve` (the release packages) and **Install** again. The SD-card recovery (`make appimage`) works
   as before. Nothing outside the application area is written.

## What you should see

| Step | On the pedal (status area, bottom right) | On the phone |
| --- | --- | --- |
| 1. start-up, a few seconds | `BT START 2`, `3`, `4`: the radio starting (the stages are the BLE image's; the digit can be cut off on the play screen, the log's bluetooth line has it) | |
| 2. ready | `BT VISIBLE`, then the visible line scrolls (below) | **Settings > Bluetooth**: `OpenPocket` appears under other devices |
| 3. tap it | `BT LINK`, the link line scrolls | paired and connected, **no PIN asked** |
| 4. play music | `BT PLAY` | the music plays **from the pedal's output**, with the guitar |
| 5. pause / disconnect | `BT LINK` / `BT VISIBLE` again | |

**A/B key, held while switching the pedal on, until the start-up screen has gone** (for that boot only; the log's `c1` line
shows `KEY`). Switch off for real (USB out too, 10 s) before each boot you compare:

| Key | That boot |
| --- | --- |
| **REV** | Bluetooth off (`BT OFF`): the same image with the radio off, to compare the sound with and without Bluetooth by ear |

(CAB and DLY held at power-on do nothing since test build 13: the c1 line only shows `KEY 1` or `KEY 2`. Test build 12's DLY
key, the output in one slot as build 11 sent it, is gone: its A/B is settled. Since build 14 the output is sent as v42 sends
it, in place of the two slots (below, "Test build 14"). A CAB key for the stock firmware's supply levels did not fit in the
flash.)

`FAILED s c` means the start failed: the line that scrolls says which step and stage (the BLE image's codes,
[BLE-TEST.md](BLE-TEST.md#reading-the-status-text)). The stall watch, the crash screen and the `LAST START FAILED` report (v42's
name; up to test build 13 `LAST BOOT FAILED`) work as in the BLE image, and after a failed boot Bluetooth stays off for that
boot, so the pedal can always be re-flashed. Since test build 13 this image's report shows the registers of a crash (`R1` `R6`,
then `R12` `SP`) where the BLE image shows `DSP CODE AT`; the lines below them move one line down, and v42's `PHOTO THIS FOR A
BUG REPORT` is the screen's last line. Photograph both screens (below, "Test build 13").

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

`BT VIS IRQ 40:1234 41:56789 EV 4 RDY SCAN 1 V F BD 0B0B ADDR 024F1A2B3C4D UID 1 HEAP 9120 STUB 0 Q40 0 KICK 0 TX 0/592/2032 F0 RX 0/0/12272 F0`

| Field | Meaning |
| --- | --- |
| `IRQ 40` `41` | the classic baseband's interrupts (packets, the alarm) and the slot timers: they rise when the radio runs |
| `EV` | events from the stack; `RDY`: it said it is ready (`NORDY`: it did not, the pedal made itself visible anyway after 2 s) |
| `SCAN` | times "visible and connectable" was sent to the controller |
| `V` | the stored radio calibration: found records (`F` = all four; since test build 14 the BLE image's flags in full: `10` and `20`, VM 106 or 107 missing and v41's defaults given in its place, `400` the calibration measured, `100` and `200` its scan or trim failed), the VM area (flash / 4096), the crystal trim (as the BLE image) |
| `ADDR` | this pedal's Bluetooth address, made from the flash chip's unique ID (`UID 1`; `UID 0`: not read, the fixed BLE address is used) |
| `HEAP` | the Bluetooth heap's lowest free bytes |
| `STUB` | calls of the stand-ins (functions this image leaves out) and the last one's name (the trace line lists them in order) |
| `Q40` | seconds since IRQ 40 last rose. While a phone is searching it stays near 0; a large number means the scans have stopped |
| `KICK` | times the pedal asked for its scans again because IRQ 40 had been silent for 5 s while visible (test build 4) |
| `TX` `RX` | the radio's send and receive pools: bytes in use / the most ever in use / the pool's size, and `F` failed allocations. While a phone is linked, `RX` must rise: the phone's packets wait there (test builds 1 to 5 never used it, see below) |

(Test build 11 also had `RNG a/b`, the random source the pairing's keys come from: it worked, and build 12 drops it.)

While no phone is linked, the link line shows the Bluetooth tasks instead (test build 8): `TASKS btapp S 01C0A1B0=0 q0 F3012
P0, btctrler Q q0 F2344 P3, btstack R q2 F1876 P7`, each task's state (`R` ready, `Q` waiting for a message, `S` waiting
on the semaphore or mutex at that address, `=` its count, `D` sleeping, `X` ended) and `q` its queued messages. A task that
stays `S` while its `q` grows is the stuck one: look its address up in `build-bt/symbols.txt`. Since test build 11 also `F`,
the bytes of its stack (4 KB) never used, `P`, the most messages it ever had queued, and `OVF` if it came within 64 bytes of
its stack's end.

The `hci` line (test build 9) is the HCI conversation between the controller and the host, which in this integrated stack
travels through the tasks' queues: the newest 11 messages (test builds 9 to 11 also had `hci0`, the first 12 since boot; build
12's a2dp line took its place, and build 13's FIFO line took the a2dp line's). `n` all messages posted to a task, `ACL` data packets to the host and `NCP` Number of Completed Packets events
(only counted), `Q` the deepest a task's queue has been and `LOST` the posts refused because a queue was full (the library
tries some of them again after a short sleep, so not each one is a message lost). Each entry: the task
(`S` the host, btstack; `C` the controller, btctrler; `E` btencry; `A` btapp), the message's type digit, then its first 5
bytes in hex. `S1` is an event to the host: the event code, the parameters' length, the parameters (`S1:0E04010C03`: Command
Complete, 1 command allowed, opcode 0x0C03; `S1:04..` Connection Request; `S1:03..` Connection Complete, status next; `S1:05..`
Disconnection Complete; `S1:31` IO Capability Request; `S1:33` User Confirmation Request; `S1:36` Simple Pairing Complete).
`C2` / `C3` are commands to the controller as its task receives them. Since test build 11 the `hci` line also has `WI`, waits
the library asked for inside the baseband's interrupt and was refused (each one a message or an allocation the library then
dropped), `WM`, waits asked by the main loop (a software timer's callback, which cannot wait), `CNF`, the pairing
confirmations the pedal sent, and `END cc:rr`, how the last link ended: `05:rr` a Disconnection Complete with its reason
(`13` the phone ended it, `08` the link was lost, `22` an LMP response timed out, `05` authentication failed), `03:ss` a link
that failed to come up and its status. A successful pairing walks `S1:31`, `S1:32`, `S1:33`, then a `C3` message (the
confirmation), `S1:36 00` (Simple Pairing Complete), `S1:18` (the link key) and `S1:08 00` (encryption on).

The trace line ends with `CR`, the last calls the library made into the firmware, oldest first (the stand-ins by name,
`request_irq 40`, `low_power_get`, `kick: scan again`, `send pool full`...).

### The link line

`BT PLAY n1 ENC 1 A2DP 1 PLAY 1 SBC 44100 2CH BP53 FR 51234 ER 0 PK 7345 BUF 98+52 MS UND 0 RATE -0.04% DISC 0 FQ 6 F A3012/C2344/S1876`

| Field | Meaning |
| --- | --- |
| `n` `ENC` `A2DP` `PLAY` | links so far, encrypted links, A2DP channels, times the music started |
| `SBC 44100 2CH BP53` | the stream: its rate, channels, bitpool (53: the usual "high quality") |
| `FR` `ER` `PK` | SBC frames decoded, frames refused, packets decoded (test build 11 stayed at one a session: no music) |
| `BUF a+b MS` | the music waiting: still encoded in the FIFO (a) and decoded (b); it keeps about 150 ms in all |
| `UND` | times the decoded music ran out (an underrun: a short gap) |
| `RATE` | how much the pedal is speeding up or slowing the music to follow the phone's clock (about -0.04 % is normal) |
| `DISC` | disconnections |
| `FQ` | the phone's packets waiting in the FIFO (test build 12; 16 at most, about 6 to 10 while it plays). Test build 11 had `PD` here |
| `F` | each Bluetooth task's stack never used, in bytes, after its initial: `A` btapp, `C` btctrler, `S` btstack (`!`: it nearly overflowed) |

### The isr line (since test build 11)

`ISR 40:12/310/1450 41:4/40/90 T5 9/31 AG 2144/2935 AI 846/942 EN 0/344/344/0/0 OC 0/3 OD 0 OG 12/240 C1 1 G 0 NAM 1 LD 42 PK 55/97 XR 3 CL 0 WC 0 BYP 0 TM 1234 VM 4/2 OVF 0 NAN 0`

What the interrupts cost the audio, and the DSP's state, for the crackles. Times are in microseconds.

| Field | Meaning |
| --- | --- |
| `40:a/b/c` `41:a/b/c` | the classic baseband's two interrupts: the longest in the last second, the longest since boot, and the time they took in all in the last second. They run above the audio |
| `T5 a/b` | the 10 kHz timer (USB, the stall watch), also above the audio: the longest in the last second / since boot |
| `AG a/b` | the longest gap between two audio interrupt entries (last second / since boot). 2,902 when one entry serves a period; less (2,144 in build 11's log): two entries a period, the input's and then the output's, so AG is the period less the input's stay; more: an entry came late. A preset change or a flash write is not counted |
| `AI a/b` | the audio interrupt's longest stay (last second / since boot), the DSP and its wait for the second core included |
| `EN a/b/c/d/e` | (test build 12) the audio interrupt's entries in the last second, by the flags they found: none / the output's only / the input's only / both / another channel's. About 344 a second each is two entries a period; `d` counts the periods whose output callback ran first; `e` must be 0 |
| `OC a/b` | (test build 14) the output blocks the driver asked for before their input's, in the last second / since boot (v42's `firmware/px_aorder.h`, `a_swaps`). Up to test build 11 each one repeated a 2.9 ms block and skipped the next: a click on any sounding note; builds 12 and 13 sent the block before (their `OC a/b c/d` counted the changes of order); since build 14 each such block is filled as soon as its input's block is made, in the same entry, with no latency more, unless its input's callback comes too late (`OD`). Each full preset change and each flash write (the audio interrupt held) adds one by itself |
| `OD` | (test build 14) of those, the ones whose input's callback came more than half a block (1,451 µs) after the output's, since boot (`px_audio.h` `A_ORDER_TICKS`): each one a click, that half of the output playing its old block again. It must stay 0 |
| `OG a/b` | (test build 14) the longest from an output callback that came first to its input's callback (last second / since boot): the radio's interrupts (`40`, `41`) and `T5` running between the two. `OD` rises once it reaches 1,451 µs |
| `C1` | 1: the second core runs; 0: the whole DSP is on one core (heavy presets then go to `BYP`) |
| `G` | why the second core is off: 0 it is not; 1 it misread shared memory, 2 a job was not back within 8 ms, 4 it faulted (1, 2 and 4: retired in this boot, and barred at every start until a power-off); 3 barred at start (by such a retirement, or by a crash of the first core while it ran, in an earlier boot of the same build); 5 it did not answer at start. The c1 line has the rest |
| `NAM` | 1: the preset's NAM amp is one that runs on both cores; 0: not (test build 12: whatever `C1` is) |
| `LD` `PK a/b` | the DSP load, and its peak since the last full preset change / since boot, % of a block's time |
| `XR` `CL` `WC` `BYP` | late blocks (each one a likely crackle), clipped blocks, the delay and reverb lane's cut blocks (clicks in the tails; since build 12 kept across a retirement or a reset), bypassed |
| `TM` `VM r/w` `OVF` | the library's software timers that fired, the pairing store's reads and writes, task stacks that nearly overflowed |
| `NAN` | (test build 12) the engine's resets after a NaN, since boot: each one 15 to 50 ms of silence |

### The FIFO line (test build 13)

`FIFO 000004D2 00000000 00000005 00000002`: the music path's counts since boot, 4 words in hex (`node tools/op.mjs log` prints
them a second time with their names). It is the log's seventh line, in place of test build 12's a2dp line (`A2` and 23 words:
the controller's own A2DP state for each packet, and a 10 kHz watch over it). That probe is gone in build 13, to make room for
the reverb's cut and the crash report; the music plays from the FIFO whatever the controller's state says, and the heal stays.

| Word | Meaning |
| --- | --- |
| 1 | packets the FIFO took (about 70 a second while the music plays) |
| 2 | packets it could not take: full in the low half, too big in the high half (`00010003`: 3 full, 1 too big) |
| 3 | packets the library handed the pedal, given back undecoded (the FIFO already has them) |
| 4 | heals: the receiving connection's two A2DP fields written back when found wrong, on the connection the host opened only |

### The c1 line (test builds 12, 13)

`C1 1 G 0@0 K 2232 KEY 0 S 00000000 F 0/0 W 0`; after a cut of the reverb's loop, 4 words in hex after `W`'s count; after a
fault of the second core, 19 more words in hex.

| Field | Meaning |
| --- | --- |
| `C1` `G g@t` | the second core runs; why it is off (as in the isr line) and when it was retired (ms; wraps every 179 s) |
| `K` | how deep the second core's stack has gone, in bytes (of 2,816) |
| `KEY` | the key held at power-on: 0 none, 1 CAB and 2 DLY (nothing in this build), 3 REV |
| `S` | where Bluetooth was when the main loop first saw a fault: stage << 28 \| state << 24 \| DSP held << 23 \| 1 << 22 \| IRQ 40's count |
| `F n/c` | the second core's faults, and which core took the exception (0: the first, the usual case) |
| `W` | (test build 13) the reverb loop's cuts at its block's end in this boot; after one, `n` and `k` at the latest cut, the time the main loop first saw a cut (ms since power-on, wraps every 179 s) and the `S` word then (how to read them: "Test build 13") |
| then | DBG (the debug unit's message: bits 16-18 a bus error, 10 a PC limit), the taking core's EMU_MSG, PC, RETS (the first core's when it took the exception), the second core's own EMU_MSG, the time (ms), the job handed and the job finished (different: one was running), its function, and the debug unit's ten words from `0x1EEE358` (the bus masters' ids of an invalid access, the write-limit error counts) |

The line holds 223 characters: with a cut and a fault of the second core in the same boot, the debug unit's last four or so
words are cut off.

## Please report

- What happened at each step of the table, the phone (model, iOS / Android version), and a photo of the status area at the
  furthest step reached (or of `FAILED ...`).
- `node tools/op.mjs log --follow` while you try it (USB cable): it reads the same lines over USB, so no one has to read the
  scrolling text. Send its output.
- With the music playing: `node tools/op.mjs dsp --presets 1,14,18,19,20,21,24 --seconds 8`, and the same with the music
  paused. The question this image is for: does a streaming radio push heavy presets into `BYP` or late blocks?
- By ear: clicks or gaps in the music (and whether `UND` rises), hum or buzz in the guitar sound while the music plays.
- For crackles: the `isr` line while they happen, and the same preset with Bluetooth off (REV held at power-on). If they are
  there with Bluetooth off too, the radio is not the cause. If they come only with Bluetooth on and nothing is playing (a
  high-gain preset, the guitar's volume at zero), ticks at the scans' rhythm point to the radio's supply or RF reaching the
  analog side, which no firmware change can fix.
- After any boot whose c1 line shows `G 4` or a `W` other than `W 0`: the c1 line as it is (its words, looked up in
  `build-bt/symbols.txt` of the same CI run, say where the second core faulted). A red crash screen: a photo of it (PC, RETS,
  VEC, DBG), before switching off, and one of the `LAST START FAILED` report that follows it (since test build 13: the registers).

## Known limits and risks

0. **The radio's interrupts run above the audio's** (test build 4): the classic baseband's handlers have slot deadlines
   (625 µs), and OpenPocket's audio interrupt runs its DSP work in blocks of 128 frames, up to 2.9 ms (stock runs its dry chain
   in its audio interrupt too, but in blocks of 64 frames, with the radio below it). A heavy preset may run late more often
   while Bluetooth is busy: the `isr` line (test build 11) shows each interrupt's cost and whether the audio started late.
   Since test build 5
   they enter through OpenPocket's own interrupt entry (`firmware/bt/bt_isr.S`, flags saved first, `csync` before `rti`) and
   C bodies that follow the library's two handlers (`firmware/bt/bt_irq.c`): the library's own handlers end without `csync`,
   and in test build 4, above the audio, the DSP they interrupted faulted (the reverb, `VEC 1`, 0.5 s after the DSP started).

1. **The pairing is kept in RAM, until power-off** (test build 11; before it, not even that). After a power-off the phone says
   it cannot connect: forget `OpenPocket` in its Bluetooth settings and pair again.
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
tools/jlfold.py              since test build 14 they are compiled folded: their configuration constants known (bt_cfg.c)
firmware/ble/                shared with the BLE image: the scheduler and heap, the radio start-up, the stall watch, DIAG
```

- **The Bluetooth stack is the stock firmware's:** JieLi's classic controller and host from the AC79 SDK (Apache-2.0, LLVM
  bitcode, the archives `third_party/jieli-ble` already pins). The SDP record the phone reads is the library's own, byte for byte
  the stock firmware's (`JL_A2DP`).
- **The SBC decoder is Android's** (`third_party/sbc-android`, Apache-2.0), fixed point, about 2 % of one core.
- **The music path:** the radio's receive pool (12 KB) holds the phone's packets as they arrive; where the controller shows each
  media packet to the A2DP code, the pedal copies it into a 16-packet FIFO (test build 12: the controller's own hand-over to the
  application failed from the second packet of each session); the main loop decodes the FIFO into a 58 ms ring of mono samples;
  the audio path reads the ring at a rate the main loop steers, so the pedal follows the phone's clock (the two differ by about
  0.04 %) with a backlog of 150 ms, by linear interpolation. Nothing of the stack runs in the audio interrupt: it only reads the
  ring.
- **Memory** (beyond the BLE image's): three task stacks of 4 KB in NOINIT (test builds 1 to 10: four of 2.75 KB, the fourth
  never used), the baseband's table (4.2 KB) and the receive pool in the SRAM below the RAM, the ring, the decoder's state and
  the mix buffer above it, the pairing store (672 bytes of RAM), the FIFO (10.8 KB of the Bluetooth heap); in the RAM code
  window's free end, the output block (512 bytes; builds 12 and 13: two, 1 KB) and, since test build 14, the engine's buffers
  that v42 moved below the RAM (14.6 KB: there they would take the baseband's table and the receive pool's place).
- **The baseband's 64 KB window.** The classic baseband reaches memory as 16-bit offsets from its table (`bd_base`, register
  0x2FC44): everything the library hands it must lie less than 64 KB above `bd_base`, and the library asserts it (a failed
  assertion stops in a loop). So the send pool (2 KB, also the page and inquiry scan buffers), the controller's own variables
  and the Bluetooth heap come first in the RAM, as the SDK keeps them next to `bd_base` (its `btctler_lib_bss.ld`), and the link
  checks it (`firmware/bt/app-bt.ld`). Test build 1 had the send pool at the end of the RAM, 118 KB above: its assertion was
  the stall at 5.5 s.
- **The radio pools' allocator** (`firmware/bt/bt_lbuf.h`) keeps each pool's header at the pool's first byte: the library
  hands `lbuf_init` the pool and then uses the pool's own address as the pool, never the value returned. Test builds 1 to 5
  aligned the header to 16 bytes; the receive pool (0x01C02DE4) is only 4-byte aligned, so every allocation from it failed and
  a phone's link died after a few seconds (`RX 0/0`).

## Test build 11

Builds 1 to 10 got the radio up and a phone linked, then each failed further along. A review of the whole path, against the
library's own code, found four walls between "linked" and "paired", one behind the other, so all four are fixed together:

1. **The host ignored the link.** It tells a classic link from an LE one by comparing the handle with `get_ble_link_handle()`,
   and that was a stand-in returning 0: no handle was classic, so every packet of the phone's was dropped and L2CAP, SDP and
   A2DP never heard of the link. It returns 0x50 now, the library's own value (`bt_glue.c`).
2. **Nobody confirmed the pairing.** "Just Works" still asks the host to confirm (HCI 0x33); this host leaves the answer to the
   application, and the pedal never gave it, so the phone's timer ran out. The pedal confirms now (`bt_a2dp.c`), as the SDK's
   demo does.
3. **The phone's SDP question was never answered.** The SDP server waited, for a phone whose maker it does not know, for its own
   SDP client, which this image does not have. It answers at once now (`bt_cfg.c`, `bt_stubs.c`).
4. **After a failed link the pedal stayed invisible.** The controller turns the scans off when it makes a link and leaves it to
   the host to turn them on again; the pedal now does at every link's end (`bt_a2dp.c`).

Also: the scheduler's clock no longer wraps every 179 s (every timer and sleep pending at the wrap used to stop for good); no
wait is made inside the baseband's interrupt any more (the library asks, and was always told "not in an interrupt"); three task
stacks of 4 KB with their high-water marks; the pairing kept in RAM until power-off; the music packet held at a stop given back.

For the crackles: since test build 4's crash, the second core was most likely **barred** on this pedal: that crash set the bar,
and every test build after it had the same id, so the bar stayed until a real power-off, and the whole DSP ran on one core,
with the radio's interrupts on top. Each test build now has its own id, and the stall watch's own crash no longer bars it: the
first boot of build 11 runs on two cores (`C1 1` in the `isr` line; the screen shows the cores only where it shows the load:
in bypass, `BYP C1 ...`, for 2 s after a late block, `C1 42%!`, and with Bluetooth off by REV, `BT OFF C1`; while Bluetooth
is on, the status area scrolls the Bluetooth line instead). Then the
`isr` line measures what the radio costs the audio, and REV at power-on gives the same image without the radio, to compare by
ear. A preset change now also waits for silence to reach the output before it holds the audio interrupt (up to build 10 the
output buzzed at 172 Hz for the length of the recompute, in every image), and the phone's music fades with the guitar.

## Test build 12

Build 11 paired an iPhone and the phone streamed (about 70 packets a second), but the pedal played nothing (`PK 1`), and the
guitar still crackled with Bluetooth on, never with it off (REV). A review of the build-11 log against the library's code found:

1. **No music.** The controller decides, for each media packet, whether it goes to the application or to the host: it keeps it
   for the application only if the packet's channel is the one the application opened. In build 11 only the first packet of
   each session was kept (the one the channel's open marked itself); every later one failed that test and went to the host,
   which has nothing to play it with. No code in the library could have undone the open; why the test fails is not settled. So
   the pedal now copies every media packet where the controller first shows it to the A2DP code, before that test, into a
   FIFO, and plays from there, whatever the test then decides. The new a2dp line measures the test's inputs for each packet,
   writes them back when wrong (`H`), and watches them 10,000 times a second, to settle the cause. (Build 13 keeps the
   writing back and drops the rest: the FIFO line.)
2. **Crackles.** The audio interrupt's driver (the SDK's) calls the output callback and the input-and-DSP callback each when it
   finds its own flag up. Build 11's isr line shows two entries a period, input first (`AG 2144` = the period less the DSP's
   stay), and seconds where the order flipped (`AG` just under the period): a few microseconds of extra latency (the radio's
   interrupts, a cold cache line) flip it. With one output buffer each flip played one 2.9 ms block twice and skipped the
   next: a step on any sounding note, with no late block, more often with Bluetooth on. The output now has two slots: the
   output callback always sends the block before the newest, whatever the order (2.9 ms more latency). `EN` and `OC` in the
   isr line count the entries and the flips; DLY at power-on brought build 11's single buffer back, to confirm by ear (gone in
   build 13: the two slots stay).
3. **The second core's fault.** It faulted (`G 4`) in every Bluetooth-on boot of build 11 and never with Bluetooth off. Its
   record is now kept and printed (the c1 line): the debug unit's message and bus-master ids, CPU1's own exception word, the
   time, the job, the stack's depth, and where Bluetooth was. (A CAB key for the stock firmware's supply levels, the cheap test
   of the "misreads under load" the board's notes describe, did not fit in the flash: the record decides whether it is needed.)

Also: Bluetooth's flash reads (the radio records, the address) run before the audio starts, and the radio's start-up burst and
the flash reads wait for the second core's job in flight (the flash path is off meanwhile); `WC` survives a retirement; the
log's ninth line; the build checks the library's static tables are where the a2dp line reads them (in build 13, where the
heal reads `channel`), and keeps the compiled controller code (`listings/` in the artifact) to compare with the library's IR.

## Test build 13

Build 12 played the phone's music. But on some power-ons with Bluetooth on it crashed about 2 s after power-on, while the radio
started: the red crash screen showed `VEC 1`, `DBG 00002000` (the first core wrote into a guard band) and a `PC` in `rv_block`,
the reverb. The photos and the compiled code tell how, not yet why:

- The reverb's loop runs over a chunk of 16 frames (the wet lane) or a block of 64 (the chain). In build 12's code the compiler
  made its end test "stop when k equals n", so a k that once got past n never stopped: the loop's store marched 13 KB through
  the RAM, until the guard band at `0x01C74000` stopped it.
- Why k got past n is open: something wrote over n or k on the loop's stack (A), or one read of n, or one branch, went wrong
  on the first core during the radio's start-up (B).

What changed:

1. **The reverb's loop stops at 64 frames, whatever its end says, and counts it**: `W` in the log's c1 line, with n as the loop
   read it and k. No caller passes more than 64, so in normal running `W` stays 0. A cut costs a click instead of a crash:
   up to 48 frames are still written past a 16-frame chunk, into the chunks after it, the lane's other buffer or the drive and
   amp's oversampling buffer, so the click can be in the reverb or, for one block, in the amp's sound. (The delay's loop has
   the same form; its cut did not fit in the flash.) In the code CI built (run 43039170246, `listings/px_dsp.s`), the cut also
   brought the end test back to "k below n": the loop now has two exits, and the compiler no longer rewrites it. A k that
   gets past n now just ends the loop. So a cut means that n was read too large, and a k that jumps is no longer counted.
2. **The `LAST BOOT FAILED` report shows the crash's registers**: `R1` and `R6`, then `R12` and `SP`, in place of `DSP CODE AT`;
   the lines below move down, and its `PLEASE TAKE A PHOTO` line is gone (the red screen before it asks already).
3. **The room for them:** build 12's a2dp line (its probe of every packet and its 10 kHz watch) and the DLY key are gone. The
   heal stays, and the log's seventh line is now the FIFO line.

### What to do

Switch off for real between boots (USB out too, 10 s). After each boot, once the pedal has started (about 10 s), read the
log once: `node tools/op.mjs log --count 1`. `W` starts from 0 at every boot, so a single reading at the end would show the
last boot only. Send all the readings.

| Boots | Phone | Key at power-on |
| --- | --- | --- |
| 5 | any | REV held (Bluetooth off) |
| 5 | its Bluetooth off until the pedal shows `BT VISIBLE` | none |
| 5 | its Bluetooth on and near | none |

If a boot crashes: a photo of the red crash screen, then one of the `LAST START FAILED` report that follows (it stays 12 s),
then the log reading. That reading is of the safe-mode boot, whose DSP is off: it cannot show the crashed boot's `W`.

Before the third set, pair the phone with the pedal once in this build (step 3 of "What you should see"), and do not forget
`OpenPocket` on the phone between these boots: it must still list the pedal. The pedal forgets the pairing at power-off, but a
phone that remembers it may try to connect while the pedal starts. That is part of the test. Note with each reading which set
it belongs to: the log tells the REV boots (`KEY 3`) from the others, but not the second set from the third.

### Reading W

`W 0`: no cut of the reverb's loop was counted in this boot. After a cut: `W count n k time S`, the count in decimal, the
rest in hex. `node tools/op.mjs log` names them and says which line of this table applies. The cut is taken when k reaches 64
(40 in hex), and it records that k.

| W shows | It means |
| --- | --- |
| `n` 10 (16) and `k` 40 (64) | n was read too large once, and was 16 again when the cut read it: a one-off wrong read of n on the first core (B) |
| `n` 40 and `k` 40 | the same in the chain's call (a block of 64), not in a lane's chunk |
| `n` anything else, `k` 40 | n's word on the stack was written over (A): its value is the writer's fingerprint |
| `k` above 40 | not expected with this end test: k jumped while n was also read too large. `0020xxxx` is the second core's known misread |
| `k` below 40 | a k below 64 cannot pass the cut's own check: that check misfired once (B), or k's register changed |
| the time and `S` | when the main loop first saw the cut (ms since power-on, wrapping every 179 s like `UPTIME`) and where Bluetooth was then, the same word as the c1 line's `S`. The main loop does not run during the radio's start-up replay (stage 3): a cut there is seen only once the replay has returned, at stage 7 or a step after it (the numbers are not in time order: 4, ready, comes after 7), and later than the cut itself (build 12 crashed at 1,971 ms) |

`W 0` does not rule out everything: a k that got past n ends the loop without a cut (this build's end test), and an n written
over with a value up to 64, or a k thrown back, still ends the loop late (up to 48 frames past the chunk) without a cut.

Many cuts in one boot, or cuts long after the start-up, are worth saying: the crash was a one-off at the radio's start-up.

### Reading the registers (if it still crashes)

The cut covers what a wrong n or k did in build 12 (above). If the crash comes back, something else went wild, and the
report's registers say what. Read `DBG` first (on the `UPTIME` line): `00002000` (bit 13) is the first core writing into a
guard band, as in build 12, and the rows below apply. `00200000` (bit 21) is a bus error flagged on the first core, most
likely the delayed flag of holding the second core (build 4's crash): the `PC` and the registers are then only where the first
core happened to be, and R1, R6 and R12 say nothing about the loop.

| The report shows | It means |
| --- | --- |
| `R1 -  R6 -` and `R12 -  SP -` | the saved registers are not this crash's (not expected: every crash of the first core saves them; a fault of the second core retires it, `G 4` in the log, and makes no report) |
| `PC` in `rv_block` | not expected: with its end test and the cut, build 13's loop cannot reach a guard band. Its registers moved too (CI listing): n is reloaded into r0, k is in r6 (r7 in the other three variants) and the store's pointer in r5, so `R1`, `R6` and `R12` are not the loop's n, k and pointer here. Send the photo: the developer reads them with the listing |
| `SP` | the stack at the crash: about `01C7BE2C`..`01C7BE40` when the audio interrupt came from the main loop |
| another `PC` | the registers are that code's: look the PC up in `build-bt/symbols.txt` of the same CI run |

**Checked before flashing build 13** (CI run 43039170246, `rv_block` in `listings/px_dsp.s`, the compiler's assembly):

- the four loop heads each copy k through the empty asm and branch to one shared cut path with an unsigned
  `if (r0 >= 64)`;
- each loop's end test is `ifs (k < r0)` with n reloaded from `[sp+128]` on every sample (build 12's was an equality), and
  the cut path records n from that same word and the k it compared;
- the frame is build 12's: `sp += -192`, n at `[sp+128]`, k saved at `[sp+88]` around the call to `rv_front`;
- the registers moved: n reloads into r0, k is in r6 (r7 in the other variants) and the store's pointer in r5. `rn` in
  `firmware/openpocket.c` was left at r1, r6 and r12: with this end test the loop cannot reach a guard band, so a crash
  report from build 13 is about some other code, and its registers are read with that code's listing.

**Again for build 14**: v42 puts `rv_front` inside `rv_block`, so its code and frame change. Before flashing, check the same
points in build 14's `listings/px_dsp.s`: the loop heads' unsigned compare and shared cut path, the end test `k < n` reloading
n on every sample, the frame, and which registers hold n, k and the store's pointer.

## Test build 14

The same image on OpenPocket v42 (v41: the NAM network split between the cores by a worker that stays on the second core,
the NAM's hand routines rdm6, rdm15 and post5; v42: the pedal's controls and screens, the audio callbacks' order, USB audio,
which is not in this image). What changed for Bluetooth:

1. **The crackles' cause, fixed as v42 fixes it.** v42 found the same fault from the other side (`firmware/px_aorder.h`): the
   10 kHz timer, above the audio interrupt, makes it enter late, both callbacks are due, and the output's runs first. An output
   block asked for before its input's is now filled as soon as that input's block is made, in the same interrupt, so the
   block goes out with no latency more. Builds 12 and 13's two slots (one block, 2.9 ms, more latency) are gone, and `OC` in
   the isr line counts these blocks. What can still click is the time between the two callbacks of one entry, where the
   radio's interrupts and the 10 kHz timer, above the audio's, can run for longer than v42 measured: an input's callback that
   comes later than the window after its output's (`px_audio.h A_ORDER_TICKS`: half a block, 1,451 µs, in this image; v42's
   725 µs elsewhere) is taken for another entry's, and that output block is left as it was (a click). `OD` counts those,
   `OG` the longest that time got (`AI` is the whole stay, the DSP included, and does not show it). Two costs of filling the
   block in its own entry, which builds 12 and 13 did not have: in every period the audio interrupt waits there for the
   second core's block (its job or the layer split's blocks after the amp), so the main loop, which decodes the music and runs
   the Bluetooth tasks, gets less of the first core's time; and the block must be ready within the period, the entry's
   lateness included, where the slots had a whole period more.
2. **The second core's worker.** While a NAM preset plays, v41's layer split keeps one job running on the second core. The
   radio's start-up burst and the flash reads (`ota_fread`), which switch the flash path off, waited for the second core's job
   to end; this one never ends, and that wait would retire the core after 8 ms (`G 2`, then `BYP` on NAM presets until
   power-off). They now wait until the worker idles in its loop (RAM code) with nothing left to do (`px_audio.h
   a_cpu1_quiet`), within the same 8 ms. A core retired there (it never idled, or it faulted) is collected there at once, as
   v42's own retirements are: one core from then on, the engine reset (15 to 50 ms of silence, `NAN`), and `G` keeps why
   (`4` for a fault). Left to the next audio interrupt, that interrupt would have handed network blocks to the stopped core,
   waited 8 ms for them, and retired it again as `G 2`.
3. **The library folded at build time** (`tools/jlfold.py fold`, the folding probe's variant B): the configuration constants
   the firmware gives it folded in, the print helpers the firmware leaves empty dropped: 94.7 KB less flash. v41's and v42's
   firmware does not fit the app slot without it. The code is JieLi's IR compiled again with its constants known, not the
   published objects: pairing and the music must be checked again. In the container, `OP_BT=1 OP_BT_FOLD=0 sh
   firmware/build.sh` builds the library as before (the image then does not fit). The probe still runs: its `B` must give
   "the real image's bytes".
4. **Memory.** v40.4 moved the engine's buffers (14.6 KB) to the SRAM below the RAM, where this image keeps the baseband's
   table and the receive pool: here they stay in the RAM code window, as in builds 12 and 13 (`firmware/bt/app-bt.ld`). To make
   that room, the NAM's hot routines (rdm6, rdm15, post5) run from the flash, as in the release (the BLE image keeps them in
   RAM, `firmware/build.sh PXN_HOT_RAM`). How they run from the flash with the radio's code also running from it is not
   measured: compare `LD` and `PK` of a NAM preset with build 13's.
5. **v42's screens**: `LAST START FAILED`, and an overload shows a red banner. The status area still shows the Bluetooth state
   (scrolling while visible or linked); v42's load text replaces it only after a late block (`DSP 88%!`) or in bypass, and
   beside `BT OFF` or `BT START` it is cut off: the isr line has `LD` and `C1`. HOME on the play screen opens the preset list;
   HOME held opens the tuner, then the diagnostics; both footswitches together open the tuner. A new id for the second core's
   bar, so a bar left by build 13 does not carry over.
6. **The visible line's `V`** shows the BLE image's flags in full (`F` stays `F`): v41 gives a pedal whose VM 106 or 107 is
   missing the Pocket X backup's values instead of zeros (`10`, `20`), so the crystal trim beside it may be that default.

### What to check

1. "What you should see", steps 1 to 5, with the phone that paired with build 13 (forget the pedal there first): the folded
   library pairs and plays.
2. The crackles: with the music playing, the guitar through CHURCH ORGAN and a heavy NAM preset. `OC` rising (beyond one for
   each preset change and flash write) with `OD` at 0 and no clicks is the fix working, and `OG` says how close those entries
   came to the window (1,451 µs); with clicks, the isr line (`OD`, `OG`, `40`, `41`, `T5`, `XR`) while they happen. Clicks with
   `OD` 0 and `XR` 0 point at a block not ready within its period: build 13's two slots are the way back.
3. The load: `node tools/op.mjs dsp --presets 1,14,18,19,20,21,24 --seconds 8` with the music playing and paused, against build
   13's figures (the NAM routines from the flash, item 4).
4. The music's decoding: with the music playing for a few minutes, the link line's `UND` and `BUF` on a heavy preset without NAM
   (`C1 1`, a high `LD`) and on a heavy NAM preset, against build 13. `UND` rising where build 13's stayed at 0 is the first
   core's new wait in each period (item 1) leaving the main loop too little time.
5. The crash at the radio's start-up (about 1.96 s): the same 15 boots as build 13 ("What to do"), with `W` and `G` of each.
   Build 12's record showed the second core trapping at that moment in every Bluetooth boot; v41's worker keeps it busy through
   the whole start-up on a NAM preset. Before flashing, check `rv_block` in the listings as for build 13 ("Reading the
   registers").

## Checked on a computer

- `tests/test_bt_ring.c`: a phone at 44.1 kHz with 15 ms of jitter, the pedal at its real 44,117.6 Hz, 60 s: no underrun, the rate
  settles at the clocks' ratio, a 1 kHz sine comes out 52 dB clean, the pitch wanders 0.7 cents; a 300 ms pause stops it once,
  without a click, and it starts again.
- `tests/test_bt_sbc.c`: Android's encoder makes the frames a phone sends (joint stereo, 328 kb/s), packed as A2DP packets
  (contributors, an extension, padding): every frame decodes, the output is the mono mix 64 dB above the difference; mono
  streams, also with frames of 8 and 4 blocks (each gives its own count of samples); a frame whose CRC fails is refused and the
  stream goes on; a header whose contributors or extension run past the packet leaves nothing to decode.
- `tests/test_ble_taskq.c`, built twice (the BLE image's scheduler and the Bluetooth audio image's): the task queues with the
  library's message sizes, the recursive mutexes, the clock across TIMER4's wrap (a periodic timer, a one-shot and a task asleep
  across it), and in the audio image's build the waits refused inside the baseband's interrupt, each task's queue peak, stack
  depth and red zone.
- `tests/test_bt_lbuf.c`: the radio pools' allocator through 100,000 random operations (no overlap, no corruption, no leak).
- `tests/test_bt_sources.mjs`: the vendor objects are as published and listed, the licences are there.
- `tests/test_log.mjs`: the FIFO line and `W` named, and the nine lines classified and reported, as the log tool prints them.
- Test build 13's cut, by hand (it is not in `tests/run_tests.sh`): the engine tests built with `-DPX_LOOP_GUARD=1` print the
  same as without it and leave `W` at 0; `rv_block` called with an n above 64 stops at k 64 in each of its four loops, records n
  and k, and writes nothing past frame 63. With the guard off (the release and BLE images) `engine/px_dsp.c` preprocesses to
  the same text as before.
- Test build 14, by hand: the engine is v42's and the cut (`tests/engine/equiv.c`, 852 runs: the same output as v42's engine,
  with and without `-DPX_LOOP_GUARD=1`, `W` 0); the engine tests, plain and with the guard; `engine/px_dsp.c` and
  `firmware/openpocket.c` preprocessed for the release image as v42's (`px_dsp.c` byte for byte; `openpocket.c` apart from
  macros that compile to nothing: its optimised IR is v42's, function for function, now that test build 11's stall-watch
  exception to the second core's bar and power-on key record are built in the Bluetooth images only); `tools/jlfold.py fold`
  on the library's IR (the same 149 constants folded and 86 print helpers emptied as the probe in CI); v42's
  `tests/test_aorder.c` (the callbacks' order). A host model with the real `px_audio.h` and engine and a stand-in second core:
  `a_cpu1_quiet` returns at once for an idle worker, drains a request and its queued network blocks, retires a worker that
  never idles as `G 2` within 8 ms and a faulted core as `G 4`, and collects both (one core, nothing left queued, the next
  interrupt hands nothing over: before, it waited 8 ms and turned `G 4` into `G 2`); this image's callbacks' order fills an
  output-first block whose input comes up to 1,440 µs later and counts one at 1,460 µs in `OD`, with `OG` its time.
- The member set was chosen from the archives' symbol tables: what it references and does not define is supplied
  (`bt_glue.c`, `bt_cfg.c`) or stood in for (`bt_stubs.c`), with the library's own types.
