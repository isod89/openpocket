/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the plain way to make the sound, for an app without a schedule of its
 * own. Every block is made in the I2S interrupt, on one core. (OpenPocket has its own, px_audio.h: two cores.)
 *
 * The app's file provides, before this one:
 *   app_sound(in, out)   A_FR frames of the guitar (float, mono) -> A_FR frames of the app's sound; with A_CH 2
 *                        (base/audio.h) out takes the left block, then the right block
 *   app_sound_reset()    its sound's state cleared (delay lines, filters)
 * app_sound is not called while a_hold is set (the start, safe mode) or after the load stayed above 85 % for 8 blocks
 * (a_bypass): the guitar then goes straight to the output, on both sides.
 * For the app's main loop:
 *   a_nan_poll()         every pass (base/loop.h does): after a NaN or an infinity from app_sound the output is silence
 *                        (dsp_out); this resets the sound's state and fades it back in
 *   a_change(fn)         a change that must not be heard, or run beside the sound (another effect, a buffer cleared):
 *                        the output fades out (5.8 ms), fn runs with the interrupt held, the output fades back in
 */
#pragma once

static void a_block(const uint8_t *data, int len)      /* a guitar block: its sound made */
{
    a_input(data, len);
    const uint32_t t0 = fm1_ticks();
    if (!a_bypass && !a_hold) app_sound(a_in, a_out);
    else for (uint32_t i = 0; i < A_OUT_N; i++) a_out[i] = a_in[i % A_FR];
    dsp_out();
    dsp_load(t0, 85u);
}
static void a_send(int32_t *d, uint32_t n) { a_output(d, n); }   /* an output block filled with that sound */
static void a_collect(void) { }                        /* (nothing is in progress outside the interrupt) */

static void a_nan_poll(void)
{
    if (!a_nan) return;
    fm1_irq_mask(FM1_IRQ_ALNK0);
    app_sound_reset();
    a_xf = 0.0f; a_xft = 1;                            /* (back from silence) */
    a_nan = 0; a_nan_count++;
    fm1_irq_unmask(FM1_IRQ_ALNK0);
}
static void a_change(void (*change)(void))
{
    if (a_running) {                                   /* the interrupt fades the output: wait for silence, 12 blocks at most */
        const uint32_t t0 = fm1_ticks();
        a_xft = 0;
        while (a_xf != 0.0f && fm1_ticks() - t0 < 12u * A_BUDGET) fm1_wdt_feed();
    }
    fm1_irq_mask(FM1_IRQ_ALNK0);
    change();
    fm1_irq_unmask(FM1_IRQ_ALNK0);
    a_xft = 1;
}
