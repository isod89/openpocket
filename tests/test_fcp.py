#!/usr/bin/env python3
"""tools/chocolate/fcp.py against a made-up export, so the tests need no device's memory image."""
import json, random, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools/chocolate"
sys.path.insert(0, str(TOOL))
import fcp


def ok(cond, name):
    print(f"{name:<72} {'ok' if cond else 'FAIL'}")
    if not cond:
        sys.exit(1)


def run(*args):
    return subprocess.run([sys.executable, str(TOOL / "fcp.py"), *map(str, args)], capture_output=True, text=True)


def changed(a, b):
    return [i for i in range(len(a)) if a[i] != b[i]]


rng = random.Random(1)
export = bytearray(rng.randrange(256) for _ in range(fcp.SIZE))
export[fcp.DEVICE_END + 16:fcp.DEVICE_END + 16 + len(fcp.MARK)] = fcp.MARK
export = bytes(export)

with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    src, out = d / "export.fcp", d / "out.fcp"
    src.write_bytes(export)

    ok(run("check", src).returncode == 0, "an export survives decode and re-encode byte for byte")

    today = json.loads((TOOL / "today.json").read_text())
    r = run("build", TOOL / "today.json", src, out)
    built = out.read_bytes()
    ok(r.returncode == 0 and len(built) == fcp.SIZE, "today.json builds a file of the right size")
    f = fcp.Fcp.parse(built)
    ok(f.mode == fcp.MODE4, "today.json selects Mode 4")
    for key, s in today["switches"].items():
        rec = f.adv1[fcp.KEYS.index(key)]
        ok(rec.submode == fcp.TWO_BANKS, f"key {key} alternates between its two lists")
        ok(rec.a[0] == fcp.Entry(1, today["channel"] - 1, 1, s["cc"], s["first"]), f"key {key} list A sends CC {s['cc']} {s['first']}")
        ok(rec.b[0] == fcp.Entry(1, today["channel"] - 1, 1, s["cc"], s["second"]), f"key {key} list B sends CC {s['cc']} {s['second']}")
    ok(all(i == 0 or fcp.ADV1_AT <= i < fcp.ADV2_AT for i in changed(export, built)), "today.json changes only the mode and the Mode 4 records")
    ok(built[fcp.DEVICE_END:] == export[fcp.DEVICE_END:], "the bytes after the settings are carried through unchanged")

    final = json.loads((TOOL / "final.json").read_text())
    r = run("build", TOOL / "final.json", src, out)
    built = out.read_bytes()
    ok(r.returncode == 0, "final.json builds")
    f = fcp.Fcp.parse(built)
    ok(f.mode == fcp.MODE5, "final.json selects Mode 5")
    mapped = {g * 4 + fcp.KEYS.index(k): s for g, group in enumerate(final["groups"]) for k, s in group.items()}
    ok(len(mapped) == 6, "final.json maps six effects")
    for i, rec in enumerate(f.adv2):
        live = [e for e in rec.entries if e.enabled]
        want = [fcp.Entry(1, final["channel"] - 1, 1, mapped[i]["cc"], mapped[i]["value"])] if i in mapped else []
        if live != want:
            ok(False, f"Mode 5 record {i} sends only what the mapping says")
    ok(True, "every Mode 5 key sends only what the mapping says")
    ok(all(i == 0 or fcp.ADV2_AT <= i < fcp.DEVICE_END for i in changed(export, built)), "final.json changes only the mode and the Mode 5 records")

    bad = d / "bad.fcp"
    bad.write_bytes(bytes(fcp.SIZE))
    r = run("build", TOOL / "today.json", bad, out)
    ok(r.returncode != 0 and "not a read from the Chocolate" in r.stderr, "a failed read is refused")
    ok("WARNING" in run("decode", bad).stdout, "decode warns about a failed read")
    bad.write_bytes(export[:-1])
    ok(run("build", TOOL / "today.json", bad, out).returncode != 0, "a file of the wrong size is refused")
    ok(run("build", TOOL / "today.json", src, src).returncode != 0 and src.read_bytes() == export, "the export is never overwritten")
    wrong = d / "wrong.json"
    wrong.write_text(json.dumps({"mode": "mode4", "channel": 2, "switches": {"A": {"cc": 128}}}))
    ok(run("build", wrong, src, out).returncode != 0, "a CC number out of range is refused")

print("fcp test passed")
