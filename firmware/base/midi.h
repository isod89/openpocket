/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): musical MIDI over USB for an app. usb.c puts the host's Program Changes,
 * Control Changes and clock / transport bytes into a ring, as 4-byte USB-MIDI event packets with the millisecond each
 * arrived. SysEx never takes this road: it goes to the update code and to the app's own frames (base/product.h).
 *
 * midi_poll(), called in every main-loop pass (base/loop.h does), empties that ring and gives the app each message.
 * The app's file names its MIDI map before this file:
 *   #define APP_MIDI my_map
 *   static void my_map(const midi_msg_t *m, uint32_t ms)     ms: when the message arrived
 * The ring must be emptied by every app, with a map or without: while it is nearly full usb.c leaves the host's packets
 * unread, and the installer's SysEx comes in the same packets. Without APP_MIDI the messages are dropped.
 * midi_tempo turns the clock into a quarter note's time (tap tempo's other source).
 * (OpenPocket has its own map and its own copy of this decoder: px_midi.h, px_midi_io.h.)
 */
#pragma once

enum { MIDI_NONE, MIDI_PC, MIDI_CC, MIDI_CLOCK, MIDI_START, MIDI_STOP };
typedef struct { uint8_t type, ch, d1, d2; } midi_msg_t;   /* ch 0..15; PC: d1 the program; CC: d1 the number, d2 the value */

/* One USB-MIDI event packet: byte 0 = cable number << 4 | code index, bytes 1..3 = the MIDI message. A Program Change, a
 * Control Change, the clock and Start / Continue / Stop pass; anything else, or a packet that is not well formed, is
 * MIDI_NONE. */
static midi_msg_t midi_decode(uint32_t pkt)
{
    midi_msg_t m = {MIDI_NONE, 0, 0, 0};
    const uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0xFFu, d2 = (pkt >> 24) & 0xFFu;
    if (cin == 0xFu) {                                 /* a single byte: realtime */
        if (st == 0xF8u) m.type = MIDI_CLOCK;
        else if (st == 0xFAu || st == 0xFBu) m.type = MIDI_START;
        else if (st == 0xFCu) m.type = MIDI_STOP;
        return m;
    }
    if (cin != 0xBu && cin != 0xCu) return m;
    if ((st >> 4) != cin || (d1 & 0x80u)) return m;    /* the status does not match the code index, or bad data */
    if (cin == 0xBu && (d2 & 0x80u)) return m;
    m.type = cin == 0xCu ? MIDI_PC : MIDI_CC;
    m.ch = (uint8_t)(st & 15u);
    m.d1 = (uint8_t)d1;
    m.d2 = (uint8_t)(cin == 0xBu ? d2 : 0);
    return m;
}

/* MIDI clock -> a quarter note. 24 clocks a quarter note, timed from the first of a run to the 25th, so a millisecond or
 * two of jitter is spread over the whole note. A gap of more than 100 ms between two clocks starts the run over
 * (midi_tempo_reset: at a Start or a Stop). A time is given once two runs in a row agree within 3 ms. */
typedef struct { uint32_t first_ms, last_ms, n, prev_q; } midi_tempo_t;
static void midi_tempo_reset(midi_tempo_t *c) { c->n = 0; c->prev_q = 0; }
/* one clock at time ms: the quarter note in ms (200..2000, 300 to 30 a minute), or 0 */
static uint32_t midi_tempo(midi_tempo_t *c, uint32_t ms)
{
    uint32_t q;
    if (c->n && ms - c->last_ms > 100u) c->n = 0;
    c->last_ms = ms;
    if (c->n == 0) { c->first_ms = ms; c->n = 1; return 0; }
    if (++c->n < 25u) return 0;
    q = ms - c->first_ms;
    c->first_ms = ms;
    c->n = 1;
    if (q < 200u || q > 2000u) { c->prev_q = 0; return 0; }
    if (c->prev_q && (q > c->prev_q ? q - c->prev_q : c->prev_q - q) <= 3u) { c->prev_q = q; return q; }
    c->prev_q = q;
    return 0;
}

#ifdef APP_MIDI
static void APP_MIDI(const midi_msg_t *m, uint32_t ms);
#endif
static void midi_poll(void)
{
    while (mi_r != mi_w) {                             /* usb.c's timer interrupt is the only writer */
        const uint32_t at = mi_r % MQ, pkt = midi_in_q[at], ms = midi_in_ms[at];
        RING_PUBLISH();
        mi_r++;
#ifdef APP_MIDI
        {
            const midi_msg_t m = midi_decode(pkt);
            if (m.type != MIDI_NONE) APP_MIDI(&m, ms);
        }
#else
        (void)pkt; (void)ms;
#endif
    }
    if (midi_in_overflow && mi_r == mi_w) midi_in_overflow = 0;   /* usb.c stopped queueing on overflow: start over */
}
