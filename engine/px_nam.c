/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — NAM A2-Lite in fixed point (see px_nam.h). Written from the published A2 architecture; checked
 * against tools/nam_a2.py (tests/test_nam.py).
 *
 * How it is computed, for this CPU (no data cache in front of the SRAM, a taken branch costs about 8 cycles, two
 * instructions can issue together): a block of up to PXN_N frames goes through the network layer by layer.
 *  - gather: each frame's inputs to the layer are copied into one row of X: [u, 1.0, then for each kernel tap its 3
 *    channels], from the layer's history ring (interleaved [time][channel], 3 words moved by one instruction);
 *  - dot: each output channel is a dot product of its weight row with the X rows, 4 frames at a time (a weight is read
 *    once for 4 frames), in the CPU's hardware loop, multiply-adds paired with loads: the 3 conv outputs, then the
 *    head the same way;
 *  - post: LeakyReLU, the head sum, the residual 1x1 into the next layer's ring.
 * The three inner routines exist twice: by hand for the pedal (pi32v2 assembly), and in C (the reference, used on the
 * PC and on the pedal if the start-up check finds the hand ones differ by a single bit). */
#include "px_nam.h"
#include <string.h>

static const uint8_t pxn_k[PXN_NL] = {6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 15, 15, 6, 6, 6, 6, 6, 6, 6};
static const uint16_t pxn_d[PXN_NL] = {1, 3, 7, 17, 41, 101, 239, 1, 3, 7, 17, 41, 101, 239, 1, 13,
                                       1, 3, 7, 17, 41, 101, 239};
#define PXN_ONE (1 << PXN_QA)

/* the block's work buffers: X rows (+ a row of slack: the dot routines read one word past a row) */
static int32_t pxn_xs[(PXN_N + 1) * PXN_S];
static int32_t *pxn_x = pxn_xs;                         /* the X rows in use: pxn_xs, or for the vector unit's modes
                                                           the copy in the fast memory (pxn_set_fast) */
static int32_t *pxn_xf, *pxn_wf;                        /* fast memory: X rows, then the weights (wa, head) for the unit */
static int32_t *pxn_fast;                               /* offered (pxn_set_fast), checked by pxn_selftest */
#define PXN_WF_HEAD 1550                                /* the head row in pxn_wf */
#define PXN_FAST_WORDS ((PXN_N + 1) * PXN_S + PXN_WF_HEAD + PXN_S + 4)
static uint8_t pxn_rd = 1;                              /* the CPU's layers by rdot (1) or gathered X rows (0): timed */
static uint32_t pxn_rdt[5];                             /* that timing (mode 0): [0] gathered, [1] rdot, [2] rdq, [3] rdl,
                                                           [4] rdm */
static uint8_t pxn_rdq_on = 1;                          /* rdot's load-ahead versions (rdq) where checked: timed */
static uint8_t pxn_rdl_on = 1;                          /* rdl: a layer's groups in one call, where checked: timed */
static uint8_t pxn_rdm_on = 1;                          /* rdm: rdl two rows at a time (the 3-row layers), where checked */
uint32_t pxn_rdot_ticks(int rd) { return rd >= 0 && rd < 5 ? pxn_rdt[rd] : 0; }
/* v37 diagnostic 18: where the CPU's time goes (mode 0, timed by pxn_tune): [0] input, [1] layers' 4-frame groups (tap
 * tables + kernel), [2] layers' 1..3-frame rests (gathered), [3] post, [4] rings + head copy + output, [5] whole block,
 * [6] head groups, [7] head rest, [8] [1] again with the kernel skipped (the tap tables alone) */
static uint32_t (*pxn_pt)(void);
static uint32_t pxn_prof[9];
static uint8_t pxn_pskip, pxn_pd = 1;
static uint8_t pxn_p3ok, pxn_pc = 4;                 /* post3 checked; frames per post call (4 with the vector unit) */
static uint32_t pxn_ptt[4];                          /* mode 0 timed with post2, post3, post4 and post5 */
static uint8_t pxn_p4ok, pxn_p4on;                   /* post4 checked; in use (where x is read from the ring) */
static uint8_t pxn_p5ok, pxn_p5on;                   /* post5 checked; in use (post4's frames, two at a time) */
uint32_t pxn_post_ticks(int v) { return v >= 0 && v < 4 ? pxn_ptt[v] : 0; }
int pxn_post3_state(void) { return pxn_p3ok ? 1 + (pxn_ptt[1] && pxn_ptt[1] < pxn_ptt[0]) : 0; }
int pxn_post4_state(void) { return pxn_p4ok ? 1 + (pxn_p4on != 0) : 0; }
int pxn_post5_state(void) { return pxn_p5ok ? 1 + (pxn_p5on != 0) : 0; }
#define PXN_T() (pxn_pt ? pxn_pt() : 0u)
uint32_t pxn_prof_ticks(int k) { return k >= 0 && k < 9 ? pxn_prof[k] : 0; }
static int32_t pxn_z[PXN_N * PXN_C + 4], pxn_hs[PXN_N * PXN_C];
static int32_t pxn_u[PXN_N + 4];                        /* the block's input (Q20), for rdot */
/* v40.2: a block's scratch as a layer uses it: z (its rows' sums), u (the block's input, Q20), hs (the head sums), and q:
 * when not 0, the layer's output goes there (3 words a frame) instead of the next layer's ring (the layer split's
 * hand-over between the cores, pxn_lp_*). The one-core and frame-split ways use pxn_sc0. */
typedef struct { int32_t *z; const int32_t *u; int32_t *hs; int32_t *q; } pxn_sc;
static const pxn_sc pxn_sc0 = {pxn_z, pxn_u, pxn_hs, 0};
static const int32_t *pxn_tpt[2][PXN_HK];               /* rdot's tap tables, one per core */

/* dot: z[3f] = (w . x_f) >> 24 for ng frames (dot4: ng groups of 4 frames), x_f the X rows sb bytes apart.
 * The hand routines read w[T] and x_f[T] (one word past): the buffers have slack. Offsets read by the hand code. */
typedef struct {
    const int32_t *w;                   /*  0 */
    const int32_t *x;                   /*  4 */
    int32_t t;                          /*  8: row length */
    int32_t sb;                         /* 12: X row stride, bytes */
    int32_t *z;                         /* 16: output, 3 words a frame */
    int32_t ng;                         /* 20: groups (> 0) */
} pxn_dot;
/* post, for n frames (> 0): a = LeakyReLU(z); hs += a; if out: out[3f+o] = x_f[o] + ((rr_o . a) >> 24) + rr_o[3],
 * x_f the layer's input in the X row (sb bytes apart) */
typedef struct {
    const int32_t *z;                   /*  0 */
    const int32_t *x;                   /*  4 */
    int32_t sb;                         /*  8 */
    const int32_t *rr;                  /* 12: 3 x {w0, w1, w2, bias (Q20)} */
    int32_t *hs;                        /* 16 */
    int32_t *out;                       /* 20: 0 = none (the last layer) */
    int32_t n;                          /* 24 */
    const int32_t *rr2;                 /* 28: the same residual for pxn_post2_asm: w[o][c] by c then o, then 3 biases */
} pxn_post;

/* rdot (v37): a layer's rows for 4 frames straight from its history ring, no gather: for each kernel tap k, tp[k]
 * points at the 3 channels of the first frame, the next frames following (12 bytes apart). The row is [mixin, bias,
 * taps x channels] as for the X rows, against [u, 1.0, taps]: the same sums, so the same result. rows: 3 (a layer, its
 * rows contiguous) or 1 (the head). Offsets read by the hand code. */
typedef struct {
    const int32_t *w;                   /*  0 */
    const int32_t *const *tp;           /*  4: K pointers */
    int32_t k;                          /*  8: taps */
    const int32_t *u;                   /* 12: the 4 frames' u */
    int32_t *z;                         /* 16: out, z[3f + o] */
    int32_t rows;                       /* 20 */
    int32_t ng;                         /* 24: rdl: groups of 4 frames */
    const int32_t *end;                 /* 28: rdl: the ring's end (its mirrored frames follow) */
    int32_t nlen;                       /* 32: rdl: minus the ring's length in bytes */
} pxn_rdot;

/* ---- the C versions (the reference) ---- */
static void pxn_dot4_c(const pxn_dot *j)
{
    const int32_t *x = j->x;
    int32_t *z = j->z;
    const int s = j->sb / 4;
    for (int g = 0; g < j->ng; g++, x += 4 * s, z += 12) {
        int64_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        for (int t = 0; t < j->t; t++) {
            const int64_t w = j->w[t];
            a0 += w * x[t]; a1 += w * x[s + t]; a2 += w * x[2 * s + t]; a3 += w * x[3 * s + t];
        }
        z[0] = (int32_t)(a0 >> PXN_QW); z[3] = (int32_t)(a1 >> PXN_QW);
        z[6] = (int32_t)(a2 >> PXN_QW); z[9] = (int32_t)(a3 >> PXN_QW);
    }
}
/* the 3 rows of a layer (contiguous, t each) for groups of 4 frames: z[3f + o] */
static void pxn_dot3x4_c(const pxn_dot *j)
{
    for (int o = 0; o < PXN_C; o++) {
        pxn_dot d = *j;
        d.w = j->w + o * j->t;
        d.z = j->z + o;
        pxn_dot4_c(&d);
    }
}
static void pxn_dot1_c(const pxn_dot *j)
{
    const int32_t *x = j->x;
    const int s = j->sb / 4;
    for (int g = 0; g < j->ng; g++, x += s) {
        int64_t a = 0;
        for (int t = 0; t < j->t; t++) a += (int64_t)j->w[t] * x[t];
        j->z[3 * g] = (int32_t)(a >> PXN_QW);
    }
}
static void pxn_rdot_c(const pxn_rdot *j)
{
    const int T = 3 * j->k + 2;
    for (int o = 0; o < j->rows; o++) {
        const int32_t *w = j->w + o * T;
        int64_t a[4];
        for (int f = 0; f < 4; f++) a[f] = (int64_t)w[0] * j->u[f] + (int64_t)w[1] * PXN_ONE;
        for (int k = 0; k < j->k; k++)
            for (int c = 0; c < PXN_C; c++)
                for (int f = 0; f < 4; f++) a[f] += (int64_t)w[2 + 3 * k + c] * j->tp[k][3 * f + c];
        for (int f = 0; f < 4; f++) j->z[3 * f + o] = (int32_t)(a[f] >> PXN_QW);
    }
}
/* rdl: ng groups of rdot, the job's u and z moved on, each tap's pointer 4 frames on (back by the ring past its end) */
static void pxn_rdl_c(pxn_rdot *j)
{
    const int32_t *tp[PXN_HK];
    for (int k = 0; k < j->k; k++) tp[k] = j->tp[k];
    pxn_rdot q = *j;
    q.tp = tp;
    for (int g = 0; g < j->ng; g++) {
        pxn_rdot_c(&q);
        q.u += 4; q.z += 12;
        for (int k = 0; k < j->k; k++) {
            const int32_t *p = tp[k] + 12;
            if (p >= j->end) p = (const int32_t *)((const char *)p + j->nlen);
            tp[k] = p;
        }
    }
    j->u = q.u; j->z = q.z;
}
static inline __attribute__((always_inline)) int32_t pxn_lrelu(int32_t z)   /* LeakyReLU 0.01 on a Q20 value */
{
    return z < 0 ? (int32_t)(((int64_t)z * 42949673) >> 32) : z;
}
static void pxn_post_c(const pxn_post *j)
{
    const int32_t *z = j->z, *x = j->x;
    int32_t *hs = j->hs, *out = j->out;
    for (int f = 0; f < j->n; f++, z += 3, hs += 3, x += j->sb / 4) {
        const int32_t a0 = pxn_lrelu(z[0]), a1 = pxn_lrelu(z[1]), a2 = pxn_lrelu(z[2]);
        hs[0] += a0; hs[1] += a1; hs[2] += a2;
        if (!out) continue;
        const int32_t *r = j->rr;
        for (int o = 0; o < PXN_C; o++, r += 4) {
            const int64_t v = (int64_t)r[0] * a0 + (int64_t)r[1] * a1 + (int64_t)r[2] * a2;
            *out++ = x[o] + (int32_t)(v >> PXN_QW) + r[3];
        }
    }
}
/* gather: n frames of 3 words, src contiguous -> dst rows sb bytes apart */
static void pxn_gcopy_c(int32_t *dst, const int32_t *src, int n, int sb)
{
    for (int f = 0; f < n; f++, src += 3, dst += sb / 4) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }
}

/* the residual reordered for post2: w[o][c] by c then o, then the 3 biases */
static void pxn_rr2(const int32_t *rr, int32_t *r2)
{
    for (int c = 0; c < PXN_C; c++)
        for (int o = 0; o < PXN_C; o++) r2[3 * c + o] = rr[4 * o + c];
    for (int o = 0; o < PXN_C; o++) r2[9 + o] = rr[4 * o + 3];
}

/* v40.2: PXN_LEAN (the Bluetooth test image, whose flash is full) leaves out the hand routines only the start-up check and
 * timing still use (rdot, rdot6, the rdq ones) and the bench screen's instruction loops: their C versions stay */
#ifndef PXN_LEAN
#define PXN_LEAN 0
#endif
/* ---- the same, by hand (v40: moved to RAM with their callers on the pedal, tools/ramcode.py --plain). v40.2: PXN_HAND
 * runs their start-up check, timing and dispatch: on the pedal; on the PC only in tests/engine/hand_test.c, which gives
 * them C stand-ins that read exactly what the hand code reads (a fault in the check shows there, not as a boot loop) */
void pxn_dot3x4_asm(const pxn_dot *j);
void pxn_dot1_asm(const pxn_dot *j);
void pxn_post_asm(const pxn_post *j);
void pxn_post2_asm(const pxn_post *j);
void pxn_post3_asm(const pxn_post *j);
void pxn_post4_asm(const pxn_post *j);
void pxn_post5_asm(const pxn_post *j);
void pxn_rdot_asm(const pxn_rdot *j);
void pxn_rdot6_asm(const pxn_rdot *j);
void pxn_rdq6_asm(const pxn_rdot *j), pxn_rdq15_asm(const pxn_rdot *j), pxn_rdq16_asm(const pxn_rdot *j);
void pxn_rdl6_asm(pxn_rdot *j), pxn_rdl15_asm(pxn_rdot *j), pxn_rdl16_asm(pxn_rdot *j);
void pxn_rdm6_asm(pxn_rdot *j), pxn_rdm15_asm(pxn_rdot *j);
void pxn_gcopy_asm(int32_t *dst, const int32_t *src, int n, int sb);
/* v40.5: where the routines that do most of the network's work lie (rdm6, rdm15, post5: 6.6 KB). On the pedal the RAM
 * code gets an instruction word in about 1.5 cycles; the flash code, through the instruction cache, one every cycle
 * (measured: sixteen 6-byte instructions of 1 cycle in a loop take 2.14 cycles each from RAM, 1.49 from the flash, 1.0
 * in a hardware loop, which fetches nothing). These loops are multiply-adds paired with loads, 6 bytes for 2 cycles of
 * work: from RAM they wait for their own instructions. They are small, run all the time and stay in the cache.
 * PXN_HOT_RAM 1 keeps them in the RAM code (the Bluetooth test image, whose radio code runs from the flash all the
 * time and has not been measured this way). */
