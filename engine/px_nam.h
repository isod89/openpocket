/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — Neural Amp Modeler A2-Lite on the pedal, in fixed point.
 *
 * The network (NAM "A2", 2026, the 3-channel "Lite" size; tools/nam_a2.py is the float reference):
 *   x = rechannel * u                                     1 -> 3
 *   23 layers, kernel 6 (15 for layers 14 and 15), dilations 1 3 7 17 41 101 239 | x2 | 1 13 | 1 .. 239:
 *     z = conv(x) + bias + mixin * u ; a = LeakyReLU(z, 0.01) ; head += a ; x += layer1x1(a) + bias
 *   y = head_scale * (conv16(head) + bias)                3 -> 1
 * 1871 weights, read in the .nam file's order (pxn_load). About 1,730 multiply-adds per sample.
 *
 * Fixed point: activations are int32 with 20 fraction bits (range +-2048, step 1e-6), weights int32 with 24 (+-128),
 * products summed in int64 then shifted back by a constant (one instruction on the pedal).
 * Samples go through in blocks of up to PXN_N frames (px_nam.c: gather, dot, post). Each layer keeps its input history
 * in a ring of (kernel - 1) x dilation + PXN_N frames, the 3 channels side by side. The rings take 83.4 KB, the
 * weights 6.2 KB, the block buffers 7 KB.
 */
#pragma once
#include <stdint.h>

#define PXN_C      3
#define PXN_NL     23
#define PXN_HK     16
#define PXN_NW     1871                 /* A2-Lite weights, head_scale included */
#define PXN_N      32                   /* frames per block */
#define PXN_S      50                   /* a block row: u, 1.0, then up to 16 taps x 3 channels */
#define PXN_QA     20                   /* activations: fraction bits */
#define PXN_QW     24                   /* weights: fraction bits */
#define PXN_LOOK   6331                 /* sum of (kernel - 1) x dilation */
#define PXN_MIR    3                    /* each ring's first frames mirrored after its end (4-frame reads) */
#define PXN_RING   (PXN_LOOK + (PXN_HK - 1) + (PXN_NL + 1) * (PXN_N + PXN_MIR))   /* frames in all rings (head included) */


typedef struct {
    int32_t *ring;                      /* len frames x PXN_C channels, interleaved */
    int32_t len, w;                     /* ring length (lookback + PXN_N), where the next block is written */
    int16_t k, d;                       /* kernel, dilation */
    int32_t off;                        /* this layer's weight rows in pxn_model.wa */
    int32_t rr[12];                     /* its residual 1x1: 3 x {w[o][0..2] Q24, bias[o] Q20} */
    int32_t rr2[12];                    /* the same for the pedal's post2: w[o][c] by c then o, then the 3 biases */
} pxn_layer;

typedef struct {
    int32_t wa[1542 + 4];               /* each layer's 3 rows [mixin, bias, taps x channels], Q24 (+ slack) */
    int32_t rech[PXN_C];
    int32_t head[PXN_S + 4];            /* [0, bias, 16 taps x 3 channels] Q24 (+ slack) */
    float out_gain;                     /* head_scale / 2^20 */
    pxn_layer L[PXN_NL];
    int32_t hlen, hw;                   /* head ring */
    int32_t *hring;
    int ready;
} pxn_model;

/* Converts 1871 float weights (the .nam file's order) into the model. 0 = ok, -1 = a weight is not finite, -2 = the
 * ring memory is too small. The rings (PXN_RING_BYTES in all) come from the caller in up to two pieces (r0 of n0
 * bytes, r1 of n1 bytes: one RAM region may not hold them all); they are cleared. */
int pxn_load(pxn_model *m, const float *w, int32_t *r0, unsigned n0, int32_t *r1, unsigned n1);
#define PXN_RING_BYTES ((unsigned)PXN_RING * PXN_C * 4u)
/* the pedal's hand-written routines: checked once against the C ones (1 = in use); call before audio starts.
 * scratch: PXN_TEST_WORDS words (the ring memory will do: reset the model after) */
