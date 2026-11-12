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
 * while a link is free. The MIDI of either link reaches the pedal; the editor's replies go to the link that asked.
 * Test build 28: the pedal as the central of a footswitch that is only a peripheral (the regular M-VAVE Chocolate): while its
 * link is down the pedal scans, at 5 % of the radio's time, for the BLE MIDI UUID and the name (bt_le_central.h), connects to
 * it, finds its MIDI characteristic with an ATT client of its own and subscribes; its notifications go to the same MIDI
 * reader. The controller's master link is ll_master.c's (third_party/jieli-ble, test build 28). DLY held at power-on: no
 * central role, LE as in test build 27 (le_cen_off, openpocket.c). */
#include "ble_adv.h"                                     /* (ble/: the advertising data, the scan response with the name) */
#include "bt_le_links.h"                                 /* (test build 27: which link a handle is, when to advertise) */
#include "bt_le_central.h"                               /* (test build 28: which advertisement is the footswitch's, the ATT client) */

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
/* test build 28, the central role (att_dispatch.c, l2cap.c, sm.c: members already linked) */
extern void att_dispatch_register_client(void (*cb)(uint8_t, uint16_t, uint8_t *, uint16_t));   /* every odd ATT opcode */
extern void att_dispatch_register_server(void (*cb)(uint8_t, uint16_t, uint8_t *, uint16_t));   /* (test build 33: the spy) */
extern int att_dispatch_client_can_send_now(uint16_t con);
extern int l2cap_reserve_packet_buffer(void);             /* 1: reserved now */
extern uint8_t *l2cap_get_outgoing_buffer(void);          /* (after the HCI and L2CAP heads) */
extern int l2cap_send_prepared_connectionless(uint16_t con, uint16_t cid, uint16_t len);   /* 0 sent; else the buffer stays ours */
extern void l2cap_release_packet_buffer(void);
extern uint8_t sm_api_request_pairing(uint16_t con);

enum { LE_CMD_ADV_ENABLE = 1, LE_CMD_ADV_PARAM, LE_CMD_ADV_DATA, LE_CMD_RSP_DATA, LE_CMD_DISCONNECT = 5 };
/* test build 28: the host's commands for the central role (btstack_main.c __ble_thread_loop_handler): scan parameters (type,
 * interval, window), scan enable (enable, filter duplicates), create connection (a packed {interval max, latency, timeout,
 * address type, address}; ll_hci_create_conn: interval min 7.5 ms, and with the classic stack on it scans 5 ms every 15 ms) */
enum { LE_CMD_SCAN_PARAM = 13, LE_CMD_CREATE_CONN = 15, LE_CMD_CREATE_CONN_CANCEL = 16, LE_CMD_SCAN_ENABLE = 22 };
enum { LE_EV_ENCRYPTION_CHANGE = 0x08, LE_EV_KEY_REFRESH = 0x30, LE_EV_ADV_REPORT = 0xE2 };   /* (0xE2: the host's GAP report) */
enum { LE_HCI_EVENT_PACKET = 4, LE_EV_DISCONNECTION_COMPLETE = 0x05, LE_EV_META = 0x3E, LE_SUB_CONNECTION_COMPLETE = 0x01,
       LE_SUB_CONNECTION_UPDATE_COMPLETE = 0x03, LE_SUB_LTK_REQUEST = 0x05, LE_SUB_DATA_LENGTH_CHANGE = 0x07,
       LE_SUB_ENHANCED_CONNECTION_COMPLETE = 0x0A,
       LE_SUB_PHY_UPDATE_COMPLETE = 0x0C, LE_SM_JUST_WORKS_REQUEST = 0xD0 };
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
/* test build 28: the LE side's events and health, for the log. In test build 27 an iPhone's editor link ended by supervision
 * timeout (0x08) about 20 s in, as the catalogue read ended, and the LE side then stayed silent until power-off: no LE interrupt,
 * the controller's own thread idle, advertising asked again but never on air. (RF_ble.c le_hw_close frees a lost link's entity
 * and resumes a suspended advertiser; ll_adv_open answers an enable with "already open", so asking again changes nothing) */
static uint32_t le_drop_ms;                                    /* when the last LE link ended (ms_now) */
static uint32_t le_link_t0[LE_LINKS], le_life_ms;              /* (test build 31: when each link came, the last one's life in ms) */
static uint32_t le_enc_n, le_ltk_n, le_att_fwd;                /* (test build 31: encryption changes and key refreshes, LTK
                                                                 * requests, ATT packets of a phone's link handed to the server) */
static uint8_t le_enc_st;                                      /* (the last encryption change's status: 06 key missing) */
static uint32_t le_upd_n, le_phy_n, le_dl_n;                   /* connection updates, PHY updates, data length changes */
static uint16_t le_upd_intv, le_upd_lat, le_upd_tmo, le_dl_tx;   /* the last update's interval (1.25 ms), latency, timeout (10 ms);
                                                                 * the last data length change's transmit octets */
static uint8_t le_phy_tx, le_phy_rx;                           /* the last PHY update's (1: 1M, 2: 2M, 3: coded) */
static uint32_t le_msg_n, le_ev_msg;                           /* (the host's messages, ble_profile_loop; the last event's) */
static uint8_t le_ev_last[2];
static le_watch_t le_wd;                                       /* (bt_le_links.h le_watch_due) */
static volatile uint8_t le_wd_req;                             /* (the main loop's watchdog asks ble_profile_loop to act) */
static uint32_t le_wd_n, le_wd_ms;                             /* the watchdog's restarts, the last one's time (ms_now) */
typedef char le_links_check[LE_LINKS == ED_BT_LINKS ? 1 : -1];   /* (px_editor.h keeps a frame for each link) */
/* test build 27: the MIDI characteristic's client configuration as each link's central wrote it (bit 0: notifications). The
 * library keeps it only after an att_send_init this image never makes (att_send.c: att_set_ccc_config does nothing without its
 * buffer), so it is kept here, for the central's reads and the log; the replies do not wait for it: a central that asks the
 * editor something over BLE MIDI listens (one bonded earlier may not write it again) */
static uint16_t le_ccc[LE_LINKS];

