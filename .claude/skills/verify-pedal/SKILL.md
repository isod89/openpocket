---
name: verify-pedal
description: Verify an OpenPocket build on the real, USB-connected pedal without a person reading its screen: flash the Bluetooth test image, wait for boot, read boot state, last-boot report and radio counters over USB MIDI (node tools/op.mjs log), run the Mac Bluetooth scan, and report pass or fail with the reason in words. Use after a firmware/ble or boot change when a pedal is attached. For checks without a pedal, use the verify skill.
---

# Verify on the real pedal

`node tools/op.mjs log` asks the pedal for the text its screen shows (the `DIAG` request, read only) and judges it. Only the
Bluetooth test image answers `DIAG`; the release firmware does not. The pedal is one shared device: one driver at a time.

## Launch

The pedal must run the build under test. Follow the `openpocket-flash` skill: its rules on authority apply, and
`--confirm-flash` never goes in a script. One flash per attempt, no sudo.

```
npm ci --prefix tools                                                         # once
node tools/op.mjs ble                                                         # build/ble/*.fwsc, manifest.json
node tools/op.mjs flash --dry-run --manifest build/ble/manifest.json
node tools/op.mjs flash --confirm-flash --manifest build/ble/manifest.json    # ends with "done: the pedal reports ..."
```

Ready means the log says stage `OK`, about 7 s after start. `--wait` below waits for that. A pedal that restarts again
and again is a crash loop: stop, do not flash again, and report (see `features/boot-failure.md`).

## Doctor

`node tools/op.mjs log --count 1` is read only and says how far the pedal answers. Exit 2 means the MIDI library is missing.
Check first that no other agent is flashing: `ps ax | grep -E 'op.mjs (flash|log)'`.

## Drive

```
node tools/op.mjs log --wait 40 --scan 30       # the whole check: two readings, the scan, a third reading, the verdict
node tools/op.mjs log --wait 40                 # state only, no scan
node tools/op.mjs log --follow --interval 2     # watch it; add --json for one object per reading
```

| Verdict | Exit | Meaning |
| --- | --- | --- |
| `PASS` | 0 | the Mac saw the advertisement |
| `RUNNING` | 0 | radio events rise, no scan was run: not proof of packets on the air |
| `FAIL` | 1 | the reason says which step, or that nothing was seen |
| `PENDING`, `UNKNOWN` | 3 | not decided: still starting, scanner unusable, or less could be read (below) |

Report the verdict line as it stands. Field meanings: `docs/BLE-TEST.md`, and printed by the command.

## What it cannot read

| Pedal state | What the log gives | Fallback |
| --- | --- | --- |
| red `CRASH` screen, or the 12 s `LAST BOOT FAILED` report | no port, or a silent port, while it shows | read again after it: the next boot gives the same report as `last boot`; a screen photo meanwhile |
| main loop stuck, firmware failed before USB service | port there, nothing answers | screen photo; power cycle or the UBOOT rescue |
| boot loader, update mode | identity only, or no port | finish the update; `openpocket-flash` skill |
| release firmware, or a test image without `DIAG` | version and DSP load, says the rest is missing | screen photo |
| stock firmware | identity only | nothing to read |

The log never shows sound, footswitches or the drawing on the screen.

## Evidence

Save the output with `tee` to a folder outside the repository, with the package SHA-256 from the flash log. Proof standards:
the flash log, the `log --wait 40 --scan 30` output and the verdict, from the real pedal. The scan listens only; it never
connects or pairs. `RUNNING` is not `PASS`. `r` stays 0 with a Mac scan, so it says nothing here. A boot-failure claim needs
the `last boot` line from the real pedal, not the simulator.

## Cleanup

Nothing stays running; stop `--follow` with Ctrl-C. Restore the release firmware only when the task says so:
`node tools/op.mjs flash --confirm-flash` after `node tools/op.mjs package`. `build/ble-scan` is rebuilt when missing.
Evidence stays in the folder you chose.

Features: `features/README.md`. Keep them true with `/maintain-verification-skill`.