int pxn_selftest(int32_t *scratch);
#define PXN_TEST_WORDS 1280
int pxn_asm_in_use(void);
/* the vector unit: its start-up check's largest difference with C if in use (>= 0), else -1 - that difference (or -1) */
int pxn_vec_check(void);
void pxn_vstart(const int32_t *w, const int32_t *x, int T, int sb, int zb, int32_t *z, int n);   /* bench */
void pxn_vwait(void);
/* measurements for the firmware's bench screen (px_audio.h, BENCH_AT_BOOT): ticks, see px_nam.c */
uint32_t pxn_bench_layer(int32_t *scratch, uint32_t (*ticks)(void), int hand, int reps);
uint32_t pxn_bench_dot(int32_t *x, int32_t *w, int32_t *z, int vec, uint32_t (*ticks)(void), int reps);
uint32_t pxn_bench_post(int32_t *b, uint32_t (*ticks)(void), int reps, int v2);
uint32_t pxn_bench_vcmd(int32_t *buf, uint32_t (*ticks)(void), int nlen, int nloop, int xs, int zs, int reps, int start_only);
/* how the blocks run: 0 the CPU alone, 1 each dot product on the vector unit in turn, 2..4 the vector unit and the
 * CPU at once (half a block on the unit while the CPU prepares or finishes the other half; the CPU also takes 0, 4 or
 * 8 frames of each half's dot products itself), 5..10 two cores (the other core takes the block's first 12 to 24
 * frames; with or without the vector unit beside this one). pxn_tune times them on a loaded model and keeps the
 * fastest (ticks for 256 frames in t[], 0 = not available); the model is cleared after. */
#define PXN_MODES 11
/* the other core, for modes 5..8: run(fn, arg) hands it fn(arg) (1 = taken, 0 = do it yourself), wait() waits for it
 * (0 = done, -1 = dropped). Without it those modes are not used. */
void pxn_set_helper(int (*run)(void (*fn)(uint32_t), uint32_t arg), int (*wait)(void));
/* the two-core pipeline (v37): CPU1 serves the network's blocks after its own work, the roles the other way round
 * (see px_nam.c): pxn_srv_begin(lateness in ticks) on CPU0 before starting CPU1's job, pxn_serve() at the end of that
 * job, pxn_srv_end() on CPU0 after the network, then wait for the job */
void pxn_srv_begin(int32_t late);
void pxn_serve(void);
void pxn_srv_end(void);
int pxn_srv_active(void);
uint32_t pxn_frame_ticks(void);        /* one frame through every layer on one core (ticks, timed by pxn_tune) */
int pxn_dual(void);                     /* 1 = the mode in use wants the other core (the caller must not be it) */
int pxn_tune(pxn_model *m, uint32_t (*ticks)(void), uint32_t t[PXN_MODES]);
int pxn_mode(void);
int pxn_post2_in_use(void);
int pxn_rdot_in_use(void);
/* fast memory for the vector unit's inputs (X rows, a copy of the weights: PXN_FAST_BYTES), away from the RAM the CPUs
 * work in: offered before pxn_selftest, which checks the unit there and then uses it */
