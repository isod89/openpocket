/* SPDX-License-Identifier: GPL-3.0-only
 * The starter app's engine (firmware/apps/starter, docs/ALT-FIRMWARE.md): the mono guitar to a left and a right side.
 * It is small on purpose. It shows what an engine of an app on the base firmware is: a C file of its own, compiled as
 * OpenPocket's engine is, its state in the app's POOL region, its per-sample code in RAM (app.sh), no C library.
 *   LEVEL  the volume of both sides: 50 as it comes, 100 four times that
 *   WIDTH  the right side comes later, by 0 to 30 ms: 0 is mono, a few ms spread the guitar between the two sides
 *   PAN    50 the middle; below it the right side fades, above it the left
 */
#pragma once
#include <stdint.h>

#define SE_FR  128u                             /* frames a block: the base firmware's A_FR */
#define SE_DLY 2048u                            /* the right side's delay line, in samples (46 ms at 44.1 kHz) */
enum { SE_LEVEL, SE_WIDTH, SE_PAN, SE_NPAR };   /* a sound's settings, each 0..100 */

typedef struct {
    float gl, gr, d;                            /* now: the two sides' gains, the right side's delay in samples */
    float tgl, tgr, td;                         /* where they glide to (se_set) */
    uint32_t w;                                 /* where the next sample goes in the line */
    float line[SE_DLY];
} se_t;

void se_reset(se_t *e);                                    /* silence in the line, nothing gliding */
void se_set(se_t *e, const uint8_t *par, int on);          /* the settings (on 0: the guitar as it is, on both sides) */
void se_run(se_t *e, const float *in, float *out);         /* SE_FR frames in; out: SE_FR left, then SE_FR right */
