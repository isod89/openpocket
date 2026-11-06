/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket: a BLE MIDI packet (the write to the characteristic of the MIDI service, "MIDI over Bluetooth Low Energy" 1.0)
 * -> the 4-byte USB-MIDI event packets px_midi_decode takes, so a message means the same on both roads. No hardware here:
 * tests/midisim.c runs this file on a PC.
 *
 *   header, then for each message a timestamp byte and the message. Both have bit 7 set, so a byte with bit 7 set where a
 *   message may start is a timestamp. The next byte is a status, or a data byte (running status). A message may also follow
 *   with neither timestamp nor status. Timestamps are ignored: the Chocolate Plus sends the same two bytes on every write.
 *
 * SysEx and the system common messages have no meaning here: they are skipped. SysEx can run over several writes, so that
 * is the one thing kept between packets. */
#pragma once
#include <stdint.h>

typedef struct { uint8_t sysex; } px_blem_t;

static uint32_t px_blem_data_len(uint32_t st)          /* how many data bytes follow a status */
{
    if (st >= 0xF0u) return st == 0xF2u ? 2u : st == 0xF1u || st == 0xF3u ? 1u : 0u;
    return (st & 0xE0u) == 0xC0u ? 1u : 2u;            /* Program Change, Channel Pressure: one */
}

/* one write -> up to `max` event packets in out; returns how many */
static uint32_t px_blem_parse(px_blem_t *s, const uint8_t *p, uint32_t n, uint32_t *out, uint32_t max)
{
    uint32_t i = 1, run = 0, got = 0;
    if (n < 2u || !(p[0] & 0x80u)) return 0;           /* no header: not a BLE MIDI packet */
    while (i < n && got < max) {
        uint32_t b = p[i], need, d1, d2;
        if (s->sysex && !(b & 0x80u)) { i++; continue; }
        if (b & 0x80u) {                               /* a timestamp, so a status or running-status data follows */
            if (++i >= n) break;
            b = p[i];
            if (b & 0x80u) {
                i++;
                if (b >= 0xF8u) { out[got++] = 0xFu | (b << 8); continue; }   /* realtime: running status survives it */
                if (s->sysex && b != 0xF7u) s->sysex = 0;
                run = 0;
                if (b == 0xF0u) s->sysex = 1;
                else if (b == 0xF7u) s->sysex = 0;
                else if (b >= 0xF1u) i += px_blem_data_len(b);
                else run = b;
                if (!run) continue;
            } else if (s->sysex) continue;
        }
        if (!run) { i++; continue; }                   /* data with no status to belong to */
        need = px_blem_data_len(run);
        if (i + need > n) break;                       /* cut short: dropped */
        d1 = p[i];
        d2 = need > 1u ? p[i + 1u] : 0u;
        if ((d1 | d2) & 0x80u) { run = 0; continue; }  /* a status inside a message: drop it and read that byte again */
        out[got++] = (run >> 4) | (run << 8) | (d1 << 16) | (d2 << 24);
        i += need;
    }
    return got;
}