void pxn_set_fast(int32_t *base, unsigned bytes);
#define PXN_FAST_BYTES (((PXN_N + 1) * PXN_S + 1550 + PXN_S + 4) * 4u)
int pxn_fast_in_use(void);
int pxn_rdot_chosen(void);              /* the CPU's layers: 1 rdot, 0 gathered X rows (timed by pxn_tune) */
uint32_t pxn_rdot_ticks(int rd);        /* that timing, mode 0: 0 gathered, 1 rdot, 2 rdq (load-ahead rdot) */
uint32_t pxn_prof_ticks(int k);
int pxn_rdl_state(void);              /* rdl (a layer's groups in one call): 0 failed its check, 1 slower, 2 in use */
int pxn_rdm_state(void);              /* rdm (rdl two rows at a time): 0 failed its check, 1 slower, 2 in use */
uint32_t pxn_post_ticks(int v);        /* mode 0 timed with post2 (0) and post3 (1) */
int pxn_post3_state(void);             /* 0 post3 failed its check, 1 slower than post2, 2 in use */        /* where mode 0's time goes (pxn_tune): see px_nam.c */
int pxn_post4_state(void);             /* 0 post4 failed its check, 1 slower, 2 in use (layers read from their rings) */
int pxn_post5_state(void);             /* 0 post5 failed its check, 1 slower, 2 in use (post4's frames two at a time) */
int pxn_rdq_in_use(void);
int pxn_rdq_checked(void);              /* all three passed the start-up check */
uint32_t pxn_bench_part(int32_t *scratch, uint32_t (*ticks)(void), int part, int reps);
uint32_t pxn_bench_vfast(int32_t *buf, uint32_t (*ticks)(void), int v);   /* v 0..7, see px_nam.c */
int pxn_vfast_macs(int v);
uint32_t pxn_bench_ops(int kind, int32_t *buf, int reps, int *n, uint32_t (*ticks)(void));
/* v40.2: two cores by layers (see px_nam.c): CPU0 queues each block through layers 0..s (pxn_lp_a, PXN_N frames), CPU1
 * finishes the oldest queued block (pxn_lp_b: 1 = one finished, 0 = none waiting), writing its output where pxn_lp_a
 * was told. Memory: PXN_LP_BYTES from the caller (pxn_set_lp); pxn_lp_ok() says whether it can run in the mode chosen;
 * pxn_lp_reset() after the network is reset or loaded (no block in progress); pxn_lp_split(s) sets s for the next
 * blocks (0..PXN_NL-1) */
#define PXN_LPQ 6
#define PXN_LP_BYTES ((unsigned)PXN_LPQ * (16u + 4u * (PXN_N + 4u + 2u * PXN_N * PXN_C)) + 4u * (PXN_N * PXN_C + 4u + PXN_NL) + 8u)
void pxn_set_lp(void *base, unsigned bytes);
int pxn_lp_ok(void);
void pxn_lp_reset(void);
void pxn_lp_split(int s);
int pxn_lp_split_now(void);
uint32_t pxn_lp_queued(void);
int pxn_lp_split_used(void);             /* v40.4: the s of the last block handed over (it follows the s asked: px_nam.c) */
void pxn_lp_force(void);                 /* the next block takes the s asked, waiting for the other core if it must */
uint32_t pxn_lp_handed(void);            /* v40.4: blocks handed over so far */
int pxn_lp_before(uint32_t upto);        /* 1 = a block handed over before that count is not finished */
uint32_t pxn_lp_share(int s);            /* the network's work in layers 0..s, in 1/1024 of the whole */
void pxn_lp_clock(uint32_t (*ticks)(void), uint32_t limit);   /* v40.5: CPU0's waits for the other core end after limit */
int pxn_lp_gave_up(void);                /* v40.5: 1 = one did (the other core is stuck): the caller retires it, resets */
void pxn_lp_a(pxn_model *m, const float *in, float *out);
int pxn_lp_b(pxn_model *m);
int pxn_lp_equiv(pxn_model *m);          /* against one core, the other core serving: blocks that differ */
/* the hand routines against C on the whole network (64 blocks, mode `mode` against all C, mode 0): the blocks whose
 * output differs by a bit, -1 if not available (see px_nam.c) */
int pxn_equiv(pxn_model *m, int mode);
/* clears the histories (silence in) */
void pxn_reset(pxn_model *m);
/* n frames (any n): in -> out, float, in place allowed */
void pxn_process(pxn_model *m, const float *in, float *out, int n);
