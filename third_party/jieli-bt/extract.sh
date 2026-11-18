#!/bin/sh
# Rebuild third_party/jieli-bt/ from a checkout of JieLi's fw-AC79_AIoT_SDK (release/AC79NN_SDK_V1.2.0, commit
# 0617f4cc09685332aaf1104893718291c54ee5bb):  sh third_party/jieli-bt/extract.sh /path/to/fw-AC79_AIoT_SDK
# The archives are checked against the hashes in README.md first; the objects are copied out unchanged.
set -e
SDK="${1:?usage: extract.sh /path/to/fw-AC79_AIoT_SDK}"
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$SDK/cpu/wl82/liba"
sum() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1 || shasum -a 256 "$1" | cut -d' ' -f1; }
[ "$(sum "$LIB/btctrler.a")" = d98f2ad9d483cd2c103cab56c2760162edf92cc451a04e5668af02a590c95cb3 ] || { echo "btctrler.a is not the pinned file"; exit 1; }
[ "$(sum "$LIB/btstack.a")" = 24e99a6d63960e1b1b24b88980bc0ce37521c272bba9bcee71a47a9860b237e0 ] || { echo "btstack.a is not the pinned file"; exit 1; }
[ "$(sum "$LIB/crypto_toolbox_Osize.a")" = 9757a8acbfda785ee39b51721fbfe55f5eed29a8b3c12a9c6fa769856b5e7ac6 ] || { echo "crypto_toolbox_Osize.a is not the pinned file"; exit 1; }
grep -v '^#' "$HERE/MEMBERS" | while IFS="$(printf '\t')" read -r arch member dest; do
  mkdir -p "$HERE/$(dirname "$dest")"
  ar p "$LIB/$arch" "$member" > "$HERE/$dest"
done
cp "$SDK/LICENSE" "$HERE/LICENSE-JieLi-Apache-2.0"
echo "extracted $(grep -vc '^#' "$HERE/MEMBERS") objects"
