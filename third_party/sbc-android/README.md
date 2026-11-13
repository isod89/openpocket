# third_party/sbc-android: Android's SBC decoder (Apache-2.0)

The SBC codec of Android's Bluetooth stack, **as published, unmodified**, for the Bluetooth audio test image
(`make bt`, [docs/BT-AUDIO-TEST.md](../../docs/BT-AUDIO-TEST.md)). SBC is the audio codec every A2DP phone sends.

| | |
| --- | --- |
| Source | https://android.googlesource.com/platform/packages/modules/Bluetooth, `system/embdrv/sbc/` |
| Revision | `745ee92b87ed62b5a8a1bac47d5df8cad623bf59` |
| Licence | Apache-2.0 (each file's header; the module's NOTICE, kept here as `LICENSE`) |
| Copyright | The Android Open Source Project; Open Interface North America, Inc. (the decoder) |

- `decoder/` (fixed point, no allocation, no operating system): linked into the Bluetooth audio image only, through
  `firmware/bt/bt_sbc.c`. Nothing here is part of the normal OpenPocket firmware.
- `encoder/`: used only by the host test `tests/test_bt_sbc.c` (it makes the SBC frames the decoder is checked on).