/* ---- test build 28: the central role's state (bt_le_central.h: the advertisement filter and the ATT client) */
#define LE_SCAN_INTERVAL 320u     /* 200 ms (0.625 ms units) */
#define LE_SCAN_WINDOW 16u        /* 10 ms of it: 5 % of the radio's time, and only while the footswitch's link is down */
#define LE_CEN_INTERVAL 24u       /* the footswitch's link: 30 ms at most (1.25 ms units), latency 0 */
#define LE_CEN_TIMEOUT 200u       /* its supervision timeout, 2 s (10 ms units): a flash write's pause does not end it */
#define LE_CEN_INIT_MS 3000u      /* a connection not made in 3 s is cancelled, and the pedal scans again */
#define LE_CEN_REQ_MS 5000u       /* an ATT request unanswered for 5 s, or pairing that encrypts nothing in 8 s: the link goes */
#define LE_CEN_PAIR_MS 8000u
enum { LE_CEN_OFF, LE_CEN_SCAN, LE_CEN_INIT, LE_CEN_LINK, LE_CEN_PAUSE };
/* test build 31: the scanner alone. Beside advertising the library starts a scanner suspended (RF_ble.c le_hw_scanning: more
 * than one entity) and only its slot scheduler, from the LE event interrupt, resumes it: when a gap of the scan interval
 * less its window (304 slots) lies ahead and register 0x28038 bit 1 is clear (ble_slot_schedule). On the pedal (test build
 * 30) the scanner never ran beside advertising: the LE interrupts were advertising's alone, no report in 16 s. So with no
 * phone linked, advertising pauses while the pedal scans: alone, the scanner is enabled at once (le_hw_scanning: one
 * entity). LE_ALONE_SCAN_MS scanning, then LE_ALONE_ADV_MS advertising (PAUSE), in turn, until the footswitch is linked
 * (its connection is made alone too). With a phone linked, its link and the scanner, as in test build 30. 0: as 30 */
#ifndef LE_SCAN_ALONE
#define LE_SCAN_ALONE 1
#endif
#define LE_ALONE_SCAN_MS 4000u
#define LE_ALONE_ADV_MS 2000u
static uint8_t le_adv_held;                /* advertising held off while the scanner (or the connection to the footswitch) runs alone */
/* test build 34: build 33's two footswitch links ended unanswered (3E, "failed to be established", after 197 ms: six events of
 * 30 ms), with advertising turned on again at the very connection complete beside the new master link. Now it waits until the
 * footswitch is subscribed, or LE_CEN_ADV_HOLD_MS, or the link's end. And the footswitch's links in the cen line apart
 * from a phone's: the last one's end (reason, life in ms), the 3E endings, the interval its connection complete gave */
#define LE_CEN_ADV_HOLD_MS 10000u  /* (a safety net: the client's own limits end a link that does not answer, LE_CEN_REQ_MS) */
static uint32_t le_cen_link_ms, le_cen_life, le_cen_3e;
static uint8_t le_cen_end;                 /* (the last footswitch link's end reason, 0: none yet) */
static uint16_t le_cen_intv;
static uint32_t le_raw, le_raw_kick, le_scan_opens;   /* (the log: advertising reports at the callback, before any test; the
                                                        * report count at the last ask; scanner starts asked) */
static uint8_t le_cen_off;                 /* openpocket.c: DLY held at power-on, no central role this boot */
static uint8_t le_cen_st, le_cen_scan;     /* the step; scanning asked for */
static uint8_t le_cen_hold;                /* pairing asked: the request waits for the link's encryption */
static uint32_t le_cen_ms, le_cen_kick;    /* when the step (or the request out) began; scanning asked again, every 2 s */
static uint16_t le_cen_val, le_cen_ccc;    /* the known footswitch's MIDI value and CCC handles (0: find them) */
static uint32_t le_cen_tries, le_cen_links, le_cen_subs, le_cen_fails, le_cen_midi, le_cen_pairs;
static le_cen_scan_t le_cs;
static attc_t le_ac;
static struct __attribute__((packed)) { uint16_t intv, lat, tmo; uint8_t type, addr[6]; } le_cen_param;   /* (read later by the
                                                                 * host's thread: static) */

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
static void le_adv_more(void);
static void le_scan_on(uint32_t on);
static void le_cen_rest(uint32_t now);
static void le_cen_poll(uint32_t now);
/* the host's task, after each of its messages (btstack_task.c calls this hook; the library's is weak). Up to test build 26 an
 * empty one in bt_glue.c. Also, while a link is free, advertising is asked again every 2 s: asked while it runs it does nothing
 * (ll_adv.c ll_adv_open: already open), and it starts it again if an enable after a link's end found no free entity yet */
