/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket X — the REAL sound: guitar -> DSP engine (engine/px_dsp.c) -> output, set by the UI.
 *
 * The audio's way in and out (the I2S, the block's stages, the tuner's capture) is the base's: base/audio.h.
 *
 * Processing runs in the I2S interrupt (block of 128 frames = 2.9 ms), in float on the hardware
 * FPU (-mcpu=r3 -mfprev1). Safeguard: if the DSP takes more than 85 % of the time for 8 blocks
 * in a row, we switch to BYPASS (direct guitar) instead of locking up the pedal. The UI shows the load.
 */
#pragma once
#include <stdint.h>
#ifndef BENCH_AT_BOOT
#define BENCH_AT_BOOT 0                                 /* 1 = DSP benchmark screen at boot (diagnostics) */
#endif
#include <math.h>
#include "px_dsp.h"
#include "base/audio.h"                                 /* the I2S, a_input, dsp_out, dsp_load, a_output, the tuner */

/* ===================== engine + interrupt-time processing ===================== */
static px_engine g_eng __attribute__((section(".pool"), aligned(8)));   /* ~250 KB: "POOL" RAM region */
static px_preset_t a_kp;
static volatile uint8_t a_kpend;
static uint8_t a_full;                                 /* the next apply must be a full one (a user model rewritten) */
static void a_commit(void) { if (a_kpend) { px_engine_knobs(&g_eng, &a_kp); a_kpend = 0; } }
/* v38: where the time goes, per block, over the last second: % of an audio block's time (index PX_NBLK: the NAM network;
 * with a NAM amp the blocks after it run on the second core, beside the ones before it) */
static uint8_t a_pf[PX_NBLK + 1];
static uint32_t a_wcut;                                /* v39: wet blocks cut short so far (the footer, when not 0) */
/* DSP work for one block: a_in -> a_res (gain, clipping) + load measurement. Runs on the 2nd core (CPU1)
 * when available, otherwise on CPU0 in the interrupt. */
static void dsp_block(void)
{
    uint32_t t0 = fm1_ticks();
    a_commit();
    if (!a_bypass && !a_hold) {
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_process(&g_eng, a_in + o, a_out + o, PX_MAX_BLOCK);
    } else {
        for (uint32_t i = 0; i < A_FR; i++) a_out[i] = a_in[i];
    }
    dsp_out();
    dsp_load(t0, 85u);
}
/* --- second core: block N goes to CPU1 at the end of IRQ N; IRQ N+1 collects the result before
 * sending it out. Same latency as before (processing already had to finish before the next block), but CPU0
 * is free between IRQs: display and UI never touch the audio again. Automatic fallback to CPU0
 * if the 2nd core rejects a job or misbehaves (FM1_CPU1_FALLBACK 1, Felucca's fm1_cpu1.h). */
static volatile uint8_t a_c1;                          /* 1 = DSP runs on the 2nd core */
static uint8_t a_job;                                  /* a block is in progress on CPU1 */
/* v39: the wet lanes (px_dsp.h px_wet_*, docs/STOCK-FIRMWARE.md 7.3): with the 2nd core on, the delay and the reverb make
 * their sound beside the chain, two blocks late (their taps make up for it):
 *  - one core's chain (no NAM amp): CPU1 runs the chain, CPU0 the lanes at the same time, in the interrupt (cut short,
 *    and counted, if they ever reach 85 % of the block);
 *  - a NAM amp on two cores: CPU1 runs the lanes first in its share (post_job), where the delay and the reverb ran in
 *    v38; the network's sharing between the cores takes their time into account as before.
 * v39.1: the lanes of a NAM preset no longer run after the block, in the time left before the next interrupt: there was
 * too little of it (the network shares its work between the cores, so the lanes' time only half came back), they were
 * cut short every block, and the echoes and the reverb came out chopped at the block rate (a whistle). */
#ifndef OP_WET_LANES
#define OP_WET_LANES 1                                 /* 0: the delay and the reverb in the chain, as in v38 */
#endif
#if DSP_CPU1
#define A_WLAG (2 * (int)A_FR)
#ifndef OP_NAM_LP
#define OP_NAM_LP 1                                    /* v40.2: 0 = the network split between the cores by frames, as v40 */
#endif
static void a_c1_off(void)                             /* one core from now on: no lanes, no layer split */
{
    a_c1 = 0;
    px_wet_mode(&g_eng, 0, 0);
    if (g_eng.nam_lp) { px_nam_lp(&g_eng, 0); a_nan = 1; }   /* (a block CPU1 left half done: the engine is reset) */
}
static void dsp_job(uint32_t arg) { (void)arg; dsp_block(); }
#endif
#if DSP_CPU1
/* the NAM network's helper core (CPU1), for its two-core modes: the job functions (px_nam_helper) */
static int a_hrun(void (*fn)(uint32_t), uint32_t arg) { return fm1_cpu1_run(fn, arg); }
static int a_hwait(void) { return fm1_cpu1_wait(); }
/* v37: a NAM amp on two cores, pipelined (one block more latency: 2.9 ms). In each block's interrupt CPU1 first runs
 * the blocks after the amp on the previous block's network output (and makes its output), while CPU0 runs the blocks
 * before the amp on this block; then both run the network on this block (CPU1 as its helper). PRE and POST never touch
 * the same block's state (tests/engine/pipe_test.c: the same output as one core, one block later). */
