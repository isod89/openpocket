/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket X — the REAL sound: guitar -> DSP engine (engine/px_dsp.c) -> output, set by the UI.
 *
 * Pocket X audio chain (found with test builds v10..v14, see docs/AUDIO-SDK.md):
 *   I2S ALNK0, group IIS_PORTA, JieLi master, MCLK=PA4 SCLK=PA5 LRCK=PA6, 44.1 kHz, 24-bit stereo (v26)
 *   24-bit like the official firmware (PocketX_016: "24-bit" bit of each channel in ALNK CON1): in 24-bit mode
 *   the DMA stores one sample per 32-bit word, right-aligned (bit 23 = sign); LEN counts words, so
 *   sr_points = 2 x A_FR to keep 128 stereo frames per block. In 16-bit, the input quantisation noise,
 *   multiplied by the amp gain, was heard as a constant hiss.
 *   guitar INPUT = DAT1 (PA8)    OUTPUT = DAT0 (PA7) -> DAC "344C" -> SGM8905 amp -> OUT + headphones
 *   amp ENABLE = PA9 at 3.3 V
 * I2S driver: the one from the official JieLi SDK (sdk/iis.bc, Apache-2.0), compiled from its bitcode.
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

/* ===================== JieLi I2S driver API (SDK iis.h) ===================== */
struct iis_platform_data {
    uint8_t channel_in, channel_out, port_sel, data_width, mclk_output, slave_mode,
            update_edge, f32e, keep_alive, sel, width_16_to_24;
    uint16_t dump_points_num, sr_points;
};
int  iis_open(struct iis_platform_data *pd, uint8_t index);
int  iis_set_sample_rate(int sample_rate, uint8_t index);
void iis_channel_on(uint8_t channel, uint8_t index);
void iis_set_enc_data_handler(void *priv, void (*cb)(void *, uint8_t *, int, uint8_t), uint8_t index);
void iis_set_dec_data_handler(void *priv, void (*cb)(void *, uint8_t *, int, uint8_t), uint8_t index);
void iis_irq_handler(uint8_t index);
#define IIS_PORTA 0

/* ===================== functions provided to the driver (and the engine) ===================== */
const uint8_t log_tag_const_d_AUDIO_IIS = 0, log_tag_const_e_AUDIO_IIS = 0, log_tag_const_i_AUDIO_IIS = 0;
const uint32_t config_asser = 0;
void log_print(int lvl, const char *tag, const char *fmt, ...) { (void)lvl; (void)tag; (void)fmt; }
int  printf(const char *fmt, ...) { (void)fmt; return 0; }
void *os_current_task_rom(void) { return 0; }
void cpu_assert_debug(void) { }
void P33_SYSTEM_RESET(void) { for (;;) ; }

#if !OP_BLE_TEST   /* the ble build has a real heap (ble/ble_os.c); iis_open draws its block from it */
static uint8_t jl_heap[4096] __attribute__((aligned(8)));   /* iis_open: 2 channels x 8 x sr_points (256) */
void *malloc(unsigned n) { return (n <= sizeof jl_heap) ? jl_heap : 0; }   /* single block: iis_open */
void free(void *p) { (void)p; }
#endif
/* memset / memcpy: felucca-bsp libc.c (included by openpocket.c) */

static volatile uint32_t *jl_preg(uint32_t gpio, uint32_t off)
{
    uint32_t port = gpio >> 4;
    if (port == 3u || port > 7u) return 0;             /* no port D (flash) */
    if (port == 2u && (gpio & 15u) == 7u) return 0;    /* PC7 = display */
    return (volatile uint32_t *)(0x50000u + port * 0x40u + off);
}
static void jl_rmw(uint32_t g, uint32_t off, int on)
{
    volatile uint32_t *r = jl_preg(g, off);
    if (!r) return;
    if (on) *r |= 1u << (g & 15u); else *r &= ~(1u << (g & 15u));
}
int gpio_direction_input(uint32_t g)         { jl_rmw(g, 0x08, 1); return 0; }
int gpio_direction_output(uint32_t g, int v) { jl_rmw(g, 0x00, v); jl_rmw(g, 0x08, 0); return 0; }
int gpio_set_die(uint32_t g, int on)         { jl_rmw(g, 0x0C, on); return 0; }
int gpio_set_pull_up(uint32_t g, int on)     { jl_rmw(g, 0x10, on); return 0; }
int gpio_set_pull_down(uint32_t g, int on)   { jl_rmw(g, 0x14, on); return 0; }

