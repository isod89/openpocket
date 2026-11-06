/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The Bluetooth MIDI peripheral (docs/BLE-TEST.md): the GATT table, what the vendor host (third_party/jieli-ble/btstack) calls
 * back with, and pairing. The stack's task calls all of this from ble_poll, so it runs in the main loop's turn.
 *
 *   GAP      0x1800  device name
 *   GATT     0x1801
 *   MIDI     03B80E5A-EDE8-4B33-A751-6CE34EC4C700, characteristic 7772E5DB-3868-4112-A1A9-F2669D106BF3:
 *            read (empty), write without response (px_midi_ble_write), notify (the pedal sends nothing yet)
 *
 * Pairing is Just Works with bonding and is never asked for here, only accepted: the MIDI characteristic itself is open, as in
 * the stock firmware. The keys are kept in RAM until the next power cycle (syscfg stores nothing, ble_os.c). */
#include "ble_adv.h"

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
enum { BLE_CMD_ADV_ENABLE = 1, BLE_CMD_ADV_PARAM, BLE_CMD_ADV_DATA, BLE_CMD_RSP_DATA };
enum { HCI_EVENT_PACKET = 4, HCI_EVENT_DISCONNECTION_COMPLETE = 0x05, HCI_EVENT_LE_META = 0x3E,
       HCI_SUBEVENT_LE_CONNECTION_COMPLETE = 0x01, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE = 0x03,
       HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE = 0x0A,
       SM_EVENT_JUST_WORKS_REQUEST = 0xD0, SM_EVENT_PAIR_PROCESS = 0xDF };
enum { IO_CAPABILITY_NO_INPUT_NO_OUTPUT = 3, SM_AUTHREQ_BONDING = 1 };

/* attribute size (16), flags (16), handle (16), uuid (16 or 128), value: the format of the SDK's gatt_inc_generator.
 * Flags: 0x02 read, 0x04 write without response, 0x08 write, 0x10 notify, 0x100 dynamic, 0x200 128-bit uuid. */
enum { H_GAP_NAME = 0x0003, H_MIDI = 0x0007, H_MIDI_CCC = 0x0008 };
#define MIDI_SERVICE 0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
#define MIDI_CHAR 0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77
static const uint8_t ble_profile[] = {
    0x0A, 0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x28, 0x00, 0x18,                 /* 0001 primary service 1800 */
    0x0D, 0x00, 0x02, 0x00, 0x02, 0x00, 0x03, 0x28, 0x02, 0x03, 0x00, 0x00, 0x2A, /* 0002 characteristic 2A00: read */
    0x08, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, 0x2A,                             /* 0003 value: dynamic */
    0x0A, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x28, 0x01, 0x18,                 /* 0004 primary service 1801 */
    0x18, 0x00, 0x02, 0x00, 0x05, 0x00, 0x00, 0x28, MIDI_SERVICE,               /* 0005 primary service MIDI */
    0x1B, 0x00, 0x02, 0x00, 0x06, 0x00, 0x03, 0x28, 0x16, 0x07, 0x00, MIDI_CHAR, /* 0006 characteristic: read, write without response, notify */
    0x16, 0x00, 0x16, 0x03, 0x07, 0x00, MIDI_CHAR,                              /* 0007 value: dynamic */
    0x0A, 0x00, 0x0A, 0x01, 0x08, 0x00, 0x02, 0x29, 0x00, 0x00,                 /* 0008 client characteristic configuration */
    0x00, 0x00,
};

static uint16_t ble_conn;                                       /* 0: no link */
static uint16_t ble_conn_int, ble_conn_lat, ble_conn_tmo;       /* the link's interval (1.25 ms), latency, timeout (10 ms) */
static uint8_t ble_peer[6];                                     /* little endian, as in the event */
static uint32_t ble_links, ble_writes, ble_pairs, ble_drop_reason;
static uint32_t ble_ev[8], ble_ev_n;                            /* the last events the host passed on, without Number Of Completed Packets: type, code, subevent or status, status */

/* as the SDK after a disconnection too: the controller keeps neither the parameters nor the data from one advertising run to the next */
static int ble_adv_on(void)
{
    ble_user_cmd_prepare(BLE_CMD_ADV_PARAM, 3, BLE_ADV_INTERVAL, BLE_ADV_IND, BLE_ADV_CHMAP);
    ble_user_cmd_prepare(BLE_CMD_ADV_DATA, 2, (int)sizeof ble_adv_data, ble_adv_data);
    ble_user_cmd_prepare(BLE_CMD_RSP_DATA, 2, (int)sizeof ble_scan_rsp, ble_scan_rsp);
    return ble_user_cmd_prepare(BLE_CMD_ADV_ENABLE, 1, 1);
}

static uint16_t att_read(uint16_t con, uint16_t handle, uint16_t offset, uint8_t *buf, uint16_t size)
{
    (void)con;
    if (handle == H_GAP_NAME) {
        uint16_t len = BLE_NAME_LEN, n;
        if (!buf) return len;
        if (offset >= len) return 0;
        n = (uint16_t)(len - offset) < size ? (uint16_t)(len - offset) : size;
        memcpy(buf, ble_scan_rsp + 2 + offset, n);
        return n;
    }
    if (handle == H_MIDI_CCC) {
        if (buf && size >= 2u) { uint16_t c = att_get_ccc_config(handle); buf[0] = (uint8_t)c; buf[1] = (uint8_t)(c >> 8); }
        return 2;
    }
    return 0;                                                   /* the MIDI value reads empty, as the specification wants */
}

