/* SPDX-License-Identifier: Apache-2.0 */
/* Controller and host configuration of the Bluetooth audio test image: the values the JieLi SDK's own configuration files give a
 * classic-only build (fw-AC79_AIoT_SDK V1.2.0, Apache-2.0, see third_party/jieli-bt/: apps/common/config/log_config/
 * lib_btctrler_config.c, lib_btstack_config.c, apps/common/config/bt_profile_config.c), reduced to one classic link and an A2DP
 * sink: no calls, no TWS, no AAC, no inquiry of other devices, no test modes. Since test build 23 Bluetooth LE beside it (BLE
 * MIDI, bt_le_midi.c), and since test build 27 two LE links at once (the Chocolate and the web editor, bt_le_links.h). Names,
 * types and constness are the library's (read from its declarations); a few values the SDK leaves to an application are the
 * SDK's usual ones, marked so. */
#include <stdint.h>

enum { BT_MODULE_CLASSIC = 1u << 0, BT_MODULE_LE = 1u << 1 };   /* (test build 23: both, as the stock firmware) */
enum { BT_BTSTACK_CLASSIC = 1u << 0, BT_BTSTACK_LE = 1u << 2 };
enum { BT_NORMAL = 0x01 };

/* ---- controller (lib_btctrler_config.c) */
const int config_btctler_modules = BT_MODULE_CLASSIC | BT_MODULE_LE;   /* test build 23: BLE MIDI beside the music (bt_le_midi.c) */
int config_btctler_mode = BT_NORMAL;
int config_btctler_hci_standard = 0;
const int config_bt_function = 0;
const int config_btctler_le_tws = 0;
const int CONFIG_BTCTLER_TWS_ENABLE = 0;
const int CONFIG_WIFI_DETECT_ENABLE = 0;
const int ESCO_FORWARD_ENABLE = 0;
const int CONFIG_ESCO_MUX_RX_BULK_ENABLE = 0;
const int CONFIG_BTCTLER_FAST_CONNECT_ENABLE = 0;
const int CONFIG_LMP_NAME_REQ_ENABLE = 1;                   /* the phone may ask the name over the link */
const int CONFIG_LMP_PASSKEY_ENABLE = 1;
const int CONFIG_LMP_MASTER_ESCO_ENABLE = 0;                /* (SDK 1: calls; none here) */
const int CONFIG_INQUIRY_PAGE_OFFSET_ADJUST = 0;
const int config_force_bt_pwr_tab_using_normal_level = 0;   /* 0: the power table of each mode */
unsigned char config_btctler_bredr_master = 0;              /* the phone stays master */
const int config_btctler_dual_a2dp = 0;
const int config_btctler_role_switch_fail_disconnect = 1;
const int config_btctler_role_switch_fail_back_to_slave = 0;
const int config_bredr_afh_user = 0;
const uint8_t rx_fre_offset_adjust_enable = 1;              /* the receiver follows the phone's frequency offset */
const int config_fix_fre_enable = 0;
const int config_delete_link_key = 1;                       /* "PIN or key missing": forget the old key, pair again */
const int config_btstask_auto_exit_sniff = 1;
const int CONFIG_TEST_DUT_CODE = 0;                         /* (SDK 1: the RF test mode; not built) */
const int CONFIG_TEST_DUT_ONLY_BOX_CODE = 0;
const int CONFIG_BREDR_INQUIRY = 0;                         /* the pedal never searches for others: it is found */
const int config_bt_security_vulnerability = 0;
const int sniff_support_reset_anchor_point = 0;
const int config_btctler_single_carrier_en = 0;
const int config_btctler_le_master_multilink = 0;
const int config_btctler_le_roles = 31;                     /* test build 28: LE_MASTER | LE_SLAVE | LE_ADV | LE_SCAN | LE_INIT (1, 2, 4, 8, 16,
                                                             * ble/ble_cfg.c): as before it advertises and a central connects; it also
                                                             * scans for a footswitch and connects to it as its central (ll_scan.c,
                                                             * ll_init.c, ll_master.c), unless DLY was held at power-on. Up to 27: 6 */
