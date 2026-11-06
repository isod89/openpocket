#!/usr/bin/env python3
"""pocketx_fw.py - unpack / verify / repack JieLi AC791N .fwsc packages
(Donner Pocket X, M-VAVE / Sinco ANN BlackBox; same format as the M-VAVE FM-1).

  pocketx_fw.py info   PKG.fwsc
  pocketx_fw.py unpack PKG.fwsc OUTDIR
  pocketx_fw.py pack   OUTDIR NEW.fwsc [--tag PocketX_016]

`unpack` writes every package entry, the decrypted flash image (flash_dec.bin),
the decrypted app (app.bin), cfg_tool.bin and a manifest. Edit app.bin in place
(same size: patches, strings, tables) and run `pack`: only the 32-byte flash
blocks you changed are re-encrypted, then every CRC is rebuilt
(app.bin data CRC, its JLFS entry CRC, app_area_head CRC, flash.bin CRC in the
UFW list, list CRC, header CRC) and the 20 identity markers are re-encoded.
`pack` on an untouched unpack reproduces the original file byte for byte.

Format (all CRCs are CRC16 poly 0x1021 init 0, JieLi "ENC" = 16-bit LFSR XOR):
  .fwsc   = logical UFW image with one marker byte after each of the first 20
            0x2F-byte blocks (marker = identity char + index + 1, 0x7D = unused)
  UFW     = 0x40 header (ENC 0xFFFF): hdr_crc=crc(hdr[2:0x40]), list_crc,
            total, count, "AC791N"; then count x 0x50 entries (each ENC 0xFFFF,
            list_crc = crc of the encrypted entries); flash.bin, info.log,
            isd_config.ini, ota.bin (usb_hid_ota.bin loader), script.ver,
            blimit.bin, tail.bin
  flash   = [0,0x4000) head: flash header + JLFS (ENC 0xFFFF per 32 B), SPL
            uboot.boot, isd_config blob (chip key 0x980F)
            [0x4000,..) app area, SFC: ENC per 32-B block, key 0x980F ^ (rel>>2)
            app_area_head (dcrc = crc(area[0x20:len])), app.bin, cfg_tool.bin,
            VM / PRCT / BTIF / USR region descriptors
"""
import argparse, hashlib, json, os, struct, sys
from pathlib import Path

CHIPKEY = 0x980F
NSLOT = 20
UNUSED = 0x7D


