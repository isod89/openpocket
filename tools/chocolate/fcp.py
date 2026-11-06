#!/usr/bin/env python3
"""Decode and build M-VAVE Chocolate Plus .fcp files. Python 3 standard library only.

  fcp.py decode FILE                  print the settings in FILE
  fcp.py build MAP.json EXPORT OUT    write OUT from your own CubeSuite export and a mapping
  fcp.py diff A B                     list every byte that differs
  fcp.py check EXPORT                 decode, re-encode, and compare to EXPORT

An .fcp file is a raw copy of 23,646 bytes of the Chocolate's settings memory. It has no
header, length field or checksum: CubeSuite reads the file and writes it to the Chocolate
unchanged.

Two layouts apply from offset 0x5d onward, and they disagree:
  - CubeSuite (Mac and Android): 8 groups x 4 keys x 417 bytes.
  - The Chocolate, as read back in an export: 4 Mode 4 records of 561 bytes, then 64
    Mode 5 records of 281 bytes, ending at 0x4f61. Later bytes appear to be live RAM.
This tool writes the Chocolate's layout, because the Chocolate is what acts on the bytes.
"""
import argparse, json, sys
from dataclasses import dataclass, field
from pathlib import Path

SIZE = 0x5C5E
TAP_AT, ADV1_AT, ADV2_AT, DEVICE_END = 0x0D, 0x5D, 0x921, 0x4F61
ADV1_LEN, ADV2_LEN, LIST_LEN, ENTRY_LEN = 561, 281, 16, 5
TYPES = ["PC", "CC", "Note ON", "Note OFF", "SysEx"]
APP_MODES = ["Program change A", "Program change B", "Custom mode", "Advanced custom mode",
             "Manufacturer control", "Touch screen (Android)", "Video Model", "Keyboard A",
             "Keyboard B", "MultiMedia keyboard", "Custom keyboard mode", "Mix key", "Speark mode"]
SUBMODES = ["Single step (single Bank)", "Single step (switch between two Banks)",
            "Press down - release", "Long step", "Step short or step long"]
KEYS = "ABCD"
MARK = b"FootCtrlPlus"  # the Chocolate's Bluetooth name, in the RAM after the settings
MODE4, MODE5, TWO_BANKS = 3, 4, 1


@dataclass
class Entry:
    enabled: int = 0
    channel: int = 0  # stored as MIDI channel minus 1
    type: int = 0
    data1: int = 0
    data2: int = 0

    def pack(self):
        return bytes([self.enabled, self.channel, self.type, self.data1, self.data2])

    def text(self):
        kind = TYPES[self.type] if self.type < len(TYPES) else f"type {self.type}"
        return f"ch {self.channel + 1} {kind} {self.data1} {self.data2}"


def read_list(b):
    return [Entry(*b[i:i + ENTRY_LEN]) for i in range(0, LIST_LEN * ENTRY_LEN, ENTRY_LEN)]


def pack_list(entries):
    return b"".join(e.pack() for e in entries)


@dataclass
class Adv1:
    """Mode 4 key: sub-mode, list A, list B, then two 200-byte SysEx areas."""
    submode: int
    a: list
    b: list
    sysex: bytes

    @classmethod
    def read(cls, r):
        return cls(r[0], read_list(r[1:81]), read_list(r[81:161]), r[161:])

    def pack(self):
        return bytes([self.submode]) + pack_list(self.a) + pack_list(self.b) + self.sysex


@dataclass
class Adv2:
    """Mode 5 key: one flag byte (meaning unknown), one list, one 200-byte SysEx area."""
    flag: int
    entries: list
    sysex: bytes

    @classmethod
    def read(cls, r):
        return cls(r[0], read_list(r[1:81]), r[81:])

    def pack(self):
        return bytes([self.flag]) + pack_list(self.entries) + self.sysex


@dataclass
class Fcp:
    mode: int
    trs: int
    channel: int
    usr: bytes
    tap: list
    adv1: list = field(default_factory=list)
    adv2: list = field(default_factory=list)
    tail: bytes = b""  # RAM after the settings; always carried through unchanged

    @classmethod
    def parse(cls, d):
        if len(d) != SIZE:
            sys.exit(f"expected {SIZE} bytes, found {len(d)}")
        return cls(d[0], d[1], d[2], d[3:TAP_AT], read_list(d[TAP_AT:ADV1_AT]),
                   [Adv1.read(d[ADV1_AT + i * ADV1_LEN:ADV1_AT + (i + 1) * ADV1_LEN]) for i in range(4)],
                   [Adv2.read(d[ADV2_AT + i * ADV2_LEN:ADV2_AT + (i + 1) * ADV2_LEN]) for i in range(64)],
                   d[DEVICE_END:])

    def pack(self):
        return (bytes([self.mode, self.trs, self.channel]) + self.usr + pack_list(self.tap)
                + b"".join(r.pack() for r in self.adv1) + b"".join(r.pack() for r in self.adv2)
                + self.tail)