static float a_nb[2][A_FR];                            /* the network's output: being made / the previous block's */
static uint8_t a_nbi, a_nbv, a_prun;                  /* a_nb[a_nbi] is this block's; a_nbv: the other one holds one */
static void post_run(uint32_t prev, int run, int nbv)   /* run: the engine on; nbv: a_nb[prev] holds a block */
{
    if (g_eng.wl_on) px_wet_run(&g_eng, 0u, 0);       /* v39.1: the lanes (the previous block's sends), first */
    if (run && nbv) {
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_part(&g_eng, a_nb[prev] + o, a_out + o, PX_MAX_BLOCK, PX_PART_POST);
    } else if (!run) {
        for (uint32_t i = 0; i < A_FR; i++) a_out[i] = a_in[i];
    } else for (uint32_t i = 0; i < A_FR; i++) a_out[i] = 0.0f;   /* the pipeline's first block */
    dsp_out();
}
static void post_job(uint32_t prev) { post_run(prev, a_prun, a_nbv); }
/* diagnostic 22: CPU1 then goes straight on to the network (pxn_serve), taking the last frames of each 32-frame block
 * while CPU0 takes the first ones and never waits; CPU0 takes more frames in the first blocks by how late CPU1 came
 * to the network in the blocks before (a_late, ticks: POST's end against PRE's) */
static volatile uint32_t a_tpost;                      /* (CPU1) when POST was done */
static int32_t a_late;
static void pipe_job(uint32_t prev)
{
    post_job(prev);
    a_tpost = fm1_ticks();
    pxn_serve();
}
static void dsp_pipe_run(int run)                      /* run: the engine on (else bypassed) */
{
    const uint32_t prev = a_nbi ^ 1u;
    a_commit();                                        /* (before CPU1 starts: the engine is all ours here) */
    a_prun = (uint8_t)run;
    if (run) pxn_srv_begin(a_late > 0 ? a_late : 0);
    const int job = fm1_cpu1_run(run ? pipe_job : post_job, prev);
    if (run && !job) pxn_srv_end();
    if (run) for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
        px_engine_part(&g_eng, a_in + o, a_nb[a_nbi] + o, PX_MAX_BLOCK, PX_PART_PRE);
    const uint32_t tpre = fm1_ticks();
    if (!job) post_job(prev);                          /* (CPU1 off: here) */
    if (run) for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
        px_engine_part(&g_eng, a_nb[a_nbi] + o, a_nb[a_nbi] + o, PX_MAX_BLOCK, PX_PART_NAM);
    if (job) {
        if (run) pxn_srv_end();
        if (fm1_cpu1_wait() < 0) { a_c1_off(); post_job(prev); }   /* (dropped: here, and one core from now on) */
        else if (run) a_late += ((int32_t)(a_tpost - tpre) - a_late) / 4;
    }
    a_nbv = (uint8_t)run;
    a_nbi = (uint8_t)prev;
}
/* v40.2: the network split between the cores by layers (engine/px_nam.c pxn_lp_*). CPU0 runs the blocks before the amp
 * and the network's first layers in each block's interrupt; CPU1 runs the blocks after the amp on the block before's
 * network output, the network's other layers and its head. Every layer runs on whole blocks: no per-layer waits; the
 * output is the same (tests/engine/lp_test.c).
 * v40.4: CPU1 works on through the interrupts. Until now its job ended with the block's last network block, which CPU0
 * hands over at the end of its own share: CPU1 idled at the start of each block, the next interrupt waited for it, and
 * the two cores' work in a block had to fit one after the other (the blocks before the amp + CPU0's share of the
 * network + one network block on CPU1 within 2.9 ms): a chain could run only up to about 88 % a core, and after a full
 * change, when the interrupt runs block after block to catch up, the wait counted as load and engaged the bypass on
 * chains that fit. Now CPU1 runs one worker (lp_job) for as long as the layer split is on: each interrupt asks it for
 * the blocks after the amp (a_lpreq); it first finishes the network blocks handed over before the request (their
 * output is what those blocks play), runs them, and goes back to the network. The interrupt waits only for the
 * output it sends (a_lp_post, in a_dec) and never for the network: the blocks handed over late in a block are finished
 * during the next one. Each core's work may now fill its block. The load shown is the busier core's: CPU0's interrupt
 * (its waits included), or what CPU1 has to do in a block (the blocks after the amp, as timed, and its share of the
 * network). How many layers CPU0 takes follows from the two cores' other work, as timed, and the network's parts
 * (px_nam_lp_share): set in one step, not layer by layer. */
