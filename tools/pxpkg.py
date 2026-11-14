#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# OpenPocket — update packages (.fwsc) for the Donner Pocket X and the M-VAVE BlackBox.
#
# Adapted from Felucca's fm1pkg_make.py (GPL-3.0, Leo Kuroshita / Hügelton Instruments) for the Pocket X flash
# layout, which this script reproduces byte for byte from the official PocketX_016 package (tests/test_pxpkg.py).
#
#   pxpkg.py APP.bin OTA.bin OUTDIR --version 24 [--sdk DIR]
#       -> OUTDIR/PocketX_924.fwsc, OUTDIR/BlackBox_924.fwsc, OUTDIR/manifest.json
#   pxpkg.py APP.bin OTA.bin OUTDIR --version 1 --app starter      an app on the base firmware other than OpenPocket
#       -> OUTDIR/starter-PocketX_901.fwsc, OUTDIR/starter-BlackBox_901.fwsc, and a manifest.json that says which
#          product they are ("app": its id, name, version and what it can do, read from the image itself)
#
# Package = JieLi UFW image: flash.bin (the whole [0, 0xA3000) image the update loader writes from 0x4000 up) and
# ota.bin (the update loader, loader/). The flash head [0, 0x4000) is the JieLi SDK default (flash header, SDK SPL
# uboot.boot, an isd_config blob that decodes to the chip key): the OpenPocket loader never writes it.
# The app area: app_area_head, app.bin, cfg_tool.bin, the Pocket X region descriptors (VM, PRCT, BTIF, USR) and the
# nested "cfg" directory, SFC-encrypted with the chip key. All checks in the format are CRC16 (poly 0x1021, init 0).
# The package identity (the 20 marker bytes of the .fwsc) is "PocketX_9NN" or "BlackBox_9NN"; the same string is
# written into the app (OP_PRODUCT), so the running firmware reports exactly the package it came from.
# The identity is the same for every product on the base firmware (an installer of any age finds the pedal and can put
# OpenPocket back). Which product an image is stands in the image: an app other than OpenPocket holds a descriptor
# (firmware/base/product.h, "OP-APP:"), and OpenPocket holds none. A package is never built as the other kind.
import argparse
import hashlib
import json
import os
import re
import struct
from pathlib import Path

KEY = 0x980F                      # chip key of the Pocket X / BlackBox (and FM-1): isd_config.ini in the flash head
AREA_END = 0xA3000                # end of the app area (PRCT) in PocketX_016; the loader writes [0x4000, AREA_END)
APP_SLOT = 0x9D740                # app.bin slot in PocketX_016 (the XIP window the SPL maps at 0x02000120)
FLASH_HDR = bytes.fromhex("20022f104cc900f00f001010a4ff4e5d0b41e0cd781dc856417fb367cf9f")   # = PocketX_016's
UFW_CHIP = b"AC791N"
PRODUCT_MARK = b"OP-PRODUCT:"     # firmware: const char OP_PRODUCT[32] = "OP-PRODUCT:" + identity
APP_MARK = b"OP-APP:\0"            # firmware: const op_app_t OP_APP = {"OP-APP:", id, name, version, caps, editor}
APP_CAPS = ("stereo", "usb-audio", "midi", "store")      # op_app_t.caps, bit 0 first (OP_CAP_*)
MODELS = ("PocketX", "BlackBox")
# region descriptors exactly as PocketX_016 has them (16-byte name fields include the SDK packer's trailing bytes)
REGIONS = [
    (0xA3000, 0x1B000, 0x12, 0x81, 0, bytes.fromhex("564d00ffffffffffffffffff00800000")),   # VM
    (0x00000, AREA_END, 0x92, 0x82, 0, bytes.fromhex("5052435400ffffffffffffff434f4445")),  # PRCT "CODE"
    (0xBE000, 0x01000, 0x92, 0x81, 0, bytes.fromhex("4254494600ffffffffffffff4155544f")),  # BTIF "AUTO"
    (0xBF000, 0x40000, 0x92, 0x81, 1, b"USR"),
]
SDK_FILES = ("uboot.boot", "cfg_tool.bin", "cfg/eq_cfg_hw.bin")


