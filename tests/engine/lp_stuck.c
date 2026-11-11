/* v40.5: CPU0's waits for a stuck other core end after the limit (pxn_lp_clock). Built without PXN_EMUVEC, so nobody
 * runs the other core's part (pxn_lp_b) while CPU0 waits: the queue full, and a forced rise, must give up, hand nothing
 * over and write no output; pxn_lp_reset clears it; without a clock nothing changes. Under the sanitizers (run_tests.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "px_dsp.c"
static uint8_t lpmem[PXN_LP_BYTES] __attribute__((aligned(8)));
static uint32_t now;
static uint32_t tk(void) { return now += 1u; }
static int hrun(void (*fn)(uint32_t), uint32_t arg) { (void)fn; (void)arg; return 1; }
static int hwait(void) { return 0; }
int main(void)
{
    static float in[PXN_N], out[PXN_N];
    float lv;
    pxn_set_helper(hrun, hwait);
    pxn_set_lp(lpmem, sizeof lpmem);
    if (nam_load(0, &lv)) { puts("no model"); return 1; }
    pxn_model *m = &px_nam_m;
    pxn_mode_v = 5;
    if (!pxn_lp_ok()) { puts("lp cannot run"); return 1; }
    pxn_lp_clock(tk, 1000u);
    /* 1: the queue full, the other core never finishing */
    pxn_reset(m); pxn_lp_reset(); pxn_lp_split(3);
    for (int b = 0; b < PXN_LPQ; b++) pxn_lp_a(m, in, out);
    if (pxn_lp_gave_up() || pxn_lp_queued() != PXN_LPQ) { puts("FAIL 1a"); return 1; }
    out[0] = 12345.0f;
    pxn_lp_a(m, in, out);
    if (!pxn_lp_gave_up() || pxn_lp_queued() != PXN_LPQ || out[0] != 12345.0f) { puts("FAIL 1b"); return 1; }
    pxn_lp_a(m, in, out);                                   /* (given up: returns at once) */
    if (pxn_lp_queued() != PXN_LPQ) { puts("FAIL 1c"); return 1; }
    /* 2: a forced rise waiting for layers the other core never runs */
    pxn_reset(m); pxn_lp_reset();
    if (pxn_lp_gave_up()) { puts("FAIL 2a (reset)"); return 1; }
    pxn_lp_split(0); pxn_lp_a(m, in, out);
    pxn_lp_split(PXN_NL - 1); pxn_lp_force();
    pxn_lp_a(m, in, out);
    if (!pxn_lp_gave_up() || pxn_lp_queued() != 1u) { puts("FAIL 2b"); return 1; }
    /* 3: no clock: the other core finishing in time changes nothing (one core does both parts) */
    pxn_lp_clock(0, 0);
    pxn_reset(m); pxn_lp_reset(); pxn_lp_split(5);
    for (int b = 0; b < 20; b++) { pxn_lp_a(m, in, out); while (pxn_lp_b(m)) { } }
    if (pxn_lp_gave_up()) { puts("FAIL 3"); return 1; }
    puts("lp_stuck: ok (a stuck other core: CPU0 gives up after the limit, nothing handed over)");
    return 0;
}