#ifndef PXN_HOT_RAM
#define PXN_HOT_RAM 0
#endif
#if PXN_HOT_RAM
#define PXN_HOT "\t.text\n"
#else
#define PXN_HOT "\t.section .text.pxn_hot,\"ax\"\n"
#endif
#if defined(__PI32V2__) && !defined(PXN_NO_ASM)
#define PXN_ASM 1
__asm__(
    "\t.text\n"
    /* dot3x4: the 3 rows (contiguous) for 4 frames at a time. Locals: [sp+0] x, [sp+4] z, [sp+8] groups left,
     * [sp+12] job; r0 runs through the weights (back to the row start each group) */
    "\t.global pxn_dot3x4_asm\n"
    "pxn_dot3x4_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -16\n"
    "\tr1 = [r0+4]\n\t[sp+0] = r1\n"
    "\tr1 = [r0+16]\n\t[sp+4] = r1\n"
    "\tr1 = [r0+20]\n\t[sp+8] = r1\n"
    "\t[sp+12] = r0\n"
    ".Lpxn_d4g:\n"
    "\tr0 = [sp+12]\n\tr0 = [r0+0]\n"
    "\tr5 = [sp+12]\n"
    "\tr12 = [r5+12]\n"
    "\tr7 = [r5+8]\n"
    "\tr1 = [sp+0]\n"
    "\tr2 = r1 + r12\n"
    "\tr3 = r2 + r12\n"
    "\tr4 = r3 + r12\n"
    "\tr9_r8 = 0\n"
    "\tr11_r10 = 0\n"
    "\tr13_r12 = 0\n"
    "\tr15_r14 = 0\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    ".Lpxn_d4r0:\n"
    "\trep r7 {\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r2++=4]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r3++=4]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r4++=4]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    "\t}\n"
    "\tif (r7 != 0) goto .Lpxn_d4r0\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr1 = [sp+4]\n"
    "\t[r1+0] = r8\n"
    "\t[r1+12] = r10\n"
    "\t[r1+24] = r12\n"
    "\t[r1+36] = r14\n"
    "\tr0 += -4\n"
    "\tr5 = [sp+12]\n"
    "\tr12 = [r5+12]\n"
    "\tr7 = [r5+8]\n"
    "\tr1 = [sp+0]\n"
    "\tr2 = r1 + r12\n"
    "\tr3 = r2 + r12\n"
    "\tr4 = r3 + r12\n"
    "\tr9_r8 = 0\n"
    "\tr11_r10 = 0\n"
    "\tr13_r12 = 0\n"
    "\tr15_r14 = 0\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    ".Lpxn_d4r1:\n"
    "\trep r7 {\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r2++=4]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r3++=4]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r4++=4]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    "\t}\n"
    "\tif (r7 != 0) goto .Lpxn_d4r1\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr1 = [sp+4]\n"
    "\t[r1+4] = r8\n"
    "\t[r1+16] = r10\n"
    "\t[r1+28] = r12\n"
    "\t[r1+40] = r14\n"
    "\tr0 += -4\n"
    "\tr5 = [sp+12]\n"
    "\tr12 = [r5+12]\n"
    "\tr7 = [r5+8]\n"
    "\tr1 = [sp+0]\n"
    "\tr2 = r1 + r12\n"
    "\tr3 = r2 + r12\n"
    "\tr4 = r3 + r12\n"
    "\tr9_r8 = 0\n"
    "\tr11_r10 = 0\n"
    "\tr13_r12 = 0\n"
    "\tr15_r14 = 0\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    ".Lpxn_d4r2:\n"
    "\trep r7 {\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r2++=4]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r3++=4]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r4++=4]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    "\t}\n"
    "\tif (r7 != 0) goto .Lpxn_d4r2\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr1 = [sp+4]\n"
    "\t[r1+8] = r8\n"
    "\t[r1+20] = r10\n"
    "\t[r1+32] = r12\n"
    "\t[r1+44] = r14\n"
    "\tr0 += -4\n"
    "\tr1 = [sp+4]\n\tr1 += 48\n\t[sp+4] = r1\n"
    "\tr0 = [sp+12]\n\tr12 = [r0+12]\n\tr12 = r12 << 2\n"
    "\tr1 = [sp+0]\n\tr1 += r12\n\t[sp+0] = r1\n"
    "\tr1 = [sp+8]\n\tr1 += -1\n\t[sp+8] = r1\n"
    "\tif (r1 != 0) goto .Lpxn_d4g\n"
    "\tsp += 16\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* dot1 */
    "\t.global pxn_dot1_asm\n"
    "pxn_dot1_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -16\n"
    "\tr1 = [r0+4]\n\t[sp+0] = r1\n"
    "\tr1 = [r0+16]\n\t[sp+4] = r1\n"
    "\tr1 = [r0+20]\n\t[sp+8] = r1\n"
    "\t[sp+12] = r0\n"
    ".Lpxn_d1g:\n"
    "\tr0 = [sp+12]\n"
    "\tr7 = [r0+8]\n"
    "\tr1 = [sp+0]\n"
    "\tr0 = [r0+0]\n"
    "\tr9_r8 = 0\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    ".Lpxn_d1r:\n"
    "\trep r7 {\n"
    "\tr9_r8 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1++=4]\n"
    "\t}\n"
    "\tif (r7 != 0) goto .Lpxn_d1r\n"
    "\tr9_r8 >>= 24\n"
    "\tr1 = [sp+4]\n\t[r1+0] = r8\n\tr1 += 12\n\t[sp+4] = r1\n"
    "\tr0 = [sp+12]\n\tr12 = [r0+12]\n"
    "\tr1 = [sp+0]\n\tr1 += r12\n\t[sp+0] = r1\n"
    "\tr1 = [sp+8]\n\tr1 += -1\n\t[sp+8] = r1\n"
    "\tif (r1 != 0) goto .Lpxn_d1g\n"
    "\tsp += 16\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* post: r1 z, r2 x, r3 hs, r7 out, r11 rr, r12 the LeakyReLU slope, r13 frames left */
    "\t.global pxn_post_asm\n"
    "pxn_post_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tr1 = [r0+0]\n\tr2 = [r0+4]\n\tr11 = [r0+12]\n\tr3 = [r0+16]\n\tr7 = [r0+20]\n\tr13 = [r0+24]\n"
    "\tr12 = 42949673\n"
    ".Lpxn_pf:\n"
    "\tr4 = [r1++=4]\n\tr5 = [r1++=4]\n\tr6 = [r1++=4]\n"
    "\tifs (r4 < 0) {\n\tr15_r14 = r4 * r12 (s)\n\tr4 = r15\n\t}\n"
    "\tifs (r5 < 0) {\n\tr15_r14 = r5 * r12 (s)\n\tr5 = r15\n\t}\n"
    "\tifs (r6 < 0) {\n\tr15_r14 = r6 * r12 (s)\n\tr6 = r15\n\t}\n"
    "\tr14 = [r3+0]\n\tr14 += r4\n\t[r3++=4] = r14\n"
    "\tr14 = [r3+0]\n\tr14 += r5\n\t[r3++=4] = r14\n"
    "\tr14 = [r3+0]\n\tr14 += r6\n\t[r3++=4] = r14\n"
    "\tif (r7 == 0) goto .Lpxn_pn\n"
    "\tr10 = r11\n\tr15 = 4\n"
    "\tr9_r8 = 0\n"
    "\tr9_r8 += [r10 ++= r15]*r4 (s)\n\tr9_r8 += [r10 ++= r15]*r5 (s)\n\tr9_r8 += [r10 ++= r15]*r6 (s)\n"
    "\tr9_r8 >>= 24\n\tr14 = [r10++=4]\n\tr8 += r14\n\tr14 = [r2+0]\n\tr8 += r14\n\t[r7++=4] = r8\n"
    "\tr9_r8 = 0\n"
    "\tr9_r8 += [r10 ++= r15]*r4 (s)\n\tr9_r8 += [r10 ++= r15]*r5 (s)\n\tr9_r8 += [r10 ++= r15]*r6 (s)\n"
    "\tr9_r8 >>= 24\n\tr14 = [r10++=4]\n\tr8 += r14\n\tr14 = [r2+4]\n\tr8 += r14\n\t[r7++=4] = r8\n"
    "\tr9_r8 = 0\n"
    "\tr9_r8 += [r10 ++= r15]*r4 (s)\n\tr9_r8 += [r10 ++= r15]*r5 (s)\n\tr9_r8 += [r10 ++= r15]*r6 (s)\n"
    "\tr9_r8 >>= 24\n\tr14 = [r10++=4]\n\tr8 += r14\n\tr14 = [r2+8]\n\tr8 += r14\n\t[r7++=4] = r8\n"
    ".Lpxn_pn:\n"
    "\tr14 = [r0+8]\n\tr2 += r14\n"
    "\tr13 += -1\n"
    "\tif (r13 != 0) goto .Lpxn_pf\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* post2 (v37): the same as post, with fewer memory steps: z, hs and x moved 3 words by one instruction, the
     * residual's 9 multiply-adds in 3 accumulators, each weight loaded in the same step as the multiply before it (a
     * load paired with a multiply takes r0..r7 only). r1 z, r2 x, r3 hs, r7 out, r5 weights, the activations in
     * r4 r14 r15; [sp+0] frames left, [sp+4] x stride - 12 */
    "\t.global pxn_post2_asm\n"
    "pxn_post2_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -8\n"
    "\tr1 = [r0+0]\n\tr2 = [r0+4]\n\tr3 = [r0+16]\n\tr7 = [r0+20]\n"
    "\tr15 = [r0+24]\n\t[sp+0] = r15\n"
    "\tr15 = [r0+8]\n\tr15 += -12\n\t[sp+4] = r15\n"
    "\tif (r7 == 0) goto .Lpxn_p2h\n"
    ".Lpxn_p2f:\n"
    "\t{r6-r4} = [r1++]\n"
    "\tr14 = 42949673\n"
    "\tifs (r4 < 0) {\n\tr9_r8 = r4 * r14 (s)\n\tr4 = r9\n\t}\n"
    "\tifs (r5 < 0) {\n\tr9_r8 = r5 * r14 (s)\n\tr5 = r9\n\t}\n"
    "\tifs (r6 < 0) {\n\tr9_r8 = r6 * r14 (s)\n\tr6 = r9\n\t}\n"
    "\t{r10-r8} = [r3++]\n"
    "\tr8 += r4\n\tr9 += r5\n\tr10 += r6\n"
    "\t[r3+-12] = r8\n\t[r3+-8] = r9\n\t[r3+-4] = r10\n"
    "\tr14 = r5\n\tr15 = r6\n"
    "\tr5 = [r0+28]\n"
    "\tr6 = [r5++=4]\n"
    "\tr9_r8 = r6 * r4 (s)  #  r6 = [r5++=4]\n"
    "\tr11_r10 = r6 * r4 (s)  #  r6 = [r5++=4]\n"
    "\tr13_r12 = r6 * r4 (s)  #  r6 = [r5++=4]\n"
    "\tr9_r8 += r6 * r14 (s)  #  r6 = [r5++=4]\n"
    "\tr11_r10 += r6 * r14 (s)  #  r6 = [r5++=4]\n"
    "\tr13_r12 += r6 * r14 (s)  #  r6 = [r5++=4]\n"
    "\tr9_r8 += r6 * r15 (s)  #  r6 = [r5++=4]\n"
    "\tr11_r10 += r6 * r15 (s)  #  r6 = [r5++=4]\n"
    "\tr13_r12 += r6 * r15 (s)  #  r6 = [r5++=4]\n"
    "\tr9_r8 >>= 24\n\tr11_r10 >>= 24\n\tr13_r12 >>= 24\n"
    "\tr8 += r6\n\tr6 = [r5++=4]\n"
    "\tr10 += r6\n\tr6 = [r5+0]\n"
    "\tr12 += r6\n"
    "\t{r6-r4} = [r2++]\n"
    "\tr8 += r4\n\tr10 += r5\n\tr12 += r6\n"
    "\t[r7++=4] = r8\n\t[r7++=4] = r10\n\t[r7++=4] = r12\n"
    "\tr15 = [sp+4]\n\tr2 += r15\n"
    "\tr15 = [sp+0]\n\tr15 += -1\n\t[sp+0] = r15\n"
    "\tif (r15 != 0) goto .Lpxn_p2f\n"
    "\tgoto .Lpxn_p2e\n"
    ".Lpxn_p2h:\n"
    "\t{r6-r4} = [r1++]\n"
    "\tr14 = 42949673\n"
    "\tifs (r4 < 0) {\n\tr9_r8 = r4 * r14 (s)\n\tr4 = r9\n\t}\n"
    "\tifs (r5 < 0) {\n\tr9_r8 = r5 * r14 (s)\n\tr5 = r9\n\t}\n"
    "\tifs (r6 < 0) {\n\tr9_r8 = r6 * r14 (s)\n\tr6 = r9\n\t}\n"
    "\t{r10-r8} = [r3++]\n"
    "\tr8 += r4\n\tr9 += r5\n\tr10 += r6\n"
    "\t[r3+-12] = r8\n\t[r3+-8] = r9\n\t[r3+-4] = r10\n"
    "\tr15 = [sp+0]\n\tr15 += -1\n\t[sp+0] = r15\n"
    "\tif (r15 != 0) goto .Lpxn_p2h\n"
    ".Lpxn_p2e:\n"
    "\tsp += 8\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* post3 (v37 diagnostic 18): post2 with the three LeakyReLU products computed side by side (one predicated move
     * each), the residual's weights at fixed offsets from r0 and loaded two multiply-adds ahead in r4/r5 (activations
     * in r14 r15 r6). r1 z, r2 x, r3 hs, r7 out, r0 the rr2 weights; [sp+0] frames left, [sp+4] x stride - 12 */
    "\t.global pxn_post3_asm\n"
    "pxn_post3_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -8\n"
    "\tr1 = [r0+0]\n\tr2 = [r0+4]\n\tr3 = [r0+16]\n\tr7 = [r0+20]\n"
    "\tr15 = [r0+24]\n\t[sp+0] = r15\n"
    "\tr15 = [r0+8]\n\tr15 += -12\n\t[sp+4] = r15\n"
    "\tr0 = [r0+28]\n"
    "\tif (r7 == 0) goto .Lpxn_p3h\n"
    ".Lpxn_p3f:\n"
    "\t{r6-r4} = [r1++]\n"
    "\tr15 = 42949673\n"
    "\tr9_r8 = r4 * r15 (s)\n"
    "\tr11_r10 = r5 * r15 (s)\n"
    "\tr13_r12 = r6 * r15 (s)\n"
    "\tifs (r4 < 0) {\n\tr4 = r9\n\t}\n"
    "\tifs (r5 < 0) {\n\tr5 = r11\n\t}\n"
    "\tifs (r6 < 0) {\n\tr6 = r13\n\t}\n"
    "\tr14 = r4\n\tr15 = r5\n"
    "\tr4 = [r0+0]\n\tr5 = [r0+4]\n"
    "\t{r10-r8} = [r3++]\n"
    "\tr8 += r14\n\tr9 += r15\n\tr10 += r6\n"
    "\t[r3+-12] = r8\n\t[r3+-8] = r9\n\t[r3+-4] = r10\n"
    "\tr9_r8 = r14 * r4 (s)  #  r4 = [r0+8]\n"
    "\tr11_r10 = r14 * r5 (s)  #  r5 = [r0+12]\n"
    "\tr13_r12 = r14 * r4 (s)  #  r4 = [r0+16]\n"
    "\tr9_r8 += r15 * r5 (s)  #  r5 = [r0+20]\n"
    "\tr11_r10 += r15 * r4 (s)  #  r4 = [r0+24]\n"
    "\tr13_r12 += r15 * r5 (s)  #  r5 = [r0+28]\n"
    "\tr9_r8 += r6 * r4 (s)  #  r4 = [r0+32]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r5 = [r0+36]\n"
    "\tr13_r12 += r6 * r4 (s)  #  r4 = [r0+40]\n"
    "\tr9_r8 >>= 24\n\tr11_r10 >>= 24\n\tr13_r12 >>= 24\n"
    "\tr8 += r5\n\tr5 = [r0+44]\n"
    "\tr10 += r4\n"
    "\tr12 += r5\n"
    "\t{r6-r4} = [r2++]\n"
    "\tr8 += r4\n\tr10 += r5\n\tr12 += r6\n"
    "\t[r7++=4] = r8\n\t[r7++=4] = r10\n\t[r7++=4] = r12\n"
    "\tr15 = [sp+4]\n\tr2 += r15\n"
    "\tr15 = [sp+0]\n\tr15 += -1\n\t[sp+0] = r15\n"
    "\tif (r15 != 0) goto .Lpxn_p3f\n"
    "\tgoto .Lpxn_p3e\n"
    ".Lpxn_p3h:\n"
    "\t{r6-r4} = [r1++]\n"
    "\tr15 = 42949673\n"
    "\tr9_r8 = r4 * r15 (s)\n"
    "\tr11_r10 = r5 * r15 (s)\n"
    "\tr13_r12 = r6 * r15 (s)\n"
    "\tifs (r4 < 0) {\n\tr4 = r9\n\t}\n"
    "\tifs (r5 < 0) {\n\tr5 = r11\n\t}\n"
    "\tifs (r6 < 0) {\n\tr6 = r13\n\t}\n"
    "\t{r10-r8} = [r3++]\n"
    "\tr8 += r4\n\tr9 += r5\n\tr10 += r6\n"
    "\t[r3+-12] = r8\n\t[r3+-8] = r9\n\t[r3+-4] = r10\n"
    "\tr15 = [sp+0]\n\tr15 += -1\n\t[sp+0] = r15\n"
    "\tif (r15 != 0) goto .Lpxn_p3h\n"
    ".Lpxn_p3e:\n"
    "\tsp += 8\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* post4 (v40.2): post2 for x straight from the layer's ring (sb 12: 3 words a frame), no counter in memory (the body
     * is too long for the hardware loop): the residual's 12 words copied onto the stack first, so that their loads pair with the
     * multiply-adds ([sp+...]) and r0 counts the frames (decremented by the branch). LeakyReLU as smax(z, (z * 42949673) >> 32): the same value for
     * every z (z >= 0: the product is never above z; z < 0: never below it). r1 z, r2 x, r3 hs, r7 out; a0 a1 a2 in
     * r14 r15 r6 */
    "\t.global pxn_post4_asm\n"
    "pxn_post4_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -48\n"
    "\tr7 = [r0+28]\n"
    "\tr4 = [r7+0]\n\t[sp+0] = r4\n\tr4 = [r7+4]\n\t[sp+4] = r4\n\tr4 = [r7+8]\n\t[sp+8] = r4\n"
    "\tr4 = [r7+12]\n\t[sp+12] = r4\n\tr4 = [r7+16]\n\t[sp+16] = r4\n\tr4 = [r7+20]\n\t[sp+20] = r4\n"
    "\tr4 = [r7+24]\n\t[sp+24] = r4\n\tr4 = [r7+28]\n\t[sp+28] = r4\n\tr4 = [r7+32]\n\t[sp+32] = r4\n"
    "\tr4 = [r7+36]\n\t[sp+36] = r4\n\tr4 = [r7+40]\n\t[sp+40] = r4\n\tr4 = [r7+44]\n\t[sp+44] = r4\n"
    "\tr1 = [r0+0]\n\tr2 = [r0+4]\n\tr3 = [r0+16]\n\tr7 = [r0+20]\n\tr0 = [r0+24]\n"
    "\tif (r7 == 0) goto .Lpxn_p4h\n"
    ".Lpxn_p4f:\n"
    "\t{r6-r4} = [r1++]\n"
    "\tr15 = 42949673\n"
    "\tr9_r8 = r4 * r15 (s)\n"
    "\tr11_r10 = r5 * r15 (s)\n"
    "\tr13_r12 = r6 * r15 (s)\n"
    "\tr14 = smax(r4, r9)\n"
    "\tr15 = smax(r5, r11)\n"
    "\tr6 = smax(r6, r13)\n"
    "\t{r10-r8} = [r3++]\n"
    "\tr8 += r14\n\tr9 += r15\n\tr10 += r6\n"
    "\t[r3+-12] = r8\n\t[r3+-8] = r9\n\t[r3+-4] = r10\n"
    "\tr4 = [sp+0]\n\tr5 = [sp+4]\n"
    "\tr9_r8 = r14 * r4 (s)  #  r4 = [sp+8]\n"
    "\tr11_r10 = r14 * r5 (s)  #  r5 = [sp+12]\n"
    "\tr13_r12 = r14 * r4 (s)  #  r4 = [sp+16]\n"
    "\tr9_r8 += r15 * r5 (s)  #  r5 = [sp+20]\n"
    "\tr11_r10 += r15 * r4 (s)  #  r4 = [sp+24]\n"
    "\tr13_r12 += r15 * r5 (s)  #  r5 = [sp+28]\n"
    "\tr9_r8 += r6 * r4 (s)  #  r4 = [sp+32]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r5 = [sp+36]\n"
    "\tr13_r12 += r6 * r4 (s)  #  r4 = [sp+40]\n"
    "\tr9_r8 >>= 24\n\tr11_r10 >>= 24\n\tr13_r12 >>= 24\n"
    "\tr8 += r5\n\tr10 += r4\n"
    "\tr5 = [sp+44]\n"
    "\tr12 += r5\n"
    "\t{r6-r4} = [r2++]\n"
    "\tr8 += r4\n\tr10 += r5\n\tr12 += r6\n"
    "\t[r7++=4] = r8\n\t[r7++=4] = r10\n\t[r7++=4] = r12\n"
    "\tif (--r0 != 0) goto .Lpxn_p4f\n"
    "\tgoto .Lpxn_p4e\n"
    ".Lpxn_p4h:\n"
    "\t{r6-r4} = [r1++]\n"
    "\tr15 = 42949673\n"
    "\tr9_r8 = r4 * r15 (s)\n"
    "\tr11_r10 = r5 * r15 (s)\n"
    "\tr13_r12 = r6 * r15 (s)\n"
    "\tr14 = smax(r4, r9)\n"
    "\tr15 = smax(r5, r11)\n"
    "\tr6 = smax(r6, r13)\n"
    "\t{r10-r8} = [r3++]\n"
    "\tr8 += r14\n\tr9 += r15\n\tr10 += r6\n"
    "\t[r3+-12] = r8\n\t[r3+-8] = r9\n\t[r3+-4] = r10\n"
    "\tif (--r0 != 0) goto .Lpxn_p4h\n"
    ".Lpxn_p4e:\n"
    "\tsp += 48\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* post5 (v40.5, tools/gen_rdq.py --post5): post4's frames two at a time, for a layer with an output. The residual's 9
     * weights are read once for the two frames, and every load and store rides on an instruction that computes: on the
     * pedal that is free on a multiply or multiply-add and costs the memory word's own time on a 1-cycle instruction
     * (measured), so a turn of 2 frames takes 109 cycles against post4's 159. Every value is loaded 2 instructions or
     * more before its use, and no word outside the job's is read. r0 z, r1 hs then out, r7 x; r2 r3 r6 values (r6 the
     * slope while the activations are made); r5_r4 and r9_r8 the two frames' sums; r10..r15 the 6 activations. Stack:
     * [sp+0..44] the residual, [sp+48] hs - z, [sp+52] out - z, [sp+56] z's end. n even and > 0, out not 0.
     * In the flash (PXN_HOT): from RAM its 292 bytes a turn take 126 cycles to fetch and run, from the flash 109 */
    PXN_HOT
    "\t.global pxn_post5_asm\n"
    "pxn_post5_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -60\n"
    "\tr7 = [r0+28]\n"
    "\tr4 = [r7+0]\n"
    "\t[sp+0] = r4\n"
    "\tr4 = [r7+4]\n"
    "\t[sp+4] = r4\n"
    "\tr4 = [r7+8]\n"
    "\t[sp+8] = r4\n"
    "\tr4 = [r7+12]\n"
    "\t[sp+12] = r4\n"
    "\tr4 = [r7+16]\n"
    "\t[sp+16] = r4\n"
    "\tr4 = [r7+20]\n"
    "\t[sp+20] = r4\n"
    "\tr4 = [r7+24]\n"
    "\t[sp+24] = r4\n"
    "\tr4 = [r7+28]\n"
    "\t[sp+28] = r4\n"
    "\tr4 = [r7+32]\n"
    "\t[sp+32] = r4\n"
    "\tr4 = [r7+36]\n"
    "\t[sp+36] = r4\n"
    "\tr4 = [r7+40]\n"
    "\t[sp+40] = r4\n"
    "\tr4 = [r7+44]\n"
    "\t[sp+44] = r4\n"
    "\tr1 = [r0+0]\n"
    "\tr2 = [r0+4]\n"
    "\tr3 = [r0+16]\n"
    "\tr4 = [r0+20]\n"
    "\tr5 = [r0+24]\n"
    "\tr6 = r3 - r1\n"
    "\t[sp+48] = r6\n"
    "\tr6 = r4 - r1\n"
    "\t[sp+52] = r6\n"
    "\tr6 = 12\n"
    "\tr5 = r5 * r6\n"
    "\tr5 += r1\n"
    "\t[sp+56] = r5\n"
    "\tr7 = r2 + -24\n"
    "\tr0 = r1\n"
    "\tr1 = r3\n"
    ".Lpxn_p5f:\n"
    "\tr7 += 24  #  r2 = [r0+0]\n"
    "\tr6 = 42949673\n"
    "\tr9_r8 = r2 * r6 (s)  #  r3 = [r0+4]\n"
    "\tr10 = smax(r2, r9)  #  r4 = [r1+0]\n"
    "\tr9_r8 = r3 * r6 (s)  #  r2 = [r0+8]\n"
    "\tr4 += r10  #  r5 = [r1+4]\n"
    "\tr11 = smax(r3, r9)  #  [r1++=4] = r4\n"
    "\tr9_r8 = r2 * r6 (s)  #  r3 = [r0+12]\n"
    "\tr5 += r11  #  r4 = [r1+4]\n"
    "\tr12 = smax(r2, r9)  #  [r1++=4] = r5\n"
    "\tr9_r8 = r3 * r6 (s)  #  r2 = [r0+16]\n"
    "\tr4 += r12  #  r5 = [r1+4]\n"
    "\tr13 = smax(r3, r9)  #  [r1++=4] = r4\n"
    "\tr9_r8 = r2 * r6 (s)  #  r3 = [r0+20]\n"
    "\tr5 += r13  #  r4 = [r1+4]\n"
    "\tr14 = smax(r2, r9)  #  [r1++=4] = r5\n"
    "\tr9_r8 = r3 * r6 (s)  #  r5 = [r1+4]\n"
    "\tr4 += r14  #  r2 = [sp+0]\n"
    "\tr15 = smax(r3, r9)  #  [r1++=4] = r4\n"
    "\tr5 += r15  #  r3 = [sp+12]\n"
    "\tr9_r8 = r2 * r13 (s)  #  [r1++=4] = r5\n"
    "\tr5_r4 = r2 * r10 (s)  #  r6 = [sp+52]\n"
    "\tr5_r4 += r3 * r11 (s)  #  r2 = [sp+24]\n"
    "\tr9_r8 += r3 * r14 (s)  #  r3 = [r7+0]\n"
    "\tr1 = r0 + r6  #  r6 = [sp+36]\n"
    "\tr5_r4 += r2 * r12 (s)\n"
    "\tr9_r8 += r2 * r15 (s)  #  r2 = [r7+12]\n"
    "\tr5_r4 >>= 24\n"
    "\tr9_r8 >>= 24\n"
    "\tr4 += r6\n"
    "\tr4 += r3  #  r3 = [sp+4]\n"
    "\tr6 += r8  #  [r1+0] = r4\n"
    "\tr6 += r2  #  r2 = [sp+16]\n"
    "\tr5_r4 = r3 * r10 (s)  #  [r1+12] = r6\n"
    "\tr9_r8 = r3 * r13 (s)  #  r3 = [sp+28]\n"
    "\tr5_r4 += r2 * r11 (s)  #  r6 = [sp+40]\n"
    "\tr9_r8 += r2 * r14 (s)  #  r2 = [r7+4]\n"
    "\tr5_r4 += r3 * r12 (s)\n"
    "\tr9_r8 += r3 * r15 (s)  #  r3 = [r7+16]\n"
    "\tr5_r4 >>= 24\n"
    "\tr9_r8 >>= 24\n"
    "\tr4 += r6\n"
    "\tr4 += r2  #  r2 = [sp+8]\n"
    "\tr6 += r8  #  [r1+4] = r4\n"
    "\tr6 += r3  #  r3 = [sp+20]\n"
    "\tr5_r4 = r2 * r10 (s)  #  [r1+16] = r6\n"
    "\tr9_r8 = r2 * r13 (s)  #  r2 = [sp+32]\n"
    "\tr5_r4 += r3 * r11 (s)  #  r6 = [sp+44]\n"
    "\tr9_r8 += r3 * r14 (s)  #  r3 = [r7+8]\n"
    "\tr5_r4 += r2 * r12 (s)\n"
    "\tr9_r8 += r2 * r15 (s)  #  r2 = [r7+20]\n"
    "\tr5_r4 >>= 24\n"
    "\tr9_r8 >>= 24\n"
    "\tr4 += r6\n"
    "\tr4 += r3  #  r3 = [sp+48]\n"
    "\tr6 += r8  #  [r1+8] = r4\n"
    "\tr6 += r2  #  r2 = [sp+56]\n"
    "\tr0 += 24  #  [r1+20] = r6\n"
    "\tr1 = r0 + r3\n"
    "\tif (r0 != r2) goto .Lpxn_p5f\n"
    "\tsp += 60\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* v40.2: rdot, rdot6, the rdq routines and rdl6, rdl15 stay in the flash (.text.pxn_cold, not moved to RAM): they
     * only serve the start-up check and timing, rdm doing the work, and the RAM code window needs the room */
    "\t.section .text.pxn_cold,\"ax\"\n"