/* ===================== engine + interrupt-time processing ===================== */
#define A_FR     128u                                  /* frames per I2S block */
#define A_BUDGET (A_FR * 1000000u / 44100u * FM1_TICKS_PER_US)   /* ticks available per block (~2.9 ms) */
#define G_AMP_EN 9u                                    /* PA9: SGM8905 amp enable */

static px_engine g_eng __attribute__((section(".pool"), aligned(8)));   /* ~250 KB: "POOL" RAM region */
static float a_in[A_FR], a_out[A_FR];
static int32_t a_res[A_FR];                            /* output, 24-bit right-aligned */
static volatile float a_lvl[2];                        /* input L / R: mean square, in 24-bit steps */
/* input level in dB below full scale (dBFS), rounded; -199 = digital silence */
static int a_level_db(int ch)
{
    float m = a_lvl[ch & 1] * (1.0f / (8388608.0f * 8388608.0f));
    if (m < 1e-20f) return -199;
    return (int)(4.3429448f * logf(m) - 0.5f);         /* 10 log10 */
}
static volatile float a_gain = 0.64f;                  /* MASTER volume (knob), applied at the output */
static volatile uint32_t a_load, a_peak, a_blocks;     /* DSP load in % */
/* v37: what can be heard as a crackle. a_xrun: blocks that took longer than their 2.9 ms (the output then repeats or
 * misses part of a block), a_xrun_at the block of the last one; a_clip: blocks whose output reached the soft clipper's
 * knee (0.8 of full scale; it used to be a hard clip at full scale) */
static volatile uint32_t a_xrun, a_xrun_at, a_clip, a_peak_all;
static volatile uint8_t a_bypass, a_over, a_hold;   /* a_hold: DSP held off (boot, safe mode) */
static volatile uint8_t a_mute;                      /* output zeros (a flash write is coming) */
static volatile uint8_t a_tmute;                     /* v36: tuner open: the output fades to silence (5 ms) */
static float a_mg = 1.0f;
/* v38: changes without clicks. A change of type, order or NAM model fades the output out (a_xft 0), is applied, and fades
 * it back in (~6 ms each way). Knob changes and blocks on / off are handed to the audio side (a_kp, a_kpend) and applied
 * at the start of its next block, where they glide and fade (px_engine_knobs): the interrupt is never held off for them. */
static volatile float a_xf = 1.0f;                     /* the change fade's gain now */
static float a_gv = 0.64f;                             /* the volume as it glides */
static volatile uint8_t a_xft = 1;                     /* where the change fade goes: 1 sound, 0 silence */
static px_preset_t a_kp;
static volatile uint8_t a_kpend;
static uint8_t a_full;                                 /* the next apply must be a full one (a user model rewritten) */
static void a_commit(void) { if (a_kpend) { px_engine_knobs(&g_eng, &a_kp); a_kpend = 0; } }
/* v38: where the time goes, per block, over the last second: % of an audio block's time (index PX_NBLK: the NAM network;
 * with a NAM amp the blocks after it run on the second core, beside the ones before it) */
static uint8_t a_pf[PX_NBLK + 1];
static uint32_t a_wcut;                                /* v39: wet blocks cut short so far (the footer, when not 0) */
static volatile uint8_t a_nan;                         /* engine output a NaN/Inf: needs a reset */
static volatile uint32_t a_nan_count;
static int a_running;

#if OP_BT_AUDIO
/* the Bluetooth audio test image: the phone's music (firmware/bt/bt_a2dp.c decodes it into this ring), added after the volume and
 * before the soft knee, at its own level; never while the output is muted for a flash write */
#include "px_bt_ring.h"
static bt_ring_t bt_ring;
static const float bt_gain = 0.5f;                     /* -6 dB: the phone's own volume does the rest */
static float bt_mix[A_FR] __attribute__((section(".bss.lo2.bt_mix")));   /* (beside the ring: app-bt.ld .bss_lo2) */
#endif
/* DSP work for one block: a_in -> a_res (gain, clipping) + load measurement. Runs on the 2nd core (CPU1)
 * when available, otherwise on CPU0 in the interrupt. */
