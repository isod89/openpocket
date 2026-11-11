#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# OpenPocket — writes the rdq routines of engine/px_nam.c (pi32v2 assembly, as C string lines): a NAM layer's rows for 4
# frames straight from the history ring, unrolled for 6, 15 and 16 taps, each multiply-add using a value loaded 2 to 4
# steps before (on the pedal a multiply-add paired with the load of the value it needs next costs 3.2 cycles, without
# that wait 2.2). Paste its output into px_nam.c's asm block (the lines after "rdq6, rdq15, rdq16").
#   python3 tools/gen_rdq.py > rdq.inc        (rdq6, rdq15, rdq16)
#   python3 tools/gen_rdq.py --rdl > rdl.inc  (rdl6, rdl15, rdl16: a whole layer's groups in one call)
#   python3 tools/gen_rdq.py --rdm | --post5  (rdm6, rdm15: two rows at a time; post5: the post two frames a turn)
ACC = ["r9_r8", "r11_r10", "r13_r12", "r15_r14"]
X = ["r3", "r4", "r6", "r7"]
def gen(K):
    n = "pxn_rdq%d_asm" % K
    L = []
    a = L.append
    a("\t.global %s" % n)
    a("%s:" % n)
    a("\t[--sp] = {rets, r15-r4}")
    a("\tsp += -16")
    a("\t[sp+0] = r0")
    a("\tr7 = [r0+20]\n\t[sp+4] = r7")
    a("\tr7 = [r0+16]\n\t[sp+8] = r7")
    a("\tr7 = [r0+4]\n\t[sp+12] = r7")
    a("\tr0 = [r0+0]")
    a(".L%so:" % n)
    a("\tr4 = [sp+0]\n\tr7 = [r4+12]")
    a("\tr5 = [r0++=4]\n\tr6 = [r0++=4]")
    for f in range(4):
        a("\tr3 = [r7+%d]" % (4 * f))
        a("\t%s = r3 * r5 (s)" % ACC[f])
    a("\tr3 = 1048576")
    for f in range(4):
        a("\t%s += r3 * r6 (s)" % ACC[f])
    a("\tr1 = [sp+12]\n\tr1 = [r1+0]")
    a("\tr5 = [r0++=4]")
    for f in range(4):
        a("\t%s = [r1+%d]" % (X[f], 12 * f))
    for k in range(K):
        A = "r1" if k % 2 == 0 else "r2"
        B = "r2" if k % 2 == 0 else "r1"
        last = k == K - 1
        for c in range(3):
            if c < 2:
                base, off = A, 4 * (c + 1)
            else:
                base, off = B, 0
            for f in range(4):
                if f < 3:
                    if c == 2 and last:
                        a("\t%s += %s * r5 (s)" % (ACC[f], X[f]))
                    else:
                        a("\t%s += %s * r5 (s)  #  %s = [%s+%d]" % (ACC[f], X[f], X[f], base, off + 12 * f))
                else:
                    a("\t%s += %s * r5 (s)  #  r5 = [r0++=4]" % (ACC[f], X[f]))
            if not (c == 2 and last):
                a("\tr7 = [%s+%d]" % (base, off + 36))
            if c == 0 and not last:
                a("\t%s = [sp+12]\n\t%s = [%s+%d]" % (B, B, B, 4 * (k + 1)))
    a("\tr0 += -4")
    for f in range(4):
        a("\t%s >>= 24" % ACC[f])
    a("\tr4 = [sp+8]")
    a("\t[r4+0] = r8\n\t[r4+12] = r10\n\t[r4+24] = r12\n\t[r4+36] = r14")
    a("\tr4 += 4\n\t[sp+8] = r4")
    a("\tr7 = [sp+4]\n\tr7 += -1\n\t[sp+4] = r7")
    a("\tif (r7 != 0) goto .L%so" % n)
    a("\tsp += 16")
    a("\t{pc, r15-r4} = [sp++]")
    out = []
    for ln in "\n".join(L).split("\n"):
        out.append('    "%s\\n"' % ln.replace("\t", "\\t"))
    return "\n".join(out)
