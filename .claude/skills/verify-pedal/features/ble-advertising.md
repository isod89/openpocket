# Bluetooth advertising

The test image sends `OpenPocket-BLE-TEST` with the BLE-MIDI service UUID, at 100 ms, not connectable.

## Sub-features

- Controller start-up: `STARTING n`, `FAILED s c`, `ADV ...`.
- Radio counters: `i` about 10 a second is the advertising rate; `t`, `s`, and `V` (stored radio records) are reported as notes.
- The Mac scan: `tools/ble-scan.swift`, built into `build/ble-scan` on first use.

## How to get to it (user POV)

Pedal running the test image, within a few metres of the Mac, Bluetooth on.

## Driving it with tools/op.mjs

`node tools/op.mjs log --wait 40 --scan 30`: 30 s filtered on `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`, then 30 s unfiltered,
matching the name or the UUID. Proof: `verdict    PASS`, with the name and RSSI.

## Gotchas

- `others` above 0 is what shows the scanner works when the target is not seen; a scanner with no authorization gives `UNKNOWN`.
- The first run may ask macOS for Bluetooth permission for the terminal.
- The name is in the scan response, the UUID in the advertising data: a sighting may carry only one.
- `ADV` only means the controller accepted the command.
