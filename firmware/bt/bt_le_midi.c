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
 *  - a disconnection restarts the advertising only when it is the LE link's (the classic link's go to bt_a2dp.c). */
#include "ble_adv.h"                                     /* (ble/: the advertising data, the scan response with the name) */

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

static uint16_t le_conn;                                       /* the LE link's handle, 0: none */
static uint16_t le_int, le_lat, le_tmo;                        /* its interval (1.25 ms), latency, timeout (10 ms) */
static uint32_t le_links, le_writes, le_pairs, le_drop, le_adv_rc, le_adv_n;
static uint8_t le_adv_ok;                                      /* advertising asked for at least once */
static uint8_t le_mac[6];

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
    (void)con;
    if (handle == LE_H_GAP_NAME) {
        uint16_t len = BLE_NAME_LEN, n;
        if (!buf) return len;
        if (offset >= len) return 0;
        n = (uint16_t)(len - offset) < size ? (uint16_t)(len - offset) : size;
        memcpy(buf, ble_scan_rsp + 2 + offset, n);
        return n;
    }
    if (handle == LE_H_MIDI_CCC) {
        if (buf && size >= 2u) { uint16_t c = att_get_ccc_config(handle); buf[0] = (uint8_t)c; buf[1] = (uint8_t)(c >> 8); }
        return 2;
    }
    return 0;                                                   /* the MIDI value reads empty, as the specification wants */
}
static int le_att_write(uint16_t con, uint16_t handle, uint16_t mode, uint16_t offset, uint8_t *buf, uint16_t size, uint8_t *sig)
{
    (void)con; (void)mode; (void)offset; (void)sig;
    if (handle == LE_H_MIDI) { le_writes++; px_midi_ble_write(buf, size); }   /* (px_midi_io.h: the USB path's px_midi_in) */
    else if (handle == LE_H_MIDI_CCC && size >= 1u) att_set_ccc_config(handle, buf[0]);
    return 0;
}

/* the host's events: the LE link's (the classic link's reach bt_a2dp.c through update_bt_current_status) */
static void le_on_event(uint8_t type, uint16_t chan, uint8_t *p, uint16_t size)
{
    (void)chan; (void)size;
    if (type != LE_HCI_EVENT_PACKET) return;
    if (p[0] == LE_EV_DISCONNECTION_COMPLETE) {
        const uint16_t h = (uint16_t)((p[3] | p[4] << 8) & 0x0FFFu);
        if (le_conn && h == le_conn) {                          /* (a classic disconnection is not ours) */
            le_conn = 0;
            le_drop = p[5];
            le_adv_rc = (uint32_t)le_adv_on();
        }
    } else if (p[0] == LE_EV_META) {
        const uint8_t *q = 0;
        if (p[2] == LE_SUB_CONNECTION_COMPLETE && !p[3]) q = p + 14;
        else if (p[2] == LE_SUB_ENHANCED_CONNECTION_COMPLETE && !p[3]) q = p + 26;
        else if (p[2] == LE_SUB_CONNECTION_UPDATE_COMPLETE && !p[3]) q = p + 6;
        if (q) {
            le_int = (uint16_t)(q[0] | q[1] << 8);
            le_lat = (uint16_t)(q[2] | q[3] << 8);
            le_tmo = (uint16_t)(q[4] | q[5] << 8);
        }
        if (q && p[2] != LE_SUB_CONNECTION_UPDATE_COMPLETE) { le_conn = (uint16_t)(p[4] | p[5] << 8); le_links++; }
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
    le_adv_rc = (uint32_t)le_adv_on();
}

/* the log's words for the LE side, appended to the classic lines (bt_a2dp.c): "LE 1 L2 W134 P1 A1/0 I48 IRQ 812/97" =
 * a link up, links so far, MIDI writes, Just Works pairings, advertising asked n times / its last return, the link's interval
 * (1.25 ms units), the LE baseband's two interrupts (45 event, 29 receive; ble/ble_isr.S counts them) */
static char *bt_le_text(char *o)
{
    o = ble__put(o, " LE "); o = bt__dec(o, le_conn != 0);
    o = ble__put(o, " L"); o = bt__dec(o, le_links);
    o = ble__put(o, " W"); o = bt__dec(o, le_writes);
    o = ble__put(o, " P"); o = bt__dec(o, le_pairs);
    o = ble__put(o, " A"); o = bt__dec(o, le_adv_n); *o++ = '/'; o = bt__dec(o, le_adv_rc);
    if (le_conn) { o = ble__put(o, " I"); o = bt__dec(o, le_int); }
    if (le_drop) { o = ble__put(o, " D"); o = ble__hex(o, le_drop); }
    o = ble__put(o, " IRQ "); o = bt__dec(o, ble_cnt_irq_ev); *o++ = '/'; o = bt__dec(o, ble_cnt_irq_rx);
    return o;
}
