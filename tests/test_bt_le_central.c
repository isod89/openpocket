/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket: the Bluetooth audio image as a BLE MIDI central (firmware/bt/bt_le_central.h, test build 28) on a PC. The ATT
 * client against a simulated footswitch's GATT server that answers as a real one at the default MTU of 23 (responses cut to
 * 23 bytes, one 128-bit entry at a time), with tables laid out in several ways, a missing service or descriptor, security
 * asked for, a stale cached handle; and the advertisement filter: the BLE MIDI UUID in the advertising data and the name in
 * the scan response, case, RSSI, not connectable, another BLE MIDI device, the known address, malformed data.
 *   cc -O1 -Wall -Ifirmware/bt -o build/test_bt_le_central tests/test_bt_le_central.c && build/test_bt_le_central */
#include <stdio.h>
#include <string.h>
#include "bt_le_central.h"

static int failed;
static void ok(int c, const char *what) { printf("%-76s %s\n", what, c ? "ok" : "FAIL"); if (!c) failed++; }

/* ---- a footswitch's GATT server */
typedef struct { uint16_t h; uint8_t t128; uint16_t t16; const uint8_t *tu; uint8_t v[20]; uint8_t vn; } attr_t;
#define MTU 23u
static attr_t db[32];
static uint32_t ndb, need_enc, enc, ccc_val, writes;
static void add(uint16_t h, uint16_t t16, const uint8_t *tu, const uint8_t *v, uint8_t vn)
{
    attr_t *a = &db[ndb++];
    memset(a, 0, sizeof *a); a->h = h; a->t16 = t16; a->tu = tu; a->t128 = tu != 0; if (vn) memcpy(a->v, v, vn); a->vn = vn;
}
static void svc16(uint16_t h, uint16_t u) { uint8_t v[2] = {(uint8_t)u, (uint8_t)(u >> 8)}; add(h, 0x2800, 0, v, 2); }
static void svc128(uint16_t h, const uint8_t *u) { add(h, 0x2800, 0, u, 16); }
static void chr16(uint16_t h, uint8_t props, uint16_t u)
{
    uint8_t v[5] = {props, (uint8_t)(h + 1), (uint8_t)((h + 1) >> 8), (uint8_t)u, (uint8_t)(u >> 8)};
    add(h, 0x2803, 0, v, 5); add((uint16_t)(h + 1), u, 0, (const uint8_t *)"\x00", 1);
}
static void chr128(uint16_t h, uint8_t props, const uint8_t *u)
{
    uint8_t v[19] = {props, (uint8_t)(h + 1), (uint8_t)((h + 1) >> 8)};
    memcpy(v + 3, u, 16); add(h, 0x2803, 0, v, 19); add((uint16_t)(h + 1), 0, u, 0, 0);
}
static void dsc16(uint16_t h, uint16_t u) { add(h, u, 0, (const uint8_t *)"\x00\x00", 2); }
static uint32_t err(uint8_t *r, uint8_t op, uint16_t h, uint8_t code) { r[0] = 1; r[1] = op; r[2] = (uint8_t)h; r[3] = (uint8_t)(h >> 8); r[4] = code; return 5; }
/* one request -> its response in r (as a server at MTU 23) */
static uint32_t serve(const uint8_t *q, uint32_t qn, uint8_t *r)
{
    uint32_t i, k = 2, len = 0;
    const uint32_t s = qn >= 5 ? (uint32_t)(q[1] | q[2] << 8) : 0, e = qn >= 5 ? (uint32_t)(q[3] | q[4] << 8) : 0;
    const uint32_t t = qn >= 7 ? (uint32_t)(q[5] | q[6] << 8) : 0;
    if (q[0] == 0x10) {                                   /* Read By Group Type (primary services) */
        r[0] = 0x11;
        for (i = 0; i < ndb; i++) {
            const attr_t *a = &db[i];
            uint32_t end = 0xFFFF, j;
            if (a->h < s || a->h > e || a->t128 || a->t16 != t) continue;
            for (j = i + 1; j < ndb; j++) if (!db[j].t128 && db[j].t16 == 0x2800) { end = db[j].h - 1u; break; }
            if (!len) len = 4u + a->vn;
            if (4u + a->vn != len || k + len > MTU) break;
            r[k] = (uint8_t)a->h; r[k + 1] = (uint8_t)(a->h >> 8); r[k + 2] = (uint8_t)end; r[k + 3] = (uint8_t)(end >> 8);
            memcpy(r + k + 4, a->v, a->vn); k += len;
        }
        if (!len) return err(r, q[0], (uint16_t)s, 0x0A);
        r[1] = (uint8_t)len; return k;
    }
    if (q[0] == 0x08) {                                   /* Read By Type (characteristic declarations) */
        r[0] = 0x09;
        for (i = 0; i < ndb; i++) {
            const attr_t *a = &db[i];
            if (a->h < s || a->h > e || a->t128 || a->t16 != t) continue;
            if (!len) len = 2u + a->vn;
            if (2u + a->vn != len || k + len > MTU) break;
            r[k] = (uint8_t)a->h; r[k + 1] = (uint8_t)(a->h >> 8); memcpy(r + k + 2, a->v, a->vn); k += len;
        }
        if (!len) return err(r, q[0], (uint16_t)s, 0x0A);
        r[1] = (uint8_t)len; return k;
    }
    if (q[0] == 0x04) {                                   /* Find Information */
        uint32_t fmt = 0;
        r[0] = 0x05;
        for (i = 0; i < ndb; i++) {
            const attr_t *a = &db[i];
            if (a->h < s || a->h > e) continue;
            if (!fmt) fmt = a->t128 ? 2u : 1u;
            if ((a->t128 ? 2u : 1u) != fmt || k + (fmt == 1u ? 4u : 18u) > MTU) break;
            r[k] = (uint8_t)a->h; r[k + 1] = (uint8_t)(a->h >> 8);
            if (fmt == 1u) { r[k + 2] = (uint8_t)a->t16; r[k + 3] = (uint8_t)(a->t16 >> 8); k += 4; }
            else { memcpy(r + k + 2, a->tu, 16); k += 18; }
        }
        if (!fmt) return err(r, q[0], (uint16_t)s, 0x0A);
        r[1] = (uint8_t)fmt; return k;
    }
    if (q[0] == 0x12) {                                   /* Write Request */
        writes++;
        for (i = 0; i < ndb; i++) if (db[i].h == s && !db[i].t128 && db[i].t16 == 0x2902) {
            if (need_enc && !enc) return err(r, q[0], (uint16_t)s, 0x0F);
            ccc_val = (uint32_t)(q[3] | q[4] << 8); r[0] = 0x13; return 1;
        }
        return err(r, q[0], (uint16_t)s, 0x01);
    }
    return err(r, q[0], 0, 0x06);
}
/* the driver, as bt_le_midi.c: sends req, sets busy, feeds the response; pairing makes the link encrypted. The last result */
static uint32_t run(attc_t *c, int steps)
{
    uint8_t r[32];
    uint32_t res = ATTC_R_NONE;
    while (steps-- > 0 && c->req_n) {
        uint8_t q[8]; uint32_t qn = c->req_n, rn;
        memcpy(q, c->req, qn); c->req_n = 0; c->busy = 1;
        rn = serve(q, qn, r);
        res = attc_rx(c, r, rn);
        if (res == ATTC_R_PAIR) enc = 1;
        if (res == ATTC_R_READY || res == ATTC_R_FAIL) break;
    }
    return res;
}
static void table(int layout)
{
    ndb = 0; ccc_val = 0; writes = 0; need_enc = 0; enc = 0;
    if (layout == 0) {            /* GAP, GATT, device information, MIDI, battery */
        svc16(1, 0x1800); chr16(2, 0x02, 0x2A00); chr16(4, 0x02, 0x2A01);
        svc16(6, 0x1801);
        svc16(7, 0x180A); chr16(8, 0x02, 0x2A29); chr16(10, 0x02, 0x2A24);
        svc128(12, LE_UUID_MIDI_SVC); chr128(13, 0x16, LE_UUID_MIDI_CHR); dsc16(15, 0x2902);
        svc16(16, 0x180F); chr16(17, 0x12, 0x2A19); dsc16(19, 0x2902);
    } else if (layout == 1) {     /* MIDI first, a user description and a vendor 128-bit descriptor before its CCC */
        svc128(1, LE_UUID_MIDI_SVC); chr128(2, 0x16, LE_UUID_MIDI_CHR); dsc16(4, 0x2901); add(5, 0, LE_UUID_MIDI_SVC, 0, 0); dsc16(6, 0x2902);
        svc16(7, 0x1800); chr16(8, 0x02, 0x2A00);
    } else if (layout == 2) {     /* a vendor characteristic before MIDI's, in the MIDI service */
        svc16(1, 0x1800); chr16(2, 0x02, 0x2A00);
        svc128(4, LE_UUID_MIDI_SVC); chr128(5, 0x0A, LE_UUID_MIDI_SVC); chr128(7, 0x16, LE_UUID_MIDI_CHR); dsc16(9, 0x2902);
    } else if (layout == 3) {     /* no MIDI service */
        svc16(1, 0x1800); chr16(2, 0x02, 0x2A00); svc16(4, 0x180F); chr16(5, 0x12, 0x2A19); dsc16(7, 0x2902);
    } else if (layout == 4) {     /* the MIDI characteristic without a CCC, another characteristic after it */
        svc128(1, LE_UUID_MIDI_SVC); chr128(2, 0x06, LE_UUID_MIDI_CHR); chr16(4, 0x12, 0x2A19); dsc16(6, 0x2902);
    }
}

