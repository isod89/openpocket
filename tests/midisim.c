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

static int failed;
static void ok(int c, const char *what) { printf("%-76s %s\n", what, c ? "ok" : "FAIL"); if (!c) failed++; }
/* a USB-MIDI event packet: cable 0 */
static uint32_t pk(uint32_t cin, uint32_t st, uint32_t d1, uint32_t d2) { return cin | st << 8 | d1 << 16 | d2 << 24; }
static uint32_t cc_pkt(uint32_t ch, uint32_t cc, uint32_t v) { return pk(0xB, 0xB0 | ch, cc, v); }
static uint32_t pc_pkt(uint32_t ch, uint32_t n) { return pk(0xC, 0xC0 | ch, n, 0); }

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
            uint32_t cs[2 + PX_NPAR], i;
            t.blk = (uint8_t)b; t.k = 0;
            t.kind = PXM_ON; cs[0] = px_cc_of(&t);
            t.kind = PXM_TYPE; cs[1] = px_cc_of(&t);
            for (k = 0; k < PX_NPAR; k++) { t.kind = PXM_PARAM; t.k = (uint8_t)k; cs[2 + k] = px_cc_of(&t); }
            for (i = 0; i < 2 + PX_NPAR; i++) {
                n++;
                if (cs[i] > 119u) bad++;
                if (used[cs[i]]++) dup++;
                for (k = 0; k < sizeof RESERVED; k++) if (RESERVED[k] == cs[i]) bad++;
            }
        }
        ok(n == 84 && !dup, "84 settings, each on its own Control Change number");
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

    printf("\n%s\n", failed ? "MIDI tests FAILED" : "all MIDI tests passed");
    return failed ? 1 : 0;
}
