# OpenPocket feature map

| File | Feature | Driver |
| --- | --- | --- |
| [build-and-packages.md](build-and-packages.md) | Firmware build, update loader, `.fwsc` packages, `manifest.json` | `op.mjs package` |
| [installer-page.md](installer-page.md) | Web installer `docs/index.html`: connect, detect, install, rescue | `pages.sh`; by hand: `op.mjs sim --update` |
| [editor-page.md](editor-page.md) | Web editor `docs/editor.html`: presets, sound, models and their library, TONE3000, MIDI controls | `pages.sh`; by hand: `op.mjs sim` |
| [screen-ui.md](screen-ui.md) | Pedal screen pages (play, edit, save, tuner) | `uisim` via `op.mjs test`; live: `op.mjs sim` |
| [base-firmware.md](base-firmware.md) | The base firmware and the products on it (`firmware/base/`, `firmware/apps/starter`): the release pin, an alternative firmware's build, simulator, install and way back | `op.mjs all`; by hand: `op.mjs sim --app starter`, `pages.sh` |
| [midi-usb.md](midi-usb.md) | USB-MIDI: update/editor SysEx today; musical MIDI with the MIDI feature | simulators; hardware by the captain |
