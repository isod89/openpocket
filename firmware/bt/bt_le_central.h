/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket: the Bluetooth audio image as a BLE MIDI central (test build 28), for a footswitch that is only a peripheral (the
 * regular M-VAVE Chocolate: phones connect to it, it connects to nothing). Which advertisement is the footswitch's, and a small
 * ATT client that finds its MIDI characteristic, subscribes to its notifications and hands their BLE MIDI packets on. No
 * hardware here: tests/test_bt_le_central.c runs this file on a PC; bt/bt_le_midi.c drives it from the host's task.
 *
 * The client is ours, not the library's GATT client (gatt_client.c, user_client: not linked): att_dispatch.c gives every ATT
 * packet with an odd opcode (the responses, the notifications) to the handler att_dispatch_register_client registers, and the
 * requests go out as the ATT server's notifications do (l2cap's prepared buffer, channel 4). One request at a time, as ATT
 * wants. */
#pragma once
#include <stdint.h>
#include <string.h>

/* the BLE MIDI service and its characteristic, least significant byte first, as ATT and the advertisements carry them */
static const uint8_t LE_UUID_MIDI_SVC[16] = {0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03};
static const uint8_t LE_UUID_MIDI_CHR[16] = {0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77};

/* ---- the advertisements: the host's GAP_EVENT_ADVERTISING_REPORT (0xE2, hci_vendor.c le_handle_advertisement_report):
 * [2] event type (0 connectable, 1 directed, 2 scannable, 3 not connectable, 4 scan response), [3] address type, [4..9] the
 * address as HCI gives it, [10] RSSI (dBm), [11] the data's length, [12..] the data (31 bytes at most). The footswitch is the
 * address that has shown, in its advertising data or its scan response (two reports), the BLE MIDI service UUID and a name
 * containing LE_CEN_NAME, has advertised as connectable, and is heard at LE_CEN_RSSI or more. Once one has been linked and
 * subscribed, only that address is taken again (until power-off): another player's controller on stage is never taken */
#ifndef LE_CEN_NAME
#define LE_CEN_NAME "CHOCOLATE"       /* (compared without case) */
#endif
#ifndef LE_CEN_RSSI
#define LE_CEN_RSSI (-80)
#endif
#define LE_CEN_SEEN 6u
enum { LE_SEEN_MIDI = 1, LE_SEEN_NAME = 2, LE_SEEN_CONN = 4 };
typedef struct { uint8_t addr[6], type, flags; int8_t rssi; uint32_t ms; } le_seen_t;
typedef struct {
    le_seen_t seen[LE_CEN_SEEN];  /* the addresses heard lately, what each has shown */
    uint8_t addr[6], type, known; /* the footswitch linked and subscribed (known: until power-off) */
    uint32_t reports, matches;    /* (the log's counts) */
} le_cen_scan_t;

static int le_cen_name_has(const uint8_t *v, uint32_t n, const char *w)   /* w (upper case) inside v, without case */
{
    uint32_t i, k;
    for (i = 0; w[0] && i < n; i++) {
        for (k = 0; w[k] && i + k < n; k++) {
            uint32_t c = v[i + k];
            if (c >= 'a' && c <= 'z') c -= 32u;
            if (c != (uint8_t)w[k]) break;
        }
        if (!w[k]) return 1;
    }
    return 0;
}
/* what an advertisement's data shows (AD structures: length, type, value): LE_SEEN_MIDI the BLE MIDI UUID in a list of 128-bit
 * UUIDs (0x06 incomplete, 0x07 complete), LE_SEEN_NAME the name (0x08 short, 0x09 complete) containing LE_CEN_NAME */
static uint32_t le_cen_ad(const uint8_t *d, uint32_t n)
{
    uint32_t i = 0, f = 0, k;
    while (i + 1u < n) {
        const uint32_t len = d[i], type = d[i + 1];
        const uint8_t *v = d + i + 2;
        if (!len || i + 1u + len > n) break;          /* (a malformed structure ends the reading) */
        if (type == 0x06u || type == 0x07u)
            for (k = 0; k + 16u <= len - 1u; k += 16u) if (!memcmp(v + k, LE_UUID_MIDI_SVC, 16)) f |= LE_SEEN_MIDI;
        if ((type == 0x08u || type == 0x09u) && le_cen_name_has(v, len - 1u, LE_CEN_NAME)) f |= LE_SEEN_NAME;
        i += 1u + len;
    }
    return f;
}
/* one advertising report p[0..n) at time ms: 1 when it is the footswitch, to connect to now (its address type and address in
 * *type and addr) */
