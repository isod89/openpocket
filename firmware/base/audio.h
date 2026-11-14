/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the audio's way in and out, whatever the app does with the sound.
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
 * A block is 128 frames (2.9 ms), in float on the hardware FPU (-mcpu=r3 -mfprev1). Its stages here:
 *   a_input   the I2S's guitar block -> a_in (mono), the input levels, the tuner's samples
 *   dsp_out   a_out -> a_res: the NaN guard, the mutes, the change fade, the MASTER volume, the soft knee
 *   dsp_load  the block's load, and the safety bypass when it stays above the limit 8 blocks running
 *   a_output  a_res -> the I2S's output block, both channels; USB audio
 * The guitar is mono (the converter's right input carries noise only). The app's sound is mono too, and then goes to
 * both outputs alike, unless the app's file defines A_CH 2 before this one: a_out and a_res then hold the left block
 * followed by the right block, and every stage here treats the two alike.
 * Between a_in and a_out is the app's sound. The app's file provides, after this one:
 *   a_block(data, len)  a guitar block arrived: a_input, then its sound made, or started on another core
 *   a_send(d, n)        an output block to fill: that sound finished, then a_output
 *   a_collect()         the sound in progress on another core is finished (before the flash is written, or the
 *                       engine set); empty for an app that makes its sound in a_block
 * base/audio_plain.h provides the three for an app that makes its sound in the interrupt, on one core.
 */
#pragma once
#include <stdint.h>
#include <math.h>

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

#ifndef JL_GPIO
#define JL_GPIO 0x50000u                               /* the ports' registers (a host build has its own: tests/host) */
#endif
static volatile uint32_t *jl_preg(uint32_t gpio, uint32_t off)
{
    uint32_t port = gpio >> 4;
    if (port == 3u || port > 7u) return 0;             /* no port D (flash) */
    if (port == 2u && (gpio & 15u) == 7u) return 0;    /* PC7 = display */
    return (volatile uint32_t *)(JL_GPIO + port * 0x40u + off);
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

/* ===================== the block: its buffers, its figures, its stages ===================== */
#define A_FR     128u                                  /* frames per I2S block */
#define A_BUDGET (A_FR * 1000000u / 44100u * FM1_TICKS_PER_US)   /* ticks available per block (~2.9 ms) */
#define G_AMP_EN 9u                                    /* PA9: SGM8905 amp enable */
#ifndef A_CH
#define A_CH 1u                                        /* the channels of the app's sound: 1, or 2 (left, then right) */
#endif
#define A_OUT_N (A_CH * A_FR)                          /* samples in a_out and a_res: channel c is at c * A_FR */
static float a_in[A_FR], a_out[A_OUT_N];
static int32_t a_res[A_OUT_N];                         /* output, 24-bit right-aligned */
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
/* v40.4: the smoothed loads' sums (the load x 8). Smoothed in whole per cent, (load * 7 + pct) / 8 rounded down at every
 * block: a load reached from below stopped up to 7 short (79 % read 73), one reached from above read right */
static uint32_t a_lds, a_cls[2];
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
/* v40.3: the last full change, for PING (ticks): the last change seen (any), the full one's, the engine's set (how
 * long), the sound back (the end of the fade-in, written by the audio side) */
static volatile uint32_t a_sw0, a_swc, a_swset, a_sw1;
static volatile uint8_t a_swin;                        /* 1 from a full change until its fade-in ends */
static float a_gv = 0.64f;                             /* the volume as it glides */
static volatile uint8_t a_xft = 1;                     /* where the change fade goes: 1 sound, 0 silence */
static volatile uint8_t a_nan;                         /* engine output a NaN/Inf: needs a reset */
/* v42: USB audio (px_usb.h, docs/USB-AUDIO.md). While the USB AUDIO setting is on, each output block goes through
 * px_ua_block in a_send (CPU0, the I2S interrupt): the block to the computer, the computer's playback into the output.
 * a_uat: the ticks it took, part of CPU0's work in a block (0 while the setting is off: the loads are then as before) */
static volatile uint8_t a_ua;
static uint32_t a_uat;
static void px_ua_block(int32_t *d, uint32_t n);
static volatile uint32_t a_nan_count;
static int a_running;
static void a_block(const uint8_t *data, int len);     /* the app's (see above) */
static void a_send(int32_t *d, uint32_t n);
static void a_collect(void);

/* one sample through the output's soft knee (dsp_out says why); *clip is set when the sample reached it */
static inline float a_knee(float v, int *clip)
{
    float m = v < 0.0f ? -v : v;
    if (m > 0.8f) {
        float t = (m - 0.8f) * 5.0f;
        m = 0.8f + 0.2f * t / (1.0f + t);
        v = v < 0.0f ? -m : m;
        *clip = 1;
    }
    return v;
}
static void dsp_out(void)                              /* a_out -> a_res: safety net, mutes, volume (each channel alike) */
{
    /* safety net: a NaN/Inf would poison the filters forever (silence). Output silence and
     * the main loop resets the engine (audio_poll). */
    for (uint32_t i = 0; i < A_OUT_N; i++) {
        float v = a_out[i];
        if (v != v || v > 1.0e6f || v < -1.0e6f) { a_nan = 1; break; }
    }
    if (a_nan) for (uint32_t i = 0; i < A_OUT_N; i++) a_out[i] = 0.0f;
    if (a_mute) for (uint32_t i = 0; i < A_OUT_N; i++) a_out[i] = 0.0f;
    {   /* tuner mute, with a short ramp both ways (no click) */
        float tg = a_tmute ? 0.0f : 1.0f;
        if (a_mg != tg) {
            for (uint32_t i = 0; i < A_FR; i++) {
                a_mg += (tg - a_mg) * 0.005f; a_out[i] *= a_mg;
#if A_CH == 2
                a_out[A_FR + i] *= a_mg;
#endif
            }
            if (tg > 0.5f ? a_mg > 0.999f : a_mg < 0.0005f) a_mg = tg;
        } else if (tg == 0.0f) for (uint32_t i = 0; i < A_OUT_N; i++) a_out[i] = 0.0f;
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
#if A_CH == 2
                a_out[A_FR + i] *= xf;
#endif
            }
            a_xf = xf;
            if (xf == 1.0f && a_swin) { a_sw1 = fm1_ticks(); a_swin = 0; }
        } else if (tg == 0.0f) for (uint32_t i = 0; i < A_OUT_N; i++) a_out[i] = 0.0f;
    }
    const float g = a_gain;                            /* v38: the volume glides there (~3 ms), no zipper */
    float gv = a_gv;
    int clip = 0;
    for (uint32_t i = 0; i < A_FR; i++) {
        gv += (g - gv) * 0.008f;
        a_res[i] = (int32_t)(a_knee(a_out[i] * gv, &clip) * 8388607.0f);
#if A_CH == 2
        a_res[A_FR + i] = (int32_t)(a_knee(a_out[A_FR + i] * gv, &clip) * 8388607.0f);
#endif
    }
    a_gv = gv;
    if (clip) a_clip++;
}
static void dsp_load(uint32_t t0, uint32_t lim)        /* the block's load, the safety bypass (over lim % 8 blocks running) */
{
    uint32_t pct = (fm1_ticks() - t0) * 100u / A_BUDGET;
    a_lds += pct - a_lds / 8u;
    a_load = a_lds / 8u;
    if (pct > a_peak) a_peak = pct;
    if (pct > a_peak_all) a_peak_all = pct;
    if (pct >= 100u && !a_hold) { a_xrun++; a_xrun_at = a_blocks; }
    if (!a_bypass && !a_hold) {
        if (pct > lim) { if (++a_over >= 8u) a_bypass = 1; } else a_over = 0;
    }
    a_blocks++;
}

