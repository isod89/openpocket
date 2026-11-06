/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* firmware/ble/ble_vm.h: the reader of the JieLi VM item log, on synthetic areas built to the layout read off the stock
 * firmware's mount (docs/BLE-TEST.md). Run: cc -O1 -Wall -o build/ble_vm tests/test_ble_vm.c && build/ble_vm */
#include <stdio.h>
#include <string.h>
#include "../firmware/ble/ble_vm.h"

static int bad;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); bad++; } } while (0)

static uint32_t put(uint8_t *a, uint32_t off, uint32_t id, const uint8_t *d, uint32_t n)   /* append an item; the next offset */
{
    a[off] = (uint8_t)ble_vm_crc16(d, n, 0);
    a[off + 1] = (uint8_t)id;
    a[off + 2] = (uint8_t)(n << 4 | (id >> 8 & 0xFu));
    a[off + 3] = (uint8_t)(n >> 4);
    memcpy(a + off + 4, d, n);
    return off + 4 + n;
}

static const uint8_t *mem; static uint32_t memsize;
static int rd(uint32_t off, void *dst, uint32_t n) { if (off + n > memsize) return -1; memcpy(dst, mem + off, n); return 0; }
static uint32_t get(uint32_t base, uint32_t size, uint32_t id, uint8_t *out) { return ble_vm_item(rd, base, size, id, out, 68); }

int main(void)
{
    static uint8_t a[8192];
    const uint8_t t1[2] = { 0x05, 0x0A }, t2[2] = { 0x07, 0x03 }, r107[6] = { 1, 2, 3, 4, 5, 6 }, big[68] = { 9 }, huge[300] = { 1, 2, 3 };
    uint8_t o[68]; uint32_t n, off;

    mem = a; memsize = sizeof a;
    memset(a, 0xFF, sizeof a);
    a[0x100] = 0x55; a[0x101] = 0xAA; a[0x102] = 0xAA; a[0x103] = 0x55;   /* the area at 0x100: the active state word, little endian */
    CHECK(*(uint32_t *)(a + 0x100) == BLE_VM_ACTIVE, "state word");
    off = put(a + 0x100, 4, 106, t1, 2);
    off = put(a + 0x100, off, 107, r107, 6);
    off = put(a + 0x100, off, 0x123, t2, 2);                            /* an id above 255: bits 11:8 in the length word */
    off = put(a + 0x100, off, 187, big, 68);                            /* a length above 15 uses the upper byte */
    off = put(a + 0x100, off, 300, huge, 300);                          /* longer than the read window and than cap: still checked whole */
    off = put(a + 0x100, off, 106, t2, 2);                              /* a later item replaces the earlier */
    memset(o, 0, sizeof o);

    n = get(0x100, 0x1000, 106, o);
    CHECK(n == 2 && o[0] == 7 && o[1] == 3, "the last item of an id wins");
    n = get(0x100, 0x1000, 107, o);
    CHECK(n == 6 && !memcmp(o, r107, 6), "a six byte item");
    n = get(0x100, 0x1000, 0x123, o);
    CHECK(n == 2 && o[0] == 7, "a twelve bit id");
    CHECK(!get(0x100, 0x1000, 0x23, o), "id 0x23 is not id 0x123");
    n = get(0x100, 0x1000, 187, o);
    CHECK(n == 68 && o[0] == 9, "a 68 byte item");
    n = get(0x100, 0x1000, 300, o);
    CHECK(n == 300 && o[0] == 1 && o[2] == 3, "a 300 byte item: its length, the first 68 bytes");
    CHECK(!get(0x100, 0x1000, 108, o), "a missing item");

    a[0x100 + 4 + 4] ^= 0x40;                                           /* damage the first item's data: the log ends there */
    CHECK(!get(0x100, 0x1000, 107, o), "a bad CRC ends the log");
    a[0x100 + 4 + 4] ^= 0x40;
    CHECK(get(0x100, 0x1000, 107, o) != 0, "restored");
    CHECK(!get(0x100, 4 + 4 + 2 + 3, 107, o), "an item that does not fit is not returned");
    memset(a + 0x104, 0xFF, sizeof a - 0x104);
    CHECK(!get(0x100, 0x1000, 106, o), "an erased area has no items");

    puts(bad ? "FAILED" : "ble_vm: all ok");
    return bad != 0;
}
