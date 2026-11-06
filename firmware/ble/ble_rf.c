/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The part of the radio start-up that the JieLi controller library leaves to the Wi-Fi library: its bt_rf_init (RF.c)
 * first calls wf_rf_common_init, which brings up the shared Wi-Fi front end (about 34,000 register writes). Linking
 * the Wi-Fi library for that would pull in its SDIO/MAC stack, so the writes come from the committed capture of the
 * stock start-up instead (bsp/chip/ble_rf_tables_v15.h, run by bsp/chip/fm1_ble_rf.h: stock's boot-2 order, groups 2
 * to 13), with this unit's trims measured by fm1_rf_cal.h (docs/BLE-TEST.md, "What is not verified").
 *
 * The saved radio records (VM items 106, 107, 108, 187) are not read: the VM stays untouched. The records are given
 * as zeros, which leaves the crystal trim (106) and the other stored fields at the captured defaults. */
#include "ble_os.h"
#include "ble_status.h"
#include "fm1_ble_rf.h"
#include "ble_vm.h"

/* VM items 106, 107, 108 (the fourth, 187, is measured by fm1_rf_cal.h): read from the pedal's VM area, zeros where an item is
 * missing. ble_vm_load runs once, from the main loop, before the controller starts (flash read through XIP, nothing written). */
#define VM_BASE 0xA3000u                                 /* the VM region of the stock layout: 0xA3000 (016) or 0xA5000 (020) to 0xBE000 */
#define VM_END  0xBE000u
static const uint8_t no_record[68];
static uint8_t vm_rec[4][68];
static const uint16_t vm_id[4] = { 106, 107, 108, 187 };
uint32_t ble_vm_have;                                    /* bit k: item vm_id[k] was found */
uint32_t ble_vm_area;                                    /* flash offset of the active area, 0 if none */

uint32_t ble_vm_trim(void) { return (uint32_t)vm_rec[0][0] << 8 | vm_rec[0][1]; }

void ble_vm_load(void)
{
    uint32_t a, k;
    ble_vm_have = 0; ble_vm_area = 0;
    for (a = VM_BASE; a < VM_END; a += 0x1000u) {
        uint32_t w;
        if (ota_fread(a, &w, 4) || w != BLE_VM_ACTIVE) continue;
        ble_vm_area = a;
        for (k = 0; k < 4u; k++)
            if (ble_vm_item(ota_fread, a, VM_END - a, vm_id[k], vm_rec[k], sizeof vm_rec[k])) ble_vm_have |= 1u << k;
        break;
    }
}

extern int keep_BT_CMPUK1, keep_BT_CMPUK2, keep_BT_CMPUK3, keep_BT_OFSI, keep_BT_OFSQ;   /* ble_stubs.c: the library's transmit I/Q trims */

/* Returns 0 like the stock start-up on a first run: the library calls wlc_main (the Wi-Fi/Bluetooth coexistence arbiter on, 0x14000
 * bit 1) when this is not 0, and a void function left a stray register value there. The five trims are the neutral base value of
 * bt_rf_analog_init (0x80008080 for 0x2FC10); left at zero they write a zero I/Q gain, which a silent transmitter fits. */
int wf_rf_common_init(void)
{
    ble_stage_set(BLE_STAGE_RF);
    fm1_wdt_feed();                                              /* the replay takes a while: keep the watchdog fed around it */
    fm1_rf_cal_on = (ble_vm_have & 8u) ? 0u : 1u;                /* the pedal's own stored calibration (VM 187) when it has one, else measure */
    ble_busy(0);                                                 /* the replay is long, and its waits are bounded */
    fm1_ble_rf_run(vm_rec[0], vm_rec[1], vm_rec[2], vm_rec[3]);
    ble_busy(1);
    fm1_wdt_feed();
    keep_BT_CMPUK1 = 0x80; keep_BT_CMPUK2 = 0x80; keep_BT_CMPUK3 = 0; keep_BT_OFSI = 0; keep_BT_OFSQ = 0;
    ble_stage_set(BLE_STAGE_RF_DONE);                            /* the replay returned: what follows is the library's own start-up */
    return 0;
}

/* The library asks for saved items (trims, bonding): none, as in audio_probe.h. It stores nothing: the VM and BTIF
 * areas are never written by this image. */
int syscfg_read(uint16_t id, void *buf, uint16_t len) { (void)id; (void)buf; (void)len; return -1; }
int syscfg_write(uint16_t id, void *buf, uint16_t len) { (void)id; (void)buf; return len; }
void hw_udelay(uint32_t us) { fm1_delay_us(us); fm1_wdt_feed(); }
uint32_t sys_clk_get(void) { return 360000000u; }                /* SYS_CLK in the stock isd_config.ini */
uint8_t boot_info[64];                                          /* read by the library's chip-id helpers only */