#if !PXN_LEAN
    /* rdot: r0 the weights (row after row), r1 a tap's pointer, r2 the tap table, r3 taps left, r4 u, r5 a weight,
     * r6 an x; 4 accumulators. [sp+0] job, [sp+4] rows left, [sp+8] z */
    "\t.global pxn_rdot_asm\n"
    "pxn_rdot_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -12\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+20]\n\t[sp+4] = r7\n"
    "\tr7 = [r0+16]\n\t[sp+8] = r7\n"
    "\tr0 = [r0+0]\n"
    ".Lpxn_rdo:\n"
    "\tr4 = [sp+0]\n"
    "\tr2 = [r4+4]\n"
    "\tr3 = [r4+8]\n"
    "\tr4 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr7 = [r0++=4]\n"
    "\tr6 = [r4+0]\n"
    "\tr9_r8 = r6 * r5 (s)  #  r6 = [r4+4]\n"
    "\tr11_r10 = r6 * r5 (s)  #  r6 = [r4+8]\n"
    "\tr13_r12 = r6 * r5 (s)  #  r6 = [r4+12]\n"
    "\tr15_r14 = r6 * r5 (s)\n"
    "\tr6 = 1048576\n"
    "\tr9_r8 += r6 * r7 (s)\n"
    "\tr11_r10 += r6 * r7 (s)\n"
    "\tr13_r12 += r6 * r7 (s)\n"
    "\tr15_r14 += r6 * r7 (s)\n"
    "\tr5 = [r0++=4]\n"
    ".Lpxn_rdk:\n"
    "\tr1 = [r2++=4]\n"
    "\tr6 = [r1+0]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+12]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+36]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+4]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+16]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+40]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+8]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+20]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+44]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr3 += -1\n"
    "\tif (r3 != 0) goto .Lpxn_rdk\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n\tr11_r10 >>= 24\n\tr13_r12 >>= 24\n\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n\t[r4+12] = r10\n\t[r4+24] = r12\n\t[r4+36] = r14\n"
    "\tr4 += 4\n\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n\tr7 += -1\n\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdo\n"
    "\tsp += 12\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* rdot6: the same for 6 taps, unrolled (no branch per tap): most layers */
    "\t.global pxn_rdot6_asm\n"
    "pxn_rdot6_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -12\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+20]\n\t[sp+4] = r7\n"
    "\tr7 = [r0+16]\n\t[sp+8] = r7\n"
    "\tr0 = [r0+0]\n"
    ".Lpxn_r6o:\n"
    "\tr4 = [sp+0]\n"
    "\tr2 = [r4+4]\n"
    "\tr3 = [r4+8]\n"
    "\tr4 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr7 = [r0++=4]\n"
    "\tr6 = [r4+0]\n"
    "\tr9_r8 = r6 * r5 (s)  #  r6 = [r4+4]\n"
    "\tr11_r10 = r6 * r5 (s)  #  r6 = [r4+8]\n"
    "\tr13_r12 = r6 * r5 (s)  #  r6 = [r4+12]\n"
    "\tr15_r14 = r6 * r5 (s)\n"
    "\tr6 = 1048576\n"
    "\tr9_r8 += r6 * r7 (s)\n"
    "\tr11_r10 += r6 * r7 (s)\n"
    "\tr13_r12 += r6 * r7 (s)\n"
    "\tr15_r14 += r6 * r7 (s)\n"
    "\tr5 = [r0++=4]\n"
    "\tr1 = [r2++=4]\n"
    "\tr6 = [r1+0]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+12]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+36]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+4]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+16]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+40]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+8]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+20]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+44]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr1 = [r2++=4]\n"
    "\tr6 = [r1+0]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+12]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+36]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+4]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+16]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+40]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+8]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+20]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+44]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr1 = [r2++=4]\n"
    "\tr6 = [r1+0]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+12]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+36]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+4]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+16]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+40]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+8]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+20]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+44]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr1 = [r2++=4]\n"
    "\tr6 = [r1+0]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+12]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+36]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+4]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+16]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+40]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+8]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+20]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+44]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr1 = [r2++=4]\n"
    "\tr6 = [r1+0]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+12]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+36]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+4]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+16]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+40]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+8]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+20]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+44]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr1 = [r2++=4]\n"
    "\tr6 = [r1+0]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+12]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+36]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+4]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+16]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+40]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr6 = [r1+8]\n"
    "\tr9_r8 += r6 * r5 (s)  #  r6 = [r1+20]\n"
    "\tr11_r10 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+44]\n"
    "\tr15_r14 += r6 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n\tr11_r10 >>= 24\n\tr13_r12 >>= 24\n\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n\t[r4+12] = r10\n\t[r4+24] = r12\n\t[r4+36] = r14\n"
    "\tr4 += 4\n\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n\tr7 += -1\n\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_r6o\n"
    "\tsp += 12\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* rdq6, rdq15, rdq16 (generated by tools/gen_rdq.py): the same as rdot, unrolled for 6, 15 and 16 taps, each
     * multiply-add using a value loaded 2 to 4 steps before (a pair whose multiply waits for the load before costs
     * 3.2 cycles, without the wait 2.2: diagnostic 12). x values in r3 r4 r6 r7, the weight in r5, the taps' pointers
     * in r1 and r2 by turns (the next one loaded during the current tap). [sp+0] job, [sp+4] rows left, [sp+8] z,
     * [sp+12] the tap table */
    "\t.global pxn_rdq6_asm\n"
    "pxn_rdq6_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -16\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+20]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r0+16]\n"
    "\t[sp+8] = r7\n"
    "\tr7 = [r0+4]\n"
    "\t[sp+12] = r7\n"
    "\tr0 = [r0+0]\n"
    ".Lpxn_rdq6_asmo:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+0]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+4]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+8]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+12]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+16]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+20]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n"
    "\t[r4+12] = r10\n"
    "\t[r4+24] = r12\n"
    "\t[r4+36] = r14\n"
    "\tr4 += 4\n"
    "\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdq6_asmo\n"
    "\tsp += 16\n"
    "\t{pc, r15-r4} = [sp++]\n"
    "\t.global pxn_rdq15_asm\n"
    "pxn_rdq15_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -16\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+20]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r0+16]\n"
    "\t[sp+8] = r7\n"
    "\tr7 = [r0+4]\n"
    "\t[sp+12] = r7\n"
    "\tr0 = [r0+0]\n"
    ".Lpxn_rdq15_asmo:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+0]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+4]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+8]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+12]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+16]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+20]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+24]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+28]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+32]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+48]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+52]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+56]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n"
    "\t[r4+12] = r10\n"
    "\t[r4+24] = r12\n"
    "\t[r4+36] = r14\n"
    "\tr4 += 4\n"
    "\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdq15_asmo\n"
    "\tsp += 16\n"
    "\t{pc, r15-r4} = [sp++]\n"
    "\t.global pxn_rdq16_asm\n"
    "pxn_rdq16_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -16\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+20]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r0+16]\n"
    "\t[sp+8] = r7\n"
    "\tr7 = [r0+4]\n"
    "\t[sp+12] = r7\n"
    "\tr0 = [r0+0]\n"
    ".Lpxn_rdq16_asmo:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+0]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+4]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+8]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+12]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+16]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+20]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+24]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+28]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+32]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+48]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+52]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+12]\n"
    "\tr1 = [r1+56]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+12]\n"
    "\tr2 = [r2+60]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n"
    "\t[r4+12] = r10\n"
    "\t[r4+24] = r12\n"
    "\t[r4+36] = r14\n"
    "\tr4 += 4\n"
    "\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdq16_asmo\n"
    "\tsp += 16\n"
    "\t{pc, r15-r4} = [sp++]\n"
    /* rdl6, rdl15, rdl16 (tools/gen_rdq.py --rdl): rdq for all the groups of 4 frames of a layer's block in one call
     * (diagnostic 20): the tap table on the stack ([sp+20+4k]), the job's u and z moved on, every tap pointer 4 frames
     * on after each group, back by the ring's length past its end (the ring's first frames are mirrored after it) */
#endif
    "\t.global pxn_rdl6_asm\n"
    "pxn_rdl6_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -44\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+24]\n"
    "\t[sp+12] = r7\n"
    "\tr1 = [r0+4]\n"
    "\tr7 = [r1+0]\n"
    "\t[sp+20] = r7\n"
    "\tr7 = [r1+4]\n"
    "\t[sp+24] = r7\n"
    "\tr7 = [r1+8]\n"
    "\t[sp+28] = r7\n"
    "\tr7 = [r1+12]\n"
    "\t[sp+32] = r7\n"
    "\tr7 = [r1+16]\n"
    "\t[sp+36] = r7\n"
    "\tr7 = [r1+20]\n"
    "\t[sp+40] = r7\n"
    ".Lpxn_rdl6_asmg:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+20]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r4+16]\n"
    "\t[sp+8] = r7\n"
    "\tr0 = [r4+0]\n"
    ".Lpxn_rdl6_asmo:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+20]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+28]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n"
    "\t[r4+12] = r10\n"
    "\t[r4+24] = r12\n"
    "\t[r4+36] = r14\n"
    "\tr4 += 4\n"
    "\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdl6_asmo\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr7 += 16\n"
    "\t[r4+12] = r7\n"
    "\tr7 = [r4+16]\n"
    "\tr7 += 48\n"
    "\t[r4+16] = r7\n"
    "\tr2 = [r4+28]\n"
    "\tr3 = [r4+32]\n"
    "\tr1 = [sp+20]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+20] = r1\n"
    "\tr1 = [sp+24]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+24] = r1\n"
    "\tr1 = [sp+28]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+28] = r1\n"
    "\tr1 = [sp+32]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+32] = r1\n"
    "\tr1 = [sp+36]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+36] = r1\n"
    "\tr1 = [sp+40]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+40] = r1\n"
    "\tr7 = [sp+12]\n"
    "\tr7 += -1\n"
    "\t[sp+12] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdl6_asmg\n"
    "\tsp += 44\n"
    "\t{pc, r15-r4} = [sp++]\n"
    "\t.global pxn_rdl15_asm\n"
    "pxn_rdl15_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -80\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+24]\n"
    "\t[sp+12] = r7\n"
    "\tr1 = [r0+4]\n"
    "\tr7 = [r1+0]\n"
    "\t[sp+20] = r7\n"
    "\tr7 = [r1+4]\n"
    "\t[sp+24] = r7\n"
    "\tr7 = [r1+8]\n"
    "\t[sp+28] = r7\n"
    "\tr7 = [r1+12]\n"
    "\t[sp+32] = r7\n"
    "\tr7 = [r1+16]\n"
    "\t[sp+36] = r7\n"
    "\tr7 = [r1+20]\n"
    "\t[sp+40] = r7\n"
    "\tr7 = [r1+24]\n"
    "\t[sp+44] = r7\n"
    "\tr7 = [r1+28]\n"
    "\t[sp+48] = r7\n"
    "\tr7 = [r1+32]\n"
    "\t[sp+52] = r7\n"
    "\tr7 = [r1+36]\n"
    "\t[sp+56] = r7\n"
    "\tr7 = [r1+40]\n"
    "\t[sp+60] = r7\n"
    "\tr7 = [r1+44]\n"
    "\t[sp+64] = r7\n"
    "\tr7 = [r1+48]\n"
    "\t[sp+68] = r7\n"
    "\tr7 = [r1+52]\n"
    "\t[sp+72] = r7\n"
    "\tr7 = [r1+56]\n"
    "\t[sp+76] = r7\n"
    ".Lpxn_rdl15_asmg:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+20]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r4+16]\n"
    "\t[sp+8] = r7\n"
    "\tr0 = [r4+0]\n"
    ".Lpxn_rdl15_asmo:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+20]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+28]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+48]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+52]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+56]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+60]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+64]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+68]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+72]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+76]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n"
    "\t[r4+12] = r10\n"
    "\t[r4+24] = r12\n"
    "\t[r4+36] = r14\n"
    "\tr4 += 4\n"
    "\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdl15_asmo\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr7 += 16\n"
    "\t[r4+12] = r7\n"
    "\tr7 = [r4+16]\n"
    "\tr7 += 48\n"
    "\t[r4+16] = r7\n"
    "\tr2 = [r4+28]\n"
    "\tr3 = [r4+32]\n"
    "\tr1 = [sp+20]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+20] = r1\n"
    "\tr1 = [sp+24]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+24] = r1\n"
    "\tr1 = [sp+28]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+28] = r1\n"
    "\tr1 = [sp+32]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+32] = r1\n"
    "\tr1 = [sp+36]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+36] = r1\n"
    "\tr1 = [sp+40]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+40] = r1\n"
    "\tr1 = [sp+44]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+44] = r1\n"
    "\tr1 = [sp+48]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+48] = r1\n"
    "\tr1 = [sp+52]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+52] = r1\n"
    "\tr1 = [sp+56]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+56] = r1\n"
    "\tr1 = [sp+60]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+60] = r1\n"
    "\tr1 = [sp+64]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+64] = r1\n"
    "\tr1 = [sp+68]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+68] = r1\n"
    "\tr1 = [sp+72]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+72] = r1\n"
    "\tr1 = [sp+76]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+76] = r1\n"
    "\tr7 = [sp+12]\n"
    "\tr7 += -1\n"
    "\t[sp+12] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdl15_asmg\n"
    "\tsp += 80\n"
    "\t{pc, r15-r4} = [sp++]\n"
    "\t.text\n"
    "\t.global pxn_rdl16_asm\n"
    "pxn_rdl16_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -84\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+24]\n"
    "\t[sp+12] = r7\n"
    "\tr1 = [r0+4]\n"
    "\tr7 = [r1+0]\n"
    "\t[sp+20] = r7\n"
    "\tr7 = [r1+4]\n"
    "\t[sp+24] = r7\n"
    "\tr7 = [r1+8]\n"
    "\t[sp+28] = r7\n"
    "\tr7 = [r1+12]\n"
    "\t[sp+32] = r7\n"
    "\tr7 = [r1+16]\n"
    "\t[sp+36] = r7\n"
    "\tr7 = [r1+20]\n"
    "\t[sp+40] = r7\n"
    "\tr7 = [r1+24]\n"
    "\t[sp+44] = r7\n"
    "\tr7 = [r1+28]\n"
    "\t[sp+48] = r7\n"
    "\tr7 = [r1+32]\n"
    "\t[sp+52] = r7\n"
    "\tr7 = [r1+36]\n"
    "\t[sp+56] = r7\n"
    "\tr7 = [r1+40]\n"
    "\t[sp+60] = r7\n"
    "\tr7 = [r1+44]\n"
    "\t[sp+64] = r7\n"
    "\tr7 = [r1+48]\n"
    "\t[sp+68] = r7\n"
    "\tr7 = [r1+52]\n"
    "\t[sp+72] = r7\n"
    "\tr7 = [r1+56]\n"
    "\t[sp+76] = r7\n"
    "\tr7 = [r1+60]\n"
    "\t[sp+80] = r7\n"
    ".Lpxn_rdl16_asmg:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+20]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r4+16]\n"
    "\t[sp+8] = r7\n"
    "\tr0 = [r4+0]\n"
    ".Lpxn_rdl16_asmo:\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+20]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+28]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+48]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+52]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+56]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+60]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+64]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+68]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+72]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+76]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+80]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr0 += -4\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+0] = r8\n"
    "\t[r4+12] = r10\n"
    "\t[r4+24] = r12\n"
    "\t[r4+36] = r14\n"
    "\tr4 += 4\n"
    "\t[sp+8] = r4\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdl16_asmo\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [r4+12]\n"
    "\tr7 += 16\n"
    "\t[r4+12] = r7\n"
    "\tr7 = [r4+16]\n"
    "\tr7 += 48\n"
    "\t[r4+16] = r7\n"
    "\tr2 = [r4+28]\n"
    "\tr3 = [r4+32]\n"
    "\tr1 = [sp+20]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+20] = r1\n"
    "\tr1 = [sp+24]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+24] = r1\n"
    "\tr1 = [sp+28]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+28] = r1\n"
    "\tr1 = [sp+32]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+32] = r1\n"
    "\tr1 = [sp+36]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+36] = r1\n"
    "\tr1 = [sp+40]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+40] = r1\n"
    "\tr1 = [sp+44]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+44] = r1\n"
    "\tr1 = [sp+48]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+48] = r1\n"
    "\tr1 = [sp+52]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+52] = r1\n"
    "\tr1 = [sp+56]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+56] = r1\n"
    "\tr1 = [sp+60]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+60] = r1\n"
    "\tr1 = [sp+64]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+64] = r1\n"
    "\tr1 = [sp+68]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+68] = r1\n"
    "\tr1 = [sp+72]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+72] = r1\n"
    "\tr1 = [sp+76]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+76] = r1\n"
    "\tr1 = [sp+80]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+80] = r1\n"
    "\tr7 = [sp+12]\n"
    "\tr7 += -1\n"
    "\t[sp+12] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdl16_asmg\n"
    "\tsp += 84\n"
    "\t{pc, r15-r4} = [sp++]\n"

    /* rdm (v40.2, tools/gen_rdq.py --rdm): rdl's job for the 3 rows of a 6- or 15-tap layer, rows 0 and 1 two frames at a
     * time (every load paired with a multiply-add), row 2 four frames at a time. v40.5: in the flash (PXN_HOT) */
    PXN_HOT
    "\t.global pxn_rdm6_asm\n"
    "pxn_rdm6_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -44\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+24]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r0+16]\n"
    "\t[sp+8] = r7\n"
    "\tr7 = [r0+12]\n"
    "\t[sp+12] = r7\n"
    "\tr7 = [r0+0]\n"
    "\t[sp+16] = r7\n"
    "\tr1 = [r0+4]\n"
    "\tr7 = [r1+0]\n"
    "\t[sp+20] = r7\n"
    "\tr7 = [r1+4]\n"
    "\t[sp+24] = r7\n"
    "\tr7 = [r1+8]\n"
    "\t[sp+28] = r7\n"
    "\tr7 = [r1+12]\n"
    "\t[sp+32] = r7\n"
    "\tr7 = [r1+16]\n"
    "\t[sp+36] = r7\n"
    "\tr7 = [r1+20]\n"
    "\t[sp+40] = r7\n"
    ".Lpxn_rdm6_asmg:\n"
    "\tr0 = [sp+16]\n"
    "\tr1 = r0 + 80\n"
    "\tr2 = [sp+12]\n"
    "\tr3 = [r0++=4]\n"
    "\tr4 = [r1++=4]\n"
    "\tr5 = [r2+0]\n"
    "\tr6 = [r2+4]\n"
    "\tr9_r8 = r3 * r5 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 = r3 * r6 (s)  #  r3 = [r1++=4]\n"
    "\tr13_r12 = r4 * r5 (s)  #  r2 = [sp+20]\n"
    "\tr15_r14 = r4 * r6 (s)\n"
    "\tr5 = 1048576\n"
    "\tr9_r8 += r7 * r5 (s)  #  r6 = [r0++=4]\n"
    "\tr13_r12 += r3 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr15_r14 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r7 * r5 (s)  #  r7 = [r1++=4]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+28]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+36]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr9_r8 += r7 * r3 (s)\n"
    "\tr11_r10 += r7 * r4 (s)\n"
    "\tr13_r12 += r5 * r3 (s)\n"
    "\tr15_r14 += r5 * r4 (s)\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr2 = [sp+8]\n"
    "\t[r2+0] = r8\n"
    "\t[r2+12] = r10\n"
    "\t[r2+4] = r12\n"
    "\t[r2+16] = r14\n"
    "\tr0 = [sp+16]\n"
    "\tr1 = r0 + 80\n"
    "\tr2 = [sp+12]\n"
    "\tr3 = [r0++=4]\n"
    "\tr4 = [r1++=4]\n"
    "\tr5 = [r2+8]\n"
    "\tr6 = [r2+12]\n"
    "\tr9_r8 = r3 * r5 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 = r3 * r6 (s)  #  r3 = [r1++=4]\n"
    "\tr13_r12 = r4 * r5 (s)  #  r2 = [sp+20]\n"
    "\tr15_r14 = r4 * r6 (s)\n"
    "\tr5 = 1048576\n"
    "\tr9_r8 += r7 * r5 (s)  #  r6 = [r0++=4]\n"
    "\tr13_r12 += r3 * r5 (s)  #  r4 = [r2+36]\n"
    "\tr15_r14 += r3 * r5 (s)  #  r3 = [r2+24]\n"
    "\tr11_r10 += r7 * r5 (s)  #  r7 = [r1++=4]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+28]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+36]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr9_r8 += r7 * r3 (s)\n"
    "\tr11_r10 += r7 * r4 (s)\n"
    "\tr13_r12 += r5 * r3 (s)\n"
    "\tr15_r14 += r5 * r4 (s)\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr2 = [sp+8]\n"
    "\t[r2+24] = r8\n"
    "\t[r2+36] = r10\n"
    "\t[r2+28] = r12\n"
    "\t[r2+40] = r14\n"
    "\tr0 = [sp+16]\n"
    "\tr0 += 160\n"
    "\tr7 = [sp+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+20]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+28]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+8] = r8\n"
    "\t[r4+20] = r10\n"
    "\t[r4+32] = r12\n"
    "\t[r4+44] = r14\n"
    "\tr4 += 48\n"
    "\t[sp+8] = r4\n"
    "\tr4 = [sp+12]\n"
    "\tr4 += 16\n"
    "\t[sp+12] = r4\n"
    "\tr4 = [sp+0]\n"
    "\tr2 = [r4+28]\n"
    "\tr3 = [r4+32]\n"
    "\tr1 = [sp+20]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+20] = r1\n"
    "\tr1 = [sp+24]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+24] = r1\n"
    "\tr1 = [sp+28]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+28] = r1\n"
    "\tr1 = [sp+32]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+32] = r1\n"
    "\tr1 = [sp+36]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+36] = r1\n"
    "\tr1 = [sp+40]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+40] = r1\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdm6_asmg\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [sp+12]\n"
    "\t[r4+12] = r7\n"
    "\tr7 = [sp+8]\n"
    "\t[r4+16] = r7\n"
    "\tsp += 44\n"
    "\t{pc, r15-r4} = [sp++]\n"
    "\t.global pxn_rdm15_asm\n"
    "pxn_rdm15_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tsp += -80\n"
    "\t[sp+0] = r0\n"
    "\tr7 = [r0+24]\n"
    "\t[sp+4] = r7\n"
    "\tr7 = [r0+16]\n"
    "\t[sp+8] = r7\n"
    "\tr7 = [r0+12]\n"
    "\t[sp+12] = r7\n"
    "\tr7 = [r0+0]\n"
    "\t[sp+16] = r7\n"
    "\tr1 = [r0+4]\n"
    "\tr7 = [r1+0]\n"
    "\t[sp+20] = r7\n"
    "\tr7 = [r1+4]\n"
    "\t[sp+24] = r7\n"
    "\tr7 = [r1+8]\n"
    "\t[sp+28] = r7\n"
    "\tr7 = [r1+12]\n"
    "\t[sp+32] = r7\n"
    "\tr7 = [r1+16]\n"
    "\t[sp+36] = r7\n"
    "\tr7 = [r1+20]\n"
    "\t[sp+40] = r7\n"
    "\tr7 = [r1+24]\n"
    "\t[sp+44] = r7\n"
    "\tr7 = [r1+28]\n"
    "\t[sp+48] = r7\n"
    "\tr7 = [r1+32]\n"
    "\t[sp+52] = r7\n"
    "\tr7 = [r1+36]\n"
    "\t[sp+56] = r7\n"
    "\tr7 = [r1+40]\n"
    "\t[sp+60] = r7\n"
    "\tr7 = [r1+44]\n"
    "\t[sp+64] = r7\n"
    "\tr7 = [r1+48]\n"
    "\t[sp+68] = r7\n"
    "\tr7 = [r1+52]\n"
    "\t[sp+72] = r7\n"
    "\tr7 = [r1+56]\n"
    "\t[sp+76] = r7\n"
    ".Lpxn_rdm15_asmg:\n"
    "\tr0 = [sp+16]\n"
    "\tr1 = r0 + 188\n"
    "\tr2 = [sp+12]\n"
    "\tr3 = [r0++=4]\n"
    "\tr4 = [r1++=4]\n"
    "\tr5 = [r2+0]\n"
    "\tr6 = [r2+4]\n"
    "\tr9_r8 = r3 * r5 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 = r3 * r6 (s)  #  r3 = [r1++=4]\n"
    "\tr13_r12 = r4 * r5 (s)  #  r2 = [sp+20]\n"
    "\tr15_r14 = r4 * r6 (s)\n"
    "\tr5 = 1048576\n"
    "\tr9_r8 += r7 * r5 (s)  #  r6 = [r0++=4]\n"
    "\tr13_r12 += r3 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr15_r14 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r7 * r5 (s)  #  r7 = [r1++=4]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+28]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+36]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+44]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+48]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+52]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+56]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+60]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+64]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+68]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+72]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr2 = [sp+76]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+0]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+12]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+4]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+16]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+8]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+20]\n"
    "\tr9_r8 += r7 * r3 (s)\n"
    "\tr11_r10 += r7 * r4 (s)\n"
    "\tr13_r12 += r5 * r3 (s)\n"
    "\tr15_r14 += r5 * r4 (s)\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr2 = [sp+8]\n"
    "\t[r2+0] = r8\n"
    "\t[r2+12] = r10\n"
    "\t[r2+4] = r12\n"
    "\t[r2+16] = r14\n"
    "\tr0 = [sp+16]\n"
    "\tr1 = r0 + 188\n"
    "\tr2 = [sp+12]\n"
    "\tr3 = [r0++=4]\n"
    "\tr4 = [r1++=4]\n"
    "\tr5 = [r2+8]\n"
    "\tr6 = [r2+12]\n"
    "\tr9_r8 = r3 * r5 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 = r3 * r6 (s)  #  r3 = [r1++=4]\n"
    "\tr13_r12 = r4 * r5 (s)  #  r2 = [sp+20]\n"
    "\tr15_r14 = r4 * r6 (s)\n"
    "\tr5 = 1048576\n"
    "\tr9_r8 += r7 * r5 (s)  #  r6 = [r0++=4]\n"
    "\tr13_r12 += r3 * r5 (s)  #  r4 = [r2+36]\n"
    "\tr15_r14 += r3 * r5 (s)  #  r3 = [r2+24]\n"
    "\tr11_r10 += r7 * r5 (s)  #  r7 = [r1++=4]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+28]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+36]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+44]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+48]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+52]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+56]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+60]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+64]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+68]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+72]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr2 = [sp+76]\n"
    "\tr9_r8 += r7 * r3 (s)  #  r6 = [r0++=4]\n"
    "\tr11_r10 += r7 * r4 (s)  #  r7 = [r1++=4]\n"
    "\tr13_r12 += r5 * r3 (s)  #  r3 = [r2+24]\n"
    "\tr15_r14 += r5 * r4 (s)  #  r4 = [r2+36]\n"
    "\tr9_r8 += r6 * r3 (s)  #  r5 = [r0++=4]\n"
    "\tr11_r10 += r6 * r4 (s)  #  r6 = [r1++=4]\n"
    "\tr13_r12 += r7 * r3 (s)  #  r3 = [r2+28]\n"
    "\tr15_r14 += r7 * r4 (s)  #  r4 = [r2+40]\n"
    "\tr9_r8 += r5 * r3 (s)  #  r7 = [r0++=4]\n"
    "\tr11_r10 += r5 * r4 (s)  #  r5 = [r1++=4]\n"
    "\tr13_r12 += r6 * r3 (s)  #  r3 = [r2+32]\n"
    "\tr15_r14 += r6 * r4 (s)  #  r4 = [r2+44]\n"
    "\tr9_r8 += r7 * r3 (s)\n"
    "\tr11_r10 += r7 * r4 (s)\n"
    "\tr13_r12 += r5 * r3 (s)\n"
    "\tr15_r14 += r5 * r4 (s)\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr2 = [sp+8]\n"
    "\t[r2+24] = r8\n"
    "\t[r2+36] = r10\n"
    "\t[r2+28] = r12\n"
    "\t[r2+40] = r14\n"
    "\tr0 = [sp+16]\n"
    "\tr0 += 376\n"
    "\tr7 = [sp+12]\n"
    "\tr5 = [r0++=4]\n"
    "\tr6 = [r0++=4]\n"
    "\tr3 = [r7+0]\n"
    "\tr9_r8 = r3 * r5 (s)\n"
    "\tr3 = [r7+4]\n"
    "\tr11_r10 = r3 * r5 (s)\n"
    "\tr3 = [r7+8]\n"
    "\tr13_r12 = r3 * r5 (s)\n"
    "\tr3 = [r7+12]\n"
    "\tr15_r14 = r3 * r5 (s)\n"
    "\tr3 = 1048576\n"
    "\tr9_r8 += r3 * r6 (s)\n"
    "\tr11_r10 += r3 * r6 (s)\n"
    "\tr13_r12 += r3 * r6 (s)\n"
    "\tr15_r14 += r3 * r6 (s)\n"
    "\tr1 = [sp+20]\n"
    "\tr5 = [r0++=4]\n"
    "\tr3 = [r1+0]\n"
    "\tr4 = [r1+12]\n"
    "\tr6 = [r1+24]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+24]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+28]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+32]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+48]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+52]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+56]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+60]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+64]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+68]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr2 = [sp+72]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+40]\n"
    "\tr1 = [sp+76]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r2+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r2+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r2+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r2+44]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+0]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+12]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+24]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+36]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+4]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+16]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+28]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+40]\n"
    "\tr9_r8 += r3 * r5 (s)  #  r3 = [r1+8]\n"
    "\tr11_r10 += r4 * r5 (s)  #  r4 = [r1+20]\n"
    "\tr13_r12 += r6 * r5 (s)  #  r6 = [r1+32]\n"
    "\tr15_r14 += r7 * r5 (s)  #  r5 = [r0++=4]\n"
    "\tr7 = [r1+44]\n"
    "\tr9_r8 += r3 * r5 (s)\n"
    "\tr11_r10 += r4 * r5 (s)\n"
    "\tr13_r12 += r6 * r5 (s)\n"
    "\tr15_r14 += r7 * r5 (s)\n"
    "\tr9_r8 >>= 24\n"
    "\tr11_r10 >>= 24\n"
    "\tr13_r12 >>= 24\n"
    "\tr15_r14 >>= 24\n"
    "\tr4 = [sp+8]\n"
    "\t[r4+8] = r8\n"
    "\t[r4+20] = r10\n"
    "\t[r4+32] = r12\n"
    "\t[r4+44] = r14\n"
    "\tr4 += 48\n"
    "\t[sp+8] = r4\n"
    "\tr4 = [sp+12]\n"
    "\tr4 += 16\n"
    "\t[sp+12] = r4\n"
    "\tr4 = [sp+0]\n"
    "\tr2 = [r4+28]\n"
    "\tr3 = [r4+32]\n"
    "\tr1 = [sp+20]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+20] = r1\n"
    "\tr1 = [sp+24]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+24] = r1\n"
    "\tr1 = [sp+28]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+28] = r1\n"
    "\tr1 = [sp+32]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+32] = r1\n"
    "\tr1 = [sp+36]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+36] = r1\n"
    "\tr1 = [sp+40]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+40] = r1\n"
    "\tr1 = [sp+44]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+44] = r1\n"
    "\tr1 = [sp+48]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+48] = r1\n"
    "\tr1 = [sp+52]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+52] = r1\n"
    "\tr1 = [sp+56]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+56] = r1\n"
    "\tr1 = [sp+60]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+60] = r1\n"
    "\tr1 = [sp+64]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+64] = r1\n"
    "\tr1 = [sp+68]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+68] = r1\n"
    "\tr1 = [sp+72]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+72] = r1\n"
    "\tr1 = [sp+76]\n"
    "\tr1 += 48\n"
    "\tr5 = r1 + r3\n"
    "\tif (r1 >= r2) {\n"
    "\tr1 = r5\n"
    "\t}\n"
    "\t[sp+76] = r1\n"
    "\tr7 = [sp+4]\n"
    "\tr7 += -1\n"
    "\t[sp+4] = r7\n"
    "\tif (r7 != 0) goto .Lpxn_rdm15_asmg\n"
    "\tr4 = [sp+0]\n"
    "\tr7 = [sp+12]\n"
    "\t[r4+12] = r7\n"
    "\tr7 = [sp+8]\n"
    "\t[r4+16] = r7\n"
    "\tsp += 80\n"
    "\t{pc, r15-r4} = [sp++]\n"
    "\t.text\n"
    /* gcopy: r0 dst, r1 src, r2 frames, r3 row stride */
    "\t.global pxn_gcopy_asm\n"
    "pxn_gcopy_asm:\n"
    "\t[--sp] = {rets, r15-r4}\n"
    "\tr3 += -12\n"
    ".Lpxn_gc:\n"
    "\trep r2 {\n"
    "\t{r6-r4} = [r1++]\n"
    "\t[r0++] = {r6-r4}\n"
    "\tr0 += r3\n"
    "\t}\n"
    "\tif (r2 != 0) goto .Lpxn_gc\n"
    "\t{pc, r15-r4} = [sp++]\n");
