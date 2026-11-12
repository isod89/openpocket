/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* What the classic Bluetooth controller and host (third_party/jieli-ble, third_party/jieli-bt) reference but the Bluetooth audio
 * image never runs: Bluetooth LE, TWS (earbud pairs), eSCO / HFP calls, AVRCP, HID, SPP, OBEX, PAN, the GATT client, the controller's
 * test modes, Wi-Fi coexistence. The configuration (bt_cfg.c: classic only, one link, an A2DP sink) keeps the libraries from
 * calling them. Each function is empty, returns 0 (or NULL) and counts the call in bt_stub_calls, keeping its name in
 * bt_stub_last, so a call that should not happen shows in the log line (bt_a2dp.c). The data ones are zero, sized as the
 * library's own objects.
 * Generated from the members' symbol tables and types (LLVM IR of the archives in third_party/jieli-ble and third_party/jieli-bt,
 * the members left out of firmware/bt/members.txt). */
#include <stdint.h>

uint32_t bt_stub_calls;
const char *bt_stub_last;
#ifndef BT_CRUMB
#define BT_CRUMB(s) ((void)0)
#endif
/* (each call also leaves a crumb, firmware/bt/bt_stall.h: the log's trace line names the stand-ins called, in order) */
#define BT_STUB(ret, name) ret name(void) { bt_stub_calls++; bt_stub_last = #name; BT_CRUMB(#name); return (ret)0; }
#define BT_STUB_V(name) void name(void) { bt_stub_calls++; bt_stub_last = #name; BT_CRUMB(#name); }

