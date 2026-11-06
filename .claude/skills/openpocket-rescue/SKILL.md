---
name: openpocket-rescue
description: Rescue a Donner Pocket X / M-VAVE BlackBox that restarts in a loop or no longer boots, from a Mac with no extra hardware (make rescue, tools/uboot_rescue.py, over the chip's USB UBOOT boot state). Use when the installer's rescue mode cannot finish, the pedal loops after a bad image, or a flash left the app area broken. Covers when to use it, the safety rules, who runs the sudo write, and how to read the result.
---

# Rescue a looping or bricked pedal

The pedal's mask ROM always enters the USB boot state ("UBOOT": USB `4C4A:8057`, shown as `WL80UBOOT1.00`) when the app is bad,
and it cannot be damaged. `tools/uboot_rescue.py` waits for that state, loads JieLi's RAM loader and rewrites the app area from
the official package. Full walkthrough and the record of the first real run: `docs/RESCUE.md`.

## When to use it

- The pedal restarts in a loop and the installer page's rescue mode (or `node tools/op.mjs flash`) cannot finish. Try those first:
  they need no root.
- The pedal stays in the boot state (no loop, no sound) after an interrupted flash: the rescue finishes the job.
- Not for Bluetooth or firmware behaviour questions, and not on a pedal that works.

## Rules (read first)

1. **Read-only first.** `make rescue` writes nothing to the flash (the loader goes to RAM only) and saves a backup of the whole flash
   to `~/OpenPocket-rescue`. Read its output before anything else.
2. **The captain runs the write.** `make rescue-write` and every `sudo` command need the captain's password and instruction. An
   agent runs neither: prepare the exact command and give it with the `!` prefix.
3. The write range is only whole 4 KiB sectors in `[0x4000, end of the package)`, `[0x4000, 0xA3000)` for BlackBox. Never write
   the head `[0, 0x4000)` or anything above the package. The tool refuses; do not work around it.
4. Never chip erase (`FB02`) and never burn a key (`FC12`). The tool cannot send them; do not add them.
5. Stop on any `REFUSED` (wrong device, chip key not `0x980F`, flash not 1 MiB, head differs from the package). Nothing was written;
   report the message.
6. Keep the first backup. Never commit or publish a backup or a package: they hold the official firmware.
7. After a restore, do not reinstall the Bluetooth test image until the cause of the loop is known.

## How

```
brew install libusb          # once
make doctor                  # rescue lines: libusb, official package, loader, root
make rescue                  # read-only: INQUIRY, loader to RAM, key, flash id, backup, head check, sectors that differ
make rescue-write            # only on the captain's instruction, after the read-only run passed
```

Options go after `ARGS=` (`make rescue ARGS="--package FILE.fwsc"`; `--image flash.bin`, `--loader FILE`, `--backup-dir DIR`,
`--attempts N`). Plug the pedal straight into the Mac (no hub). Start the command first; it waits for the pedal and starts by
itself the moment it appears (the pedal is in UBOOT about 44 s of every 70 s while it loops). Needs the official BlackBox package
(default `~/Documents/BlackBox (2).fwsc`) and Python 3.

## Reading the result

| Output | Meaning |
| --- | --- |
| `INQUIRY: WL82 UBOOT1.00 1.00` | the right device |
| `loader running: chip key 0x980f, flash id ...  (1024 KiB)` | the checks passed; record the flash id |
| `backup saved: ...dump.bin` | the rollback copy is complete on disk |
| `head [0, 0x4000) matches the image` | the package fits this pedal |
| `N of 160 sectors ... differ` | what a write would do; a rerun after a partial write shows fewer |
| `Read-only run finished, nothing was written` | exit 0, the read-only run is done |
| `Done: N sectors written and verified` | exit 0 after `--write`; power-cycle the pedal |
| `the pedal went away ... waiting for it again` | the window closed; it resumes by itself |
| exit 1 `ERROR` | setup: libusb, `sudo`, missing package; the message names the fix |
| exit 2 `REFUSED` | a safety check stopped it, nothing written |
| `LIBUSB_ERROR_ACCESS` | run with `sudo` (`make rescue` does); if it persists as root, report it |

Save the output of a real run (`make rescue 2>&1 | tee rescue.log`) and add its numbers to `docs/RESCUE.md`.

## Tests

`make rescue-check` runs the tool against a simulated UBOOT pedal (protocol, guards, resume, no hardware); it is part of
`make test`. Hardware behaviour is not covered by any test.
