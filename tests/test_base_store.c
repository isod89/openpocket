/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — host test of the base firmware's store for an app's own data (firmware/base/store.h) on a simulated NOR
 * flash: save and reload, the two copies in turn, a power cut at every step of a save, another magic, the flash around
 * it untouched, and the bytes the boot code and the update loaders read (the last 256 of each sector) kept erased
 * whatever the app saves.
 *   cc -O1 -Wall -Wno-unused-function -Ifirmware tests/test_base_store.c -o build/test_base_store
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t nor[0x100000], ref[0x100000];
static int fails, ops, cut_at = -1, dead, erase_fails, erases;   /* power cut: after cut_at flash operations nothing more happens */
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

/* ---- the firmware pieces base/store.h uses ---- */
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && (uint32_t)(n) <= (hi) - (uint32_t)(off))
static uint32_t ota_crc16(const uint8_t *p, uint32_t n, uint32_t c)
{
    while (n--) { c ^= (uint32_t)*p++ << 8; for (int i = 0; i < 8; i++) c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1; c &= 0xFFFFu; }
    return c;
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { memcpy(p, nor + off, n); return 0; }
static volatile uint8_t a_mute;
static int irq_held, collected;
static void fm1_wdt_feed(void) {}
static void fm1_delay_ms(int ms) { (void)ms; }
#define FM1_IRQ_ALNK0 11
static void fm1_irq_mask(int n) { (void)n; irq_held++; }
static void fm1_irq_unmask(int n) { (void)n; irq_held--; }
static void a_collect(void) { collected++; }
static uint8_t px_flash_ok = 1;
#define BS_IRQ_OFF() ((void)0)
static int fl_erase4k(uint32_t off, uint32_t *t);
static int fl_write(uint32_t off, const uint8_t *s, uint32_t n);
#include "base/store.h"

/* the flash, as px_usb.h guards it for an app whose OP_FLASH_OWN is BS_FLASH_OWN; written only with the sound held */
static int fl_erase4k(uint32_t off, uint32_t *t)
{
    (void)t;
    if (!BS_FLASH_OWN(off, 0x1000u) || (off & 0xFFFu)) { printf("FAIL erase outside the store %x\n", off); fails++; return -1; }
    if (!a_mute || irq_held != 1) { printf("FAIL erase with the sound running\n"); fails++; }
    if (erase_fails) return -9;
    if (dead || (cut_at >= 0 && ops++ >= cut_at)) { dead = 1; return 0; }
    memset(nor + off, 0xFF, 0x1000);
    erases++;
    return 0;
}
static int fl_write(uint32_t off, const uint8_t *s, uint32_t n)
{
    if (!BS_FLASH_OWN(off, n)) { printf("FAIL write outside the store %x\n", off); fails++; return -1; }
    if (!a_mute || irq_held != 1) { printf("FAIL write with the sound running\n"); fails++; }
    for (uint32_t i = 0; i < n; i++) {                 /* a cut can land in the middle of a write */
        if (dead || (cut_at >= 0 && ops++ >= cut_at)) { dead = 1; return 0; }
        nor[off + i] &= s[i];
    }
    return 0;
}

#define MAGIC 0x31505041u                              /* "APP1" */
static uint8_t data[BS_MAX], back[BS_MAX];
static void fill(uint32_t seed, uint32_t n) { for (uint32_t i = 0; i < n; i++) data[i] = (uint8_t)(seed * 131u + i * 7u + (i >> 8)); }
static void reboot(void) { bs_cur = bs_seq = 0; dead = 0; cut_at = -1; ops = 0; }
/* what a start finds: the data's first byte's seed and its length, or -1 when there is no whole copy */
static int found(uint32_t *len)
{
    uint32_t ver = 0;
    memset(back, 0xEE, sizeof back);
    if (!bs_load(MAGIC, back, sizeof back, len, &ver)) return -1;
    return (int)ver;
}
/* the record of an update in progress, as loader/ldr_core.c and the boot code know one: 80 bytes, "AT" at 6, its CRC16 first */
static int is_record(const uint8_t *r)
{
    const uint32_t c = (uint32_t)r[0] | (uint32_t)r[1] << 8;
    return r[6] == 0x41 && r[7] == 0x54 && c && c == ota_crc16(r + 2, 78, 0);
}
static int outside_untouched(void) { return !memcmp(nor, ref, BS_LO) && !memcmp(nor + BS_HI, ref + BS_HI, sizeof nor - BS_HI); }
/* every sector the store has written (it differs from what the pedal had there) ends in 256 erased bytes; returns the
 * sectors written, or -1 */
static int tails_erased(void)
{
    int written = 0;
    for (uint32_t s = BS_LO; s < BS_HI; s += 0x1000u) {
        if (!memcmp(nor + s, ref + s, 0x1000u)) continue;
        written++;
        if (is_record(nor + s + 0xF00u)) return -1;
        for (uint32_t i = BS_USE; i < 0x1000u; i++) if (nor[s + i] != 0xFFu) return -1;
    }
    return written;
}

int main(void)
{
    uint32_t len = 0, ver = 0, n;
    int v;
    /* a pedal that never ran this app: the official sound bank is where the store will be, OpenPocket's data around it */
    for (n = 0; n < sizeof nor; n++) nor[n] = (uint8_t)(n * 2654435761u >> 13);
    memcpy(ref, nor, sizeof nor);

    reboot();
    CHECK(found(&len) == -1 && back[0] == 0xEE && back[BS_MAX - 1u] == 0xEE, "no copy yet: nothing is loaded, the app's data is left as it was");
    CHECK(!memcmp(nor, ref, sizeof nor) && !erases, "and a start alone writes nothing: the flash is as the pedal had it");

    /* save and reload, the copies in turn */
    fill(1, 300);
    CHECK(bs_save(MAGIC, 3, data, 300) == 0 && bs_cur == BS_LO && bs_seq == 1u, "the first save goes to the first copy");
    CHECK(!a_mute && irq_held == 0 && collected == 1, "after it the sound is on again, the interrupt released");
    CHECK(erases == 1 && !memcmp(nor + BS_LO + 0x1000u, ref + BS_LO + 0x1000u, BS_HI - BS_LO - 0x1000u), "300 bytes need one sector: the others are not erased");
    reboot();
    v = found(&len);
    CHECK(v == 3 && len == 300u && !memcmp(back, data, 300), "the next start finds it: its data, its length, its format version");
    fill(2, 5000);
    CHECK(bs_save(MAGIC, 4, data, 5000) == 0 && bs_cur == BS_LO + 0x5000u && bs_seq == 2u, "the second save goes to the other copy");
    reboot();
    v = found(&len);
    CHECK(v == 4 && len == 5000u && !memcmp(back, data, 5000) && bs_cur == BS_LO + 0x5000u, "the newest copy wins");
    fill(3, BS_MAX);
    CHECK(bs_save(MAGIC, 4, data, BS_MAX) == 0 && bs_cur == BS_LO, "the largest block an app can keep (19184 bytes) fits a copy");
    reboot();
    CHECK(found(&len) == 4 && len == BS_MAX && !memcmp(back, data, BS_MAX), "and comes back whole");
    CHECK(bs_save(MAGIC, 4, data, BS_MAX + 1u) == -1, "one byte more is refused");
    CHECK(outside_untouched() && tails_erased() == 7, "nothing outside the store was written; each of the 7 sectors written so far ends in 256 erased bytes");

    /* the flash an app with this store may write (px_usb.h asks OP_FLASH_OWN): the ten sectors, not one byte around them */
    CHECK(BS_FLASH_OWN(0xE5000u, 0x1000u) && BS_FLASH_OWN(0xEE000u, 0x1000u) && BS_FLASH_OWN(0xE5000u, 0xA000u), "the store's own sectors may be written");
    CHECK(!BS_FLASH_OWN(0xE4000u, 0x1000u) && !BS_FLASH_OWN(0xE4FFFu, 2u), "not the update's staging area below it");
    CHECK(!BS_FLASH_OWN(0xEF000u, 0x1000u) && !BS_FLASH_OWN(0xEEFFFu, 2u) && !BS_FLASH_OWN(0xF0000u, 16u) && !BS_FLASH_OWN(0xC0000u, 16u) &&
          !BS_FLASH_OWN(0x4000u, 16u), "not the sector above it, OpenPocket's presets, models and IRs, or the firmware");

    /* a start that offers less room than the copy holds, another app's magic, OpenPocket's */
    reboot();
    CHECK(!bs_load(MAGIC, back, 100, &len, &ver), "a copy longer than the room the app offers is not loaded");
    CHECK(!bs_load(0x5350504Fu, back, sizeof back, &len, &ver) && !bs_load(MAGIC + 1u, back, sizeof back, &len, &ver), "another magic (OpenPocket's, another app's) finds nothing here");

    /* a flash that fails, a flash that is not the expected part */
    reboot(); (void)found(&len);
    erase_fails = 1; n = bs_cur;
    CHECK(bs_save(MAGIC, 4, data, 100) == -2 && !a_mute && irq_held == 0, "an erase that fails: the save says so, the sound is on again");
    CHECK(bs_cur == n && bs_seq == 3u, "and the copy in use is still the one before");
    erase_fails = 0;
    reboot();
    CHECK(found(&len) == 4 && len == BS_MAX, "and the copy before it still stands");
    px_flash_ok = 0;
    CHECK(bs_save(MAGIC, 4, data, 100) == -1 && !bs_load(MAGIC, back, sizeof back, &len, &ver), "an unknown flash part: nothing is read or written");
    px_flash_ok = 1;

    /* a power cut at every step of a save: the next start finds the copy before it or the new one, whole */
    {
        int cut, old = 0, neu = 0, bad = 0;
        reboot(); (void)found(&len);
        fill(4, 4000); bs_save(MAGIC, 5, data, 4000);            /* the copy before: 4000 bytes, version 5 */
        for (cut = 0; cut < 2 + 16 + 4100 + 40; cut += (cut < 40 || cut > 4080 ? 1 : 97)) {
            static uint8_t keep[0x100000];
            reboot(); (void)found(&len);
            memcpy(keep, nor, sizeof nor);
            fill(5, 4100);
            cut_at = cut; ops = 0;
            (void)bs_save(MAGIC, 6, data, 4100);
            reboot();
            v = found(&len);
            if (v == 6 && len == 4100u && !memcmp(back, data, 4100)) neu++;
            else { fill(4, 4000); if (v == 5 && len == 4000u && !memcmp(back, data, 4000)) old++; else bad++; }
            if (tails_erased() < 0) bad++;
            memcpy(nor, keep, sizeof nor);                       /* (each cut starts from the same flash) */
        }
        CHECK(!bad && old > 40 && neu >= 1, "a power cut at any step of a save: the copy before it, or the new one, never a damaged one");
    }

    /* data that is itself full of update records, at every place a sector's end could take one from */
    {
        uint8_t rec[80];
        uint32_t c;
        for (n = 0; n < 80u; n++) rec[n] = (uint8_t)(n + 1u);
        rec[6] = 0x41; rec[7] = 0x54;
        c = ota_crc16(rec + 2, 78, 0); rec[0] = (uint8_t)c; rec[1] = (uint8_t)(c >> 8);
        CHECK(is_record(rec), "(the test's update record is one the loader would take)");
        for (n = 0; n < 2u; n++) {                     /* both copies */
            uint32_t at;
            reboot(); (void)found(&len);
            for (at = 0; at < BS_MAX; at++) data[at] = rec[(at + 16u + n) % 80u];
            CHECK(bs_save(MAGIC, 7, data, BS_MAX) == 0, "data made of update records is saved like any other");
        }
        CHECK(tails_erased() == 10 && outside_untouched(), "none of the store's 10 sectors ends like an update record: no loader erases it, the boot code starts no update");
    }
    printf(fails ? "base store: %d FAILURES\n" : "base store: ok (0 failures)\n", fails);
    return fails ? 1 : 0;
}
