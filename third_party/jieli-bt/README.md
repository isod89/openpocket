# third_party/jieli-bt: JieLi's classic Bluetooth controller and host (Apache-2.0)

The classic (BR/EDR) half of JieLi's Bluetooth libraries, **as published, unmodified**, for the Bluetooth audio test image
(`make bt`, [docs/BT-AUDIO-TEST.md](../../docs/BT-AUDIO-TEST.md)): the link manager and baseband, Secure Simple Pairing, and the
host's SDP, AVDTP and A2DP sink. They are the same libraries the stock BlackBox firmware runs (its SDP record, `JL_A2DP`, is
this host's own, byte for byte). Nothing here is part of the normal OpenPocket firmware or of the BLE MIDI image.

| | |
| --- | --- |
| Source | JieLi `fw-AC79_AIoT_SDK`, branch `release/AC79NN_SDK_V1.2.0`, commit `0617f4cc09685332aaf1104893718291c54ee5bb` (the one `third_party/jieli-ble` pins) |
| URL | https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK (files under `cpu/wl82/liba/`) |
| Licence | Apache-2.0, the SDK's `LICENSE`, kept here as `LICENSE-JieLi-Apache-2.0` (identical to `third_party/jieli-ble`'s) |
| Copyright | Zhuhai Jieli Technology Co., Ltd. |
| Ruling | As for `third_party/jieli-ble`: the project owner ruled that linking the SDK's Apache-2.0 binary libraries is acceptable. LLVM bitcode, not source. |

| Directory | From | Objects |
| --- | --- | --- |
| `btctrler/` | `btctrler.a` (the archive `jieli-ble` pins, sha256 `d98f2ad9...`) | the classic controller: LMP, its HCI commands, the BR/EDR baseband, link, radio, slot timers and low power, the hopping channels, authentication and the pairing crypto task (17 of the archive's members) |
| `btstack/` | `btstack.a` (sha256 `24e99a6d...`) | the classic host: SDP, AVDTP, A2DP and its sink, the A2DP media queue, the classic control API (`avctp_user`), the paired devices' records (10 members) |
| `crypto/` | `crypto_toolbox_Osize.a` (sha256 `9757a8acbfda785ee39b51721fbfe55f5eed29a8b3c12a9c6fa769856b5e7ac6`) | ECDH on P-192, SHA-256, HMAC: Secure Simple Pairing (7 members) |

The image also links the shared members of `third_party/jieli-ble` (the controller's task, HCI, radio and buffers, the host's task,
L2CAP and utilities); `firmware/bt/members.txt` lists every member it compiles. Not used: Bluetooth LE, TWS, eSCO / HFP, AVRCP,
HID, SPP, OBEX, PAN, the GATT client; `firmware/bt/bt_stubs.c` stands in for what they would define.

`extract.sh` rebuilds this directory from an SDK checkout and checks the archive hashes above:
`sh third_party/jieli-bt/extract.sh /path/to/fw-AC79_AIoT_SDK`.
