/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The advertisement the Bluetooth test image sends, as the Bluetooth Core Specification wants it (Vol 3 Part C 11, Vol 6
 * Part B 2.3, Vol 4 Part E 7.8.5-7.8.9). No hardware access: tests/test_ble_status.c checks it on the PC.
 *
 *   PDU            ADV_SCAN_IND: scannable, not connectable. Scan requests are answered; no connection is accepted.
 *   interval       0x00A0 x 0.625 ms = 100 ms (min = max). ADV_SCAN_IND needs at least 100 ms in Core 4.0 and 20 ms since 4.1.
 *   channels       37, 38, 39 (map 0x07).  filter policy 0: scans from anybody.
 *   address        the controller's public address slot (the library's own-address type is a build setting of its HCI layer,
 *                  0 = public, and cannot be changed from here). Left alone it is zero, which is not a valid device address;
 *                  it is set to a fixed, non-zero, locally administered unicast address. Not an IEEE-assigned one: a test image.
 *   advertising data   flags (LE general discoverable, BR/EDR not supported) + complete list of 128-bit service UUIDs
 *                      (BLE-MIDI 03B80E5A-EDE8-4B33-A751-6CE34EC4C700, little endian): 21 of the 31 bytes
 *   scan response      complete local name "OpenPocket-BLE-TEST": 21 bytes
 * Transmit power is the library's own power table (no stored trims are read: docs/BLE-TEST.md). */
#pragma once
#include <stdint.h>

enum { BLE_ADV_SCAN_IND = 2, BLE_ADV_INTERVAL = 0x00A0, BLE_ADV_CHMAP = 7, BLE_ADV_FILTER = 0 };

static uint8_t ble_adv_data[] = {
    2, 0x01, 0x06,                                              /* flags: LE general discoverable, no BR/EDR */
    17, 0x07,                                                   /* complete list of 128-bit service UUIDs, little endian */
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03,
};
static uint8_t ble_scan_rsp[] = { 20, 0x09, 'O','p','e','n','P','o','c','k','e','t','-','B','L','E','-','T','E','S','T' };
static uint8_t ble_adv_mac[6] = { 0x54, 0x4C, 0x42, 0x50, 0x4F, 0x02 };   /* 02:4F:50:42:4C:54, little endian: bit 1 of the first octet = locally administered, bit 0 = unicast */
static uint8_t ble_no_addr[6];                                  /* direct address: unused by ADV_SCAN_IND, but the library reads six bytes */
