# third_party/jieli-ble: JieLi's Bluetooth controller (Apache-2.0)

This directory holds JieLi's own Bluetooth LE controller code, **as published, unmodified**, for the advertise-only
Bluetooth test image (`make ble`, [docs/BLE-TEST.md](../../docs/BLE-TEST.md)). Nothing here is part of the normal
OpenPocket firmware or of the USB MIDI image; only the `ble` build target links it.

| | |
| --- | --- |
| Source | JieLi `fw-AC79_AIoT_SDK`, branch `release/AC79NN_SDK_V1.2.0`, commit `0617f4cc09685332aaf1104893718291c54ee5bb` |
| URL | https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK (files under `cpu/wl82/liba/`) |
| Licence | Apache-2.0, the SDK's `LICENSE`, kept here as `LICENSE-JieLi-Apache-2.0` (identical to `firmware/sdk/LICENSE-JieLi-Apache-2.0`) |
| Copyright | Zhuhai Jieli Technology Co., Ltd. |
| Ruling | The project owner ruled that linking the SDK's Apache-2.0 binary libraries is acceptable (the same reading as for `firmware/sdk/iis.bc`, `docs/AUDIO-SDK.md`). The libraries are shipped as LLVM bitcode, not as source. |

## What is here

Individual objects (LLVM bitcode, clang 4.0.1, the same compiler as our toolchain), taken as they are from the
archives; `build.sh` compiles each with `clang -target pi32v2 -x ir`, as it does for `firmware/sdk/iis.bc`.

| Directory | From | Objects |
| --- | --- | --- |
| `btctrler/` | `btctrler.a` (sha256 `d98f2ad9d483cd2c103cab56c2760162edf92cc451a04e5668af02a590c95cb3`) | the LE controller: HCI front end, link layer, advertising, slave link, the LE and BT radio drivers, interrupt and DMA helpers, their utilities (33 of the archive's 106 bitcode members) |
| `cpu/` | `cpu.a` (sha256 `3d3e0876e21afe2c9cb7de713378d3802b1d0f544a1787f07e982269a0e7e67d`) | `delay`, `crc16`, `wlc` (small helpers the controller calls) |

Not used: the classic Bluetooth, TWS, LE 5 extended/ISO members of `btctrler.a`, and all of `btstack.a` (the host
stack: this image sends its advertisement through the controller's own HCI-level functions).

`extract.sh` rebuilds this directory from an SDK checkout and checks the archive hashes above:
`sh third_party/jieli-ble/extract.sh /path/to/fw-AC79_AIoT_SDK`.

## What OpenPocket supplies

`firmware/ble/` is OpenPocket's own code that these objects need: a small cooperative scheduler with the operating
system calls they make (tasks, queues, semaphores, timers, heap, interrupt registration), the configuration
(`ble_cfg.c`, derived from the SDK's `apps/common/config/log_config/lib_btctrler_config.c`, Apache-2.0), empty stand-ins
for the parts that are not built (`ble_stubs.c`), and the radio start-up replay (`ble_rf.c`).
