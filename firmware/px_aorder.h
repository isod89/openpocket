/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — the I2S driver's two callbacks, in either order (v42).
 *
 * An audio interrupt has the guitar's block (a_enc) and the output block to fill (a_dec). The output is the block the
 * guitar's was just made into, so a_enc must come first. The driver (sdk/iis.bc iis_irq_handler) serves its channels in
 * their order, the output (0) before the input (1). The input's block ends a few microseconds before the output's, so
 * the interrupt normally enters with the input alone pending, a_enc runs, and the interrupt enters again for a_dec.
 * When it enters late, both are pending and a_dec comes first. Until v42 it then sent the block before once more,
 * and the block a_enc made after it was never sent: one block (2.9 ms) twice and one lost, a click at the output and
 * in everything made from it. The interrupt enters late when TIMER5's, which is above it, runs at that moment. Its USB
 * poll is short while USB is quiet; with a USB audio stream open it builds or takes a packet every millisecond (up to
 * 40 us) and the order turned about twice a second, three times with both streams (measured: 102 blocks a minute sent
 * twice while a computer recorded). MIDI and the editor's messages lengthen the poll too, less often.
 * Now an a_dec that comes before its a_enc only notes its block, and the a_enc that follows it within A_ORDER_TICKS
 * fills it when it has started the sound. An a_enc that comes later is another interrupt's (the input had stopped):
 * the note is dropped, so a block is never filled while the output plays it.
 *
 * The including file provides a_block(data, len), the guitar's block (its sound started on the second core, or made),
 * a_send(d, n), an output block of n frames filled with that sound, A_ORDER_TICKS and fm1_ticks (base/audio.h);
 * tests/test_aorder.c runs this file on a PC.
 */
#pragma once

#ifndef A_ORDER_OLD
#define A_ORDER_OLD 0u                          /* (base/audio.h OP_UADEV: the order the driver asks, to measure it) */
#endif
static uint8_t a_encd;                          /* a_enc has run since the last output block */
static int32_t *a_dq;                           /* an output block asked for before its a_enc */
static uint32_t a_dqn, a_dqt;                   /* its frames; when it was asked for (ticks) */
static uint32_t a_swaps;                        /* how many were (USB_AUDIO's counts, docs/EDITOR-PROTOCOL.md) */

static void a_enc(void *priv, uint8_t *data, int len, uint8_t ch)     /* guitar block received */
{
    (void)priv;
    if (ch != 1u) return;
#if OP_UADEV
    uad_enc(fm1_ticks());
#endif
    int32_t *const d = a_dq && fm1_ticks() - a_dqt < A_ORDER_TICKS ? a_dq : 0;
    a_dq = 0;
    a_block(data, len);
    if (d) a_send(d, a_dqn);
    else a_encd = 1;
}
static void a_dec(void *priv, uint8_t *data, int len, uint8_t ch)     /* output block to fill */
{
    (void)priv; (void)ch;
    if (!a_encd) {
        a_swaps++;
        if (!a_dq && !A_ORDER_OLD) { a_dq = (int32_t *)data; a_dqn = (uint32_t)len / 8u; a_dqt = fm1_ticks(); return; }
    }
    a_encd = 0;
    a_dq = 0;                                   /* (two in a row: the input has stopped; this one is sent as it is) */
    a_send((int32_t *)data, (uint32_t)len / 8u);
}