def genl(K):
    """rdl: the same rows for a whole layer's groups of 4 frames in one call (diagnostic 20): the tap table copied onto
    the stack ([sp+20+4k], one load per tap instead of two), the u and z pointers moved on in the job, and every tap's
    pointer 4 frames on after each group, brought back by the ring's length past its end (job +28 end, +32 minus the
    ring's length in bytes; the ring's first frames are mirrored after its end). [sp+12] groups left."""
    n = "pxn_rdl%d_asm" % K
    F = 20 + 4 * K
    L = []
    a = L.append
    a("\t.global %s" % n)
    a("%s:" % n)
    a("\t[--sp] = {rets, r15-r4}")
    a("\tsp += -%d" % F)
    a("\t[sp+0] = r0")
    a("\tr7 = [r0+24]\n\t[sp+12] = r7")
    a("\tr1 = [r0+4]")
    for k in range(K):
        a("\tr7 = [r1+%d]\n\t[sp+%d] = r7" % (4 * k, 20 + 4 * k))
    a(".L%sg:" % n)
    a("\tr4 = [sp+0]")
    a("\tr7 = [r4+20]\n\t[sp+4] = r7")
    a("\tr7 = [r4+16]\n\t[sp+8] = r7")
    a("\tr0 = [r4+0]")
    a(".L%so:" % n)
    a("\tr4 = [sp+0]\n\tr7 = [r4+12]")
    a("\tr5 = [r0++=4]\n\tr6 = [r0++=4]")
    for f in range(4):
        a("\tr3 = [r7+%d]" % (4 * f))
        a("\t%s = r3 * r5 (s)" % ACC[f])
    a("\tr3 = 1048576")
    for f in range(4):
        a("\t%s += r3 * r6 (s)" % ACC[f])
    a("\tr1 = [sp+20]")
    a("\tr5 = [r0++=4]")
    for f in range(4):
        a("\t%s = [r1+%d]" % (X[f], 12 * f))
    for k in range(K):
        A = "r1" if k % 2 == 0 else "r2"
        B = "r2" if k % 2 == 0 else "r1"
        last = k == K - 1
        for c in range(3):
            if c < 2:
                base, off = A, 4 * (c + 1)
            else:
                base, off = B, 0
            for f in range(4):
                if f < 3:
                    if c == 2 and last:
                        a("\t%s += %s * r5 (s)" % (ACC[f], X[f]))
                    else:
                        a("\t%s += %s * r5 (s)  #  %s = [%s+%d]" % (ACC[f], X[f], X[f], base, off + 12 * f))
                else:
                    a("\t%s += %s * r5 (s)  #  r5 = [r0++=4]" % (ACC[f], X[f]))
            if not (c == 2 and last):
                a("\tr7 = [%s+%d]" % (base, off + 36))
            if c == 0 and not last:
                a("\t%s = [sp+%d]" % (B, 20 + 4 * (k + 1)))
    a("\tr0 += -4")
    for f in range(4):
        a("\t%s >>= 24" % ACC[f])
    a("\tr4 = [sp+8]")
    a("\t[r4+0] = r8\n\t[r4+12] = r10\n\t[r4+24] = r12\n\t[r4+36] = r14")
    a("\tr4 += 4\n\t[sp+8] = r4")
    a("\tr7 = [sp+4]\n\tr7 += -1\n\t[sp+4] = r7")
    a("\tif (r7 != 0) goto .L%so" % n)
    a("\tr4 = [sp+0]")
    a("\tr7 = [r4+12]\n\tr7 += 16\n\t[r4+12] = r7")
    a("\tr7 = [r4+16]\n\tr7 += 48\n\t[r4+16] = r7")
    a("\tr2 = [r4+28]\n\tr3 = [r4+32]")
    for k in range(K):
        a("\tr1 = [sp+%d]\n\tr1 += 48\n\tr5 = r1 + r3" % (20 + 4 * k))
        a("\tif (r1 >= r2) {\n\tr1 = r5\n\t}")
        a("\t[sp+%d] = r1" % (20 + 4 * k))
    a("\tr7 = [sp+12]\n\tr7 += -1\n\t[sp+12] = r7")
    a("\tif (r7 != 0) goto .L%sg" % n)
    a("\tsp += %d" % F)
    a("\t{pc, r15-r4} = [sp++]")
    out = []
    for ln in "\n".join(L).split("\n"):
        out.append('    "%s\\n"' % ln.replace("\t", "\\t"))
    return "\n".join(out)
