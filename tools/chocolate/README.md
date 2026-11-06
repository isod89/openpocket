# M-VAVE Chocolate Plus setup

`fcp.py` turns your own CubeSuite export into a file that makes a Chocolate Plus switch the pedal's effects over Bluetooth.
It is Python 3 with no dependencies. The pedal needs the Bluetooth test image (`make ble`, `docs/BLE-TEST.md`), with its MIDI
CHANNEL set to OMNI or 2.

| Mapping | Mode | Footswitches | Status |
| --- | --- | --- | --- |
| `today.json` | Mode 4 | A DRIVE, B MOD, C DELAY, D REVERB, alternating CC 24, 28, 29, 30 between 127 and 0 | Sent these messages to a Mac over Bluetooth |
| `final.json` | Mode 5 | Group 1 as above, group 2 A SFX, B PITCH, as toggles (CC 8 to 19, `docs/MIDI.md`) | Untested as a whole |

A Chocolate also changed DRIVE, MOD, DELAY and REVERB on a real pedal. The pedal's log keeps only the last message, so that
test does not show which file the Chocolate was running.

`today.json` can drift out of step: each switch remembers its own side, so a preset change can make a tap appear to do
nothing. On the one Chocolate tested, the first tap sent 0 and the second 127, the reverse of the file. To swap them, exchange
`first` and `second`. The toggles in `final.json` do not drift.

## The file format

This section records what is understood, from CubeSuite's code and from one Chocolate's exports.

An `.fcp` is a raw copy of 23,646 bytes of the Chocolate's memory. It has no header, length field or checksum, and CubeSuite
writes it to the Chocolate without checking it.

- Byte 0 is the mode, byte 1 the interface, and byte 2 the channel minus 1.
- Each message is 5 bytes: enabled, channel minus 1, type, data 1, data 2.
- From 0x5d there are 4 Mode 4 records of 561 bytes, then 64 Mode 5 records of 281 bytes, ending at 0x4f61.
- The rest, 3,325 bytes, appears to be live RAM. It holds pointers and the Chocolate's Bluetooth advertising data.

The Mode 4 layout is proven: all four keys sent their records' messages. The Mode 5 mode byte and record order are inferred, so
`final.json` may select the wrong mode. It is not known where Mode 5 stores the group count.

**Your export is private.** It holds your Chocolate's memory image, including its Bluetooth data, so do not publish it or
anything built from it. The repository ignores `*.fcp`.

## Export from CubeSuite

1. Connect the Chocolate by USB and turn it on.
2. Quit CubeSuite fully (Cmd-Q on a Mac), then start it again.
3. Open the Chocolate page and check that it shows real settings. A failed read shows no error.
4. Click **Export** and save the file as `export.fcp`. Keep this file, because it is your way back.

A failed read still exports a file of the right size, filled with CubeSuite's own memory. `fcp.py` refuses such a file.

## Generate and import

    python3 fcp.py decode export.fcp
    python3 fcp.py check export.fcp
    python3 fcp.py build today.json export.fcp today.fcp
    python3 fcp.py diff export.fcp today.fcp

`diff` lists every changed byte. Below 0x4f61 it must show only the mode and the records of the keys in your mapping. Then:

1. Click **Import** in CubeSuite and choose `today.fcp`.
2. Do not click anything else on that page. Every control writes to the Chocolate at once, using the wrong layout.
3. Power-cycle the Chocolate, set its switch to H, and test the footswitches.

The one import tested showed "Write data failed !" twice and then "Import success!", and the settings were stored anyway.
"Import success!" appears whatever the result. The real test is at the footswitches, or a fresh export compared with `diff`.

**CubeSuite shows some fields wrongly.** It expects a different layout, so after an import the bank lists look empty, and
`final.json` should appear as Manufacturer control. The "A Bank" lines it shows are the Bluetooth advertising bytes, not
settings. Do not treat what it shows as a failure.

## Restore

Click **Import** and choose your original `export.fcp`. It uses the same path, so expect the same messages, and check the
result with a fresh export. A restore to a saved export was not itself tested.

## Risk

Import sends the whole file, including the bytes after 0x4f61. Those bytes are a snapshot of RAM from the moment of the
export, and it is not known whether the Chocolate ignores them. The last few survived a power cycle after the import, so some
may be stored settings. `fcp.py` carries them through unchanged, so a built file carries the same risk as importing any
export. The likely worst case is a hang until power-off, but that is an inference and has not been tested.

The layout comes from one Chocolate. If `decode` shows nonsense for your export, your firmware may differ, so do not import.
