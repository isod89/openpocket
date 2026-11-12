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
 * is the one thing kept between packets.
 *
 * Below px_blem_parse, the Bluetooth audio image's web editor (px_editor.h ED_BT): px_blem_sysex keeps the SysEx of the same
 * writes, across writes, in a second pass that leaves px_blem_parse and the messages it gives untouched; px_blem_pack cuts a
 * reply into the packets the pedal sends back as notifications. */
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

/* ---- the web editor's SysEx over BLE MIDI (the Bluetooth audio image, px_editor.h ED_BT) ----
 * Read as Android's BluetoothMidiService and Apple's CoreMIDI write it: after the header, a byte with bit 7 set is a timestamp
 * unless the byte before it was one, and then it is a status. A SysEx's data has no timestamps and runs on into the next writes,
 * which begin with their header alone (a timestamp there is read as one, and skipped); its end, F7, comes after a timestamp of its
 * own; real-time messages, each after a timestamp, may sit inside it; any other status ends it unfinished. */
#ifndef PX_BLEM_SX_MAX
#define PX_BLEM_SX_MAX 160u   /* the longest SysEx kept, between F0 and F7 (the editor's longest requests are 134 bytes) */
#endif
typedef struct {
    uint8_t buf[PX_BLEM_SX_MAX];  /* the bytes between F0 and F7 */
    uint32_t n;                   /* how many */
    uint8_t in, keep, ready;      /* inside a SysEx; keeping it; a whole one waits in buf for its reader (who clears ready) */
    uint32_t got, drop;           /* whole ones kept; dropped: too long, ended unfinished, or come while one waited */
} px_blem_sx_t;

/* one write: its SysEx bytes, kept across writes */
static void px_blem_sysex(px_blem_sx_t *s, const uint8_t *p, uint32_t n)
{
    uint32_t i, ts = 0;                                    /* ts: the byte before was a timestamp */
    if (n < 2u || !(p[0] & 0x80u)) return;                 /* no header: not a BLE MIDI packet */
    for (i = 1; i < n; i++) {
        const uint32_t b = p[i];
        if (!(b & 0x80u)) {                                /* data: a SysEx's, or another message's (not kept) */
            ts = 0;
            if (s->keep) {
                if (s->n < PX_BLEM_SX_MAX) s->buf[s->n++] = (uint8_t)b;
                else { s->keep = 0; s->drop++; }           /* too long: not the editor's */
            }
            continue;
        }
        if (!ts) { ts = 1; continue; }                     /* a timestamp: a status follows */
        ts = 0;
        if (b >= 0xF8u) continue;                          /* real time: a SysEx goes on after it */
        if (b == 0xF7u) {                                  /* the end */
            if (s->keep) { s->ready = 1; s->got++; }
            s->in = s->keep = 0;
            continue;
        }
        if (s->keep) s->drop++;                            /* another status: the SysEx ends unfinished */
        s->in = b == 0xF0u;
        s->keep = s->in && !s->ready;                      /* (one at a time: a SysEx come while one waits is dropped) */
        if (s->in && s->ready) s->drop++;
        if (s->keep) s->n = 0;
    }
}

/* the next packet of the message(s) m[0..n), F0 .. F7 with 7-bit bytes between, from m[*at] on, in at most max bytes (3 or
 * more): the header, then the bytes, each status (F0, F7) after a timestamp of its own and the data without, as the writes
 * px_blem_sysex reads. Returns the packet's length, 0 when nothing is left; *at moves past what it holds. ms: the time, whose
 * 13 low bits are the packet's timestamp (the header holds the upper 6, each timestamp byte the lower 7) */
static uint32_t px_blem_pack(const uint8_t *m, uint32_t n, uint32_t *at, uint32_t ms, uint8_t *out, uint32_t max)
{
    uint32_t i = *at, k = 0;
    if (i >= n || max < 3u) return 0;
    out[k++] = (uint8_t)(0x80u | (ms >> 7 & 0x3Fu));
    while (i < n) {
        if (m[i] & 0x80u) {                                /* a status: with its timestamp, in the same packet */
            if (k + 2u > max) break;
            out[k++] = (uint8_t)(0x80u | (ms & 0x7Fu));
        } else if (k + 1u > max) break;
        out[k++] = m[i++];
    }
    *at = i;
    return k;
}