def _crc_table():
    t = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else (c << 1)
        t.append(c & 0xFFFF)
    return t


_T = _crc_table()


def crc16(data, crc=0):
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _T[((crc >> 8) ^ b) & 0xFF]
    return crc


def enc(buf, off, size, key=0xFFFF):
    """JieLi ENC stream cipher, in place"""
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def sfc(buf, off, size, base, key):
    """SFC cipher: ENC per 32-byte block, the key mixed with the block's offset in the area"""
    for i in range(0, size, 32):
        enc(buf, off + i, min(size - i, 32), key ^ ((off + i - base) >> 2))


def chipkey_decode(d):
    s = sum(d[:16]) & 0xFF
    s = 0xAA if s >= 0xE0 else 0x55 if s <= 0x10 else s
    return sum(1 << i for i in range(16) if (d[16 + i] ^ d[15 - i]) < s)


def entry(dcrc, offset, size, flags, resvd, index, name, fill=0xFF):
    if isinstance(name, bytes) and len(name) == 16:
        nm = name
    else:
        nm = (name if isinstance(name, bytes) else name.encode()) + b"\0"
        nm = nm + bytes([fill]) * (16 - len(nm))
    body = struct.pack("<HIIBBH16s", dcrc, offset & 0xFFFFFFFF, size & 0xFFFFFFFF, flags, resvd, index, nm)
    return struct.pack("<H", crc16(body)) + body


def key_blob(key):
    """32 bytes that chipkey_decode() turns into key, + CRC16"""
    d = bytearray([0x10] * 16) + bytearray(16)
    for i in range(16):
        d[16 + i] = d[15 - i] ^ (0x00 if key >> i & 1 else 0xFF)
    assert chipkey_decode(bytes(d)) == key
    return bytes(d) + struct.pack("<H", crc16(d))


def sdk_files(sdk, flat=None):
    """the three JieLi SDK files (Apache-2.0): from an SDK checkout, or a flat directory (firmware/sdk/packager)"""
    root = Path(sdk or os.environ.get("AC79_SDK", "")) / "cpu" / "wl82" / "tools"
    out = {}
    for rel in SDK_FILES:
        path = Path(flat) / Path(rel).name if flat else root / rel
        try:
            out[rel] = path.read_bytes()
        except OSError as e:
            raise SystemExit(f"pxpkg: {path}: {e.strerror}. Give --sdk-files DIR or --sdk (JieLi AC79 SDK).")
    return out


def app_area(app, cfg_tool, eq, key=KEY):
    """the app area [0x4000, ...) as the SPL reads it, SFC-encrypted (relative offsets from 0x4000)"""
    if len(app) > APP_SLOT:
        raise SystemExit(f"pxpkg: app is {len(app)} B, the slot is {APP_SLOT} B")
    app = app + b"\xFF" * (APP_SLOT - len(app))
    blk = 0x120 + APP_SLOT + len(cfg_tool)
    area = bytearray(b"\xFF" * blk)
    area[0x120:0x120 + APP_SLOT] = app
    area[0x120 + APP_SLOT:blk] = cfg_tool
    ents = [entry(crc16(app), 0x120, APP_SLOT, 0x82, 0xFF, 0, "app.bin"),
            entry(crc16(cfg_tool), 0x120 + APP_SLOT, len(cfg_tool), 0x82, 0xFF, 0, "cfg_tool.bin")]
    ents += [entry(0xFFFF, a, n, fl, rs, ix, nm) for a, n, fl, rs, ix, nm in REGIONS]
    area[32:32 + 32 * len(ents)] = b"".join(ents)
    area[0:32] = entry(crc16(area[32:blk]), 0x02000120, blk, 0x83, 0xFF, 0, "app_area_head")
    eq_e = entry(crc16(eq), 0x40, len(eq), 0x82, 0xFF, 1, "eq_cfg_hw.bin")
    cfg_body = eq_e + eq
    cfg = entry(crc16(cfg_body), 0x20, 0x20 + len(cfg_body), 0x83, 0xFF, 1, "cfg") + cfg_body
    region = bytearray(area + cfg)
    sfc(region, 0, len(region), 0, key)
    if 0x4000 + len(region) > AREA_END:
        raise SystemExit(f"pxpkg: app area ends at 0x{0x4000 + len(region):x}, past 0x{AREA_END:x}")
    return bytes(region)


