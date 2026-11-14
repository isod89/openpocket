#!/bin/sh
# The release pin: OpenPocket's image and packages must be the bytes of the pinned release (upstream's published v42
# with the bass tuner fix), whatever changed in the base firmware or in another app (docs/BASE-FIRMWARE.md, section 9).
# tests/release.sha256 holds their SHA-256.
#   tests/release_pin.sh [SUMS]   compare SUMS (default build/SHA256SUMS, which `tools/op.mjs package` writes) with the pin
# The same sums mean that nothing in OpenPocket needs a check on a pedal. To pin a new OpenPocket release (a merge of
# upstream's next version): compare the packages with upstream's published ones, then copy build/SHA256SUMS over
# tests/release.sha256 in the same commit. A change made to OpenPocket on purpose, such as the bass tuner fix, is
# pinned the same way once its own tests pass.
HERE="$(cd "$(dirname "$0")" && pwd)"
SUMS="${1:-$HERE/../build/SHA256SUMS}"
if [ ! -f "$SUMS" ]; then echo "release pin: $SUMS not found (run: node tools/op.mjs package)"; exit 1; fi
if diff -u "$HERE/release.sha256" "$SUMS"; then
  echo "release pin: OpenPocket's image and packages are the release's, byte for byte ($(wc -l < "$SUMS" | tr -d ' ') files)"
else
  echo "release pin: FAILED. OpenPocket's image or packages changed (lines with + above are this build's)."
  echo "  A change to the base firmware or to another app must leave them the same: find what changed OpenPocket's code."
  echo "  A new OpenPocket release is pinned on purpose: see the head of tests/release_pin.sh."
  exit 1
fi
