/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Test build 23: Bluetooth MIDI (BLE) beside the classic link that plays the phone's music, as M-VAVE's own firmware runs both
 * (V22: its GATT table holds the BLE-MIDI service and characteristic next to JL_A2DP, read 2026-11-12). The LE side is ble/ble_midi.c's,
 * which VinSpee ran on a BlackBox with a Chocolate Plus (its footswitches A-D switched DRIVE, MOD, DLY, REV; docs/BLE-TEST.md on
 * fm/openpocket-ble-midi-link): the same GATT table (GAP name, GATT, the MIDI service and its characteristic: read empty, write
 * without response, notify), Just Works pairing on request, the same MIDI reader (px_ble_midi.h through px_midi_io.h, the USB
 * path's px_midi_in). Its names here are le_* so as not to meet the classic image's (bt_a2dp.c: ble_conn is the phone's link).
 * Differences from the BLE image:
 *  - the classic stack starts as before (bt_a2dp.c bt_app_task, btstack_init) with the LE controller and host linked beside it
 *    (bt/members.txt, bt/bt_cfg.c: BT_MODULE_CLASSIC | BT_MODULE_LE); the host calls ble_profile_init (the library's is weak);
 *  - advertising starts on the stack's INIT_OK (bt_a2dp.c), every 500 ms as the stock firmware (BLE MIDI image: 100 ms), to
 *    leave the radio to the music;
 *  - the LE address is the classic one with its top octet 0x06 instead of 0x02 (both locally administered, unicast), so each
 *    pedal has its own and a phone does not take the two for one device;
 *  - a disconnection restarts the advertising only when it is the LE link's (the classic link's go to bt_a2dp.c).
 * Test build 27: the web editor over BLE MIDI too (px_editor.h ED_BT): its SysEx comes in the MIDI writes (px_midi_io.h), its
 * replies go back as notifications of the MIDI characteristic (below, ed_bt_send and ble_profile_loop). And two LE links at once
 * (bt_le_links.h, bt_cfg.c): the Chocolate on one, the editor on the other, beside the phone's music; the pedal advertises
 * while a link is free. The MIDI of either link reaches the pedal; the editor's replies go to the link that asked. */
#include "ble_adv.h"                                     /* (ble/: the advertising data, the scan response with the name) */
#include "bt_le_links.h"                                 /* (test build 27: which link a handle is, when to advertise) */

extern void le_device_db_init(void);
extern void sm_init(void);
extern void sm_set_io_capabilities(uint8_t io);
extern void sm_set_authentication_requirements(uint8_t auth);
extern void sm_set_encryption_key_size_range(uint8_t min, uint8_t max);
extern void sm_set_request_security(int enable);
extern void sm_event_callback_set(void (*cb)(uint8_t, uint16_t, uint8_t *, uint16_t));
extern void sm_just_works_confirm(uint16_t handle);
extern void att_server_init(const uint8_t *db, uint16_t (*rd)(uint16_t, uint16_t, uint16_t, uint8_t *, uint16_t),
                            int (*wr)(uint16_t, uint16_t, uint16_t, uint16_t, uint8_t *, uint16_t, uint8_t *));
extern void att_server_register_packet_handler(void (*cb)(uint8_t, uint16_t, uint8_t *, uint16_t));
extern void hci_event_callback_set(void (*cb)(uint8_t, uint16_t, uint8_t *, uint16_t));
extern void le_l2cap_register_packet_handler(void (*cb)(uint8_t, uint16_t, uint8_t *, uint16_t));
extern void att_set_ccc_config(uint16_t handle, uint16_t cfg);
extern uint16_t att_get_ccc_config(uint16_t handle);
extern int ble_user_cmd_prepare(int cmd, int argc, ...);
extern int le_controller_set_mac(const uint8_t *mac);
/* test build 27, the editor's replies (all three in members already linked: att_server.c, hci_vendor.c, btstack_task.c) */
extern int att_server_notify(uint16_t con, uint16_t handle, uint8_t *value, uint16_t len);   /* 0 sent, 87 no room now */
extern uint8_t hci_le_get_acl_packet_num(uint16_t con);   /* the link's packets with the controller, not yet acknowledged */
extern void stack_run_loop_resume(void);                  /* a message to the host's task (type 255): it runs its loop hooks */

enum { LE_CMD_ADV_ENABLE = 1, LE_CMD_ADV_PARAM, LE_CMD_ADV_DATA, LE_CMD_RSP_DATA };
enum { LE_HCI_EVENT_PACKET = 4, LE_EV_DISCONNECTION_COMPLETE = 0x05, LE_EV_META = 0x3E, LE_SUB_CONNECTION_COMPLETE = 0x01,
       LE_SUB_CONNECTION_UPDATE_COMPLETE = 0x03, LE_SUB_ENHANCED_CONNECTION_COMPLETE = 0x0A, LE_SM_JUST_WORKS_REQUEST = 0xD0 };
enum { LE_IO_NO_INPUT_NO_OUTPUT = 3, LE_AUTHREQ_BONDING = 1 };
enum { LE_H_GAP_NAME = 0x0003, LE_H_MIDI = 0x0007, LE_H_MIDI_CCC = 0x0008 };
enum { LE_ADV_INTERVAL = 0x0320 };                             /* 800 x 0.625 ms = 500 ms, the stock firmware's (STOCK-FIRMWARE.md 19.4) */
#define LE_MIDI_SERVICE 0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
#define LE_MIDI_CHAR 0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77
/* the GATT table, ble/ble_midi.c's to the byte */
static const uint8_t le_profile[] = {
    0x0A, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,                 /* 0001 primary service 1800 */
    0x0D, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2A, /* 0002 characteristic 2A00: read */
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2A,                             /* 0003 value: dynamic */
    0x0A, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, 0x01, 0x18,                 /* 0004 primary service 1801 */
    0x18, 0x00, 0x02, 0x00, 0x05, 0x00, 0x00, 0x28, LE_MIDI_SERVICE,            /* 0005 primary service MIDI */
    0x1B, 0x00, 0x02, 0x00, 0x06, 0x00, 0x03, 0x28, 0x16, 0x07, 0x00, LE_MIDI_CHAR, /* 0006 characteristic: read, write w/o response, notify */
    0x16, 0x00, 0x16, 0x03, 0x07, 0x00, LE_MIDI_CHAR,                           /* 0007 value: dynamic */
    0x0A, 0x00, 0x0A, 0x01, 0x08, 0x00, 0x02, 0x29, 0x00, 0x00,                 /* 0008 client characteristic configuration */
    0x00, 0x00,
};

static le_links_t le_lk;                                       /* the LE links (test build 27: two; up to 26 one, le_conn) */
static uint32_t le_writes, le_pairs, le_drop, le_adv_rc, le_adv_n;
static uint8_t le_adv_ok;                                      /* advertising asked for at least once */
static uint8_t le_mac[6];
static uint32_t le_adv_kick;                                   /* (ble_profile_loop: advertising asked again, every 2 s) */
typedef char le_links_check[LE_LINKS == ED_BT_LINKS ? 1 : -1];   /* (px_editor.h keeps a frame for each link) */
/* test build 27: the MIDI characteristic's client configuration as each link's central wrote it (bit 0: notifications). The
 * library keeps it only after an att_send_init this image never makes (att_send.c: att_set_ccc_config does nothing without its
 * buffer), so it is kept here, for the central's reads and the log; the replies do not wait for it: a central that asks the
 * editor something over BLE MIDI listens (one bonded earlier may not write it again) */
static uint16_t le_ccc[LE_LINKS];

/* ---- test build 27: the web editor's replies (px_editor.h ED_BT), as BLE MIDI notifications of the MIDI characteristic.
 * ed_bt_send copies a reply into le_tx; the host's task sends it a packet at a time from ble_profile_loop, which it calls after
 * each of its messages, the controller's acknowledgements of sent packets (Number of Completed Packets, hci_vendor.c) among them:
 * each acknowledgement brings the next packet. A packet is one link-layer packet (20 bytes of value: with the ATT and L2CAP
 * heads, the 27 bytes the controller sends at once, whatever MTU the central asked for), and only a few wait unacknowledged in
 * the controller (le_tx_inflight): about 2 packets for each 15 ms of the link's interval, so the editor takes the same small
 * share of the radio's time whatever interval the central chose, about 2.5 KB/s of SysEx (a TEXTS reply in 0.4 s), and the
 * classic link keeps the rest for the music. The ATT server's own answers share those slots. */
#define LE_TX_BYTES 1024u   /* the longest reply sent: TEXTS, 918 bytes (DIAG's 2,304 are for USB: dropped here) */
#define LE_TX_PKT 20u
#define LE_TX_GIVE_UP 3000u /* ms without a packet sent: the reply is dropped (the editor asks again; the link may be gone) */
static uint32_t le_tx_inflight(uint32_t intv)   /* intv in 1.25 ms: 1 at 7.5 ms, 2 at 11.25 and 15 ms, 3 at 22.5 ms, 4 from
                                                 * 30 ms (of the controller's 5 LE buffers, bt_cfg.c, both links': one is left) */
{
    const uint32_t k = (intv + 3u) / 6u;
    return k < 1u ? 1u : k > 4u ? 4u : k;
}
static uint8_t le_tx[LE_TX_BYTES];
static uint32_t le_tx_at, le_tx_n, le_tx_link;                 /* le_tx[le_tx_at, le_tx_n) is still to send, to link le_tx_link */
static uint32_t le_tx_pkts, le_tx_lost;                        /* notifications sent; replies dropped (no link, too long, stuck) */
static uint32_t le_tx_seen, le_tx_ms, le_tx_kick;              /* (the main loop's watch over a reply that does not move) */

/* px_editor.h, every main-loop pass: the buffer is empty. A reply that sends nothing for LE_TX_GIVE_UP ms is dropped; every
 * 50 ms the host's task is woken in case no acknowledgement does it */
static int ed_bt_ready(uint32_t now)
{
    if (le_tx_at >= le_tx_n) return 1;
    if (le_tx_pkts != le_tx_seen) { le_tx_seen = le_tx_pkts; le_tx_ms = now; }
    else if (!le_lk.conn[le_tx_link] || now - le_tx_ms > LE_TX_GIVE_UP) { le_tx_at = le_tx_n; le_tx_lost++; return 1; }
    if (now - le_tx_kick >= 50u) { le_tx_kick = now; stack_run_loop_resume(); }
    return 0;
}
/* px_editor.h ed_send: a reply, F0 .. F7, for link `link`, from the main loop; px_editor.h calls it only with the buffer empty */
static void ed_bt_send(uint32_t link, const uint8_t *p, uint32_t n)
{
    if (link >= LE_LINKS || !le_lk.conn[link] || n > LE_TX_BYTES || le_tx_at < le_tx_n) { le_tx_lost++; return; }
    memcpy(le_tx, p, n);
    le_tx_at = 0;
    le_tx_n = n;
    le_tx_link = link;
    le_tx_seen = le_tx_pkts;
    le_tx_ms = le_tx_kick = ms_now();
    stack_run_loop_resume();                                   /* the host's task sends it */
}
static int le_adv_on(void);
/* the host's task, after each of its messages (btstack_task.c calls this hook; the library's is weak). Up to test build 26 an
 * empty one in bt_glue.c. Also, while a link is free, advertising is asked again every 2 s: asked while it runs it does nothing
 * (ll_adv.c ll_adv_open: already open), and it starts it again if an enable after a link's end found no free entity yet */
void ble_profile_loop(void)
{
    uint8_t pkt[LE_TX_PKT];
    const uint32_t h = le_lk.conn[le_tx_link];
    while (le_tx_at < le_tx_n && h && hci_le_get_acl_packet_num((uint16_t)h) < le_tx_inflight(le_lk.intv[le_tx_link])) {
        uint32_t at = le_tx_at;
        const uint32_t k = px_blem_pack(le_tx, le_tx_n, &at, ble_ms(), pkt, LE_TX_PKT);
        if (!k || att_server_notify((uint16_t)h, LE_H_MIDI, pkt, (uint16_t)k)) break;   /* (no room now: the next message tries) */
        le_tx_at = at;
        le_tx_pkts++;
    }
    if (le_adv_ok && le_links_up(&le_lk) < LE_LINKS && ble_ms() - le_adv_kick >= 2000u) {
        le_adv_kick = ble_ms();
        if (le_lk.adv) (void)ble_user_cmd_prepare(LE_CMD_ADV_ENABLE, 1, 1);
        else if (!le_adv_on()) le_lk.adv = 1;
    }
}
/* link i came or went: nothing of the last one's on it is kept or sent (px_editor.h ed_bt_lost: its frame, its pushes) */
static void le_link_reset(uint32_t i)
{
    le_ccc[i] = 0;
    if (le_tx_link == i) le_tx_at = le_tx_n = 0;
    ed_bt_lost(i);
}
/* advertising, when a link is free and none is asked for (after a link is made, and after one ends) */
static void le_adv_more(void)
{
    if (!le_links_want_adv(&le_lk)) return;
    le_adv_rc = (uint32_t)le_adv_on();
    if (!le_adv_rc) le_lk.adv = 1;
    le_adv_kick = ble_ms();
}

static int le_adv_on(void)
{
    ble_user_cmd_prepare(LE_CMD_ADV_PARAM, 3, LE_ADV_INTERVAL, BLE_ADV_IND, BLE_ADV_CHMAP);
    ble_user_cmd_prepare(LE_CMD_ADV_DATA, 2, (int)sizeof ble_adv_data, ble_adv_data);
    ble_user_cmd_prepare(LE_CMD_RSP_DATA, 2, (int)sizeof ble_scan_rsp, ble_scan_rsp);
    le_adv_n++;
    return ble_user_cmd_prepare(LE_CMD_ADV_ENABLE, 1, 1);
}

static uint16_t le_att_read(uint16_t con, uint16_t handle, uint16_t offset, uint8_t *buf, uint16_t size)
{
    if (handle == LE_H_GAP_NAME) {
        uint16_t len = BLE_NAME_LEN, n;
        if (!buf) return len;
        if (offset >= len) return 0;
        n = (uint16_t)(len - offset) < size ? (uint16_t)(len - offset) : size;
        memcpy(buf, ble_scan_rsp + 2 + offset, n);
        return n;
    }
    if (handle == LE_H_MIDI_CCC) {                             /* (test build 27: was always 0) */
        const int i = le_links_find(&le_lk, con);
        const uint16_t c = i >= 0 ? le_ccc[i] : 0u;
        if (buf && size >= 2u) { buf[0] = (uint8_t)c; buf[1] = (uint8_t)(c >> 8); }
        return 2;
    }
    return 0;                                                   /* the MIDI value reads empty, as the specification wants */
}
/* (con: the link's handle, att_db.c: the connection's con_handle) */
static int le_att_write(uint16_t con, uint16_t handle, uint16_t mode, uint16_t offset, uint8_t *buf, uint16_t size, uint8_t *sig)
{
    const int i = le_links_find(&le_lk, con);
    (void)mode; (void)offset; (void)sig;
    if (handle == LE_H_MIDI) {                                 /* (px_midi_io.h: the USB path's px_midi_in, test build 27 the
                                                                * editor's SysEx too, kept for the link it came on) */
        le_writes++;
        px_midi_ble_write_link(i >= 0 ? (uint32_t)i : LE_LINKS, buf, size);
    } else if (handle == LE_H_MIDI_CCC && size >= 1u) {
        if (i >= 0) le_ccc[i] = (uint16_t)(buf[0] | (size >= 2u ? buf[1] << 8 : 0));
        att_set_ccc_config(handle, buf[0]);
    }
    return 0;
}

/* the host's events: the LE link's (the classic link's reach bt_a2dp.c through update_bt_current_status) */
static void le_on_event(uint8_t type, uint16_t chan, uint8_t *p, uint16_t size)
{
    (void)chan; (void)size;
    if (type != LE_HCI_EVENT_PACKET) return;
    if (p[0] == LE_EV_DISCONNECTION_COMPLETE) {                 /* (a classic disconnection, or the same event again: not ours) */
        const int i = le_links_ended(&le_lk, (uint32_t)(p[3] | p[4] << 8) & 0x0FFFu);
        if (i >= 0) {
            le_drop = p[5];
            le_link_reset((uint32_t)i);                         /* (test build 27: the editor's reply and frame on that link) */
            le_adv_more();                                      /* (a link free: advertising, if it is not on yet) */
        }
    } else if (p[0] == LE_EV_META) {
        const uint8_t *q = 0;
        const uint32_t h = (uint32_t)(p[4] | p[5] << 8) & 0x0FFFu;
        if (p[2] == LE_SUB_CONNECTION_COMPLETE && !p[3]) q = p + 14;
        else if (p[2] == LE_SUB_ENHANCED_CONNECTION_COMPLETE && !p[3]) q = p + 26;
        else if (p[2] == LE_SUB_CONNECTION_UPDATE_COMPLETE && !p[3]) q = p + 6;
        if (q && p[2] == LE_SUB_CONNECTION_UPDATE_COMPLETE) le_links_update(&le_lk, h, (uint32_t)(q[0] | q[1] << 8));
        else if (q) {                                           /* a link made (once, whatever the callbacks it reaches) */
            const int i = le_links_made(&le_lk, h, (uint32_t)(q[0] | q[1] << 8));
            if (i >= 0) {
                le_link_reset((uint32_t)i);
                le_adv_more();                                  /* (test build 27: the other link stays open to a central) */
            }
        }
    }
}
static void le_on_sm(uint8_t type, uint16_t chan, uint8_t *p, uint16_t size)
{
    (void)chan; (void)size;
    if (type != LE_HCI_EVENT_PACKET) return;
    if (p[0] == LE_SM_JUST_WORKS_REQUEST) { le_pairs++; sm_just_works_confirm((uint16_t)(p[2] | p[3] << 8)); }
}

/* the host's LE start-up hook (btstack_main.c's is weak): the security manager, the GATT server and its table, the callbacks */
void ble_profile_init(void)
{
    BT_CRUMB("ble_profile_init");
    le_device_db_init();
    sm_init();
    sm_set_io_capabilities(LE_IO_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(LE_AUTHREQ_BONDING);
    sm_set_encryption_key_size_range(7, 16);
    sm_set_request_security(0);
    sm_event_callback_set(le_on_sm);
    att_server_init(le_profile, le_att_read, le_att_write);
    att_server_register_packet_handler(le_on_event);
    hci_event_callback_set(le_on_event);
    le_l2cap_register_packet_handler(le_on_event);
}

/* on the stack's INIT_OK (bt_a2dp.c), once: this pedal's LE address, then advertising */
static void bt_le_advertise(void)
{
    uint32_t i;
    if (le_adv_ok) return;
    le_adv_ok = 1;
    for (i = 0; i < 6u; i++) le_mac[i] = bt_mac[i];             /* (bt_glue.c: from the flash's unique ID) */
    le_mac[5] = 0x06;                                           /* (the most significant octet: 0x02 is the classic address's) */
    le_controller_set_mac(le_mac);
    BT_CRUMB("le advertise");
    le_adv_more();
}

/* the log's words for the LE side, appended to the classic lines (bt_a2dp.c): "LE 2 L3 W134 P1 A4/0 I12/24 IRQ 812/97" =
 * the links up (test build 27: up to two), links made so far, MIDI writes, Just Works pairings, advertising asked n times / its
 * last return, the links' intervals (1.25 ms units), the LE baseband's two interrupts (45 event, 29 receive; ble/ble_isr.S
 * counts them) */
static char *bt_le_text(char *o)
{
    uint32_t i, k = 0;
    o = ble__put(o, " LE "); o = bt__dec(o, le_links_up(&le_lk));
    o = ble__put(o, " L"); o = bt__dec(o, le_lk.made);
    o = ble__put(o, " W"); o = bt__dec(o, le_writes);
    o = ble__put(o, " P"); o = bt__dec(o, le_pairs);
    o = ble__put(o, " A"); o = bt__dec(o, le_adv_n); *o++ = '/'; o = bt__dec(o, le_adv_rc);
    for (i = 0; i < LE_LINKS; i++) if (le_lk.conn[i]) { o = ble__put(o, k++ ? "/" : " I"); o = bt__dec(o, le_lk.intv[i]); }
    if (le_drop) { o = ble__put(o, " D"); o = ble__hex(o, le_drop); }
    o = ble__put(o, " IRQ "); o = bt__dec(o, ble_cnt_irq_ev); *o++ = '/'; o = bt__dec(o, ble_cnt_irq_rx);
    return o;
}
/* test build 27, at the MIDI line's end (bt_a2dp.c ble_midi_line), once the editor has used Bluetooth: "ED 12/340 N1/0" = its
 * frames answered over Bluetooth / notifications sent, each link's client configuration (1: notifications on), then X and the
 * replies dropped, SX and the SysEx dropped, when there are any */
static char *bt_le_ed_text(char *o)
{
    if (!ed_bt_frames && !ed_bt_rx[0].got && !ed_bt_rx[1].got && !ed_bt_rx[0].drop && !ed_bt_rx[1].drop && !le_tx_lost) return o;
    o = ble__put(o, " ED "); o = bt__dec(o, ed_bt_frames); *o++ = '/'; o = bt__dec(o, le_tx_pkts);
    o = ble__put(o, " N"); o = bt__dec(o, le_ccc[0]); *o++ = '/'; o = bt__dec(o, le_ccc[1]);
    if (le_tx_lost) { o = ble__put(o, " X"); o = bt__dec(o, le_tx_lost); }
    if (ed_bt_rx[0].drop + ed_bt_rx[1].drop) { o = ble__put(o, " SX"); o = bt__dec(o, ed_bt_rx[0].drop + ed_bt_rx[1].drop); }
    return o;
}
