/* SPDX-License-Identifier: Apache-2.0 */
/* Controller and host configuration of the Bluetooth audio test image: the values the JieLi SDK's own configuration files give a
 * classic-only build (fw-AC79_AIoT_SDK V1.2.0, Apache-2.0, see third_party/jieli-bt/: apps/common/config/log_config/
 * lib_btctrler_config.c, lib_btstack_config.c, apps/common/config/bt_profile_config.c), reduced to one classic link and an A2DP
 * sink: no Bluetooth LE, no calls, no TWS, no AAC, no inquiry of other devices, no test modes. Names, types and constness are the
 * library's (read from its declarations); a few values the SDK leaves to an application are the SDK's usual ones, marked so. */
#include <stdint.h>

enum { BT_MODULE_CLASSIC = 1u << 0 };
enum { BT_BTSTACK_CLASSIC = 1u << 0 };
enum { BT_NORMAL = 0x01 };

/* ---- controller (lib_btctrler_config.c) */
const int config_btctler_modules = BT_MODULE_CLASSIC;
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
const int config_btctler_le_roles = 0;                      /* no Bluetooth LE */
const int config_btctler_le_hw_nums = 0;
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
const int config_stack_modules = BT_BTSTACK_CLASSIC;
const int config_le_hci_connection_num = 0;
const int config_le_sm_support_enable = 0;
const int config_le_gatt_server_num = 0;
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
#undef TAG