static int le_cen_report(le_cen_scan_t *s, const uint8_t *p, uint32_t n, uint32_t ms, uint8_t *type, uint8_t *addr)
{
    uint32_t i, dn, f;
    le_seen_t *e = 0;
    if (n < 12u || p[0] != 0xE2u) return 0;
    s->reports++;
    dn = p[11] < n - 12u ? p[11] : n - 12u;
    f = le_cen_ad(p + 12, dn) | (p[2] <= 1u ? LE_SEEN_CONN : 0u);
    if (s->known) {                                     /* the footswitch already linked once: its address only */
        if (memcmp(p + 4, s->addr, 6) || !(f & LE_SEEN_CONN)) return 0;
        *type = s->type; memcpy(addr, s->addr, 6);
        s->matches++;
        return 1;
    }
    for (i = 0; i < LE_CEN_SEEN; i++) if (!memcmp(s->seen[i].addr, p + 4, 6) && s->seen[i].ms) { e = &s->seen[i]; break; }
    if (!e) {                                           /* a new address takes the place of the one heard longest ago */
        e = &s->seen[0];
        for (i = 1; i < LE_CEN_SEEN; i++) if (ms - s->seen[i].ms > ms - e->ms) e = &s->seen[i];
        memset(e, 0, sizeof *e);
        memcpy(e->addr, p + 4, 6);
    }
    e->flags |= (uint8_t)f;
    e->rssi = (int8_t)p[10];
    e->ms = ms ? ms : 1u;
    if (p[2] <= 1u) e->type = p[3];                     /* (the advertiser's own address type: its connectable reports') */
    if ((e->flags & 7u) != 7u || e->rssi < LE_CEN_RSSI) return 0;
    *type = e->type; memcpy(addr, e->addr, 6);
    s->matches++;
    return 1;
}

/* ---- the ATT client. Steps: the primary services (Read By Group Type, 0x2800) until the BLE MIDI one; its characteristics
 * (Read By Type, 0x2803) until the MIDI one; its descriptors (Find Information) after the characteristic's value until its
 * client configuration (0x2902), before the next characteristic's declaration; then the notifications on (Write Request, 1).
 * A link to the same footswitch again starts at the last step, with the handles found before. An error that asks for security
 * (insufficient authentication, encryption, its key size, authorization) asks for pairing once, and the same request goes
 * again once the link is encrypted */
enum { ATTC_IDLE, ATTC_SVC, ATTC_CHR, ATTC_DSC, ATTC_SUB, ATTC_READY, ATTC_FAIL };
enum { ATTC_R_NONE, ATTC_R_MIDI, ATTC_R_PAIR, ATTC_R_READY, ATTC_R_FAIL };
typedef struct {
    uint8_t st;                   /* the step (ATTC_*) */
    uint8_t busy;                 /* a request is out, its response awaited (the driver sets it once it has sent req) */
    uint8_t pair;                 /* pairing asked (once a link) */
    uint8_t err;                  /* the last ATT error code, or why it failed (0xF0 + the step) */
    uint16_t next, end;           /* the range still to read */
    uint16_t val, ccc;            /* the MIDI characteristic's value and client configuration handles (0: not found) */
    uint8_t req[8], req_n;        /* the request to send (req_n 0: none) */
} attc_t;

static void attc_put16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static uint32_t attc_get16(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8; }
static void attc_req(attc_t *c, uint32_t op, uint32_t a, uint32_t b, uint32_t t, uint32_t n)
{
    c->req[0] = (uint8_t)op; attc_put16(c->req + 1, a); attc_put16(c->req + 3, b); attc_put16(c->req + 5, t);
    c->req_n = (uint8_t)n;
}
static void attc_ask_svc(attc_t *c) { c->st = ATTC_SVC; attc_req(c, 0x10, c->next, 0xFFFF, 0x2800, 7); }
static void attc_ask_chr(attc_t *c) { c->st = ATTC_CHR; attc_req(c, 0x08, c->next, c->end, 0x2803, 7); }
static void attc_ask_dsc(attc_t *c) { c->st = ATTC_DSC; attc_req(c, 0x04, c->next, c->end, 0, 5); }
static void attc_ask_sub(attc_t *c) { c->st = ATTC_SUB; attc_req(c, 0x12, c->ccc, 0x0001, 0, 5); }
/* a new link: from the start, or (val and ccc known for this footswitch) straight to the notifications */
static void attc_start(attc_t *c, uint32_t val, uint32_t ccc)
{
    memset(c, 0, sizeof *c);
    c->val = (uint16_t)val; c->ccc = (uint16_t)ccc;
    if (val && ccc) attc_ask_sub(c);
    else { c->next = 1; attc_ask_svc(c); }
}
static uint32_t attc_fail(attc_t *c, uint32_t why) { c->err = (uint8_t)why; c->st = ATTC_FAIL; c->req_n = 0; c->busy = 0; return ATTC_R_FAIL; }
/* the range goes on after h, or the step has found nothing */
static uint32_t attc_go_on(attc_t *c, uint32_t h, void (*ask)(attc_t *))
{
    if (h >= c->end || h >= 0xFFFFu) return attc_fail(c, 0xF0u + c->st);
    c->next = (uint16_t)(h + 1u);
    ask(c);
    return ATTC_R_NONE;
}
/* one ATT packet from the footswitch, p[0..n): what the driver does next. ATTC_R_MIDI: p[3..n) is a BLE MIDI packet of the
 * MIDI characteristic; ATTC_R_PAIR: ask for pairing (req stays, to go again once encrypted); ATTC_R_READY: subscribed */
