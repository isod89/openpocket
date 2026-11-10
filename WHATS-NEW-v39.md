# What's new in OpenPocket v39: the delay and the reverb off the critical path

Taken from the stock BlackBox firmware ([STOCK-FIRMWARE.md](STOCK-FIRMWARE.md), section 7.3): there, the dry sound is
made in the audio interrupt and the delay and the reverb run beside it, in a task of their own, one block late. A late
echo is never heard; a late block of the whole sound is a crackle.

## The wet lanes

- **Until v38** the delay and the reverb ran inside the chain, in series: every block had to finish them before the next
  interrupt, like the amp. On a heavy preset they could push a block past its 2.9 ms: a crackle of the whole sound.
- **Now** (with the second core on, which is always the case unless it has failed) they each have a **lane**. At its
  place in the chain the delay (or the reverb) only sends the signal to its lane and mixes in the lane's wet sound of two
  blocks before; the lane makes that wet sound **beside the chain**:
  - **without a NAM amp** the second core runs the chain, and the first core runs the lanes at the same time;
  - **with a NAM amp** (both cores share the network) the second core runs the lanes first in its share, where the delay
    and the reverb ran in v38; the network's sharing between the cores takes their time into account as before.

  **Fixed after the first v39 build** (the delay and the reverb whistled): with a NAM amp the lanes ran after the block,
  in the time left before the next interrupt. There was too little of it (the network shares its work between the cores,
  so the lanes' time only half came back to that gap): they were cut short every block, and the echoes and the reverb
  came out chopped 344 times a second, a whistle. They now run in the second core's share again. To put the delay and
  the reverb back in the chain exactly as in v38, build with `OP_WET_LANES 0` (`firmware/px_audio.h`).
- **The echoes keep their time to the sample.** The delay's taps that are heard read 256 frames (5.8 ms) less far back,
  the ones fed back read as before: the first echo and the spacing of the repeats are the same as in v38, bit for bit on
  the digital and dual delays (`tests/engine/wet_test.c`). On the modulated delays the wobble is the same, 5.8 ms
  later.
- **The reverb starts 5.8 ms later**, as if it had 5.8 ms more pre-delay: its tail is otherwise the same, sample for
  sample (also checked by the test).
- **Nothing else changes in the sound.** Without the second core (if it has failed) the delay and the reverb run in the
  chain, as before.

## On the tuner page

- The footer's CPU figures for DLY and REV are now their lanes' time, spent beside the chain (on the other core, or after
  the block).
- **`CUT n`** appears after them once a lane has had to stop short (without a NAM amp only, if the lanes ever reach 85 %
  of a block on the first core): n wet blocks cut since the start. It should stay absent; a photo of that page with
  `LATE` and `CUT` says where the time goes.

## For developers

- Engine (`engine/px_dsp.h`): `px_wet_mode`, `px_wet_swap`, `px_wet_run`; the state in `px_engine.wl_*`, the buffers
  (2 KB of RAM) in `px_dsp.c` (`px_wl`). Off by default: the tests, the simulator and a one-core pedal run the delay and
  the reverb inline, with the same output as v38 bit for bit.
- The delay and reverb loops now write only their wet part (`dly_block`, `rv_block`); the chain adds the dry part.
- Firmware (`firmware/px_audio.h`): `a_enc` swaps the lanes after collecting the previous block and, without a NAM amp,
  runs them on CPU0 beside the chain; with a NAM amp `post_job` runs them first; `a_c1_off` turns them off with the
  second core; `OP_WET_LANES 0` turns them off altogether.
- New test: `tests/engine/wet_test.c` (in `tests/run_tests.sh`).