const int config_btctler_le_hw_nums = 2;                   /* test build 27: two LE radio entities, so two links (the Chocolate and
                                                             * the web editor), or one link and advertising (bt/bt_le_links.h); up to
                                                             * 26: 1, one link (the footswitch, or a phone's MIDI app). The LE
                                                             * baseband's memory grows by 664 bytes (RF_ble.c le_hw_init) */
const int config_vendor_le_bb = 0;
const int config_update_mode = 0;                           /* no update over Bluetooth */
/* the SDK's usual values (an application's in the SDK): the A2DP buffer levels the link uses to pace itself (ms), sniff */
const int CONFIG_A2DP_DATA_CACHE_LOW_AAC = 100;
const int CONFIG_A2DP_DATA_CACHE_HI_AAC = 250;
const int CONFIG_A2DP_DATA_CACHE_LOW_SBC = 150;
const int CONFIG_A2DP_DATA_CACHE_HI_SBC = 260;
const int sniff_long_interval = 800;                        /* 500 ms in slots */
int CONFIG_TWS_SUPER_TIMEOUT = 4000;
uint8_t tws_auto_pair_enable = 0;

/* ---- host (lib_btstack_config.c, bt_profile_config.c) */
const int config_stack_modules = BT_BTSTACK_CLASSIC | BT_BTSTACK_LE;
const int config_le_hci_connection_num = 2;                /* test build 27: two (the host's 1,240 bytes a link, btstack_memory.c,
                                                             * and its bonding table's 56, le_device_db_memory.c, from the Bluetooth
                                                             * heap); never above config_btctler_le_hw_nums, which the host asserts
                                                             * at start-up (btstack_main.c btstack_mem_init: a reset) */
const int config_le_sm_support_enable = 1;                 /* (Just Works, as ble/ble_cfg.c: a Chocolate pairs by itself) */
const int config_le_gatt_server_num = 1;
const int config_le_gatt_client_num = 0;
uint8_t a2dp_profile_support = 1;                          /* the library's is a weak 0: the A2DP sink is on */
const uint8_t a2dp_mutual_support = 0;                      /* one phone */
const uint8_t more_addr_reconnect_support = 0;
const uint8_t more_hfp_cmd_support = 0;
const uint8_t adt_profile_support = 0;
const uint8_t btstack_emitter_support = 0;                  /* a receiver, not a transmitter */
const uint8_t hci_inquiry_support = 0;
/* 0, not the SDK's 1: with 1 the SDP server answers a phone whose maker it does not know yet (every phone: no store of devices)
 * only after its own SDP client has asked the phone (sdp.c sdp_packet_handler), and that client (sdp_master.c) is not linked: the
 * phone's SDP channel waited for ever and it never learnt there is an A2DP sink. Only HID and TWS read it otherwise */
const uint8_t hid_conn_depend_on_dev_company = 0;
const int CONFIG_BTSTACK_SUPPORT_AAC = 0;                   /* SBC only, as the stock firmware */

/* HCI vendor commands an application defines in the SDK (TWS and background modes, never sent here): empty */
/* test build 23: the LE side's settings, ble/ble_cfg.c's (the BLE MIDI image VinSpee ran with a Chocolate Plus) */
const int CONFIG_TWS_AFH_ENABLE = 0;
const int CONFIG_LOW_LATENCY_ENABLE = 0;
const int CONFIG_TEST_FCC_CODE = 0;
const int ble_disable_wait_enable = 1;
const uint64_t config_btctler_le_features = 1;                /* LE_ENCRYPTION (LL_FEAT_ENCRYPTION): pairing */
const int config_btctler_le_afh_en = 0;
const int config_btctler_le_hw_ram_use_static = 1;
const int config_multilink_support_esco_enalbe = 0;
const int config_btctler_le_slave_multilink = 1;            /* (RF_ble.c: the multi-link scheduler places the links' events; test
                                                             * build 27's two links use it) */