def flash_image(app, sdk, key=KEY):
    spl, cfg_tool, eq = sdk["uboot.boot"], sdk["cfg_tool.bin"], sdk["cfg/eq_cfg_hw.bin"]
    isd = key_blob(key) + b"[OPENPOCKET]\r\n"
    f = bytearray(b"\xFF" * AREA_END)
    spl_off = 0xA0
    isd_off = spl_off + len(spl)
    assert isd_off + len(isd) <= 0x4000
    f[spl_off:spl_off + len(spl)] = spl
    f[isd_off:isd_off + len(isd)] = isd
    top = (entry(crc16(spl), spl_off, len(spl), 0x00, 0x00, 0, "uboot.boot") +
           entry(crc16(isd), isd_off, len(isd), 0x02, 0x80, 0, "isd_config.ini") +
           entry(0xFFFF, 0x4000, 0xFFFFFFFF, 0x81, 0xFF, 0, "app_dir_head") +
           entry(0xFFFF, 0xFF000, 0x1000, 0x12, 0x01, 0xFFFF, "key_mac"))
    head = bytearray(struct.pack("<H", crc16(FLASH_HDR)) + FLASH_HDR + top)
    for o in range(0, len(head), 32):
        enc(head, o, 32)
    f[0:len(head)] = head
    region = app_area(app, cfg_tool, eq, key)
    f[0x4000:0x4000 + len(region)] = region
    return bytes(f)


def ufw(flash, ota, product):
    """UFW image (flash.bin + ota.bin) with the 20 identity markers of a .fwsc"""
    files = [(0, "flash.bin", flash), (100, "ota.bin", ota)]
    off, ents = 0x400, []
    for i, (typ, name, d) in enumerate(files):
        off = (off + 0xFF) & ~0xFF
        e = bytearray(0x50)
        struct.pack_into("<HHHHIII", e, 0, typ, i, crc16(d), 0, off, len(d), (len(d) + 31) & ~31)
        e[0x40:0x40 + len(name)] = name.encode()
        ents.append((e, off, d))
        off += len(d)
    total = off
    hdr = bytearray(0x40)
    struct.pack_into("<IH", hdr, 4, total, len(files))
    struct.pack_into("<HHH", hdr, 10, 0x0004, 0x0200, 0)
    hdr[16:16 + len(UFW_CHIP)] = UFW_CHIP
    lst = bytearray()
    for e, _, _ in ents:
        x = bytearray(e)
        enc(x, 0, 0x50)
        lst += x
    struct.pack_into("<H", hdr, 2, crc16(lst))
    struct.pack_into("<H", hdr, 0, crc16(hdr[2:0x40]))
    enc(hdr, 0, 0x40)
    logical = bytearray(b"\xFF" * total)
    logical[0:0x40] = hdr
    logical[0x40:0x40 + len(lst)] = lst
    for e, o, d in ents:
        logical[o:o + len(d)] = d
    if len(product) > 20 or any((ord(c) + i + 1) & 0xFF == 0x7D for i, c in enumerate(product)):
        raise SystemExit(f"pxpkg: product {product!r}: at most 20 characters, none encoding to the unused marker")
    raw = bytearray()
    for i in range(20):
        ch = product[i] if i < len(product) else None
        raw += logical[i * 0x2F:(i + 1) * 0x2F] + bytes([(ord(ch) + i + 1) & 0xFF if ch else 0x7D])
    raw += logical[20 * 0x2F:]
    return bytes(raw)


def product_of(fwsc):
    s = ""
    for i in range(20):
        m = fwsc[i * 0x30 + 0x2F]
        if m != 0x7D:
            s += chr((m - i - 1) & 0xFF)
    return s