static void dsp_out(void);
static void dsp_load(uint32_t t0, uint32_t lim);
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
static void dsp_out(void)                              /* a_out -> a_res: safety net, mutes, volume */
{
    /* safety net: a NaN/Inf would poison the filters forever (silence). Output silence and
     * the main loop resets the engine (audio_poll). */
    for (uint32_t i = 0; i < A_FR; i++) {
        float v = a_out[i];
        if (v != v || v > 1.0e6f || v < -1.0e6f) { a_nan = 1; break; }
    }
    if (a_nan) for (uint32_t i = 0; i < A_FR; i++) a_out[i] = 0.0f;
    if (a_mute) for (uint32_t i = 0; i < A_FR; i++) a_out[i] = 0.0f;
    {   /* tuner mute, with a short ramp both ways (no click) */
        float tg = a_tmute ? 0.0f : 1.0f;
        if (a_mg != tg) {
            for (uint32_t i = 0; i < A_FR; i++) { a_mg += (tg - a_mg) * 0.005f; a_out[i] *= a_mg; }
            if (tg > 0.5f ? a_mg > 0.999f : a_mg < 0.0005f) a_mg = tg;
        } else if (tg == 0.0f) for (uint32_t i = 0; i < A_FR; i++) a_out[i] = 0.0f;
    }
    /* the output: a soft knee from 0.8 of full scale (the same slope there, then bending towards full scale, which it
     * never reaches) instead of a hard clip, whose corners crackle on the loudest notes */
    {   /* v38: the change fade, linear, 256 samples (5.8 ms) each way */
        const float tg = a_xft ? 1.0f : 0.0f;
        float xf = a_xf;
        if (xf != tg) {
            const float st = tg > xf ? 1.0f / 256.0f : -1.0f / 256.0f;
            for (uint32_t i = 0; i < A_FR; i++) {
                xf += st;
                if (st > 0.0f ? xf > 1.0f : xf < 0.0f) xf = tg;
                a_out[i] *= xf;
            }
            a_xf = xf;
        } else if (tg == 0.0f) for (uint32_t i = 0; i < A_FR; i++) a_out[i] = 0.0f;
    }
    const float g = a_gain;                            /* v38: the volume glides there (~3 ms), no zipper */
    float gv = a_gv;
    int clip = 0;
#if OP_BT_AUDIO
    for (uint32_t i = 0; i < A_FR; i++) bt_mix[i] = 0.0f;
    if (!a_mute) bt_ring_mix(&bt_ring, bt_mix, A_FR, bt_gain);
#endif
    for (uint32_t i = 0; i < A_FR; i++) {
        gv += (g - gv) * 0.008f;
#if OP_BT_AUDIO
        float v = a_out[i] * gv + bt_mix[i], m = v < 0.0f ? -v : v;
#else
        float v = a_out[i] * gv, m = v < 0.0f ? -v : v;
#endif
        if (m > 0.8f) {
            float t = (m - 0.8f) * 5.0f;
            m = 0.8f + 0.2f * t / (1.0f + t);
            v = v < 0.0f ? -m : m;
            clip = 1;
        }
        a_res[i] = (int32_t)(v * 8388607.0f);
    }
    a_gv = gv;
    if (clip) a_clip++;
}
static void dsp_load(uint32_t t0, uint32_t lim)        /* the block's load, the safety bypass (over lim % 8 blocks running) */
{
    uint32_t pct = (fm1_ticks() - t0) * 100u / A_BUDGET;
    a_load = (a_load * 7u + pct) / 8u;
    if (pct > a_peak) a_peak = pct;
    if (pct > a_peak_all) a_peak_all = pct;
    if (pct >= 100u && !a_hold) { a_xrun++; a_xrun_at = a_blocks; }
    if (!a_bypass && !a_hold) {
        if (pct > lim) { if (++a_over >= 8u) a_bypass = 1; } else a_over = 0;
    }
    a_blocks++;
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
static void post_job(uint32_t prev)
{
    if (g_eng.wl_on) px_wet_run(&g_eng, 0u, 0);       /* v39.1: the lanes (the previous block's sends), first */
    if (a_prun && a_nbv) {
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_part(&g_eng, a_nb[prev] + o, a_out + o, PX_MAX_BLOCK, PX_PART_POST);
    } else if (!a_prun) {
        for (uint32_t i = 0; i < A_FR; i++) a_out[i] = a_in[i];
    } else for (uint32_t i = 0; i < A_FR; i++) a_out[i] = 0.0f;   /* the pipeline's first block */
    dsp_out();
}
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
/* v40.2: the network split between the cores by layers (engine/px_nam.c pxn_lp_*). In each block's interrupt CPU1 runs
 * the blocks after the amp on the block before's network output as before, then finishes this block's 32-frame network
 * blocks (their layers after CPU0's share, the head) as CPU0 hands them over, and goes on after the interrupt until all
 * are done. a_dec waits only for the output (a_lppost: the blocks after the amp done), the next a_enc for the whole job.
 * CPU0 runs the blocks before the amp and the network's first layers, and does not wait. Every layer runs on whole
 * blocks: no per-layer waits, the head on CPU1; the output is the same (tests/engine/lp_test.c). The load shown is the
 * busier core's: CPU0's (a_enc from its start, its wait for CPU1 included, and a_dec's wait) or CPU1's job. How many
 * layers CPU0 takes follows the difference between the two. */
static volatile uint8_t a_lpstop, a_lppost;
static uint32_t a_lppend;
static volatile uint32_t a_lpc1;                       /* (CPU1) its last job's own work, its waits taken out (ticks) */
static uint32_t a_lpc0, a_decw;                        /* CPU0's own share last block; a_dec's wait for the output (ticks) */
static int32_t a_lpbal;                                /* CPU1's job minus CPU0's share, smoothed */
static uint32_t a_cl[2], a_cpk[2];                     /* v40.2: each core's load (%, smoothed) and peak since the preset was set */
static void lp_job(uint32_t prev)
{
    const uint32_t t0 = fm1_ticks();
    uint32_t idle = 0;
    for (uint32_t k = a_lppend; k; k--) px_nam_lp_b(&g_eng);
    post_job(prev);
    fm1_cpu1_sync();
    a_lppost = 1;                                      /* (the output is there: a_dec may send it) */
    fm1_cpu1_sync();
    for (;;) {
        if (px_nam_lp_b(&g_eng)) continue;
        if (a_lpstop) break;                           /* (stopped and none left: this block's are all done) */
        const uint32_t ti = fm1_ticks();               /* (waiting for CPU0's next block: not CPU1's work) */
        while (!px_nam_lp_queued() && !a_lpstop) fm1_cpu1_sync();
        idle += fm1_ticks() - ti;
    }
    a_lpc1 = fm1_ticks() - t0 - idle;                  /* its own work (an overrun shows as CPU0's wait at a_enc) */
}
static void dsp_lp_run(int run)
{
    const uint32_t prev = a_nbi ^ 1u, t0 = fm1_ticks();
    a_commit();                                        /* (before CPU1 starts: the engine is all ours here) */
    if (run && a_lpc0) {                               /* the split: a layer's worth (both ways: twice) apart, moved */
        const int32_t d = (int32_t)a_lpc1 - (int32_t)a_lpc0;
        a_lpbal += (d - a_lpbal) / 4;
        const int32_t thr = (int32_t)(pxn_frame_ticks() * 4u * PXN_N / PXN_NL);
        if (a_lpbal > thr) { px_nam_lp_split(px_nam_lp_split_now() + 1); a_lpbal = 0; }
        else if (a_lpbal < -thr) { px_nam_lp_split(px_nam_lp_split_now() - 1); a_lpbal = 0; }
    }
    a_prun = (uint8_t)run;
    a_lppend = px_nam_lp_queued();
    a_lpstop = 0;
    a_lppost = 0;
    fm1_cpu1_sync();
    const int job = fm1_cpu1_run(lp_job, prev);
    if (!job) { while (px_nam_lp_b(&g_eng)) { } post_job(prev); a_lpc1 = 0; }   /* (CPU1 off: here) */
    if (run) {
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_part(&g_eng, a_in + o, a_nb[a_nbi] + o, PX_MAX_BLOCK, PX_PART_PRE);
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_part(&g_eng, a_nb[a_nbi] + o, a_nb[a_nbi] + o, PX_MAX_BLOCK, PX_PART_NAM);
        if (!job) while (px_nam_lp_b(&g_eng)) { }
    }
    a_lpc0 = run && job ? fm1_ticks() - t0 : 0u;
    fm1_cpu1_sync();
    a_lpstop = 1;
    fm1_cpu1_sync();
    if (job) a_job = 1;                                /* (collected by the next a_dec or a_enc, or before a recompute) */
    a_nbv = (uint8_t)run;
    a_nbi = (uint8_t)prev;
}
static void dsp_pipe(uint32_t t_irq)
{
    uint32_t t0 = fm1_ticks();
    if (g_eng.nam_lp) {
        const uint32_t c1 = a_lpc1;                    /* (CPU1's last job, collected by this a_enc) */
        dsp_lp_run(!a_bypass && !a_hold);
        const uint32_t c0 = fm1_ticks() - t_irq + a_decw;
        t0 = fm1_ticks() - (c1 > c0 ? c1 : c0);         /* the busier core */
        for (int k = 0; k < 2; k++) {                   /* each core's own (PING's profile) */
            const uint32_t pc = (k ? c1 : c0) * 100u / A_BUDGET;
            a_cl[k] = (a_cl[k] * 7u + pc) / 8u;
            if (pc > a_cpk[k]) a_cpk[k] = pc;
        }
    } else dsp_pipe_run(!a_bypass && !a_hold);
    dsp_load(t0, 93u);                                 /* (CPU0's whole block here, the UI gets the rest) */
}
#endif
static void a_collect(void)                            /* collects the block in progress on CPU1 */
{
#if DSP_CPU1
    if (a_job) {
        a_job = 0;
        if (fm1_cpu1_wait() < 0) a_c1_off();           /* dropped: CPU0 takes over (this block is lost) */
    }
#endif
}

/* ---- tuner: the interrupt stores the input decimated by 4 (11025 Hz); the main loop estimates the note ---- */
#define TN_N 1024u
#if OP_BLE_TEST   /* the Bluetooth test image: the buffers in the cache RAM (app.ld CRAM); without it (pxc_init failed) no tuner */
#define TN_MEM __attribute__((section(".cram.tuner")))
#define TN_READY (pxc_status == PXC_READY)
#else
#define TN_MEM
#define TN_READY 1
#endif
static float tn_buf[TN_N] TN_MEM;
static volatile uint32_t tn_w;
static void tn_capture(const float *x, uint32_t n)
{
    if (!TN_READY) return;
    uint32_t w = tn_w;
    for (uint32_t i = 0; i + 3u < n; i += 4u) {
        tn_buf[w] = (x[i] + x[i + 1u] + x[i + 2u] + x[i + 3u]) * 0.25f;
        w = (w + 1u) & (TN_N - 1u);
    }
    tn_w = w;
}
#define TN_W 600u
#define TN_TMAX 180u
static float tn_lin[TN_W + TN_TMAX] TN_MEM;   /* the newest W + TMAX samples in order: all the difference function reads */
static float tn_d[200] TN_MEM;
static float tuner_estimate(void)      /* frequency in Hz, or 0 (silence / unclear). YIN algorithm. */
{
    const uint32_t W = TN_W, TMIN = 10u, TMAX = TN_TMAX;  /* 61 Hz .. 1100 Hz at 11025 Hz */
    uint32_t w = tn_w;
    float e = 0.0f;
    if (!TN_READY) return 0.0f;
    for (uint32_t i = 0; i < TN_N; i++) {              /* the energy over all TN_N samples, the copy of the newest */
        const float v = tn_buf[(w + i) & (TN_N - 1u)];
        e += v * v;
        if (i >= TN_N - W - TMAX) tn_lin[i - (TN_N - W - TMAX)] = v;
    }
    if (e < (float)TN_N * 1.0e-6f) return 0.0f;        /* too weak: nothing to tune */
    const float *x = tn_lin;
    float run = 0.0f;
    tn_d[0] = 1.0f;
    for (uint32_t t = 1; t <= TMAX; t++) {
        float d = 0.0f;
        for (uint32_t i = 0; i < W; i++) { float v = x[i] - x[i + t]; d += v * v; }
        run += d;
        tn_d[t] = (run > 0.0f) ? d * (float)t / run : 1.0f;
    }
    uint32_t best = 0;
    for (uint32_t t = TMIN; t < TMAX; t++)
        if (tn_d[t] < 0.15f) { while (t + 1u < TMAX && tn_d[t + 1u] < tn_d[t]) t++; best = t; break; }
    if (!best) {
        float m = 0.35f;
        for (uint32_t t = TMIN; t < TMAX; t++) if (tn_d[t] < m) { m = tn_d[t]; best = t; }
        if (!best) return 0.0f;
    }
    float a = tn_d[best - 1u], b = tn_d[best], c = tn_d[best + 1u], den = a - 2.0f * b + c;
    float tf = (float)best + ((den != 0.0f) ? 0.5f * (a - c) / den : 0.0f);
    return 11025.0f / tf;
}

/* v40.2: OP_TESTSIG 1 (measurements only, never a release): a synthetic guitar replaces the input, so the effects that
 * work only while something sounds (envelopes, pitch tracking, the synth's keys, tails) run as with a player: a note and
 * its fifth, sawtooth, plucked every 0.4 s at about -12 dBFS and decaying, the notes walking two octaves up and down */
#ifndef OP_TESTSIG
#define OP_TESTSIG 0
#endif
#if OP_TESTSIG
static void a_testsig(void)
{
    static const float HZ[8] = {82.4f, 110.0f, 146.8f, 196.0f, 246.9f, 329.6f, 196.0f, 110.0f};
    static float ph[2], env;
    static uint32_t n, k;
    for (uint32_t i = 0; i < A_FR; i++, n++) {
        if (n >= 17640u) { n = 0; k = (k + 1u) & 7u; env = 0.25f; }   /* a pluck every 0.4 s */
        const float f = HZ[k] / 44100.0f;
        ph[0] += f; if (ph[0] >= 1.0f) ph[0] -= 1.0f;
        ph[1] += f * 1.4983f; if (ph[1] >= 1.0f) ph[1] -= 1.0f;
        a_in[i] = env * ((2.0f * ph[0] - 1.0f) + 0.6f * (2.0f * ph[1] - 1.0f));
        env *= 0.99995f;
    }
}
#endif
static void a_enc(void *priv, uint8_t *data, int len, uint8_t ch)     /* guitar block received */
{
    (void)priv;
    if (ch != 1u) return;
#if DSP_CPU1
    const uint32_t t_irq = fm1_ticks();
#endif
    a_collect();                                       /* a_in must no longer be read by CPU1 */
#if DSP_CPU1
    if (g_eng.wl_on) px_wet_swap(&g_eng);              /* v39: nothing runs: the lanes' buffers change roles */
#endif
    const uint32_t *s = (const uint32_t *)data;
    uint32_t n = (uint32_t)len / 8u;                   /* 2 32-bit words per frame */
    if (n > A_FR) n = A_FR;
    float sl = 0.0f, sr = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float l = (float)((int32_t)(s[2u * i] << 8) >> 8), r = (float)((int32_t)(s[2u * i + 1u] << 8) >> 8);
        sl += l * l; sr += r * r;
        a_in[i] = l * (0.5f / 8388608.0f);             /* guitar = L channel only (like the official fw); R carries
                                                          only noise (~ -77 dBFS, measured v26). x0.5: same level
                                                          as the old (L + R) / 2, settings are unchanged */
    }
    for (uint32_t i = n; i < A_FR; i++) a_in[i] = 0.0f;
    if (n) {                                           /* input levels L / R (tuner screen) */
        a_lvl[0] += (sl / (float)n - a_lvl[0]) * 0.0625f;
        a_lvl[1] += (sr / (float)n - a_lvl[1]) * 0.0625f;
    }
#if OP_TESTSIG
    a_testsig();
#endif
    tn_capture(a_in, A_FR);
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
static void a_dec(void *priv, uint8_t *data, int len, uint8_t ch)     /* output block to fill */
{
    (void)priv; (void)ch;
#if DSP_CPU1
    a_decw = 0;
    if (a_job && g_eng.nam_lp) {                       /* v40.2: the layer split: only its output (the job goes on) */
        const uint32_t t = fm1_ticks();
        while (!a_lppost && fm1_cpu1_busy()) fm1_cpu1_sync();
        if (!a_lppost) a_collect();                    /* (its job ended without it: dropped) */
        a_decw = fm1_ticks() - t;
    } else
#endif
    a_collect();                                       /* the previous block must be finished */
    int32_t *d = (int32_t *)data;
    uint32_t n = (uint32_t)len / 8u;
    if (n > A_FR) n = A_FR;
    for (uint32_t i = 0; i < n; i++) d[2u * i] = d[2u * i + 1u] = a_res[i];
}
void fm1_alnk0_irq(void) { iis_irq_handler(0); }
extern void isr_alnk0(void);

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
    /* anything else: the output fades out, the engine is set (the interrupt held off meanwhile: a late block now only
     * repeats silence), and the output fades back in */
    if (live) { a_xft = 0; for (uint32_t t = 0; a_xf != 0.0f && t < 400u; t++) fm1_delay_us(100); }   /* (40 ms at most) */
    if (a_running) fm1_irq_mask(FM1_IRQ_ALNK0);
    a_collect();                                       /* CPU1 must not process during the recompute */
#if DSP_CPU1
    while (px_nam_lp_b(&g_eng)) { }                    /* v40.2: the layer split's blocks in flight, finished */
#endif
    px_engine_set(&g_eng, &pp);
#if DSP_CPU1
    px_nam_lp(&g_eng, a_c1 && OP_NAM_LP && px_nam_dual(&g_eng));
#endif
    a_kpend = 0;
#if DSP_CPU1
    a_nbv = 0;                                         /* (the pipeline restarts from silence) */
#endif
    a_bypass = 0; a_over = 0; a_peak = 0; a_cpk[0] = a_cpk[1] = 0;
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
static void audio_volume(uint32_t vol)                 /* 0..100 -> gain (quadratic curve, ~logarithmic) */
{
    float v = (float)vol * 0.01f;
    a_gain = v * v;
}


/* ===================== boot benchmark screen (6 s): CPU/FPU speed and engine cost ===================== */
static void b_num(char *d, uint32_t v)
{
    char t[10]; int n = 0;
    while (*d) d++;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 9);
    while (n) *d++ = t[--n];
    *d = 0;
}
static void b_str(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }
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
static void b_fix2(char *d, uint32_t v100)           /* 123 -> "1.23" */
{
    b_num(d, v100 / 100u);
    b_str(d, ".");
    if (v100 % 100u < 10u) b_str(d, "0");
    b_num(d, v100 % 100u);
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

static struct iis_platform_data a_pd;
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
    px_wet_mode(&g_eng, a_c1 && OP_WET_LANES, A_WLAG);   /* v39: the wet lanes, with the 2nd core */
#endif
    audio_volume(master_vol);
    audio_apply();
    op_stage(ST_I2S, "AUDIO START...");
    a_pd.channel_in = 1u << 1;                         /* DAT1 = PA8: guitar */
    a_pd.channel_out = 1u << 0;                        /* DAT0 = PA7: output */
    a_pd.port_sel = IIS_PORTA;
    a_pd.data_width = (1u << 1) | (1u << 0);          /* 24-bit on both channels, like the official fw */
    a_pd.mclk_output = 1; a_pd.slave_mode = 0;
    a_pd.dump_points_num = 0; a_pd.sr_points = 2u * A_FR;   /* LEN in words: 2 per frame in 24-bit */
    iis_set_enc_data_handler(0, a_enc, 0);
    iis_set_dec_data_handler(0, a_dec, 0);
    iis_open(&a_pd, 0);
    iis_set_sample_rate(44100, 0);
    iis_channel_on((uint8_t)(a_pd.channel_in | a_pd.channel_out), 0);
    fm1_irq_attach(FM1_IRQ_ALNK0, isr_alnk0, 3);
    a_running = 1;
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    gpio_direction_output(G_AMP_EN, 1);                /* amp ON (after I2S: no "pop") */
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
/* call from the main loop: applies preset changes (40 ms debounce) */
static void audio_poll(uint32_t now)
{
    audio_prof(now);
    static uint32_t last_h, t_change, t_apply, vol_shown = 1000u;
    static int pending;
    if (a_nan) audio_reset();
    uint32_t h = ui_hash();
    if (h != last_h) { last_h = h; t_change = now; pending = 1; }
    /* 40 ms after the last change; v38: while a knob turns, every 40 ms too (it glides: no need to wait for the end) */
    if (pending && (now - t_change >= 40u ||
                    (now - t_apply >= 40u && !a_kpend && px_engine_change(&g_eng, &PRESET) == PX_CH_KNOBS))) {
        pending = 0; t_apply = now; audio_apply();
    }
    if (master_vol != vol_shown) { vol_shown = master_vol; audio_volume(master_vol); }
}
