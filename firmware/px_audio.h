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
#define BENCH_AT_BOOT 0                                 /* 1 = DSP benchmark screen at boot */
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
static volatile uint8_t a_bypass, a_over, a_hold;   /* a_hold: DSP held off (boot, safe mode) */
static volatile uint8_t a_mute;                      /* output zeros (a flash write is coming) */
static volatile uint8_t a_tmute;                     /* v36: tuner open: the output fades to silence (5 ms) */
static float a_mg = 1.0f;
static volatile uint8_t a_nan;                         /* engine output a NaN/Inf: needs a reset */
static volatile uint32_t a_nan_count;
static int a_running;

/* DSP work for one block: a_in -> a_res (gain, clipping) + load measurement. Runs on the 2nd core (CPU1)
 * when available, otherwise on CPU0 in the interrupt. */
static void dsp_block(void)
{
    uint32_t t0 = fm1_ticks();
    if (!a_bypass && !a_hold) {
        for (uint32_t o = 0; o < A_FR; o += PX_MAX_BLOCK)
            px_engine_process(&g_eng, a_in + o, a_out + o, PX_MAX_BLOCK);
    } else {
        for (uint32_t i = 0; i < A_FR; i++) a_out[i] = a_in[i];
    }
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
    float g = a_gain * 8388607.0f;
    for (uint32_t i = 0; i < A_FR; i++) {
        float v = a_out[i] * g;
        if (v > 8388607.0f) v = 8388607.0f; else if (v < -8388608.0f) v = -8388608.0f;
        a_res[i] = (int32_t)v;
    }
    uint32_t pct = (fm1_ticks() - t0) * 100u / A_BUDGET;
    a_load = (a_load * 7u + pct) / 8u;
    if (pct > a_peak) a_peak = pct;
    if (!a_bypass && !a_hold) {
        if (pct > 85u) { if (++a_over >= 8u) a_bypass = 1; } else a_over = 0;
    }
    a_blocks++;
}

/* --- second core: block N goes to CPU1 at the end of IRQ N; IRQ N+1 collects the result before
 * sending it out. Same latency as before (processing already had to finish before the next block), but CPU0
 * is free between IRQs: display and UI never touch the audio again. Automatic fallback to CPU0
 * if the 2nd core rejects a job or misbehaves (FM1_CPU1_FALLBACK 1, Felucca's fm1_cpu1.h). */
static volatile uint8_t a_c1;                          /* 1 = DSP runs on the 2nd core */
static uint8_t a_job;                                  /* a block is in progress on CPU1 */
#if DSP_CPU1
static void dsp_job(uint32_t arg) { (void)arg; dsp_block(); }
#endif
static void a_collect(void)                            /* collects the block in progress on CPU1 */
{
#if DSP_CPU1
    if (a_job) {
        a_job = 0;
        if (fm1_cpu1_wait() < 0) a_c1 = 0;             /* dropped: CPU0 takes over (this block is lost) */
    }
#endif
}