def genm(K):
    """rdm (v40.2): rdl's job for a layer's 3 rows, two rows at a time where it can. On the pedal a multiply-add costs
    2 cycles, and so does a load, unless it is paired with the multiply-add (then it is free). rdl reads 5 values for 4
    multiply-adds (one weight, 4 frames); here rows 0 and 1 go 2 frames at a time (2 weights, 2 frames: 4 values for 4
    multiply-adds, every load paired), row 2 as rdl. Per group of 4 frames: part A (rows 0 and 1, frames 0 and 1), part B
    (frames 2 and 3), part C (row 2, frames 0 to 3). r0 and r1 the rows' weights, r2 the tap's pointer, r3..r7 the values
    (their registers turn round: each is loaded 2 to 5 steps before its use), r8..r15 the 4 accumulators. Stack: [sp+0]
    job, [sp+4] groups left, [sp+8] z, [sp+12] u, [sp+16] weights, [sp+20+4k] the tap table."""
    n = "pxn_rdm%d_asm" % K
    T = 3 * K + 2
    F = 20 + 4 * K
    L = []
    a = L.append
    a("\t.global %s" % n)
    a("%s:" % n)
    a("\t[--sp] = {rets, r15-r4}")
    a("\tsp += -%d" % F)
    a("\t[sp+0] = r0")
    a("\tr7 = [r0+24]\n\t[sp+4] = r7")
    a("\tr7 = [r0+16]\n\t[sp+8] = r7")
    a("\tr7 = [r0+12]\n\t[sp+12] = r7")
    a("\tr7 = [r0+0]\n\t[sp+16] = r7")
    a("\tr1 = [r0+4]")
    for k in range(K):
        a("\tr7 = [r1+%d]\n\t[sp+%d] = r7" % (4 * k, 20 + 4 * k))
    a(".L%sg:" % n)
    for fa in (0, 2):                                   # parts A and B: rows 0 and 1, frames fa and fa + 1
        oa, ob = 12 * fa, 12 * (fa + 1)
        a("\tr0 = [sp+16]\n\tr1 = r0 + %d" % (4 * T))
        a("\tr2 = [sp+12]")
        a("\tr3 = [r0++=4]\n\tr4 = [r1++=4]\n\tr5 = [r2+%d]\n\tr6 = [r2+%d]" % (4 * fa, 4 * fa + 4))
        a("\tr9_r8 = r3 * r5 (s)  #  r7 = [r0++=4]")
        a("\tr11_r10 = r3 * r6 (s)  #  r3 = [r1++=4]")
        a("\tr13_r12 = r4 * r5 (s)  #  r2 = [sp+20]")
        a("\tr15_r14 = r4 * r6 (s)")
        a("\tr5 = 1048576")
        a("\tr9_r8 += r7 * r5 (s)  #  r6 = [r0++=4]")
        a("\tr13_r12 += r3 * r5 (s)  #  r4 = [r2+%d]" % ob)
        a("\tr15_r14 += r3 * r5 (s)  #  r3 = [r2+%d]" % oa)   # (r3, row 1's bias, read before its load replaces it)
        a("\tr11_r10 += r7 * r5 (s)  #  r7 = [r1++=4]")
        wa, wb, x0, x1, fr = "r6", "r7", "r3", "r4", "r5"
        for j in range(3 * K):
            k, c = divmod(j, 3)
            last = j == 3 * K - 1
            if c == 2 and not last:
                a("\tr2 = [sp+%d]" % (20 + 4 * (k + 1)))
            if last:
                a("\tr9_r8 += %s * %s (s)" % (wa, x0))
                a("\tr11_r10 += %s * %s (s)" % (wa, x1))
                a("\tr13_r12 += %s * %s (s)" % (wb, x0))
                a("\tr15_r14 += %s * %s (s)" % (wb, x1))
                break
            c2 = (c + 1) % 3
            a("\tr9_r8 += %s * %s (s)  #  %s = [r0++=4]" % (wa, x0, fr))
            a("\tr11_r10 += %s * %s (s)  #  %s = [r1++=4]" % (wa, x1, wa))
            a("\tr13_r12 += %s * %s (s)  #  %s = [r2+%d]" % (wb, x0, x0, oa + 4 * c2))
            a("\tr15_r14 += %s * %s (s)  #  %s = [r2+%d]" % (wb, x1, x1, ob + 4 * c2))
            wa, wb, fr = fr, wa, wb
        for acc in ACC:
            a("\t%s >>= 24" % acc)
        a("\tr2 = [sp+8]")
        a("\t[r2+%d] = r8\n\t[r2+%d] = r10\n\t[r2+%d] = r12\n\t[r2+%d] = r14" % (oa, ob, oa + 4, ob + 4))
    # part C: row 2, frames 0 to 3, as rdl
    a("\tr0 = [sp+16]\n\tr0 += %d" % (8 * T))
    a("\tr7 = [sp+12]")
    a("\tr5 = [r0++=4]\n\tr6 = [r0++=4]")
    for f in range(4):
        a("\tr3 = [r7+%d]" % (4 * f))
        a("\t%s = r3 * r5 (s)" % ACC[f])
    a("\tr3 = 1048576")
    for f in range(4):
        a("\t%s += r3 * r6 (s)" % ACC[f])
    a("\tr1 = [sp+20]")
    a("\tr5 = [r0++=4]")
    for f in range(4):
        a("\t%s = [r1+%d]" % (X[f], 12 * f))
    for k in range(K):
        A = "r1" if k % 2 == 0 else "r2"
        B = "r2" if k % 2 == 0 else "r1"
        last = k == K - 1
        for c in range(3):
            if c < 2:
                base, off = A, 4 * (c + 1)
            else:
                base, off = B, 0
            for f in range(4):
                if f < 3:
                    if c == 2 and last:
                        a("\t%s += %s * r5 (s)" % (ACC[f], X[f]))
                    else:
                        a("\t%s += %s * r5 (s)  #  %s = [%s+%d]" % (ACC[f], X[f], X[f], base, off + 12 * f))
                elif c == 2 and last:
                    a("\t%s += %s * r5 (s)" % (ACC[f], X[f]))
                else:
                    a("\t%s += %s * r5 (s)  #  r5 = [r0++=4]" % (ACC[f], X[f]))
            if not (c == 2 and last):
                a("\tr7 = [%s+%d]" % (base, off + 36))
            if c == 0 and not last:
                a("\t%s = [sp+%d]" % (B, 20 + 4 * (k + 1)))
    for f in range(4):
        a("\t%s >>= 24" % ACC[f])
    a("\tr4 = [sp+8]")
    a("\t[r4+8] = r8\n\t[r4+20] = r10\n\t[r4+32] = r12\n\t[r4+44] = r14")
    # the group's end: z and u on by 4 frames, every tap 4 frames on (back by the ring's length past its end)
    a("\tr4 += 48\n\t[sp+8] = r4")
    a("\tr4 = [sp+12]\n\tr4 += 16\n\t[sp+12] = r4")
    a("\tr4 = [sp+0]")
    a("\tr2 = [r4+28]\n\tr3 = [r4+32]")
    for k in range(K):
        a("\tr1 = [sp+%d]\n\tr1 += 48\n\tr5 = r1 + r3" % (20 + 4 * k))
        a("\tif (r1 >= r2) {\n\tr1 = r5\n\t}")
        a("\t[sp+%d] = r1" % (20 + 4 * k))
    a("\tr7 = [sp+4]\n\tr7 += -1\n\t[sp+4] = r7")
    a("\tif (r7 != 0) goto .L%sg" % n)
    a("\tr4 = [sp+0]")
    a("\tr7 = [sp+12]\n\t[r4+12] = r7")
    a("\tr7 = [sp+8]\n\t[r4+16] = r7")
    a("\tsp += %d" % F)
    a("\t{pc, r15-r4} = [sp++]")
    out = []
    for ln in "\n".join(L).split("\n"):
        out.append('    "%s\\n"' % ln.replace("\t", "\\t"))
    return "\n".join(out)