static volatile uint8_t a_lpquit;                      /* (CPU0) the worker must return */
static volatile uint32_t a_lpreq, a_lpdone;            /* requests for the blocks after the amp: asked (CPU0), served (CPU1) */
static volatile uint32_t a_lparg, a_lpupto;            /* the request: prev | nbv << 1 | run << 2; the network blocks before it */
static volatile uint32_t a_lptp;                       /* (CPU1) its last blocks after the amp, lanes and output included (ticks) */
static uint8_t a_lpw;                                  /* the worker runs */
static uint32_t a_lpc0, a_decw;                        /* CPU0's own share last block; its wait for the output (ticks) */
static uint32_t a_lpc1;                                /* CPU1's work for a block: a_lptp + its share of the network (ticks) */
static uint32_t a_lpta;                                /* CPU0's share of the network last block (ticks) */
static uint32_t a_lpe0, a_lpe1;                        /* the cores' other work, smoothed (ticks x 4; 0: not timed yet) */
static uint32_t a_encw;                                /* a_enc's wait for the output (ticks: not CPU0's work) */
static uint8_t a_lpreal;                               /* the last request ran the blocks after the amp (a_lptp is theirs) */
static uint8_t a_lpstuck;                              /* blocks in a row whose share stayed below the one asked */
static uint8_t a_lpnew;                                /* blocks timed since the preset was set (up to 6) */
/* the network's work in layers 0..s, in 1/1024 (px_nam_lp_share), in the RAM code window's free end (app.ld .bss_hi) */
static uint16_t a_lpsh[PXN_NL] __attribute__((section(".bss.hi.a_lpsh")));
static uint32_t a_cl[2], a_cpk[2];                     /* v40.2: each core's load (%, smoothed) and peak since the preset was set */
static void lp_job(uint32_t arg)
{
    uint32_t done = a_lpdone;
    (void)arg;
    for (;;) {
        fm1_cpu1_sync();
        if (a_lpreq != done) {                         /* an interrupt asked for the blocks after the amp */
            const uint32_t q = a_lparg, upto = a_lpupto;
            while (px_nam_lp_before(upto)) px_nam_lp_b(&g_eng);   /* (the network blocks they play, first) */
            const uint32_t t0 = fm1_ticks();
            post_run(q & 1u, (int)(q >> 2 & 1u), (int)(q >> 1 & 1u));
            a_lptp = fm1_ticks() - t0;
            fm1_cpu1_sync();
            a_lpdone = ++done;                         /* (the output is there: a_dec may send it) */
            fm1_cpu1_sync();
            continue;
        }
        if (a_lpquit) break;                           /* (asked to return: the network blocks left stay queued) */
        px_nam_lp_b(&g_eng);                           /* (the oldest network block handed over, if any) */
    }
}
/* the blocks after the amp asked by the interrupt before are done: a_res is whole, and the lanes and those blocks'
 * state are no longer in use (CPU1 is in the network, or idle) */
