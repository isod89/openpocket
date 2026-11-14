/* SPDX-License-Identifier: GPL-3.0-only
 * The starter app's engine: see engine.h. */
#include "engine.h"

void se_reset(se_t *e)
{
    for (uint32_t i = 0; i < SE_DLY; i++) e->line[i] = 0.0f;
    e->w = 0;
    e->gl = e->tgl; e->gr = e->tgr; e->d = e->td;
}

void se_set(se_t *e, const uint8_t *par, int on)
{
    if (!on) { e->tgl = e->tgr = 1.0f; e->td = 0.0f; return; }
    const float lv = (float)par[SE_LEVEL] * 0.02f, pan = (float)par[SE_PAN] * 0.02f;   /* 0..2, 1 in the middle */
    const float g = lv * lv;
    e->tgl = g * (pan > 1.0f ? 2.0f - pan : 1.0f);
    e->tgr = g * (pan < 1.0f ? pan : 1.0f);
    e->td = (float)par[SE_WIDTH] * 13.23f;                 /* 0.3 ms a step at 44.1 kHz */
}

void se_run(se_t *e, const float *in, float *out)
{
    float gl = e->gl, gr = e->gr, d = e->d;
    uint32_t w = e->w;
    for (uint32_t i = 0; i < SE_FR; i++) {
        gl += (e->tgl - gl) * 0.004f;                      /* about 6 ms: a turned knob does not click */
        gr += (e->tgr - gr) * 0.004f;
        d += (e->td - d) * 0.0005f;                        /* slower: a moving delay bends the pitch */
        e->line[w] = in[i];
        float at = (float)w - d;                           /* the right side: the line d samples ago, between two samples */
        if (at < 0.0f) at += (float)SE_DLY;
        const uint32_t i1 = (uint32_t)at & (SE_DLY - 1u), i2 = (i1 + 1u) & (SE_DLY - 1u);
        const float fr = at - (float)(uint32_t)at;
        out[i] = in[i] * gl;
        out[SE_FR + i] = (e->line[i1] + (e->line[i2] - e->line[i1]) * fr) * gr;
        w = (w + 1u) & (SE_DLY - 1u);
    }
    e->gl = gl; e->gr = gr; e->d = d; e->w = w;
}