def with_product(app, product):
    """the app with its OP_PRODUCT field set to the package identity"""
    i = app.find(PRODUCT_MARK)
    if i < 0 or app.find(PRODUCT_MARK, i + 1) >= 0:
        raise SystemExit("pxpkg: the app must hold exactly one OP_PRODUCT field (\"OP-PRODUCT:\")")
    field = PRODUCT_MARK + product.encode()
    if len(field) >= 32:
        raise SystemExit("pxpkg: product too long for OP_PRODUCT")
    a = bytearray(app)
    a[i:i + 32] = field + b"\0" * (32 - len(field))
    return bytes(a)


def app_info(app):
    """which product the image is: the descriptor of an app other than OpenPocket (firmware/base/product.h), or None"""
    i = app.find(APP_MARK)
    if i < 0:
        return None
    if app.find(APP_MARK, i + 1) >= 0:
        raise SystemExit("pxpkg: the app holds more than one product descriptor (\"OP-APP:\")")
    f = app[i:i + 52]
    text = [f[a:b].split(b"\0")[0].decode("ascii", "replace") for a, b in ((8, 24), (24, 40), (40, 48))]
    if len(f) < 52 or not re.fullmatch(r"[a-z0-9-]{1,15}", text[0]) or not text[1] or not text[2]:
        raise SystemExit(f"pxpkg: the app's product descriptor is damaged (id {text[0]!r}, name {text[1]!r})")
    return {"id": text[0], "name": text[1], "version": text[2],
            "caps": [c for k, c in enumerate(APP_CAPS) if f[48] >> k & 1], "editor": f[49]}


def build(app, ota, outdir, version, sdk, flat=None, app_id=None):
    info = app_info(app)
    if app_id is None and info:
        raise SystemExit(f"pxpkg: this image is {info['name']} (the app {info['id']!r}), not OpenPocket: "
                         f"package it with --app {info['id']}")
    if app_id is not None and (not info or info["id"] != app_id):
        raise SystemExit(f"pxpkg: --app {app_id}: the image " + (f"is the app {info['id']!r}" if info else
                         "holds no product descriptor (the app must include base/product.h, with OP_APP_ID its folder's name)"))
    outdir.mkdir(parents=True, exist_ok=True)
    files = sdk_files(sdk, flat)
    man = {"version": version, **({"app": info} if info else {}), "packages": []}
    for model in MODELS:
        product = f"{model}_{900 + version}"
        pkg = ufw(flash_image(with_product(app, product), files), ota, product)
        name = f"{info['id']}-{product}.fwsc" if info else f"{product}.fwsc"
        (outdir / name).write_bytes(pkg)
        man["packages"].append({"model": model, "product": product, "file": name, "size": len(pkg),
                                "sha256": hashlib.sha256(pkg).hexdigest()})
        print(f"package {outdir / name}: {len(pkg)} B, identity {product}" + (f", product {info['name']} {info['version']}" if info else ""))
    (outdir / "manifest.json").write_text(json.dumps(man, indent=2) + "\n")
    return man


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("app", type=Path)
    ap.add_argument("ota", type=Path)
    ap.add_argument("outdir", type=Path)
    ap.add_argument("--version", type=int, required=True, help="OpenPocket version number (24 -> PocketX_924)")
    ap.add_argument("--sdk", help="JieLi AC79 SDK checkout (default: $AC79_SDK)")
    ap.add_argument("--sdk-files", help="directory holding uboot.boot, cfg_tool.bin, eq_cfg_hw.bin")
    ap.add_argument("--app", dest="app_id", help="the image is this app on the base firmware (its folder under firmware/apps), not OpenPocket")
    a = ap.parse_args()
    if not 1 <= a.version <= 99:
        raise SystemExit("pxpkg: --version 1..99")
    build(a.app.read_bytes(), a.ota.read_bytes(), a.outdir, a.version, a.sdk, a.sdk_files, a.app_id)


if __name__ == "__main__":
    main()