static void a_collect(void);
static uint32_t a_lp_post(void)                        /* returns the ticks it waited */
{
    if (!a_lpw) return 0u;
    const uint32_t t = fm1_ticks();
    while (a_lpdone != a_lpreq && fm1_cpu1_busy()) {
        fm1_cpu1_sync();
        /* v40.5: not for ever. The worker is one job for as long as the split runs, so fm1_cpu1_busy stays 1 and
         * fm1_cpu1_wait's limit never applies here: a CPU1 that stopped without a fault would hold the interrupt until
         * the watchdog. As fm1_cpu1_wait: past FM1_CPU1_TIMEOUT_US it is retired (LATE), a_collect then goes to one core */
        if (fm1_ticks() - t >= FM1_CPU1_TIMEOUT_US * FM1_TICKS_PER_US) { fm1_cpu1_retire(FM1_CPU1_LATE); break; }
    }
    if (a_lpdone != a_lpreq) a_collect();              /* (its worker ended without it: dropped) */
    return fm1_ticks() - t;
}
static void lp_split(uint32_t e0, uint32_t e1)         /* the split for the cores' other work e0, e1 (ticks) */
{
    /* CPU0: e0 + n sh(s), CPU1: e1 + n (1 - sh(s)), n the network's ticks for a block: the s that brings them
     * closest; the split in use stays unless another is better by half a layer's work (a layer: n / 25) */
    const int32_t n = (int32_t)(pxn_frame_ticks() * A_FR), d = (int32_t)e0 - (int32_t)e1 - n;
    const int now = px_nam_lp_split_used();
    int best = now;
    int32_t fb = d + ((n >> 5) * (int32_t)a_lpsh[now] >> 4);          /* (n sh / 512 = 2 n sh / 1024) */
    if (fb < 0) fb = -fb;
    fb -= n / 50;
    for (int k = 0; k < PXN_NL; k++) {
        int32_t f = d + ((n >> 5) * (int32_t)a_lpsh[k] >> 4);
        if (f < 0) f = -f;
        if (f < fb) { fb = f; best = k; }
    }
    px_nam_lp_split(best);
    /* CPU0 takes more layers only as CPU1 has finished them for the blocks before, without waiting for it. With CPU1
     * behind (it is the busier one) that never comes: for 2 layers or more, or after 4 blocks, CPU0 waits for it once */
    a_lpstuck = (uint8_t)(best > now ? a_lpstuck + 1u : 0u);
    if (best >= now + 2 || a_lpstuck >= 4u) { px_nam_lp_force(); a_lpstuck = 0; }
}
static void dsp_lp_run(int run, uint32_t t_irq)
{
    const uint32_t prev = a_nbi ^ 1u;
    a_commit();                                        /* (CPU1 is in the network, or idle: the blocks are all ours here) */
    if (run && a_lpc0) {                               /* the cores' other work last block; the split from it */
        /* (as timed for the first 6 blocks, the lanes coming in two blocks late; then smoothed) */
        const uint32_t o0 = a_lpc0 > a_lpta ? a_lpc0 - a_lpta : 0u, o1 = a_lptp;
        const int fresh = a_lpnew < 6u;
        if (fresh) a_lpnew++;
        a_lpe0 = a_lpe0 && !fresh ? a_lpe0 - a_lpe0 / 4u + o0 : o0 * 4u;
        if (a_lpreal) {                                /* (not the pipeline's first block: silence) */
            if (!a_lpe1) { a_peak = 0; a_cpk[0] = a_cpk[1] = 0; }   /* (the peaks: from the first split set for this preset) */
            a_lpe1 = a_lpe1 && !fresh ? a_lpe1 - a_lpe1 / 4u + o1 : o1 * 4u;
        }
        if (a_lpe1) lp_split(a_lpe0 / 4u, a_lpe1 / 4u);
    }
    a_lpreal = (uint8_t)(run && a_nbv);
    a_prun = (uint8_t)run;
    a_lparg = prev | (uint32_t)a_nbv << 1 | (uint32_t)(run != 0) << 2;
    a_lpupto = px_nam_lp_handed();
    fm1_cpu1_sync();
    a_lpreq++;
    fm1_cpu1_sync();
    if (!a_lpw) {                                      /* the worker, started (CPU1 off: its work here) */
        a_lpquit = 0;
        fm1_cpu1_sync();
        if (fm1_cpu1_run(lp_job, 0)) { a_lpw = 1; a_job = 1; }
        else {
            while (px_nam_lp_b(&g_eng)) { }
            post_run(prev, run, a_nbv);
            a_lpdone = a_lpreq; a_lptp = 0;
        }
    }
    uint32_t ta = 0;
    if (run) {
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_part(&g_eng, a_in + o, a_nb[a_nbi] + o, PX_MAX_BLOCK, PX_PART_PRE);
        ta = fm1_ticks();
        if (!a_lpe1 && a_lpw) {
            /* the first two blocks after a change: CPU1's other work is not timed yet, and it has little (the pipeline
             * and the lanes fill): CPU0 takes as many layers as fit in 80 % of its block, so that the split then only
             * has to come down (CPU1 takes layers over at once; CPU0 only as CPU1 has finished them) */
            const uint32_t n = pxn_frame_ticks() * A_FR, used = ta - t_irq;
            int s = PXN_NL - 1;
            while (s > 0 && used + (n >> 5) * a_lpsh[s] / 32u > A_BUDGET * 80u / 100u) s--;
            px_nam_lp_split(s);
        }
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_part(&g_eng, a_nb[a_nbi] + o, a_nb[a_nbi] + o, PX_MAX_BLOCK, PX_PART_NAM);
        /* v40.5: a wait for CPU1 in the network gave up (px_nam.c pxn_lp_late): CPU1 stopped without a fault. Retired
         * (LATE): a_dec's a_lp_post then finds it gone and a_collect goes to one core, the engine reset (a_c1_off) */
        if (px_nam_lp_gave_up()) fm1_cpu1_retire(FM1_CPU1_LATE);
        ta = fm1_ticks() - ta;
        if (!a_lpw) while (px_nam_lp_b(&g_eng)) { }
    }
    a_lpta = ta;
    a_lpc0 = run && a_lpw ? fm1_ticks() - t_irq - a_encw + a_uat : 0u;   /* (from the interrupt: the input, the tuner's samples; USB audio) */
    /* CPU1's work for a block: the blocks after the amp as last timed, and its share of the network at this split */
    {
        const uint32_t n = pxn_frame_ticks() * A_FR;
        a_lpc1 = run && a_lpw ? a_lptp + (n >> 5) * (1024u - a_lpsh[px_nam_lp_split_now()]) / 32u : 0u;
    }
    a_nbv = (uint8_t)run;
    a_nbi = (uint8_t)prev;
}
static void dsp_pipe(uint32_t t_irq)
{
    uint32_t t0 = fm1_ticks();
    if (g_eng.nam_lp) {
        dsp_lp_run(!a_bypass && !a_hold, t_irq);
        const uint32_t c1 = a_lpc1, c0 = fm1_ticks() - t_irq + a_decw + a_uat;
        t0 = fm1_ticks() - (c1 > c0 ? c1 : c0);         /* the busier core */
        for (int k = 0; k < 2; k++) {                   /* each core's own (PING's profile) */
            const uint32_t pc = (k ? c1 : c0) * 100u / A_BUDGET;
            a_cls[k] += pc - a_cls[k] / 8u;
            a_cl[k] = a_cls[k] / 8u;
            if (pc > a_cpk[k]) a_cpk[k] = pc;
        }
    } else { dsp_pipe_run(!a_bypass && !a_hold); t0 -= a_uat; }   /* (USB audio's time in a_dec: CPU0's too) */
    dsp_load(t0, 93u);                                 /* (CPU0's whole block here, the UI gets the rest) */
}
#endif
static void a_collect(void)                            /* collects the block in progress on CPU1 */
{
#if DSP_CPU1
    if (a_job) {
        a_job = 0;
        if (a_lpw) {                                   /* v40.4: the layer split's worker: asked to return (it serves */
            fm1_cpu1_sync();                           /* a request it has first) */
            a_lpquit = 1;
            fm1_cpu1_sync();
        }
        if (fm1_cpu1_wait() < 0) a_c1_off();           /* dropped: CPU0 takes over (this block is lost) */
        if (a_lpw) {
            a_lpw = 0;
            a_lpdone = a_lpreq;                        /* (served before it returned, or dropped with it) */
            while (px_nam_lp_b(&g_eng)) { }            /* (the network blocks it left, finished here) */
        }
    }
#endif
}

