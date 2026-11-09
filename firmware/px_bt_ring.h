/* SPDX-License-Identifier: GPL-3.0-only */
/* OpenPocket — the phone's music into the pedal's output (the Bluetooth audio test image, docs/BT-AUDIO-TEST.md).
 *
 * The writer (the main loop, firmware/bt/bt_a2dp.c) decodes SBC frames into a ring of mono 16-bit samples; the reader (the audio
 * path, px_audio.h dsp_out, on either core) reads it at a rate the writer sets, by linear interpolation, and adds it to the
 * output. The pedal's clock and the phone's differ (the pedal's runs about 0.04 % fast), so the writer steers the rate from
 * everything buffered, decoded or not (bt_ring_steer): the phone's jitter is absorbed by the encoded packets waiting in the
 * radio's receive pool, compact, and this ring only covers the main loop's pauses.
 *
 * One writer and one reader, no lock: the writer owns w, starts and step; the reader owns r, frac, stops and its fade. The
 * reader plays while starts != stops: the writer starts it when enough is decoded, the reader stops it when it runs dry (an
 * underrun, counted), with a short fade both ways so it never clicks. Pure C, no hardware: tests/test_bt_ring.c. */
#pragma once
#include <stdint.h>

#ifndef BT_RING_N
#define BT_RING_N 2560u                         /* samples (58 ms at 44.1 kHz) */
#endif
#define BT_STEP_ONE 65536u                      /* the reader's step, Q16: 1.0 = the phone's rate */
#define BT_STEP_MAX (BT_STEP_ONE / 200u)        /* +-0.5 %: the furthest the rate is pulled (about 9 cents) */
#define BT_FADE 0.004f                          /* the fade's step a sample: 250 samples (6 ms) */

typedef struct {
    int16_t *buf;                               /* BT_RING_N samples */
    volatile uint32_t w;                        /* (writer) samples written, ever */
    volatile uint32_t starts;                   /* (writer) times the reader was started */
    volatile uint32_t step;                     /* (writer) the reader's step, Q16 */
    uint32_t base;                              /* (writer) the step at the stream's nominal rate: rate / 44,100, Q16 */
    volatile uint32_t r;                        /* (reader) samples consumed, ever: the integer part of the position */
    uint32_t frac;                              /* (reader) the fraction, Q16 */
    volatile uint32_t stops;                    /* (reader) times it ran dry */
    float g;                                    /* (reader) the fade's gain now, 0..1 */
    float last;                                 /* (reader) the last sample played (the fade-out after an underrun) */
    float est;                                  /* (writer) the backlog, smoothed (samples) */
} bt_ring_t;

static inline void bt_ring_init(bt_ring_t *q, int16_t *buf)
{
    uint32_t i;
    q->buf = buf;
    for (i = 0; i < BT_RING_N; i++) buf[i] = 0;
    q->w = q->r = q->frac = 0;
    q->starts = q->stops = 0;
    q->step = q->base = BT_STEP_ONE;
    q->g = q->last = 0.0f;
    q->est = 0.0f;
}

/* ---- writer */
static inline uint32_t bt_ring_fill(const bt_ring_t *q) { return q->w - q->r; }
static inline uint32_t bt_ring_room(const bt_ring_t *q) { uint32_t f = bt_ring_fill(q); return f + 2u >= BT_RING_N ? 0u : BT_RING_N - 2u - f; }
static inline int bt_ring_playing(const bt_ring_t *q) { return q->starts != q->stops; }
/* n mono samples in (n <= bt_ring_room) */
static inline void bt_ring_put(bt_ring_t *q, const int16_t *x, uint32_t n)
{
    uint32_t w = q->w, i;
    for (i = 0; i < n; i++) q->buf[(w + i) % BT_RING_N] = x[i];
    __asm__ volatile("" ::: "memory");          /* (the samples before the index: the reader may be on the other core) */
    q->w = w + n;
}
/* the stream's sampling rate (a phone may choose 48 kHz): the reader resamples it to the pedal's 44.1 kHz */
static inline void bt_ring_rate(bt_ring_t *q, uint32_t hz) { q->base = hz ? (uint32_t)(((uint64_t)hz << 16) / 44100u) : BT_STEP_ONE; }

/* once per main-loop pass (about 1 kHz): start the reader when the whole backlog (decoded here + still encoded, in samples) has
 * reached `target` and at least `prebuf` samples are decoded, so it starts (and starts again after running dry) with its full
 * margin; then steer its rate towards keeping that backlog. The backlog jumps by whole packets, so it is smoothed first (about
 * 0.2 s); the pull is proportional, at most BT_STEP_MAX at a backlog of 0 or 2 x target. */
static inline void bt_ring_steer(bt_ring_t *q, uint32_t encoded, uint32_t target, uint32_t prebuf)
{
    const float backlog = (float)(bt_ring_fill(q) + encoded);
    float d;
    int32_t s;
    if (!bt_ring_playing(q)) {
        q->est = backlog;
        q->step = q->base;
        if (bt_ring_fill(q) >= prebuf && backlog >= (float)target) q->starts = q->starts + 1u;
        return;
    }
    q->est += (backlog - q->est) * 0.005f;
    d = (q->est - (float)target) / (float)target;   /* -1 .. +1 around the target */
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    s = (int32_t)q->base + (int32_t)(d * (float)BT_STEP_MAX * ((float)q->base * (1.0f / (float)BT_STEP_ONE)));
    q->step = (uint32_t)s;
}

/* ---- reader: adds n samples of the music, times `gain`, to out (the audio path; inline, so it runs from RAM with dsp_out).
 * Running dry stops it (stops = starts: the writer starts it again when enough is decoded); the last sample then fades out over
 * about 60 samples instead of stopping dead. */
static inline __attribute__((always_inline)) void bt_ring_mix(bt_ring_t *q, float *out, uint32_t n, float gain)
{
    uint32_t i, r = q->r, frac = q->frac;
    const uint32_t w = q->w, step = q->step;
    float g = q->g, last = q->last;
    int on = q->starts != q->stops;
    if (!on && g <= 0.0f) return;
    for (i = 0; i < n; i++) {
        float s;
        if (on && r + 1u < w) {
            const float a = (float)q->buf[r % BT_RING_N], b = (float)q->buf[(r + 1u) % BT_RING_N];
            s = a + (b - a) * (float)frac * (1.0f / 65536.0f);
            frac += step;
            r += frac >> 16;
            frac &= 0xFFFFu;
            if (g < 1.0f) { g += BT_FADE; if (g > 1.0f) g = 1.0f; }
        } else {
            if (on) { q->stops = q->starts; on = 0; }   /* ran dry: an underrun */
            s = last;
            g -= BT_FADE * 4.0f;
            if (g <= 0.0f) { g = 0.0f; last = 0.0f; break; }
        }
        last = s;
        out[i] += s * (1.0f / 32768.0f) * g * gain;
    }
    q->r = r;
    q->frac = frac;
    q->g = g;
    q->last = last;
}
