/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — the tuner: the interrupt stores the input decimated by 4 (11025 Hz); the main loop estimates the note.
 *
 * The including file provides TN_MEM (where the buffers live) and TN_READY (whether they can be used) (base/audio.h);
 * tests/test_tuner.c runs this file on a PC.
 */
#pragma once

#define TN_N 1024u
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
#define TN_TMAX 400u   /* 27.6 Hz: a five-string bass's low B (30.9 Hz), tuned up from flat */
static float tn_lin[TN_W + TN_TMAX] TN_MEM;   /* the newest W + TMAX samples in order: all the difference function reads */
static float tuner_estimate(void)      /* frequency in Hz, or 0 (silence / unclear). YIN algorithm. */
{
    const uint32_t W = TN_W, TMIN = 10u, TMAX = TN_TMAX;  /* 27.6 Hz .. 1100 Hz at 11025 Hz */
    uint32_t w = tn_w;
    float e = 0.0f;
    if (!TN_READY) return 0.0f;
    for (uint32_t i = 0; i < TN_N; i++) {              /* the energy over all TN_N samples, the copy of the newest */
        const float v = tn_buf[(w + i) & (TN_N - 1u)];
        e += v * v;
        if (i >= TN_N - W - TMAX) tn_lin[i - (TN_N - W - TMAX)] = v;
    }
    if (e < (float)TN_N * 1.0e-6f) return 0.0f;        /* too weak: nothing to tune */
    /* the normalised difference d' lag by lag; a lag is judged once the next one is known: the first under 0.15,
     * followed down to its bottom (then nothing more to compute), else the deepest under 0.35 */
    const float *x = tn_lin;
    float run = 0.0f, d0 = 1.0f, d1 = 1.0f, m = 0.35f, a = 0.0f, c = 0.0f;   /* d0, d1: d' at t - 2, t - 1 */
    uint32_t best = 0, dip = 0;
    for (uint32_t t = 1; t <= TMAX; t++) {
        float d = 0.0f;
        for (uint32_t i = 0; i < W; i++) { float v = x[i] - x[i + t]; d += v * v; }
        run += d;
        const float d2 = (run > 0.0f) ? d * (float)t / run : 1.0f;   /* d' at t: the lag u has both neighbours */
        const uint32_t u = t - 1u;
        if (u >= TMIN) {
            if (d1 < 0.15f) dip = 1u;
            if (dip ? !(u + 1u < TMAX && d2 < d1) : d1 < m) { m = d1; best = u; a = d0; c = d2; if (dip) break; }
        }
        d0 = d1; d1 = d2;
    }
    if (!best) return 0.0f;
    float den = a - 2.0f * m + c;
    float tf = (float)best + ((den != 0.0f) ? 0.5f * (a - c) / den : 0.0f);
    return 11025.0f / tf;
}