static void a_block(const uint8_t *data, int len)      /* a guitar block: its DSP started, or run */
{
#if DSP_CPU1
    const uint32_t t_irq = fm1_ticks();
#endif
#if DSP_CPU1
    /* a_in must no longer be read by CPU1. v40.4: the layer split's worker stays (it may be in the network): only its
     * blocks after the amp must be done */
    a_encw = 0;
    if (a_lpw && a_c1 && g_eng.nam_lp && px_nam_dual(&g_eng)) a_encw = a_lp_post(); else
#endif
    a_collect();
#if DSP_CPU1
    if (g_eng.wl_on) px_wet_swap(&g_eng);              /* v39: nothing runs: the lanes' buffers change roles */
#endif
    a_input(data, len);
#if DSP_CPU1
    /* a NAM amp on two cores (px_nam_dual): pipelined here (dsp_pipe), CPU1 its helper */
    if (a_c1 && !px_nam_dual(&g_eng)) {
        a_nbv = 0;
        a_commit();                                    /* v39: here, before the lanes run beside the chain */
        if (fm1_cpu1_run(dsp_job, 0)) {
            a_job = 1;
            if (g_eng.wl_on) px_wet_run(&g_eng, t_irq + A_BUDGET * 85u / 100u, 0);   /* v39: the lanes, on CPU0 */
            return;
        }
        a_c1_off();
    }
    if (a_c1) { dsp_pipe(t_irq); return; }            /* (the lanes: in post_job) */
    a_nbv = 0;
#endif
    dsp_block();
}
static void a_send(int32_t *d, uint32_t n)             /* an output block filled: the block a_block just started */
{
#if OP_UADEV
    uad_send();
#endif
#if DSP_CPU1
    a_decw = 0;
    if (a_lpw) a_decw = a_lp_post();                   /* v40.2: the layer split: only its output (the worker goes on) */
    else
#endif
    a_collect();                                       /* the previous block must be finished */
    a_output(d, n);
}
/* ===================== UI -> engine: same structure (px_preset_t), plain copy ===================== */
static void ui_to_px(px_preset_t *pp) { *pp = PRESET; }
static uint32_t ui_hash(void)                          /* hash of the whole preset (volume handled separately) */
{
    const uint8_t *b = (const uint8_t *)&PRESET;
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < sizeof(PRESET); i++) h = (h ^ b[i]) * 16777619u;
    return h;
}
static int a_wait(volatile uint8_t *v, uint8_t until, uint32_t ms)   /* 1 = *v reached until within ms */
{
    for (uint32_t t = 0; *v != until; t++) { if (t >= ms * 10u) return 0; fm1_delay_us(100); }
    return 1;
}
static void audio_apply(void)                          /* the preset to the engine (main loop) */
{
    px_preset_t pp;
    ui_to_px(&pp);
    /* v38: knob changes and blocks on / off go to the audio side, which applies them at the start of a block */
    const int live = a_running && !a_mute && a_wait(&a_kpend, 0, 50u);
    if (live && !a_full) {
        const int ch = px_engine_change(&g_eng, &pp);
        if (ch == PX_CH_NONE) return;
        if (ch == PX_CH_KNOBS) {
            px_engine_prepare(&g_eng, &pp);            /* the cabinet's heavy part, here, while the audio plays */
            a_kp = pp;
            __asm__ volatile("" ::: "memory");          /* (a_kp and the cabinet store written before the flag) */
            a_kpend = 1;
            return;
        }
    }
    a_full = 0;
    a_swc = a_sw0;
    /* anything else: the output fades out, the engine is set (the interrupt held off meanwhile: a late block now only
     * repeats silence), and the output fades back in */
    if (live) { a_xft = 0; for (uint32_t t = 0; a_xf != 0.0f && t < 400u; t++) fm1_delay_us(100); }   /* (40 ms at most) */
    const uint32_t t0 = fm1_ticks();
    if (a_running) fm1_irq_mask(FM1_IRQ_ALNK0);
    a_collect();                                       /* CPU1 must not process during the recompute */
#if DSP_CPU1
    while (px_nam_lp_b(&g_eng)) { }                    /* v40.2: the layer split's blocks in flight, finished */
#endif
    px_engine_set(&g_eng, &pp);
#if DSP_CPU1
    px_nam_lp(&g_eng, a_c1 && OP_NAM_LP && px_nam_dual(&g_eng));
#endif
    a_swset = fm1_ticks() - t0;
    a_kpend = 0;
#if DSP_CPU1
    a_nbv = 0;                                         /* (the pipeline restarts from silence) */
#endif
    a_bypass = 0; a_over = 0; a_peak = 0; a_cpk[0] = a_cpk[1] = 0;
#if DSP_CPU1
    a_lpc0 = 0; a_lpe0 = a_lpe1 = 0; a_lptp = 0; a_lpreal = 0; a_lpstuck = 0; a_lpnew = 0;   /* v40.4: the cores' other work is timed anew */
#endif
    if (a_xf != 1.0f) a_swin = 1; else a_sw1 = fm1_ticks();   /* (not faded out: the audio was not running) */
    if (a_running) fm1_irq_unmask(FM1_IRQ_ALNK0);
    a_xft = 1;
}
static void audio_reset(void)                          /* after a NaN: engine reset, preset re-applied */
{
    px_preset_t pp;
    ui_to_px(&pp);
    if (a_running) fm1_irq_mask(FM1_IRQ_ALNK0);
    a_collect();
    px_engine_init(&g_eng, 44100.0f);
#if DSP_CPU1
    px_wet_mode(&g_eng, a_c1 && OP_WET_LANES, A_WLAG);   /* (the init cleared it) */
#endif
    px_engine_set(&g_eng, &pp);
#if DSP_CPU1
    px_nam_lp(&g_eng, a_c1 && OP_NAM_LP && px_nam_dual(&g_eng));   /* (the init cleared it; the network was reset) */
#endif
    a_kpend = 0; a_xf = 0.0f; a_xft = 1;               /* (back from silence) */
    a_nan = 0; a_nan_count++;
    if (a_running) fm1_irq_unmask(FM1_IRQ_ALNK0);
}
/* ===================== boot benchmark screen (6 s): CPU/FPU speed and engine cost ===================== */
static uint32_t b_engine(const px_preset_t *pp)        /* % of the real-time budget for 128 frames */
{
    px_engine_set(&g_eng, pp);
    for (uint32_t i = 0; i < A_FR; i++) a_in[i] = 0.3f * sinf((float)i * 0.0157f) + 0.05f * sinf((float)i * 0.11f);
    for (int w = 0; w < 4; w++) { px_engine_process(&g_eng, a_in, a_out, 64); px_engine_process(&g_eng, a_in + 64, a_out + 64, 64); }
    fm1_wdt_feed();
    uint32_t t0 = fm1_ticks();
    for (int r = 0; r < 8; r++) { px_engine_process(&g_eng, a_in, a_out, 64); px_engine_process(&g_eng, a_in + 64, a_out + 64, 64); }
    uint32_t dt = (fm1_ticks() - t0) / 8u;
    fm1_wdt_feed();
    return dt * 100u / A_BUDGET;
}
static uint32_t b_ticks(void) { return fm1_ticks(); }
/* v37: what the NAM network costs on the pedal, piece by piece (cycles at 360 MHz: ticks x 15). Off by default:
 * BENCH_AT_BOOT 1 (at the top of this file) shows it for 12 s at boot. */
