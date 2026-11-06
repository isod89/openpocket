/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The JieLi VM: the pedal's own store of per-unit items (the radio's crystal trim and RF records are items 106, 107, 108
 * and 187), read only. The layout is read off the stock firmware's VM mount and read routines (BlackBox_016 app.bin:
 * the mount scan at image offset 0x36e12, the read at 0x36f1a; docs/BLE-TEST.md "The stored radio records").
 *
 *   area   u32 state word, then items back to back (an erased 0xFF header ends the log)
 *   item   u8 crc, u8 id (low 8 bits), u16 le: bits 15:4 length, bits 3:0 the id's bits 11:8; then length data bytes
 *   crc    the low byte of CRC-16 (poly 0x1021, init 0) of the data
 *   the flash is plain here; read through the XIP window it comes out decrypted, as noise (found on hardware: peek of 0xA3000)
 *   a later item with the same id replaces an earlier one (the stock mount keeps the last)
 * The active area has the state word 0x55AAAA55 (0xDDEEAA55: a defrag was filling it; 0xDDEEBB77: the retired copy). */
#pragma once
#include <stdint.h>

#define BLE_VM_ACTIVE 0x55AAAA55u

typedef int (*ble_vm_rd_t)(uint32_t off, void *dst, uint32_t n);   /* raw flash read (ota_fread): 0 = ok */

static uint32_t ble_vm_crc16(const uint8_t *p, uint32_t n, uint32_t c)
{
    uint32_t i;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8u; i++) c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
        c &= 0xFFFFu;
    }
    return c;
}

/* The last valid item `id` of the area at flash offset base (size bytes in all): its length, 0 if there is none. The first
 * min(length, cap) bytes go to out (cap <= 68). The flash is read raw: the VM is plain, the XIP window would decrypt it. */
static uint32_t ble_vm_item(ble_vm_rd_t rd, uint32_t base, uint32_t size, uint32_t id, uint8_t *out, uint32_t cap)
{
    uint8_t h[4], tmp[68], buf[64];
    uint32_t off = 4, found = 0;
    while (off + 4u <= size) {
        uint32_t w, n, i, c = 0, k, m;
        if (rd(base + off, h, 4)) break;
        w = (uint32_t)h[2] | (uint32_t)h[3] << 8; n = w >> 4; i = h[1] | (w & 0xFu) << 8;
        if (n == 0 || off + 4u + n > size) break;
        for (k = 0; k < n; k += m) {
            m = n - k > sizeof buf ? (uint32_t)sizeof buf : n - k;
            if (rd(base + off + 4u + k, buf, m)) return found;
            c = ble_vm_crc16(buf, m, c);
            if (k < cap && k < sizeof tmp) { uint32_t j; for (j = 0; j < m && k + j < cap && k + j < sizeof tmp; j++) tmp[k + j] = buf[j]; }
        }
        if ((c & 0xFFu) != h[0]) break;
        if (i == id) { uint32_t j; found = n; for (j = 0; j < n && j < cap && j < sizeof tmp; j++) out[j] = tmp[j]; }
        off += 4u + n;
    }
    return found;
}
