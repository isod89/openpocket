/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — the factory NAM models an image leaves out (test build 28: engine/px_nam_factory.h PXN_KEEP), on a PC:
 *   cc -O2 -DPXN_KEEP=0x93u -Iengine tests/engine/nam_keep.c engine/px_dsp.c firmware/px_math.c -lm -o build/nam_keep
 * (the Bluetooth audio image's set: TWIN, AC15, JCM900, MARK IV; built without PXN_KEEP every model is its own)
 * Checks: only the kept models' weights are built in; each of the 12 knob places plays a model, a kept one its own and a left
 * out one the closest kept (the cleans TWIN, the Marshalls and the crunch JCM900, the high gains MARK IV), sample for sample
 * as that model, and shows that model's name; the user slots keep their places. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "px_dsp.h"
#include "px_nam.h"
#include "px_nam_factory.h"

#define SR 44100.0f
#define N (44100 / 2)
#define B 64
static px_engine e;
static float in[N];
static int failed;
static void ok(int c, const char *what) { printf("%-96s %s\n", what, c ? "ok" : "FAIL"); if (!c) failed++; }

static void run(int m, float *y)
{
    px_preset_t p;
    px_preset_factory(&p, 0);
    for (int b = 0; b < PX_NBLK; b++) p.on[b] = 0;
    p.p[PX_B_GATE][PX_PATCH_LEVEL] = 50;
    p.on[PX_B_AMP] = 1; p.type[PX_B_AMP] = PX_AMP_NAM;
    p.p[PX_B_AMP][0] = (uint8_t)px_nam_knob(m);
    px_engine_init(&e, SR);
    px_engine_set(&e, &p);
    for (int k = 0; k < N; k += B) px_engine_process(&e, in + k, y + k, N - k < B ? N - k : B);
}

int main(void)
{
    for (int k = 0; k < N; k++) in[k] = 0.25f * sinf(6.2831853f * 110.0f * (float)k / SR) * (k < N / 2 ? 1.0f : 0.3f);
    printf("PXN_KEEP 0x%03X: %u of %d factory models built in, %u bytes of weights\n", (unsigned)PXN_KEEP, (unsigned)PXN_NROWS,
           PX_NAM_NFACT, (unsigned)sizeof PXN_FW);
#if PXN_KEEP == 0x93u
    static const char *const WANT[PX_NAM_NFACT] = {"TWIN", "AC15", "TWIN", "TWIN", "JCM900", "JCM900", "JCM900", "MARK IV",
                                                   "JCM900", "MARK IV", "MARK IV", "MARK IV"};
    static const int PLAYS[PX_NAM_NFACT] = {0, 1, 0, 0, 4, 4, 4, 7, 4, 7, 7, 7};
    static float out[PX_NAM_NFACT][N];
    ok(PXN_NROWS == 4u && sizeof PXN_FW == 4u * sizeof PXN_FW[0], "the Bluetooth audio image's set: 4 models' weights built in");
    int names = 1, same = 1, differ = 1;
    for (int m = 0; m < PX_NAM_NFACT; m++) {
        char nm[16];
        if (!px_nam_name(m, nm) || strcmp(nm, WANT[m])) { names = 0; printf("  model %d: name %s, want %s\n", m, nm, WANT[m]); }
        run(m, out[m]);
    }
    for (int m = 0; m < PX_NAM_NFACT; m++) if (memcmp(out[m], out[PLAYS[m]], sizeof out[m])) { same = 0; printf("  model %d differs from %d\n", m, PLAYS[m]); }
    if (!memcmp(out[0], out[4], sizeof out[0]) || !memcmp(out[4], out[7], sizeof out[0]) || !memcmp(out[0], out[1], sizeof out[0])) differ = 0;
    ok(names, "every knob place shows the model that plays (a left-out one: the closest kept)");
    ok(same, "a left-out model plays the closest kept one, sample for sample");
    ok(differ, "the four kept models still sound different from one another");
    {
        char nm[16];
        int u = px_nam_name(PX_NAM_NFACT, nm);
        ok(!u && !strcmp(nm, "U1 EMPTY") && px_nam_model(px_nam_knob(PX_NAM_NFACT)) == PX_NAM_NFACT,
           "the user slots keep their places on the knob (U1 after the 12 factory places)");
    }
#else
    int own = 1;
    for (int m = 0; m < PX_NAM_NFACT; m++) {
        char nm[16];
        px_nam_name(m, nm);
        if (strncmp(nm, PXN_FNAME[m], 11)) own = 0;
    }
    ok(PXN_NROWS == (unsigned)PX_NAM_NFACT && own, "no PXN_KEEP: all 12 models built in, each with its own name");
#endif
    printf("\n%s\n", failed ? "NAM keep tests FAILED" : "all NAM keep tests passed");
    return failed ? 1 : 0;
}