int main(void)
{
    attc_t c;
    uint8_t note[] = {0x1B, 14, 0, 0x80, 0x80, 0xB0, 24, 127}, other[] = {0x1B, 18, 0, 0x80, 0x80, 0xB0, 1, 1};

    /* ---- the client */
    table(0); attc_start(&c, 0, 0);
    ok(run(&c, 20) == ATTC_R_READY && c.val == 14 && c.ccc == 15 && ccc_val == 1, "client: services, characteristics, descriptors, notifications on (layout 0)");
    ok(attc_rx(&c, note, sizeof note) == ATTC_R_MIDI && attc_rx(&c, other, sizeof other) == ATTC_R_NONE,
       "client: a notification of the MIDI value is MIDI; another characteristic's is not");
    table(1); attc_start(&c, 0, 0);
    ok(run(&c, 20) == ATTC_R_READY && c.val == 3 && c.ccc == 6, "client: MIDI first, the CCC after a description and a 128-bit descriptor (layout 1)");
    table(2); attc_start(&c, 0, 0);
    ok(run(&c, 20) == ATTC_R_READY && c.val == 8 && c.ccc == 9, "client: a vendor characteristic before MIDI's in its service (layout 2)");
    table(3); attc_start(&c, 0, 0);
    ok(run(&c, 20) == ATTC_R_FAIL && c.err == 0xF0 + ATTC_SVC && !writes, "client: no MIDI service: fails, writes nothing");
    ok(attc_not_midi(&c) && c.fail_st == ATTC_SVC, "client: no MIDI service: no BLE MIDI there (test build 33: skipped)");
    table(4); attc_start(&c, 0, 0);
    ok(run(&c, 20) == ATTC_R_FAIL && c.err == 0xF0 + ATTC_DSC && !writes, "client: no CCC before the next characteristic: fails, writes nothing");
    ok(attc_not_midi(&c), "client: no CCC: no usable BLE MIDI either");
    table(0); need_enc = 1; attc_start(&c, 0, 0);
    ok(run(&c, 20) == ATTC_R_READY && enc && c.pair && ccc_val == 1 && writes == 2, "client: encryption asked: pairing once, the same write again, then on");
    table(0); need_enc = 1; attc_start(&c, 0, 0);
    {   /* pairing that does not make the link encrypted: the second refusal ends it */
        uint8_t q[8], r[32]; uint32_t res = ATTC_R_NONE, rn;
        for (int i = 0; i < 20 && c.req_n && res != ATTC_R_FAIL; i++) { memcpy(q, c.req, c.req_n); rn = serve(q, c.req_n, r); c.req_n = 0; c.busy = 1; res = attc_rx(&c, r, rn); }
        ok(res == ATTC_R_FAIL && c.err == 0x0F, "client: still refused after pairing: fails");
        ok(!attc_not_midi(&c), "client: a refusal for security is not \"no BLE MIDI\": not skipped");
    }
    {   /* (test build 33) the service search itself refused for security, after pairing: not skipped either */
        uint8_t e5[5] = {0x01, 0x10, 0x01, 0x00, 0x05};
        uint32_t r1, r2;
        attc_start(&c, 0, 0); c.busy = 1; c.req_n = 0;
        r1 = attc_rx(&c, e5, 5);
        c.busy = 1; c.req_n = 0;
        r2 = attc_rx(&c, e5, 5);
        ok(r1 == ATTC_R_PAIR && r2 == ATTC_R_FAIL && c.fail_st == ATTC_SVC && !attc_not_midi(&c),
           "client: services refused for security twice: fails at the service step, not \"no BLE MIDI\"");
    }
    table(0); attc_start(&c, 14, 15);
    ok(c.st == ATTC_SUB && run(&c, 3) == ATTC_R_READY && writes == 1, "client: the same footswitch again: straight to the notifications, one write");
    table(1); attc_start(&c, 14, 15);
    ok(run(&c, 3) == ATTC_R_FAIL && c.err == 0x01, "client: a stale handle is refused: fails (the driver forgets the handles, then finds them)");
    {   /* responses nobody asked for, and junk while a request is out */
        uint8_t rsp[] = {0x13}, junk[] = {0x0B, 1, 2}, e0[] = {0x01, 0x10, 1, 0, 0x0A};
        table(0); attc_start(&c, 0, 0);
        ok(attc_rx(&c, rsp, 1) == ATTC_R_NONE && attc_rx(&c, e0, 5) == ATTC_R_NONE && c.st == ATTC_SVC, "client: a response with no request out is not taken");
        c.req_n = 0; c.busy = 1;
        ok(attc_rx(&c, junk, 3) == ATTC_R_NONE && c.busy && c.st == ATTC_SVC, "client: an unexpected response leaves the request awaited");
    }

    /* ---- the advertisements */
    {
        le_cen_scan_t s;
        uint8_t t = 9, a[6];
        static const uint8_t A1[6] = {1, 2, 3, 4, 5, 6}, A2[6] = {9, 9, 9, 9, 9, 9};
        uint8_t adv[64], rsp[64], ev[64];
        uint32_t n;
        #define REPORT(et, at, ad, data, dn, rssi) (ev[0] = 0xE2, ev[1] = (uint8_t)(10 + (dn)), ev[2] = (et), ev[3] = (at), memcpy(ev + 4, (ad), 6), \
                                                     ev[10] = (uint8_t)(rssi), ev[11] = (uint8_t)(dn), memcpy(ev + 12, (data), (dn)), (uint32_t)(12 + (dn)))
        memset(&s, 0, sizeof s);
        adv[0] = 2; adv[1] = 0x01; adv[2] = 0x06; adv[3] = 17; adv[4] = 0x07; memcpy(adv + 5, LE_UUID_MIDI_SVC, 16);   /* flags, MIDI */
        rsp[0] = 10; rsp[1] = 0x09; memcpy(rsp + 2, "Chocolate", 9);
        n = REPORT(0, 0, A1, adv, 21, -60);
        ok(!le_cen_report(&s, ev, n, 100, &t, a), "scan: the MIDI UUID alone is not enough (the name is in the scan response)");
        n = REPORT(4, 0, A1, rsp, 11, -61);
        ok(le_cen_report(&s, ev, n, 110, &t, a) && t == 0 && !memcmp(a, A1, 6), "scan: then the scan response's name: the footswitch, its address");
        memset(&s, 0, sizeof s);
        rsp[0] = 9; rsp[1] = 0x09; memcpy(rsp + 2, "FootCtrl", 8);
        n = REPORT(0, 0, A1, adv, 21, -60); le_cen_report(&s, ev, n, 100, &t, a);
        n = REPORT(4, 0, A1, rsp, 10, -60);
        ok(le_cen_report(&s, ev, n, 110, &t, a) && !memcmp(a, A1, 6), "scan: the name FootCtrl, as the regular Chocolate advertises: the footswitch");
        memset(&s, 0, sizeof s);
        rsp[0] = 6; rsp[1] = 0x08; memcpy(rsp + 2, "FOOTC", 5);
        n = REPORT(0, 0, A1, adv, 21, -60); le_cen_report(&s, ev, n, 100, &t, a);
        n = REPORT(4, 0, A1, rsp, 7, -60);
        ok(!le_cen_report(&s, ev, n, 110, &t, a), "scan: a short name cut before the whole word: not taken");
        memset(&s, 0, sizeof s);
        rsp[0] = 17; rsp[1] = 0x09; memcpy(rsp + 2, "M-VAVE CHOCOLATE", 16);
        n = REPORT(4, 1, A2, rsp, 18, -50); le_cen_report(&s, ev, n, 10, &t, a);
        n = REPORT(0, 1, A2, adv, 21, -90);
        ok(!le_cen_report(&s, ev, n, 20, &t, a), "scan: too far (RSSI -90): not yet");
        n = REPORT(0, 1, A2, adv, 21, -70);
        ok(le_cen_report(&s, ev, n, 30, &t, a) && t == 1, "scan: nearer, the name in upper case: taken, with its random address type");
        memset(&s, 0, sizeof s);
        n = REPORT(3, 0, A1, adv, 21, -60); le_cen_report(&s, ev, n, 10, &t, a);
        rsp[0] = 10; rsp[1] = 0x09; memcpy(rsp + 2, "Chocolate", 9);
        n = REPORT(4, 0, A1, rsp, 11, -60);
        ok(!le_cen_report(&s, ev, n, 20, &t, a), "scan: an advertiser that is not connectable is never taken");
        memset(&s, 0, sizeof s);
        rsp[0] = 5; rsp[1] = 0x09; memcpy(rsp + 2, "WIDI", 4);
        n = REPORT(0, 0, A2, adv, 21, -40); le_cen_report(&s, ev, n, 10, &t, a);
        n = REPORT(4, 0, A2, rsp, 7, -40);
        ok(!le_cen_report(&s, ev, n, 20, &t, a), "scan: another BLE MIDI device, by its name: never taken");
        s.known = 1; memcpy(s.addr, A1, 6); s.type = 0;
        n = REPORT(0, 0, A1, (const uint8_t *)"\x02\x01\x06", 3, -95);
        ok(le_cen_report(&s, ev, n, 30, &t, a) && !memcmp(a, A1, 6), "scan: the footswitch known: its address alone is enough, whatever it shows");
        rsp[0] = 10; rsp[1] = 0x09; memcpy(rsp + 2, "Chocolate", 9);
        n = REPORT(0, 0, A2, adv, 21, -40); le_cen_report(&s, ev, n, 40, &t, a);
        n = REPORT(4, 0, A2, rsp, 11, -40);
        ok(!le_cen_report(&s, ev, n, 50, &t, a), "scan: and another Chocolate is then never taken");
        memset(&s, 0, sizeof s);
        uint8_t bad[] = {30, 0x07, 1, 2, 3};
        n = REPORT(0, 0, A1, bad, 5, -40);
        ok(!le_cen_report(&s, ev, n, 10, &t, a) && s.reports == 1, "scan: a malformed advertisement is read safely");
        for (uint32_t k = 0; k < 9; k++) { uint8_t ad[6] = {(uint8_t)k, 7, 7, 7, 7, 7}; n = REPORT(0, 0, ad, adv, 21, -40); le_cen_report(&s, ev, n, 100 + k, &t, a); }
        n = REPORT(4, 0, (const uint8_t *)"\x08\x07\x07\x07\x07\x07", rsp, 11, -40);
        ok(le_cen_report(&s, ev, n, 200, &t, a) && a[0] == 8, "scan: nine advertisers, six kept: the latest are, the oldest go");
    }
    {   /* test build 33: the filter without the MIDI UUID, its view for the log, and the addresses skipped */
        le_cen_scan_t s;
        uint8_t t = 9, a[6], ev[64], fl[3] = {2, 0x01, 0x06}, rsp[16], uu[21];
        static const uint8_t A1[6] = {1, 2, 3, 4, 5, 6}, A2[6] = {7, 7, 7, 7, 7, 7}, A3[6] = {3, 3, 3, 3, 3, 3};
        uint32_t n;
        #define REP(et, ad, data, dn, rssi) (ev[0] = 0xE2, ev[1] = (uint8_t)(10 + (dn)), ev[2] = (et), ev[3] = 0, memcpy(ev + 4, (ad), 6), \
                                             ev[10] = (uint8_t)(rssi), ev[11] = (uint8_t)(dn), memcpy(ev + 12, (data), (dn)), (uint32_t)(12 + (dn)))
        memset(&s, 0, sizeof s);
        rsp[0] = 9; rsp[1] = 0x09; memcpy(rsp + 2, "FootCtrl", 8);
        n = REP(0, A1, fl, 3, -70); le_cen_report(&s, ev, n, 10, &t, a);
        n = REP(4, A1, rsp, 10, -71);
        ok(le_cen_report(&s, ev, n, 20, &t, a) && !memcmp(a, A1, 6), "scan: FootCtrl without the MIDI UUID (Manufacturer mode), connectable: taken");
        ok(s.name_n == 1 && s.uuid_n == 0 && s.conn_n == 1 && s.best_set && s.best == -71 && s.last_flags == 6 && s.last_rssi == -71,
           "scan: the log's view: one name, no UUID, one connectable report, best -71, last flags 6 at -71");
        ok(s.last_ctype == 1, "scan: its connectable reports were ADV_IND (type 0, kept as 1: the log's LF .../0)");
        n = REP(1, A1, fl, 3, -70); le_cen_report(&s, ev, n, 30, &t, a);
        ok(s.last_ctype == 2, "scan: then a directed one (type 1, kept as 2: LF .../1, another central's)");
        memset(&s, 0, sizeof s);
        n = REP(0, A2, fl, 3, -86); le_cen_report(&s, ev, n, 10, &t, a);
        n = REP(4, A2, rsp, 10, -86);
        ok(!le_cen_report(&s, ev, n, 20, &t, a) && s.best == -86 && s.last_flags == 6, "scan: at -86 dBm, below the floor: not taken, but seen");
        n = REP(4, A2, rsp, 10, -85);
        ok(le_cen_report(&s, ev, n, 30, &t, a), "scan: at -85 dBm: taken");
        memset(&s, 0, sizeof s);
        n = REP(4, A3, rsp, 10, -60); le_cen_report(&s, ev, n, 10, &t, a);
        ok(!le_cen_report(&s, ev, n, 20, &t, a) && s.last_flags == 2, "scan: the name only in scan responses, never connectable: not taken (flags 2)");
        uu[0] = 17; uu[1] = 0x07; memcpy(uu + 2, LE_UUID_MIDI_SVC, 16); uu[18] = 2; uu[19] = 0x01; uu[20] = 0x06;
        n = REP(0, A3, uu, 21, -60);
        ok(le_cen_report(&s, ev, n, 30, &t, a) && s.uuid_n == 1 && s.last_flags == 7, "scan: the same address connectable with the UUID: taken (flags 7)");
        memset(&s, 0, sizeof s);
        le_cen_skip(&s, A1);
        n = REP(0, A1, fl, 3, -50); le_cen_report(&s, ev, n, 10, &t, a);
        n = REP(4, A1, rsp, 10, -50);
        ok(!le_cen_report(&s, ev, n, 20, &t, a) && s.skip_n == 1 && s.name_n == 1, "scan: an address skipped (no BLE MIDI there): never taken, still counted");
        n = REP(0, A2, fl, 3, -50); le_cen_report(&s, ev, n, 30, &t, a);
        n = REP(4, A2, rsp, 10, -50);
        ok(le_cen_report(&s, ev, n, 40, &t, a) && !memcmp(a, A2, 6), "scan: another FootCtrl: taken");
        le_cen_skip(&s, A1);
        ok(s.skip_n == 1, "scan: the same address skipped twice counts once");
        s.known = 1; memcpy(s.addr, A2, 6);
        le_cen_skip(&s, A2);
        ok(!s.known && s.skip_n == 2 && le_cen_skipped(&s, A2), "scan: the known footswitch skipped: no longer the only one taken");
        for (uint32_t k = 0; k < 4; k++) { uint8_t ad[6] = {9, 9, 9, 9, 9, (uint8_t)k}; le_cen_skip(&s, ad); }
        ok(s.skip_n == 6 && !le_cen_skipped(&s, A1) && !le_cen_skipped(&s, A2), "scan: four kept: the oldest skipped addresses come back");
    }
    printf("\n%s\n", failed ? "BLE central tests FAILED" : "all BLE central tests passed");
    return failed ? 1 : 0;
}