def genp5():
    """post5 (v40.5): post4's job (LeakyReLU, head sum, residual; x 3 words a frame, as in a ring) two frames a turn. The
    residual's 9 weights are then read once for two frames, and every load and store rides on an instruction that
    computes: free on a multiply or multiply-add (2 cycles), and on a 1-cycle instruction for the memory word's own
    time (measured on the pedal). Each value is loaded at least 2 instructions before its use (no wait). No word outside
    the job's is read. r0 z, r1 hs then out (both set from r0), r7 x; r2 r3 r6 values (r6 the slope while the
    activations are made); r5_r4 and r9_r8 the two frames' sums; r10..r15 the 6 activations. Stack: [sp+0..44] the
    residual (w[o][c] at 4(3c+o), the biases at 36+4o), [sp+48] hs - z, [sp+52] out - z, [sp+56] z's end.
    n even and > 0, out not 0 (pxn_post_part sees to both)."""
    L = []
    a = L.append
    a("\t.global pxn_post5_asm")
    a("pxn_post5_asm:")
    a("\t[--sp] = {rets, r15-r4}")
    a("\tsp += -60")
    a("\tr7 = [r0+28]")
    for k in range(12):
        a("\tr4 = [r7+%d]\n\t[sp+%d] = r4" % (4 * k, 4 * k))
    a("\tr1 = [r0+0]\n\tr2 = [r0+4]\n\tr3 = [r0+16]\n\tr4 = [r0+20]\n\tr5 = [r0+24]")
    a("\tr6 = r3 - r1\n\t[sp+48] = r6")
    a("\tr6 = r4 - r1\n\t[sp+52] = r6")
    a("\tr6 = 12\n\tr5 = r5 * r6\n\tr5 += r1\n\t[sp+56] = r5")
    a("\tr7 = r2 + -24")
    a("\tr0 = r1\n\tr1 = r3")
    a(".Lpxn_p5f:")
    a("\tr7 += 24  #  r2 = [r0+0]")
    a("\tr6 = 42949673")
    Z, H = ["r2", "r3"], ["r4", "r5"]
    # the activations: for value v, M (z * slope), X (smax -> r10+v), D (hs + a); hs is stored as r1 moves on
    def M(v, b): a("\tr9_r8 = %s * r6 (s)  #  %s" % (Z[v & 1], b))
    def X(v, b): a("\tr%d = smax(%s, r9)  #  %s" % (10 + v, Z[v & 1], b))
    def D(v, b): a("\t%s += r%d  #  %s" % (H[v & 1], 10 + v, b))
    lz = lambda v: "%s = [r0+%d]" % (Z[v & 1], 4 * v)
    lh = lambda v: "%s = [r1+%d]" % (H[v & 1], 4 if v else 0)
    sh = lambda v: "[r1++=4] = %s" % H[v & 1]
    M(0, lz(1)); X(0, lh(0)); M(1, lz(2)); D(0, lh(1)); X(1, sh(0))
    M(2, lz(3)); D(1, lh(2)); X(2, sh(1))
    M(3, lz(4)); D(2, lh(3)); X(3, sh(2))
    M(4, lz(5)); D(3, lh(4)); X(4, sh(3))
    M(5, lh(5)); D(4, "r2 = [sp+0]"); X(5, sh(4)); D(5, "r3 = [sp+12]")
    # the residual, output channel o for the two frames: P and Q hold the weights, then the two x; r6 the bias, then
    # the second frame's sum
    P, Q = "r2", "r3"
    for o in range(3):
        nx = o + 1
        if o == 0:
            a("\tr9_r8 = %s * r13 (s)  #  [r1++=4] = r5" % P)
            a("\tr5_r4 = %s * r10 (s)  #  r6 = [sp+52]" % P)
            a("\tr5_r4 += %s * r11 (s)  #  %s = [sp+24]" % (Q, P))
            a("\tr9_r8 += %s * r14 (s)  #  %s = [r7+0]" % (Q, Q))
            a("\tr1 = r0 + r6  #  r6 = [sp+36]")
        else:
            a("\tr5_r4 = %s * r10 (s)  #  [r1+%d] = r6" % (P, 12 + 4 * (o - 1)))
            a("\tr9_r8 = %s * r13 (s)  #  %s = [sp+%d]" % (P, P, 24 + 4 * o))
            a("\tr5_r4 += %s * r11 (s)  #  r6 = [sp+%d]" % (Q, 36 + 4 * o))
            a("\tr9_r8 += %s * r14 (s)  #  %s = [r7+%d]" % (Q, Q, 4 * o))
        a("\tr5_r4 += %s * r12 (s)" % P)
        a("\tr9_r8 += %s * r15 (s)  #  %s = [r7+%d]" % (P, P, 12 + 4 * o))
        a("\tr5_r4 >>= 24")
        a("\tr9_r8 >>= 24")
        a("\tr4 += r6")
        a("\tr4 += %s  #  %s = [sp+%d]" % (Q, Q, 4 * nx if o < 2 else 48))
        a("\tr6 += r8  #  [r1+%d] = r4" % (4 * o))
        a("\tr6 += %s  #  %s = [sp+%d]" % (P, P, 12 + 4 * nx if o < 2 else 56))
        P, Q = Q, P
    a("\tr0 += 24  #  [r1+20] = r6")
    a("\tr1 = r0 + %s" % P)
    a("\tif (r0 != %s) goto .Lpxn_p5f" % Q)
    a("\tsp += 60")
    a("\t{pc, r15-r4} = [sp++]")
    out = []
    for ln in "\n".join(L).split("\n"):
        out.append('    "%s\\n"' % ln.replace("\t", "\\t"))
    return "\n".join(out)
import sys
if "--post5" in sys.argv:
    print(genp5())
elif "--rdm" in sys.argv:
    print("\n".join(genm(K) for K in (6, 15)))
elif "--rdl" in sys.argv:
    print("\n".join(genl(K) for K in (6, 15, 16)))
else:
    print("\n".join(gen(K) for K in (6, 15, 16)))
