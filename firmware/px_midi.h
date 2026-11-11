/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — musical MIDI over USB: what a message means for the pedal, and what the pedal says back.
 * The map is documented in docs/MIDI.md. No hardware here (tests/midisim.c runs this file on a PC); the rings and
 * the pedal's state are in px_midi_io.h.
 *
 *   px_midi_decode   one 4-byte USB-MIDI event packet (from usb.c's input ring) -> a message, or "ignore"
 *   px_midi_cc_apply a Control Change -> the preset (and the MASTER volume)
 *   px_midi_tx_next  the next message the pedal sends so the host follows the pedal (Program Change, then CCs)
 *   px_midi_clock_*  MIDI clock -> a quarter-note time, for the delay TIME
 *
 * Needs px_dsp.h (px_preset_t, px_ntypes, px_dly_value_for_ms). SysEx never comes here: usb.c routes it to the
 * update / editor code, and only channel voice messages and clock reach the ring this file reads.
 */
#pragma once

/* ---- settings (one byte in the preset store's header, px_store.h): all-zero = the defaults ---- */
typedef struct {
    uint8_t chan;          /* 0 = omni (receive any channel; send on 1), 1..16 = that channel */
    uint8_t send;          /* 1 = the pedal sends Program Change / Control Change when its sound changes */
    uint8_t clock;         /* 1 = MIDI clock sets the delay TIME to a quarter note */
} px_midi_cfg_t;
#define PXM_CFG_NOSEND 0x20u
#define PXM_CFG_CLOCK  0x40u
static uint8_t px_midi_cfg_pack(const px_midi_cfg_t *c)
{
    return (uint8_t)((c->chan & 31u) | (c->send ? 0u : PXM_CFG_NOSEND) | (c->clock ? PXM_CFG_CLOCK : 0u));
}
static void px_midi_cfg_unpack(px_midi_cfg_t *c, uint8_t b)
{
    c->chan = (uint8_t)(b & 31u);
    if (c->chan > 16u) c->chan = 0;                    /* a byte this firmware never wrote: omni */
    c->send = (b & PXM_CFG_NOSEND) ? 0u : 1u;
    c->clock = (b & PXM_CFG_CLOCK) ? 1u : 0u;
}
/* v42: USB AUDIO (docs/USB-AUDIO.md): 1 = the pedal is a USB sound card too. The MIDI page's fourth row and the editor
 * (USB_AUDIO) set it, the store keeps it (px_store.h, st_usb), px_usb.h presents the USB device with or without the
 * audio streams. Not in the Bluetooth test image, whose RAM is full: OP_UAUDIO 0 leaves the row and the streams out. */
#if OP_BLE_TEST
#define OP_UAUDIO 0
#else
#define OP_UAUDIO 1
#endif
static uint8_t px_ua_on;
/* the pedal's MIDI state, shared by the screen (ui.h) and the glue (px_midi_io.h) */
static px_midi_cfg_t mid_cfg;           /* all-zero until the store is read: omni, send on, clock off */
static uint8_t mid_usb_up;              /* a USB host is connected */
static uint8_t mid_last[3];
static uint8_t mid_c1_off;              /* v41: the second core stopped: 1 misread, 2 late, 3 barred, 4 fault, 5 silent, 0xFF other; 0 runs */             /* the last message received: type (PXM_*), data 1, data 2 */
static uint32_t px_midi_tx_chan(const px_midi_cfg_t *c) { return c->chan ? (uint32_t)c->chan - 1u : 0u; }

/* ---- the Control Change map (docs/MIDI.md) ---- */
enum { PXM_NONE, PXM_ON, PXM_TOGGLE, PXM_TYPE, PXM_PARAM, PXM_MASTER };
typedef struct { uint8_t kind, blk, k; } px_cc_t;
#define PXM_CC_MASTER 7u
#define PXM_CC_TOGGLE0 8u                              /* 8..19: block b flips on value 0, in the block order of 20..31 */
#define PXM_CC_ON0    20u                              /* 20..31: block b on/off */
#define PXM_CC_TYPE0  52u                              /* 52..63: block b model */
static uint32_t px_cc_param(uint32_t b, uint32_t k)    /* 60 settings over three runs of free numbers (38, Data Entry LSB, skipped) */
{
    uint32_t i = b * PX_NPAR + k;
    return i < 5u ? 33u + i : i < 18u ? 34u + i : i < 46u ? 70u + (i - 18u) : 102u + (i - 46u);
}
static uint32_t px_cc_of(const px_cc_t *t)
{
    switch (t->kind) {
    case PXM_ON:     return PXM_CC_ON0 + t->blk;
    case PXM_TOGGLE: return PXM_CC_TOGGLE0 + t->blk;
    case PXM_TYPE:   return PXM_CC_TYPE0 + t->blk;
    case PXM_PARAM:  return px_cc_param(t->blk, t->k);
    case PXM_MASTER: return PXM_CC_MASTER;
    }
    return 0;
}
static px_cc_t px_cc_tab[128];                         /* the inverse of px_cc_of, built once */
static uint8_t px_cc_tab_ok;
static void px_cc_build(void)
{
    px_cc_t t;
    uint32_t b, k;
    if (px_cc_tab_ok) return;
    for (b = 0; b < PX_NBLK; b++) {
        t.blk = (uint8_t)b; t.k = 0;
        t.kind = PXM_ON;   px_cc_tab[px_cc_of(&t)] = t;
        t.kind = PXM_TOGGLE; px_cc_tab[px_cc_of(&t)] = t;
        t.kind = PXM_TYPE; px_cc_tab[px_cc_of(&t)] = t;
        for (k = 0; k < PX_NPAR; k++) { t.kind = PXM_PARAM; t.k = (uint8_t)k; px_cc_tab[px_cc_of(&t)] = t; }
    }
    t.kind = PXM_MASTER; t.blk = t.k = 0;
    px_cc_tab[px_cc_of(&t)] = t;
    px_cc_tab_ok = 1;
}

/* 0..100 <-> 0..127; a model number <-> the middle of its slice of 0..127 */
static uint32_t pxm_to_cc(uint32_t v) { return (v * 127u + 50u) / 100u; }
static uint32_t pxm_from_cc(uint32_t c) { return (c * 100u + 63u) / 127u; }
static uint32_t pxm_type_to_cc(uint32_t t, uint32_t n) { return (t * 128u + 64u) / n; }
static uint32_t pxm_type_from_cc(uint32_t c, uint32_t n) { return c * n / 128u; }

/* ---- a received message ---- */
enum { PXM_IGNORE, PXM_PC, PXM_CC, PXM_CLOCK, PXM_START, PXM_STOP };
typedef struct { uint8_t type, d1, d2; } px_midi_msg_t;

/* One USB-MIDI event packet: byte 0 = cable number << 4 | code index, bytes 1..3 = the MIDI message. Only the
 * channel voice messages the map uses and the realtime clock / transport bytes pass; everything else is ignored.
 * A message on another channel than the setting is ignored (omni: any). */
static px_midi_msg_t px_midi_decode(const px_midi_cfg_t *cfg, uint32_t pkt)
{
    px_midi_msg_t m = {PXM_IGNORE, 0, 0};
    uint32_t cin = pkt & 15u, st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0xFFu, d2 = (pkt >> 24) & 0xFFu;
    if (cin == 0xFu) {                                 /* a single byte: realtime */
        if (st == 0xF8u) m.type = PXM_CLOCK;
        else if (st == 0xFAu || st == 0xFBu) m.type = PXM_START;
        else if (st == 0xFCu) m.type = PXM_STOP;
        return m;
    }
    if (cin != 0xBu && cin != 0xCu) return m;          /* not a Control Change or a Program Change */
    if ((st >> 4) != cin || (d1 & 0x80u)) return m;    /* the status does not match the code index, or bad data */
    if (cin == 0xBu && (d2 & 0x80u)) return m;
    if (cfg->chan && (st & 15u) != (uint32_t)cfg->chan - 1u) return m;
    m.type = cin == 0xCu ? PXM_PC : PXM_CC;
    m.d1 = (uint8_t)d1;
    m.d2 = (uint8_t)(cin == 0xBu ? d2 : 0);
    return m;
}

/* A Control Change on the preset. Returns 1 when the sound changed. `master` (0..100) is set for CC 7 (a change
 * is reported through *master_set). Numbers outside the map change nothing (modulation wheel, sustain, "all notes
 * off"...). */
static int px_midi_cc_apply(px_preset_t *p, uint32_t cc, uint32_t val, uint32_t *master, int *master_set)
{
    px_cc_t t;
    uint32_t v;
    px_cc_build();
    if (cc > 127u || val > 127u) return 0;
    t = px_cc_tab[cc];
    switch (t.kind) {
    case PXM_ON:
        v = val >= 64u;
        if (p->on[t.blk] == v) return 0;
        p->on[t.blk] = (uint8_t)v;
        return 1;
    case PXM_TOGGLE:                                   /* the pedal holds the state, so a preset change cannot put a footswitch out of step */
        if (val) return 0;
        p->on[t.blk] = !p->on[t.blk];
        return 1;
    case PXM_TYPE:
        v = pxm_type_from_cc(val, (uint32_t)px_ntypes(t.blk));
        if (p->type[t.blk] == v) return 0;
        p->type[t.blk] = (uint8_t)v;
        return 1;
    case PXM_PARAM:
        v = pxm_from_cc(val);
        if (p->p[t.blk][t.k] == v) return 0;
        p->p[t.blk][t.k] = (uint8_t)v;
        return 1;
    case PXM_MASTER:
        *master = pxm_from_cc(val);
        *master_set = 1;
        return 0;
    }
    return 0;
}

/* ---- what the pedal sends: it follows the preset it plays ---- */
typedef struct {
    px_preset_t seen;          /* the sound the host was last told about */
    uint8_t slot;              /* the slot it was last told about (Program Change) */
    uint8_t valid;             /* 0 until the first sync after boot: nothing is sent for the sound we start with */
} px_midi_tx_t;

/* Take the pedal's present state as already known to the host (after a message received from it, at boot). */
static void px_midi_tx_sync(px_midi_tx_t *t, const px_preset_t *cur, uint32_t slot)
{
    t->seen = *cur;
    t->slot = (uint8_t)slot;
    t->valid = 1;
}

/* The next message to send as a USB-MIDI event packet (cable 0), 0 = nothing to say. A slot change is a Program
 * Change and tells the host the whole new sound at once (no CC burst); after that, each changed setting is one
 * Control Change, in the order on / model / settings of the blocks. Call again while the ring has room. */
static uint32_t px_midi_tx_next(px_midi_tx_t *t, const px_midi_cfg_t *cfg, const px_preset_t *cur, uint32_t slot)
{
    uint32_t ch = px_midi_tx_chan(cfg), b, k;
    px_cc_t c;
    if (!t->valid) { px_midi_tx_sync(t, cur, slot); return 0; }
    if (!cfg->send) { px_midi_tx_sync(t, cur, slot); return 0; }
    if (t->slot != slot) {
        px_midi_tx_sync(t, cur, slot);
        return 0xCu | ((0xC0u | ch) << 8) | (slot << 16);
    }
    px_cc_build();
    for (b = 0; b < PX_NBLK; b++) {
        if (cur->on[b] != t->seen.on[b]) {
            t->seen.on[b] = cur->on[b];
            c.kind = PXM_ON; c.blk = (uint8_t)b; c.k = 0;
            return 0xBu | ((0xB0u | ch) << 8) | (px_cc_of(&c) << 16) | ((cur->on[b] ? 127u : 0u) << 24);
        }
        if (cur->type[b] != t->seen.type[b]) {
            t->seen.type[b] = cur->type[b];
            c.kind = PXM_TYPE; c.blk = (uint8_t)b; c.k = 0;
            return 0xBu | ((0xB0u | ch) << 8) | (px_cc_of(&c) << 16) |
                   (pxm_type_to_cc(cur->type[b], (uint32_t)px_ntypes((int)b)) << 24);
        }
        for (k = 0; k < PX_NPAR; k++) {
            if (cur->p[b][k] != t->seen.p[b][k]) {
                t->seen.p[b][k] = cur->p[b][k];
                c.kind = PXM_PARAM; c.blk = (uint8_t)b; c.k = (uint8_t)k;
                return 0xBu | ((0xB0u | ch) << 8) | (px_cc_of(&c) << 16) | (pxm_to_cc(cur->p[b][k]) << 24);
            }
        }
    }
    return 0;
}

/* ---- MIDI clock -> a quarter note ----
 * 24 clocks per quarter note. The time of 24 clocks is measured from the first of a run to the 25th (one run is
 * one quarter note), so a timestamp's jitter of a millisecond or two is spread over the whole note. A run that is
 * broken (a gap of more than 100 ms between two clocks, or a Stop) starts over. A time is reported once two runs in
 * a row agree within 3 ms, so a drifting host does not make the delay time jump. */
typedef struct { uint32_t first_ms, last_ms, n, prev_q; } px_midi_clock_t;
static void px_midi_clock_reset(px_midi_clock_t *c) { c->n = 0; c->prev_q = 0; }
/* feed one clock at time `ms`; returns the quarter note in ms (200..2000), or 0 */
static uint32_t px_midi_clock_tick(px_midi_clock_t *c, uint32_t ms)
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
