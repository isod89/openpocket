# Credits and licences

OpenPocket is GPL-3.0-only (`LICENSE`), except the parts below, which keep their own licences.

| Part | Origin | Licence |
| --- | --- | --- |
| `firmware/bsp/` (chip support: startup, interrupts, second core, flash, USB-MIDI device, M-UPGRADE update entry `ota.c`) | [felucca-bsp](https://github.com/hugelton/felucca-bsp), Leo Kuroshita / Hügelton Instruments and contributors. `chip/ota.c` changed for OpenPocket (identity read at run time). `chip/usb.c` changed for OpenPocket's USB audio (v42): the audio streams switched at run time (`FELUCCA_UAC_SWITCH`), the record stream's packet pacing of felucca-bsp `e79a1f8`, the playback mix in integers, fewer controller reads a poll. | MPL-2.0 (`LICENSES/MPL-2.0.txt`) |
| `loader/` (update loader) | felucca-bsp `loader/`, changed for the Pocket X flash layout | MPL-2.0 |
| `docs/pxota.js`, `docs/pxpkg.js`, `tools/pxpkg.py`, `tools/mkloader.py`, `tools/lz4blk.py` | adapted from [Felucca](https://github.com/hugelton/Felucca)'s web installer and build tools (as used by CLIPPER and SLOOP) | GPL-3.0 |
| `firmware/sdk/` (I2S and DAC drivers as LLVM bitcode, SPL, cfg files) | JieLi AC79 SDK V1.2.0 (fw-AC79_AIoT_SDK) | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| `firmware/px_cacheram.h` (data-cache ways as RAM, for the NAM network's vector-unit buffers) | [melodee](https://github.com/keremimo/melodee) `firmware/hal/fm1_cache.h`, Kerem Kilic (Ellic Studio), after the AC79 SDK's cache way setup | GPL-3.0 |
| `third_party/jieli-ble/` (JieLi's Bluetooth LE controller, 33 objects of `btctrler.a` and 3 of `cpu.a`, as published LLVM bitcode, used only by the Bluetooth test image `make ble`) | JieLi AC79 SDK V1.2.0 (fw-AC79_AIoT_SDK, commit `0617f4c`), `cpu/wl82/liba/`; see `third_party/jieli-ble/README.md` | Apache-2.0 (`third_party/jieli-ble/LICENSE-JieLi-Apache-2.0`, `LICENSES/Apache-2.0.txt`) |
| `firmware/ble/ble_cfg.c` | values and names from the same SDK's `apps/common/config/log_config/lib_btctrler_config.c` | Apache-2.0 |
| `firmware/fonts.h` (bitmaps generated from Inter) | [Inter](https://rsms.me/inter/), Rasmus Andersson | SIL OFL 1.1 (`LICENSES/OFL-1.1-Inter.txt`) |
| `assets/loops/*.wav` (guitar loops for the simulator) | cut from [Guitar-TECHS](https://zenodo.org/records/14963133), credits in `assets/loops/ATTRIBUTION.md` | CC BY 4.0 |

The factory NAM models (`engine/px_nam_factory.h`, v37) are A2-Lite networks trained by OpenPocket from Neural Amp
Modeler community captures ([pelennor2170/NAM_models](https://github.com/pelennor2170/NAM_models), GPL v3; authors
in `docs/NAM.md`) played with DI guitar from [Alec-Wright/Automated-GuitarAmpModelling](https://github.com/Alec-Wright/Automated-GuitarAmpModelling)
(GPL v3). The NAM A2 architecture is from Neural Amp Modeler (Steven Atkinson, MIT); `engine/px_nam.c`,
`tools/nam_a2.py` and `tools/train_a2.py` are written for OpenPocket from the published architecture.

Contributors: **Vince Speelman ([VinSpee](https://github.com/VinSpee))**: the simulator's WebAssembly engine and its sound, the
command-line flasher, MIDI, the Bluetooth test image (the radio's fix, a connectable BLE-MIDI peripheral, the Chocolate
Plus test, `op.mjs log`), and v41's two-core NAM work (the network split by layers, the `rdm` / `post4` / `post5` routines, 24 KB of RAM
freed, the faster preset switches with LOADING, PING's DSP health and `tools/dsp.mjs`, the pedal screen's quick wins;
pull request #22), and v42's web editor in four sections (#23), the pedal's reworked controls and diagnostics pages
(#24) and USB audio (#25).
**Ivan Shevchuk ([Testing7B](https://github.com/Testing7B))**: v42's check and restore of the BlackBox V21 factory
sounds (pull request #26).

Sound models: the names refer to the gear that inspired each voicing (circuit analyses published by ElectroSmash,
Electric Druid, R.G. Keen / GEOFEX, Rob Robinette, Duncan's Tone Stack Calculator; speaker data sheets; manufacturers'
manuals). All trademarks belong to their owners; OpenPocket is not affiliated with Donner, M-VAVE or any gear maker.

No part of the official Pocket X / BlackBox firmware is in this repository. The installer's way back to the official
firmware uses the file you download from the maker; it is checked in your browser and never uploaded.