def enc(data, key=0xFFFF):
    out = bytearray(data)
    for i in range(len(out)):
        out[i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF
    return bytes(out)


_T = []
for _i in range(256):
    _c = _i << 8
    for _ in range(8):
        _c = ((_c << 1) ^ 0x1021) if _c & 0x8000 else (_c << 1)
    _T.append(_c & 0xFFFF)


def crc16(data, crc=0):
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _T[((crc >> 8) ^ b) & 0xFF]
    return crc


def sfc_block(blk, rel, key=CHIPKEY):
    return enc(blk, (key ^ (rel >> 2)) & 0xFFFF)


def sfc(data, base_rel, key=CHIPKEY):
    return b"".join(sfc_block(data[i:i + 32], base_rel + i, key) for i in range(0, len(data), 32))


# ---------------------------------------------------------------- .fwsc layer
def fwsc_to_logical(raw):
    tag = ""
    L = bytearray()
    for i in range(NSLOT):
        L += raw[i * 0x30:i * 0x30 + 0x2F]
        m = raw[i * 0x30 + 0x2F]
        if m != UNUSED:
            tag += chr((m - i - 1) & 0xFF)
    return bytes(L + raw[NSLOT * 0x30:]), tag


def logical_to_fwsc(L, tag):
    if len(tag) > NSLOT or any((ord(c) + i + 1) & 0xFF == UNUSED for i, c in enumerate(tag)):
        raise SystemExit(f"tag {tag!r}: max 20 chars, none encoding to 0x7D")
    raw = bytearray()
    for i in range(NSLOT):
        m = (ord(tag[i]) + i + 1) & 0xFF if i < len(tag) else UNUSED
        raw += L[i * 0x2F:(i + 1) * 0x2F] + bytes([m])
    return bytes(raw + L[NSLOT * 0x2F:])


# ---------------------------------------------------------------- UFW layer
def ufw_parse(L):
    hdr = enc(L[:0x40])
    hcrc, lcrc, total, count = struct.unpack_from("<HHIH", hdr)
    ents = []
    for i in range(count):
        o = 0x40 + i * 0x50
        e = enc(L[o:o + 0x50])
        typ, idx, crc, _r, off, ln, alloc = struct.unpack_from("<HHHHIII", e)
        name = e[0x40:0x50].split(b"\0")[0].decode()
        ents.append(dict(i=i, type=typ, idx=idx, crc=crc, off=off, len=ln, alloc=alloc, name=name, raw=e))
    return dict(hdr=hdr, hcrc=hcrc, lcrc=lcrc, total=total, count=count, ents=ents)


def ufw_checks(L, u):
    lst = L[0x40:0x40 + u["count"] * 0x50]
    return {"ufw header crc": crc16(u["hdr"][2:0x40]) == u["hcrc"],
            "ufw list crc": crc16(lst) == u["lcrc"]}


# ---------------------------------------------------------------- flash layer
def jlfs_entry(dec, o):
    e = dec[o:o + 32]
    hc, dc, addr, ln, attr, rs, idx = struct.unpack_from("<HHIIBBH", e)
    name = e[16:32].split(b"\0")[0].split(b"\xff")[0]
    return dict(o=o, hcrc=hc, dcrc=dc, addr=addr, len=ln, attr=attr, name=name.decode("latin1"),
                hdr_ok=crc16(e[2:32]) == hc)


def flash_decrypt(flash):
    return flash[:0x4000] + sfc(flash[0x4000:], 0)


def flash_parse(dec):
    head = jlfs_entry(dec, 0x4000)
    ents = [head]
    o = 0x4020
    while o < 0x4120:
        e = jlfs_entry(dec, o)
        if not e["name"] or not e["name"].isprintable():
            break
        ents.append(e)
        o += 32
    return ents


def flash_checks(dec, ents):
    res = {}
    head = ents[0]
    res["app_area_head entry crc"] = head["hdr_ok"]
    res["app_area_head data crc"] = crc16(dec[0x4020:0x4000 + head["len"]]) == head["dcrc"]
    for e in ents[1:]:
        res[f"{e['name']} entry crc"] = e["hdr_ok"]
        if e["dcrc"] != 0xFFFF:
            d = dec[0x4000 + e["addr"]:0x4000 + e["addr"] + e["len"]]
            res[f"{e['name']} data crc"] = crc16(d) == e["dcrc"]
    return res


# ---------------------------------------------------------------- commands
def load(path):
    raw = Path(path).read_bytes()
    L, tag = fwsc_to_logical(raw)
    u = ufw_parse(L)
    return raw, L, tag, u


def package_checks(L, u):
    """Every CRC of a package (UFW header and list, flash.bin and its JLFS entries, ota.bin, tail.bin) as {name: ok}."""
    ok = ufw_checks(L, u)
    for e in u["ents"]:
        d = L[e["off"]:e["off"] + e["len"]]
        if e["name"] == "flash.bin":
            ok["flash.bin crc"] = crc16(d) == e["crc"]
            dec = flash_decrypt(d)
            ok.update(flash_checks(dec, flash_parse(dec)))
        if e["name"] in ("ota.bin", "tail.bin"):
            ok[f"{e['name']} crc"] = crc16(d) == e["crc"]
    return ok


def cmd_info(a):
    raw, L, tag, u = load(a.pkg)
    print(f"{a.pkg}: {len(raw)} B, identity {tag!r}, sha256 {hashlib.sha256(raw).hexdigest()[:16]}")
    print(f"UFW: chip {u['hdr'][16:22].decode()}, total {u['total']:#x}, {u['count']} entries")
    for e in u["ents"]:
        d = L[e["off"]:e["off"] + e["len"]]
        print(f"  {e['name']:15s} type {e['type']:#06x} off {e['off']:#08x} len {e['len']:#08x} crc {e['crc']:04x}")
        if e["name"] == "flash.bin":
            dec = flash_decrypt(d)
            fe = flash_parse(dec)
            for f in fe:
                print(f"      {f['name']:15s} addr {f['addr']:#010x} len {f['len']:#08x} attr {f['attr']:#04x}")
            app = fe[1]
            a_ = dec[0x4000 + app["addr"]:0x4000 + app["addr"] + app["len"]]
            import re
            ver = [m.group().decode() for m in re.finditer(rb"[A-Za-z]+X?_\d{3}\b", a_)][:3]
            print(f"      app version strings: {ver}")
        if e["name"] == "script.ver":
            print(f"      -> {sfc(d, e['off']).decode('latin1')!r}")
    ok = package_checks(L, u)
    bad = [k for k, v in ok.items() if not v]
    for k, v in ok.items():
        print(f"  [{'OK' if v else 'FAIL'}] {k}")
    print("ALL CHECKS PASS" if not bad else f"{len(bad)} CHECK(S) FAILED")
    return 0 if not bad else 1


def cmd_unpack(a):
    raw, L, tag, u = load(a.pkg)
    out = Path(a.outdir)
    (out / "entries").mkdir(parents=True, exist_ok=True)
    man = dict(source=os.path.basename(a.pkg), sha256=hashlib.sha256(raw).hexdigest(), tag=tag, entries=[])
    for e in u["ents"]:
        d = L[e["off"]:e["off"] + e["len"]]
        (out / "entries" / e["name"]).write_bytes(d)
        man["entries"].append({k: e[k] for k in ("i", "type", "idx", "crc", "off", "len", "alloc", "name")})
        if e["name"] in ("isd_config.ini", "script.ver", "blimit.bin"):
            (out / (e["name"] + ".dec")).write_bytes(sfc(d, e["off"]))
        if e["name"] == "flash.bin":
            dec = flash_decrypt(d)
            (out / "flash_dec.bin").write_bytes(dec)
            fe = flash_parse(dec)
            man["jlfs"] = fe
            for f in fe[1:]:
                if f["dcrc"] != 0xFFFF:
                    (out / f["name"]).write_bytes(dec[0x4000 + f["addr"]:0x4000 + f["addr"] + f["len"]])
    (out / "logical.bin").write_bytes(L)
    (out / "manifest.json").write_text(json.dumps(man, indent=1))
    print(f"unpacked {a.pkg} ({tag}) to {out}/  -> edit {out}/app.bin, then: pack {out} NEW.fwsc")


def cmd_pack(a):
    src = Path(a.outdir)
    man = json.loads((src / "manifest.json").read_text())
    L = bytearray((src / "logical.bin").read_bytes())
    u = ufw_parse(L)
    fe_ent = next(e for e in u["ents"] if e["name"] == "flash.bin")
    flash = bytearray(L[fe_ent["off"]:fe_ent["off"] + fe_ent["len"]])
    dec_orig = flash_decrypt(bytes(flash))
    dec = bytearray(dec_orig)
    changed = []
    for f in man["jlfs"][1:]:
        p = src / f["name"]
        if f["dcrc"] == 0xFFFF or not p.exists():
            continue
        new = p.read_bytes()
        if len(new) != f["len"]:
            raise SystemExit(f"{f['name']}: {len(new)} B, must stay {f['len']} B (same-size edits only)")
        lo = 0x4000 + f["addr"]
        if new != dec[lo:lo + f["len"]]:
            changed.append(f["name"])
            dec[lo:lo + f["len"]] = new
            # data crc -> entry, entry crc
            o = f["o"]
            struct.pack_into("<H", dec, o + 2, crc16(new))
            struct.pack_into("<H", dec, o, crc16(dec[o + 2:o + 32]))
    if changed:
        # app_area_head data crc, then its entry crc
        hl = struct.unpack_from("<I", dec, 0x4008)[0]
        struct.pack_into("<H", dec, 0x4002, crc16(dec[0x4020:0x4000 + hl]))
        struct.pack_into("<H", dec, 0x4000, crc16(dec[0x4002:0x4020]))
        nblk = 0
        for o in range(0x4000, len(dec), 32):
            if dec[o:o + 32] != dec_orig[o:o + 32]:
                flash[o:o + 32] = sfc_block(dec[o:o + 32], o - 0x4000)
                nblk += 1
        print(f"changed: {', '.join(changed)}; {nblk} flash blocks re-encrypted")
    else:
        print("no change in the app area")
    L[fe_ent["off"]:fe_ent["off"] + fe_ent["len"]] = flash
    # flash.bin crc in its UFW entry, re-encrypt entry, list crc, header crc
    e = bytearray(fe_ent["raw"])
    struct.pack_into("<H", e, 4, crc16(flash))
    L[0x40 + fe_ent["i"] * 0x50:0x40 + (fe_ent["i"] + 1) * 0x50] = enc(e)
    hdr = bytearray(u["hdr"])
    struct.pack_into("<H", hdr, 2, crc16(L[0x40:0x40 + u["count"] * 0x50]))
    struct.pack_into("<H", hdr, 0, crc16(hdr[2:0x40]))
    L[0:0x40] = enc(hdr)
    tag = a.tag if a.tag is not None else man["tag"]
    out = logical_to_fwsc(bytes(L), tag)
    Path(a.out).write_bytes(out)
    same = hashlib.sha256(out).hexdigest() == man["sha256"]
    print(f"wrote {a.out}: {len(out)} B, identity {tag!r}" + ("  (identical to the original)" if same else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("info"); p.add_argument("pkg"); p.set_defaults(f=cmd_info)
    p = sp.add_parser("unpack"); p.add_argument("pkg"); p.add_argument("outdir"); p.set_defaults(f=cmd_unpack)
    p = sp.add_parser("pack"); p.add_argument("outdir"); p.add_argument("out"); p.add_argument("--tag")
    p.set_defaults(f=cmd_pack)
    a = ap.parse_args()
    sys.exit(a.f(a) or 0)


if __name__ == "__main__":
    main()
