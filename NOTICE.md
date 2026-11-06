# Credits and licences

OpenPocket is GPL-3.0-only (`LICENSE`), except the parts below, which keep their own licences.

| Part | Origin | Licence |
| --- | --- | --- |
| `firmware/bsp/` (chip support: startup, interrupts, second core, flash, USB-MIDI device, M-UPGRADE update entry `ota.c`) | [felucca-bsp](https://github.com/hugelton/felucca-bsp), Leo Kuroshita / Hügelton Instruments and contributors. `chip/ota.c` changed for OpenPocket (identity read at run time). | MPL-2.0 (`LICENSES/MPL-2.0.txt`) |
| `loader/` (update loader) | felucca-bsp `loader/`, changed for the Pocket X flash layout | MPL-2.0 |
| `docs/pxota.js`, `docs/pxpkg.js`, `tools/pxpkg.py`, `tools/mkloader.py`, `tools/lz4blk.py` | adapted from [Felucca](https://github.com/hugelton/Felucca)'s web installer and build tools (as used by CLIPPER and SLOOP) | GPL-3.0 |
| `firmware/sdk/` (I2S and DAC drivers as LLVM bitcode, SPL, cfg files) | JieLi AC79 SDK V1.2.0 (fw-AC79_AIoT_SDK) | Apache-2.0 (`LICENSES/Apache-2.0.txt`) |
| `third_party/jieli-ble/` (JieLi's Bluetooth LE controller, 33 objects of `btctrler.a` and 3 of `cpu.a`, as published LLVM bitcode, used only by the Bluetooth test image `make ble`) | JieLi AC79 SDK V1.2.0 (fw-AC79_AIoT_SDK, commit `0617f4c`), `cpu/wl82/liba/`; see `third_party/jieli-ble/README.md` | Apache-2.0 (`third_party/jieli-ble/LICENSE-JieLi-Apache-2.0`, `LICENSES/Apache-2.0.txt`) |
| `firmware/ble/ble_cfg.c` | values and names from the same SDK's `apps/common/config/log_config/lib_btctrler_config.c` | Apache-2.0 |
| `firmware/fonts.h` (bitmaps generated from Inter) | [Inter](https://rsms.me/inter/), Rasmus Andersson | SIL OFL 1.1 (`LICENSES/OFL-1.1-Inter.txt`) |

Sound models: the names refer to the gear that inspired each voicing (circuit analyses published by ElectroSmash,
Electric Druid, R.G. Keen / GEOFEX, Rob Robinette, Duncan's Tone Stack Calculator; speaker data sheets; manufacturers'
manuals). All trademarks belong to their owners; OpenPocket is not affiliated with Donner, M-VAVE or any gear maker.

No part of the official Pocket X / BlackBox firmware is in this repository. The installer's way back to the official
firmware uses the file you download from the maker; it is checked in your browser and never uploaded.
