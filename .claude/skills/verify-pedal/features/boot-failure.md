# Boot failure report

The `LAST BOOT FAILED` screen, readable from the computer on the next boot.

## Sub-features

- `last boot` line: stage, reset cause, audio counters; after a crash, PC, RETS, vector, uptime.
- Safe mode: after a failed boot Bluetooth stays off for one boot (`this boot ... SAFE MODE`).

## How to get to it (user POV)

Nothing to do: it is there on the first boot that gets USB up after the failure.

## Driving it with tools/op.mjs

`node tools/op.mjs log --wait 60`. Look the addresses up in `firmware/build-ble/symbols.txt` of the same build.

## Gotchas

- Not yet seen on hardware: only the simulator has produced this line (`tests/test_log.mjs`). Do not break a pedal to test it.
- While the report or the red crash screen is up, USB is down: `--wait` rides that out; the crash itself cannot be streamed.
- Only one previous boot is kept. If the safe-mode boot fails as well, the line describes that boot.