/* btstack: a2dp_source.c */
BT_STUB_V(a2dp_media_packet_info)
BT_STUB(int, a2dp_source_media_start)
BT_STUB(int, a2dp_source_media_suspend)
/* btctrler: tws_le_link.c */
BT_STUB_V(adv_disable_for_connect_timeout)
/* btstack: bt_audio.c */
BT_STUB_V(aec_sco_connection_start)
BT_STUB_V(aec_sco_connection_stop)
/* lib_ccm_aes: ccm_memory.c (test build 23: linked, BLE MIDI) */
/* btstack: ancs_client.c */
BT_STUB(int, ancs_client_btstack_init_for_check)
/* btstack: att_send.c (test build 23: linked, BLE MIDI) */
/* btstack: att_dispatch.c (test build 23: linked, BLE MIDI) */
/* btstack: att_send.c (test build 23: linked, BLE MIDI) */
/* btstack: att_server.c (test build 23: linked, BLE MIDI) */
/* btstack: att_send.c (test build 23: linked, BLE MIDI) */
/* btstack: avctp.c */
BT_STUB(int, auto_pause_music_player)
BT_STUB(int, auto_start_music_player)
BT_STUB(char, avctp_get_remote_vol_sync)
BT_STUB_V(avctp_init)
BT_STUB(int, avctp_try_send)
/* btstack: bip.c */
BT_STUB(int, bip_cmd_io_ctrl)
/* btctrler: bredr_dut.c */
BT_STUB_V(bredr_dut_detch)
/* btctrler: tws_link.c */
BT_STUB(char, bredr_esco_tws_link_close)
BT_STUB(char, bredr_esco_tws_link_open)
/* btctrler: bredr_fcc.c */
BT_STUB_V(bredr_fcc_init)
BT_STUB_V(bredr_fcc_set_param)
/* btctrler: tws_link.c */
BT_STUB(void *, bredr_get_tws_esco_packet)
/* lib_ccm_aes: ccm_memory.c (test build 23: linked, BLE MIDI) */
/* btstack: a2dp_source.c */
BT_STUB_V(clear_start_flag)
/* btctrler: bredr_dut.c */
BT_STUB_V(dut_clear_tx_buf)
/* btctrler: direct_test_dut.c */
BT_STUB_V(dut_hci_controller_init)
BT_STUB_V(dut_le_hw_open)
/* btctrler: bredr_dut.c */
BT_STUB_V(dut_rx_handler)
/* btstack: gatt_client.c */
BT_STUB_V(gatt_client_exit)
BT_STUB_V(gatt_client_schedule)
/* btctrler: link_layer.c (get_ble_link_handle: not a stand-in since test build 11, bt_glue.c) */
/* btstack: bt_audio.c */
BT_STUB(char, get_esco_coder_busy_flag)
/* btctrler: tws_pair.c */
BT_STUB(char, get_esco_wait_start_monitor)
/* btstack: avctp.c */
BT_STUB(int, get_music_volume)
BT_STUB(int, get_pp_key_statue)
/* btctrler: hci_ll_cmds.c (test build 23: linked, BLE MIDI) */
/* btctrler: hci_ll.c (test build 23: linked, BLE MIDI) */
/* btctrler: ll_common_iso.c */
BT_STUB_V(hci_push_iso_sdu)
/* btctrler: h4_uart_interface_loop.c */
BT_STUB(void *, hci_transport_uart_instance)
/* btctrler: vendor_manager.c */
BT_STUB(int, hci_vendor_cmd_handler)
/* btstack: hfp_ag_profile.c */
BT_STUB_V(hfp_ag_profile_init)
BT_STUB(int, hfp_ag_send_cmd_io_ctrl)
/* btstack: hfp_profile.c */
BT_STUB_V(hfp_profile_init)
BT_STUB(int, hfp_send_bcc_cmd)
BT_STUB(int, hfp_send_cmd_io_ctrl)
BT_STUB(int, hfp_send_hangup_cmd)
/* btstack: hid.c */
BT_STUB(int, hid_diy_data_try_send)
BT_STUB_V(hid_init)
BT_STUB(int, hid_send_cmd_ioctrl)
/* btctrler: ll_resolve_list.c (test build 23: linked, BLE MIDI) */
/* btstack: avctp.c */
BT_STUB(int, is_need_to_suspend)
/* btctrler: hci_ll_cmds.c (test build 23: linked, BLE MIDI) */
/* btctrler: direct_test_dut.c */
BT_STUB_V(le_dut_hci_reset_handle)
/* btctrler: RF_ble.c (test build 23: linked, BLE MIDI) */
/* btctrler: tws_le_link.c */
BT_STUB(int, le_tws_connect_timer_handle)
BT_STUB_V(link_le_adv_try_start_disable)
BT_STUB_V(link_le_scan_try_start_disable)
/* btctrler: link_layer.c (test build 23: linked, BLE MIDI) */
/* btctrler: hci_ll.c (test build 23: linked, BLE MIDI) */
/* btctrler: ll_extended_advertising.c */
BT_STUB_V(ll_ext_destroy)
/* btctrler: hci_ll.c (test build 23: linked, BLE MIDI) */
/* btctrler: link_layer.c (test build 23: linked, BLE MIDI) */
/* btctrler: hci_ll_cmds.c (test build 23: linked, BLE MIDI) */
/* btctrler: link_layer.c (test build 23: linked, BLE MIDI) */
/* btctrler: ll_resolve_list.c (test build 23: linked, BLE MIDI) */
/* btctrler: hci_ll.c (test build 23: linked, BLE MIDI) */
/* btctrler: direct_test_dut.c */
BT_STUB(int, ogf_vendor_le_controller)
/* btstack: opp.c */
BT_STUB(int, opp_cmd_io_ctrl)
BT_STUB_V(opp_init)
/* btstack: pan.c */
BT_STUB(int, pan_cmd_io_ctrl)
BT_STUB_V(pan_init)
/* btstack: avctp.c */
BT_STUB(int, pause_other_player)
/* btstack: rfcomm.c */
BT_STUB_V(rfcomm_init)
/* lib_sbc_eng: sbceng.c */
BT_STUB(int, sbc_cal_energy)
/* btstack: a2dp_source.c */
BT_STUB_V(sbc_param_init)
/* btctrler: tws_media_sync.c */
BT_STUB_V(sbc_sync_packet_len)
/* btctrler: tws_le_link.c */
BT_STUB_V(scan_disable_for_connect_timeout)
/* btstack: sdp_master.c */
BT_STUB(char, sdp_master_channel_disconnect)
BT_STUB_V(sdp_master_init)
/* the SDP client's query: -1, "no SDP client", as the real one says without one (sdp_master.c, not linked). With 0, "started", the
 * SDP server parks a phone's SDP channel until that query ends, which here it never does (sdp.c sdp_packet_handler; bt_cfg.c
 * hid_conn_depend_on_dev_company). Its other callers ignore the value */