void ble_profile_loop(void)
{
    uint8_t pkt[LE_TX_PKT];
    const uint32_t h = le_lk.conn[le_tx_link];
    le_msg_n++;                                                /* (le_ev_again: the next events are another message's) */
    if (le_wd_req) {                                           /* test build 28: the LE side silent (bt_le_watch): advertising and
                                                                 * scanning off and on again, new radio entities scheduled afresh
                                                                 * (RF_ble.c le_hw_close, le_hw_open, le_hw_advertising) */
        le_wd_req = 0; le_wd_n++; le_wd_ms = ms_now();
        BT_CRUMB("le watchdog: advertising again");
        if (le_lk.adv) { (void)ble_user_cmd_prepare(LE_CMD_ADV_ENABLE, 1, 0); le_lk.adv = 0; }
        if (le_cen_scan) { le_scan_on(0); if (le_cen_st == LE_CEN_SCAN) le_cen_rest(ble_ms()); }   /* (le_cen_poll: on again;
                                                                 * test build 31: after advertising's turn if it was held) */
        le_adv_more();
    }
    while (le_tx_at < le_tx_n && h && hci_le_get_acl_packet_num((uint16_t)h) < le_tx_inflight(le_lk.intv[le_tx_link])) {
        uint32_t at = le_tx_at;
        const uint32_t k = px_blem_pack(le_tx, le_tx_n, &at, ble_ms(), pkt, LE_TX_PKT);
        if (!k || att_server_notify((uint16_t)h, LE_H_MIDI, pkt, (uint16_t)k)) break;   /* (no room now: the next message tries) */
        le_tx_at = at;
        le_tx_pkts++;
    }
    if (le_adv_ok && !le_adv_held && (le_lk.adv || le_links_want_adv(&le_lk)) && ble_ms() - le_adv_kick >= 2000u) {
        le_adv_kick = ble_ms();
        if (le_lk.adv) (void)ble_user_cmd_prepare(LE_CMD_ADV_ENABLE, 1, 1);
        else if (!le_adv_on()) le_lk.adv = 1;
    }
    le_cen_poll(ble_ms());                                     /* (test build 28: the central role) */
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
    if (le_adv_held || !le_links_want_adv(&le_lk)) return;     /* (test build 31: not while the scanner runs alone) */
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

/* ---- test build 28: the central role, all in the host's task (its events, ble_profile_loop) */
static void le_scan_on(uint32_t on)                            /* active scanning (for the scan responses' names), or off */
{
    if (on && !le_cen_scan) ble_user_cmd_prepare(LE_CMD_SCAN_PARAM, 3, 1, LE_SCAN_INTERVAL, LE_SCAN_WINDOW);
    if (on || le_cen_scan) ble_user_cmd_prepare(LE_CMD_SCAN_ENABLE, 2, (int)on, 0);
    le_cen_scan = (uint8_t)on;
}
static int le_cen_slot(void)                                   /* the footswitch's link, or -1 */
{
    uint32_t i;
    for (i = 0; i < LE_LINKS; i++) if (le_lk.conn[i] && le_lk.role[i] == LE_ROLE_CENTRAL) return (int)i;
    return -1;
}
/* one ATT request to the footswitch, as the ATT server sends its notifications (att_server.c att_server_notify) */
static int le_att_send(uint16_t con, const uint8_t *pdu, uint32_t n)
{
    if (!att_dispatch_client_can_send_now(con) || !l2cap_reserve_packet_buffer()) return -1;
    memcpy(l2cap_get_outgoing_buffer(), pdu, n);
    if (l2cap_send_prepared_connectionless(con, 4, (uint16_t)n)) { l2cap_release_packet_buffer(); return -1; }
    return 0;
}
/* scanning again later (a scan turn over, a connection not made, the watchdog). Test build 31: advertising's turn first when
 * it was held (PAUSE), so that a phone always finds the pedal between the scanner's turns */
static void le_cen_rest(uint32_t now)
{
    if (le_adv_held) { le_adv_held = 0; le_cen_st = LE_CEN_PAUSE; le_cen_ms = now; le_adv_more(); }
    else le_cen_st = LE_CEN_OFF;
}
static void le_adv_release(void)                               /* (test build 34: the footswitch's link established) */
{
    if (!le_adv_held) return;
    le_adv_held = 0;
    le_adv_more();
}
static void le_cen_drop(uint16_t con)                          /* the footswitch's link goes: it is found again from scanning */
{
    le_cen_fails++;
    le_ac.st = ATTC_FAIL; le_ac.req_n = 0; le_ac.busy = 0; le_cen_hold = 0;
    ble_user_cmd_prepare(LE_CMD_DISCONNECT, 1, (int)con);
}
/* every pass of the host's task: scanning while the footswitch's link is down; a connection not made in time cancelled;
 * the ATT client's request out, its time limit */
static void le_cen_poll(uint32_t now)
{
    const int i = le_cen_slot();
    const int alone = LE_SCAN_ALONE && !le_links_count(&le_lk, LE_ROLE_PERIPH);   /* (test build 31: no phone linked) */
    if (!le_lk.cen || !le_adv_ok) return;                      /* (the role off, or the stack not up yet) */
    if (le_cen_st == LE_CEN_OFF && le_links_want_scan(&le_lk)) {
        if (alone && le_lk.adv) { (void)ble_user_cmd_prepare(LE_CMD_ADV_ENABLE, 1, 0); le_lk.adv = 0; }   /* (closed first) */
        le_adv_held = (uint8_t)alone;
        le_scan_on(1);
        le_scan_opens++;
        le_cen_st = LE_CEN_SCAN;
        le_cen_ms = le_cen_kick = now;
        le_raw_kick = le_raw;
    } else if (le_cen_st == LE_CEN_SCAN && !le_links_want_scan(&le_lk)) {
        le_scan_on(0);                                         /* (test build 33: a phone's link came: no scanner beside it) */
        le_cen_rest(now);
    } else if (le_cen_st == LE_CEN_SCAN && le_adv_held && now - le_cen_ms >= LE_ALONE_SCAN_MS) {
        le_scan_on(0);                                         /* (test build 31: advertising's turn) */
        le_cen_rest(now);
    } else if (le_cen_st == LE_CEN_SCAN && now - le_cen_kick >= 2000u) {
        /* asked again. Test build 31: off and on when nothing was heard since the last ask: a scanner the library left
         * half-open (ll_scan.c ll_scan_open: its link allocated, then no radio entity free, 58; every later open answers 12,
         * "already open") is cleared only by an off (ll_scan_close frees it). Up to 30: an enable alone, which did nothing */
        le_cen_kick = now;
        if (le_raw == le_raw_kick) { le_scan_on(0); le_scan_on(1); le_scan_opens++; }
        le_raw_kick = le_raw;
    } else if (le_cen_st == LE_CEN_PAUSE && now - le_cen_ms >= LE_ALONE_ADV_MS) {
        le_cen_st = LE_CEN_OFF;                                /* (scanning again at the next pass) */
    } else if (le_cen_st == LE_CEN_INIT && now - le_cen_ms > LE_CEN_INIT_MS) {
        ble_user_cmd_prepare(LE_CMD_CREATE_CONN_CANCEL, 0);
        le_cen_rest(now);                                      /* (scanning again later) */
    } else if (le_cen_st == LE_CEN_LINK && i >= 0) {
        const uint16_t con = le_lk.conn[i];
        if (le_adv_held && now - le_cen_link_ms >= LE_CEN_ADV_HOLD_MS) le_adv_release();   /* (test build 34: not subscribed yet) */
        if (le_ac.req_n && !le_ac.busy && !le_cen_hold && !le_att_send(con, le_ac.req, le_ac.req_n)) {
            le_ac.req_n = 0; le_ac.busy = 1; le_cen_ms = now;
        }
        if ((le_ac.busy && now - le_cen_ms > LE_CEN_REQ_MS) || (le_cen_hold && now - le_cen_ms > LE_CEN_PAIR_MS)) {
            if (le_ac.st == ATTC_SUB) le_cen_val = le_cen_ccc = 0;   /* (a footswitch that changed: find its handles again) */
            le_cen_drop(con);
        }
    }
}
/* an advertising report: the footswitch, while scanning, is connected to */
static void le_cen_adv(const uint8_t *p, uint32_t n, uint32_t now)
{
    if (le_cen_st != LE_CEN_SCAN || !le_cen_report(&le_cs, p, n, now, &le_cen_param.type, le_cen_param.addr)) return;
    le_scan_on(0);
    le_cen_param.intv = LE_CEN_INTERVAL; le_cen_param.lat = 0; le_cen_param.tmo = LE_CEN_TIMEOUT;
    ble_user_cmd_prepare(LE_CMD_CREATE_CONN, 1, &le_cen_param);
    le_cen_st = LE_CEN_INIT;
    le_cen_ms = now;
    le_cen_tries++;
}
/* the ATT packets with an odd opcode (att_dispatch.c): the footswitch's responses and notifications. Test build 31: att_dispatch.c
 * gives every odd opcode to the client once one is registered, and to the server only while none is (up to test build 27); so
 * a packet of another link (a phone's) is handed to the server here, as before. The server's handler is att_global_info's
 * second word (struct att_info_t: client, server, flags, ...) */
extern uint8_t att_global_info[];
typedef void (*le_att_handler_t)(uint8_t type, uint16_t con, uint8_t *p, uint16_t n);
/* test build 33: the GATT server's ATT traffic, for the log (an iPhone ended its link every few seconds in test builds 30 and 31,
 * with DLY too). The server's own handler (att_server.c att_packet_handler, registered by att_server_init) is put behind this one
 * (att_dispatch_register_server again). A request in (ATT data, an even opcode) is counted and its opcode kept. The server
 * writes its answer into l2cap's outgoing buffer and sends it at once (att_handle_request, l2cap_send_prepared_connectionless),
 * in the call that brings the request or in the later can-send-now one: so a mark (0xFF, no ATT opcode) put there before each
 * call and found overwritten after it means the call answered. The answer's opcode is counted; an Error Response (0x01)'s
 * request opcode, handle and code are kept; the last four request > answer pairs; the client's MTU and the server's */
static le_att_handler_t le_att_srv;                            /* the server's own handler */
static uint32_t le_att_req, le_att_ans, le_att_err;            /* requests in, answers out, error responses */
static uint8_t le_att_q, le_att_e[5];                          /* the last request's opcode; the last error: its 5 bytes */
static uint8_t le_att_pair[4][2];                              /* the last four (request, answer) opcodes */
static uint32_t le_att_pair_n;
static uint16_t le_att_mtu_c, le_att_mtu_s;                    /* the client's MTU (its Exchange MTU Request), the server's */
static void le_att_server_spy(uint8_t type, uint16_t con, uint8_t *p, uint16_t n)
{
    uint8_t *out = 0;
    if (type == 8u && n >= 1u && !(p[0] & 1u)) {
        le_att_req++; le_att_q = p[0];
        if (p[0] == 0x02u && n >= 3u) le_att_mtu_c = (uint16_t)(p[1] | p[2] << 8);
    }
    if (l2cap_reserve_packet_buffer()) {                       /* (marked only while free: a packet still on its way out is
                                                                 * never touched; else this call's answer goes uncounted) */
        out = l2cap_get_outgoing_buffer();
        if (out) out[0] = 0xFFu;
        l2cap_release_packet_buffer();
    }
    if (le_att_srv) le_att_srv(type, con, p, n);
    if (!out || out[0] == 0xFFu) return;
    le_att_ans++;
    le_att_pair[le_att_pair_n % 4u][0] = le_att_q; le_att_pair[le_att_pair_n % 4u][1] = out[0];
    le_att_pair_n++;
    if (out[0] == 0x01u) { le_att_err++; memcpy(le_att_e, out, 5); }
    else if (out[0] == 0x03u) le_att_mtu_s = (uint16_t)(out[1] | out[2] << 8);
}
static void le_att_client(uint8_t type, uint16_t con, uint8_t *p, uint16_t n)
{
    const int i = le_links_find(&le_lk, con);
    uint32_t r;
    if (i < 0 || le_lk.role[i] != LE_ROLE_CENTRAL) {           /* (a phone's link: the server's, as with no client) */
        const le_att_handler_t srv = *(const le_att_handler_t *)(const void *)(att_global_info + 4);
        if (type == 8u && srv) { le_att_fwd++; srv(type, con, p, n); }
        return;
    }
    if (type != 8u) return;
    r = attc_rx(&le_ac, p, n);
    if (r == ATTC_R_MIDI) { le_cen_midi++; px_midi_ble_write_link((uint32_t)i, p + 3, (uint32_t)n - 3u); }
    else if (r == ATTC_R_PAIR) { le_cen_pairs++; le_cen_hold = 1; le_cen_ms = ble_ms(); (void)sm_api_request_pairing(con); }
    else if (r == ATTC_R_READY) {                              /* subscribed: this footswitch is the one, until power-off */
        le_adv_release();                                      /* (test build 34: advertising may come, beside this link) */
        le_cen_subs++;
        le_cs.known = 1; le_cs.type = le_cen_param.type; memcpy(le_cs.addr, le_cen_param.addr, 6);
        le_cen_val = le_ac.val; le_cen_ccc = le_ac.ccc;
    } else if (r == ATTC_R_FAIL) {
        le_cen_val = le_cen_ccc = 0;
        if (attc_not_midi(&le_ac)) le_cen_skip(&le_cs, le_cen_param.addr);   /* (test build 33: no BLE MIDI there: not taken
                                                                 * again this boot, SK in the log) */
        le_cen_drop(con);
    }
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

/* test build 28: the same event at another of the callbacks it reaches (the host gives some events to all three, in one of its
 * messages; ble_profile_loop runs after each message) */
static int le_ev_again(const uint8_t *p)
{
    if (le_ev_msg == le_msg_n && le_ev_last[0] == p[0] && le_ev_last[1] == p[2]) return 1;
    le_ev_msg = le_msg_n; le_ev_last[0] = p[0]; le_ev_last[1] = p[2];
    return 0;
}
/* the host's events: the LE link's (the classic link's reach bt_a2dp.c through update_bt_current_status) */
static void le_on_event(uint8_t type, uint16_t chan, uint8_t *p, uint16_t size)
{
    (void)chan;
    if (type != LE_HCI_EVENT_PACKET) return;
    if (p[0] == LE_EV_META && size >= 3u && !le_ev_again(p)) {  /* test build 28: the link layer's procedures, for the log */
        if (p[2] == LE_SUB_CONNECTION_UPDATE_COMPLETE && size >= 12u && !p[3]) {
            le_upd_n++;
            le_upd_intv = (uint16_t)(p[6] | p[7] << 8); le_upd_lat = (uint16_t)(p[8] | p[9] << 8); le_upd_tmo = (uint16_t)(p[10] | p[11] << 8);
        } else if (p[2] == LE_SUB_DATA_LENGTH_CHANGE && size >= 7u) {
            le_dl_n++; le_dl_tx = (uint16_t)(p[5] | p[6] << 8);
        } else if (p[2] == LE_SUB_PHY_UPDATE_COMPLETE && size >= 8u && !p[3]) {
            le_phy_n++; le_phy_tx = p[6]; le_phy_rx = p[7];
        } else if (p[2] == LE_SUB_LTK_REQUEST) {
            le_ltk_n++;                                         /* (test build 31: a central encrypting with a key it holds) */
        }
    }
    if ((p[0] == LE_EV_ENCRYPTION_CHANGE || p[0] == LE_EV_KEY_REFRESH) && size >= 5u
        && le_links_find(&le_lk, (uint32_t)(p[3] | p[4] << 8) & 0x0FFFu) >= 0 && !le_ev_again(p)) {
        le_enc_n++; le_enc_st = p[2];                           /* (test build 31: an LE link's; 06: the key is missing) */
    }
    if (p[0] == LE_EV_DISCONNECTION_COMPLETE) {                 /* (a classic disconnection, or the same event again: not ours) */
        const int i = le_links_ended(&le_lk, (uint32_t)(p[3] | p[4] << 8) & 0x0FFFu);
        if (i >= 0) {
            le_drop = p[5];
            le_drop_ms = ms_now();                              /* (test build 28: the log's D08@31500) */
            le_life_ms = le_drop_ms - le_link_t0[i];            /* (test build 31: how long it lasted) */
            le_link_reset((uint32_t)i);                         /* (test build 27: the editor's reply and frame on that link) */
            if (le_lk.role[i] == LE_ROLE_CENTRAL) {
                le_cen_hold = 0; le_ac.busy = 0; le_ac.req_n = 0;
                le_cen_end = p[5]; le_cen_life = le_life_ms;    /* (test build 34: the footswitch's own, in the cen line) */
                if (p[5] == 0x3Eu) le_cen_3e++;
                le_cen_rest(ble_ms());                          /* (scanning again later; advertising's turn first if held) */
            }
            le_adv_more();                                      /* (a link free: advertising, if it is not on yet) */
        }
    } else if (p[0] == LE_EV_ADV_REPORT) {
        le_raw++;                                               /* (test build 31: every report, before any test) */
        le_cen_adv(p, size, ble_ms());                          /* (test build 28: the footswitch, while scanning) */
    } else if ((p[0] == LE_EV_ENCRYPTION_CHANGE && size >= 6u && !p[2] && p[5]) || (p[0] == LE_EV_KEY_REFRESH && size >= 5u && !p[2])) {
        const int i = le_links_find(&le_lk, (uint32_t)(p[3] | p[4] << 8) & 0x0FFFu);
        if (i >= 0 && le_lk.role[i] == LE_ROLE_CENTRAL) le_cen_hold = 0;   /* (paired: the request goes again) */
    } else if (p[0] == LE_EV_META) {
        const uint8_t *q = 0;
        const uint32_t h = (uint32_t)(p[4] | p[5] << 8) & 0x0FFFu;
        if (p[2] == LE_SUB_CONNECTION_COMPLETE && !p[3]) q = p + 14;
        else if (p[2] == LE_SUB_ENHANCED_CONNECTION_COMPLETE && !p[3]) q = p + 26;
        else if (p[2] == LE_SUB_CONNECTION_UPDATE_COMPLETE && !p[3]) q = p + 6;
        if (q && p[2] == LE_SUB_CONNECTION_UPDATE_COMPLETE) le_links_update(&le_lk, h, (uint32_t)(q[0] | q[1] << 8));
        else if (q) {                                           /* a link made (once, whatever the callbacks it reaches) */
            const uint32_t role = p[6] ? LE_ROLE_PERIPH : LE_ROLE_CENTRAL;   /* (the pedal's role: 1 slave, 0 master) */
            const int i = le_links_made(&le_lk, h, (uint32_t)(q[0] | q[1] << 8), role);
            if (i >= 0) {
                le_link_reset((uint32_t)i);
                le_link_t0[i] = ms_now();                       /* (test build 31: the link's life, in the log's D word) */
                if (role == LE_ROLE_CENTRAL) {                  /* (test build 28: the footswitch, connected to: its client) */
                    const int known = le_cs.known && !memcmp(le_cs.addr, p + 8, 6);
                    le_scan_on(0);                      /* (a connection that came after its cancel, while scanning again) */
                    le_cen_st = LE_CEN_LINK;
                    le_cen_links++;
                    le_cen_hold = 0;
                    le_cen_link_ms = ble_ms();          /* (test build 34: advertising stays held until the footswitch is
                                                         * subscribed, LE_CEN_ADV_HOLD_MS, or the link ends: le_adv_release) */
                    le_cen_intv = (uint16_t)(q[0] | q[1] << 8);
                    attc_start(&le_ac, known ? le_cen_val : 0u, known ? le_cen_ccc : 0u);
                }
                le_adv_more();                                  /* (test build 27: the other link stays open to a central) */
            }
        } else if ((p[2] == LE_SUB_CONNECTION_COMPLETE || p[2] == LE_SUB_ENHANCED_CONNECTION_COMPLETE) && p[3] && le_cen_st == LE_CEN_INIT) {
            le_cen_rest(ble_ms());                              /* (the connection failed or was cancelled: scanning again
                                                                 * later; test build 31: advertising's turn first if held) */
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
    le_att_srv = *(const le_att_handler_t *)(const void *)(att_global_info + 4);   /* (test build 33: the server's handler, */
    if (le_att_srv) att_dispatch_register_server(le_att_server_spy);              /* behind the spy) */
    att_server_register_packet_handler(le_on_event);
    hci_event_callback_set(le_on_event);
    le_l2cap_register_packet_handler(le_on_event);
    if (!le_cen_off) att_dispatch_register_client(le_att_client);   /* (test build 28: the footswitch's responses and
                                                                 * notifications; test build 31: only with the central role on, so
                                                                 * that with it off (DLY) the routing is test build 27's) */
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
    le_lk.cen = !le_cen_off;                                    /* (test build 28: the central role, unless DLY was held) */
    le_adv_more();
}

/* test build 28: an LE link up (bt_a2dp.c: the classic scans' kick then waits longer) */
static uint32_t bt_le_up(void) { return le_links_up(&le_lk); }
/* test build 34: the footswitch linked, or the central role off (bt_a2dp.c: the last phone's call-back waits for it) */
static int bt_le_cen_settled(void) { return !le_lk.cen || le_cen_slot() >= 0; }
/* test build 28: the LE side's watchdog, from the main loop (ble_test_poll: the host's task may have nothing to do then).
 * Something should be on air (advertising, scanning or a link) and neither LE interrupt has moved for LE_WD_MS (advertising
 * alone moves them every 500 ms): ble_profile_loop turns advertising and scanning off and on again. WD in the LE line */
#define LE_WD_MS 4000u
/* test build 31: the LE radio entities' run flags (bb_hdl, offsets in le_hw_text below), sampled on every main-loop pass: the
 * resumes (flag 2) and suspensions (1, 3) seen, per slot, a lower bound of the slot scheduler's work (HW in the LE line) */
extern uint8_t *bb_hdl;
static uint8_t le_hw_flag[8];
static uint16_t le_hw_res[8], le_hw_sus[8];
static void le_hw_sample(void)
{
    const uint8_t *const h = bb_hdl, *a;
    uint32_t i;
    if (!h || !(a = *(const uint8_t *const *)(const void *)(h + 16))) return;
    for (i = 0; i < 8u; i++) {
        const uint8_t *const e = *(const uint8_t *const *)(const void *)(a + 28u * i);
        const uint8_t f = e && *(const uint16_t *)(const void *)(h + 2u * i) ? e[344] : 0xFFu;
        if (f == le_hw_flag[i]) continue;
        if (f == 2u) le_hw_res[i]++; else if (f == 1u || f == 3u) le_hw_sus[i]++;
        le_hw_flag[i] = f;
    }
}
/* test build 34: the central role's clock. le_cen_poll runs in ble_profile_loop, after a message of the host's task; with no
 * phone and no music nothing posts one (advertising alone brings the host no event), and build 33 stayed in PAUSE for good
 * (SO2 in 76 s, the Chocolate never heard). So while the role is on and the footswitch is not subscribed, the main loop wakes
 * the host's task every LE_CEN_WAKE_MS (a type-255 message, stack_run_loop_resume: the editor's 50 ms kick's way) */
#define LE_CEN_WAKE_MS 200u
static uint32_t le_cen_wake_ms;
static void bt_le_watch(uint32_t now)
{
    le_hw_sample();
    if (le_lk.cen && le_adv_ok && !(le_cen_st == LE_CEN_LINK && le_ac.st == ATTC_READY) && now - le_cen_wake_ms >= LE_CEN_WAKE_MS) {
        le_cen_wake_ms = now;
        stack_run_loop_resume();
    }
    const int on_air = le_adv_ok && (le_lk.adv || le_cen_scan || le_links_up(&le_lk));
    if (!le_watch_due(&le_wd, ble_cnt_irq_ev + ble_cnt_irq_rx, now, on_air, LE_WD_MS)) return;
    le_wd_req = 1;
    stack_run_loop_resume();                                   /* (the host's task: ble_profile_loop) */
}

/* test build 31: the LE baseband's radio entities as the library keeps them (RF_ble.c bb_hdl, struct ble_baseband_layer_handle;
 * the offsets are its types', laid out by clang from its bitcode: the entities' ids, 8 x u16, at 0, the array of struct ble_hw
 * (28 bytes each, its struct ble4_hw pointer first) at 16, their count at 32; in struct ble4_hw the state at 343 (1 scanning,
 * 2 advertising, 3 initiating, 6 master link, 7 slave link) and its run flag at 344 (0 enabled at once, alone; 1 or 3
 * suspended, le_hw_suspend; 2 resumed by the slot scheduler, le_hw_resume)). "HW2 0:2/0 1:1/1 IE 01 BB 00": two entities,
 * slot 0 advertising, slot 1 a scanner suspended; the entities' interrupt enables (0x28030) and the status register 0x28038
 * (bit 1 keeps the scheduler from resuming a scanner) as read now */
extern uint8_t *bb_hdl;
static char *le_hw_text(char *o)
{
    const uint8_t *const h = bb_hdl, *a;
    uint32_t i;
    o = ble__put(o, " HW");
    if (!h) return ble__put(o, "-");
    o = bt__dec(o, h[32]);
    a = *(const uint8_t *const *)(const void *)(h + 16);
    for (i = 0; a && i < 8u; i++) {
        const uint16_t id = *(const uint16_t *)(const void *)(h + 2u * i);
        const uint8_t *const e = *(const uint8_t *const *)(const void *)(a + 28u * i);
        if (!id || !e) continue;
        *o++ = ' '; o = bt__dec(o, i); *o++ = ':'; o = bt__dec(o, e[343]); *o++ = '/'; o = bt__dec(o, e[344]);
        if (le_hw_res[i] || le_hw_sus[i]) { *o++ = '+'; o = bt__dec(o, le_hw_res[i]); *o++ = '-'; o = bt__dec(o, le_hw_sus[i]); }
    }
    o = ble__put(o, " IE "); o = bt__hex(o, *(volatile uint32_t *)0x28030u & 0xFFu, 2);
    o = ble__put(o, " BB "); return bt__hex(o, *(volatile uint32_t *)0x28038u & 0xFFu, 2);
}
static char *le_dec_s(char *o, int32_t v)                      /* (a signed number: an RSSI) */
{
    if (v < 0) { *o++ = '-'; v = -v; }
    return bt__dec(o, (uint32_t)v);
}
/* test build 28: the last LE link's end, "D08@31500": its reason (08 supervision timeout, 13 the central ended it, 16 the pedal
 * did, 3E it never came up) and when (ms since power-on, as SE and RC) */
static char *le_drop_text(char *o)
{
    if (!le_drop) return o;
    o = ble__put(o, " D"); o = bt__hex(o, le_drop, 2); *o++ = '@'; o = bt__dec(o, le_drop_ms);
    *o++ = '/'; return bt__dec(o, le_life_ms);                  /* (test build 31: "D13@49866/2950", the link lasted 2.95 s) */
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
    o = le_drop_text(o);
    o = ble__put(o, " IRQ "); o = bt__dec(o, ble_cnt_irq_ev); *o++ = '/'; o = bt__dec(o, ble_cnt_irq_rx);
    if (le_wd_n) { o = ble__put(o, " WD"); o = bt__dec(o, le_wd_n); }
    return o;
}
/* test build 27: the editor's words, "ED 12/340 N1/0" = its frames answered over Bluetooth / notifications sent, each link's
 * client configuration (1: notifications on), then X and the replies dropped, SX and the SysEx dropped, when there are any */
static char *bt_le_ed_text(char *o)
{
    o = ble__put(o, " ED "); o = bt__dec(o, ed_bt_frames); *o++ = '/'; o = bt__dec(o, le_tx_pkts);
    o = ble__put(o, " N"); o = bt__dec(o, le_ccc[0]); *o++ = '/'; o = bt__dec(o, le_ccc[1]);
    if (le_tx_lost) { o = ble__put(o, " X"); o = bt__dec(o, le_tx_lost); }
    if (ed_bt_rx[0].drop + ed_bt_rx[1].drop) { o = ble__put(o, " SX"); o = bt__dec(o, ed_bt_rx[0].drop + ed_bt_rx[1].drop); }
    return o;
}
/* test build 28: the DIAG's eleventh line, the LE side:
 *   "LE 2 L3 W134 P1 A4/0 C24 P12 U2 24/0/200 D08@31500 IRQ 812/97 WD1@36000 ED 12/340 N0/1"
 * the links up and made, MIDI writes, Just Works pairings, advertising asked / its last answer, each link up as its role (C: the
 * pedal is its central, the footswitch; P: a phone or computer is) and interval (1.25 ms); when there are any, the connection
 * updates (U: count, the last one's interval, latency and supervision timeout, 10 ms), PHY updates (PHY: count, the last one's
 * transmit/receive PHY) and data length changes (DL: count, the last transmit octets), and the last link's end (D: reason and
 * ms since power-on); the GATT server's words (AT, test build 33); the LE baseband's interrupts, the watchdog's restarts (WD,
 * bt_le_watch: count, the last one's time); the editor's words. Up to test build 32 the central role's words followed here: with
 * test build 33's GATT server and filter words the line passed the 223 characters a line holds (its end, the footswitch's
 * outcome, cut off) and, with a phone's updates as well, the 300 of t: they have the twelfth line, bt_le_cen_line */
void bt_le_line(char *out)
{
    char t[300], *o = t;
    uint32_t i;
    o = ble__put(o, "LE "); o = bt__dec(o, le_links_up(&le_lk));
    o = ble__put(o, " L"); o = bt__dec(o, le_lk.made);
    o = ble__put(o, " W"); o = bt__dec(o, le_writes);
    o = ble__put(o, " P"); o = bt__dec(o, le_pairs);
    o = ble__put(o, " A"); o = bt__dec(o, le_adv_n); *o++ = '/'; o = bt__dec(o, le_adv_rc);
    for (i = 0; i < LE_LINKS; i++) if (le_lk.conn[i]) { *o++ = ' '; *o++ = le_lk.role[i] == LE_ROLE_CENTRAL ? 'C' : 'P'; o = bt__dec(o, le_lk.intv[i]); }
    if (le_upd_n) {                                             /* connection updates: count, the last one's interval/latency/timeout */
        o = ble__put(o, " U"); o = bt__dec(o, le_upd_n); *o++ = ' ';
        o = bt__dec(o, le_upd_intv); *o++ = '/'; o = bt__dec(o, le_upd_lat); *o++ = '/'; o = bt__dec(o, le_upd_tmo);
    }
    if (le_phy_n) { o = ble__put(o, " PHY"); o = bt__dec(o, le_phy_n); *o++ = ' '; o = bt__dec(o, le_phy_tx); *o++ = '/'; o = bt__dec(o, le_phy_rx); }
    if (le_dl_n) { o = ble__put(o, " DL"); o = bt__dec(o, le_dl_n); *o++ = ' '; o = bt__dec(o, le_dl_tx); }
    o = le_drop_text(o);
    /* test build 31: a phone's link: encryption changes and key refreshes (count / the last status: 00 encrypted, 06 the key
     * is missing), LTK requests (the central encrypts with a key from an earlier pairing: this image keeps LE keys in RAM
     * only), ATT packets of its link handed to the server past the client (le_att_client) */
    if (le_enc_n) { o = ble__put(o, " EN"); o = bt__dec(o, le_enc_n); *o++ = '/'; o = bt__hex(o, le_enc_st, 2); }
    if (le_ltk_n) { o = ble__put(o, " LTK"); o = bt__dec(o, le_ltk_n); }
    if (le_att_fwd) { o = ble__put(o, " AF"); o = bt__dec(o, le_att_fwd); }
    if (le_att_req || le_att_ans) {                            /* test build 33: the GATT server (le_att_server_spy): "AT 14/13
                                                                 * E1 08:0007/0A M185/23 10>11 08>09 08>01 0A>0B" = requests in /
                                                                 * answers out, the error responses (count, the request's opcode:
                                                                 * handle / code), the client's MTU / the server's, the last four
                                                                 * request > answer opcodes, oldest first */
        uint32_t k;
        o = ble__put(o, " AT"); o = bt__dec(o, le_att_req); *o++ = '/'; o = bt__dec(o, le_att_ans);
        if (le_att_err) {
            o = ble__put(o, " E"); o = bt__dec(o, le_att_err); *o++ = ' '; o = bt__hex(o, le_att_e[1], 2); *o++ = ':';
            o = bt__hex(o, le_att_e[3], 2); o = bt__hex(o, le_att_e[2], 2); *o++ = '/'; o = bt__hex(o, le_att_e[4], 2);
        }
        if (le_att_mtu_c) { o = ble__put(o, " M"); o = bt__dec(o, le_att_mtu_c); *o++ = '/'; o = bt__dec(o, le_att_mtu_s); }
        for (k = le_att_pair_n > 4u ? le_att_pair_n - 4u : 0u; k < le_att_pair_n; k++) {
            *o++ = ' '; o = bt__hex(o, le_att_pair[k % 4u][0], 2); *o++ = '>'; o = bt__hex(o, le_att_pair[k % 4u][1], 2);
        }
    }
    o = ble__put(o, " IRQ "); o = bt__dec(o, ble_cnt_irq_ev); *o++ = '/'; o = bt__dec(o, ble_cnt_irq_rx);
    if (le_wd_n) { o = ble__put(o, " WD"); o = bt__dec(o, le_wd_n); *o++ = '@'; o = bt__dec(o, le_wd_ms); }
    o = bt_le_ed_text(o);
    *o = 0;
    for (i = 0; i < 223u && t[i]; i++) out[i] = t[i];
    out[i] = 0;
}
/* test build 33: the DIAG's twelfth line, the central role for the footswitch (up to test build 32 the LE line's end):
 *   "CEN LINK T2 K2 S2 F0 MIDI 41 CL5/00 @A1B2C3D4E5F6 R310 M2 NM40 UU40 CN40 B-60 LF7/-62 RH320 RAW320 SO3 HW2 0:2/2 1:6/0 IE 03
 *    BB 00"
 * its state: OFF (DLY held at power-on: "CEN KEY"), SCAN (AH: advertising held, the scanner alone, test build 31), INIT
 * (connecting), LINK, PAUSE; what came of it, first: connections asked, links made, subscriptions, failures, MIDI notifications,
 * pairings asked (PR), the ATT client's step and last error (CL), the footswitch's address once known; then the filter's view:
 * the advertising reports read, the footswitch's matched, reports showing a footswitch's name (NM), the MIDI UUID (UU),
 * connectable ones (CN), the best RSSI of a name-matched address (B), the last one's flags (1 MIDI UUID, 2 name, 4 connectable: 6
 * is enough) and RSSI (LF), the addresses connected to that had no BLE MIDI (SK); then the scanner's: reports posted to the host
 * (RH, test build 31, bt_a2dp.c), read (RAW), the scanner opened again (SO); the LE radio entities last (le_hw_text), the first
 * words cut off should a line pass 223 characters. With the role off, "CEN KEY" and the radio entities */
void bt_le_cen_line(char *out)
{
    static const char *const ST[5] = {"OFF", "SCAN", "INIT", "LINK", "PAUSE"};
    char t[300], *o = t;
    uint32_t i;
    o = ble__put(o, "CEN "); o = ble__put(o, !le_lk.cen ? (le_cen_off ? "KEY" : "OFF") : ST[le_cen_st < 5u ? le_cen_st : 0u]);
    if (le_lk.cen) {
        if (le_adv_held) o = ble__put(o, " AH");
        /* test build 34: the footswitch's links apart from a phone's: the last one's end (CD reason / life in ms), the 3E
         * endings (failed to be established: no answer to the connection), the interval its connection complete gave */
        if (le_cen_end) { o = ble__put(o, " CD"); o = bt__hex(o, le_cen_end, 2); *o++ = '/'; o = bt__dec(o, le_cen_life); }
        if (le_cen_3e) { o = ble__put(o, " X3E "); o = bt__dec(o, le_cen_3e); }
        if (le_cen_intv) { o = ble__put(o, " IV"); o = bt__dec(o, le_cen_intv); }
        o = ble__put(o, " T"); o = bt__dec(o, le_cen_tries);
        o = ble__put(o, " K"); o = bt__dec(o, le_cen_links);
        o = ble__put(o, " S"); o = bt__dec(o, le_cen_subs);
        o = ble__put(o, " F"); o = bt__dec(o, le_cen_fails);
        o = ble__put(o, " MIDI "); o = bt__dec(o, le_cen_midi);
        if (le_cen_pairs) { o = ble__put(o, " PR"); o = bt__dec(o, le_cen_pairs); }
        if (le_cen_st == LE_CEN_LINK || le_ac.err) { o = ble__put(o, " CL"); o = bt__dec(o, le_ac.st); *o++ = '/'; o = ble__hex(o, le_ac.err); }
        if (le_cs.known) { o = ble__put(o, " @"); for (i = 6; i--;) o = bt__hex(o, le_cs.addr[i], 2); }
        o = ble__put(o, " R"); o = bt__dec(o, le_cs.reports);
        o = ble__put(o, " M"); o = bt__dec(o, le_cs.matches);
        o = ble__put(o, " NM"); o = bt__dec(o, le_cs.name_n);
        o = ble__put(o, " UU"); o = bt__dec(o, le_cs.uuid_n);
        o = ble__put(o, " CN"); o = bt__dec(o, le_cs.conn_n);
        if (le_cs.best_set) { o = ble__put(o, " B"); o = le_dec_s(o, le_cs.best); }
        if (le_cs.last_flags) {
            o = ble__put(o, " LF"); o = bt__dec(o, le_cs.last_flags); *o++ = '/'; o = le_dec_s(o, le_cs.last_rssi);
            if (le_cs.last_ctype) { *o++ = '/'; o = bt__dec(o, le_cs.last_ctype - 1u); }   /* (test build 34: 0 ADV_IND, 1 directed) */
        }
        if (le_cs.skip_n) { o = ble__put(o, " SK"); o = bt__dec(o, le_cs.skip_n); }
        o = ble__put(o, " RH"); o = bt__dec(o, bt_le_rep_host);
        o = ble__put(o, " RAW"); o = bt__dec(o, le_raw);
        o = ble__put(o, " SO"); o = bt__dec(o, le_scan_opens);
    }
    o = le_hw_text(o);
    *o = 0;
    for (i = 0; i < 223u && t[i]; i++) out[i] = t[i];
    out[i] = 0;
}
