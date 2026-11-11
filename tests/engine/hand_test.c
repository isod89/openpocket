/* v40.2: the NAM hand routines' start-up check, timing and dispatch on the PC (PXN_HAND without the pedal's assembly).
 * Each hand routine is replaced by a C stand-in that reads exactly what the pedal's code reads: the job's fields it uses,
 * the tap count it is written for (not j->k), 3 rows for rdm (not j->rows), the residual from rr2 for post2..post5, x three
 * words a frame for post4 and post5 (not j->sb), at least one frame (the hand loops count down from n), and for post5 an
 * even count and an output (it takes two frames a turn and always writes the residual). Built with the address
 * sanitizer: a check that hands a routine a bad pointer, or a dispatch that calls one with a job it cannot take, fails
 * here instead of faulting at the pedal's boot (a boot loop). Then the network with every routine in use against C, in
 * the one-core and two-core modes, and a NAM preset through the engine both ways: the same output bit for bit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "px_dsp.c"

static void need(int ok, const char *what)
{
    if (!ok) { printf("hand_test FAILED: %s\n", what); exit(1); }
}
static void frames(int n) { need(n > 0, "a hand loop called with no frame (it would run 2^32 times)"); }

void pxn_dot3x4_asm(const pxn_dot *j) { need(j->ng > 0, "dot3x4 without a group"); pxn_dot3x4_c(j); }
void pxn_dot1_asm(const pxn_dot *j) { need(j->ng > 0, "dot1 without a frame"); pxn_dot1_c(j); }
void pxn_gcopy_asm(int32_t *dst, const int32_t *src, int n, int sb) { frames(n); pxn_gcopy_c(dst, src, n, sb); }
void pxn_post_asm(const pxn_post *j) { frames(j->n); pxn_post_c(j); }
/* post2, post3: the residual from rr2 (w[o][c] by c then o, then the biases) */
static void post_rr2(const pxn_post *j, int sb)
{
    frames(j->n);
    int32_t rr[12];
    for (int c = 0; c < PXN_C; c++)
        for (int o = 0; o < PXN_C; o++) rr[4 * o + c] = j->rr2[3 * c + o];
    for (int o = 0; o < PXN_C; o++) rr[4 * o + 3] = j->rr2[9 + o];
    pxn_post q = *j;
    q.rr = rr; q.sb = sb;
    pxn_post_c(&q);
}
void pxn_post2_asm(const pxn_post *j) { post_rr2(j, j->sb); }
void pxn_post3_asm(const pxn_post *j) { post_rr2(j, j->sb); }
void pxn_post4_asm(const pxn_post *j) { post_rr2(j, 12); }
void pxn_post5_asm(const pxn_post *j)
{
    need(j->n > 0 && !(j->n & 1), "post5 with an odd number of frames (it would run past the end)");
    need(j->out != 0, "post5 without an output (it would write the residual to address 0)");
    post_rr2(j, 12);
}
static void rdot_k(const pxn_rdot *j, int K)
{
    pxn_rdot q = *j;
    q.k = K;
    pxn_rdot_c(&q);
}
void pxn_rdot_asm(const pxn_rdot *j) { rdot_k(j, j->k); }
void pxn_rdot6_asm(const pxn_rdot *j) { rdot_k(j, 6); }
void pxn_rdq6_asm(const pxn_rdot *j) { rdot_k(j, 6); }
void pxn_rdq15_asm(const pxn_rdot *j) { rdot_k(j, 15); }
void pxn_rdq16_asm(const pxn_rdot *j) { rdot_k(j, 16); }
static void rdl_k(pxn_rdot *j, int K, int rows)
{
    need(j->ng > 0, "rdl/rdm without a group");
    pxn_rdot q = *j;
    q.k = K; q.rows = rows;
    pxn_rdl_c(&q);
    j->u = q.u; j->z = q.z;
}
void pxn_rdl6_asm(pxn_rdot *j) { rdl_k(j, 6, j->rows); }
void pxn_rdl15_asm(pxn_rdot *j) { rdl_k(j, 15, j->rows); }
void pxn_rdl16_asm(pxn_rdot *j) { rdl_k(j, 16, j->rows); }
void pxn_rdm6_asm(pxn_rdot *j) { rdl_k(j, 6, 3); }
void pxn_rdm15_asm(pxn_rdot *j) { rdl_k(j, 15, 3); }

static uint32_t clk;
static uint32_t ticks(void) { return clk += 997u; }
static int hrun(void (*fn)(uint32_t), uint32_t arg) { fn(arg); return 1; }   /* the other core: at once */
static int hwait(void) { return 0; }

static px_engine e;
static float sig[64 * 400], a[64 * 400], b[64 * 400];
static void run(int f, float *out)
{
    px_preset_t p;
    px_preset_factory(&p, f);
    px_engine_init(&e, 44100.0f);
    px_engine_set(&e, &p);
    for (int k = 0; k < 400; k++) px_engine_process(&e, sig + 64 * k, out + 64 * k, 64);
}

int main(void)
{
    px_nam_helper(hrun, hwait);
    need(px_nam_check(ticks) == 1, "the start-up check refused a hand routine whose stand-in is the C one");
    need(px_nam_lp(&e, 1) == 0 && px_nam_lp_equiv() == -1, "the layer split ran without its memory");   /* (none given) */
    need(pxn_rdm_state() > 0 && pxn_post4_state() > 0 && pxn_post5_state() > 0 && pxn_rdl_state() > 0, "rdm, post4, post5 or rdl not checked");
    pxn_rd = 1; pxn_rdq_on = 1; pxn_rdl_on = 1; pxn_rdm_on = 1; pxn_p4on = 1; pxn_p5on = 1;   /* the pedal's way (v40.5) */
    static const int M[] = {0, 5, 9, 10};
    for (unsigned i = 0; i < sizeof M / sizeof M[0]; i++) {
        const int d = px_nam_equiv(M[i]);
        printf("mode %d: %d blocks differ from C\n", M[i], d);
        need(d == 0, "the network with the hand routines differs from C");
    }
    unsigned s = 11u;
    for (int i = 0; i < 64 * 400; i++) { s = s * 1103515245u + 12345u; sig[i] = 0.3f * sinf((float)i * 0.013f) + 0.01f * (float)((int)(s >> 16) - 32768) / 32768.0f; }
    int presets = 0;
    for (int f = 0; f < 24; f++) {
        px_preset_t p;
        px_preset_factory(&p, f);
        if (!p.on[PX_B_AMP] || p.type[PX_B_AMP] != PX_AMP_NAM) continue;
        pxn_rdm_on = 1; pxn_p4on = 1; pxn_p5on = 1;
        run(f, a);
        pxn_rdm_on = 0; pxn_p4on = 0; pxn_p5on = 0; pxn_rdl_on = 0; pxn_rdq_on = 0;
        run(f, b);
        pxn_rdl_on = 1; pxn_rdq_on = 1;
        need(!memcmp(a, b, sizeof a), "a NAM preset sounds different with rdm, post4 and post5");
        presets++;
    }
    printf("hand_test ok: start-up check, %d modes and %d NAM presets the same as C\n", (int)(sizeof M / sizeof M[0]), presets);
    return 0;
}