/* ---- tuner: the interrupt stores the input decimated by 4 (11025 Hz); the main loop estimates the note ---- */
#define TN_N 1024u
static float tn_buf[TN_N];
static volatile uint32_t tn_w;
static void tn_capture(const float *x, uint32_t n)
{
    uint32_t w = tn_w;
    for (uint32_t i = 0; i + 3u < n; i += 4u) {
        tn_buf[w] = (x[i] + x[i + 1u] + x[i + 2u] + x[i + 3u]) * 0.25f;
        w = (w + 1u) & (TN_N - 1u);
    }
    tn_w = w;
}
static float tn_lin[TN_N];
static float tn_d[200];
static float tuner_estimate(void)      /* frequency in Hz, or 0 (silence / unclear). YIN algorithm. */
{
    const uint32_t W = 600u, TMIN = 10u, TMAX = 180u;  /* 61 Hz .. 1100 Hz at 11025 Hz */
    uint32_t w = tn_w;
    float e = 0.0f;
    for (uint32_t i = 0; i < TN_N; i++) { tn_lin[i] = tn_buf[(w + i) & (TN_N - 1u)]; e += tn_lin[i] * tn_lin[i]; }
    if (e < (float)TN_N * 1.0e-6f) return 0.0f;        /* too weak: nothing to tune */
    const float *x = tn_lin + (TN_N - W - TMAX);
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

static void a_enc(void *priv, uint8_t *data, int len, uint8_t ch)     /* guitar block received */
{
    (void)priv;
    if (ch != 1u) return;
    a_collect();                                       /* a_in must no longer be read by CPU1 */
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
    tn_capture(a_in, A_FR);
#if DSP_CPU1
    if (a_c1 && fm1_cpu1_run(dsp_job, 0)) { a_job = 1; return; }
    a_c1 = 0;
#endif
    dsp_block();
}
static void a_dec(void *priv, uint8_t *data, int len, uint8_t ch)     /* output block to fill */
{
    (void)priv; (void)ch;
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
static void audio_apply(void)                          /* coefficient recompute: outside the interrupt */
{
    px_preset_t pp;
    ui_to_px(&pp);
    if (a_running) fm1_irq_mask(FM1_IRQ_ALNK0);
    a_collect();                                       /* CPU1 must not process during the recompute */
    px_engine_set(&g_eng, &pp);
    a_bypass = 0; a_over = 0; a_peak = 0;
    if (a_running) fm1_irq_unmask(FM1_IRQ_ALNK0);
}
static void audio_reset(void)                          /* after a NaN: engine reset, preset re-applied */
{
    px_preset_t pp;
    ui_to_px(&pp);
    if (a_running) fm1_irq_mask(FM1_IRQ_ALNK0);
    a_collect();
    px_engine_init(&g_eng, 44100.0f);
    px_engine_set(&g_eng, &pp);
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
static void audio_bench(void)
{
    char l[48];
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
    l[0] = 0; b_str(l, "INT   100K LOOPS: "); b_num(l, ti / 24u); b_str(l, " US");
    lcd_text(4, 30, l, UI_TXT, UI_BG, 1);
    l[0] = 0; b_str(l, "FLOAT 100K LOOPS: "); b_num(l, tf / 24u); b_str(l, " US");
    lcd_text(4, 42, l, UI_TXT, UI_BG, 1);
    /* engine cost, block by block */
    px_preset_t pp;
    static const char *NAMES[4] = {"AMP ONLY     ", "+ CAB        ", "+ DRIVE/GATE ", "ALL ON       "};
    for (int c = 0; c < 4; c++) {
        px_preset_factory(&pp, 17);                    /* 5150 RHYTHM: 4-stage 5150 */
        for (int z = 0; z < PX_NBLK; z++) pp.on[z] = 0;
        pp.on[PX_B_AMP] = 1;
        if (c >= 1) pp.on[PX_B_CAB] = 1;
        if (c >= 2) { pp.on[PX_B_DRIVE] = 1; pp.on[PX_B_GATE] = 1; }
        if (c >= 3) { for (int z = 0; z < PX_NBLK; z++) pp.on[z] = 1; pp.type[PX_B_MOD] = PX_MOD_ROTARY; pp.type[PX_B_PITCH] = PX_PIT_POLYOCT;
                        pp.type[PX_B_REVERB] = PX_REV_SHIMMER; pp.type[PX_B_DELAY] = PX_DLY_SPACE; }
        uint32_t pct = b_engine(&pp);
        l[0] = 0; b_str(l, NAMES[c]); b_num(l, pct); b_str(l, " %");
        lcd_text(4, 64u + (uint32_t)c * 16u, l, pct <= 85u ? GRN : RED, UI_BG, 2);
    }
    lcd_text(4, 140, "100% = REAL TIME (2.9 MS / BLOCK)", UI_TXT, UI_BG, 1);
    lcd_text(4, 160, "PLEASE TAKE A PHOTO OF THIS SCREEN", UI_ACC, UI_BG, 1);
    for (int i = 0; i < 60; i++) { fm1_wdt_feed(); fm1_delay_ms(100); }
}

static struct iis_platform_data a_pd;
static void audio_start(void)
{
#if BENCH_AT_BOOT
    px_engine_init(&g_eng, 44100.0f);
    audio_bench();                                     /* benchmark screen (diagnostic): off by default */
#endif
    op_stage(ST_ENG_INIT, "ENGINE INIT...");
    px_engine_init(&g_eng, 44100.0f);
    op_stage(ST_ENG_SET, "ENGINE SET...");
#if DSP_CPU1
    a_c1 = fm1_cpu1_on;                                /* started by app_main at the very start (fm1_cpu1_start) */
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

/* call from the main loop: applies preset changes (40 ms debounce) */
static void audio_poll(uint32_t now)
{
    static uint32_t last_h, t_change, vol_shown = 1000u;
    static int pending;
    if (a_nan) audio_reset();
    uint32_t h = ui_hash();
    if (h != last_h) { last_h = h; t_change = now; pending = 1; }
    if (pending && now - t_change >= 40u) { pending = 0; audio_apply(); }
    if (master_vol != vol_shown) { vol_shown = master_vol; audio_volume(master_vol); }
}
