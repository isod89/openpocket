/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — host test of the audio callbacks' order (firmware/px_aorder.h, v42): the I2S driver calls a_enc (the
 * guitar's block) and a_dec (the output block to fill) once an interrupt, in either order; the output block must hold
 * the sound made from that interrupt's guitar block, whichever came first.
 *   cc -O1 -Wall -Ifirmware tests/test_aorder.c -o build/test_aorder && build/test_aorder
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* the pedal's side, as px_audio.h has it: a_block makes the sound of a guitar block, a_send fills an output block with
 * the sound last made. Here the sound of guitar block k is the number k. */
static int made;                                /* the guitar block whose sound is ready */
static int blocks, sends;
static void a_block(const uint8_t *data, int len) { (void)len; made = data[0]; blocks++; }
static void a_send(int32_t *d, uint32_t n) { for (uint32_t i = 0; i < n; i++) d[2u * i] = d[2u * i + 1u] = made; sends++; }
#define T_BLOCK 69648u                          /* a block in the pedal's ticks (px_audio.h A_BUDGET) */
#define A_ORDER_TICKS (T_BLOCK / 4u)
static uint32_t now;
static uint32_t fm1_ticks(void) { return now; }
#include "px_aorder.h"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

static int32_t out[2][2 * 128];                 /* the driver's two halves of the output buffer */
static uint8_t in[8 * 128];
/* one interrupt: guitar block k, output half h; dec_first: the driver found both pending and served the output first */
static void irq(int k, int h, int dec_first)
{
    in[0] = (uint8_t)k;
    for (int i = 0; i < 2 * 128; i++) out[h][i] = -1;
    now += T_BLOCK - 50000u;
    if (dec_first) { a_dec(0, (uint8_t *)out[h], 8 * 128, 0); now += 240u; a_enc(0, in, 8 * 128, 1); now += 49760u; }
    else { a_enc(0, in, 8 * 128, 1); now += 50000u; a_dec(0, (uint8_t *)out[h], 8 * 128, 0); }
}
static int holds(int h, int k)
{
    for (int i = 0; i < 2 * 128; i++) if (out[h][i] != k) return 0;
    return 1;
}

int main(void)
{
    char msg[160];
    int k, bad = 0, first = 1;
    uint32_t seed = 12345u, turned = 0;

    /* the first interrupt may already come output first */
    irq(1, 0, 1);
    CHECK(holds(0, 1) && a_swaps == 1u, "the first interrupt, output first: its own block");
    irq(2, 1, 0);
    CHECK(holds(1, 2) && a_swaps == 1u, "input first: its own block");

    /* 100000 interrupts, the order turned at random about one in 50: every output block is its interrupt's */
    for (k = 3; k < 100003; k++) {
        const int df = ((seed = seed * 1664525u + 1013904223u) >> 16) % 50u == 0u;
        irq(k & 127, k & 1, df);
        turned += (uint32_t)df;
        if (!holds(k & 1, k & 127)) { if (first) { printf("  first wrong block: interrupt %d (output first: %d)\n", k, df); first = 0; } bad++; }
    }
    snprintf(msg, sizeof msg, "100000 interrupts, %u with the output first: every output block is its own (%d were not)", (unsigned)turned, bad);
    CHECK(!bad && turned > 1000u, msg);
    CHECK(a_swaps == 1u + turned, "each output-first interrupt is counted once");
    CHECK(blocks == sends, "one output block filled for each guitar block");
    now += 0xFFFF0000u - now;                             /* the tick counter wraps between the two callbacks */
    irq(5, 0, 1);
    CHECK(holds(0, 5), "output first across the tick counter's wrap");

    /* the input stops (its channel is never served): the output goes on, each block filled as it is asked */
    {
        const int s0 = sends;
        memset(out, 0xFF, sizeof out);
        now += T_BLOCK; a_dec(0, (uint8_t *)out[0], 8 * 128, 0);   /* noted, waiting for its a_enc */
        now += T_BLOCK; a_dec(0, (uint8_t *)out[1], 8 * 128, 0);   /* no a_enc came: this one is filled */
        CHECK(sends == s0 + 1 && holds(1, made), "no input: the second output block in a row is filled at once");
        now += T_BLOCK; a_dec(0, (uint8_t *)out[0], 8 * 128, 0);
        CHECK(sends == s0 + 1 && out[0][0] == -1, "no input: the next one waits again for an a_enc");
        irq(77, 1, 0);                                    /* the input is back, a block later: the block that waited is the output's now */
        CHECK(out[0][0] == -1 && holds(1, 77) && sends == s0 + 2, "the input back: the block noted a block ago is left alone, the new one filled");
        irq(78, 0, 0);
        CHECK(holds(0, 78), "and the order is the usual one again");
        irq(79, 1, 1);
        CHECK(holds(1, 79), "also with the output first");
    }

    /* a callback for another channel is not a guitar block */
    {
        const int b0 = blocks;
        a_enc(0, in, 8 * 128, 0);
        CHECK(blocks == b0, "a_enc for a channel that is not the guitar's does nothing");
    }
    printf("  %u of 100002 interrupts with the output block asked first\n", (unsigned)turned + 1u);
    printf(fails ? "audio callbacks' order: %d FAILURES\n" : "audio callbacks' order: ok (0 failures)\n", fails);
    return fails ? 1 : 0;
}
