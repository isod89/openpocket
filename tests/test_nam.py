#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# OpenPocket — the NAM engine (engine/px_nam.c, fixed point, the pedal's C path) against the float reference
# (tools/nam_a2.py), with each factory model's weights (engine/px_nam_factory.h): DI clip (docs/nam_di.wav) at the
# pedal's input level, several block sizes, the rings in one piece and in two. Wants SNR > 80 dB. Modes 2..8 (the
# vector unit beside the CPU, a second core, simulated: tests/engine/nam_test.c) must give exactly the same output as the CPU alone.
#   python3 tests/test_nam.py build/nam_test
import os, re, subprocess, sys, wave
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import nam_a2  # noqa: E402

exe = sys.argv[1]
out = os.path.join(ROOT, "build", "nam")
os.makedirs(out, exist_ok=True)
src = open(os.path.join(ROOT, "engine", "px_nam_factory.h")).read()
names = re.search(r"PXN_FNAME\[PX_NAM_NFACT\] = \{(.*?)\};", src).group(1)
names = re.findall(r'"([^"]*)"', names)
body = src[re.search(r"PXN_FW\[\w+\]\[PXN_NW\] = \{", src).start():]   # (since test build 28 PXN_NROWS: every model is still in the
                                                                        # text, each inside its #if PXN_KB(i))
models = [np.array([float(v.rstrip("f")) for v in re.findall(r"[-+0-9.eE]+f", m)], np.float32)
          for m in re.findall(r"\{([^{}]*)\}", body)]
w = wave.open(os.path.join(ROOT, "docs", "nam_di.wav"))
x = (np.frombuffer(w.readframes(w.getnframes()), np.int16) / 32768.0).astype(np.float32)[:44100]
x = x / 0.15 * 0.5                                     # about the level the engine feeds the network (rc_nam)
x.tofile(os.path.join(out, "in.f32"))
bad = 0
for k, (name, wt) in enumerate(zip(names, models)):
    if len(wt) != 1871:
        print("%-10s %d weights" % (name, len(wt))); bad += 1; continue
    wt.tofile(os.path.join(out, "w.f32"))
    ref = nam_a2.run(nam_a2.unpack(wt.astype(np.float64), 3), x.astype(np.float64))
    worst = 1e9
    outs = {}
    runs = [(64, 0, 0), (37, 1, 0), (1, 0, 0)] + [(64, 0, md) for md in range(1, 11)] + [(37, 1, 4), (37, 1, 6)]
    for blk, split, pipe in runs:
        env = dict(os.environ)
        if split: env["NAM_SPLIT"] = "1"
        if pipe: env["NAM_PIPE"] = str(pipe)
        subprocess.run([exe, os.path.join(out, "w.f32"), os.path.join(out, "in.f32"), os.path.join(out, "out.f32"),
                        str(blk)], check=True, env=env)
        y = np.fromfile(os.path.join(out, "out.f32"), np.float32)
        outs[blk, pipe] = y
        snr = 10 * np.log10(np.sum(ref ** 2) / max(np.sum((y - ref) ** 2), 1e-30))
        worst = min(worst, snr)
    for srv in (1, 2):                                 # the two-core pipeline's mode: CPU1 serving, eager or late
        subprocess.run([exe, os.path.join(out, "w.f32"), os.path.join(out, "in.f32"), os.path.join(out, "out.f32"),
                        "128"], check=True, env=dict(os.environ, NAM_SRV=str(srv)))
        outs[128, "s%d" % srv] = np.fromfile(os.path.join(out, "out.f32"), np.float32)
    for pipe in (0, 5):                                # the CPU's layers through gathered X rows instead of rdot
        env = dict(os.environ, NAM_GATHER="1", **({"NAM_PIPE": str(pipe)} if pipe else {}))
        subprocess.run([exe, os.path.join(out, "w.f32"), os.path.join(out, "in.f32"), os.path.join(out, "out.f32"),
                        "64"], check=True, env=env)
        outs[64, "g%d" % pipe] = np.fromfile(os.path.join(out, "out.f32"), np.float32)
    same = all(np.array_equal(outs[b, 0], outs[b, p]) for b, _, p in runs if p) and \
        np.array_equal(outs[64, 0], outs[64, "g0"]) and np.array_equal(outs[64, 0], outs[64, "g5"]) and \
        np.array_equal(outs[64, 0], outs[128, "s1"]) and np.array_equal(outs[64, 0], outs[128, "s2"])
    ok = worst > 80 and same
    if not same: print("%-10s modes 2..8 differ from the CPU alone" % name)
    bad += not ok
    print("%-10s fixed point vs float: SNR %.1f dB  %s" % (name, worst, "ok" if ok else "FAIL"))
print("NAM: %d models, %s" % (len(models), "all ok" if not bad else "%d FAILED" % bad))
sys.exit(1 if bad else 0)
