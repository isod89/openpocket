/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — the musical MIDI code (firmware/px_midi.h) on a PC: the packet decoder, the Control Change map, the
 * messages the pedal sends, the settings byte, and MIDI clock. No hardware, no sound.
 *   cc -O1 -Wall -Wno-unused-function -Iengine -Ifirmware -o build/midisim tests/midisim.c engine/px_dsp.c firmware/px_math.c -lm && build/midisim
 * Exits 1 on the first group with a failure. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "px_dsp.h"
#include "px_midi.h"
#include "px_ble_midi.h"

static int failed;
static void ok(int c, const char *what) { printf("%-76s %s\n", what, c ? "ok" : "FAIL"); if (!c) failed++; }
/* a USB-MIDI event packet: cable 0 */
static uint32_t pk(uint32_t cin, uint32_t st, uint32_t d1, uint32_t d2) { return cin | st << 8 | d1 << 16 | d2 << 24; }
static uint32_t cc_pkt(uint32_t ch, uint32_t cc, uint32_t v) { return pk(0xB, 0xB0 | ch, cc, v); }
static uint32_t pc_pkt(uint32_t ch, uint32_t n) { return pk(0xC, 0xC0 | ch, n, 0); }

/* a reference reader of BLE MIDI packets, as Android's BluetoothPacketDecoder reads them: the MIDI bytes, timestamps taken out
 * (after the header, a byte with bit 7 set is a timestamp unless the byte before was one). 0 if the header is not one */
static uint32_t ref_unpack(const uint8_t *p, uint32_t n, uint8_t *out)
{
    uint32_t i, k = 0, ts = 0;
    if (!n || (p[0] & 0xC0u) != 0x80u) return 0;
    for (i = 1; i < n; i++) {
        if ((p[i] & 0x80u) && !ts) { ts = 1; continue; }
        ts = 0;
        out[k++] = p[i];
    }
    return k;
}
static uint32_t rnd_state = 12345u;
static uint32_t rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }

int main(void)
{
    px_midi_cfg_t omni = {0, 1, 0}, ch3 = {3, 1, 0};
    px_midi_msg_t m;

    /* ---- decoder: good messages, the channel filter, and anything malformed or not ours ---- */
    m = px_midi_decode(&omni, pc_pkt(0, 12));
    ok(m.type == PXM_PC && m.d1 == 12, "Program Change 12 is understood");
    m = px_midi_decode(&omni, cc_pkt(5, 20, 127));
    ok(m.type == PXM_CC && m.d1 == 20 && m.d2 == 127, "Control Change 20 = 127 on channel 6 is understood (omni)");
    m = px_midi_decode(&ch3, cc_pkt(2, 20, 127));
    ok(m.type == PXM_CC, "channel 3 setting: a message on channel 3 passes");
    m = px_midi_decode(&ch3, cc_pkt(3, 20, 127));
    ok(m.type == PXM_IGNORE, "channel 3 setting: a message on channel 4 is ignored");
    m = px_midi_decode(&ch3, pk(0xF, 0xF8, 0, 0));
    ok(m.type == PXM_CLOCK, "the clock passes on any channel setting");
    ok(px_midi_decode(&omni, pk(0xF, 0xFA, 0, 0)).type == PXM_START && px_midi_decode(&omni, pk(0xF, 0xFB, 0, 0)).type == PXM_START &&
       px_midi_decode(&omni, pk(0xF, 0xFC, 0, 0)).type == PXM_STOP, "start / continue / stop");
    ok(px_midi_decode(&omni, pk(0xF, 0xFE, 0, 0)).type == PXM_IGNORE && px_midi_decode(&omni, pk(0xF, 0xFF, 0, 0)).type == PXM_IGNORE,
       "active sensing and system reset are ignored");
    ok(px_midi_decode(&omni, pk(0x9, 0x90, 60, 100)).type == PXM_IGNORE, "a Note On is ignored");
    ok(px_midi_decode(&omni, pk(0xE, 0xE0, 0, 64)).type == PXM_IGNORE, "a pitch bend is ignored");
    ok(px_midi_decode(&omni, pk(0xB, 0xC0, 20, 1)).type == PXM_IGNORE, "bad: code index Control Change but a Program Change status");
    ok(px_midi_decode(&omni, cc_pkt(0, 20, 0x80)).type == PXM_IGNORE && px_midi_decode(&omni, cc_pkt(0, 0x94, 5)).type == PXM_IGNORE,
       "bad: a data byte with the top bit set");
    ok(px_midi_decode(&omni, pk(0x4, 0xF0, 0x7D, 0x4F)).type == PXM_IGNORE && px_midi_decode(&omni, pk(0x7, 0x50, 0x01, 0xF7)).type == PXM_IGNORE,
       "SysEx packets never decode to a musical message");
    ok(px_midi_decode(&omni, 0).type == PXM_IGNORE, "an empty packet is ignored");
    {   /* a cable number other than 0 does not change the meaning */
        uint32_t i, good = 1;
        for (i = 0; i < 16; i++) good &= px_midi_decode(&omni, cc_pkt(0, 20, 127) | i << 4).type == PXM_CC;
        ok(good, "the cable number (high nibble of byte 0) is not used");
    }

    /* ---- the map: unique, in range, clear of the reserved numbers, and it round-trips ---- */
    {
        px_cc_t t;
        uint8_t used[128];
        uint32_t b, k, n = 0, dup = 0, bad = 0;
        static const uint8_t RESERVED[] = {0, 6, 32, 38, 64, 65, 66, 67, 68, 69, 98, 99, 100, 101};
        memset(used, 0, sizeof used);
        px_cc_build();
        for (b = 0; b < PX_NBLK; b++) {
            uint32_t cs[3 + PX_NPAR], i;
            t.blk = (uint8_t)b; t.k = 0;
            t.kind = PXM_ON; cs[0] = px_cc_of(&t);
            t.kind = PXM_TYPE; cs[1] = px_cc_of(&t);
            t.kind = PXM_TOGGLE; cs[2] = px_cc_of(&t);
            for (k = 0; k < PX_NPAR; k++) { t.kind = PXM_PARAM; t.k = (uint8_t)k; cs[3 + k] = px_cc_of(&t); }
            for (i = 0; i < 3 + PX_NPAR; i++) {
                n++;
                if (cs[i] > 119u) bad++;
                if (used[cs[i]]++) dup++;
                for (k = 0; k < sizeof RESERVED; k++) if (RESERVED[k] == cs[i]) bad++;
            }
        }
        ok(n == 96 && !dup, "96 settings and toggles, each on its own Control Change number");
        ok(!bad, "none above 119 (120..127 are channel-mode messages) or on a reserved number (bank, data entry, pedals, RPN)");
        ok(!used[PXM_CC_MASTER], "7 (volume) is not used by a block");
        bad = 0;
        for (b = 0; b < 128; b++) {
            if (!used[b]) { if (px_cc_tab[b].kind != PXM_NONE && b != PXM_CC_MASTER) bad++; continue; }
        }
        for (b = 0; b < PX_NBLK; b++) {
            t.blk = (uint8_t)b; t.k = 2; t.kind = PXM_PARAM;
            if (px_cc_tab[px_cc_of(&t)].kind != PXM_PARAM || px_cc_tab[px_cc_of(&t)].blk != b || px_cc_tab[px_cc_of(&t)].k != 2) bad++;
        }
        ok(!bad, "the table by number is the exact inverse of the table by setting");
    }
    {   /* value scaling: every pedal value goes out and comes back unchanged */
        uint32_t v, t, n, bad = 0, badt = 0;
        for (v = 0; v <= 100; v++) if (pxm_from_cc(pxm_to_cc(v)) != v || pxm_to_cc(v) > 127) bad++;
        ok(!bad, "settings 0..100 -> 0..127 -> 0..100 round-trip exactly");
        for (n = 1; n <= 40; n++) for (t = 0; t < n; t++) if (pxm_type_from_cc(pxm_type_to_cc(t, n), n) != t) badt++;
        ok(!badt, "model numbers round-trip for every block size up to 40");
        ok(pxm_from_cc(0) == 0 && pxm_from_cc(127) == 100 && pxm_from_cc(64) == 50, "0 -> 0, 127 -> 100, 64 -> 50");
    }

    /* ---- applying a Control Change ---- */
    {
        px_preset_t p;
        uint32_t master = 77;
        int mset = 0, ch;
        px_preset_factory(&p, 0);
        p.on[PX_B_AMP] = 1;
        ch = px_midi_cc_apply(&p, 20 + PX_B_AMP, 0, &master, &mset);
        ok(ch == 1 && p.on[PX_B_AMP] == 0, "CC 25 = 0 switches the amp off");
        ch = px_midi_cc_apply(&p, 20 + PX_B_AMP, 63, &master, &mset);
        ok(ch == 0 && p.on[PX_B_AMP] == 0, "63 is still off (the threshold is 64)");
        ch = px_midi_cc_apply(&p, 20 + PX_B_AMP, 64, &master, &mset);
        ok(ch == 1 && p.on[PX_B_AMP] == 1, "64 switches it on");
        ch = px_midi_cc_apply(&p, 20 + PX_B_AMP, 127, &master, &mset);
        ok(ch == 0, "the same state again reports no change");
        {
            px_cc_t t = {PXM_PARAM, PX_B_AMP, 1};
            ch = px_midi_cc_apply(&p, px_cc_of(&t), 127, &master, &mset);
            ok(ch == 1 && p.p[PX_B_AMP][1] == 100, "the amp's 2nd setting at 127 is 100");
        }
        {
            px_cc_t t = {PXM_TYPE, PX_B_AMP, 0};
            uint32_t n = (uint32_t)px_ntypes(PX_B_AMP);
            p.type[PX_B_AMP] = 0;                   /* (v37: the factory presets already play the last amp, NAM) */
            ch = px_midi_cc_apply(&p, px_cc_of(&t), 127, &master, &mset);
            ok(ch == 1 && p.type[PX_B_AMP] == n - 1u, "the amp model at 127 is its last one");
            ch = px_midi_cc_apply(&p, px_cc_of(&t), 0, &master, &mset);
            ok(ch == 1 && p.type[PX_B_AMP] == 0, "and at 0 its first");
        }
        ch = px_midi_cc_apply(&p, 7, 127, &master, &mset);
        ok(ch == 0 && mset && master == 100, "CC 7 sets the MASTER volume (0..100), not the sound");
        {
            px_preset_t before = p;
            uint32_t other[] = {0, 1, 6, 11, 32, 64, 66, 98, 99, 120, 121, 123, 127}, i, same = 1;
            for (i = 0; i < sizeof other / sizeof *other; i++) same &= px_midi_cc_apply(&p, other[i], 100, &master, &mset) == 0;
            ok(same && !memcmp(&before, &p, sizeof p), "bank, mod wheel, sustain, RPN, 'all notes off': nothing changes");
        }
        ok(px_midi_cc_apply(&p, 200, 5, &master, &mset) == 0 && px_midi_cc_apply(&p, 20, 200, &master, &mset) == 0, "out of range numbers change nothing");
    }

    /* ---- what the pedal sends ---- */
    {
        px_preset_t cur;
        px_midi_tx_t tx;
        px_midi_cfg_t cfg = {0, 1, 0}, off = {0, 0, 0}, c5 = {5, 1, 0};
        uint32_t pkt, n;
        px_preset_factory(&cur, 3);
        memset(&tx, 0, sizeof tx);
        ok(px_midi_tx_next(&tx, &cfg, &cur, 3) == 0 && tx.valid, "first look after boot: nothing is sent for the sound it starts on");
        ok(px_midi_tx_next(&tx, &cfg, &cur, 3) == 0, "nothing changed: nothing sent");
        px_preset_factory(&cur, 7);                       /* the pedal has loaded slot 7's sound when it asks */
        pkt = px_midi_tx_next(&tx, &cfg, &cur, 7);
        ok(pkt == pc_pkt(0, 7), "a new slot sends Program Change (slot 8 = number 7) on channel 1 when omni");
        ok(px_midi_tx_next(&tx, &cfg, &cur, 7) == 0, "the new slot's sound is not sent as Control Changes after it");
        cur.p[PX_B_DELAY][0] = (uint8_t)(cur.p[PX_B_DELAY][0] ^ 1);
        {
            px_cc_t t = {PXM_PARAM, PX_B_DELAY, 0};
            pkt = px_midi_tx_next(&tx, &cfg, &cur, 7);
            ok(pkt == cc_pkt(0, px_cc_of(&t), pxm_to_cc(cur.p[PX_B_DELAY][0])), "a turned setting sends its Control Change");
        }
        ok(px_midi_tx_next(&tx, &cfg, &cur, 7) == 0, "...once");
        cur.on[PX_B_REVERB] ^= 1;
        pkt = px_midi_tx_next(&tx, &cfg, &cur, 7);
        ok(pkt == cc_pkt(0, 20 + PX_B_REVERB, cur.on[PX_B_REVERB] ? 127 : 0), "a block switched sends 127 or 0");
        cur.type[PX_B_AMP] = (uint8_t)((cur.type[PX_B_AMP] + 1) % px_ntypes(PX_B_AMP));
        pkt = px_midi_tx_next(&tx, &cfg, &cur, 7);
        ok((pkt & 15u) == 0xB && ((pkt >> 16) & 0xFF) == 52u + PX_B_AMP, "a new model sends the model Control Change (52 + block)");
        n = 0;
        for (uint32_t b = 0; b < PX_NBLK; b++) cur.p[b][4] = (uint8_t)(cur.p[b][4] ^ 1), n++;
        {
            uint32_t sent = 0;
            while (px_midi_tx_next(&tx, &cfg, &cur, 7)) sent++;
            ok(sent == n, "12 changed settings are 12 messages, then silence");
        }
        px_midi_tx_sync(&tx, &cur, 7);
        cur.p[PX_B_AMP][0] ^= 1;
        px_midi_tx_sync(&tx, &cur, 7);
        ok(px_midi_tx_next(&tx, &cfg, &cur, 7) == 0, "a change made by the host (synced) is not sent back");
        pkt = px_midi_tx_next(&tx, &c5, &cur, 9);
        ok(pkt == pc_pkt(4, 9), "channel 5 setting: Program Change goes out on channel 5");
        cur.p[PX_B_AMP][0] ^= 1;
        ok(px_midi_tx_next(&tx, &off, &cur, 9) == 0 && px_midi_tx_next(&tx, &cfg, &cur, 9) == 0,
           "SEND off: nothing goes out, and the changes made meanwhile are not sent when it is switched on again");
    }

    /* ---- a received message and the pedal's own message agree ---- */
    {
        px_preset_t a, b;
        px_midi_tx_t tx;
        px_midi_cfg_t cfg = {0, 1, 0};
        uint32_t master = 0, bad = 0, i, sent = 0, want = 0, pkt;
        int mset = 0;
        px_preset_factory(&a, 20); px_preset_factory(&b, 20);
        for (i = 0; i < PX_NBLK; i++) {
            b.on[i] ^= 1; b.p[i][1] = (uint8_t)((b.p[i][1] + 37) % 101); b.type[i] = (uint8_t)((b.type[i] + 1) % px_ntypes((int)i));
            want += 2u + (b.type[i] != a.type[i]);
        }
        memset(&tx, 0, sizeof tx);
        px_midi_tx_sync(&tx, &a, 20);
        while ((pkt = px_midi_tx_next(&tx, &cfg, &b, 20))) {
            px_midi_msg_t m = px_midi_decode(&cfg, pkt);
            sent++;
            if (m.type != PXM_CC) { bad++; continue; }
            px_midi_cc_apply(&a, m.d1, m.d2, &master, &mset);
        }
        ok(sent == want && want >= 24 && !bad && !memcmp(a.on, b.on, sizeof a.on) && !memcmp(a.type, b.type, sizeof a.type) && !memcmp(a.p, b.p, sizeof a.p),
           "what the pedal sends, a second pedal applies: the same sound (on, models, settings)");
    }

    /* ---- the settings byte ---- */
    {
        px_midi_cfg_t c, d;
        uint32_t i, bad = 0;
        for (i = 0; i < 256; i++) {
            px_midi_cfg_unpack(&c, (uint8_t)i);
            if (c.chan > 16) bad++;
            d = c;
            if (px_midi_cfg_pack(&d) != px_midi_cfg_pack(&c)) bad++;
        }
        ok(!bad, "any stored byte unpacks to a legal setting, and packing it again is stable");
        px_midi_cfg_unpack(&c, 0);
        ok(c.chan == 0 && c.send == 1 && c.clock == 0, "0 (every earlier save) = omni, send on, clock sync off");
        px_midi_cfg_unpack(&c, 0xFF);
        ok(c.chan == 0 && !c.send && c.clock, "a value this firmware never wrote is omni, not a wild channel");
        c.chan = 16; c.send = 0; c.clock = 1;
        px_midi_cfg_unpack(&d, px_midi_cfg_pack(&c));
        ok(d.chan == 16 && !d.send && d.clock, "channel 16, send off, clock on survive a save");
    }

    /* ---- MIDI clock -> a quarter note ---- */
    {
        px_midi_clock_t c;
        uint32_t i, got = 0, last = 0;
        px_midi_clock_reset(&c);
        for (i = 0; i < 24 * 8; i++) {          /* 120 BPM: 20.833 ms per clock, in whole milliseconds */
            uint32_t q = px_midi_clock_tick(&c, 1000u + (i * 20833u + 500u) / 1000u);
            if (q) { got++; last = q; }
        }
        ok(got >= 5 && last >= 499 && last <= 501, "120 BPM: a quarter note of 500 ms, reported after two agreeing runs");
        px_midi_clock_reset(&c);
        got = 0;
        for (i = 0; i < 24 * 4; i++) if (px_midi_clock_tick(&c, 5000u + i * 10u)) got++;
        ok(got > 0, "250 BPM (10 ms per clock, a 240 ms quarter note) is followed");
        px_midi_clock_reset(&c);
        got = 0;
        for (i = 0; i < 24 * 4; i++) if (px_midi_clock_tick(&c, 5000u + i * 1u)) got++;
        ok(got == 0, "absurd tempo (24 ms per beat) is refused");
        px_midi_clock_reset(&c);
        got = 0;
        for (i = 0; i < 24 * 6; i++) if (px_midi_clock_tick(&c, 9000u + i * 40u + (i % 5u))) got++;
        ok(got > 0, "1 ms of jitter on each clock does not stop the sync");
        px_midi_clock_reset(&c);
        got = 0;
        {   /* 20 ms per clock for one beat, 30 ms for the next, and so on: 480 ms, 720 ms, 480 ms... */
            uint32_t ms = 20000u;
            for (i = 0; i < 24 * 8; i++) {
                if (px_midi_clock_tick(&c, ms)) got++;
                ms += ((i / 24u) % 2u) ? 30u : 20u;
            }
        }
        ok(got == 0, "a tempo that keeps changing is not followed");
        px_midi_clock_reset(&c);
        {   /* a gap of more than 100 ms between clocks starts over: no run is counted across it */
            for (i = 0; i < 20; i++) px_midi_clock_tick(&c, 30000u + i * 20u);
            got = px_midi_clock_tick(&c, 31000u);
            ok(got == 0 && c.n == 1, "a gap (the host stopped sending) starts a new run");
        }
        ok(px_dly_value_for_ms(PX_DLY_DIGITAL, 500.0f) >= 38 && px_dly_value_for_ms(PX_DLY_DIGITAL, 500.0f) <= 40,
           "the delay TIME nearest to 500 ms (20 + 12.25 v: v = 39)");
        ok(px_dly_value_for_ms(PX_DLY_DIGITAL, 2000.0f) == 100 && px_dly_value_for_ms(PX_DLY_DIGITAL, 1.0f) == 0, "the ends of the scale");
        ok(px_dly_value_for_ms(PX_DLY_REVERSE, 100.0f) == 0, "REVERSE has its own scale (250 ms and up)");
    }

    /* ---- the toggle range: CC 8 + b flips the block CC 20 + b switches ---- */
    {
        px_preset_t p, q;
        uint32_t mv = 0, b, same = 1;
        int ms = 0;
        px_preset_factory(&p, 3);
        q = p;
        for (b = 0; b < PX_NBLK; b++) {
            uint32_t was = p.on[b];
            if (!px_midi_cc_apply(&p, 8u + b, 0, &mv, &ms) || p.on[b] == was) same = 0;
            px_midi_cc_apply(&q, 20u + b, was ? 0u : 127u, &mv, &ms);
            if (p.on[b] != q.on[b]) same = 0;
            if (!px_midi_cc_apply(&p, 8u + b, 0, &mv, &ms) || p.on[b] != was) same = 0;
            px_midi_cc_apply(&q, 20u + b, was ? 127u : 0u, &mv, &ms);
        }
        ok(same, "CC 8 to 19, value 0: each flips the same block as CC 20 to 31, there and back");
        ok(px_midi_cc_apply(&p, 12, 127, &mv, &ms) == 0 && px_midi_cc_apply(&p, 12, 1, &mv, &ms) == 0 && !memcmp(&p, &q, sizeof p),
           "a toggle fires on value 0 only: a 127 then 0 tap flips once");
        p.on[PX_B_DELAY] = 0;
        px_midi_cc_apply(&p, 17, 0, &mv, &ms);
        ok(p.on[PX_B_DELAY] == 1, "CC 17 is DELAY");
        p.on[PX_B_REVERB] = 0; p.on[PX_B_MOD] = 0; p.on[PX_B_DRIVE] = 0; p.on[PX_B_PITCH] = 0; p.on[PX_B_SFX] = 0;
        px_midi_cc_apply(&p, 18, 0, &mv, &ms); px_midi_cc_apply(&p, 16, 0, &mv, &ms); px_midi_cc_apply(&p, 12, 0, &mv, &ms);
        px_midi_cc_apply(&p, 10, 0, &mv, &ms); px_midi_cc_apply(&p, 11, 0, &mv, &ms);
        ok(p.on[PX_B_REVERB] && p.on[PX_B_MOD] && p.on[PX_B_DRIVE] && p.on[PX_B_PITCH] && p.on[PX_B_SFX], "CC 18 REVERB, 16 MOD, 12 DRIVE, 10 PITCH, 11 SFX");
        q = p;
        ok(px_midi_cc_apply(&p, 0, 0, &mv, &ms) == 0 && px_midi_cc_apply(&p, 7 + 0, 0, &mv, &ms) == 0 && !memcmp(&p, &q, sizeof p), "CC 0 (bank select) still does nothing");
    }

    /* ---- a BLE MIDI write -> the same event packets as USB ---- */
    {
        px_blem_t b = {0};
        uint32_t o[8];
        uint8_t choc[] = {0x80, 0x8E, 0xB1, 0x00, 0x7F};               /* the Chocolate Plus, footswitch A, as captured */
        ok(px_blem_parse(&b, choc, sizeof choc, o, 8) == 1 && o[0] == cc_pkt(1, 0, 127), "BLE: the Chocolate's write is CC 0 = 127 on channel 2");
        ok(px_midi_decode(&omni, o[0]).type == PXM_CC, "BLE: and it decodes as a USB packet does");
        uint8_t pc[] = {0x80, 0x81, 0xC2, 0x05};
        ok(px_blem_parse(&b, pc, sizeof pc, o, 8) == 1 && o[0] == pc_pkt(2, 5), "BLE: Program Change has one data byte");
        uint8_t two[] = {0x93, 0xA0, 0xB0, 24, 127, 0xA1, 0xC0, 3};
        ok(px_blem_parse(&b, two, sizeof two, o, 8) == 2 && o[0] == cc_pkt(0, 24, 127) && o[1] == pc_pkt(0, 3), "BLE: two messages in one write");
        uint8_t run[] = {0x80, 0x80, 0xB0, 24, 127, 25, 0, 0x85, 26, 64};
        ok(px_blem_parse(&b, run, sizeof run, o, 8) == 3 && o[1] == cc_pkt(0, 25, 0) && o[2] == cc_pkt(0, 26, 64),
           "BLE: running status, with and without a timestamp");
        uint8_t rt[] = {0x80, 0x80, 0xB0, 24, 127, 0x81, 0xF8, 0x82, 25, 1};
        ok(px_blem_parse(&b, rt, sizeof rt, o, 8) == 3 && px_midi_decode(&omni, o[1]).type == PXM_CLOCK && o[2] == cc_pkt(0, 25, 1),
           "BLE: a clock between messages keeps the running status");
        uint8_t nohdr[] = {0x00, 0x80, 0xB0, 24, 127};
        ok(px_blem_parse(&b, nohdr, sizeof nohdr, o, 8) == 0, "BLE: no header bit: not a packet");
        uint8_t cut[] = {0x80, 0x80, 0xB0, 24};
        ok(px_blem_parse(&b, cut, sizeof cut, o, 8) == 0, "BLE: a message cut short is dropped");
        ok(px_blem_parse(&b, cut, 1, o, 8) == 0 && px_blem_parse(&b, cut, 0, o, 8) == 0, "BLE: empty writes give nothing");
        uint8_t nostat[] = {0x80, 24, 127};
        ok(px_blem_parse(&b, nostat, sizeof nostat, o, 8) == 0, "BLE: running status does not reach over from the last write");
        uint8_t sx1[] = {0x80, 0x80, 0xF0, 1, 2, 3}, sx2[] = {0x80, 4, 5, 0x81, 0xF7, 0x82, 0xB0, 24, 127};
        ok(px_blem_parse(&b, sx1, sizeof sx1, o, 8) == 0 && b.sysex == 1, "BLE: SysEx is skipped");
        ok(px_blem_parse(&b, sx2, sizeof sx2, o, 8) == 1 && o[0] == cc_pkt(0, 24, 127) && !b.sysex, "BLE: a SysEx that runs over two writes ends, and what follows counts");
        uint8_t sc[] = {0x80, 0x80, 0xF3, 7, 0x81, 0xB0, 24, 1};
        ok(px_blem_parse(&b, sc, sizeof sc, o, 8) == 1 && o[0] == cc_pkt(0, 24, 1), "BLE: system common messages are skipped with their data");
        uint8_t full[] = {0x80, 0x80, 0xB0, 24, 1, 25, 2, 26, 3};
        ok(px_blem_parse(&b, full, sizeof full, o, 2) == 2, "BLE: never more than the room given");
    }

    /* ---- the web editor over BLE MIDI (the Bluetooth audio image, px_editor.h ED_BT): its SysEx kept across writes
     * (px_blem_sysex), in a pass of its own that leaves px_blem_parse's messages as they were ---- */
    {
        static px_blem_sx_t s;
        px_blem_t b = {0};
        uint32_t o[8];
        /* a SysEx of 10 bytes as CoreMIDI or Android write it over three writes: the header, a timestamp and F0, data; the
         * header and data; data, the end's timestamp and F7 */
        uint8_t w1[] = {0x80, 0x81, 0xF0, 0x7D, 0x4F, 0x50, 0x12, 0x01, 0x02}, w2[] = {0x80, 0x03, 0x04, 0x05}, w3[] = {0x80, 0x06, 0x82, 0xF7};
        const uint8_t want[] = {0x7D, 0x4F, 0x50, 0x12, 1, 2, 3, 4, 5, 6};
        px_blem_sysex(&s, w1, sizeof w1);
        ok(!s.ready && s.in && s.n == 6, "BLE SysEx: its first write is kept, not yet whole");
        px_blem_sysex(&s, w2, sizeof w2);
        ok(!s.ready && s.n == 9, "BLE SysEx: a write of data alone (the header, no timestamp) goes on with it");
        px_blem_sysex(&s, w3, sizeof w3);
        ok(s.ready && !s.in && s.n == sizeof want && !memcmp(s.buf, want, sizeof want) && s.got == 1, "BLE SysEx: the end after its timestamp makes it whole: 10 bytes, as sent");
        ok(px_blem_parse(&b, w1, sizeof w1, o, 8) == 0 && px_blem_parse(&b, w2, sizeof w2, o, 8) == 0 && px_blem_parse(&b, w3, sizeof w3, o, 8) == 0 && !b.sysex,
           "BLE SysEx: px_blem_parse still skips the same three writes");
        s.ready = 0;
        uint8_t mix[] = {0x80, 0x80, 0xF0, 0x7D, 0x4F, 0x50, 0x12, 0x81, 0xF7, 0x82, 0xB0, 24, 127};
        px_blem_sysex(&s, mix, sizeof mix);
        ok(s.ready && s.n == 4 && s.buf[3] == 0x12, "BLE SysEx: a whole one in one write, a Control Change after it");
        ok(px_blem_parse(&b, mix, sizeof mix, o, 8) == 1 && o[0] == cc_pkt(0, 24, 127), "BLE SysEx: and the Control Change still reaches the pedal as before");
        s.ready = 0;
        uint8_t rt[] = {0x80, 0x80, 0xF0, 1, 2, 0x81, 0xF8, 3, 0x82, 0xF7};
        px_blem_sysex(&s, rt, sizeof rt);
        ok(s.ready && s.n == 3 && s.buf[0] == 1 && s.buf[1] == 2 && s.buf[2] == 3, "BLE SysEx: a clock inside it (with its timestamp) is not part of it");
        s.ready = 0;
        uint8_t t1[] = {0x80, 0x80, 0xF0, 1, 2}, t2[] = {0x80, 0x81, 3, 4, 0x82, 0xF7};
        px_blem_sysex(&s, t1, sizeof t1); px_blem_sysex(&s, t2, sizeof t2);
        ok(s.ready && s.n == 4 && s.buf[2] == 3 && s.buf[3] == 4, "BLE SysEx: a write that goes on after a timestamp of its own");
        s.ready = 0;
        uint8_t odd[] = {0xBF, 0xF7, 0xF0, 1, 2, 0xF0, 0xF7};       /* (timestamps whose low 7 bits read as F7 and F0) */
        px_blem_sysex(&s, odd, sizeof odd);
        ok(s.ready && s.n == 2 && s.buf[0] == 1 && s.buf[1] == 2, "BLE SysEx: a timestamp byte that reads as F7 or F0 is a timestamp");
        s.ready = 0;
        uint32_t d0 = s.drop;
        uint8_t cut[] = {0x80, 0x80, 0xF0, 1, 2, 0x81, 0xB0, 24, 127}, next[] = {0x80, 0x80, 0xF0, 5, 0x81, 0xF7};
        px_blem_sysex(&s, cut, sizeof cut);
        ok(!s.ready && !s.in && s.drop == d0 + 1u, "BLE SysEx: another status ends it unfinished: dropped");
        px_blem_sysex(&s, next, sizeof next);
        ok(s.ready && s.n == 1 && s.buf[0] == 5, "BLE SysEx: and the next one is kept whole");
        s.ready = 0;
        uint8_t st[] = {0x80, 0x80, 0xF0}, en[] = {0x80, 0x80, 0xF7}, dat[21] = {0x80};
        for (uint32_t i = 1; i < sizeof dat; i++) dat[i] = (uint8_t)i;
        d0 = s.drop;
        px_blem_sysex(&s, st, sizeof st);
        for (uint32_t k = 0; k < 9u; k++) px_blem_sysex(&s, dat, sizeof dat);      /* 180 bytes */
        px_blem_sysex(&s, en, sizeof en);
        ok(!s.ready && s.drop == d0 + 1u, "BLE SysEx: one longer than 160 bytes is dropped (no editor frame is)");
        px_blem_sysex(&s, st, sizeof st);
        for (uint32_t k = 0; k < 8u; k++) px_blem_sysex(&s, dat, sizeof dat);      /* 160 bytes */
        px_blem_sysex(&s, en, sizeof en);
        ok(s.ready && s.n == PX_BLEM_SX_MAX && s.buf[159] == 20, "BLE SysEx: one of 160 bytes is kept whole");
        s.ready = 0;
        uint8_t fa[] = {0x80, 0x80, 0xF0, 7, 8, 9, 0x80, 0xF7}, fb[] = {0x80, 0x80, 0xF0, 1, 2, 0x80, 0xF7}, fc[] = {0x80, 0x80, 0xF0, 4, 0x80, 0xF7};
        px_blem_sysex(&s, fa, sizeof fa);
        d0 = s.drop;
        px_blem_sysex(&s, fb, sizeof fb);
        ok(s.ready && s.n == 3 && s.buf[0] == 7 && s.buf[2] == 9 && s.drop == d0 + 1u, "BLE SysEx: one come while one waits is dropped; the waiting one is left as it was");
        s.ready = 0;                                                    /* (its reader took it) */
        px_blem_sysex(&s, fc, sizeof fc);
        ok(s.ready && s.n == 1 && s.buf[0] == 4, "BLE SysEx: once it was taken, the next is kept");
        s.ready = 0;
        uint8_t nohdr[] = {0x00, 0x80, 0xF0, 1, 0x80, 0xF7}, lone[] = {0x80, 0x80, 0xF7}, cc1[] = {0x80, 0x80, 0xB0, 24, 1};
        px_blem_sysex(&s, nohdr, sizeof nohdr); px_blem_sysex(&s, nohdr, 1); px_blem_sysex(&s, lone, sizeof lone); px_blem_sysex(&s, cc1, sizeof cc1);
        ok(!s.ready && !s.in, "BLE SysEx: no header, an empty write, an end without a start, a Control Change: nothing");
    }

    /* ---- the editor's replies cut into BLE MIDI packets (px_blem_pack): the pedal's notifications ---- */
    {
        static px_blem_sx_t s;
        static uint8_t m[1200], pkts[1300][520], got[1300], got2[1400];
        uint32_t lens[1300];
        const uint8_t ping[] = {0xF0, 0x7D, 0x4F, 0x50, 0x12, 0xF7};
        uint8_t p[32];
        uint32_t at = 0, k;
        k = px_blem_pack(ping, sizeof ping, &at, 0x1234u, p, 20);
        const uint8_t one[] = {0xA4, 0xB4, 0xF0, 0x7D, 0x4F, 0x50, 0x12, 0xB4, 0xF7};
        ok(k == sizeof one && !memcmp(p, one, k) && at == sizeof ping, "BLE pack: a short reply is one packet: header, timestamp, F0, data, timestamp, F7");
        ok(px_blem_pack(ping, sizeof ping, &at, 0, p, 20) == 0, "BLE pack: then nothing is left");
        at = 0;
        uint32_t n3 = 0, ok3 = 1;
        while ((k = px_blem_pack(ping, sizeof ping, &at, 0, p, 3)) != 0) {
            ok3 &= k <= 3u && (p[0] & 0xC0u) == 0x80u;
            if (n3 == 0) ok3 &= k == 3u && p[1] == 0x80u && p[2] == 0xF0u;
            n3++;
        }
        ok(ok3 && n3 == 4u && p[1] == 0x80u && p[2] == 0xF7u, "BLE pack: three bytes a packet: F0 and F7 each come with their timestamp");
        at = 0;
        ok(px_blem_pack(ping, sizeof ping, &at, 0, p, 2) == 0 && at == 0, "BLE pack: less than three bytes a packet: nothing");
        /* every length up to 1,100 bytes (TEXTS, the longest reply sent over Bluetooth, is 918), cut at every packet size from 3
         * to 40 and at the MTUs centrals ask for: each packet within its size and with a header, the reference reader gets the
         * reply back byte for byte, px_blem_sysex keeps it whole (up to 160 bytes), and px_blem_parse finds no message in it */
        static const uint32_t SIZES[] = {64, 101, 182, 244, 514};
        uint32_t runs = 0, bad = 0, n918 = 0;
        for (uint32_t r = 0; r < 3000u; r++) {
            const uint32_t n = r < 1099u ? 2u + r : 2u + rnd() % 1099u;
            const uint32_t max = r % 43u < 38u ? 3u + r % 43u : SIZES[r % 43u - 38u];
            px_blem_t pb = {0};
            uint32_t np = 0, g = 0, ev = 0, i;
            m[0] = 0xF0; m[n - 1] = 0xF7;
            for (i = 1; i + 1 < n; i++) m[i] = (uint8_t)(rnd() & 0x7Fu);
            at = 0;
            while (np < 1300u && (k = px_blem_pack(m, n, &at, rnd(), pkts[np], max)) != 0) lens[np++] = k;
            memset(&s, 0, sizeof s);
            for (i = 0; i < np; i++) {
                uint32_t e[8];
                if (lens[i] > max || (pkts[i][0] & 0xC0u) != 0x80u) bad++;
                g += ref_unpack(pkts[i], lens[i], got2 + g);
                px_blem_sysex(&s, pkts[i], lens[i]);
                ev += px_blem_parse(&pb, pkts[i], lens[i], e, 8);
            }
            if (at != n || g != n || memcmp(got2, m, n)) bad++;
            if (n - 2u <= PX_BLEM_SX_MAX ? !s.ready || s.n != n - 2u || memcmp(s.buf, m + 1, n - 2u) : s.ready || s.drop != 1u) bad++;
            if (ev || pb.sysex) bad++;
            if (n == 918u && max == 20u) n918 = np;
            runs++;
        }
        ok(!bad, "BLE pack: 3,000 replies of 2 to 1,100 bytes in packets of 3 to 514: back whole, nothing else");
        at = 0; n918 = 0;
        memset(m, 0, 918); m[0] = 0xF0; m[917] = 0xF7;
        while (px_blem_pack(m, 918, &at, 0, got, 20)) n918++;
        printf("  (a 918-byte reply, TEXTS: %u notifications of 20 bytes at most)\n", n918);
        ok(n918 == 49u, "BLE pack: TEXTS's 918 bytes are 49 notifications of 20 bytes");
        (void)runs;
    }

    printf("\n%s\n", failed ? "MIDI tests FAILED" : "all MIDI tests passed");
    return failed ? 1 : 0;
}