static uint32_t attc_rx(attc_t *c, const uint8_t *p, uint32_t n)
{
    uint32_t i, len, h = 0, op;
    if (!n) return ATTC_R_NONE;
    op = p[0];
    if (op == 0x1Bu) return n >= 3u && c->val && attc_get16(p + 1) == c->val ? ATTC_R_MIDI : ATTC_R_NONE;   /* a notification */
    if (!c->busy) return ATTC_R_NONE;                   /* (a response with no request out: not ours) */
    if (op == 0x01u) {                                  /* Error Response: [request opcode][handle][code] */
        const uint32_t code = n >= 5u ? p[4] : 0u;
        c->busy = 0;
        c->err = (uint8_t)code;
        if (code == 0x05u || code == 0x0Fu || code == 0x0Cu || code == 0x08u) {   /* security: pair, then the same again */
            if (c->pair) return attc_fail(c, code);
            c->pair = 1;
            c->req_n = (uint8_t)(c->st == ATTC_SUB ? 5 : c->st == ATTC_DSC ? 5 : 7);
            return ATTC_R_PAIR;
        }
        return attc_fail(c, code);                      /* (0x0A attribute not found: no MIDI service, characteristic or CCC) */
    }
    switch (c->st) {
    case ATTC_SVC:                                      /* [0x11][len][start, end, UUID]... */
        if (op != 0x11u || n < 2u || p[1] < 6u) break;
        len = p[1];
        for (i = 2; i + len <= n; i += len) {
            h = attc_get16(p + i + 2);                  /* (the group's end) */
            if (len == 20u && !memcmp(p + i + 4, LE_UUID_MIDI_SVC, 16)) {
                c->busy = 0;
                c->next = (uint16_t)attc_get16(p + i);
                c->end = (uint16_t)h;
                attc_ask_chr(c);
                return ATTC_R_NONE;
            }
        }
        c->busy = 0;
        c->end = 0xFFFF;
        return attc_go_on(c, h, attc_ask_svc);
    case ATTC_CHR:                                      /* [0x09][len][handle, properties, value handle, UUID]... */
        if (op != 0x09u || n < 2u || p[1] < 7u) break;
        len = p[1];
        for (i = 2; i + len <= n; i += len) {
            h = attc_get16(p + i);
            if (len == 21u && !memcmp(p + i + 5, LE_UUID_MIDI_CHR, 16)) {
                c->busy = 0;
                c->val = (uint16_t)attc_get16(p + i + 3);
                c->next = (uint16_t)(c->val + 1u);
                if (c->next > c->end) return attc_fail(c, 0xF0u + ATTC_DSC);
                attc_ask_dsc(c);
                return ATTC_R_NONE;
            }
        }
        c->busy = 0;
        return attc_go_on(c, h, attc_ask_chr);
    case ATTC_DSC:                                      /* [0x05][format 1: 16-bit, 2: 128-bit][handle, UUID]... */
        if (op != 0x05u || n < 2u || (p[1] != 1u && p[1] != 2u)) break;
        len = p[1] == 1u ? 4u : 18u;
        for (i = 2; i + len <= n; i += len) {
            h = attc_get16(p + i);
            if (len == 4u && attc_get16(p + i + 2) == 0x2902u) { c->busy = 0; c->ccc = (uint16_t)h; attc_ask_sub(c); return ATTC_R_NONE; }
            if (len == 4u && attc_get16(p + i + 2) == 0x2803u) return attc_fail(c, 0xF0u + ATTC_DSC);   /* (the next one's) */
        }
        c->busy = 0;
        return attc_go_on(c, h, attc_ask_dsc);
    case ATTC_SUB:                                      /* [0x13] */
        if (op != 0x13u) break;
        c->busy = 0;
        c->st = ATTC_READY;
        return ATTC_R_READY;
    default:
        break;
    }
    return ATTC_R_NONE;                                 /* (anything else: ignored, the request still awaited) */
}