#else
#define PXN_ASM 0
#endif
#ifndef PXN_HAND
#define PXN_HAND PXN_ASM
#endif

/* the routines in use: C until the start-up check (pxn_selftest) picks the hand ones */
static void (*pxn_dot3x4)(const pxn_dot *) = pxn_dot3x4_c;
static void (*pxn_dot1)(const pxn_dot *) = pxn_dot1_c;
static void (*pxn_post_f)(const pxn_post *) = pxn_post_c;
static void (*pxn_gcopy)(int32_t *, const int32_t *, int, int) = pxn_gcopy_c;
static void (*pxn_rdot_f)(const pxn_rdot *) = pxn_rdot_c;
static void (*pxn_rdot6_f)(const pxn_rdot *) = pxn_rdot_c;   /* for 6 taps */
static void (*pxn_rdq_f[PXN_HK + 1])(const pxn_rdot *);       /* by tap count: the load-ahead routines, if checked */
static void (*pxn_rdl_f[PXN_HK + 1])(pxn_rdot *);             /* by tap count: rdl, if checked */
static void (*pxn_rdm_f[2])(pxn_rdot *);                      /* rdm (3 rows) for 6 and 15 taps, if checked */
#define PXN_RDM(K) pxn_rdm_f[(K) == 15]                       /* (K: 6 or 15, the 3-row layers) */

/* ---- the chip's vector unit (in its FFT block, 0x1EE0400): dot products in hardware ----
 * One command computes nloop dot products of nlen 32-bit pairs, z = sum(y * x) >> q, the x, y and z addresses moving by
 * their own byte steps between loops: a layer's weight row (y, still) against its X rows (x, one row a loop), into
 * pxn_z (z, 3 words a loop). The register layout is the one in JieLi's AC79 SDK (apps/common/jl_math, Apache-2.0):
 * control 0xAA1 = dot product, x y z 32 bits; config0 = q << 24 | nloop << 12 | nlen; config1 = z step << 20 |
 * y step << 10 | x step; start 0x149, done = bit 7, cleared by setting bit 6. It runs beside the CPU. Used only if
 * the start-up check finds it there and within 1 of the C result on every value. */
static uint8_t pxn_vec_ok, pxn_vfail __attribute__((unused));
static int32_t pxn_vec_diff = -1;                       /* the start-up check's largest difference (-1: not run) */
static int pxn_mode_v;                                  /* see pxn_mode() */
int pxn_mode(void) { return pxn_mode_v; }
/* a command: nloop n dot products of nlen T: the weight row against x rows xs bytes apart, into z (zs bytes apart) */
static __attribute__((unused)) void pxn_vfill(volatile uint32_t *c, const int32_t *w32, const int32_t *x, int T, int xs, int zs, int32_t *z, int n)
{
    c[0] = 0xAA1u;                                       /* dot product, x y z 32 bits */
    c[1] = (uint32_t)(uintptr_t)x;
    c[2] = (uint32_t)(uintptr_t)w32;
    c[3] = (uint32_t)(uintptr_t)z;
    c[4] = ((uint32_t)PXN_QW << 24) | ((uint32_t)n << 12) | (uint32_t)T;
    c[5] = ((uint32_t)zs << 20) | (uint32_t)xs;
    c[6] = 0;
    c[7] = 0;
}
#if PXN_ASM
#define PXN_FFT_CON   (*(volatile uint32_t *)0x1EE0400u)
#define PXN_FFT_CADR  (*(volatile uint32_t *)0x1EE0404u)
#define PXN_FFT_CONST (*(volatile uint32_t *)0x1EE0408u)
static volatile uint32_t pxn_vctx[8] __attribute__((aligned(32)));
/* 0 = done, -1 = no answer (the unit is not there or not clocked) */
static int pxn_vdot(const int32_t *w, const int32_t *x, int T, int sb, int32_t *z, int n)
{
    pxn_vfill(pxn_vctx, w, x, T, sb, 12, z, n);
    __asm__ volatile("csync" ::: "memory");
    PXN_FFT_CONST = 0;
    PXN_FFT_CADR = (uint32_t)(uintptr_t)pxn_vctx;
    PXN_FFT_CON = 0x149u;
    for (uint32_t k = 0; !(PXN_FFT_CON & 0x80u); k++)
        if (k > 400000u) { PXN_FFT_CON = 0; return -1; }
    PXN_FFT_CON |= 0x40u;
    return 0;
}
/* the same, without waiting (the bench: what runs beside it); pxn_vwait waits for it */
void pxn_vstart(const int32_t *w, const int32_t *x, int T, int sb, int zb, int32_t *z, int n)
{
    pxn_vfill(pxn_vctx, w, x, T, sb, zb, z, n);
    __asm__ volatile("csync" ::: "memory");
    PXN_FFT_CONST = 0;
    PXN_FFT_CADR = (uint32_t)(uintptr_t)pxn_vctx;
    PXN_FFT_CON = 0x149u;
}
void pxn_vwait(void)
{
    for (uint32_t k = 0; !(PXN_FFT_CON & 0x80u); k++)
        if (k > 4000000u) { PXN_FFT_CON = 0; return; }
    PXN_FFT_CON |= 0x40u;
}
#else
void pxn_vstart(const int32_t *w, const int32_t *x, int T, int sb, int zb, int32_t *z, int n)
{
    (void)w; (void)x; (void)T; (void)sb; (void)zb; (void)z; (void)n;
}
void pxn_vwait(void) { }
#if PXN_HAND
static int pxn_vdot(const int32_t *w, const int32_t *x, int T, int sb, int32_t *z, int n)   /* (no unit) */
{
    (void)w; (void)x; (void)T; (void)sb; (void)z; (void)n;
    return -1;
}
#endif
#endif
/* the command queue (mode 2): commands wait in their own contexts; pxn_kick, called often by the CPU's work, starts the
 * next one as soon as the unit is done. Counters: queued, started, finished. */
#if PXN_ASM || defined(PXN_EMUVEC)
#define PXN_VQON 1
#define PXN_VQ 8
static volatile uint32_t pxn_vq[PXN_VQ][8] __attribute__((aligned(32)));
static uint32_t pxn_vq_in, pxn_vq_st, pxn_vq_done;
#if PXN_ASM
static inline __attribute__((always_inline)) void pxn_kick(void)
{
    if (pxn_vq_st != pxn_vq_done) {
        if (!(PXN_FFT_CON & 0x80u)) return;
        PXN_FFT_CON |= 0x40u;
        pxn_vq_done++;
    }
    if (pxn_vq_st != pxn_vq_in) {
        __asm__ volatile("csync" ::: "memory");
        PXN_FFT_CONST = 0;
        PXN_FFT_CADR = (uint32_t)(uintptr_t)pxn_vq[pxn_vq_st % PXN_VQ];
        PXN_FFT_CON = 0x149u;
        pxn_vq_st++;
    }
}
#define PXN_VSTOP() (PXN_FFT_CON = 0)
#else
/* PC tests (PXN_EMUVEC): a unit that finishes each command at some later kick, at random, with the C dot product
 * reading the X rows then: a row overwritten too early shows as a wrong result */
static struct { const int32_t *w, *x; int32_t *z; } pxn_vqp[PXN_VQ];
static uint32_t pxn_emu_r = 1u;
static void pxn_kick(void)
{
    if (pxn_vq_st != pxn_vq_done) {
        pxn_emu_r = pxn_emu_r * 1103515245u + 12345u;
        if ((pxn_emu_r >> 27) & 3u) return;
        const int k = (int)(pxn_vq_done % PXN_VQ);
        pxn_dot d = {pxn_vqp[k].w, pxn_vqp[k].x, (int32_t)(pxn_vq[k][4] & 0xFFFu), PXN_S * 4, pxn_vqp[k].z,
                     (int32_t)(pxn_vq[k][4] >> 12 & 0xFFFu)};
        pxn_dot1_c(&d);
        pxn_vq_done++;
    }
    if (pxn_vq_st != pxn_vq_in) pxn_vq_st++;
}
#define PXN_VSTOP() ((void)0)
static int32_t pxn_emu_fast[PXN_FAST_WORDS];
void pxn_emu_on(int mode)                               /* (before pxn_load: the unit's fast copies are tested too) */
{
    pxn_vec_ok = 1; pxn_vfail = 0; pxn_mode_v = mode;
    pxn_xf = pxn_emu_fast; pxn_wf = pxn_emu_fast + (PXN_N + 1) * PXN_S;
}
#endif
/* the unit stopped answering: off for good (the CPU does the rest), queue emptied */
static void pxn_vdead(void)
{
    PXN_VSTOP();
    pxn_vfail = 1;
    pxn_vec_ok = 0;
    pxn_vq_in = pxn_vq_st = pxn_vq_done = 0;
}
static int pxn_vupto(uint32_t seq)                      /* waits until `seq` commands are finished; -1 = unit dead */
{
    for (uint32_t k = 0; (int32_t)(pxn_vq_done - seq) < 0; k++) {
        pxn_kick();
        if (k > 2000000u) { pxn_vdead(); return -1; }
    }
    return 0;
}
static void pxn_venq(const int32_t *w, const int32_t *x, int T, int32_t *z, int n)
{
    if (pxn_vq_in - pxn_vq_done >= PXN_VQ && pxn_vupto(pxn_vq_in - PXN_VQ + 1)) return;
    volatile uint32_t *c = pxn_vq[pxn_vq_in % PXN_VQ];
    pxn_vfill(c, w, x, T, PXN_S * 4, 12, z, n);           /* (on the PC: pxn_vqp) */
#if !PXN_ASM
    pxn_vqp[pxn_vq_in % PXN_VQ].w = w; pxn_vqp[pxn_vq_in % PXN_VQ].x = x; pxn_vqp[pxn_vq_in % PXN_VQ].z = z;
#endif
    pxn_vq_in++;
    pxn_kick();
}
#else
#define PXN_VQON 0
#define pxn_kick() ((void)0)
#endif
int pxn_vec_check(void) { return pxn_vec_ok ? (int)pxn_vec_diff : -1 - (pxn_vec_diff >= 0 ? (int)pxn_vec_diff : 0); }

/* ---- start-up check ---- */
/* the test pattern: X rows (9 of PXN_S + slack), weight rows, residual rows, both signs, values as large as a model
 * meets (+-32); scratch: PXN_TEST_WORDS */
