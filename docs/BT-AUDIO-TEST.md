# Bluetooth audio test image (A2DP: the phone's music through the pedal)

An optional build of OpenPocket that makes the pedal a **Bluetooth speaker**, as the stock BlackBox firmware is: it shows up on a
phone as **`OpenPocket`**, pairs with no PIN, and the phone's music plays **mixed into the pedal's output**, with the guitar.

It is a test image, its own build target (`make bt`). The normal firmware and the installer's packages are unchanged.

> **Status: test build 12.** Builds 1 to 10 ran on a pedal: the radio starts, a phone sees the pedal and links, but no phone
> could pair, and the guitar crackled. Build 11 paired an iPhone (below, "Test build 11"), but no music came out, and the guitar
> still crackled with Bluetooth on. Build 12 takes the phone's packets where the controller first shows them, sends the output
> in two slots against the crackles, and records the second core's fault (below, "Test build 12"). Every part that can be
> checked on a computer is checked (see "Checked on a computer").

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
2. **Connect**, **Install**. The start-up screen reads `v40 BT AUDIO TEST 12` (the number is the test build's).
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

**A/B keys, held while switching the pedal on, until the start-up screen has gone** (each for that boot only; the log's `c1`
line shows `KEY`, test build 12). Switch off for real (USB out too, 10 s) before each boot you compare:

| Key | That boot |
| --- | --- |
| **REV** | Bluetooth off (`BT OFF`): the same image with the radio off, to compare the sound with and without Bluetooth by ear |
| **DLY** | the output as test build 11 sent it, one slot (test build 12): with it the crackles of the callbacks' order changes come back, if they were the crackles |

(CAB held at power-on does nothing in test build 12: the c1 line only shows `KEY 1`. A CAB key for the stock firmware's supply
levels did not fit in the flash.)

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

`BT VIS IRQ 40:1234 41:56789 EV 4 RDY SCAN 1 V F BD 0B0B ADDR 024F1A2B3C4D UID 1 HEAP 9120 STUB 0 Q40 0 KICK 0 TX 0/592/2032 F0 RX 0/0/12272 F0`

| Field | Meaning |
| --- | --- |
| `IRQ 40` `41` | the classic baseband's interrupts (packets, the alarm) and the slot timers: they rise when the radio runs |
| `EV` | events from the stack; `RDY`: it said it is ready (`NORDY`: it did not, the pedal made itself visible anyway after 2 s) |
| `SCAN` | times "visible and connectable" was sent to the controller |
| `V` | the stored radio calibration: found records (`F` = all four), the VM area (flash / 4096), the crystal trim (as the BLE image) |
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
travels through the tasks' queues: the newest 11 messages (test builds 9 to 11 also had `hci0`, the first 12 since boot; the
a2dp line took its place in build 12). `n` all messages posted to a task, `ACL` data packets to the host and `NCP` Number of Completed Packets events
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

### The isr line (test builds 11, 12)

`ISR 40:12/310/1450 41:4/40/90 T5 9/31 AG 2144/2935 AI 846/942 EN 0/344/344/0/0 OC 0/0 3/3 C1 1 G 0 NAM 1 LD 42 PK 55/97 XR 3 CL 0 WC 0 BYP 0 TM 1234 VM 4/2 OVF 0 NAN 0`

What the interrupts cost the audio, and the DSP's state, for the crackles. Times are in microseconds.