static void audio_bench(void)
{
#if BENCH_AT_BOOT
    char l[48];
    uint32_t y = 26;
    lcd_rect(0, 0, 240, 240, UI_BG);
    lcd_text(4, 4, "DSP BENCH " OP_VERSION, UI_ACC, UI_BG, 2);
    /* integer loop and float loop: time per iteration (ns) */
    volatile uint32_t ia = 0; volatile float fa = 0.0f, fb = 1.0001f, fc = 0.5f;
    uint32_t t0 = fm1_ticks();
    for (uint32_t i = 0; i < 100000u; i++) ia += i;
    uint32_t ti = fm1_ticks() - t0;
    t0 = fm1_ticks();
    for (uint32_t i = 0; i < 100000u; i++) fa = fa * fb + fc;
    uint32_t tf = fm1_ticks() - t0;
    fm1_wdt_feed();
    (void)ti; (void)tf;
    l[0] = 0; b_str(l, "NAM ROUTINE: "); b_str(l, pxn_asm_in_use() ? "HAND (OK)" : "C (CHECK FAILED)");
    lcd_text(4, y, l, pxn_asm_in_use() ? GRN : RED, UI_BG, 1); y += 14;
    /* v37 diagnostic 22: the ways of running the network as timed at start-up (% of real time, NAM alone): the CPU, the
     * unit in turn, both at once with the CPU taking 0, 4 or 8 frames of 16; two cores (CPU1 taking 16 or 12 frames of
     * 32), with the unit beside CPU0 or not. RDOT: the layer read straight from its ring (no gather), against the gathered
     * X rows (the CPU alone); FAST RAM: the vector unit's inputs in the data-cache ways (px_cacheram.h) */
    {
        l[0] = 0; b_str(l, "RDL ");                /* diagnostic 20: a layer's groups in one call */
        if (pxn_rdl_state()) b_num(l, pxn_rdot_ticks(3) * 100u / (A_BUDGET * 2u)); else b_str(l, "FAIL");
        b_str(l, "% RDQ "); b_str(l, pxn_rdq_checked() ? "" : "(FAILED) ");
        b_num(l, pxn_rdot_ticks(2) * 100u / (A_BUDGET * 2u)); b_str(l, "% RDOT ");
        b_num(l, pxn_rdot_ticks(1) * 100u / (A_BUDGET * 2u));
        b_str(l, pxn_rdl_state() == 2 ? "% >L" : pxn_rdq_in_use() ? "% >Q" : pxn_rdot_chosen() ? "% >R" : "% >G");
        lcd_text(4, y, l, UI_TXT, UI_BG, 1); y += 12;
        fm1_wdt_feed();
        static const char *M[PXN_MODES] = {"C", " V", " B0 ", " B4 ", " B8 ", "2C ", " V12 ", " V16 ", " V12C4 ",
                                           " 2C20 ", " 2C24 "};
        for (int m = 0; m < PXN_MODES; m++) {
            if (m == 0 || m == 5 || m == 9) l[0] = 0;
            uint32_t t = px_nam_tuned(m);
            b_str(l, M[m]);
            if (t) b_num(l, t * 100u / (A_BUDGET * 2u)); else b_str(l, "-");
            if (m == 4 || m == 8) { lcd_text(4, y, l, GRN, UI_BG, 1); y += 12; }
        }
        b_str(l, " >"); b_num(l, (uint32_t)pxn_mode());
        lcd_text(4, y, l, GRN, UI_BG, 1); y += 12;
        /* diagnostic 18: where the CPU alone (mode 0) spends it: input, tap tables, kernel (groups of 4 frames), the
         * gathered 1..3-frame rests, post, the head (groups, rest, rings + output), the whole */
#define B_P(k) (pxn_prof_ticks(k) * 100u / (A_BUDGET * 2u))
        const uint32_t g1 = pxn_prof_ticks(1), g8 = pxn_prof_ticks(8);
        l[0] = 0; b_str(l, "IN "); b_num(l, B_P(0)); b_str(l, " TBL "); b_num(l, B_P(8));
        b_str(l, " KER "); b_num(l, (g1 > g8 ? g1 - g8 : 0u) * 100u / (A_BUDGET * 2u));
        b_str(l, " REST "); b_num(l, B_P(2));
        lcd_text(4, y, l, YEL, UI_BG, 1); y += 12;
        l[0] = 0; b_str(l, "POST "); b_num(l, B_P(3)); b_str(l, " HEAD "); b_num(l, B_P(6)); b_str(l, "+");
        b_num(l, B_P(7)); b_str(l, "+"); b_num(l, B_P(4)); b_str(l, " ALL "); b_num(l, B_P(5));
        lcd_text(4, y, l, YEL, UI_BG, 1); y += 12;
        {   /* the new post (post3) against post2, mode 0; >3 when it is the one used */
            const int p3 = pxn_post3_state();
            l[0] = 0; b_str(l, "POST2 "); b_num(l, pxn_post_ticks(0) * 100u / (A_BUDGET * 2u));
            b_str(l, " POST3 ");
            if (p3) b_num(l, pxn_post_ticks(1) * 100u / (A_BUDGET * 2u)); else b_str(l, "FAILED");
            b_str(l, p3 == 2 ? " >3" : " >2");
            lcd_text(4, y, l, p3 ? YEL : RED, UI_BG, 1); y += 14;
        }
#undef B_P
    }
    fm1_wdt_feed();
    /* diagnostic 21: the network alone, then every factory preset as the audio interrupt runs it (with a NAM amp:
     * pipelined on the two cores; otherwise the whole chain on one core), % of real time: green up to 80, yellow up
     * to 85, red above (the safety bypass: 93 pipelined, 85 otherwise) */
    px_preset_t pp;
    px_preset_factory(&pp, 17);
    for (int z = 0; z < PX_NBLK; z++) pp.on[z] = 0;
    pp.on[PX_B_AMP] = 1;
    {
        uint32_t pct = b_engine(&pp);
        l[0] = 0; b_str(l, "NAM AMP ONLY "); b_num(l, pct); b_str(l, " %  PRESETS:");
        lcd_text(4, y, l, pct <= 85u ? GRN : RED, UI_BG, 1); y += 12;
    }
    for (int f = 0; f < 24; f++) {
        px_preset_factory(&pp, f);
        uint32_t pct = b_engine(&pp);
#if DSP_CPU1
        if (px_nam_dual(&g_eng)) {
            a_nbv = 0;
            uint32_t dt = 0;
            a_late = 0;
            for (int r = 0; r < 16; r++) {
                uint32_t t0 = fm1_ticks();
                dsp_pipe_run(1);
                if (r >= 8) dt += fm1_ticks() - t0;
            }
            a_nbv = 0;
            fm1_wdt_feed();
            pct = dt / 8u * 100u / A_BUDGET;
        }
#endif
        l[0] = 0;
        if (f < 9) b_str(l, "0");
        b_num(l, (uint32_t)f + 1u); b_str(l, " "); b_num(l, pct);
        lcd_text(4 + (uint32_t)(f % 5) * 48u, y, l, pct <= 80u ? GRN : pct <= 85u ? YEL : RED, UI_BG, 1);
        if (f % 5 == 4 || f == 23) y += 12;
    }
    lcd_text(4, y + 4, "100% = REAL TIME. PLEASE TAKE A PHOTO", UI_ACC, UI_BG, 1);
    for (int i = 0; i < 120; i++) { fm1_wdt_feed(); fm1_delay_ms(100); }
#endif
}

