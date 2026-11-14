/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — host test of the base firmware's audio stages (firmware/base/audio.h) under the plain schedule
 * (firmware/base/audio_plain.h), as an app on it uses them (firmware/apps/starter): the I2S driver's two callbacks
 * with a guitar block and an output block to fill, the app's sound between a_in and a_out.
 * The pedal's own code runs here; only the chip is stood in for (the tick counter, the I2S driver's entry points).
 * audio_io_start is not called: it writes the chip's registers.
 * Built twice (tests/run_tests.sh): as it is, for an app with a mono sound, and with -DA_CH=2, for an app whose sound
 * has a left and a right block (the starter app, firmware/apps/starter). The stereo app here makes its right side a
 * quarter of its left, so a stage that forgot a side, or mixed the two, shows.
 *   cc -O1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-int-to-pointer-cast -Ifirmware [-DA_CH=2] \
 *      tests/test_base_audio.c firmware/px_math.c -lm -o build/test_base_audio
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- the chip, stood in for ---- */
#define FM1_TICKS_PER_US 24u
static uint32_t now;
static uint32_t fm1_ticks(void) { return now; }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
static void fm1_irq_attach(int irq, void (*isr)(void), int prio) { (void)irq; (void)isr; (void)prio; }
static void fm1_irq_enable_all(void) { }
#define FM1_IRQ_ALNK0 0
static int t_held, t_held_max;                  /* the audio's interrupt held off (a_nan_poll, a_change) */
static void fm1_irq_mask(int irq) { (void)irq; if (++t_held > t_held_max) t_held_max = t_held; }
static void fm1_irq_unmask(int irq) { (void)irq; t_held--; }
static void fm1_wdt_feed(void);                 /* while the main loop waits, the interrupt goes on: below */
void isr_alnk0(void) { }
enum { ST_I2S };
static void op_stage(uint32_t s, const char *txt) { (void)s; (void)txt; }
/* base/audio.h defines the C library functions the I2S driver calls: under other names here, the PC has its own */
#define printf jl_printf
#define malloc jl_malloc
#define free jl_free
#include "base/audio.h"
int  iis_open(struct iis_platform_data *pd, uint8_t index) { (void)pd; (void)index; return 0; }
int  iis_set_sample_rate(int sample_rate, uint8_t index) { (void)sample_rate; (void)index; return 0; }
void iis_channel_on(uint8_t channel, uint8_t index) { (void)channel; (void)index; }
void iis_set_enc_data_handler(void *priv, void (*cb)(void *, uint8_t *, int, uint8_t), uint8_t index) { (void)priv; (void)cb; (void)index; }
void iis_set_dec_data_handler(void *priv, void (*cb)(void *, uint8_t *, int, uint8_t), uint8_t index) { (void)priv; (void)cb; (void)index; }
void iis_irq_handler(uint8_t index) { (void)index; }

/* ---- the app: its sound is the guitar times t_gain; it takes t_cost ticks, and says so ---- */
static float t_gain = 1.0f;
static uint32_t t_cost, t_calls;
static int t_nan;
#if A_CH == 2
#define T_RIGHT 0.25f                           /* the app's right side, as a part of its left */
#else
#define T_RIGHT 1.0f                            /* a mono sound: both outputs alike */
#endif
static void app_sound(const float *in, float *out)
{
    for (uint32_t i = 0; i < A_FR; i++) out[i] = in[i] * t_gain;
#if A_CH == 2
    for (uint32_t i = 0; i < A_FR; i++) out[A_FR + i] = in[i] * t_gain * T_RIGHT;
#endif
    if (t_nan) out[A_OUT_N - A_FR / 2u] = 0.0f / 0.0f * in[0];   /* (in the right block, when there is one) */
    now += t_cost;
    t_calls++;
}
static uint32_t t_resets;
static void app_sound_reset(void) { t_resets++; if (t_held != 1) t_resets += 1000u; }   /* (it must run with the interrupt held) */
#include "base/audio_plain.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { fprintf(stdout, "FAIL %s\n", what); fails++; } } while (0)