#define PXN_T_X(s)   (s)
#define PXN_T_W(s)   ((s) + 10 * PXN_S)
#define PXN_T_RR(s)  (PXN_T_W(s) + 3 * PXN_S + 4)
#define PXN_T_O(s, k) (PXN_T_RR(s) + 12 + (k) * 64)       /* 8 output areas of 64 words */
static void pxn_test_data_sh(int32_t *s, int sh)
{
    uint32_t r = 12345u;
    for (int i = 0; i < 13 * PXN_S + 16; i++) {
        r = r * 1103515245u + 12345u;
        s[i] = (int32_t)r >> sh;
    }
    for (int k = 0; k < 8; k++)
        for (int i = 0; i < 64; i++) PXN_T_O(s, k)[i] = i * 77 - 1000;
}
static void pxn_test_data(int32_t *s) { pxn_test_data_sh(s, 6); }
int pxn_selftest(int32_t *scratch)
{
#if PXN_HAND
    int32_t *s = scratch, ok = 1, p2ok = 1, p3ok = 1, p4ok = 1, p5ok = 1;
    for (int pass = 0; pass < 4 && ok; pass++) {
        pxn_test_data(s);
        const int T = pass & 1 ? 47 : 20, sb = PXN_S * 4;
        pxn_dot d = {PXN_T_W(s), PXN_T_X(s), T, sb, PXN_T_O(s, 0), 2};
        pxn_dot3x4_c(&d);
        d.z = PXN_T_O(s, 1); pxn_dot3x4_asm(&d);
        d.ng = 3; d.x = PXN_T_X(s) + PXN_S; d.z = PXN_T_O(s, 2) + 1; pxn_dot1_c(&d);
        d.z = PXN_T_O(s, 3) + 1; pxn_dot1_asm(&d);
        pxn_post p = {PXN_T_O(s, 0), PXN_T_X(s) + 2 + pass, sb, PXN_T_RR(s), PXN_T_O(s, 4), pass < 2 ? PXN_T_O(s, 5) : 0, 7};
        pxn_post_c(&p);
        p.hs = PXN_T_O(s, 6); if (p.out) p.out = PXN_T_O(s, 7);
        pxn_post_asm(&p);
        if (memcmp(PXN_T_O(s, 0), PXN_T_O(s, 1), 64 * 4) || memcmp(PXN_T_O(s, 2), PXN_T_O(s, 3), 64 * 4) ||
            memcmp(PXN_T_O(s, 4), PXN_T_O(s, 6), 64 * 4) || memcmp(PXN_T_O(s, 5), PXN_T_O(s, 7), 64 * 4)) ok = 0;
        {   /* post2 against the C post (same z, x, residual; hs and out from the same start): z is O0, still
             * the C dot's; hs and out in O2 and O3, whose check is done */
            int32_t *r2 = PXN_T_O(s, 8);
            pxn_rr2(PXN_T_RR(s), r2);
            for (int i = 0; i < 64; i++) PXN_T_O(s, 2)[i] = PXN_T_O(s, 3)[i] = i * 77 - 1000;
            pxn_post q = p;
            q.hs = PXN_T_O(s, 2); if (q.out) q.out = PXN_T_O(s, 3);
            q.rr2 = r2;
            pxn_post2_asm(&q);
            if (memcmp(PXN_T_O(s, 2), PXN_T_O(s, 4), 64 * 4) || memcmp(PXN_T_O(s, 3), PXN_T_O(s, 5), 64 * 4)) p2ok = 0;
            for (int i = 0; i < 64; i++) PXN_T_O(s, 2)[i] = PXN_T_O(s, 3)[i] = i * 77 - 1000;
            pxn_post3_asm(&q);                            /* post3: the same check */
            if (memcmp(PXN_T_O(s, 2), PXN_T_O(s, 4), 64 * 4) || memcmp(PXN_T_O(s, 3), PXN_T_O(s, 5), 64 * 4)) p3ok = 0;
            /* post4: x 3 words a frame (as in a ring), against the C post on the same x */
            pxn_post c4 = p;
            c4.x = PXN_T_X(s) + pass; c4.sb = 12; c4.rr2 = r2;
            c4.hs = PXN_T_O(s, 2); if (c4.out) c4.out = PXN_T_O(s, 3);
            for (int i = 0; i < 64; i++) PXN_T_O(s, 2)[i] = PXN_T_O(s, 3)[i] = PXN_T_O(s, 6)[i] = PXN_T_O(s, 7)[i] = i * 77 - 1000;
            pxn_post_c(&c4);
            pxn_post q4 = c4;
            q4.hs = PXN_T_O(s, 6); if (q4.out) q4.out = PXN_T_O(s, 7);
            pxn_post4_asm(&q4);
            if (memcmp(PXN_T_O(s, 2), PXN_T_O(s, 6), 64 * 4) || memcmp(PXN_T_O(s, 3), PXN_T_O(s, 7), 64 * 4)) p4ok = 0;
            if (c4.out) {                                 /* post5: the same check, 6 frames (it takes them in twos) */
                for (int i = 0; i < 64; i++) PXN_T_O(s, 2)[i] = PXN_T_O(s, 3)[i] = PXN_T_O(s, 6)[i] = PXN_T_O(s, 7)[i] = i * 77 - 1000;
                c4.n = q4.n = 6;
                pxn_post_c(&c4);
                pxn_post5_asm(&q4);
                if (memcmp(PXN_T_O(s, 2), PXN_T_O(s, 6), 64 * 4) || memcmp(PXN_T_O(s, 3), PXN_T_O(s, 7), 64 * 4)) p5ok = 0;
            }
        }
        pxn_gcopy_c(PXN_T_O(s, 0), PXN_T_W(s) + pass, 5, 40);
        pxn_gcopy_asm(PXN_T_O(s, 1), PXN_T_W(s) + pass, 5, 40);
        if (memcmp(PXN_T_O(s, 0), PXN_T_O(s, 1), 64 * 4)) ok = 0;
    }
    {
        static const uint8_t QK[3] = {6, 15, 16};
#if !PXN_LEAN
        /* rdot: 3 rows of 6 taps, 3 rows of 15, the head's 1 row of 16, against the C one */
        int rok = 1;
        for (int pass = 0; pass < 3 && rok; pass++) {
            pxn_test_data(s);
            const int K = pass == 0 ? 6 : pass == 1 ? 15 : 16, rows = pass == 2 ? 1 : 3;
            const int32_t *tp[16];
            for (int k = 0; k < K; k++) tp[k] = PXN_T_X(s) + 3 * (k * 5 + pass);
            pxn_rdot j = {PXN_T_W(s), tp, K, PXN_T_X(s) + 400, PXN_T_O(s, 0), rows};
            pxn_rdot_c(&j);
            j.z = PXN_T_O(s, 1);
            pxn_rdot_asm(&j);
            if (memcmp(PXN_T_O(s, 0), PXN_T_O(s, 1), 64 * 4)) rok = 0;
            if (K == 6) {
                j.z = PXN_T_O(s, 2);
                pxn_rdot6_asm(&j);
                if (memcmp(PXN_T_O(s, 0), PXN_T_O(s, 2), 64 * 4)) rok = 0;
            }
        }
        pxn_rdot_f = rok ? pxn_rdot_asm : pxn_rdot_c;
        pxn_rdot6_f = rok ? pxn_rdot6_asm : pxn_rdot_c;
        /* the load-ahead routines: 3 rows of 6 and 15 taps, 1 and 3 rows of 16, each against C */
        static void (*const Q[3])(const pxn_rdot *) = {pxn_rdq6_asm, pxn_rdq15_asm, pxn_rdq16_asm};
        for (int q = 0; q < 3; q++) {
            int qok = 1;
            for (int pass = 0; pass < 4 && qok; pass++) {
                pxn_test_data(s);
                const int K = QK[q], rows = pass & 1 ? 1 : 3;
                const int32_t *tp[16];
                for (int k = 0; k < K; k++) tp[k] = PXN_T_X(s) + 3 * (k * 5 + pass);
                pxn_rdot j = {PXN_T_W(s), tp, K, PXN_T_X(s) + 400, PXN_T_O(s, 0), rows};
                pxn_rdot_c(&j);
                j.z = PXN_T_O(s, 1);
                Q[q](&j);
                if (memcmp(PXN_T_O(s, 0), PXN_T_O(s, 1), 64 * 4)) qok = 0;
            }
            pxn_rdq_f[QK[q]] = qok ? Q[q] : 0;
        }
#endif
        /* rdl: 3 groups of 3 rows (6, 15 taps) and of 1 row (16), on a "ring" whose end some taps pass (brought back
         * by its length), against the C one: outputs, and the job's u and z after */
        static void (*const R[3])(pxn_rdot *) = {pxn_rdl6_asm, pxn_rdl15_asm, pxn_rdl16_asm};
        for (int q = 0; q < 3; q++) {
            int lok = 1;
            for (int pass = 0; pass < 2 && lok; pass++) {
                pxn_test_data(s);
                const int K = QK[q], rows = q == 2 ? 1 : 3;
                const int32_t *tp[16];
                for (int k = 0; k < K; k++) tp[k] = PXN_T_X(s) + 3 * (k * 5 + pass);
                pxn_rdot j = {PXN_T_W(s), tp, K, PXN_T_X(s) + 400, PXN_T_O(s, 0), rows, 3,
                              PXN_T_X(s) + 3 * (K * 4), -3 * (K * 3) * 4};
                pxn_rdot c = j;
                pxn_rdl_c(&c);
                j.z = PXN_T_O(s, 1);
                R[q](&j);
                if (memcmp(PXN_T_O(s, 0), PXN_T_O(s, 1), 64 * 4) || j.u != c.u || j.z - PXN_T_O(s, 1) != c.z - PXN_T_O(s, 0))
                    lok = 0;
            }
            pxn_rdl_f[QK[q]] = lok && (PXN_LEAN || pxn_rdq_f[QK[q]]) ? R[q] : 0;
        }
        /* rdm: the same check, the 3-row layers (6 and 15 taps) */
        static void (*const M[2])(pxn_rdot *) = {pxn_rdm6_asm, pxn_rdm15_asm};
        for (int q = 0; q < 2; q++) {
            int mok = 1;
            for (int pass = 0; pass < 2 && mok; pass++) {
                pxn_test_data(s);
                const int K = QK[q];
                const int32_t *tp[16];
                for (int k = 0; k < K; k++) tp[k] = PXN_T_X(s) + 3 * (k * 5 + pass);
                pxn_rdot j = {PXN_T_W(s), tp, K, PXN_T_X(s) + 400, PXN_T_O(s, 0), 3, 3,
                              PXN_T_X(s) + 3 * (K * 4), -3 * (K * 3) * 4};
                pxn_rdot c = j;
                pxn_rdl_c(&c);
                j.z = PXN_T_O(s, 1);
                M[q](&j);
                if (memcmp(PXN_T_O(s, 0), PXN_T_O(s, 1), 64 * 4) || j.u != c.u || j.z - PXN_T_O(s, 1) != c.z - PXN_T_O(s, 0))
                    mok = 0;
            }
            pxn_rdm_f[q] = mok && pxn_rdl_f[QK[q]] ? M[q] : 0;
        }
    }
    if (ok) {
        pxn_dot3x4 = pxn_dot3x4_asm; pxn_dot1 = pxn_dot1_asm; pxn_gcopy = pxn_gcopy_asm;
        pxn_post_f = p2ok ? pxn_post2_asm : pxn_post_asm;
        pxn_p3ok = (uint8_t)(p2ok && p3ok);             /* pxn_tune times post3 against post2 */
        pxn_p4ok = (uint8_t)(p2ok && p4ok);             /* and post4 against both */
        pxn_p5ok = (uint8_t)(pxn_p4ok && p5ok);         /* and post5 against post4, whose frames it takes */
    }
    /* the vector unit: rows of 20 and 47 over 8 X rows, against the C dot */
    {
        int vok = 1;
        int32_t maxd = 0;
        for (int pass = 0; pass < 2 && vok; pass++) {
            pxn_test_data_sh(s, 9);                       /* +-4 (the z values a model gives fit in 32 bits) */
            const int T = pass ? 47 : 20;
            pxn_dot d = {PXN_T_W(s), PXN_T_X(s), T, PXN_S * 4, PXN_T_O(s, 0), 8};
            pxn_dot1_c(&d);
            if (pxn_vdot(PXN_T_W(s), PXN_T_X(s), T, PXN_S * 4, PXN_T_O(s, 1), 8)) { vok = 0; maxd = 99999; break; }
            for (int f = 0; f < 8; f++) {
                int32_t df = PXN_T_O(s, 0)[3 * f] - PXN_T_O(s, 1)[3 * f];
                if (df < 0) df = -df;
                if (df > maxd) maxd = df;
            }
        }
        pxn_vec_diff = maxd;
        pxn_vec_ok = (uint8_t)(vok && maxd <= 1);
        pxn_vfail = 0;
        pxn_mode_v = pxn_vec_ok ? 1 : 0;
    }
    /* the fast memory offered (pxn_set_fast): the unit reading X rows and weights there, against the C dot */
    pxn_xf = pxn_wf = 0;
    if (pxn_fast && pxn_vec_ok) {
        int32_t *xf = pxn_fast, *wf = pxn_fast + (PXN_N + 1) * PXN_S, fok = 1;
        for (int pass = 0; pass < 2 && fok; pass++) {
            pxn_test_data_sh(s, 9);
            const int T = pass ? 47 : 20;
            memcpy(xf, PXN_T_X(s), 9u * PXN_S * 4u);
            memcpy(wf, PXN_T_W(s), (unsigned)T * 4u + 4u);
            pxn_dot d = {PXN_T_W(s), PXN_T_X(s), T, PXN_S * 4, PXN_T_O(s, 0), 8};
            pxn_dot1_c(&d);
            if (pxn_vdot(wf, xf, T, PXN_S * 4, PXN_T_O(s, 1), 8)) { fok = 0; break; }
            for (int f = 0; f < 8; f++) {
                int32_t df = PXN_T_O(s, 0)[3 * f] - PXN_T_O(s, 1)[3 * f];
                if (df < -1 || df > 1) fok = 0;
            }
        }
        if (fok) { pxn_xf = xf; pxn_wf = wf; }
    }
    return ok;
#else
    (void)scratch;
    return 0;
#endif
}
int pxn_post2_in_use(void)
{
#if PXN_HAND
    return pxn_post_f == pxn_post2_asm || pxn_post_f == pxn_post3_asm;
#else
    return 0;
#endif
}
void pxn_set_fast(int32_t *base, unsigned bytes) { pxn_fast = bytes >= PXN_FAST_WORDS * 4u ? base : 0; }
int pxn_fast_in_use(void) { return pxn_xf != 0; }
int pxn_rdot_chosen(void) { return pxn_rd; }
int pxn_rdq_in_use(void) { return pxn_rdq_on && pxn_rdq_f[6]; }
int pxn_rdl_state(void) { return pxn_rdl_f[6] && pxn_rdl_f[15] && pxn_rdl_f[16] ? 1 + (pxn_rdl_on != 0) : 0; }
int pxn_rdm_state(void) { return pxn_rdm_f[0] && pxn_rdm_f[1] ? 1 + (pxn_rdm_on != 0) : 0; }
int pxn_rdq_checked(void) { return pxn_rdq_f[6] && pxn_rdq_f[15] && pxn_rdq_f[16]; }
int pxn_rdot_in_use(void)
{
#if PXN_HAND && !PXN_LEAN
    return pxn_rdot_f == pxn_rdot_asm;
#else
    return 0;
#endif
}
int pxn_asm_in_use(void)
{
#if PXN_HAND
    return pxn_dot3x4 == pxn_dot3x4_asm;
#else
    return 0;
#endif
}

/* ---- measurements for the firmware's bench screen (BENCH_AT_BOOT) ---- */
#if !PXN_LEAN   /* v40.4: nothing calls them in a shipped image (the linker keeps every function): out of the Bluetooth test image, whose flash is full */
/* ticks for `reps` runs of the work of one kernel-6 layer over 32 frames (gather 6 taps, 3 dot rows of 20, post with
 * the residual): 1,920 multiply-adds a run. hand: the hand routines (if any), else C. scratch: PXN_TEST_WORDS */
uint32_t pxn_bench_layer(int32_t *scratch, uint32_t (*ticks)(void), int hand, int reps)
{
    int32_t ring[3 * 64] = {0};                    /* (on the stack: only the bench screen runs this, at boot) */
    int32_t *s = scratch;
    void (*d4)(const pxn_dot *) = pxn_dot3x4_c;
    void (*po)(const pxn_post *) = pxn_post_c;
    void (*gc)(int32_t *, const int32_t *, int, int) = pxn_gcopy_c;
#if PXN_ASM
    if (hand) { d4 = pxn_dot3x4_asm; po = pxn_post_asm; gc = pxn_gcopy_asm; }
#else
    (void)hand;
#endif
    pxn_test_data(s);
    uint32_t t0 = ticks();
    for (int r = 0; r < reps; r++) {
        for (int k = 0; k < 6; k++) gc(pxn_x + 2 + 3 * k, ring + 3 * k, 32, PXN_S * 4);
        pxn_dot d = {PXN_T_W(s), pxn_x, 20, PXN_S * 4, pxn_z, 8};
        d4(&d);
        pxn_post p = {pxn_z, pxn_x + 17, PXN_S * 4, PXN_T_RR(s), pxn_hs, ring, 32, 0};
        po(&p);
    }
    return ticks() - t0;
}

/* ticks for `reps` runs of one part of that layer work, by hand (C without hand code): 0 the dot (3 rows of 20 for
 * 32 frames: 1,920 multiply-adds), 1 the gather (6 taps x 32 frames), 2 the post (32 frames, with the residual) */
uint32_t pxn_bench_part(int32_t *scratch, uint32_t (*ticks)(void), int part, int reps)
{
    int32_t ring[3 * 64] = {0};                    /* (on the stack: only the bench screen runs this, at boot) */
    int32_t *s = scratch;
    pxn_test_data(s);
    pxn_dot d = {PXN_T_W(s), pxn_x, 20, PXN_S * 4, pxn_z, 8};
    pxn_post p = {pxn_z, pxn_x + 17, PXN_S * 4, PXN_T_RR(s), pxn_hs, ring, 32, 0};
    uint32_t t0 = ticks();
    for (int r = 0; r < reps; r++) {
#if PXN_ASM
        if (part == 3) { for (int o = 0; o < 3; o++) pxn_vdot(d.w + 20 * o, pxn_x, 20, PXN_S * 4, pxn_z + o, 32); }
        else if (part == 0) pxn_dot3x4_asm(&d);
        else if (part == 1) for (int k = 0; k < 6; k++) pxn_gcopy_asm(pxn_x + 2 + 3 * k, ring + 3 * k, 32, PXN_S * 4);
        else pxn_post_asm(&p);
#else
        if (part == 0) pxn_dot3x4_c(&d);
        else if (part == 1) for (int k = 0; k < 6; k++) pxn_gcopy_c(pxn_x + 2 + 3 * k, ring + 3 * k, 32, PXN_S * 4);
        else pxn_post_c(&p);
#endif
    }
    return ticks() - t0;
}

/* ticks for `reps` runs of a layer's dot work over 32 frames (3 rows of 20: 1,920 multiply-adds), on buffers given
 * (x: 33 rows of PXN_S words, w: 64 words, z: 100 words), by the CPU routine (vec 0) or the vector unit (vec 1);
 * then of the post over 32 frames on one buffer (4,000 words: z, X rows, rr, hs, out). For the bench screen:
 * where the data sits changes the cost on this chip. */
uint32_t pxn_bench_dot(int32_t *x, int32_t *w, int32_t *z, int vec, uint32_t (*ticks)(void), int reps)
{
    uint32_t r = 777u;
    for (int i = 0; i < 33 * PXN_S; i++) { r = r * 1103515245u + 12345u; x[i] = (int32_t)r >> 10; }
    for (int i = 0; i < 64; i++) { r = r * 1103515245u + 12345u; w[i] = (int32_t)r >> 10; }
    pxn_dot d = {w, x, 20, PXN_S * 4, z, 8};
    uint32_t t0 = ticks();
    for (int k = 0; k < reps; k++) {
#if PXN_ASM
        if (vec) { for (int o = 0; o < 3; o++) pxn_vdot(w + 20 * o, x, 20, PXN_S * 4, z + o, 32); }
        else pxn_dot3x4_asm(&d);
#else
        (void)vec;
        pxn_dot3x4_c(&d);
#endif
    }
    return ticks() - t0;
}
uint32_t pxn_bench_post(int32_t *b, uint32_t (*ticks)(void), int reps, int v2)
{
    int32_t *z = b, *x = b + 100, *rr = b + 100 + 33 * PXN_S, *rr2 = rr + 12, *hs = rr + 32, *out = hs + 100;
    uint32_t r = 99u;
    for (int i = 0; i < 4000; i++) { r = r * 1103515245u + 12345u; b[i] = (int32_t)r >> 10; }
    pxn_rr2(rr, rr2);
    pxn_post p = {z, x + 17, PXN_S * 4, rr, hs, out, 32, rr2};
    uint32_t t0 = ticks();
    for (int k = 0; k < reps; k++) {
#if PXN_ASM
        if (v2) pxn_post2_asm(&p); else pxn_post_asm(&p);
#else
        (void)v2;
        pxn_post_c(&p);
#endif
    }
    return ticks() - t0;
}
/* ticks for `reps` vector commands of nloop dot products of nlen words (x moving xs bytes and z zs bytes a loop),
 * each waited for; start_only: only the CPU's part of starting one (it is waited for outside the timing).
 * buf: 40,000 words (x from 0, the weights at 30,000, z at 31,000) */
uint32_t pxn_bench_vcmd(int32_t *buf, uint32_t (*ticks)(void), int nlen, int nloop, int xs, int zs, int reps, int start_only)
{
#if PXN_ASM
    int32_t *x = buf, *w = buf + 30000, *z = buf + 31000;
    for (int i = 0; i < 1000; i++) { w[i] = i * 13 - 6000; x[i] = 3000 - i * 7; }
    uint32_t dt = 0;
    if (start_only) {
        for (int k = 0; k < reps; k++) {
            uint32_t t0 = ticks();
            pxn_vstart(w, x, nlen, xs, zs, z, nloop);
            dt += ticks() - t0;
            pxn_vwait();
        }
        return dt;
    }
    uint32_t t0 = ticks();
    for (int k = 0; k < reps; k++) { pxn_vstart(w, x, nlen, xs, zs, z, nloop); pxn_vwait(); }
    return ticks() - t0;
#else
    (void)buf; (void)ticks; (void)nlen; (void)nloop; (void)xs; (void)zs; (void)reps; (void)start_only;
    return 0;
#endif
}