static void audio_start(void)
{
#if BENCH_AT_BOOT
    px_engine_init(&g_eng, 44100.0f);
    audio_bench();                                     /* benchmark screen (diagnostic): off by default */
    px_nam_check(b_ticks);                             /* (the bench used the NAM memory) */
#endif
    op_stage(ST_ENG_INIT, "ENGINE INIT...");
    px_engine_init(&g_eng, 44100.0f);
    px_clock = b_ticks;                                /* v38: each block's time (the tuner page shows it) */
    op_stage(ST_ENG_SET, "ENGINE SET...");
#if DSP_CPU1
    a_c1 = fm1_cpu1_on;                                /* started by app_main at the very start (fm1_cpu1_start) */
    for (int i = 0; i < PXN_NL; i++) a_lpsh[i] = (uint16_t)px_nam_lp_share(i);
    px_wet_mode(&g_eng, a_c1 && OP_WET_LANES, A_WLAG);   /* v39: the wet lanes, with the 2nd core */
#endif
    audio_volume(master_vol);
    audio_apply();
    audio_io_start();
}

static void audio_prof(uint32_t now)                    /* (main loop) once a second: px_engine.pf -> a_pf */
{
    static uint32_t t_last, b_last;
    if (now - t_last < 1000u) return;
    const uint32_t nb = a_blocks - b_last;
    t_last = now; b_last = a_blocks;
    for (int i = 0; i <= PX_NBLK; i++) {
        uint32_t t = g_eng.pf[i];
        g_eng.pf[i] = 0;
        if (i == PX_NBLK) { t += g_eng.nam_pfb; g_eng.nam_pfb = 0; }   /* v40.2: the layer split's other core */
        if (i == PX_B_DELAY || i == PX_B_REVERB) {     /* v39: with the lanes, their time beside the chain */
            const int L = i == PX_B_REVERB;
            t += g_eng.wl_pf[L]; g_eng.wl_pf[L] = 0;
        }
        const uint32_t den = A_BUDGET * nb / 100u;      /* (32-bit: the pedal has no 64-bit division) */
        const uint32_t pc = den ? t / den : 0u;
        a_pf[i] = (uint8_t)(pc > 255u ? 255u : pc);
    }
    a_wcut = g_eng.wl_late;
}
/* v40.3: the status corner shows LOADING (a_ld, from the main loop) while the sound waits for a NAM model, the MODEL
 * knob's 40 ms: from 20 ms after the change until the new model plays, 300 ms at least (no flash); a preset is set at
 * once, its model with it, so it shows nothing. For PING: how long the last one was shown (ms), how many since boot */
