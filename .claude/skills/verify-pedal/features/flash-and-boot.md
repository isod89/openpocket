# Flash and boot

Write the test image, and see it start.

## Sub-features

- Flash over USB MIDI (`openpocket-flash` skill).
- Boot stages and the boot guard: `this boot` line (`STAGE`, `GUARD`, reset cause).

## How to get to it (user POV)

Pedal running OpenPocket, plugged in, no installer page open.

## Driving it with tools/op.mjs

Dry run, flash, then `node tools/op.mjs log --wait 40`. Proof: `done: the pedal reports BlackBox_936`, then
`this boot  pedal clock N s (wraps every 179 s), stage OK, normal`.

## Gotchas

- The version stays `v36` between test builds: only the package SHA-256 tells them apart.
- The boot after a flash shows reset by `POWER-ON`.
- The stage is `FIRST 6 S` until 6 s after start; the boot guard clears then.
