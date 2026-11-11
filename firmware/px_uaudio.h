/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — USB audio (v42, docs/USB-AUDIO.md): the pedal as a class-compliant USB sound card.
 *
 * The USB side is felucca-bsp's (bsp/chip/usb.c, FELUCCA_UAC and FELUCCA_UAC_OUT: USB Audio Class 1, both streams on
 * endpoint 4, isochronous, 16-bit stereo at 44.1 kHz; its descriptors, its rings and their pacing). This file is the
 * pedal's side:
 *   px_ua_block     each output block: the pedal's sound into the record ring, the computer's playback into the output
 *   px_ua_service   the USB AUDIO setting (px_midi.h px_ua_on) to the USB device, which re-enumerates on a change
 *   ed_ua_stat      the streams' state and counts, for the editor's USB_AUDIO (docs/EDITOR-PROTOCOL.md)
 * The clocks: the pedal's I2S clock is 44,117.6 Hz and not locked to USB's. To the computer the endpoint is
 * asynchronous: each 1 ms packet carries the frames the I2S clock made (44 or 45) and the ring's fill steers the rest by
 * one frame a block at most, so the computer follows the pedal's clock. From the computer the endpoint is adaptive:
 * its packets are resampled to the pedal's clock, the ring's fill steering the ratio.
 * The including file provides usb.c (with px_uaconf.h's streams and ring figures), a_out, a_mute, a_gv, a_ua, a_uat
 * and a_swaps (px_audio.h) and px_ua_on (px_midi.h); tests/test_usb_audio.c runs this file and usb.c on a PC.
 */
#pragma once

#if OP_UADEV
/* the measurement option (px_audio.h OP_UADEV). USB_AUDIO's first argument: bit 0 the setting, bit 1 a counter in the
 * record stream (a saw of 20000 frames, -10000..9999, both channels), bit 2 the computer's playback in it, as it is added
 * to the output (the pedal's own sound left out), bit 3 a steady tone as the input (its period 100 frames, which no block
 * boundary repeats), bit 4 the output blocks in the order the driver asks (px_audio.h A_ORDER_OLD). Then, each optional:
 * the band's floor, its top / 2, the priming / 2, the playback target / 4 (frames). The figures after the normal ones are
 * extremes since the last USB_AUDIO reply. */
static struct {
    uint32_t t_poll, t_enc;                     /* when the USB poll and a_enc last began */
    uint32_t enc_lo, enc_hi;                    /* ticks between a_enc's starts (the interrupt's period) */
    uint32_t j_lo, j_hi;                        /* ticks from a_enc's start to the output block's (a_send) */
    uint32_t poll_hi;                           /* ticks between USB polls */
    uint32_t fill_lo, fill_hi;                  /* the record ring's fill when a block is written */
    uint32_t ofill_lo, ofill_hi;                /* the playback ring's fill when a block is read */
    uint32_t jh[8];                             /* blocks by that time, in eighths of a block */
    uint32_t blocks;
    uint32_t sq;                                /* the counter */
    uint32_t tn;                                /* the tone's frame */
    int32_t lb[2u * HALF_FRAMES];               /* the playback alone, for the loopback */
} uad;
static void uad_reset(void)
{
    uint32_t i;
    uad.enc_lo = uad.j_lo = uad.fill_lo = uad.ofill_lo = 0xFFFFFFFu;
    uad.enc_hi = uad.j_hi = uad.poll_hi = uad.fill_hi = uad.ofill_hi = uad.blocks = 0;
    for (i = 0; i < 8u; i++) uad.jh[i] = 0;
}
#define UAD_LO(f, v) do { if ((v) < uad.f) uad.f = (v); } while (0)
#define UAD_HI(f, v) do { if ((v) > uad.f) uad.f = (v); } while (0)
static void uad_poll(void)
{
    const uint32_t t = fm1_ticks(), g = t - uad.t_poll;
    if (uad.t_poll) UAD_HI(poll_hi, g);
    uad.t_poll = t;
}
static void uad_enc(uint32_t t)
{
    const uint32_t g = t - uad.t_enc;
    if (uad.t_enc) { UAD_LO(enc_lo, g); UAD_HI(enc_hi, g); }
    uad.t_enc = t;
}
static void uad_send(void)
{
    const uint32_t j = fm1_ticks() - uad.t_enc, k = j * 8u / A_BUDGET;
    UAD_LO(j_lo, j); UAD_HI(j_hi, j);
    uad.jh[k > 7u ? 7u : k]++;
    uad.blocks++;
}
static void uad_tone(float *x)
{
    static float tb[100];
    uint32_t i;
    if (!(ua_dev & 4u)) return;
    if (tb[25] == 0.0f) for (i = 0; i < 100u; i++) tb[i] = 0.25f * sinf((float)i * (6.2831853f / 100.0f));
    for (i = 0; i < A_FR; i++) { x[i] = tb[uad.tn]; if (++uad.tn >= 100u) uad.tn = 0; }
}
static uint32_t uad_set(const uint8_t *a, uint32_t n)       /* returns the setting */
{
    ua_dev = (uint8_t)(a[0] >> 1);
    if (n >= 2u && a[1]) uad_lo = a[1];
    if (n >= 3u && a[2]) uad_hi = a[2] * 2u;
    if (n >= 4u && a[3]) uad_prime = a[3] * 2u;
    if (n >= 5u && a[4]) uad_tgt = a[4] * 4u;
    return a[0] & 1u;
}
/* the loopback: the playback read on its own, added to the output, and left for the record ring */
static int uad_loop(int32_t *d, uint32_t n)
{
    uint32_t i;
    if (uo.go) { const uint32_t f = uo_w - uo_r; UAD_LO(ofill_lo, f); UAD_HI(ofill_hi, f); }
    if (uac.go) { const uint32_t f = ua_w - ua_r; UAD_LO(fill_lo, f); UAD_HI(fill_hi, f); }
    if (!(ua_dev & 2u)) return 0;
    for (i = 0; i < 2u * HALF_FRAMES; i++) uad.lb[i] = 0;
    if (a_mute) uo.go = 0;
    else if (uo.alt) uac_play(uad.lb, n, (uint32_t)(a_gv * 4096.0f));
    for (i = 0; i < 2u * n; i++) {
        const int32_t v = d[i] + uad.lb[i];
        d[i] = v > 8388607 ? 8388607 : v < -8388608 ? -8388608 : v;
    }
    return 1;
}
#endif

/* One output block (a_send: the I2S interrupt on CPU0, where the block CPU1 made is whole; TIMER5's usb_poll, which
 * sends and takes the packets, nests in it as it does in felucca-bsp's render).
 * To the computer: a_out, the chain's output after the mutes and the change fade and before MASTER, through the
 * output's soft knee: what the output plays with MASTER at 100, so the recording's level does not follow the knob.
 * Mono, on both channels, 16 bits. The ring and its pacing are usb.c's (uac_render_start, uac_packet).
 * From the computer: usb.c's uac_play adds its stereo playback to the block, resampled to the pedal's clock, at
 * MASTER's gain, and clips the sum to the 24 bits the I2S takes. It is not in the recording. */
static void px_ua_block(int32_t *d, uint32_t n)
{
    uint32_t i;
#if OP_UADEV
    const int lb = uad_loop(d, n);
#endif
    if (uac.go && ua_w == ua_r) uac.go = 0;     /* the ring ran dry (a flash write held the audio): primed again */
    uac_render_start();
    if (uac.feed) {
        const uint32_t w = ua_w;
        if (UA_N - (w - ua_r) < n) uac.overruns++;   /* (nobody reads: the block is dropped) */
        else {
            for (i = 0; i < n; i++) {
                float v = a_out[i], m = v < 0.0f ? -v : v;
                if (m > 0.8f) {                 /* (dsp_out's knee) */
                    const float t = (m - 0.8f) * 5.0f;
                    m = 0.8f + 0.2f * t / (1.0f + t);
                    v = v < 0.0f ? -m : m;
                }
                ua_ring[(w + i) & (UA_N - 1u)] = (uint32_t)(uint16_t)(int16_t)(v * 32767.0f) * 0x10001u;
#if OP_UADEV
                if (ua_dev & 1u) {              /* the counter instead */
                    ua_ring[(w + i) & (UA_N - 1u)] = (uint32_t)(uint16_t)(int16_t)((int32_t)(uad.sq % 20000u) - 10000) * 0x10001u;
                    uad.sq++;
                } else if (lb)                  /* the playback instead */
                    ua_ring[(w + i) & (UA_N - 1u)] = (uint32_t)(uint16_t)(int16_t)(uad.lb[2u * i] >> 8) |
                                                     (uint32_t)(uint16_t)(int16_t)(uad.lb[2u * i + 1u] >> 8) << 16;
#endif
            }
            RING_PUBLISH();
            ua_w = w + n;
        }
    }
#if OP_UADEV
    if (lb) return;
#endif
    if (a_mute) uo.go = 0;                      /* (a flash write is coming: the output is zeros; primed again after it) */
    else if (uo.alt) uac_play(d, n, (uint32_t)(a_gv * 4096.0f));
}
/* the setting to the USB device (main loop): a change drops the pedal off the bus for a second, and it comes back with
 * or without the audio streams; the computer sees a new device. 150 ms first, so the editor's reply leaves */
static void px_ua_service(uint32_t now)
{
    static uint32_t t;
    static uint8_t seen;
    if (px_ua_on == usb_uac_on) { seen = 0; return; }
    if (!seen) { seen = 1; t = now; return; }
    if (now - t < 150u) return;
    seen = 0;
    a_ua = 0;                                   /* (the interrupt leaves the rings alone before the streams go) */
    usb_uac_switch(px_ua_on);
    a_uat = 0;
    a_ua = usb_uac_on;
    usb_retry_ms = now;                         /* off the bus for a whole second: the computer sees it leave */
}
/* USB_AUDIO's figures for the editor (px_editor.h, ED_UA_STATS of them after the state): 0 the state (bit 0: the
 * computer has the record stream open, bit 1: it takes our packets, bit 2: it has the playback stream open, bit 3: its
 * playback sounds); then, counted since the pedal started: the record stream's starts, packets, frames, the ring empty
 * (a frame repeated), the ring full (a block dropped), no packet ready, the computer stopped reading, packets a frame
 * longer, a frame shorter, the ring's lowest and highest fill at a block's start (since the stream started); the
 * playback stream's starts, packets, frames, the ring empty, the ring full; the last block's time in a_send (ticks);
 * (v42) the record packets the computer took late and the playback packets that came late (a USB frame or more
 * without one); the output blocks asked for before their input (px_aorder.h a_swaps) */
static uint32_t ed_ua_stat(uint32_t k)
{
    switch (k) {
    case 0: return (uac.alt ? 1u : 0u) | (uac.flowing ? 2u : 0u) | (uo.alt ? 4u : 0u) | (uo.go ? 8u : 0u);
    case 1: return uac.starts;
    case 2: return uac.pkts;
    case 3: return uac.frames;
    case 4: return uac.underruns;
    case 5: return uac.overruns;
    case 6: return uac.missed;
    case 7: return uac.stalls;
    case 8: return uac.adj_up;
    case 9: return uac.adj_down;
    case 10: return uac.fill_lo;
    case 11: return uac.fill_hi;
    case 12: return uo.starts;
    case 13: return uo.pkts;
    case 14: return uo.frames;
    case 15: return uo.underruns;
    case 16: return uo.overruns;
    case 17: return a_uat;
    case 18: return uac.skips;
    case 19: return uo.gaps;
    case 20: return a_swaps;
#if OP_UADEV
    case 21: return uad.enc_lo;
    case 22: return uad.enc_hi;
    case 23: return uad.j_lo;
    case 24: return uad.j_hi;
    case 25: return uad.poll_hi;
    case 26: return uad.fill_lo;
    case 27: return uad.fill_hi;
    case 28: return uad.ofill_lo;
    case 29: return uad.ofill_hi;
    case 30: return uo.step;
    case 31: return uad.blocks;
    case 40: uad_reset(); return 0u;
    default: return k >= 32u && k <= 39u ? uad.jh[k - 32u] : 0u;
#else
    default: return 0u;
#endif
    }
}
