#!/bin/sh
# Rebuild third_party/jieli-ble/ from a checkout of JieLi's fw-AC79_AIoT_SDK (release/AC79NN_SDK_V1.2.0, commit
# 0617f4cc09685332aaf1104893718291c54ee5bb):  sh third_party/jieli-ble/extract.sh /path/to/fw-AC79_AIoT_SDK
# The archives are checked against the hashes in README.md first; the objects are copied out unchanged.
set -e
SDK="${1:?usage: extract.sh /path/to/fw-AC79_AIoT_SDK}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$SDK/cpu/wl82/liba"
sum() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$1" | cut -d' ' -f1; }
[ "$(sum "$LIB/btctrler.a")" = d98f2ad9d483cd2c103cab56c2760162edf92cc451a04e5668af02a590c95cb3 ] || { echo "btctrler.a is not the pinned file"; exit 1; }
[ "$(sum "$LIB/cpu.a")" = 3d3e0876e21afe2c9cb7de713378d3802b1d0f544a1787f07e982269a0e7e67d ] || { echo "cpu.a is not the pinned file"; exit 1; }
[ "$(sum "$LIB/btstack.a")" = 24e99a6d63960e1b1b24b88980bc0ce37521c272bba9bcee71a47a9860b237e0 ] || { echo "btstack.a is not the pinned file"; exit 1; }
[ "$(sum "$LIB/lib_ccm_aes.a")" = 5dbd07a1e10e58db2fbaa712e9617837a170d809a7063415b55dc1639d1e0c1e ] || { echo "lib_ccm_aes.a is not the pinned file"; exit 1; }
grep -v '^#' "$HERE/MEMBERS" | while IFS="$(printf '\t')" read -r arch member dest; do
  mkdir -p "$HERE/$(dirname "$dest")"
  ar p "$LIB/$arch" "$member" > "$HERE/$dest"
done
cp "$SDK/LICENSE" "$HERE/LICENSE-JieLi-Apache-2.0"
echo "extracted $(grep -vc '^#' "$HERE/MEMBERS") objects"
