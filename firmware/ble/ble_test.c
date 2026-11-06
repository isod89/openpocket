/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Advertise-only Bluetooth test (build target "ble", docs/BLE-TEST.md): the controller from the JieLi SDK library
 * (third_party/jieli-ble) is started, and one scannable, non-connectable advertisement is sent on the three
 * advertising channels:
 *   advertising data   flags, the BLE-MIDI service UUID 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 (128 bit)
 *   scan response      the complete local name "OpenPocket-BLE-TEST"
 * No connection is ever accepted (the type is ADV_SCAN_IND), no pairing, nothing is sent over Bluetooth but this. */
#include <stdint.h>
#include <stdbool.h>
#include "fm1_irq.h"
#include "ble_os.h"
#include "ble_status.h"
#include "ble_adv.h"

/* the controller library's entry points (include_lib/btctrler/btctrler_task.h, ble/hci_ll.h; hci_transport.h) */
extern int btctrler_task_init(const void *transport, const void *config);
extern int btctrler_task_ready(void);        /* blocks until the controller task answers, 1 when it did */
extern const void *hci_transport_h4_controller_instance(void);
extern void ll_hci_adv_set_params(uint16_t adv_int_min, uint16_t adv_int_max, uint8_t adv_type, uint8_t direct_address_type,
                                  uint8_t *direct_address, uint8_t channel_map, uint8_t filter_policy);
extern void ll_hci_adv_set_data(uint8_t len, uint8_t *data);
extern void ll_hci_adv_scan_response_set_data(uint8_t len, uint8_t *data);
extern int ll_hci_adv_enable(bool enable);
extern int task_create(void (*fn)(void *), void *arg, const char *name);   /* ble_os.c */
extern uint32_t ble_task_waiting(const char *name);                        /* ble_os.c */
extern int le_controller_set_mac(const uint8_t *mac);                       /* cpu.a/btctrler: the public address slot */
extern int __crc16_mutex_init(void);        /* cpu.a crc16.c: creates the lock its CRC helpers take; the SDK's start-up calls it */

static uint32_t t_start;
static uint8_t cfg[64];                                         /* the controller reads its host-side settings here: none */

/* btctrler_task_ready waits (on a semaphore) until the controller's own task has started, so this runs as a task of
 * its own: the main loop must never block there. */
static void test_task(void *arg)
{
    int rc;
    (void)arg;
    ble_stage_set(BLE_STAGE_INIT);                              /* (BLE_STAGE_RF: ble_rf.c, inside the init) */
    rc = btctrler_task_init(hci_transport_h4_controller_instance(), cfg);
    if (rc) { ble_judge_start(rc, 0); return; }
    ble_stage_set(BLE_STAGE_READY);
    if (ble_judge_start(0, btctrler_task_ready())) return;
    ble_stage_set(BLE_STAGE_ADV);
    le_controller_set_mac(ble_adv_mac);                         /* the public address slot is zero otherwise (ble_adv.h) */
    ll_hci_adv_set_params(BLE_ADV_INTERVAL, BLE_ADV_INTERVAL, BLE_ADV_SCAN_IND, 0, ble_no_addr, BLE_ADV_CHMAP, BLE_ADV_FILTER);
    ll_hci_adv_set_data(sizeof(ble_adv_data), ble_adv_data);
    ll_hci_adv_scan_response_set_data(sizeof(ble_scan_rsp), ble_scan_rsp);
    ble_stage_set(BLE_STAGE_ENABLE);
    rc = ll_hci_adv_enable(true);
    if (rc) ble_fail(BLE_FAIL_ADV_ENABLE, rc); else ble_state = BLE_ADV;
}

void ble_test_start(void)
{
    __crc16_mutex_init();                                       /* else thread_create (ll_init) waits for ever in crc_get_32bit */
    ble_state = BLE_STARTING;
    ble_stage_set(BLE_STAGE_TASK);
    t_start = fm1_uptime_ms();
    if (task_create(test_task, 0, "bletest"))
        ble_fail(BLE_FAIL_TASK_CREATE, -1);
}

void ble_test_poll(void)
{
    ble_busy(1);
    ble_poll();
    ble_busy(0);
    if (ble_state == BLE_STARTING && fm1_uptime_ms() - t_start > 5000u)
    {   /* (the task, if it is still waiting, is left alone) */
        ble_wait = ble_task_waiting("btctrler");
        ble_fail(BLE_FAIL_TIMEOUT, (int32_t)ble_stage);
    }
}

/* every packet the controller returns to its host: we have none, so this only counts them (h4_controller_interface.c) */
int hci_packet_handler(uint8_t type, uint8_t *pkt, uint16_t size) { (void)size; return ble_count_hci(type, pkt); }

void ble_test_state(char *out) { ble_status_text(out); }

/* the long advertising line, with the baseband registers read now (0x28000 control, 0x28028 enables, 0x28030 pending, 0x28038 status) */
void ble_test_adv_line(char *out)
{
    ble_adv_text(out, *(volatile uint32_t *)0x28000u, *(volatile uint32_t *)0x28028u, *(volatile uint32_t *)0x28030u, *(volatile uint32_t *)0x28038u,
                 (FM1_ICFG(45) >> ((45u & 7u) * 4u)) & 0xFu, (FM1_ICFG(29) >> ((29u & 7u) * 4u)) & 0xFu, *(volatile uint32_t *)0x2FC80u);
}

/* the line the status area scrolls, and the log channel sends (out: 96 bytes) */
void ble_test_line(char *out) { if (ble_state == BLE_ADV) ble_test_adv_line(out); else ble_status_full(out); }
