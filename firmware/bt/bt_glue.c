/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* What the classic Bluetooth controller and host need from the system around them, written for OpenPocket (the SDK's own versions
 * live in its system and Wi-Fi libraries, which are not linked): the list-buffer allocator of the radio's data pools, the
 * low-power hooks (the pedal never sleeps the radio), a few helpers of the Wi-Fi front end and the flash, the pedal's own
 * Bluetooth address and name, the pairing records' store (bt_ps.h) and the one SDP record (the A2DP sink, the library's own
 * record). Signatures are the library's (read from its declarations); the allocator follows the SDK's public header
 * system/generic/lbuf.h (Apache-2.0). */
#include <stdint.h>
#include <stddef.h>
#include "ble_os.h"

void local_irq_disable(void);
void local_irq_enable(void);

#include "bt_lbuf.h"                                          /* the radio pools' allocator (tests/test_bt_lbuf.c) */
#ifndef BT_CRUMB
#define BT_CRUMB(s) ((void)0)                                    /* (the stall record's crumbs: bt_stall.h, through ble/ble_os.c) */
#endif
#include "bt_ps.h"                                            /* the pairing records, kept in the flash since test build 14
                                                               * (tests/test_bt_ps.c); also bt_crc32, for the address below */

/* ---- low power: the radio never sleeps here (the SDK's low-power service is not linked); a handle that is not NULL, as the
 * library asserts on one (the BLE image found it on hardware, docs/BLE-TEST.md) */
void *low_power_get(void *priv, void *ops) { static uint32_t handle[4]; (void)priv; (void)ops; BT_CRUMB("low_power_get"); return handle; }
void low_power_put(void *p) { (void)p; }
void low_power_request(void *p) { (void)p; BT_CRUMB("low_power_request"); }
void low_power_exit_request(void) { }
uint8_t low_power_get_default_osc_type(void) { return 0; }
uint8_t low_power_get_osc_type(void) { return 0; }
void low_power_reset_osc_type(uint8_t t) { (void)t; }
int low_power_trace_drift(int us) { (void)us; return 0; }
void wf_deepsl_reg_pop(void *p) { (void)p; }

/* ---- the Wi-Fi front end's helpers (its library is not linked: ble_rf.c replays its start-up) and the radio's AGC switch */
void rf_dc_set_max(void) { BT_CRUMB("rf_dc_set_max"); }
void rf_dc_set_reload(void) { }
int wf_get_phcom_cnt(void) { BT_CRUMB("wf_get_phcom_cnt"); return 0; }
void wifi_det_change_channel_classification(void *a, void *b) { (void)a; (void)b; }
void bt_sfr_pop(void) { }
void btagc_enalbe(uint8_t on) { (void)on; BT_CRUMB("btagc_enalbe"); }

/* ---- JieLi's private test commands and the update over Bluetooth (never used here) */
int clk_get_osc_cap(void) { BT_CRUMB("clk_get_osc_cap"); return 0; }
void doe(uint16_t key, void *p, int n, int x) { (void)key; (void)p; (void)n; (void)x; }
void bredr_esco_standard_tws_link_open(void *a, void *b, int c, uint8_t d) { (void)a; (void)b; (void)c; (void)d; }
void bredr_esco_standard_tws_link_close(void *a) { (void)a; }
void __BT_UPDATA_JUMP(void) { }
void spi_cache_way_switch(uint8_t on) { (void)on; }            /* (the Bluetooth update's path: not taken) */
int sdfile_get_disk_capacity(void) { return 0; }
const char *sdk_version_info_get(void) { BT_CRUMB("sdk_version_info_get"); return "OPENPOCKET BT AUDIO"; }
void usr_timer_del(uint16_t id) { sys_timer_del(id); }

