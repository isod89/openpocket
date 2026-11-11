#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""OpenPocket - the NAM network's post5 routine (pi32v2 assembly in engine/px_nam.c, written by tools/gen_rdq.py --post5)
checked on the PC. The pedal's own start-up check compares it with the C post and falls back if they differ; this test
runs the assembly text itself through a small model of the instructions it uses, so that a mistake shows before an
image is flashed:
  - the text in px_nam.c is the generator's output;
  - on random jobs (2 to 32 frames, values up to the 32-bit limits) hs and out are exactly pxn_post_c's, z is unchanged;
  - no word outside the job's z, hs, x, out, residual and the routine's own stack frame is read or written;
  - a paired memory step never reads or loads a register its own instruction writes, and uses r0..r7 only (base r0..r7
    with an offset of 0..60 or a step of 4, or sp with 0..252): the forms the assembler takes;
  - every value loaded in the loop is used 2 or more instructions later (a use right after the load waits a cycle).
The model's costs are the pedal's, measured for code in a hardware loop (which fetches nothing): load 2, store 1.5,
multiply and multiply-add 2 (a paired memory step is then free), other instructions 1 (a paired step then costs its own
time), taken branch 6.35. Usage: test_post5.py [OLD NEW] (with OLD and NEW: the first OLD in the text replaced, which
must make the test fail: the test of the test)."""
import os, re, subprocess, sys, random
HERE = os.path.dirname(os.path.abspath(__file__))
def strip(text):
    out = []
    for ln in text.split("\n"):
        m = re.match(r'\s*"(.*)\\n"$', ln)
        if m: out.append(m.group(1).replace("\\t", "").strip())
    return out
gen = strip(subprocess.run([sys.executable, os.path.join(HERE, "..", "tools", "gen_rdq.py"), "--post5"], capture_output=True, text=True, check=True).stdout)
src = open(os.path.join(HERE, "..", "engine", "px_nam.c")).read()
a = src.index('    "\\t.global pxn_post5_asm\\n"'); b = src.index('    "\\t{pc, r15-r4} = [sp++]\\n"', a)
lines = strip(src[a:src.index("\n", b) + 1])
if lines != gen: raise SystemExit("post5: the routine in engine/px_nam.c is not tools/gen_rdq.py --post5's output")
if len(sys.argv) == 3: lines = [l.replace(sys.argv[1], sys.argv[2], 1) if sys.argv[1] in l else l for l in lines]
M32 = 0xFFFFFFFF
def s32(v): v &= M32; return v - (1 << 32) if v & 0x80000000 else v
def s64(v): v &= (1 << 64) - 1; return v - (1 << 64) if v >> 63 else v
class Emu:
    def __init__(self, mem, allowed_r, allowed_w):
        self.r = [0] * 16; self.sp = 0x70000000; self.mem = mem; self.ar = allowed_r; self.aw = allowed_w
        self.stack_lo = self.sp - 60 - 4 * 13; self.cyc = 0.0; self.loaded = {}; self.warn = []; self.n = 0
    def rd(self, a):
        assert a % 4 == 0, hex(a)
        if not (self.stack_lo <= a < 0x70000000 or a in self.ar): raise SystemExit("read outside the job: %x" % a)
        return self.mem.get(a, 0xDEAD0000 + (a & 0xFFFF))
    def wr(self, a, v):
        if not (self.stack_lo <= a < 0x70000000 or a in self.aw): raise SystemExit("write outside the job: %x" % a)
        self.mem[a] = v & M32
    def reg(self, t): return 16 if t == "sp" else int(t[1:])
    def get(self, i): return self.sp if i == 16 else self.r[i]
    def setr(self, i, v):
        if i == 16: self.sp = v & M32
        else: self.r[i] = v & M32
    def addr(self, e):                                  # "[r1+4]" "[r1++=4]" "[sp+8]" -> (address, base, post-increment)
        m = re.match(r"\[(r\d+|sp)(\+\+=|\+)(-?\d+)\]$", e)
        b = self.reg(m.group(1)); k = int(m.group(3))
        return (self.get(b), b, k) if m.group(2) == "++=" else ((self.get(b) + k) & M32, b, 0)
    def step_a(self, s):
        """the computing part: returns (writes {reg: value}, reads set, cost, branch label or None)"""
        m = re.match(r"r(\d+)_r(\d+) (\+?=) r(\d+) \* r(\d+) \(s\)$", s)
        if m:
            hi, lo, op, x, y = int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4)), int(m.group(5))
            p = s32(self.r[x]) * s32(self.r[y])
            rds = {x, y}
            if op == "+=": p += s64(self.r[hi] << 32 | self.r[lo]); rds |= {hi, lo}
            return {hi: (p >> 32) & M32, lo: p & M32}, rds, 2.0, None
        m = re.match(r"r(\d+)_r(\d+) >>= (\d+)$", s)
        if m:
            hi, lo, k = int(m.group(1)), int(m.group(2)), int(m.group(3))
            v = s64(self.r[hi] << 32 | self.r[lo]) >> k
            return {hi: (v >> 32) & M32, lo: v & M32}, {hi, lo}, 1.0, None
        m = re.match(r"r(\d+) = smax\(r(\d+), r(\d+)\)$", s)
        if m: d, x, y = map(int, m.groups()); return {d: max(s32(self.r[x]), s32(self.r[y]))}, {x, y}, 1.0, None
        m = re.match(r"(r\d+|sp) \+= (r\d+|-?\d+)$", s)
        if m:
            d = self.reg(m.group(1))
            if m.group(2)[0] == "r": y = int(m.group(2)[1:]); return {d: self.get(d) + self.r[y]}, {d, y}, 1.0, None
            return {d: self.get(d) + int(m.group(2))}, {d}, 1.0, None
        m = re.match(r"r(\d+) = r(\d+) ([-+*]) (r\d+|-?\d+)$", s)
        if m:
            d, x, op, y = int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
            yv, rds = (self.r[int(y[1:])], {x, int(y[1:])}) if y[0] == "r" else (int(y), {x})
            v = self.r[x] + yv if op == "+" else self.r[x] - yv if op == "-" else s32(self.r[x]) * s32(yv)
            return {d: v}, rds, 2.0 if op == "*" else 1.0, None
        m = re.match(r"r(\d+) = r(\d+)$", s)
        if m: return {int(m.group(1)): self.r[int(m.group(2))]}, {int(m.group(2))}, 1.0, None
        m = re.match(r"r(\d+) = (-?\d+)$", s)
        if m: return {int(m.group(1)): int(m.group(2))}, set(), 1.0, None
        m = re.match(r"if \(r(\d+) != r(\d+)\) goto (\S+)$", s)
        if m:
            x, y = int(m.group(1)), int(m.group(2))
            return {}, {x, y}, 6.35 if self.r[x] != self.r[y] else 1.0, m.group(3) if self.r[x] != self.r[y] else None
        return None
    def step_mem(self, s, paired, awrites=frozenset()):
        """a load or store: returns (reg writes, reg reads, memory writes [(addr, value)], cost)"""
        m = re.match(r"r(\d+) = (\[.*\])$", s)
        if m:
            d = int(m.group(1)); a, b, inc = self.addr(m.group(2))
            if paired:
                assert d < 8 and (b < 8 or b == 16), "paired load: " + s
                k = int(re.search(r"(-?\d+)\]$", m.group(2)).group(1))
                assert (0 <= k <= 60 and b < 8) or (0 <= k <= 252 and b == 16) or inc == 4, "paired load range: " + s
            w = {d: self.rd(a)}
            if inc: w[b] = self.get(b) + inc
            return w, {b}, [], 2.0, d
        m = re.match(r"(\[.*\]) = r(\d+)$", s)
        if m:
            x = int(m.group(2)); a, b, inc = self.addr(m.group(1))
            if paired:
                assert x < 8 and (b < 8 or b == 16), "paired store: " + s
            w = {b: self.get(b) + inc} if inc else {}
            return w, {b, x}, [(a, self.r[x])], 1.5, None
        return None
    def run(self, lines, job):
        labels = {l[:-1]: i for i, l in enumerate(lines) if l.endswith(":")}
        pc = labels["pxn_post5_asm"]; self.r[0] = job
        turn0 = None
        while True:
            s = lines[pc]; pc += 1
            if s.endswith(":") or s.startswith(".global"):
                if s == ".Lpxn_p5f:": self.turns = getattr(self, "turns", 0) + 1; self.cyc_loop0 = getattr(self, "cyc_loop0", self.cyc)
                continue
            self.n += 1
            if s == "[--sp] = {rets, r15-r4}": self.saved = self.r[4:16]; self.sp -= 4 * 13; continue
            if s == "{pc, r15-r4} = [sp++]":
                assert self.sp == 0x70000000 - 4 * 13, "stack not restored"
                assert self.r[4:16] is not None
                self.cyc_end = self.cyc
                return
            a, _, b = s.partition("  #  ")
            ra = self.step_a(a)
            if ra is None:
                assert not b, "a memory step paired with a memory step: " + s
                w, rds, mw, cost, ld = self.step_mem(a, False)
                for q in rds: self.use(q, s)
                for k, v in w.items(): self.setr(k, v)
                for ad, v in mw: self.wr(ad, v)
                if ld is not None: self.loaded[ld] = self.n
                self.cyc += cost; continue
            aw, ards, acost, br = ra
            for q in ards: self.use(q, s)
            cost = acost
            if b:
                w, rds, mw, mcost, ld = self.step_mem(b, True)
                assert not (rds & set(aw)), "the paired step reads a register its instruction writes: " + s
                assert not (set(w) & set(aw)), "the paired step writes a register its instruction writes: " + s
                for q in rds: self.use(q, s)
                for ad, v in mw: self.wr(ad, v)
                for k, v in aw.items(): self.setr(k, v)
                for k, v in w.items(): self.setr(k, v)
                if ld is not None: self.loaded[ld] = self.n
                cost = acost if acost >= 2.0 else mcost
            else:
                for k, v in aw.items(): self.setr(k, v)
            for k in aw: self.loaded.pop(k, None)
            self.cyc += cost
            if br: pc = labels[br]
    def use(self, q, s):
        if q in self.loaded and self.n - self.loaded[q] < 2 and getattr(self, "turns", 0): self.warn.append("loaded 1 instruction before its use: " + s)
def ref(z, x, hs, out, rr2, n):
    """pxn_post_c with the residual from rr2 and x 3 words a frame"""
    lre = lambda v: (v * 42949673) >> 32 if v < 0 else v
    for f in range(n):
        a = [lre(z[3 * f + c]) for c in range(3)]
        for c in range(3): hs[3 * f + c] = s32(hs[3 * f + c] + a[c])
        for o in range(3):
            v = sum(rr2[3 * c + o] * a[c] for c in range(3))
            out[3 * f + o] = s32(x[3 * f + o] + s32(v >> 24) + rr2[9 + o])
rnd = random.Random(52)
worst = None
for trial in range(400):
    n = rnd.choice([2, 2, 4, 6, 8, 12, 32])
    big = trial % 5 == 0
    val = lambda: rnd.choice([0, -1, 1, 0x7FFFFFFF, -0x80000000]) if big and rnd.random() < 0.3 else rnd.randint(-2**31, 2**31 - 1) >> rnd.choice([0, 4, 6, 12])
    z = [val() for _ in range(3 * n)]; x = [val() for _ in range(3 * n)]; hs = [val() for _ in range(3 * n)]
    rr2 = [val() for _ in range(12)]
    bz, bx, bh, bo, br, bj = 0x10000000 + 4 * rnd.randint(0, 999), 0x20000000 + 4 * rnd.randint(0, 999), 0x30000000 + 4 * rnd.randint(0, 999), 0x40000000 + 4 * rnd.randint(0, 999), 0x50000000, 0x60000000
    mem = {}
    for i, v in enumerate(z): mem[bz + 4 * i] = v & M32
    for i, v in enumerate(x): mem[bx + 4 * i] = v & M32
    for i, v in enumerate(hs): mem[bh + 4 * i] = v & M32
    for i, v in enumerate(rr2): mem[br + 4 * i] = v & M32
    for i, v in enumerate([bz, bx, 12, 0x66660000, bh, bo, n, br]): mem[bj + 4 * i] = v & M32
    ar = set(mem); aw = {bh + 4 * i for i in range(3 * n)} | {bo + 4 * i for i in range(3 * n)}
    e = Emu(mem, ar, aw)
    e.r = [rnd.randint(0, M32) for _ in range(16)]
    e.run(lines, bj)
    h2, o2 = list(hs), [0] * (3 * n)
    ref(z, x, h2, o2, rr2, n)
    gh = [s32(mem[bh + 4 * i]) for i in range(3 * n)]; go = [s32(mem.get(bo + 4 * i, 0x5A5A5A5A)) for i in range(3 * n)]
    if gh != h2 or go != o2: raise SystemExit("trial %d (n %d): DIFFERENT\n hs %s\n    %s\n out %s\n     %s" % (trial, n, gh[:6], h2[:6], go[:6], o2[:6]))
    for i, v in enumerate(z): assert mem[bz + 4 * i] == v & M32, "z changed"
    if e.warn: raise SystemExit("\n".join(sorted(set(e.warn))))
    if n == 32: worst = (e.cyc_end - e.cyc_loop0) / 16.0, e.cyc_loop0
print("post5 ok: 400 random jobs exactly as pxn_post_c, no word outside the job touched; %.1f cycles a turn of 2 frames by the pedal's costs" % worst[0])