int sdp_send_cmd_iotl(void) { bt_stub_calls++; bt_stub_last = "sdp_send_cmd_iotl"; BT_CRUMB("sdp_send_cmd_iotl"); return -1; }
/* btstack: hfp_ag_profile.c */
BT_STUB(int, setup_hfp_ag_esco_link)
/* btstack: sm.c (test build 23: linked, BLE MIDI) */
/* btstack: spp_profile.c */
BT_STUB_V(spp_handl_init)
BT_STUB(int, spp_send_cmd_ioctrl)
/* btstack: spp_update_profile.c */
BT_STUB_V(spp_update_handl_init)
/* btctrler: tws_task.c */
BT_STUB(int, tws_api_detach)
BT_STUB(int, tws_api_get_role_internal)
BT_STUB(int, tws_api_get_tws_state)
BT_STUB(int, tws_api_send_data_to_slave)
BT_STUB(int, tws_api_set_sibling_addr)
/* btctrler: tws_le_link.c */
BT_STUB_V(tws_ble_connect_switch_bredr)
/* btctrler: tws_pair.c */
BT_STUB(int, tws_check_can_setup_esco)
BT_STUB_V(tws_conn_role_switch_lock_ms)
BT_STUB(char, tws_conn_switch_is_master)
BT_STUB(int, tws_conn_tx_event_handler)
/* btctrler: tws_le_link.c */
BT_STUB_V(tws_connect_pause_in_esco)
/* btctrler: tws_link.c */
BT_STUB(char, tws_esco_acl_communication)
/* btctrler: tws_pair.c */
BT_STUB_V(tws_esco_remove_clear_state)
BT_STUB_V(tws_esco_role_switch_post)
/* btctrler: tws_link.c */
BT_STUB_V(tws_esco_suspend_resume)
BT_STUB(char, tws_esco_transmit_deal)
/* btctrler: tws_pair.c */
BT_STUB(int, tws_host_channel_match)
/* btctrler: tws_le_acc.c */
BT_STUB(int, tws_le_acc_generation)
/* btctrler: tws_le_link.c */
BT_STUB_V(tws_le_adv_scan_parm_init)
BT_STUB_V(tws_le_clr_esco_indicate)
BT_STUB(char, tws_le_esco_connect_handle)
BT_STUB(char, tws_le_get_esco_indicate)
BT_STUB_V(tws_le_set_esco_indicate)
/* btctrler: tws_pair.c */
BT_STUB_V(tws_link_conn_rx_handler_in_irq)
BT_STUB_V(tws_link_conn_start_in_irq)
BT_STUB(char, tws_lmp_esco_link_setup)
BT_STUB(char, tws_master_notice_slave_step_fun)
BT_STUB(int, tws_phone_link_abandon_sbc_data)
BT_STUB_V(tws_phone_link_connection)
BT_STUB_V(tws_phone_link_connection_wait_role_switch)
BT_STUB_V(tws_phone_link_del)
BT_STUB_V(tws_phone_link_detach)
BT_STUB(int, tws_phone_link_sbc_data_len)
BT_STUB_V(tws_phone_link_state_change)
BT_STUB_V(tws_post_remove_esco_link)
BT_STUB(int, tws_post_remove_esco_link_super_timeout)
BT_STUB(void *, tws_rx_bulk_alloc_in_irq)
/* btctrler: tws_sniff.c */
BT_STUB_V(tws_tx_unsniff_req)
BT_STUB_V(tws_unsniff_with_phone)
/* btstack: client_user.c */
BT_STUB(int, user_client_search_profile_start)
/* btstack: att_send.c (test build 23: linked, BLE MIDI) */

/* data of the members left out (zero; sizes as the library's own objects, 32-bit) */
uint8_t PSM_L2CAP_OBEX[4] __attribute__((aligned(4)));
uint8_t fast_search_data[4] __attribute__((aligned(4)));
/* le_param: test build 23, the LE controller's own (link_layer.c and the ll_* members, linked for BLE MIDI) */
uint8_t prbs9_table1[511] __attribute__((aligned(4)));
uint8_t tws_conn[176] __attribute__((aligned(4)));
uint8_t tws_link[80] __attribute__((aligned(4)));
uint8_t tws_link_task_ops[8] __attribute__((aligned(4)));
uint8_t tws_sniff[36] __attribute__((aligned(4)));