| Field | Meaning |
| --- | --- |
| `40:a/b/c` `41:a/b/c` | the classic baseband's two interrupts: the longest in the last second, the longest since boot, and the time they took in all in the last second. They run above the audio |
| `T5 a/b` | the 10 kHz timer (USB, the stall watch), also above the audio: the longest in the last second / since boot |
| `AG a/b` | the longest gap between two audio interrupt entries (last second / since boot). 2,902 when one entry serves a period; less (2,144 in build 11's log): two entries a period, the input's and then the output's, so AG is the period less the input's stay; more: an entry came late. A preset change or a flash write is not counted |
| `AI a/b` | the audio interrupt's longest stay (last second / since boot) |
| `EN a/b/c/d/e` | (test build 12) the audio interrupt's entries in the last second, by the flags they found: none / the output's only / the input's only / both / another channel's. About 344 a second each is two entries a period; `d` counts the periods whose output callback ran first; `e` must be 0 |
| `OC a/b c/d` | (test build 12) the callbacks' order changes in the last second and since boot: two output callbacks in a row / two input ones. Up to test build 11 each one repeated a 2.9 ms block or skipped one: a click on any sounding note. With the two slots they cost nothing (and with DLY at power-on they click again). Each full preset change and each flash write (the audio interrupt held) adds one of each by itself |
| `C1` | 1: the second core runs; 0: the whole DSP is on one core (heavy presets then go to `BYP`) |
| `G` | why the second core is off: 0 it is not; 1 it misread shared memory, 2 a job was not back within 8 ms, 4 it faulted (1, 2 and 4: retired in this boot, and barred at every start until a power-off); 3 barred at start (by such a retirement, or by a crash of the first core while it ran, in an earlier boot of the same build); 5 it did not answer at start. The c1 line has the rest |
| `NAM` | 1: the preset's NAM amp is one that runs on both cores; 0: not (test build 12: whatever `C1` is) |
| `LD` `PK a/b` | the DSP load, and its peak since the last full preset change / since boot, % of a block's time |
| `XR` `CL` `WC` `BYP` | late blocks (each one a likely crackle), clipped blocks, the delay and reverb lane's cut blocks (clicks in the tails; since build 12 kept across a retirement or a reset), bypassed |
| `TM` `VM r/w` `OVF` | the library's software timers that fired, the pairing store's reads and writes, task stacks that nearly overflowed |
| `NAN` | (test build 12) the engine's resets after a NaN, since boot: each one 15 to 50 ms of silence |

### The a2dp line (test build 12)

`A2` and 23 words in hex: the phone's packets as the controller shows them to the A2DP code (`a2dp_media_packet_user_handler`),
and the controller's own A2DP state for each one. `node tools/op.mjs log` prints them a second time with their names. In test
build 11 the controller kept only the first packet of each session for the pedal and sent every later one to the host, which
plays nothing: these words say why.

| Word | Meaning |
| --- | --- |
| `n` | packets the hook saw (about 70 a second while the music plays; 0 with music playing: the hook did not link) |
| `kept` | packets the controller itself kept for the pedal (0: its keep test never passed) |
| `hc` | the last packet's handle and L2CAP CID (`0030:0041` or so) |
| `B` `Bn` `Bst` `Bt` | packets found with the receiving connection's state wrong for its keep test, the first one's number in its session (1: the first after MEDIA_START), its state word and its time (ms) |
| `O0` `Ot` | the first connection's state at the first MEDIA_START, and when (expected `10cccc01`-like, e.g. `10004101` for CID 0041: a handle, the CID, open) |
| `CHM` | packets received on a connection other than the one the host opened (expected 0) |
| `G` `Gt` `F` `Ft` | the open connection's state just before the pedal's first give-back of a library packet and just after it, and when |
| `N` | its state now |
| `RC` | the last "get" from the library: 0 nothing waiting, `FFFFFFEA` closed, no link, detaching or suspended |
| `H` | heals: the two fields written back when found wrong, on the connection the host opened only (then `kept` should rise, if the state was all that was wrong) |
| `W` `W0` `W1` `W2` | the 10 kHz watch over both connections while playing: changes seen, then the first one's time << 1 \| connection, its new state, and the code the interrupts found running (look it up in `build-bt/symbols.txt`) |
| `DROP` | packets the FIFO could not take: full / too big |
| `DUP` | packets the library handed the pedal, given back undecoded (the FIFO already has them) |

A state word: bits 0-7 the channel-open byte, 8-23 the CID the controller expects, 24 a clear-up pending, 25 a detach asked,
26 suspended, 27 detaching, 28 the connection has a handle. The music plays from the FIFO whatever these say.

### The c1 line (test build 12)

`C1 1 G 0@0 K 2232 KEY 0 S 00000000 F 0/0`, and after a fault of the second core 19 more words in hex.

| Field | Meaning |
| --- | --- |
| `C1` `G g@t` | the second core runs; why it is off (as in the isr line) and when it was retired (ms; wraps every 179 s) |
| `K` | how deep the second core's stack has gone, in bytes (of 2,816) |
| `KEY` | the key held at power-on: 0 none, 1 CAB (nothing in this build), 2 DLY, 3 REV |
| `S` | where Bluetooth was when the main loop first saw a fault: stage << 28 \| state << 24 \| DSP held << 23 \| 1 << 22 \| IRQ 40's count |
| `F n/c` | the second core's faults, and which core took the exception (0: the first, the usual case) |
| then | DBG (the debug unit's message: bits 16-18 a bus error, 10 a PC limit), the taking core's EMU_MSG, PC, RETS (the first core's when it took the exception), the second core's own EMU_MSG, the time (ms), the job handed and the job finished (different: one was running), its function, and the debug unit's ten words from `0x1EEE358` (the bus masters' ids of an invalid access, the write-limit error counts) |

## Please report

- What happened at each step of the table, the phone (model, iOS / Android version), and a photo of the status area at the
  furthest step reached (or of `FAILED ...`).
- `node tools/op.mjs log --follow` while you try it (USB cable): it reads the same lines over USB, so no one has to read the
  scrolling text. Send its output.
- With the music playing: `node tools/op.mjs dsp --presets 1,14,18,19,20,21,24 --seconds 8`, and the same with the music
  paused. The question this image is for: does a streaming radio push heavy presets into `BYP` or late blocks?
- By ear: clicks or gaps in the music (and whether `UND` rises), hum or buzz in the guitar sound while the music plays.
- For crackles: the `isr` line while they happen, and the same preset with Bluetooth off (REV held at power-on) and with DLY
  held (test build 12: build 11's output). If they are there with Bluetooth off too, the radio is not the cause. If they come
  only with Bluetooth on and nothing is playing (a high-gain preset, the guitar's volume at zero), ticks at the scans' rhythm
  point to the radio's supply or RF reaching the analog side, which no firmware change can fix.
- After any boot whose c1 line shows `G 4`: the c1 line as it is (its words, looked up in `build-bt/symbols.txt` of the same CI
  run, say where the second core faulted). A red crash screen: a photo of it (PC, RETS, VEC, DBG), before switching off.

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
  the mix buffer above it, the pairing store (672 bytes of RAM), the FIFO (10.8 KB of the Bluetooth heap), the output's two slots
  (1 KB in the RAM code window's free end).
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
   writes them back when wrong (`H`), and watches them 10,000 times a second, to settle the cause.
2. **Crackles.** The audio interrupt's driver (the SDK's) calls the output callback and the input-and-DSP callback each when it
   finds its own flag up. Build 11's isr line shows two entries a period, input first (`AG 2144` = the period less the DSP's
   stay), and seconds where the order flipped (`AG` just under the period): a few microseconds of extra latency (the radio's
   interrupts, a cold cache line) flip it. With one output buffer each flip played one 2.9 ms block twice and skipped the
   next: a step on any sounding note, with no late block, more often with Bluetooth on. The output now has two slots: the
   output callback always sends the block before the newest, whatever the order (2.9 ms more latency). `EN` and `OC` in the
   isr line count the entries and the flips; DLY at power-on brings build 11's single buffer back, to confirm by ear.
3. **The second core's fault.** It faulted (`G 4`) in every Bluetooth-on boot of build 11 and never with Bluetooth off. Its
   record is now kept and printed (the c1 line): the debug unit's message and bus-master ids, CPU1's own exception word, the
   time, the job, the stack's depth, and where Bluetooth was. (A CAB key for the stock firmware's supply levels, the cheap test
   of the "misreads under load" the board's notes describe, did not fit in the flash: the record decides whether it is needed.)

Also: Bluetooth's flash reads (the radio records, the address) run before the audio starts, and the radio's start-up burst and
the flash reads wait for the second core's job in flight (the flash path is off meanwhile); `WC` survives a retirement; the
log's ninth line; the build checks the library's static tables are where the a2dp line reads them, and keeps the compiled
controller code (`listings/` in the artifact) to compare with the library's IR.

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
- The member set was chosen from the archives' symbol tables: what it references and does not define is supplied
  (`bt_glue.c`, `bt_cfg.c`) or stood in for (`bt_stubs.c`), with the library's own types.