/* ---- tuner (px_tuner.h): the interrupt stores the input decimated by 4; the main loop estimates the note ---- */
#if OP_BLE_TEST   /* the Bluetooth test image: the buffers in the cache RAM (app.ld CRAM); without it (pxc_init failed) no tuner */
#define TN_MEM __attribute__((section(".cram.tuner")))
#define TN_READY (pxc_status == PXC_READY)
#else
#define TN_MEM
#define TN_READY 1
#endif
#include "px_tuner.h"

/* v40.2: OP_TESTSIG 1 (measurements only, never a release): a synthetic guitar replaces the input, so the effects that
 * work only while something sounds (envelopes, pitch tracking, the synth's keys, tails) run as with a player: a note and
 * its fifth, sawtooth, plucked every 0.4 s at about -12 dBFS and decaying, the notes walking two octaves up and down */
#ifndef OP_TESTSIG
#define OP_TESTSIG 0
#endif
/* OP_UADEV 1 (measurements only, never a release): USB audio's figures for the pedal's timing (px_uaudio.h): when the
 * interrupt's two callbacks run, the USB poll's gaps, the rings' fills; a counter or the computer's playback in the
 * record stream in place of the sound, so a frame lost or repeated on the way shows exactly; a steady tone as the input;
 * the output blocks sent in the order the driver asks, as before v42 */
#ifndef OP_UADEV
#define OP_UADEV 0
#endif
#if OP_UADEV
static uint8_t ua_dev;                                 /* the modes (px_uaudio.h) */
static void uad_enc(uint32_t t);
static void uad_send(void);
static void uad_tone(float *x);
#define A_ORDER_OLD (ua_dev & 8u)
#else
#define A_ORDER_OLD 0u
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
static void a_input(const uint8_t *data, int len)     /* a guitar block from the I2S: a_in, the input levels, the tuner's samples */
{
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
#if OP_UADEV
    uad_tone(a_in);
#endif
    tn_capture(a_in, A_FR);
}
static void a_output(int32_t *d, uint32_t n)           /* a_res to an output block of n frames, both channels; USB audio */
{
    if (n > A_FR) n = A_FR;
#if A_CH == 2
    for (uint32_t i = 0; i < n; i++) { d[2u * i] = a_res[i]; d[2u * i + 1u] = a_res[A_FR + i]; }
#else
    for (uint32_t i = 0; i < n; i++) d[2u * i] = d[2u * i + 1u] = a_res[i];
#endif
#if OP_UAUDIO
    if (a_ua) {                                        /* v42: USB audio */
        const uint32_t t = fm1_ticks();
        px_ua_block(d, n);
        a_uat = fm1_ticks() - t;
    }
#endif
}
#define A_ORDER_TICKS (A_BUDGET / 4u)                  /* an a_enc this soon after an a_dec is the same interrupt's (it comes within microseconds) */
#include "px_aorder.h"                                /* v42: a_enc and a_dec, in whichever order the driver calls them */
void fm1_alnk0_irq(void) { iis_irq_handler(0); }
extern void isr_alnk0(void);

static void audio_volume(uint32_t vol)                 /* 0..100 -> gain (quadratic curve, ~logarithmic) */
{
    float v = (float)vol * 0.01f;
    a_gain = v * v;
}

static struct iis_platform_data a_pd;
static void audio_io_start(void)                       /* the I2S on (its interrupt calls a_enc and a_dec), then the amp */
{
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