/* ---- the flash's factory unique ID (fm1_flash.h fl_uid_ram, built with FL_UID): the source of this pedal's own address */
static uint8_t bt_uid[16];
static int bt_uid_ok;
static void bt_uid_read(void)
{
    uint32_t f = irq_save();
    bt_uid_ok = FL_FAR(fl_uid_ram)(bt_uid) == 0;
    irq_restore(f);
}
int norflash_ioctl(void *dev, int cmd, int arg)                 /* 100: the flash's unique ID, into arg (bt_get_flash_id) */
{
    uint32_t i;
    (void)dev;
    BT_CRUMB("norflash_ioctl");
    if (cmd != 100 || !arg || !bt_uid_ok) return -1;
    /* 4 bytes: the library's buffer is one int (lmp.c bt_get_flash_id's ex_info_flash_id), and the pairing's private key
     * (local_private_key) follows it in memory. Up to test build 10 this copied 16 */
    for (i = 0; i < 4u; i++) ((uint8_t *)(uintptr_t)arg)[i] = bt_uid[i];
    return 0;
}

/* this pedal's Bluetooth address: classic Bluetooth has no random addresses, so a fixed one (the BLE image's) would make two
 * pedals one device to a phone. From the flash's unique ID: 02:4F (locally administered, "O") and four bytes of a CRC-32 of the
 * ID. The inquiry access codes' LAPs (0x9E8B00..0x9E8B3F) are avoided. Without the ID: the BLE image's address. */
static uint8_t bt_mac[6] = {0x54, 0x4C, 0x42, 0x50, 0x4F, 0x02};
static void bt_mac_make(void)
{
    uint32_t c, lap;
    bt_uid_read();
    if (!bt_uid_ok) return;
    c = bt_crc32(bt_uid, 16u);
    lap = c & 0xFFFFFFu;
    if (lap >= 0x9E8B00u && lap <= 0x9E8B3Fu) c ^= 0x00400000u;
    bt_mac[0] = (uint8_t)c; bt_mac[1] = (uint8_t)(c >> 8); bt_mac[2] = (uint8_t)(c >> 16); bt_mac[3] = (uint8_t)(c >> 24);
    bt_mac[4] = 0x4F; bt_mac[5] = 0x02;                          /* (the library keeps addresses least significant byte first) */
}
uint8_t *bt_get_mac_addr(void) { BT_CRUMB("bt_get_mac_addr"); return bt_mac; }               /* the library's is weak (hci_vendor.c) */
const char *bt_get_local_name(void) { BT_CRUMB("bt_get_local_name"); return "OpenPocket"; }   /* the library's is weak: the name a phone lists */

/* the host's task calls this hook on each pass (a weak reference: left undefined it would be a call to address 0, out of the
 * call range, which the linker refuses); the BLE image runs its GATT loop there, this one has nothing to do */
void ble_profile_loop(void) { }

/* Where the host tells a classic link from an LE one: a handle below this is classic (hci_vendor.c, at every HCI event and ACL
 * packet). 0x50 is the LE controller's own value (link_layer.c, not linked); this controller's classic handles are 0x30 and up
 * (lmp.c lmp_create_conn). Up to test build 10 it was a stand-in returning 0, so no handle was classic: the host dropped every
 * packet of the phone's and never told L2CAP, SDP or A2DP that a link was up. (Not counted as a stand-in: it runs all the time) */
uint16_t get_ble_link_handle(void) { return 0x50u; }

/* ---- the SDP record: the library's own A2DP sink record (sdp_util.c, "JL_A2DP", the stock firmware's), registered in the table
 * the SDP server reads (firmware/bt/app-bt.ld brackets .sdp_record_item as sdp_record_item_begin..end; it links the items, so
 * the table is in RAM) */
typedef struct { void *next; uint32_t handle; const uint8_t *record; } bt_sdp_item_t;
extern const uint8_t sdp_a2dp_service_data[];
bt_sdp_item_t bt_sdp_a2dp __attribute__((used, aligned(4), section(".sdp_record_item"))) = {0, 0x00010001u, sdp_a2dp_service_data};
