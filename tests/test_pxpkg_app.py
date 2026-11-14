#!/usr/bin/env python3
"""tools/pxpkg.py and the product an image is (firmware/base/product.h): OpenPocket holds no product descriptor and is
packaged as ever; an app on the base firmware holds one, and its packages and manifest are named after it. A package
is never built as the other kind. No official file needed.
  python3 tests/test_pxpkg_app.py"""
import json
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import mkloader  # noqa: E402
import pxpkg  # noqa: E402

fails = 0


def ok(cond, what):
    global fails
    print(f"{what:<96} {'ok' if cond else 'FAIL'}")
    fails += 0 if cond else 1


def refused(fn):
    try:
        fn()
    except SystemExit as e:
        return str(e)
    return ""


def desc(ident=b"starter", name=b"STARTER", ver=b"v01", caps=0x0D, editor=1):
    return struct.pack("<8s16s16s8sBB2x", b"OP-APP:", ident, name, ver, caps, editor)


sdk = ROOT / "firmware" / "sdk" / "packager"
ota = mkloader.wrap(b"\x00\x00FELUCCA-LOADER-1" + bytes(900))
plain = b"\x00" * 64 + b"OP-PRODUCT:PocketX_900" + b"\0" * 10 + bytes(range(256)) * 40
alt = plain + desc()
with tempfile.TemporaryDirectory() as t:
    out = Path(t)
    ok(pxpkg.app_info(plain) is None, "an image without a product descriptor is OpenPocket")
    man = pxpkg.build(plain, ota, out / "op", 42, None, sdk)
    ok("app" not in man and [p["file"] for p in man["packages"]] == ["PocketX_942.fwsc", "BlackBox_942.fwsc"]
       and list(json.loads((out / "op" / "manifest.json").read_text())) == ["version", "packages"],
       "OpenPocket: the packages and the manifest are named and shaped as ever")
    info = pxpkg.app_info(alt)
    ok(info == {"id": "starter", "name": "STARTER", "version": "v01", "caps": ["stereo", "midi", "store"], "editor": 1},
       "an app's descriptor is read from its image: id, name, version, what it can do, its editor protocol")
    man = pxpkg.build(alt, ota, out / "alt", 1, None, sdk, "starter")
    ok(man["app"] == info and [p["file"] for p in man["packages"]] == ["starter-PocketX_901.fwsc", "starter-BlackBox_901.fwsc"]
       and [p["product"] for p in man["packages"]] == ["PocketX_901", "BlackBox_901"],
       "the app's packages carry its id in their names, its manifest says which product they are; the identity is Model_9NN")
    pkg = (out / "alt" / "starter-PocketX_901.fwsc").read_bytes()
    ok(pxpkg.product_of(pkg) == "PocketX_901", "the package's identity markers are the model's, as an installer of any age expects")
    e = refused(lambda: pxpkg.build(alt, ota, out / "x1", 1, None, sdk))
    ok("not OpenPocket" in e and "--app starter" in e and not (out / "x1").exists(), "an app's image packaged as OpenPocket: refused, nothing written")
    e = refused(lambda: pxpkg.build(plain, ota, out / "x2", 1, None, sdk, "starter"))
    ok("no product descriptor" in e and not (out / "x2").exists(), "OpenPocket's image packaged as an app: refused, nothing written")
    e = refused(lambda: pxpkg.build(alt, ota, out / "x3", 1, None, sdk, "chroma"))
    ok("is the app 'starter'" in e and not (out / "x3").exists(), "an app's image packaged under another app's name: refused")
    ok("more than one" in refused(lambda: pxpkg.app_info(alt + desc())), "two descriptors in one image: refused")
    ok("damaged" in refused(lambda: pxpkg.app_info(plain + desc(ident=b"Not An Id"))) and "damaged" in refused(lambda: pxpkg.app_info(plain + desc(name=b""))),
       "a descriptor whose id is not lowercase letters, digits and \"-\", or without a name: refused")
    ok(pxpkg.app_info(plain + desc(caps=0x03, editor=0))["caps"] == ["stereo", "usb-audio"], "the capability bits are named in the firmware's order")
print("pxpkg products: " + (f"{fails} FAILED" if fails else "ok"))
sys.exit(1 if fails else 0)