static uint32_t in[2 * A_FR];                   /* the I2S's blocks: 2 words a frame, 24 bits right-aligned */
static int32_t out[2 * A_FR];
static uint32_t ph;
/* one interrupt: a guitar block (a sine of `amp` 24-bit steps, 441 Hz, on the left; noise on the right), its output */
static void irq(int32_t amp)
{
    for (uint32_t i = 0; i < A_FR; i++, ph++) {
        in[2u * i] = (uint32_t)(int32_t)((float)amp * sinf(6.2831853f * (float)(ph % 100u) / 100.0f)) & 0x00FFFFFFu;
        in[2u * i + 1u] = (ph * 2654435761u >> 20) & 0xFFu;
    }
    a_enc(0, (uint8_t *)in, (int)sizeof in, 1);
    now += A_BUDGET;
    a_dec(0, (uint8_t *)out, (int)sizeof out, 0);
}
static int32_t in24(uint32_t i) { return (int32_t)(in[2u * i] << 8) >> 8; }
static uint32_t t_feeds;
static void fm1_wdt_feed(void) { t_feeds++; if (!t_held) irq(1000000); }
/* every output frame is the guitar's left sample times k on the left and times kr on the right, to within `tol` steps */
static int follows2(float k, float kr, int32_t tol)
{
    for (uint32_t i = 0; i < A_FR; i++) {
        const int32_t d = out[2u * i] - (int32_t)((float)in24(i) * k), dr = out[2u * i + 1u] - (int32_t)((float)in24(i) * kr);
        if (d > tol || d < -tol || dr > tol || dr < -tol) return 0;
    }
    return 1;
}
static int follows(float k, int32_t tol) { return follows2(k, k * T_RIGHT, tol); }   /* the app's sound: its two sides */
static int straight(float k, int32_t tol) { return follows2(k, k, tol); }            /* the guitar itself, on both outputs */
/* the right output is the left's times T_RIGHT in every frame (a fade or a mute has the same gain on both sides) */
static int sides_agree(int32_t tol)
{
    for (uint32_t i = 0; i < A_FR; i++) {
        const int32_t d = out[2u * i + 1u] - (int32_t)((float)out[2u * i] * T_RIGHT);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}
static int silent(void) { for (uint32_t i = 0; i < 2u * A_FR; i++) if (out[i]) return 0; return 1; }
static int32_t peak(void) { int32_t m = 0; for (uint32_t i = 0; i < A_FR; i++) { int32_t v = out[2u * i]; if (v < 0) v = -v; if (v > m) m = v; } return m; }

void t_change(void) { app_sound_reset(); if (a_running && a_xf != 0.0f) t_resets += 1000u; }   /* (the output must be silent by now) */

int main(void)
{
    char msg[160];
    uint32_t n;

    /* the guitar through the app at MASTER 100: a_input halves it (the level every app's sound starts from) */
    audio_volume(100u);
    for (n = 0; n < 40u; n++) irq(1000000);         /* (the volume glides to its place) */
    CHECK(follows(0.5f, 2), "MASTER 100: the output is the app's sound, half the input's steps, on both channels");
    CHECK(t_calls == 40u && a_blocks == 40u, "one call of the app's sound for each guitar block");
    CHECK(a_load == 0u && !a_bypass && !a_clip && !a_xrun, "no load, no bypass, no clip, no late block");
    audio_volume(50u);
    for (n = 0; n < 40u; n++) irq(1000000);
    CHECK(follows(0.125f, 2), "MASTER 50: a quarter of that (the quadratic curve)");
    audio_volume(0u);
    for (n = 0; n < 40u; n++) irq(1000000);
    CHECK(peak() <= 1, "MASTER 0: silence");
    audio_volume(100u);
    for (n = 0; n < 40u; n++) irq(1000000);

    /* the start and safe mode: the app's sound is held off, the guitar passes */
    a_hold = 1; t_gain = 0.0f; n = t_calls;
    irq(1000000);
    CHECK(t_calls == n && straight(0.5f, 2), "a_hold: the app's sound is not called, the guitar goes straight out, on both outputs");
    a_hold = 0; t_gain = 1.0f;

    /* the output's soft knee: a sound above full scale never reaches it, and is counted */
    t_gain = 8.0f;
    for (n = 0; n < 4u; n++) irq(4000000);
    CHECK(peak() > 6710886 && peak() < 8388607 && a_clip >= 4u, "a sound over full scale: bent under it by the soft knee, the clip counted");
#if A_CH == 2
    {   /* the left side is at 1.9 of full scale, the right at 0.48: under the knee, so it comes out as it is */
        int ok = 1;
        for (uint32_t i = 0; i < A_FR; i++) { const int32_t d = out[2u * i + 1u] - in24(i); if (d > 16 || d < -16) ok = 0; }
        CHECK(ok, "the knee bends each side by its own level: the right side, under it, is untouched");
    }
    t_gain = 40.0f;                                  /* now the right side alone would be over full scale too */
    for (n = 0; n < 4u; n++) irq(4000000);
    {
        int32_t m = 0;
        for (uint32_t i = 0; i < A_FR; i++) { int32_t v = out[2u * i + 1u]; if (v < 0) v = -v; if (v > m) m = v; }
        CHECK(m > 6710886 && m < 8388607, "a right side over full scale is bent under it too");
    }
#endif
    t_gain = 1.0f; a_clip = 0;

    /* a flash write mutes; the tuner's mute fades */
    a_mute = 1; irq(1000000);
    CHECK(silent(), "a_mute: zeros");
    a_mute = 0; irq(1000000);
    CHECK(follows(0.5f, 2), "and the sound is back with the next block");
    a_tmute = 1; irq(1000000);
    CHECK(!silent() && sides_agree(2), "the tuner's mute: a ramp, the same on both sides");
    for (n = 0; n < 19u; n++) irq(1000000);
    CHECK(silent(), "and silence after the ramp");
    a_tmute = 0;
    for (n = 0; n < 20u; n++) irq(1000000);
    CHECK(follows(0.5f, 2), "and back after its ramp");

    /* a change of the sound (a type, an order): the app fades the output out, changes, fades it back in */
    a_xft = 0; irq(1000000);
    CHECK(!silent() && sides_agree(2) && out[0] != 0 && out[2u * (A_FR - 1u)] != 0, "the change fade: down over more than one block, the same on both sides");
    irq(1000000); irq(1000000);
    CHECK(silent() && a_xf == 0.0f, "silence from the third block on (256 samples down)");
    a_xft = 1; irq(1000000);
    CHECK(!silent() && sides_agree(2), "and up again, the same on both sides");
    irq(1000000); irq(1000000);
    CHECK(follows(0.5f, 2) && a_xf == 1.0f, "the sound is whole again");

    /* a NaN from the app: silence until the main loop has reset the app's sound; then it fades back in */
    a_nan_poll();
    CHECK(!t_resets && !t_held_max, "no NaN: the main loop's check does nothing");
    t_nan = 1; irq(1000000);
    CHECK(silent() && a_nan, "a NaN in the app's sound: the block is silence, a_nan is set");
    t_nan = 0; irq(1000000);
    CHECK(silent(), "and silence it stays, though the app's sound is sound again: its state may hold the NaN");
    a_nan_poll();
    CHECK(t_resets == 1u && !a_nan && a_nan_count == 1u && t_held == 0 && t_held_max == 1, "the main loop's check: the app's sound reset with the interrupt held, the NaN counted, the flag cleared");
    irq(1000000);
    CHECK(!silent() && sides_agree(2) && !follows(0.5f, 2), "the sound fades back in, the same on both sides");
    irq(1000000); irq(1000000);
    CHECK(follows(0.5f, 2), "and is whole again");

    /* a change that must not be heard (another effect, a cleared buffer): silence first, the change, then the sound again */
    {
        extern void t_change(void);
        a_running = 1; t_feeds = 0; t_held_max = 0;
        a_change(t_change);
        CHECK(t_resets == 2u && t_held == 0 && t_held_max == 1, "a_change runs the change once the output is silent, with the interrupt held, and releases it");
        CHECK(t_feeds >= 2u && t_feeds <= 4u && a_xft == 1, "it waited for the fade (256 samples: 2 blocks), feeding the watchdog; the sound is asked back");
        irq(1000000);
        CHECK(!silent() && !follows(0.5f, 2), "the first block after the change fades in from silence");
        irq(1000000); irq(1000000);
        CHECK(follows(0.5f, 2), "the sound is whole again");
        a_running = 0; t_feeds = 0;
        a_change(t_change);
        CHECK(t_resets == 3u && t_feeds == 0u, "before the audio runs (the start) the change is made at once: no waiting for a fade nobody makes");
        a_xf = 1.0f;
    }

    /* the load: 50 % reads 50; over 85 % for 8 blocks running the app's sound is bypassed; 100 % is a late block */
    t_cost = A_BUDGET / 2u;
    for (n = 0; n < 64u; n++) irq(1000000);
    snprintf(msg, sizeof msg, "an app that takes half a block: the load reads 50 %% (it reads %u)", (unsigned)a_load);
    CHECK(a_load >= 49u && a_load <= 50u && !a_bypass, msg);
    t_cost = A_BUDGET * 90u / 100u;
    for (n = 0; n < 7u; n++) irq(1000000);
    CHECK(!a_bypass, "7 blocks at 90 %: not yet bypassed");
    irq(1000000); n = t_calls; irq(1000000);
    CHECK(a_bypass && t_calls == n && straight(0.5f, 2), "the 8th: bypassed, the guitar goes straight out on both outputs, the app's sound is no longer called");
    CHECK(!a_xrun, "90 % is not a late block");
    a_bypass = 0; a_over = 0; t_cost = A_BUDGET + A_BUDGET / 10u;
    irq(1000000);
    CHECK(a_xrun == 1u, "110 %: a late block, counted");
    t_cost = 0;

    /* the input levels and the tuner's capture: a 441 Hz sine at a tenth of full scale */
    a_bypass = 0; a_over = 0;
    for (n = 0; n < 200u; n++) irq(838861);
    snprintf(msg, sizeof msg, "the left input level reads -23 dB (a sine at -20 dB of full scale: it reads %d)", a_level_db(0));
    CHECK(a_level_db(0) >= -24 && a_level_db(0) <= -22, msg);
    CHECK(a_level_db(1) < -80, "the right input (noise) reads far below");
    {
        const float hz = tuner_estimate();
        snprintf(msg, sizeof msg, "the tuner's estimate of a 441 Hz guitar: %d.%d Hz", (int)hz, (int)(hz * 10.0f) % 10);
        CHECK(hz > 440.5f && hz < 441.5f, msg);
    }
    fprintf(stdout, fails ? "base audio, %u channel(s): %d FAILURES\n" : "base audio, %u channel(s): ok (0 failures)\n", (unsigned)A_CH, fails);
    return fails ? 1 : 0;
}
