# USB-MIDI

## Sub-features
Today: the pedal enumerates as a class-compliant USB-MIDI device; its SysEx carries the installer's update protocol and the editor protocol. Musical messages (Program Change, Control Change) are added by the MIDI feature: see `docs/MIDI.md` when it exists.

## How to get to it (user POV)
Plug the pedal's USB-C into a computer; a DAW or the browser sees a MIDI port.

## Driving it without hardware
`node tools/op.mjs test` (devsim and edsim run the SysEx paths); once the MIDI feature is in, its simulator test (`tests/`) covers the parser and CC map. The pages scripts cover the SysEx clients.

Any SysEx by hand: `curl -s -X POST $U/api/sysex -d 'f0 7d 4f 50 01 f7'` against `node tools/op.mjs sim` (the editor protocol, answered by the real `px_editor.h`); `POST /api/midi` plus `GET /api/events` for unanswered or pushed messages (WATCH).

## Only a person with a pedal can verify
Enumeration on a real host, a DAW sending PC/CC, the pedal answering, audio while MIDI flows, the update still working. Use `docs/MIDI-TEST.md` when it exists; do not claim these from simulators.

## Gotchas
- Never change `usb.c` descriptors or `ota.c` without the captain: they guard the update entry.
