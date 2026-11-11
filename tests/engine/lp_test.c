/* v40.2: the NAM network split between the cores by layers (engine/px_nam.c pxn_lp_*) against one core: every factory
 * model, the split moved at random from block to block (up, down, to either end), the second core's blocks run at random
 * moments (and while the first waits for them: PXN_EMUVEC); v40.4: a rise of the split follows the second core's progress
 * without a wait, or waits for it when forced. The output must be the one-core output bit for bit. Then
 * the firmware's two-core pipeline with it (firmware/px_audio.h dsp_lp_run) on every factory preset with a NAM amp: per
 * 128-frame block the other core finishes the blocks left from the block before, runs the blocks after the amp on it,
 * and finishes some of this block's; this core runs the blocks before the amp and queues the network. The output must
 * be the plain engine's, one block later. Built with the sanitizers by tests/run_tests.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "px_dsp.c"

#define NB 300
static float sig[PXN_N * NB], ref[PXN_N * NB], got[PXN_N * NB];
static uint8_t lpmem[PXN_LP_BYTES] __attribute__((aligned(8)));
static int hrun(void (*fn)(uint32_t), uint32_t arg) { (void)fn; (void)arg; return 1; }   /* (the waits run its part) */
static int hwait(void) { return 0; }

int main(void)
{
    unsigned r = 12345u;
    for (int i = 0; i < PXN_N * NB; i++) {
        r = r * 1103515245u + 12345u;
        sig[i] = 0.4f * sinf((float)i * 0.021f) * (float)(i % 7000 < 4000) + 0.02f * (float)((int)(r >> 16) - 32768) / 32768.0f;
    }
    pxn_set_helper(hrun, hwait);
    pxn_set_lp(lpmem, sizeof lpmem);
    int bad = 0;
    for (int mdl = 0; mdl < PX_NAM_NFACT; mdl++) {
        float lv;
        if (nam_load(mdl, &lv)) { printf("lp_test: model %d not loaded\n", mdl); return 1; }
        pxn_model *m = &px_nam_m;
        pxn_mode_v = 0;
        pxn_reset(m);
        for (int b = 0; b < NB; b++) pxn_process(m, sig + PXN_N * b, ref + PXN_N * b, PXN_N);
        pxn_mode_v = 5;                                  /* (two cores, no vector unit: the split's mode) */
        if (!pxn_lp_ok()) { printf("lp_test: the layer split cannot run\n"); return 1; }
        pxn_reset(m);
        pxn_lp_reset();
        memset(got, 0, sizeof got);
        int s = (int)(r % PXN_NL);
        for (int b = 0; b < NB; b++) {
            r = r * 1103515245u + 12345u;
            const unsigned k = r >> 24;
            if (k < 40) s = (int)((r >> 8) % PXN_NL);  /* a jump anywhere */
            else if (k < 120) s += (k & 1) ? 1 : -1;    /* a step */
            if (k == 200) s = 0;
            if (k == 201) s = PXN_NL - 1;
            pxn_lp_split(s);
            s = pxn_lp_split_now();
            if (r & 0x1000u) pxn_lp_force();             /* (a rise that waits for the other core; else it follows as it can) */
            pxn_lp_a(m, sig + PXN_N * b, got + PXN_N * b);
            if (pxn_lp_split_used() > s) { printf("lp_test: block %d took more layers than asked\n", b); return 1; }
            for (unsigned n = (r >> 4) & 3u; n; n--) pxn_lp_b(m);
        }
        while (pxn_lp_b(m)) { }
        const int d = memcmp(ref, got, sizeof ref) != 0;
        if (d) {
            int i = 0; while (i < PXN_N * NB && ref[i] == got[i]) i++;
            printf("lp_test: model %d differs from frame %d\n", mdl, i);
        }
        bad += d;
    }
    if (bad) { printf("lp_test FAILED: %d models differ\n", bad); return 1; }
    {   /* the start-up check the firmware's development build runs (pxn_lp_equiv), with a helper that never runs: the
         * waits do the other core's work (PXN_EMUVEC) */
        pxn_mode_v = 5;
        const int d = px_nam_lp_equiv();
        if (d != 0) { printf("lp_test FAILED: pxn_lp_equiv %d\n", d); return 1; }
    }
    /* the pipeline, engine and all */
    static px_engine e;
    enum { FR = 128, AB = 120 };
    static float sg[FR * AB], rf[FR * AB], gt[FR * AB], nb[2][FR];
    for (int i = 0; i < FR * AB; i++) {
        r = r * 1103515245u + 12345u;
        sg[i] = 0.3f * sinf((float)i * 0.0157f) * (float)(i % 9000 < 5000) + 0.02f * (float)((int)(r >> 16) - 32768) / 32768.0f;
    }
    int presets = 0;
    for (int f = 0; f < 24; f++) {
        px_preset_t p;
        px_preset_factory(&p, f);
        if (!p.on[PX_B_AMP] || p.type[PX_B_AMP] != PX_AMP_NAM) continue;
        pxn_mode_v = 0;
        px_engine_init(&e, 44100.0f); px_engine_set(&e, &p);
        for (int b = 0; b < AB * 2; b++) px_engine_process(&e, sg + 64 * b, rf + 64 * b, 64);
        px_engine_init(&e, 44100.0f); px_engine_set(&e, &p);
        pxn_mode_v = 5;
        if (!px_nam_lp(&e, 1)) { printf("lp_test: the engine's layer split did not start\n"); return 1; }
        memset(gt, 0, sizeof gt);
        int cur = 0;
        for (int b = 0; b < AB; b++) {
            r = r * 1103515245u + 12345u;
            if ((r >> 24) < 60) px_nam_lp_split((int)((r >> 8) % PXN_NL));
            if (r & 0x2000u) px_nam_lp_force();
            for (uint32_t k = px_nam_lp_queued(); k; k--) px_nam_lp_b(&e);   /* the other core: the block before's */
            if (b) for (int o = 0; o < FR; o += 64) px_engine_part(&e, nb[cur ^ 1] + o, gt + FR * (b - 1) + o, 64, PX_PART_POST);
            for (int o = 0; o < FR; o += 64) px_engine_part(&e, sg + FR * b + o, nb[cur] + o, 64, PX_PART_PRE);
            for (int o = 0; o < FR; o += 64) {
                px_engine_part(&e, nb[cur] + o, nb[cur] + o, 64, PX_PART_NAM);
                for (unsigned k = (r >> (4 + o / 32)) & 3u; k; k--) px_nam_lp_b(&e);   /* and some of this one's */
            }
            cur ^= 1;
        }
        px_nam_lp(&e, 0);
        presets++;
        if (memcmp(rf, gt, sizeof(float) * FR * (AB - 1))) {
            int i = 0; while (rf[i] == gt[i]) i++;
            printf("lp_test: preset %02d differs from frame %d (%g against %g)\n", f + 1, i, rf[i], gt[i]);
            bad++;
        }
    }
    pxn_mode_v = 0;
    if (bad) { printf("lp_test FAILED: %d presets differ\n", bad); return 1; }
    printf("lp_test ok: %d models through the layer split (moved at random), %d NAM presets through the pipeline: "
           "identical\n", PX_NAM_NFACT, presets);
    return 0;
}