const int config_btctler_le_rx_nums = 5;
const int config_btctler_le_acl_packet_length = 27;
const int config_btctler_le_acl_total_nums = 5;
const int config_btctler_le_slave_conn_update_winden = 2500;
const int config_le_sm_sub_sc_enable = 0;
const int config_le_sm_sub_sc_bridge_edr_enable = 0;
const int CONFIG_BTSTACK_BIG_FLASH_ENABLE = 1;
const int config_rcsp_stack_enable = 0;
typedef struct { uint16_t opcode; const char *format; } bt_hci_cmd_t;
const bt_hci_cmd_t btstack_background_state = {0, ""}, btstack_update_flags = {0, ""}, btstack_update_state = {0, ""};

/* the transmit I/Q trims the library writes into the modem (ble_rf.c sets the neutral values before it does, as in the BLE image) */
int keep_BT_CMPUK1, keep_BT_CMPUK2, keep_BT_CMPUK3, keep_BT_OFSI, keep_BT_OFSQ;
void *lmp_ch_update_resume_hdl;                             /* a hook the library calls if set */
uint32_t UPDATA_BREDR_BASE_BEG;                             /* the Bluetooth update's RAM area: no update over Bluetooth */

/* the library's log switches: all off */
#define TAG(n) const char n = 0;
TAG(log_tag_const_c_BL) TAG(log_tag_const_d_BL) TAG(log_tag_const_e_BL) TAG(log_tag_const_i_BL)
TAG(log_tag_const_d_HCI_STD) TAG(log_tag_const_e_HCI_STD) TAG(log_tag_const_i_HCI_STD) TAG(log_tag_const_i_HCI_LMP)
TAG(log_tag_const_d_LMP) TAG(log_tag_const_e_LMP) TAG(log_tag_const_i_LMP) TAG(log_tag_const_v_LMP)
TAG(log_tag_const_i_Analog) TAG(log_tag_const_i_RF) TAG(log_tag_const_i_Thread) TAG(log_tag_const_i_WLC)
/* test build 23: the LE members' log switches (ble/ble_stubs.c's list) */
TAG(log_tag_const_d_HCI_LL) TAG(log_tag_const_d_LE_BB) TAG(log_tag_const_d_LL_AFH) TAG(log_tag_const_d_LL_E)
TAG(log_tag_const_e_LE5_BB) TAG(log_tag_const_e_LE_BB) TAG(log_tag_const_e_LL) TAG(log_tag_const_e_LL_ADV)
TAG(log_tag_const_e_LL_AFH) TAG(log_tag_const_e_LL_E) TAG(log_tag_const_e_LL_INIT) TAG(log_tag_const_e_LL_PHY)
TAG(log_tag_const_e_LL_RL) TAG(log_tag_const_e_LL_S) TAG(log_tag_const_e_LL_SCAN) TAG(log_tag_const_e_LL_WL)
TAG(log_tag_const_i_HCI_LL) TAG(log_tag_const_i_LE5_BB) TAG(log_tag_const_i_LE_BB) TAG(log_tag_const_i_LL)
TAG(log_tag_const_i_LL_ADV) TAG(log_tag_const_i_LL_AFH) TAG(log_tag_const_i_LL_E) TAG(log_tag_const_i_LL_INIT)
TAG(log_tag_const_i_LL_PHY) TAG(log_tag_const_i_LL_RL) TAG(log_tag_const_i_LL_S) TAG(log_tag_const_i_LL_SCAN)
TAG(log_tag_const_i_LL_WL) TAG(log_tag_const_w_LL_AFH)
TAG(log_tag_const_e_LL_M) TAG(log_tag_const_i_LL_M)             /* (test build 28: ll_master.c's) */
#undef TAG