static int att_write(uint16_t con, uint16_t handle, uint16_t mode, uint16_t offset, uint8_t *buf, uint16_t size, uint8_t *sig)
{
    (void)con; (void)mode; (void)offset; (void)sig;
    if (handle == H_MIDI) { ble_writes++; px_midi_ble_write(buf, size); }
    else if (handle == H_MIDI_CCC && size >= 1u) att_set_ccc_config(handle, buf[0]);
    return 0;
}

static void ble_on_event(uint8_t type, uint16_t chan, uint8_t *p, uint16_t size)
{
    (void)chan; (void)size;
    if (p[0] != 0x13u) ble_ev[ble_ev_n++ % 8u] = (uint32_t)type << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[2] << 8 | p[3];
    if (type != HCI_EVENT_PACKET) return;
    ble_cnt_hci++;
    if (p[0] == HCI_EVENT_DISCONNECTION_COMPLETE) {
        ble_conn = 0;
        ble_drop_reason = p[5];
        ble_adv_on();
    } else if (p[0] == HCI_EVENT_LE_META) {
        const uint8_t *q = 0;
        if (p[2] == HCI_SUBEVENT_LE_CONNECTION_COMPLETE && !p[3]) q = p + 14;
        else if (p[2] == HCI_SUBEVENT_LE_ENHANCED_CONNECTION_COMPLETE && !p[3]) q = p + 26;
        else if (p[2] == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE && !p[3]) q = p + 6;
        if (q) {
            ble_conn_int = (uint16_t)(q[0] | q[1] << 8);
            ble_conn_lat = (uint16_t)(q[2] | q[3] << 8);
            ble_conn_tmo = (uint16_t)(q[4] | q[5] << 8);
        }
        if (q && p[2] != HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) { ble_conn = (uint16_t)(p[4] | p[5] << 8); ble_links++; memcpy(ble_peer, p + 8, 6); }
    }
}

static void ble_on_sm(uint8_t type, uint16_t chan, uint8_t *p, uint16_t size)
{
    (void)chan; (void)size;
    if (type != HCI_EVENT_PACKET) return;
    if (p[0] == SM_EVENT_JUST_WORKS_REQUEST) { ble_pairs++; sm_just_works_confirm((uint16_t)(p[2] | p[3] << 8)); }
}

/* the stack calls this from its task, before it says it is up */
void ble_profile_init(void)
{
    le_device_db_init();
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
    sm_set_encryption_key_size_range(7, 16);
    sm_set_request_security(0);
    sm_event_callback_set(ble_on_sm);
    att_server_init(ble_profile, att_read, att_write);
    att_server_register_packet_handler(ble_on_event);
    hci_event_callback_set(ble_on_event);
    le_l2cap_register_packet_handler(ble_on_event);
}

void ble_profile_loop(void) {}                                  /* an optional hook of the stack's task */

/* the stack is up: stock's advertisement, without its manufacturer data, which the Chocolate does not need */
static void ble_advertise(void)
{
    ble_stage_set(BLE_STAGE_ADV);
    le_controller_set_mac(ble_adv_mac);                         /* the public address slot is zero otherwise (ble_adv.h) */
    ble_stage_set(BLE_STAGE_ENABLE);
    int rc = ble_adv_on();
    if (rc) ble_fail(BLE_FAIL_ADV_ENABLE, rc); else ble_state = BLE_ADV;
}

/* btstack_task reports each stage of the stack here (avctp_user.c in the SDK): 43 is "up" */
void update_bt_current_status(uint8_t *addr, uint8_t status, uint8_t value)
{
    (void)addr;
    if (status == 3 && value == 43) ble_advertise();
}

/* the link line of the log channel, and of the status area while connected (ble_status.h); out: 128 bytes */
void ble_midi_line(char *out)
{
    ble_link_text(out, ble_conn != 0, ble_links, ble_peer, ble_conn_int, ble_conn_lat, ble_conn_tmo, ble_writes, ble_pairs, ble_drop_reason,
                  mid_last, ble_stub_calls, ble_stub_last);
}

/* the trace line of the log channel: "EV 43E0100 4050013 ... ST name name" (out: at most 216 bytes): the last events, oldest first, and
 * the stand-ins called so far */
void ble_trace_line(char *out)
{
    char *o = ble__put(out, "EV");
    uint32_t i, n = ble_ev_n < 8u ? ble_ev_n : 8u;
    for (i = ble_ev_n - n; i != ble_ev_n; i++) { *o++ = ' '; o = ble__hex(o, ble_ev[i % 8u]); }
    o = ble__put(o, " ST");
    for (i = 0; i < 12u && ble_stub_seen[i] && o - out + strlen(ble_stub_seen[i]) + 2u < 216u; i++) { *o++ = ' '; o = ble__put(o, ble_stub_seen[i]); }
    *o = 0;
}
