#!/usr/bin/env python3
"""Test fixtures for tests/test_installer.mjs: a fake app (with its OP_PRODUCT field), a fake update loader wrapped
like the real one, OpenPocket packages built by tools/pxpkg.py, and a starting flash with an older OpenPocket.
Also, in OUTDIR/alt, the packages of an alternative firmware: the same fake app with the product descriptor of an app
on the base firmware (firmware/base/product.h): STARTER v01, id "starter".
  fixtures.py OUTDIR"""
import os
import random
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkloader  # noqa: E402
import pxpkg  # noqa: E402

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
rnd = random.Random(7)
body = bytes(rnd.getrandbits(8) for _ in range(90000))
app = b"\x00" * 64 + b"OP-PRODUCT:PocketX_900" + b"\0" * 10 + body           # as the firmware links it
loader = b"\x00\x00FELUCCA-LOADER-1" + bytes(rnd.getrandbits(8) & 0x0F for _ in range(9000))
ota = mkloader.wrap(loader)
sdk = ROOT / "firmware" / "sdk" / "packager"
pxpkg.build(app, ota, out, 24, None, sdk)                                         # PocketX_924, BlackBox_924
files = pxpkg.sdk_files(None, sdk)
old = pxpkg.flash_image(pxpkg.with_product(app[:-500] + b"older", "PocketX_923"), files)
flash = bytearray(b"\xFF" * 0x100000)
flash[:len(old)] = old
flash[0xC0000:0xC0010] = b"STOCK USER DATA!"                                     # USR: must survive
(out / "flash_old.bin").write_bytes(bytes(flash))
(out / "app_new_px.bin").write_bytes(pxpkg.with_product(app, "PocketX_924"))
alt = app + struct.pack("<8s16s16s8sBB2x", b"OP-APP:", b"starter", b"STARTER", b"v01", 0x0D, 1)   # stereo, midi, store; editor 1
pxpkg.build(alt, ota, out / "alt", 1, None, sdk, "starter")                       # alt/starter-PocketX_901.fwsc, ...
(out / "app_alt_px.bin").write_bytes(pxpkg.with_product(alt, "PocketX_901"))
on_alt = bytearray(b"\xFF" * 0x100000)                                           # a pedal that runs the alternative firmware
img = pxpkg.flash_image(pxpkg.with_product(alt, "PocketX_901"), files)
on_alt[:len(img)] = img
(out / "flash_alt.bin").write_bytes(bytes(on_alt))
pxpkg.build(app, ota, out / "op42", 42, None, sdk)                                # an OpenPocket that says which it is (v32 on)
pxpkg.build(alt, ota, out / "alt42", 42, None, sdk, "starter")                    # an alternative firmware with OpenPocket's number
print("fixtures ok:", ", ".join(sorted(p.name for p in out.iterdir())))
