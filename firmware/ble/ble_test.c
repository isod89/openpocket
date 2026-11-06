/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The Bluetooth test image (build target "ble", docs/BLE-TEST.md): JieLi's controller and host (third_party/jieli-ble) are
 * started and the pedal advertises as a connectable Bluetooth MIDI peripheral (ble_midi.c). */
#include <stdint.h>
#include <stdbool.h>
#include "fm1_irq.h"
#include "ble_os.h"
#include "ble_status.h"
#include "ble_adv.h"

extern int btstack_init(void);               /* btstack_task.c: starts the controller, waits for it, starts the host task */
extern int task_create(void (*fn)(void *), void *arg, const char *name);   /* ble_os.c */
extern uint32_t ble_task_waiting(const char *name);                        /* ble_os.c */
extern int le_controller_set_mac(const uint8_t *mac);                       /* cpu.a/btctrler: the public address slot */
extern int __crc16_mutex_init(void);        /* cpu.a crc16.c: creates the lock its CRC helpers take; the SDK's start-up calls it */

static uint32_t t_start;

/* the library's TX power ceilings (ble_txpwr_normal_set clamps to bits 15:12 of rf[0]); its .data default is 0 and the stock app sets them
 * at start-up (SDK: bt_max_pwr_set(10, 5, 8, 9)): without this the advertisement is sent at the lowest level, if at all */
extern void bt_max_pwr_set(uint8_t pwr, uint8_t pg_pwr, uint8_t iq_pwr, uint8_t ble_pwr);

/* btstack_init waits (on a semaphore) until the controller's own task has started, so this runs as a task of its own: the
 * main loop must never block there. The host then takes over in update_bt_current_status (ble_midi.c). */
static void test_task(void *arg)
{
    int rc;
    (void)arg;
    ble_stage_set(BLE_STAGE_INIT);                              /* (BLE_STAGE_RF: ble_rf.c, inside the init) */
    rc = btstack_init();
    if (rc) { ble_judge_start(rc, 1); return; }
    ble_stage_set(BLE_STAGE_READY);
    bt_max_pwr_set(10, 5, 8, 9);                                /* after the controller's own init, which resets its power settings */
}

void ble_test_start(void)
{
    ble_vm_load();                                              /* the stored radio records, read once before the controller runs */
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
    ble_stub_collect();
    if (ble_state == BLE_STARTING && fm1_uptime_ms() - t_start > 5000u)
    {   /* (the task, if it is still waiting, is left alone) */
        ble_wait = ble_task_waiting("btctrler");
        ble_fail(BLE_FAIL_TIMEOUT, (int32_t)ble_stage);
    }
}

void ble_test_state(char *out) { ble_status_text(out); }

/* the long advertising line, with the baseband registers read now (0x28000 control, 0x28028 enables, 0x28030 pending, 0x28038 status) */
void ble_test_adv_line(char *out)
{
    ble_adv_text(out, ble_vm_have, ble_vm_area >> 12, ble_vm_trim(), *(volatile uint32_t *)0x28000u, *(volatile uint32_t *)0x28028u, *(volatile uint32_t *)0x28030u, *(volatile uint32_t *)0x28038u,
                 (FM1_ICFG(45) >> ((45u & 7u) * 4u)) & 0xFu, (FM1_ICFG(29) >> ((29u & 7u) * 4u)) & 0xFu, *(volatile uint32_t *)0x2FC80u);
}

/* the line the status area scrolls, and the log channel sends (out: 96 bytes) */
void ble_test_line(char *out) { if (ble_state == BLE_ADV) ble_test_adv_line(out); else ble_status_full(out); }
