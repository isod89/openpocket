/* SPDX-License-Identifier: Apache-2.0 */
/* Controller and host configuration of the Bluetooth test image: the values the JieLi SDK's own
 * apps/common/config/log_config/lib_btctrler_config.c (fw-AC79_AIoT_SDK V1.2.0, Apache-2.0, see
 * third_party/jieli-ble/) gives a Bluetooth LE only build, reduced to LE advertising, one slave link and a GATT server. The names and
 * types are the library's; the values for features we do not build are the SDK's defaults. */
#include <stdint.h>

#define BIT(n) (1u << (n))
enum { BT_MODULE_CLASSIC = BIT(0), BT_MODULE_LE = BIT(1) };
enum { BT_NORMAL = 0x01 };
enum { LE_MASTER = BIT(0), LE_SLAVE = BIT(1), LE_ADV = BIT(2), LE_SCAN = BIT(3), LE_INIT = BIT(4) };

const int config_btctler_modules = BT_MODULE_LE;
int config_btctler_mode = BT_NORMAL;
int config_btctler_hci_standard = 0;
const int config_bt_function = 0;
const int config_btctler_le_tws = 0;
const int CONFIG_BTCTLER_TWS_ENABLE = 0;
const int CONFIG_TWS_AFH_ENABLE = 0;
const int CONFIG_WIFI_DETECT_ENABLE = 0;
const int ESCO_FORWARD_ENABLE = 0;
const int CONFIG_LOW_LATENCY_ENABLE = 0;
const int CONFIG_ESCO_MUX_RX_BULK_ENABLE = 0;
const int CONFIG_BTCTLER_FAST_CONNECT_ENABLE = 0;
const int CONFIG_LMP_NAME_REQ_ENABLE = 1;
const int CONFIG_LMP_PASSKEY_ENABLE = 1;
const int CONFIG_LMP_MASTER_ESCO_ENABLE = 1;
const int CONFIG_INQUIRY_PAGE_OFFSET_ADJUST = 0;
const int config_force_bt_pwr_tab_using_normal_level = 0;   /* 0: the power table of each mode (the stock trims) */
unsigned char config_btctler_bredr_master = 0;
const int config_btctler_dual_a2dp = 0;
const int config_btctler_role_switch_fail_disconnect = 1;
const int config_btctler_role_switch_fail_back_to_slave = 0;
const int config_bredr_afh_user = 0;
const uint8_t rx_fre_offset_adjust_enable = 1;
const int config_fix_fre_enable = 0;
const int ble_disable_wait_enable = 1;
const int config_delete_link_key = 1;
const int config_btstask_auto_exit_sniff = 1;
const int CONFIG_TEST_DUT_CODE = 0;
const int CONFIG_TEST_FCC_CODE = 0;
const int CONFIG_TEST_DUT_ONLY_BOX_CODE = 0;
const int CONFIG_BREDR_INQUIRY = 0;

const uint64_t config_btctler_le_features = 1;                /* LE_ENCRYPTION (LL_FEAT_ENCRYPTION): pairing */
const int config_btctler_le_roles = LE_ADV | LE_SLAVE;
const int config_btctler_le_afh_en = 0;
const int config_btctler_le_hw_ram_use_static = 1;
const int config_btctler_le_master_multilink = 0;
const int config_multilink_support_esco_enalbe = 0;
const int config_btctler_le_slave_multilink = 1;
const int config_btctler_le_hw_nums = 1;
const int config_btctler_le_rx_nums = 5 * 1;
const int config_btctler_le_acl_packet_length = 27;
const int config_btctler_le_acl_total_nums = 5 * 1;
const uint32_t config_vendor_le_bb = 0;
const int config_btctler_le_slave_conn_update_winden = 2500;
const int config_btctler_single_carrier_en = 0;
const int sniff_support_reset_anchor_point = 0;
const int config_bt_security_vulnerability = 0;

/* the host (lib_btstack_config.c, bt_profile_config.c): LE only, one link, a GATT server, Just Works pairing */
const int config_stack_modules = BIT(2);                      /* BT_BTSTACK_LE */
const int config_le_hci_connection_num = 1;
const int config_le_sm_support_enable = 1;
const int config_le_gatt_server_num = 1;
const int config_le_gatt_client_num = 0;
const int config_le_sm_sub_sc_enable = 0;
const int config_le_sm_sub_sc_bridge_edr_enable = 0;
const int CONFIG_BTSTACK_BIG_FLASH_ENABLE = 1;
const int CONFIG_BTSTACK_SUPPORT_AAC = 0;
const int config_rcsp_stack_enable = 0;