/* the vector unit's speed by shape (v37 diagnostic 11): one command of nloop dot products of 20 words, waited for;
 * ticks. v: 0 1000 loops, x step 0, z step 0, x and weights at one address, small values (an earlier measurement gave
 * 1.5 cycles a multiply-add like this); 1 the same with 32 loops; 2 z step 12; 3 x and weights apart; 4 large values;
 * 5 the network's shape (500 loops, x step 80, z step 12, apart, large values); 6 the same, the CPU not reading the
 * unit's register for the first half (a loop on registers only); 7 as 5 with the engine's X row stride (200 bytes).
 * buf: 21,000 words */
#if PXN_ASM
static void __attribute__((noinline)) pxn_spin(uint32_t n) { for (uint32_t i = 0; i < n; i++) __asm__ volatile(""); }
#endif
uint32_t pxn_bench_vfast(int32_t *buf, uint32_t (*ticks)(void), int v)
{
#if PXN_ASM
    int32_t *x = buf, *w = v == 3 || v >= 5 ? buf + 12000 : buf, *z = buf + 13000;
    const int big = v >= 4, nloop = v == 1 ? 32 : v == 7 ? 200 : v >= 5 ? 500 : 1000;
    const int xs = v == 7 ? 200 : v >= 5 ? 80 : 0, zs = v == 2 || v >= 5 ? 12 : 0;
    uint32_t r = 2468u;
    for (int i = 0; i < 12100; i++) {
        r = r * 1103515245u + 12345u;
        buf[i] = big ? (int32_t)r >> 6 : 3000 - (i % 1000) * 7;
    }
    if (v == 6) {
        uint32_t t0 = ticks();
        pxn_spin(100000u);
        const uint32_t per = ticks() - t0;              /* ticks for 100,000 turns of the register loop */
        t0 = ticks();
        pxn_vstart(w, x, 20, xs, zs, z, nloop);
        pxn_vwait();
        const uint32_t whole = ticks() - t0;
        t0 = ticks();
        pxn_vstart(w, x, 20, xs, zs, z, nloop);
        pxn_spin(per ? (uint32_t)((uint64_t)whole * 100000u / per / 2u) : 0u);
        pxn_vwait();
        return ticks() - t0;
    }
    uint32_t t0 = ticks();
    pxn_vstart(w, x, 20, xs, zs, z, nloop);
    pxn_vwait();
    return ticks() - t0;
#else
    (void)buf; (void)ticks; (void)v;
    return 0;
#endif
}
int pxn_vfast_macs(int v) { return 20 * (v == 1 ? 32 : v == 7 ? 200 : v >= 5 ? 500 : 1000); }

#if PXN_ASM
/* one instruction (or pair) n times in the hardware loop: what each kind costs on this CPU */
#define PXN_UB(name, body)                                                                                        \
    "\t.global " name "\n" name ":\n\t[--sp] = {rets, r15-r4}\n"                                              \
    "\tr4 = 0\n\tr5 = 0\n\tr6 = 0\n\tr7 = 0\n\tr3 = 5\n\tr10 = 3\n\tr12 = 4\n\tr2 = r0\n\tr11 = r1\n"          \
    ".L" name ":\n\trep r11 {\n" body "\t}\n\tif (r11 != 0) goto .L" name "\n"                                  \
    "\t{pc, r15-r4} = [sp++]\n"
#if !PXN_LEAN
__asm__("\t.section .text.pxn_cold,\"ax\"\n"
        PXN_UB("pxn_ub_ld", "\tr3 = [r2++=4]\n")
        PXN_UB("pxn_ub_ldd", "\tr5_r4 = d[r2++=8]\n")
        PXN_UB("pxn_ub_ldm3", "\t{r6-r4} = [r2++]\n")
        PXN_UB("pxn_ub_st", "\t[r2++=4] = r3\n")
        PXN_UB("pxn_ub_rmac", "\tr5_r4 += r3 * r10 (s)\n")
        PXN_UB("pxn_ub_pair", "\tr5_r4 += r3 * r10 (s)  #  r3 = [r2++=4]\n")
        PXN_UB("pxn_ub_fmac", "\tr5_r4 += [r2 ++= r12]*r10 (s)\n")
        PXN_UB("pxn_ub_rmac2", "\tr5_r4 += r3 * r10 (s)\n\tr7_r6 += r3 * r10 (s)\n")
        /* diagnostic 12: a multiply-add paired with a load whose value it does not use (nd), 2 or 4 accumulators, with
         * and without each multiply using the value the pair before loaded (the hand routines do) */
        PXN_UB("pxn_ub_pnd", "\tr5_r4 += r3 * r10 (s)  #  r6 = [r2++=4]\n")
        PXN_UB("pxn_ub_p2nd", "\tr5_r4 += r3 * r10 (s)  #  r6 = [r2++=4]\n\tr9_r8 += r3 * r10 (s)  #  r7 = [r2++=4]\n")
        PXN_UB("pxn_ub_p4dep", "\tr5_r4 += r3 * r10 (s)  #  r3 = [r2++=4]\n\tr9_r8 += r3 * r10 (s)  #  r3 = [r2++=4]\n"
               "\tr15_r14 += r3 * r10 (s)  #  r3 = [r2++=4]\n\tr7_r6 += r3 * r10 (s)  #  r3 = [r2++=4]\n")
        PXN_UB("pxn_ub_p4nd", "\tr5_r4 += r3 * r10 (s)  #  r1 = [r2++=4]\n\tr9_r8 += r3 * r10 (s)  #  r1 = [r2++=4]\n"
               "\tr15_r14 += r3 * r10 (s)  #  r1 = [r2++=4]\n\tr7_r6 += r3 * r10 (s)  #  r1 = [r2++=4]\n"));
void pxn_ub_ld(const int32_t *, int), pxn_ub_ldd(const int32_t *, int), pxn_ub_ldm3(const int32_t *, int),
    pxn_ub_st(const int32_t *, int), pxn_ub_rmac(const int32_t *, int), pxn_ub_pair(const int32_t *, int),
    pxn_ub_fmac(const int32_t *, int), pxn_ub_rmac2(const int32_t *, int), pxn_ub_pnd(const int32_t *, int),
    pxn_ub_p2nd(const int32_t *, int), pxn_ub_p4dep(const int32_t *, int), pxn_ub_p4nd(const int32_t *, int);
#endif
#endif
/* ticks for reps x n loops of `kind`: 0 load, 1 64-bit load, 2 3-word load, 3 store, 4 multiply-add (registers),
 * 5 multiply-add paired with a load, 6 multiply-add with its load, 7 two multiply-adds; buf: 1024 words. n: the
 * loop count used (written). 0 without hand code. */
uint32_t pxn_bench_ops(int kind, int32_t *buf, int reps, int *n, uint32_t (*ticks)(void))
{
#if PXN_ASM && !PXN_LEAN
    static void (*const F[12])(const int32_t *, int) = {pxn_ub_ld, pxn_ub_ldd, pxn_ub_ldm3, pxn_ub_st,
                                                         pxn_ub_rmac, pxn_ub_pair, pxn_ub_fmac, pxn_ub_rmac2,
                                                         pxn_ub_pnd, pxn_ub_p2nd, pxn_ub_p4dep, pxn_ub_p4nd};
    static const int N[12] = {1024, 512, 340, 1024, 1024, 1024, 1024, 1024, 1024, 512, 256, 256};
    static const int P[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 4, 4};   /* loads (pairs) a loop */
    if (kind < 0 || kind > 11) kind = 0;
    *n = N[kind] * P[kind];
    uint32_t t0 = ticks();
    for (int r = 0; r < reps; r++) F[kind](buf, N[kind]);
    return ticks() - t0;
#else
    (void)kind; (void)buf; (void)reps; (void)ticks;
    *n = 1;
    return 0;
#endif
}

#endif