def cc(channel, number, value):
    for name, v, top in (("channel", channel, 16), ("CC", number, 127), ("value", value, 127)):
        if not (1 if name == "channel" else 0) <= v <= top:
            sys.exit(f"{name} {v} is out of range")
    return Entry(1, channel - 1, 1, number, value)


def build(export, m):
    # A failed CubeSuite read exports stale app memory at the right size, with no error.
    if MARK not in export:
        sys.exit(f"no {MARK.decode()} string: this is not a read from the Chocolate, so export it again")
    f = Fcp.parse(export)
    ch = m["channel"]
    if m["mode"] == "mode4":
        f.mode = MODE4
        for key, s in m["switches"].items():
            r = f.adv1[KEYS.index(key)]
            r.submode = TWO_BANKS
            r.a[0] = cc(ch, s["cc"], s.get("first", 127))
            r.b[0] = cc(ch, s["cc"], s.get("second", 0))
    elif m["mode"] == "mode5":
        f.mode = MODE5
        # Silence every Mode 5 key first: the factory lists send PC 0, which loads a preset.
        for r in f.adv2:
            r.flag = 0
            for e in r.entries:
                e.enabled = 0
        for g, group in enumerate(m["groups"]):
            for key, s in group.items():
                f.adv2[g * 4 + KEYS.index(key)].entries[0] = cc(ch, s["cc"], s.get("value", 0))
    else:
        sys.exit("mode must be mode4 or mode5")
    return f.pack()


def decode(d):
    f = Fcp.parse(d)
    if MARK not in d:
        print(f"WARNING: no {MARK.decode()} string, so this is probably not a read from the Chocolate")
    name = APP_MODES[f.mode] if f.mode < len(APP_MODES) else "unknown"
    print(f"size {len(d)} bytes")
    print(f"mode byte {f.mode}: CubeSuite shows '{name}'"
          + ("; the manual's order makes this Mode 5" if f.mode == MODE5 else ""))
    print(f"interface {'TRS-MIDI' if f.trs else 'Expression Pedal'}, Custom mode channel {f.channel + 1}")
    print("Custom mode list indexes:", {k: f.usr[i * 2 + 1] for i, k in enumerate("ABCDI")})
    print("Mode 4 keys:")
    for k, r in zip(KEYS, f.adv1):
        sub = SUBMODES[r.submode] if r.submode < len(SUBMODES) else f"sub-mode {r.submode}"
        show = lambda l: "; ".join(e.text() for e in l if e.enabled) or "empty"
        print(f"  {k}: {sub} | A: {show(r.a)} | B: {show(r.b)}")
    print("Mode 5 keys with an enabled message:")
    for i, r in enumerate(f.adv2):
        live = [e.text() for e in r.entries if e.enabled]
        if live:
            print(f"  group {i // 4 + 1} {KEYS[i % 4]}: {'; '.join(live)}" + (f" (flag {r.flag})" if r.flag else ""))
    t = d[0x5C55:]
    print(f"tail bytes, read by CubeSuite as settings: group dropdown {min(t[3], 7) + 1}, "
          f"Polarity Reversal {'on' if t[5] else 'off'}, Mix key page {t[8]}")


def diff(a, b):
    n = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    print(f"{len(n)} bytes differ; sizes {len(a)} and {len(b)}")
    for i in n:
        print(f"  0x{i:04x}: {a[i]:3d} -> {b[i]:3d}  {where(i)}")


def where(i):
    if i < 3:
        return ["mode", "interface", "channel"][i]
    if ADV1_AT <= i < ADV2_AT:
        k, o = divmod(i - ADV1_AT, ADV1_LEN)
        return f"Mode 4 key {KEYS[k]} " + ("sub-mode" if o == 0 else f"list {'AB'[(o - 1) // 80]} byte {(o - 1) % 80}")
    if ADV2_AT <= i < DEVICE_END:
        k, o = divmod(i - ADV2_AT, ADV2_LEN)
        return f"Mode 5 group {k // 4 + 1} key {KEYS[k % 4]} " + ("flag" if o == 0 else f"list byte {o - 1}")
    return "outside the expected fields"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest="cmd", required=True)
    s.add_parser("decode").add_argument("file")
    b = s.add_parser("build")
    b.add_argument("map")
    b.add_argument("export")
    b.add_argument("out")
    c = s.add_parser("diff")
    c.add_argument("a")
    c.add_argument("b")
    s.add_parser("check").add_argument("export")
    a = p.parse_args()
    if a.cmd == "decode":
        decode(Path(a.file).read_bytes())
    elif a.cmd == "build":
        out = Path(a.out)
        if out.resolve() == Path(a.export).resolve():
            sys.exit("refusing to overwrite the export")
        out.write_bytes(build(Path(a.export).read_bytes(), json.loads(Path(a.map).read_text())))
        print(f"wrote {out}")
    elif a.cmd == "diff":
        diff(Path(a.a).read_bytes(), Path(a.b).read_bytes())
    else:
        d = Path(a.export).read_bytes()
        same = Fcp.parse(d).pack() == d
        print("round trip byte-identical" if same else "round trip FAILED")
        sys.exit(0 if same else 1)


if __name__ == "__main__":
    main()
