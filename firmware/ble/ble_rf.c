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

static const uint8_t no_record[68];

void wf_rf_common_init(void)
{
    ble_stage_set(BLE_STAGE_RF);
    fm1_wdt_feed();                                              /* the replay takes a while: keep the watchdog fed around it */
    fm1_rf_cal_on = 1;
    ble_busy(0);                                                 /* the replay is long, and its waits are bounded */
    fm1_ble_rf_run(no_record, no_record, no_record, no_record);
    ble_busy(1);
    fm1_wdt_feed();
    ble_stage_set(BLE_STAGE_RF_DONE);                            /* the replay returned: what follows is the library's own start-up */
}

/* The library asks for saved items (trims, bonding): none, as in audio_probe.h. It stores nothing: the VM and BTIF
 * areas are never written by this image. */
int syscfg_read(uint16_t id, void *buf, uint16_t len) { (void)id; (void)buf; (void)len; return -1; }
int syscfg_write(uint16_t id, void *buf, uint16_t len) { (void)id; (void)buf; return len; }
void hw_udelay(uint32_t us) { fm1_delay_us(us); fm1_wdt_feed(); }
uint32_t sys_clk_get(void) { return 360000000u; }                /* SYS_CLK in the stock isd_config.ini */
uint8_t boot_info[64];                                          /* read by the library's chip-id helpers only */
