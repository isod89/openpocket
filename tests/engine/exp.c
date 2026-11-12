/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — the expression pedal on the PC (test build 25: MIDI CC 117, engine/px_dsp.c px_exp), with the pedal's libm:
 *   cc -O2 -DPX_OWN_FX=1 -Iengine tests/engine/exp.c engine/px_dsp.c firmware/px_math.c -lm -o build/exp && build/exp
 * (the Bluetooth audio image's own engine, PX_OWN_FX: tests/run_tests.sh builds it as that image builds the engine and with
 * the host's defaults, as tests/engine/pad.c)
 * Checks, through the engine's own preset path (every other block off; px_exp_set between blocks, as the MIDI glue does):
 *   - no position yet: the engine plays as before; the pedal at the toe as the volume: the same signal
 *   - the volume: half way about -18 dB, the heel silent, no step wider than the signal's own (smoothed)
 *   - the volume comes before the delay: closed after a phrase, the repeats ring on; closed before it, nothing
 *   - the hold: a wah turned off with the pedal at the heel leaves the volume at unity until the pedal moves
 *   - the wah: heel dark, toe bright (CENTER's sweep, the preset's CENTER ignored)
 *   - the whammy: heel the note, toe the preset's SHIFT (an octave up), half way about half the interval */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "px_dsp.h"
#if !PX_OWN_FX
#error "the expression pedal is the Bluetooth audio image's: build with -DPX_OWN_FX=1"
#endif

#define SR 44100.0f
#define N (4 * 44100)
#define B 64
static px_engine e;
static float in[N], out[N], ref[N];
static int failed;
static void ok(int c, const char *what) { printf("%-100s %s\n", what, c ? "ok" : "FAIL"); if (!c) failed++; }

static void preset(px_preset_t *p)
{
    px_preset_factory(p, 0);
    for (int b = 0; b < PX_NBLK; b++) p->on[b] = 0;
    p->p[PX_B_GATE][PX_PATCH_LEVEL] = 50;
}
static void sine(float f, float a, float t0, float t1)
{
    memset(in, 0, sizeof in);
    for (int k = (int)(t0 * SR); k < (int)(t1 * SR) && k < N; k++) in[k] = a * sinf(6.2831853f * f * (float)k / SR);
}
/* runs the input through p; at time at[i] the pedal goes to pos[i] (-1: forgotten), at time sw (if > 0) the preset becomes p2 */
static void run(const px_preset_t *p, float *y, const float *at, const int *pos, int np, float sw, const px_preset_t *p2)
{
    px_exp_set(-1);
    px_engine_init(&e, SR);
    px_engine_set(&e, p);
    int pi = 0, swapped = 0;
    for (int k = 0; k < N; k += B) {
        const float t = (float)k / SR;
        while (pi < np && at[pi] <= t) { px_exp_set(pos[pi]); pi++; }
        if (!swapped && sw > 0.0f && sw <= t) { px_engine_set(&e, p2); swapped = 1; }
        px_engine_process(&e, in + k, y + k, N - k < B ? N - k : B);   /* (N is not a multiple of B) */
    }
}
static float rms(const float *y, float t0, float t1)
{
    double a = 0; int k0 = (int)(t0 * SR), k1 = (int)(t1 * SR);
    for (int k = k0; k < k1; k++) a += (double)y[k] * y[k];
    return (float)sqrt(a / (k1 - k0));
}
static float amp_at(const float *y, float f, float t0, float t1)
{
    double c = 0, s = 0; int k0 = (int)(t0 * SR), k1 = (int)(t1 * SR);
    for (int k = k0; k < k1; k++) { double w = 6.283185307 * f * k / SR; c += y[k] * cos(w); s += y[k] * sin(w); }
    return (float)(2.0 * sqrt(c * c + s * s) / (k1 - k0));
}
static float db(float r) { return 20.0f * log10f(r > 1e-12f ? r : 1e-12f); }

int main(void)
{
    px_preset_t p, q;

    /* the volume */
    preset(&p);
    sine(440.0f, 0.3f, 0.0f, 4.0f);
    run(&p, ref, 0, 0, 0, 0.0f, 0);
    {
        float at[1] = {0.5f}; int pos[1] = {127};
        run(&p, out, at, pos, 1, 0.0f, 0);
        float d = 0.0f;
        for (int k = 0; k < N; k++) { float a = fabsf(out[k] - ref[k]); if (a > d) d = a; }
        ok(d == 0.0f, "no position yet, then the pedal at the toe as the volume: the signal as it was, sample for sample");
    }
    {
        float at[3] = {0.5f, 1.5f, 2.5f}; int pos[3] = {64, 0, 127};
        run(&p, out, at, pos, 3, 0.0f, 0);
        float half = db(rms(out, 1.0f, 1.4f) / rms(ref, 1.0f, 1.4f)), heel = rms(out, 2.0f, 2.4f), back = rms(out, 3.0f, 3.4f) / rms(ref, 3.0f, 3.4f);
        float jump = 0.0f;
        for (int k = 1; k < N; k++) { float a = fabsf(out[k] - out[k - 1]); if (a > jump) jump = a; }
        printf("  half way %.1f dB, heel %.2g, toe again x%.4f, widest step %.3f (the sine's own: %.3f)\n", half, heel, back, jump,
               0.3f * 6.2831853f * 440.0f / SR);
        ok(half > -19.5f && half < -16.5f && heel < 1e-5f && fabsf(back - 1.0f) < 1e-3f && jump < 0.3f * 6.2831853f * 440.0f / SR * 1.05f,
           "the volume: half way about -18 dB, the heel silent, the toe unity again, no step wider than the sine's own");
    }

    /* before the delay: the repeats ring on */
    preset(&p);
    p.on[PX_B_DELAY] = 1; p.type[PX_B_DELAY] = PX_DLY_DIGITAL;
    p.p[PX_B_DELAY][0] = 20; p.p[PX_B_DELAY][1] = 60; p.p[PX_B_DELAY][2] = 60;   /* 265 ms, feedback, mix */
    sine(330.0f, 0.3f, 0.5f, 0.6f);
    run(&p, ref, 0, 0, 0, 0.0f, 0);
    {
        float at[2] = {0.2f, 0.65f}; int pos[2] = {127, 0};
        run(&p, out, at, pos, 2, 0.0f, 0);
        float after = rms(out, 0.7f, 2.0f) / rms(ref, 0.7f, 2.0f);
        float at2[1] = {0.2f}; int pos2[1] = {0};
        run(&p, out, at2, pos2, 1, 0.0f, 0);
        float before = rms(out, 0.5f, 2.0f) / rms(ref, 0.5f, 2.0f);
        printf("  closed after the phrase: the repeats x%.3f; closed before it: x%.2g\n", after, before);
        ok(after > 0.95f && after < 1.05f && before < 1e-4f, "the volume comes before the delay: closed after a phrase the repeats ring on, before it nothing");
    }

    /* the hold: a wah turned off with the pedal at the heel */
    preset(&p);
    p.on[PX_B_SFX] = 1; p.type[PX_B_SFX] = PX_SFX_LFOWAH;
    p.p[PX_B_SFX][0] = 30; p.p[PX_B_SFX][1] = 0; p.p[PX_B_SFX][2] = 60; p.p[PX_B_SFX][3] = 50;
    preset(&q);
    sine(440.0f, 0.3f, 0.0f, 4.0f);
    run(&q, ref, 0, 0, 0, 0.0f, 0);
    {
        float at[2] = {0.3f, 3.0f}; int pos[2] = {0, 0};
        run(&p, out, at, pos, 2, 1.5f, &q);
        float held = rms(out, 2.2f, 2.9f) / rms(ref, 2.2f, 2.9f), moved = rms(out, 3.3f, 3.9f);
        printf("  the wah off with the pedal at the heel: x%.4f; the pedal moved: %.2g\n", held, moved);
        ok(fabsf(held - 1.0f) < 1e-3f && moved < 1e-5f, "a wah turned off at the heel: the volume stays at unity until the pedal moves, then follows it");
    }

    /* the wah: heel dark, toe bright */
    {
        for (int k = 0; k < N; k++) in[k] = 0.1f * (sinf(6.2831853f * 400.0f * (float)k / SR) + sinf(6.2831853f * 2000.0f * (float)k / SR));
        float at[2] = {0.2f, 2.0f}; int pos[2] = {0, 127};
        run(&p, out, at, pos, 2, 0.0f, 0);
        float heel = amp_at(out, 2000.0f, 1.0f, 1.9f) / amp_at(out, 400.0f, 1.0f, 1.9f);
        float toe = amp_at(out, 2000.0f, 3.0f, 3.9f) / amp_at(out, 400.0f, 3.0f, 3.9f);
        printf("  the wah, 2 kHz against 400 Hz: heel %.1f dB, toe %.1f dB\n", db(heel), db(toe));
        ok(db(toe) - db(heel) > 20.0f, "the wah: heel dark, toe bright (20 dB or more between 400 Hz and 2 kHz)");
    }

    /* the whammy: SHIFT +12 (75), MIX 100, GLIDE 0 */
    preset(&p);
    p.on[PX_B_PITCH] = 1; p.type[PX_B_PITCH] = PX_PIT_WHAMMY;
    p.p[PX_B_PITCH][0] = 75; p.p[PX_B_PITCH][1] = 100; p.p[PX_B_PITCH][2] = 0;
    sine(220.0f, 0.3f, 0.0f, 4.0f);
    {
        float at[3] = {0.2f, 1.4f, 2.6f}; int pos[3] = {0, 127, 64};
        run(&p, out, at, pos, 3, 0.0f, 0);
        float h0 = amp_at(out, 220.0f, 0.6f, 1.3f), h1 = amp_at(out, 440.0f, 0.6f, 1.3f);
        float t0 = amp_at(out, 220.0f, 1.8f, 2.5f), t1 = amp_at(out, 440.0f, 1.8f, 2.5f);
        const float fm = 220.0f * powf(2.0f, 12.0f * (64.0f / 127.0f) / 12.0f);
        float m1 = amp_at(out, fm, 3.0f, 3.9f), m0 = amp_at(out, 220.0f, 3.0f, 3.9f);
        printf("  the whammy: heel 220 Hz %.3f / 440 Hz %.3f; toe %.3f / %.3f; half way %.0f Hz %.3f / 220 Hz %.3f\n", h0, h1, t0, t1, fm, m1, m0);
        ok(h0 > 4.0f * h1 && t1 > 4.0f * t0 && m1 > 4.0f * m0, "the whammy: heel the note, toe its SHIFT (an octave up), half way half the interval");
    }
    px_exp_set(-1);
    printf("\n%s\n", failed ? "expression pedal tests FAILED" : "all expression pedal tests passed");
    return failed ? 1 : 0;
}