static uint8_t a_ld;
static uint32_t a_ldms, a_ldn;
static int audio_nam_load(void)                        /* 1 = the preset needs another NAM model */
{
    return PRESET.type[PX_B_AMP] == PX_AMP_NAM && px_nam_model(PRESET.p[PX_B_AMP][0]) != g_eng.nam_idx;
}
/* call from the main loop, after the controls and before the screen: applies preset changes (40 ms debounce; v40.3: a
 * preset loaded, at once) */
static void audio_poll(uint32_t now, int at_once)
{
    audio_prof(now);
    static uint32_t last_h, t_change, t_apply, t_ld, vol_shown = 1000u;
    static int pending, nam;
    if (a_nan) audio_reset();
    uint32_t h = ui_hash();
    if (h != last_h) { last_h = h; t_change = now; pending = 1; nam = audio_nam_load(); a_sw0 = fm1_ticks(); }
    if (pending && nam && !a_ld && now - t_change >= 20u) { a_ld = 1; t_ld = now; a_ldn++; }
    /* 40 ms after the last change; v38: while a knob turns, every 40 ms too (it glides: no need to wait for the end) */
    if (pending && (at_once || now - t_change >= 40u ||
                    (now - t_apply >= 40u && !a_kpend && px_engine_change(&g_eng, &PRESET) == PX_CH_KNOBS))) {
        pending = 0; t_apply = now; audio_apply();
    }
    if (a_ld && !pending && now - t_ld >= 300u && (!a_swin || now - t_ld >= 1000u)) { a_ld = 0; a_ldms = now - t_ld; }
    if (master_vol != vol_shown) { vol_shown = master_vol; audio_volume(master_vol); }
}