/* ---- loading ---- */
static int pxn_finite(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    return (v.u & 0x7F800000u) != 0x7F800000u;
}
/* to fixed point with q fraction bits, rounded and clamped (real A2 weights stay below 2) */
static int32_t pxn_toq(float x, int32_t q)
{
    float v = x * (float)(1u << q);
    if (v >= 2147483520.0f) return 2147483647;
    if (v <= -2147483520.0f) return -2147483647;
    return (int32_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

/* the rings, largest first, each into the piece where it fits best (the smaller leftover) */
static int pxn_rings(pxn_model *m, int32_t *r0, unsigned n0, int32_t *r1, unsigned n1)
{
    unsigned need[PXN_NL + 1];
    for (int i = 0; i < PXN_NL; i++) need[i] = (unsigned)(m->L[i].len + PXN_MIR) * PXN_C * 4u;
    need[PXN_NL] = (unsigned)(m->hlen + PXN_MIR) * PXN_C * 4u;
    uint32_t done = 0;
    for (int t = 0; t <= PXN_NL; t++) {
        int b = -1;
        for (int i = 0; i <= PXN_NL; i++)
            if (!(done >> i & 1u) && (b < 0 || need[i] > need[b])) b = i;
        done |= 1u << b;
        int32_t **pp; unsigned *pn;
        const int f0 = n0 >= need[b], f1 = n1 >= need[b];
        if (!f0 && !f1) return -2;
        if (f0 && (!f1 || n0 <= n1)) { pp = &r0; pn = &n0; } else { pp = &r1; pn = &n1; }
        if (b == PXN_NL) m->hring = *pp; else m->L[b].ring = *pp;
        *pp += need[b] / 4u;
        *pn -= need[b];
    }
    return 0;
}

int pxn_load(pxn_model *m, const float *w, int32_t *r0, unsigned n0, int32_t *r1, unsigned n1)
{
    m->ready = 0;
#if !PXN_HAND
    pxn_rdl_f[6] = pxn_rdl_f[15] = pxn_rdl_f[16] = pxn_rdl_c;   /* the PC: the one-call path, by the C routine */
#endif
    for (int i = 0; i < PXN_NW; i++)
        if (!pxn_finite(w[i])) return -1;
    const float *p = w;
    for (int c = 0; c < PXN_C; c++) m->rech[c] = pxn_toq(p[c], PXN_QW);
    p += PXN_C;
    int32_t off = 0;
    for (int i = 0; i < PXN_NL; i++) {
        pxn_layer *L = &m->L[i];
        const int K = pxn_k[i], T = 3 * K + 2;
        const float *conv = p, *cb = p + 9 * K, *mix = cb + 3, *one = mix + 3, *ob = one + 9;
        /* row o: [mixin, bias, then for each kernel tap k its 3 input channels], as the X rows. File: conv [o][c][k] */
        for (int o = 0; o < PXN_C; o++) {
            int32_t *d = m->wa + off + o * T;
            d[0] = pxn_toq(mix[o], PXN_QW);
            d[1] = pxn_toq(cb[o], PXN_QW);
            for (int k = 0; k < K; k++)
                for (int c = 0; c < PXN_C; c++) d[2 + 3 * k + c] = pxn_toq(conv[(o * PXN_C + c) * K + k], PXN_QW);
            for (int c = 0; c < PXN_C; c++) L->rr[o * 4 + c] = pxn_toq(one[o * PXN_C + c], PXN_QW);
            L->rr[o * 4 + 3] = pxn_toq(ob[o], PXN_QA);
        }
        pxn_rr2(L->rr, L->rr2);
        L->k = (int16_t)K;
        L->d = (int16_t)pxn_d[i];
        L->off = off;
        L->len = (K - 1) * pxn_d[i] + PXN_N;
        off += 3 * T;
        p = ob + 3;
    }
    /* head row: [u (0), bias, then for each of its 16 taps the 3 channels]. File: head [c][k], bias, scale */
    m->head[0] = 0;
    m->head[1] = pxn_toq(p[PXN_C * PXN_HK], PXN_QW);
    for (int k = 0; k < PXN_HK; k++)
        for (int c = 0; c < PXN_C; c++) m->head[2 + 3 * k + c] = pxn_toq(p[c * PXN_HK + k], PXN_QW);
    m->out_gain = p[PXN_C * PXN_HK + 1] * (1.0f / 1048576.0f);
    m->hlen = (PXN_HK - 1) + PXN_N;
    if (pxn_wf) {                                        /* the vector unit's copy */
        memcpy(pxn_wf, m->wa, sizeof m->wa);
        memcpy(pxn_wf + PXN_WF_HEAD, m->head, sizeof m->head);
    }
    if (pxn_rings(m, r0, n0, r1, n1)) return -2;
    pxn_reset(m);
    m->ready = 1;
    return 0;
}

void pxn_reset(pxn_model *m)
{
    for (int i = 0; i < PXN_NL; i++) {
        pxn_layer *L = &m->L[i];
        memset(L->ring, 0, (unsigned)(L->len + PXN_MIR) * PXN_C * 4u);
        L->w = 0;
    }
    memset(m->hring, 0, (unsigned)(m->hlen + PXN_MIR) * PXN_C * 4u);
    m->hw = 0;
}

/* the vector unit's copy of a weight row: in the fast memory if in use (pxn_set_fast), else the row itself */
static __attribute__((unused)) const int32_t *pxn_uw(const pxn_model *m, const int32_t *w)
{
    if (!pxn_wf) return w;
    if (w >= m->head && w < m->head + PXN_S + 4) return pxn_wf + PXN_WF_HEAD + (w - m->head);
    return pxn_wf + (w - m->wa);
}
/* ---- one block ---- */
/* how each mode runs (pxn_tune picks one): h > 0: two cores, the helper core takes frames 0 .. h-1 (it never waits:
 * earlier frames never need later ones), the calling core the rest; pipe: the vector unit beside the calling core
 * (block or range in two halves), the CPU taking c frames of each half's dot products itself */
static const struct { uint8_t h, pipe, c; } PXN_MC[PXN_MODES] = {
    {0, 0, 0}, {0, 0, 0}, {0, 1, 0}, {0, 1, 4}, {0, 1, 8}, {16, 0, 0}, {12, 1, 0}, {16, 1, 0}, {12, 1, 4},
    {20, 0, 0}, {24, 0, 0}};
/* the K taps (dilation d) of frames f0 .. f0+n-1 of the block (the block starts at ring position w) into X columns 2..;
 * kick: this core drives the vector unit */
static void pxn_gather(const int32_t *ring, int32_t len, int32_t w, int K, int d, int f0, int n, int kick)
{
    for (int k = 0; k < K; k++) {
        int32_t s = w + f0 - (K - 1 - k) * d;
        if (s < 0) s += len;
        if (s >= len) s -= len;
        int n1 = len - s;
        if (n1 > n) n1 = n;
        int32_t *x = pxn_x + f0 * PXN_S + 2 + 3 * k;
        pxn_gcopy(x, ring + 3 * s, n1, PXN_S * 4);
        if (n1 < n) pxn_gcopy(x + n1 * PXN_S, ring, n - n1, PXN_S * 4);
        if (kick) pxn_kick();
    }
}
/* rows weight rows (1, or the 3 of a layer, contiguous) over X rows f0 .. f0+n-1 -> pxn_z, by the CPU */
static void pxn_rows_cpu(const int32_t *w, int T, int rows, int f0, int n, int32_t *z)
{
    const int n4 = rows == PXN_C ? n & ~3 : 0;
    pxn_dot d = {w, pxn_x + f0 * PXN_S, T, PXN_S * 4, z + 3 * f0, n4 >> 2};
    if (n4) pxn_dot3x4(&d);
    if (n4 < n)
        for (int o = 0; o < rows; o++) {
            d.w = w + o * T; d.x = pxn_x + (f0 + n4) * PXN_S; d.z = z + 3 * (f0 + n4) + o; d.ng = n - n4;
            pxn_dot1(&d);
        }
}
/* the same, on the vector unit one command after the other if mode 1 or more */
static void pxn_rows(const pxn_model *m, const int32_t *w, int T, int rows, int n)
{
#if PXN_ASM
    if (pxn_mode_v && pxn_vec_ok) {
        for (int o = 0; o < rows; o++)
            if (pxn_vdot(pxn_uw(m, w + o * T), pxn_x, T, PXN_S * 4, pxn_z + o, n)) { pxn_vec_ok = 0; break; }
        if (pxn_vec_ok) return;
    }
#else
    (void)m;
#endif
    pxn_rows_cpu(w, T, rows, 0, n, pxn_z);
}
/* a ring's first PXN_MIR frames are mirrored after its end, so that 4 frames from any position read straight on: the
 * frames p .. p+c-1 just written, those of them below PXN_MIR copied (only those: the other core may be writing the
 * others) */
static inline void pxn_mir(int32_t *ring, int32_t len, int32_t p, int c)
{
    for (; c > 0 && p < PXN_MIR; c--, p++) {
        int32_t *a = ring + 3 * p, *b = ring + 3 * (len + p);
        b[0] = a[0]; b[1] = a[1]; b[2] = a[2];
    }
}
#if PXN_HAND
/* post4's job by post5 where it can: a layer with an output, two frames at a time (an odd first frame by post4) */
static void pxn_post45(const pxn_post *p)
{
    if (!pxn_p5on || !p->out || p->n < 2) { pxn_post4_asm(p); return; }
    if (!(p->n & 1)) { pxn_post5_asm(p); return; }
    pxn_post q = *p;
    q.n = 1;
    pxn_post4_asm(&q);
    q.z += 3; q.x += 3; q.hs += 3; q.out += 3; q.n = p->n - 1;
    pxn_post5_asm(&q);
}
#endif
/* layer i's post for frames f0 .. f0+n-1: LeakyReLU, head sum, residual into the next layer's ring (its block starts
 * at its write position, still the one before this block); in pieces of 4 frames, the vector unit kicked between */
static void pxn_post_part(pxn_model *m, int i, int f0, int n, int kick, int ring, const pxn_sc *sc)
{
    pxn_layer *L = &m->L[i];
    const int K = L->k;
    pxn_post p = {sc->z + 3 * f0, pxn_x + f0 * PXN_S + 2 + 3 * (K - 1), PXN_S * 4, L->rr, sc->hs + 3 * f0, 0, 0, L->rr2};
    pxn_layer *N = i < PXN_NL - 1 && !sc->q ? &m->L[i + 1] : 0;   /* (q: the output to the hand-over, contiguous) */
    int32_t *q = i < PXN_NL - 1 && sc->q ? sc->q + 3 * f0 : 0;
    int32_t w = N ? N->w + f0 : 0, r = L->w + f0;         /* r: the layer's own input in its ring (ring = 1) */
    if (N && w >= N->len) w -= N->len;
    if (r >= L->len) r -= L->len;
    if (ring) p.sb = 12;
    while (n > 0) {
        int c = n < pxn_pc ? n : pxn_pc;
        if (N) {
            if (c > N->len - w) c = N->len - w;
            p.out = N->ring + 3 * w;
        }
        if (ring) {
            if (c > L->len - r) c = L->len - r;
            p.x = L->ring + 3 * r;
            r += c;
            if (r == L->len) r = 0;
        }
        const int32_t wc = w;
        if (N) { w += c; if (w == N->len) w = 0; }
        if (q) { p.out = q; q += 3 * c; }
        p.n = c;
#if PXN_HAND
        if (ring && pxn_p4on) pxn_post45(&p);
        else
#endif
        pxn_post_f(&p);
        if (N && wc < PXN_MIR) pxn_mir(N->ring, N->len, wc, c);
        if (kick) pxn_kick();
        p.z += 3 * c; p.x += c * (p.sb / 4); p.hs += 3 * c;
        n -= c;
    }
}
static void pxn_layer_block(pxn_model *m, int i, int n)
{
    pxn_layer *L = &m->L[i];
    const int K = L->k, T = 3 * K + 2;
    pxn_gather(L->ring, L->len, L->w, K, L->d, 0, n, 1);
    pxn_rows(m, m->wa + L->off, T, PXN_C, n);
    pxn_post_part(m, i, 0, n, 1, 0, &pxn_sc0);
}
/* rows weight rows (3 of a layer, or the head's 1) for frames f0 .. f0+n-1 by the CPU straight from a ring (its block
 * starting at position w): groups of 4 frames by rdot, a rest of 1 .. 3 frames gathered. core: whose tables (0 the
 * calling core, 1 the helper) */
static void pxn_direct(const int32_t *ring, int32_t len, int32_t w, int K, int d, const int32_t *wr, int rows, int f0,
                       int n, int core, int kick, const pxn_sc *sc)
{
    const int32_t **tp = pxn_tpt[core];
    const int n4 = pxn_rd ? n & ~3 : 0;
    uint32_t t0 = PXN_T();
    if (n4) {
        /* each tap's first 4 frames, then 4 frames on per group: read straight on past the end (the mirrored frames),
         * the pointer brought back by the ring's length once past it (diagnostic 19: no position arithmetic per tap) */
        const int32_t *const end = ring + 3 * len;
        int32_t s = w + f0 - (K - 1) * d;                /* the first tap; each next one d frames on (d < len) */
        while (s < 0) s += len;
        while (s >= len) s -= len;
        for (int k = 0; k < K; k++) {
            tp[k] = ring + 3 * s;
            s += d;
            if (s >= len) s -= len;
        }
        void (*const q)(const pxn_rdot *) = pxn_rdq_on && pxn_rdq_f[K] ? pxn_rdq_f[K] : K == 6 ? pxn_rdot6_f : pxn_rdot_f;
        pxn_rdot j = {wr, tp, K, sc->u + f0, sc->z + 3 * f0, rows, n4 >> 2, end, -3 * len * 4};
        if (pxn_rdl_on && pxn_rdl_f[K] && pxn_pc != 4) {  /* the whole block in one call (no unit to kick) */
            if (!pxn_pskip) (pxn_rdm_on && rows == PXN_C && PXN_RDM(K) ? PXN_RDM(K) : pxn_rdl_f[K])(&j);
        } else
        for (int g = 0; g < n4; g += 4) {
            if (!pxn_pskip) q(&j);
            if (kick) pxn_kick();
            j.u += 4; j.z += 12;
#define PXN_ADV(k) do { const int32_t *p_ = tp[k] + 12; if (p_ >= end) p_ -= 3 * len; tp[k] = p_; } while (0)
            if (K == 6) { PXN_ADV(0); PXN_ADV(1); PXN_ADV(2); PXN_ADV(3); PXN_ADV(4); PXN_ADV(5); }   /* (no loop branch) */
            else for (int k = 0; k < K; k++) PXN_ADV(k);
#undef PXN_ADV
        }
    }
    uint32_t t1 = PXN_T();
    if (n4 < n) {
        pxn_gather(ring, len, w, K, d, f0 + n4, n - n4, kick);
        pxn_rows_cpu(wr, 3 * K + 2, rows, f0 + n4, n - n4, sc->z);
    }
    if (pxn_pt) { pxn_prof[pxn_pd] += t1 - t0; pxn_prof[pxn_pd + 1] += PXN_T() - t1; }
}
/* layer i for frames f0 .. f0+n-1, by the CPU alone */
static void pxn_range_cpu(pxn_model *m, int i, int f0, int n, int kick, int core, const pxn_sc *sc)
{
    pxn_layer *L = &m->L[i];
    pxn_direct(L->ring, L->len, L->w, L->k, L->d, m->wa + L->off, PXN_C, f0, n, core, kick, sc);
    uint32_t t0 = PXN_T();
    pxn_post_part(m, i, f0, n, kick, 1, sc);
    if (pxn_pt) pxn_prof[3] += PXN_T() - t0;
}

/* ---- two cores: the helper (the other core, through the caller's job functions) takes the first h frames ---- */
static int (*pxn_hrun)(void (*fn)(uint32_t), uint32_t arg);   /* 1 = the other core has the job */
static int (*pxn_hwait)(void);                                  /* 0 = done, -1 = dropped */
void pxn_set_helper(int (*run)(void (*fn)(uint32_t), uint32_t arg), int (*wait)(void)) { pxn_hrun = run; pxn_hwait = wait; }
static struct { pxn_model *m; int32_t h; volatile int32_t done; } pxn_hj;   /* done: layers the helper has posted */
static uint8_t pxn_hon, pxn_hdead;
#define PXN_SYNC() __asm__ volatile("" ::: "memory")
#if PXN_ASM
#undef PXN_SYNC
#define PXN_SYNC() __asm__ volatile("csync" ::: "memory")
#endif
static void pxn_help_job(uint32_t arg)                  /* on the helper core */
{
    (void)arg;
    for (int i = 0; i < PXN_NL; i++) {
        pxn_range_cpu(pxn_hj.m, i, 0, pxn_hj.h, 0, 1, &pxn_sc0);
        PXN_SYNC();
        pxn_hj.done = i + 1;
        PXN_SYNC();
    }
}
#ifdef PXN_EMUVEC
/* PC tests: a helper that moves on by one layer only while the calling core waits for it (as late as it can be: a
 * frame read before it is ready shows) */
static uint8_t pxn_emu_hjob;
static int pxn_emu_hrun(void (*fn)(uint32_t), uint32_t arg) { (void)fn; (void)arg; pxn_emu_hjob = 1; return 1; }
static void pxn_emu_hstep(void)
{
    if (pxn_emu_hjob && pxn_hj.done < PXN_NL) { pxn_range_cpu(pxn_hj.m, pxn_hj.done, 0, pxn_hj.h, 0, 1, &pxn_sc0); pxn_hj.done++; }
}
static int pxn_emu_hwait(void) { while (pxn_hj.done < PXN_NL) pxn_emu_hstep(); pxn_emu_hjob = 0; return 0; }
void pxn_emu_helper(void) { pxn_hrun = pxn_emu_hrun; pxn_hwait = pxn_emu_hwait; }
void pxn_emu_gather(void) { pxn_rd = 0; }      /* the CPU's layers through gathered X rows */
#endif
/* the calling core: wait until the helper has posted layer j-1 (layer j's ring holds its frames); if the helper is
 * gone, its frames are done here */
static void pxn_hneed(int j)
{
    if (!pxn_hon) return;
    for (uint32_t k = 0; pxn_hj.done < j && !pxn_hdead; ) {
        PXN_SYNC();
#ifdef PXN_EMUVEC
        pxn_emu_hstep();
#endif
        if (++k > 400000u) {                             /* very late: its job API's own wait (and time limit) */
            if (pxn_hwait() < 0) pxn_hdead = 1;          /* dropped: never runs again */
            k = 0;
        }
    }
    if (pxn_hdead)
        while (pxn_hj.done < j) { pxn_range_cpu(pxn_hj.m, pxn_hj.done, 0, pxn_hj.h, 1, 0, &pxn_sc0); pxn_hj.done++; }
}
static void pxn_hstart(pxn_model *m, int h)
{
    pxn_hj.m = m;
    pxn_hj.h = h;
    pxn_hj.done = 0;
    PXN_SYNC();
    pxn_hon = 1;
    pxn_hdead = 0;
    if (!pxn_hrun || !pxn_hrun(pxn_help_job, 0)) pxn_hdead = 1;   /* no helper: its frames here, layer by layer */
}
static void pxn_hend(void)
{
    pxn_hneed(PXN_NL);
    if (!pxn_hdead && pxn_hwait) pxn_hwait();
    pxn_hon = 0;
}

#if PXN_VQON
/* the vector unit beside the CPU, over frames fa .. fb-1 in two halves. Half B of layer i, and layer i+1's half A,
 * never need each other, so while the vector unit does one half's 3 dot products, the CPU gathers or posts the other:
 *   unit:  D(i,A)        D(i,B)              D(i+1,A)            ...
 *   CPU:   G(i,B)  P(i,A) G(i+1,A)   P(i,B) G(i+1,B)   ...
 * (G gather, D dot, P post). Each half has its own X rows and z, so nothing is overwritten while the unit reads it. */
static uint32_t pxn_issue(pxn_model *m, int i, int f0, int n, int c)   /* the 3 dot products of layer i, frames f0.. */
{
    const pxn_layer *L = &m->L[i];
    const int T = 3 * L->k + 2;
    if (!pxn_vec_ok) { pxn_rows_cpu(m->wa + L->off, T, PXN_C, f0, n, pxn_z); return 0; }
    const int nv = n - c;                                /* the CPU takes the last c frames itself, meanwhile */
    for (int o = 0; o < PXN_C && nv > 0; o++)
        pxn_venq(pxn_uw(m, m->wa + L->off + o * T), pxn_x + f0 * PXN_S, T, pxn_z + 3 * f0 + o, nv);
    const uint32_t seq = nv > 0 ? pxn_vq_in : 0;
    if (c > 0) pxn_rows_cpu(m->wa + L->off, T, PXN_C, f0 + (nv > 0 ? nv : 0), nv > 0 ? c : n, pxn_z);
    return seq;
}
static void pxn_need(pxn_model *m, uint32_t seq, int i, int f0, int n, int c)
{
    if (!seq) return;
    if (!pxn_vfail && !pxn_vupto(seq)) return;
    const pxn_layer *L = &m->L[i];                       /* the unit died: the CPU does its part (X rows intact) */
    pxn_rows_cpu(m->wa + L->off, 3 * L->k + 2, PXN_C, f0, n - c, pxn_z);
}
static void pxn_layers_pipe(pxn_model *m, int fa, int fb, int c)
{
    const int h = ((fb - fa) >> 1) & ~3, f0[2] = {fa, fa + h}, nf[2] = {h, fb - fa - h};
    uint32_t seq[2];
    pxn_layer *L = &m->L[0];
    for (int s = 0; s < 2; s++) {
        pxn_gather(L->ring, L->len, L->w, L->k, L->d, f0[s], nf[s], 1);
        seq[s] = pxn_issue(m, 0, f0[s], nf[s], c);
    }
    for (int i = 0; i < PXN_NL; i++)
        for (int s = 0; s < 2; s++) {
            pxn_need(m, seq[s], i, f0[s], nf[s], c);
            pxn_post_part(m, i, f0[s], nf[s], 1, 0, &pxn_sc0);
            if (i < PXN_NL - 1) {
                pxn_hneed(i + 1);
                L = &m->L[i + 1];
                pxn_gather(L->ring, L->len, L->w, L->k, L->d, f0[s], nf[s], 1);
                seq[s] = pxn_issue(m, i + 1, f0[s], nf[s], c);
            }
        }
}
#endif

/* v37 diagnostic 22: the helper as a server for the whole audio block, the roles the other way round. In the two-core
 * pipeline CPU1 first runs the effects after the amp (POST), which often take longer than CPU0's effects before it
 * (PRE): CPU1 comes late to the network. So for each 32-frame block CPU0 takes the first hs frames and never waits, and
 * CPU1 the last n-hs frames, waiting before layer i until CPU0 has posted layer i-1 (frame t needs only frames t and
 * earlier). The lateness the firmware measured (ticks) gives CPU0 more frames (hs up to 28) in the first blocks:
 * frames moved = lateness / 2 per-frame costs. pxn_serve runs on CPU1 (after POST) until pxn_srv_end. */
/* v40.2 diagnostic, built only with PXN_DEV (a development build): ticks the cores wait for each other in the
 * pipeline: [0] CPU0 for CPU1 at the end of a block, [1] CPU1 for CPU0 before a layer, [2] CPU0 after the layers (the
 * head, rings and output), [3] blocks counted */
#ifdef PXN_DEV
extern uint32_t (*px_clock)(void);
volatile uint32_t pxn_w[4];
#define PXN_DCLK() (px_clock ? px_clock() : 0u)
#define PXN_DEVW(k, t) (pxn_w[k] += PXN_DCLK() - (t))
#else
#define PXN_DCLK() 0u
#define PXN_DEVW(k, t) ((void)(t))
#endif
static struct {
    pxn_model *m;
    volatile int32_t seq, hs, n, mdone, hlay, hdone, stop;   /* mdone: layers CPU0 has posted; hlay: layers CPU1 has */
} pxn_sv;
static uint8_t pxn_sv_on, pxn_sv_dead;
static int32_t pxn_sv_late, pxn_sv_c;                    /* CPU1's lateness left (ticks); one frame's cost, one core */
uint32_t pxn_frame_ticks(void) { return (uint32_t)pxn_sv_c; }
#ifdef PXN_EMUVEC
static uint8_t pxn_emu_eager;                            /* PC: CPU1 runs each layer as soon as allowed, or only when waited for */
static void pxn_emu_sstep(int waited)
{
    if (pxn_sv.hdone == pxn_sv.seq || (!waited && !pxn_emu_eager)) return;
    while (pxn_sv.hlay < PXN_NL && pxn_sv.hlay <= pxn_sv.mdone) {
        pxn_range_cpu(pxn_sv.m, pxn_sv.hlay, pxn_sv.hs, pxn_sv.n - pxn_sv.hs, 0, 1, &pxn_sc0);
        pxn_sv.hlay++;
    }
    if (pxn_sv.hlay == PXN_NL) pxn_sv.hdone = pxn_sv.seq;
}
void pxn_emu_srv(int eager) { pxn_emu_eager = (uint8_t)eager; pxn_sv_c = 1; }   /* (lateness then in frames x 2) */
#define PXN_SSTEP(w) pxn_emu_sstep(w)
#else
#define PXN_SSTEP(w) ((void)0)
#endif
void pxn_srv_begin(int32_t late)                         /* CPU0, before starting CPU1's job */
{
    pxn_sv.seq = pxn_sv.hdone = pxn_sv.stop = pxn_sv.hlay = 0;
    pxn_sv.mdone = 0;
    pxn_sv_late = late;
    PXN_SYNC();
    pxn_sv_on = (uint8_t)(!pxn_sv_dead && PXN_MC[pxn_mode_v].h && !PXN_MC[pxn_mode_v].pipe);
}
void pxn_srv_end(void)                                   /* CPU0: CPU1 leaves pxn_serve (then wait for its job) */
{
    PXN_SYNC();
    pxn_sv.stop = 1;
    PXN_SYNC();
    pxn_sv_on = 0;
}
int pxn_srv_active(void) { return pxn_sv_on; }
void pxn_serve(void)                                     /* CPU1 */
{
    int32_t done = 0;
    for (;;) {
        while (pxn_sv.seq == done && !pxn_sv.stop) PXN_SYNC();
        if (pxn_sv.seq == done) return;
        const int32_t q = pxn_sv.seq;
        PXN_SYNC();
        for (int i = 0; i < PXN_NL; i++) {
#ifdef PXN_DEV
            if (pxn_sv.mdone < i) { const uint32_t t0 = PXN_DCLK(); while (pxn_sv.mdone < i) PXN_SYNC(); PXN_DEVW(1, t0); }
#endif
            while (pxn_sv.mdone < i) PXN_SYNC();
            pxn_range_cpu(pxn_sv.m, i, pxn_sv.hs, pxn_sv.n - pxn_sv.hs, 0, 1, &pxn_sc0);
            PXN_SYNC();
            pxn_sv.hlay = i + 1;
        }
        PXN_SYNC();
        pxn_sv.hdone = done = q;
    }
}
static void pxn_srv_block(pxn_model *m, int n)           /* CPU0: the layers of one block with CPU1 serving */
{
    int x = 0;
    if (pxn_sv_late > 0 && pxn_sv_c > 0) {
        x = (int)(pxn_sv_late / (2 * pxn_sv_c) + 2) & ~3;
        if (x > 12) x = 12;
        pxn_sv_late -= x * 2 * pxn_sv_c;
    }
    const int hs = 16 + x;
    pxn_sv.m = m; pxn_sv.hs = hs; pxn_sv.n = n;
    pxn_sv.mdone = 0; pxn_sv.hlay = 0;
    PXN_SYNC();
    const int32_t q = pxn_sv.seq + 1;
    pxn_sv.seq = q;
    PXN_SYNC();
    PXN_SSTEP(0);
    for (int i = 0; i < PXN_NL; i++) {
        pxn_range_cpu(m, i, 0, hs, 1, 0, &pxn_sc0);
        PXN_SYNC();
        pxn_sv.mdone = i + 1;
        PXN_SSTEP(0);
    }
    const uint32_t tw0 = PXN_DCLK();
    for (uint32_t k = 0; pxn_sv.hdone != q; ) {
        PXN_SYNC();
        PXN_SSTEP(1);
        if (++k > 8000000u) {                            /* (CPU1 gone for ~0.1 s: its layers here, from where it was) */
            pxn_sv_dead = 1;
            for (int i = pxn_sv.hlay; i < PXN_NL; i++) pxn_range_cpu(m, i, hs, n - hs, 1, 0, &pxn_sc0);
            pxn_sv.hdone = q;
            pxn_sv_on = 0;
        }
    }
    PXN_DEVW(0, tw0);
#ifdef PXN_DEV
    pxn_w[3]++;
#endif
}
static void pxn_block(pxn_model *m, const float *in, float *out, int n)
{
    pxn_layer *L0 = &m->L[0];
    int32_t w0 = L0->w;
    const uint32_t tb = PXN_T();
    for (int f = 0; f < n; f++) {                        /* Q20, clamped to +-64 (far above any guitar) */
        float v = in[f] * 1048576.0f;
        const int32_t u = v >= 67108864.0f ? 67108864 : v <= -67108864.0f ? -67108864 : (int32_t)v;
        int32_t *x = pxn_xs + f * PXN_S, *r = L0->ring + 3 * w0;
        x[0] = u;
        x[1] = PXN_ONE;
        pxn_u[f] = u;
        for (int c = 0; c < PXN_C; c++) r[c] = (int32_t)(((int64_t)m->rech[c] * u) >> PXN_QW);
        if (w0 < PXN_MIR) pxn_mir(L0->ring, L0->len, w0, 1);
        if (++w0 == L0->len) w0 = 0;
    }
    memset(pxn_hs, 0, (unsigned)n * PXN_C * 4u);
    const int h = PXN_MC[pxn_mode_v].h, pipe = PXN_MC[pxn_mode_v].pipe && pxn_vec_ok, c = PXN_MC[pxn_mode_v].c;
    pxn_pc = (uint8_t)(pipe || (pxn_mode_v == 1 && pxn_vec_ok) ? 4 : PXN_N);   /* post in 4-frame steps only to kick the unit */
    {   /* the X rows: in the fast memory when the vector unit reads them (input columns copied over) */
        int32_t *xb = (pipe || pxn_mode_v == 1) && pxn_vec_ok && pxn_xf ? pxn_xf : pxn_xs;
        if (xb != pxn_xs) for (int f = 0; f < n; f++) { xb[f * PXN_S] = pxn_xs[f * PXN_S]; xb[f * PXN_S + 1] = PXN_ONE; }
        pxn_x = xb;
    }
    if (pxn_pt) pxn_prof[0] += PXN_T() - tb;
    const int srv = h && n == PXN_N && pxn_sv_on && !pipe;
    uint32_t tsv = 0;
    if (srv) { pxn_srv_block(m, n); tsv = PXN_DCLK(); }   /* two cores, CPU1 serving (the pipeline) */
    else if (h && n == PXN_N) {                          /* two cores */
        pxn_hstart(m, h);
#if PXN_VQON
        if (pipe) pxn_layers_pipe(m, h, n, c);
        else
#endif
        for (int i = 0; i < PXN_NL; i++) { pxn_hneed(i); pxn_range_cpu(m, i, h, n - h, 1, 0, &pxn_sc0); }
        pxn_hend();
    }
#if PXN_VQON
    else if (pipe && n >= 16) pxn_layers_pipe(m, 0, n, c);
#endif
    else if (pxn_mode_v == 1 && pxn_vec_ok) for (int i = 0; i < PXN_NL; i++) pxn_layer_block(m, i, n);
    else for (int i = 0; i < PXN_NL; i++) pxn_range_cpu(m, i, 0, n, 1, 0, &pxn_sc0);
    (void)pipe; (void)c;
    const uint32_t th = PXN_T();
    for (int i = 0; i < PXN_NL; i++) {                   /* every ring moves on by the block */
        pxn_layer *L = &m->L[i];
        L->w += n;
        if (L->w >= L->len) L->w -= L->len;
    }
    /* head: the activation sums into its ring, 16 taps gathered, one row */
    int n1 = m->hlen - m->hw;
    if (n1 > n) n1 = n;
    memcpy(m->hring + 3 * m->hw, pxn_hs, (unsigned)n1 * PXN_C * 4u);
    if (n1 < n) memcpy(m->hring, pxn_hs + 3 * n1, (unsigned)(n - n1) * PXN_C * 4u);
    if (n1 < n || m->hw < PXN_MIR) pxn_mir(m->hring, m->hlen, 0, PXN_MIR);   /* (this core alone writes it) */
    int32_t hw = m->hw;
    m->hw += n;
    if (m->hw >= m->hlen) m->hw -= m->hlen;
    if ((pxn_mode_v == 1 || PXN_MC[pxn_mode_v].pipe) && pxn_vec_ok) {
        pxn_gather(m->hring, m->hlen, hw, PXN_HK, 1, 0, n, 1);
        pxn_rows(m, m->head, PXN_S, 1, n);
    } else {
        pxn_pd = 6;
        pxn_direct(m->hring, m->hlen, hw, PXN_HK, 1, m->head, 1, 0, n, 0, 1, &pxn_sc0);
        pxn_pd = 1;
    }
    const float g = m->out_gain;
    for (int f = 0; f < n; f++) out[f] = (float)pxn_z[3 * f] * g;
    pxn_x = pxn_xs;
    if (srv) PXN_DEVW(2, tsv);
    if (pxn_pt) {
        const uint32_t te = PXN_T();
        pxn_prof[5] += te - tb;
        pxn_prof[4] += te - th;                          /* (the head's groups and rest taken out by the caller) */
    }
}
int pxn_dual(void) { return PXN_MC[pxn_mode_v].h != 0; }

/* ---- v40.2: two cores by layers (the audio pipeline's network) ----
 * The frame split above has both cores run every layer on half a block, the second core waiting for the first at every
 * layer, and the head on one core at the end. Here CPU0 runs the input and layers 0..s of each 32-frame block
 * (pxn_lp_a) and hands the block over; CPU1 runs layers s+1..22, the head and the output (pxn_lp_b), up to PXN_LPQ
 * blocks behind, across audio blocks (the pipeline gives the network's output to the blocks after it one audio block
 * later anyway). Every layer of a block runs once, on the whole block, after the same layer of the block before
 * (pxn_lpm.ld), and in the same order of operations: the output is the one-core output, bit for bit. s may change from
 * one block to the next. Layer s writes its output to the block's hand-over (q) instead of layer s+1's ring; CPU1 copies
 * it there before running layer s+1: each ring belongs to one core at a time. The memory (PXN_LP_BYTES) comes from the
 * firmware (pxn_set_lp); without it the frame split runs. */
typedef struct { float *out; int32_t s; int32_t u[PXN_N + 4]; int32_t hs[PXN_N * PXN_C]; int32_t q[PXN_N * PXN_C]; } pxn_lpe;
typedef struct { pxn_lpe e[PXN_LPQ]; int32_t z1[PXN_N * PXN_C + 4]; volatile uint32_t ld[PXN_NL]; } pxn_lpm;   /* ld: per layer, blocks done */
typedef char pxn_lp_size_check[sizeof(pxn_lpm) <= PXN_LP_BYTES ? 1 : -1];
static pxn_lpm *pxn_lp;
static volatile uint32_t pxn_lp_in, pxn_lp_out;        /* blocks handed over, finished */
static volatile int32_t pxn_lp_s = PXN_NL / 2;
static volatile int32_t pxn_lp_se = PXN_NL / 2;        /* v40.4: the s of the last block handed over */
static volatile uint8_t pxn_lp_hard;                   /* its next rise may wait for the other core (pxn_lp_force) */
/* v40.5: CPU0's waits for the other core end after a limit (pxn_lp_clock; none set: they wait as long as it takes, as on
 * the PC). Past it the other core is taken as stuck: pxn_lp_a returns without handing the block over (its output is not
 * written) and pxn_lp_gave_up says so until pxn_lp_reset; the caller then retires that core and resets the engine, as for
 * a core that faults. Until v40 every wait for CPU1 ended after FM1_CPU1_TIMEOUT_US (fm1_cpu1_wait, LATE): a CPU1 that
 * stopped without a fault cost one core, not a stuck audio interrupt and a watchdog reset. */
static uint32_t (*pxn_lp_tk)(void);
static uint32_t pxn_lp_lim;
static volatile uint8_t pxn_lp_dead;
void pxn_lp_clock(uint32_t (*ticks)(void), uint32_t limit) { pxn_lp_tk = ticks; pxn_lp_lim = limit; }
int pxn_lp_gave_up(void) { return pxn_lp_dead; }
static inline uint32_t pxn_lp_t0(void) { return pxn_lp_tk ? pxn_lp_tk() : 0u; }
static inline int pxn_lp_late(uint32_t t0)               /* 1 = past the limit: the other core is stuck */
{
    if (!pxn_lp_tk || pxn_lp_tk() - t0 < pxn_lp_lim) return 0;
    pxn_lp_dead = 1;
    return 1;
}
void pxn_set_lp(void *base, unsigned bytes) { pxn_lp = bytes >= sizeof(pxn_lpm) ? (pxn_lpm *)base : 0; }
/* the layer split can run: its memory, the network read straight from the rings (no X rows, which the cores would
 * share), two cores without the vector unit */
int pxn_lp_ok(void) { return pxn_lp && pxn_rd && PXN_MC[pxn_mode_v].h && !PXN_MC[pxn_mode_v].pipe; }
void pxn_lp_reset(void)                                  /* (no block in progress: the network was reset or loaded) */
{
    pxn_lp_in = pxn_lp_out = 0;
    pxn_lp_hard = 0;
    pxn_lp_dead = 0;
    pxn_lp_se = pxn_lp_s;
    if (pxn_lp) for (int i = 0; i < PXN_NL; i++) pxn_lp->ld[i] = 0;
    PXN_SYNC();
}
void pxn_lp_split(int s) { pxn_lp_s = s < 0 ? 0 : s > PXN_NL - 1 ? PXN_NL - 1 : s; }
int pxn_lp_split_now(void) { return pxn_lp_s; }
int pxn_lp_split_used(void) { return pxn_lp_se; }
void pxn_lp_force(void) { pxn_lp_hard = 1; }
uint32_t pxn_lp_queued(void) { return pxn_lp_in - pxn_lp_out; }
/* v40.4: the blocks handed over so far; 1 while one handed over before `upto` is not finished (the caller's own count) */
uint32_t pxn_lp_handed(void) { return pxn_lp_in; }
int pxn_lp_before(uint32_t upto) { return (int32_t)(upto - pxn_lp_out) > 0; }
/* the network's work in layers 0..s, in 1/1024 of the whole (the 15-tap layers weigh 1.9 six-tap ones, the head 0.7;
 * measured: tools/dsp.mjs prints the parts), for sharing it between the cores */
uint32_t pxn_lp_share(int s)
{
    uint32_t a = 0, t = 11;                              /* (the head, in 1/16 of a six-tap layer) */
    for (int i = 0; i < PXN_NL; i++) {
        const uint32_t w = pxn_k[i] > 6 ? 30u : 16u;
        t += w;
        if (i <= s) a += w;
    }
    return a * 1024u / t;
}
#ifdef PXN_EMUVEC
static pxn_model *pxn_lp_em;                             /* PC: the other core's work, done while this one waits */
#define PXN_LSTEP() (pxn_lp_em ? (void)pxn_lp_b(pxn_lp_em) : (void)0)
#else
#define PXN_LSTEP() ((void)0)
#endif
static inline void pxn_lp_wait(uint32_t b, int lo, int hi)   /* layers lo..hi of block b-1 done */
{
    for (int j = lo; j <= hi; j++)
        while ((int32_t)(pxn_lp->ld[j] - b) < 0) { PXN_SYNC(); PXN_LSTEP(); }
}
static inline int pxn_lp_wait_a(uint32_t b, int lo, int hi)   /* CPU0's pxn_lp_wait: 1 = it gave up (pxn_lp_late) */
{
    const uint32_t t0 = pxn_lp_t0();
    for (int j = lo; j <= hi; j++)
        while ((int32_t)(pxn_lp->ld[j] - b) < 0) {
            PXN_SYNC(); PXN_LSTEP();
            if (pxn_lp_late(t0)) return 1;
        }
    return 0;
}
static inline void pxn_lp_next(pxn_layer *L)
{
    L->w += PXN_N;
    if (L->w >= L->len) L->w -= L->len;
}
/* CPU0: one block of PXN_N frames, its output to be written to out by pxn_lp_b */
void pxn_lp_a(pxn_model *m, const float *in, float *out)
{
    const uint32_t b = pxn_lp_in;
#ifdef PXN_EMUVEC
    pxn_lp_em = m;
#endif
    if (pxn_lp_dead) return;                             /* (the other core gave up: nothing until pxn_lp_reset) */
    {
        const uint32_t t0 = pxn_lp_t0();
        while (b - pxn_lp_out >= PXN_LPQ) {              /* (the queue full: CPU1 far behind) */
            PXN_SYNC(); PXN_LSTEP();
            if (pxn_lp_late(t0)) return;
        }
    }
    pxn_lpe *e = &pxn_lp->e[b % PXN_LPQ];
    /* v40.4: layers 0..s here: as far as asked (pxn_lp_s), and no further than the other core has finished the block
     * before's (it ran those the block before's s left it). Where s moves up, this core does not wait for it: it
     * takes the layers once they are free (with the other core up to date at the start of an audio block: at once).
     * Unless told to (pxn_lp_force: the other core is the one behind, and stays so until this core takes them): then
     * it waits, once, as before */
    int s = 0;
    PXN_SYNC();
    while (s < pxn_lp_s && (int32_t)(pxn_lp->ld[s + 1] - b) >= 0) s++;
    if (s < pxn_lp_s && pxn_lp_hard) {
        s = pxn_lp_s;
        if (pxn_lp_wait_a(b, 0, s)) return;              /* (nothing written yet: the block is not handed over) */
    }
    pxn_lp_hard = 0;
    pxn_lp_se = s;
    e->out = out;
    e->s = s;
    pxn_layer *L0 = &m->L[0];
    int32_t w0 = L0->w;
    for (int f = 0; f < PXN_N; f++) {                    /* as pxn_block */
        float v = in[f] * 1048576.0f;
        const int32_t u = v >= 67108864.0f ? 67108864 : v <= -67108864.0f ? -67108864 : (int32_t)v;
        int32_t *r = L0->ring + 3 * w0;
        e->u[f] = u;
        for (int c = 0; c < PXN_C; c++) r[c] = (int32_t)(((int64_t)m->rech[c] * u) >> PXN_QW);
        if (w0 < PXN_MIR) pxn_mir(L0->ring, L0->len, w0, 1);
        if (++w0 == L0->len) w0 = 0;
    }
    memset(e->hs, 0, sizeof e->hs);
    pxn_sc sc = {pxn_z, e->u, e->hs, 0};
    for (int i = 0; i <= s; i++) {
        if (i == s) sc.q = e->q;
        pxn_range_cpu(m, i, 0, PXN_N, 0, 0, &sc);
        pxn_lp_next(&m->L[i]);
        PXN_SYNC();
        pxn_lp->ld[i] = b + 1;
    }
    PXN_SYNC();
    pxn_lp_in = b + 1;
    PXN_SYNC();
}
/* CPU1 (or CPU0 when it is alone): the oldest block handed over, finished; 0 = none waiting */
int pxn_lp_b(pxn_model *m)
{
    const uint32_t b = pxn_lp_out;
    if (b == pxn_lp_in) return 0;
    PXN_SYNC();
    pxn_lpe *e = &pxn_lp->e[b % PXN_LPQ];
    const int s = e->s;
    if (s < PXN_NL - 1) {
        pxn_lp_wait(b, s + 1, PXN_NL - 1);               /* (s moved down: CPU0 had them for the block before) */
        pxn_layer *L = &m->L[s + 1];                     /* the hand-over into layer s+1's ring, as pxn_post_part */
        int32_t w = L->w;
        for (int f = 0; f < PXN_N; f++) {
            int32_t *r = L->ring + 3 * w;
            r[0] = e->q[3 * f]; r[1] = e->q[3 * f + 1]; r[2] = e->q[3 * f + 2];
            if (w < PXN_MIR) pxn_mir(L->ring, L->len, w, 1);
            if (++w == L->len) w = 0;
        }
    }
    const pxn_sc sc = {pxn_lp->z1, e->u, e->hs, 0};
    for (int i = s + 1; i < PXN_NL; i++) {
        pxn_range_cpu(m, i, 0, PXN_N, 0, 1, &sc);
        pxn_lp_next(&m->L[i]);
        PXN_SYNC();
        pxn_lp->ld[i] = b + 1;
    }
    int n1 = m->hlen - m->hw;                            /* the head, as pxn_block */
    if (n1 > PXN_N) n1 = PXN_N;
    memcpy(m->hring + 3 * m->hw, e->hs, (unsigned)n1 * PXN_C * 4u);
    if (n1 < PXN_N) memcpy(m->hring, e->hs + 3 * n1, (unsigned)(PXN_N - n1) * PXN_C * 4u);
    if (n1 < PXN_N || m->hw < PXN_MIR) pxn_mir(m->hring, m->hlen, 0, PXN_MIR);
    const int32_t hw = m->hw;
    m->hw += PXN_N;
    if (m->hw >= m->hlen) m->hw -= m->hlen;
    pxn_direct(m->hring, m->hlen, hw, PXN_HK, 1, m->head, 1, 0, PXN_N, 1, 0, &sc);
    const float g = m->out_gain;
    for (int f = 0; f < PXN_N; f++) e->out[f] = (float)sc.z[3 * f] * g;
    PXN_SYNC();
    pxn_lp_out = b + 1;
    PXN_SYNC();
    return 1;
}

/* times the modes on the loaded model (8 blocks of 32 frames of a test signal each), keeps the fastest: the modes
 * with the vector unit only if it is there, the two-core ones only with a helper */
int pxn_tune(pxn_model *m, uint32_t (*ticks)(void), uint32_t t[PXN_MODES])
{
    static float in[PXN_N], out[PXN_N];
    int best = 0;
    for (int k = 0; k < PXN_MODES; k++) t[k] = 0;
    if (!m->ready) return pxn_mode_v;
    for (int rd = 4; rd >= 0; rd--) {                    /* the CPU's way: rdm, rdl, rdq, rdot or gathered X rows (mode 0) */
        if (rd == 4 && !pxn_rdm_f[0]) { pxn_rdt[4] = 0; continue; }
        if (rd == 3 && !pxn_rdl_f[6]) { pxn_rdt[3] = 0; continue; }
        pxn_rd = (uint8_t)(rd > 0);
        pxn_rdq_on = (uint8_t)(rd >= 2);
        pxn_rdl_on = (uint8_t)(rd >= 3);
        pxn_rdm_on = (uint8_t)(rd == 4);
        pxn_mode_v = 0;
        pxn_reset(m);
        uint32_t r = 4321u, dt = 0;
        for (int b = 0; b < 10; b++) {
            for (int f = 0; f < PXN_N; f++) { r = r * 1103515245u + 12345u; in[f] = (float)((int32_t)r >> 8) * (0.5f / 8388608.0f); }
            uint32_t t0 = ticks();
            pxn_block(m, in, out, PXN_N);
            if (b >= 2) dt += ticks() - t0;
        }
        pxn_rdt[rd] = dt;
    }
    {
        int b = 0;
        for (int rd = 1; rd < 5; rd++) if (pxn_rdt[rd] && pxn_rdt[rd] < pxn_rdt[b]) b = rd;
        pxn_rd = (uint8_t)(b > 0);
        pxn_rdq_on = (uint8_t)(b >= 2);
        pxn_rdl_on = (uint8_t)(b >= 3);
        pxn_rdm_on = (uint8_t)(b == 4);
        pxn_sv_c = (int32_t)(pxn_rdt[b] / (8u * PXN_N));  /* one frame through every layer, one core (ticks) */
    }
#if PXN_HAND
    pxn_p4on = pxn_p5on = 0;
    pxn_ptt[2] = pxn_ptt[3] = 0;
    if (pxn_p3ok) for (int v = 0; v < 4; v++) {          /* post2, post3, post4 (from the ring), post5: the fastest */
        if ((v == 2 && !pxn_p4ok) || (v == 3 && !pxn_p5ok)) break;
        pxn_post_f = v == 1 ? pxn_post3_asm : pxn_post2_asm;
        pxn_p4on = (uint8_t)(v >= 2);
        pxn_p5on = (uint8_t)(v == 3);
        pxn_mode_v = 0;
        pxn_reset(m);
        uint32_t r = 4321u, dt = 0;
        for (int b = 0; b < 10; b++) {
            for (int f = 0; f < PXN_N; f++) { r = r * 1103515245u + 12345u; in[f] = (float)((int32_t)r >> 8) * (0.5f / 8388608.0f); }
            uint32_t t0 = ticks();
            pxn_block(m, in, out, PXN_N);
            if (b >= 2) dt += ticks() - t0;
        }
        pxn_ptt[v] = dt;
    }
    if (pxn_p3ok) pxn_post_f = pxn_ptt[1] < pxn_ptt[0] ? pxn_post3_asm : pxn_post2_asm;
    pxn_p4on = (uint8_t)(pxn_p3ok && pxn_p4ok && pxn_ptt[2] && pxn_ptt[2] < pxn_ptt[0] && pxn_ptt[2] < pxn_ptt[1]);
    pxn_p5on = (uint8_t)(pxn_p4on && pxn_p5ok && pxn_ptt[3] && pxn_ptt[3] < pxn_ptt[2]);
#endif
    for (int pass = 0; pass < 2; pass++) {               /* the profile (mode 0, the chosen way), then without kernel */
        uint32_t keep[9];
        memcpy(keep, pxn_prof, sizeof keep);
        pxn_mode_v = 0;
        pxn_reset(m);
        pxn_pskip = (uint8_t)pass;
        uint32_t r = 4321u;
        for (int b = 0; b < 10; b++) {
            for (int f = 0; f < PXN_N; f++) { r = r * 1103515245u + 12345u; in[f] = (float)((int32_t)r >> 8) * (0.5f / 8388608.0f); }
            if (b == 2 && !pass) for (int k = 0; k < 9; k++) pxn_prof[k] = 0;
            if (b == 2 && pass) pxn_prof[1] = 0;
            pxn_pt = b >= 2 ? ticks : 0;
            pxn_block(m, in, out, PXN_N);
        }
        pxn_pt = 0;
        pxn_pskip = 0;
        if (pass) { const uint32_t t1 = pxn_prof[1]; memcpy(pxn_prof, keep, sizeof keep); pxn_prof[8] = t1; }
        else pxn_prof[4] -= pxn_prof[6] + pxn_prof[7];
    }
    for (int mode = 0; mode < PXN_MODES; mode++) {
        const int unit = mode == 1 || PXN_MC[mode].pipe;
        if ((unit && !pxn_vec_ok) || (PXN_MC[mode].h && !pxn_hrun)) continue;
        pxn_mode_v = mode;
        pxn_reset(m);
        uint32_t r = 4321u, dt = 0;
        for (int b = 0; b < 10; b++) {
            for (int f = 0; f < PXN_N; f++) { r = r * 1103515245u + 12345u; in[f] = (float)((int32_t)r >> 8) * (0.5f / 8388608.0f); }
            uint32_t t0 = ticks();
            pxn_block(m, in, out, PXN_N);
            if (b >= 2) dt += ticks() - t0;
        }
        if ((unit && !pxn_vec_ok) || (PXN_MC[mode].h && pxn_hdead)) continue;   /* failed on the way */
        t[mode] = dt;
        if (dt < t[best]) best = mode;
    }
    if (t[0]) pxn_sv_c = (int32_t)(t[0] / (8u * PXN_N));  /* v40.4: one frame, one core, with the routines chosen above */
    pxn_mode_v = best;
    pxn_reset(m);
    return best;
}

/* v40.2: the layer split against one core, the other core really serving (pxn_set_helper): 64 blocks of a test signal,
 * handed over 4 at a time (as the audio pipeline does) with the split moved between them, each block's output hashed
 * against the one-core run. Returns the blocks that differ (0: the same), -1 if it cannot run. The model is reset. */
static volatile uint8_t pxn_lpe_stop;
static void pxn_lpe_job(uint32_t arg)                    /* (the other core) */
{
    pxn_model *m = (pxn_model *)(uintptr_t)arg;
    for (;;) {
        if (pxn_lp_b(m)) continue;
        if (pxn_lpe_stop) break;
        PXN_SYNC();
    }
}
int pxn_lp_equiv(pxn_model *m)
{
    float in[4][PXN_N], out[4][PXN_N];                  /* (on the stack: it runs at boot, or on the PC) */
    uint32_t hb[64];
    if (!m->ready || !pxn_lp_ok() || !pxn_hrun) return -1;
    const int mv = pxn_mode_v, s0 = pxn_lp_s;
    int diff = 0;
    for (int pass = 0; pass < 2; pass++) {
        pxn_reset(m);
        pxn_lp_reset();
        pxn_mode_v = pass ? mv : 0;
        int job = 0;
        if (pass) {
            pxn_lpe_stop = 0;
            PXN_SYNC();
            job = pxn_hrun(pxn_lpe_job, (uint32_t)(uintptr_t)m);
        }
        uint32_t r = 2025u;
        for (int c = 0; c < 16; c++) {
            for (int j = 0; j < 4; j++)
                for (int f = 0; f < PXN_N; f++) {
                    r = r * 1103515245u + 12345u;
                    in[j][f] = (float)((int32_t)r >> 8) * (0.5f / 8388608.0f) * (float)((c * 4 + j) % 8 + 1) * 0.125f;
                }
            if (pass) {
                pxn_lp_split((int)(r >> 16) % PXN_NL);
                for (int j = 0; j < 4; j++) pxn_lp_a(m, in[j], out[j]);
                while (pxn_lp_out != pxn_lp_in) {        /* (the other core's: or here, without it) */
                    PXN_SYNC();
                    if (!job) pxn_lp_b(m); else PXN_LSTEP();
                }
            } else for (int j = 0; j < 4; j++) pxn_block(m, in[j], out[j], PXN_N);
            for (int j = 0; j < 4; j++) {
                uint32_t h = 2166136261u;
                const uint8_t *q = (const uint8_t *)out[j];
                for (unsigned i = 0; i < sizeof out[j]; i++) h = (h ^ q[i]) * 16777619u;
                if (!pass) hb[c * 4 + j] = h;
                else if (hb[c * 4 + j] != h) diff++;
            }
        }
        if (job) {
            PXN_SYNC();
            pxn_lpe_stop = 1;
            PXN_SYNC();
            if (pxn_hwait() < 0) diff = -1;
        }
    }
    pxn_mode_v = mv;
    pxn_lp_split(s0);
    pxn_reset(m);
    pxn_lp_reset();
    return diff;
}

/* v40.2: the hand routines against C on the whole network: 64 blocks of a test signal through the model as it runs in
 * `mode` (the routines the start-up timing chose), then again with every routine in C (mode 0), each block's output
 * hashed bit for bit. Returns the blocks whose hashes differ (0: the same output), or -1 if the model is not loaded or
 * the mode is not available. The model is reset after; the routines and mode in use are put back. */
int pxn_equiv(pxn_model *m, int mode)
{
    float in[PXN_N], out[PXN_N];                         /* (on the stack: it runs at boot, or on the PC) */
    uint32_t hb[64];
    if (!m->ready || mode < 0 || mode >= PXN_MODES || (PXN_MC[mode].h && !pxn_hrun) || PXN_MC[mode].pipe || mode == 1)
        return -1;
    const int mv = pxn_mode_v;
    const uint8_t rd = pxn_rd, rq = pxn_rdq_on, rl = pxn_rdl_on, rm = pxn_rdm_on, p4 = pxn_p4on, p5 = pxn_p5on;
    void (*const d3)(const pxn_dot *) = pxn_dot3x4, (*const d1)(const pxn_dot *) = pxn_dot1, (*const po)(const pxn_post *) = pxn_post_f;
    void (*const gc)(int32_t *, const int32_t *, int, int) = pxn_gcopy;
    void (*const r1)(const pxn_rdot *) = pxn_rdot_f, (*const r6)(const pxn_rdot *) = pxn_rdot6_f;
    void (*l6)(pxn_rdot *) = pxn_rdl_f[6], (*l15)(pxn_rdot *) = pxn_rdl_f[15], (*l16)(pxn_rdot *) = pxn_rdl_f[16];
    int diff = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass) {                                      /* every routine in C, one core */
            pxn_mode_v = 0; pxn_rd = 1; pxn_rdq_on = 0; pxn_rdl_on = 1; pxn_rdm_on = 0; pxn_p4on = pxn_p5on = 0;
            pxn_dot3x4 = pxn_dot3x4_c; pxn_dot1 = pxn_dot1_c; pxn_post_f = pxn_post_c; pxn_gcopy = pxn_gcopy_c;
            pxn_rdot_f = pxn_rdot6_f = pxn_rdot_c;
            pxn_rdl_f[6] = pxn_rdl_f[15] = pxn_rdl_f[16] = pxn_rdl_c;
        } else pxn_mode_v = mode;
        pxn_reset(m);
        uint32_t r = 2024u;
        for (int b = 0; b < 64; b++) {
            for (int f = 0; f < PXN_N; f++) {
                r = r * 1103515245u + 12345u;
                in[f] = (float)((int32_t)r >> 8) * (0.5f / 8388608.0f) * (float)(b % 8 + 1) * 0.125f;
            }
            pxn_block(m, in, out, PXN_N);
            uint32_t h = 2166136261u;
            const uint8_t *q = (const uint8_t *)out;
            for (unsigned i = 0; i < sizeof out; i++) h = (h ^ q[i]) * 16777619u;
            if (!pass) hb[b] = h;
            else if (hb[b] != h) diff++;
        }
    }
    pxn_mode_v = mv; pxn_rd = rd; pxn_rdq_on = rq; pxn_rdl_on = rl; pxn_rdm_on = rm; pxn_p4on = p4; pxn_p5on = p5;
    pxn_dot3x4 = d3; pxn_dot1 = d1; pxn_post_f = po; pxn_gcopy = gc; pxn_rdot_f = r1; pxn_rdot6_f = r6;
    pxn_rdl_f[6] = l6; pxn_rdl_f[15] = l15; pxn_rdl_f[16] = l16;
    pxn_reset(m);
    return diff;
}

void pxn_process(pxn_model *m, const float *in, float *out, int n)
{
    if (!m->ready) {
        if (out != in) memcpy(out, in, (unsigned)n * sizeof(float));
        return;
    }
    while (n > 0) {
        const int c = n < PXN_N ? n : PXN_N;
        pxn_block(m, in, out, c);
        in += c;
        out += c;
        n -= c;
    }
}
