/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — the pedal's sound in a browser: engine/px_dsp.c and firmware/px_math.c, built to WebAssembly with no libc
 * (tools/op.mjs wasm -> tools/sim/px_engine.wasm), driven by tools/sim/px-engine.js. docs/SIMULATOR.md.
 *
 * px_process() is dsp_block() of firmware/px_audio.h, without the I2S and the load measurement: 128 frames as two
 * blocks of 64, bypass, the NaN net, the tuner's mute, MASTER and the clip at full scale. If that function changes,
 * change this one with it. audio_apply(), audio_reset() and audio_volume() are mirrored the same way. */
#include "px_dsp.h"

#define A_FR 128u
_Static_assert(sizeof(px_preset_t) == 110, "tools/sim/px-engine.js writes the preset byte by byte");

static px_engine g_eng;
static px_preset_t preset;
static float a_in[A_FR], a_out[A_FR];
static float a_gain = 0.64f, a_mg = 1.0f;
static int a_bypass, a_tmute, a_nan;

void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }

#define EXPORT(name) __attribute__((export_name(#name)))

EXPORT(px_input) float *px_input(void) { return a_in; }
EXPORT(px_output) float *px_output(void) { return a_out; }
EXPORT(px_preset) px_preset_t *px_preset(void) { return &preset; }
EXPORT(px_factory) void px_factory(int idx) { px_preset_factory(&preset, idx); }
EXPORT(px_apply) void px_apply(void) { px_engine_set(&g_eng, &preset); }
EXPORT(px_reset) void px_reset(void) { px_engine_init(&g_eng, 44100.0f); px_engine_set(&g_eng, &preset); a_nan = 0; }
EXPORT(px_volume) void px_volume(int vol) { float v = (float)vol * 0.01f; a_gain = v * v; }
EXPORT(px_bypass) void px_bypass(int on) { a_bypass = on; }
EXPORT(px_tuner_mute) void px_tuner_mute(int on) { a_tmute = on; }

EXPORT(px_process) void px_process(void)
{
    if (a_nan) px_reset();
    if (!a_bypass) {
        for (unsigned o = 0; o < A_FR; o += PX_MAX_BLOCK) px_engine_process(&g_eng, a_in + o, a_out + o, PX_MAX_BLOCK);
    } else {
        for (unsigned i = 0; i < A_FR; i++) a_out[i] = a_in[i];
    }
    for (unsigned i = 0; i < A_FR; i++) {
        float v = a_out[i];
        if (v != v || v > 1.0e6f || v < -1.0e6f) { a_nan = 1; break; }
    }
    if (a_nan) for (unsigned i = 0; i < A_FR; i++) a_out[i] = 0.0f;
    float tg = a_tmute ? 0.0f : 1.0f;
    if (a_mg != tg) {
        for (unsigned i = 0; i < A_FR; i++) { a_mg += (tg - a_mg) * 0.005f; a_out[i] *= a_mg; }
        if (tg > 0.5f ? a_mg > 0.999f : a_mg < 0.0005f) a_mg = tg;
    } else if (tg == 0.0f) for (unsigned i = 0; i < A_FR; i++) a_out[i] = 0.0f;
    for (unsigned i = 0; i < A_FR; i++) {
        float v = a_out[i] * a_gain;
        a_out[i] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
    }
}
