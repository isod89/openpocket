/* SPDX-License-Identifier: GPL-3.0-only */
/* The Bluetooth audio image's pairing store (firmware/bt/bt_ps.h, test build 14) on a PC: a model of the NOR flash (a program
 * clears bits, an erase sets a 4 KiB sector to 0xFF), the library's access pattern (open, seek, read or write, close of 32-byte
 * records), and power cuts at random points of every erase and of every byte programmed, at power-on and during saves. Checks:
 *   - each power-on loads the records last saved, or those being saved when the power went, never anything else;
 *   - nothing outside [0xEE000, 0xF0000) changes, and bytes 0xF00..0xFFF of its two sectors are never programmed;
 *   - every program is one page into erased bytes, every erase one aligned sector, both inside the Bluetooth image's FL_RANGE_OK
 *     (firmware/px_usb.h), and none at power-on but erases; no erase takes the newest good copy; no two valid slots share a seq;
 *   - a save holds the audio once and releases it, never sooner than 1 s after the library's last write, never without a change;
 *   - a sector whose programs no longer take (worn out) and slots misread at power-on cost saves, never the copy kept;
 *   - the merged FL_RANGE_OK gives every sector erase and page program the answer of one range per area.
 * Prints the saves, the power cuts, the erases (at power-on, during a save) and the page programs: the wear figures.
 *   cc -O1 -Wall -Wno-unused-function -o build/test_bt_ps tests/test_bt_ps.c && build/test_bt_ps [rounds] [seed] */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#define BT_CRUMB(s) ((void)0)
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define FL_OTA_LO 0xE0000u
#define FL_OTA_HI 0xE5000u
/* firmware/px_usb.h's two forms (the first checked against its text below): the Bluetooth audio image's, the other images' */
#define BT_RANGE_LINE "#define FL_RANGE_OK(off, n) (FL_IN(off, n, 0xC0000u, FL_OTA_HI) || FL_IN(off, n, 0xEE000u, 0xFC000u))"
#define FL_RANGE_OK(off, n) (FL_IN(off, n, 0xC0000u, FL_OTA_HI) || FL_IN(off, n, 0xEE000u, 0xFC000u))
#define FL_RANGE_REL(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, 0xF0000u, 0xF4000u) || \
                              FL_IN(off, n, 0xC0000u, 0xE0000u) || FL_IN(off, n, 0xF4000u, 0xFC000u))
#define LO 0xEE000u                                             /* the store's two sectors */
#define HI 0xF0000u
#define WAIT 1000u                                              /* a save 1 s after the library's last write (bt_ps.h BT_PS_WAIT) */

static uint8_t nor[0x100000], outside[0x100000];
static uint32_t clk;
static uint32_t ble_ms(void) { return clk; }
static void fm1_wdt_feed(void) { }
static int hold, holds, in_boot;
static unsigned long erase_boot, erase_save, programs, cuts_boot, cuts_save, boots, loads_saved, loads_saving, saves;
static jmp_buf cut_jb;
static long cut_at = -1;                                        /* flash steps (bytes programmed, erases) until the power goes */
static int worn = -1;                                           /* a sector (0, 1) whose programs no longer take; -1 none */
static long misread_at = -1;                                    /* reads until one returns a bit flipped; -1 none */
#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, " (power-on %lu)\n", boots); exit(1); } while (0)

static int tick(void) { if (cut_at < 0) return 0; return cut_at-- == 0; }
static void st_audio_hold(int on)
{
    if (on) { if (hold) FAIL("audio hold taken twice"); hold = 1; holds++; }
    else { if (!hold) FAIL("audio hold released twice"); hold = 0; }
}
static int ota_fread(uint32_t off, void *p, uint32_t n)
{
    if (!n || off + n > sizeof nor) FAIL("read past the part: %05X+%u", off, n);
    memcpy(p, nor + off, n);
    if (misread_at >= 0 && misread_at-- == 0) ((uint8_t *)p)[rand() % n] ^= (uint8_t)(1u << (rand() & 7));
    return 0;
}

/* the flash's newest good copy, by this test's own reading (its own CRC-32, not bt_ps.h's): -1 none. No two share a seq */
static uint32_t crc_ref(const uint8_t *p, uint32_t n)
{
    static uint32_t t[256];
    uint32_t c, i, k;
    if (!t[1]) for (i = 0; i < 256u; i++) { for (c = i, k = 0; k < 8u; k++) c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    for (c = 0xFFFFFFFFu; n--; ) c = t[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}
static const uint8_t *slot_at(int i) { return nor + LO + (uint32_t)(i / 5) * 0x1000u + (uint32_t)(i % 5) * 684u; }
static int slot_valid(int i, uint32_t *seq)
{
    uint32_t c, m;
    memcpy(seq, slot_at(i) + 672, 4); memcpy(&c, slot_at(i) + 676, 4); memcpy(&m, slot_at(i) + 680, 4);
    return m == 0x5442504Fu && c == crc_ref(slot_at(i), 676);
}
static int newest(void)
{
    int i, k, best = -1, ok[10];
    uint32_t s[10] = {0};
    for (i = 0; i < 10; i++) {
        if (!(ok[i] = slot_valid(i, &s[i]))) continue;
        for (k = 0; k < i; k++) if (ok[k] && s[k] == s[i]) FAIL("slots %d and %d share seq %u", k, i, s[i]);
        if (best < 0 || (int32_t)(s[i] - s[best]) > 0) best = i;
    }
    return best;
}

static int fl_erase4k(uint32_t off, uint32_t *took)
{
    uint32_t i;
    int b;
    *took = 0;
    if (!FL_RANGE_OK(off, 0x1000u) || (off & 0xFFFu)) FAIL("erase refused by the driver: %05X", off);
    if (off < LO || off >= HI) FAIL("erase outside the store: %05X", off);
    if (!in_boot && !hold) FAIL("erase without the audio held");
    if ((b = newest()) >= 0 && LO + (uint32_t)(b / 5) * 0x1000u == off) FAIL("erase of the sector that holds the newest good copy: %05X", off);
    if (tick()) {                                               /* a torn erase: the sector left in any state */
        for (i = 0; i < 0x1000u; i++) nor[off + i] = (rand() & 3) ? 0xFFu : (uint8_t)rand();
        longjmp(cut_jb, 1);
    }
    memset(nor + off, 0xFF, 0x1000u);
    if (in_boot) erase_boot++; else erase_save++;
    return 0;
}
static int fl_page(uint32_t off, const uint8_t *src, uint32_t n)
{
    uint32_t i;
    if (!FL_RANGE_OK(off, n) || n == 0 || n > 256u || (off & 0xFFu) + n > 256u) FAIL("program refused by the driver: %05X+%u", off, n);
    if (off < LO || off + n > HI) FAIL("program outside the store: %05X", off);
    if (((off + n - 1u) & 0xFFFu) >= 0xF00u) FAIL("program in the loader's record band: %05X+%u", off, n);
    if (in_boot) FAIL("program at power-on");
    if (!hold) FAIL("program without the audio held");
    for (i = 0; i < n; i++) if (nor[off + i] != 0xFFu) FAIL("program into bytes not erased: %05X", off + i);
    programs++;
    if (worn >= 0 && (off - LO) >> 12 == (uint32_t)worn) return 0;   /* worn out: nothing takes, and the driver cannot tell */
    for (i = 0; i < n; i++) {
        if (tick()) { nor[off + i] &= (uint8_t)(src[i] | rand()); longjmp(cut_jb, 1); }   /* a torn byte: some bits programmed */
        nor[off + i] &= src[i];
    }
    return 0;
}
static int fl_write(uint32_t off, const uint8_t *src, uint32_t n)       /* as fm1_flash.h: page by page */
{
    while (n) {
        uint32_t chunk = 256u - (off & 0xFFu);
        if (chunk > n) chunk = n;
        if (fl_page(off, src, chunk)) return -1;
        off += chunk; src += chunk; n -= chunk;
    }
    return 0;
}

#include "../firmware/bt/bt_ps.h"

static uint8_t want[BT_VM_N][BT_VM_REC], saving[BT_VM_N][BT_VM_REC], cur[BT_VM_N][BT_VM_REC];
static int have_saving, changed;                               /* the records being saved when the power may go; a change not saved */
static uint32_t last_write;                                     /* the library's last write (ms) */

static void power_up(void)                                      /* RAM in any state, the statics as the C start-up leaves them */
{
    memset(&bt_ps, rand(), sizeof bt_ps);
    bt_vm_cur = -1; bt_vm_pos = 0; bt_vm_reads = bt_vm_writes = 0;
    bt_ps_t = 0; bt_ps_saves = 0; bt_ps_next = bt_ps_cur = 0; bt_ps_dirty = bt_ps_ok = bt_ps_bad = 0;
    hold = 0;
}
static void lib_start(void)                                     /* the library's start: the interface, then its reads */
{
    uint8_t r[BT_VM_REC];
    int id;
    (void)bt_vm_interface();
    for (id = 0; id < BT_VM_N; id++) {
        bt_vm_open(BT_VM_FIRST + id, 32); bt_vm_seek(0, 0);
        if (bt_vm_read(r, 32) != 32) FAIL("short read");
        bt_vm_close();
        memcpy(cur[id], r, 32);
    }
    changed = 0;
}
static void power_on(long cut)                                  /* bt_flash_reads, then the library's start; cut: steps to a cut */
{
    boots++;
    cut_at = cut;
    for (;;) {
        power_up();
        in_boot = 1;
        if (setjmp(cut_jb)) { cuts_boot++; cut_at = -1; continue; }   /* a cut during the power-on's erase: on again */
        bt_ps_boot();
        in_boot = 0;
        break;
    }
    cut_at = -1;
    lib_start();
    if (!memcmp(cur, want, sizeof want)) loads_saved++;
    else if (have_saving && !memcmp(cur, saving, sizeof saving)) { memcpy(want, saving, sizeof want); loads_saving++; }
    else FAIL("power-on loaded records that were never saved");
    have_saving = 0;
    if (bt_ps_dirty || !bt_ps_ok || bt_ps_bad) FAIL("power-on: dirty %d, store in use %d, failed %d", bt_ps_dirty, bt_ps_ok, bt_ps_bad);
    if ((newest() < 0) != (bt_ps.seq == 0)) FAIL("power-on: seq %u, newest slot %d", bt_ps.seq, newest());
}
static void lib_write(int id, const uint8_t *b)
{
    if (memcmp(cur[id], b, 32)) changed = 1;
    last_write = clk;
    bt_vm_open(BT_VM_FIRST + id, 32); bt_vm_seek(0, 0);
    if (bt_vm_write((void *)(uintptr_t)b, 32) != 32) FAIL("short write");
    bt_vm_close();
    memcpy(cur[id], b, 32);
}
static void lib_event(void)                                     /* a pairing, a reconnection, a key erased, a type learnt */
{
    int k, id, nw = 1 + rand() % 3;
    uint8_t b[32];
    uint32_t i;
    for (k = 0; k < nw; k++) {
        id = rand() % 3 == 0 ? 20 : rand() % 10;                /* (table 0's ten records and the one-record table, mostly) */
        if (rand() % 5 == 0) memset(b, 0xFF, sizeof b); else for (i = 0; i < 32u; i++) b[i] = (uint8_t)rand();
        clk += 50u + (uint32_t)(rand() % 400);
        lib_write(id, b);
    }
}
static void lib_pair(void)                                      /* a new link key: certainly a change */
{
    uint8_t b[32];
    uint32_t i;
    for (i = 0; i < 32u; i++) b[i] = (uint8_t)rand();
    b[0] ^= cur[3][0] ^ b[0] ^ 0x5Au;
    clk += 100u;
    lib_write(3, b);
}
static int service(uint32_t dt)                                 /* the main loop dt ms later: 1 if a copy was saved */
{
    int h = holds, due = changed && clk + dt - last_write >= WAIT;
    uint16_t s = bt_ps_saves;
    clk += dt;
    bt_ps_service(clk);
    if (hold || holds - h > 1) FAIL("the audio hold left on, or taken twice");
    if (holds != h && !changed) FAIL("a save without a change");
    if (holds != h && clk - last_write < WAIT) FAIL("a save %u ms after the library's last write", clk - last_write);
    if (due && bt_ps_ok && !bt_ps_bad && holds == h) FAIL("no save %u ms after a change", clk - last_write);
    if (bt_ps_saves == s) return 0;
    saves++; changed = 0;
    return 1;
}
static void check_outside(void)
{
    uint32_t i, j;
    for (i = 0; i < sizeof nor; i++)
        if ((i < LO || i >= HI) && nor[i] != outside[i]) FAIL("flash outside the store changed at %05X", i);
    for (i = LO; i < HI; i += 0x1000u)
        for (j = 0xF00u; j < 0x1000u; j++) if (nor[i + j] != outside[i + j] && nor[i + j] != 0xFFu) FAIL("record band at %05X", i + j);
}
static void fresh(unsigned seed)                                /* a pedal whose flash holds stale data everywhere (stock cabinets) */
{
    uint32_t i;
    srand(seed);
    for (i = 0; i < sizeof nor; i++) nor[i] = (uint8_t)rand();
    memcpy(outside, nor, sizeof nor);
    memset(want, 0xFF, sizeof want);                            /* nothing saved yet: every record free */
    have_saving = 0; worn = -1; misread_at = -1;
    erase_boot = erase_save = programs = cuts_boot = cuts_save = boots = loads_saved = loads_saving = saves = 0;
    power_on(-1);
}

/* random power cuts: inside saves and power-on erases */
static void run_cuts(long rounds, unsigned seed)
{
    long it;
    fresh(seed);
    for (it = 0; it < rounds; it++) {
        lib_event();
        (void)service(WAIT - 1u);                         /* too early: nothing yet */
        memcpy(saving, cur, sizeof saving); have_saving = 1;
        if (rand() % 4 == 0) {
            cut_at = rand() % 900;                              /* inside the save's erase or program, or after it */
            if (setjmp(cut_jb)) { cuts_save++; power_on(rand() % 3 == 0 ? rand() % 4 : -1); continue; }
        }
        (void)service(2u);
        cut_at = -1;
        if (changed) FAIL("a change not saved on a sound flash");
        memcpy(want, cur, sizeof want); have_saving = 0;
        if (rand() % 3 == 0) power_on(rand() % 2 ? rand() % 4 : -1);   /* a power-off now and then */
    }
    power_on(-1);
    check_outside();
    printf("power cuts, %ld rounds, seed %u: %lu saves (%lu cuts during a save, %lu during a power-on's erase), %lu power-ons: %lu "
           "loaded the records saved, %lu those being saved when the power went; erases: %lu at power-on, %lu during a save; "
           "%lu page programs: ok\n", rounds, seed, saves, cuts_save, cuts_boot, boots, loads_saved, loads_saving, erase_boot,
           erase_save, programs);
}

/* clean sessions of `every` pairings each (no power cut): the wear figures */
static void run_sessions(long rounds, long every, unsigned seed)
{
    long it;
    fresh(seed);
    for (it = 0; it < rounds; it++) {
        lib_pair();
        if (!service(WAIT)) FAIL("a save failed on a sound flash");
        memcpy(want, cur, sizeof want);
        if ((it + 1) % every == 0) power_on(-1);
    }
    power_on(-1);
    check_outside();
    if (every <= 5 && erase_save) FAIL("sessions of %ld saves: %lu erases during a save (5 free slots at least)", every, erase_save);
    printf("sessions of %ld saves, %lu saves: %lu erases at power-on, %lu during a save (%.2f a save), %lu page programs: ok\n",
           every, saves, erase_boot, erase_save, (double)(erase_boot + erase_save) / (double)saves, programs);
}

/* a sector whose programs no longer take, then the other, then neither, with power cuts: saves fail, the copy kept stays the
 * one loaded (and a slot a failed save left blank before a torn one is never programmed again unerased) */
static void run_worn(long rounds, unsigned seed)
{
    long it, failed = 0;
    int r;
    fresh(seed);
    for (it = 0; it < rounds; it++) {
        if (it % 150 == 0) worn = (int)(it / 150 % 3) - 1;      /* none, sector 0, sector 1 */
        lib_event();
        memcpy(saving, cur, sizeof saving); have_saving = 1;
        if (rand() % 5 == 0) {
            cut_at = rand() % 1500;
            if (setjmp(cut_jb)) { cuts_save++; power_on(rand() % 3 == 0 ? rand() % 4 : -1); continue; }
        }
        for (r = 0; r < 4 && changed; r++) if (service(WAIT)) memcpy(want, cur, sizeof want);
        cut_at = -1; have_saving = 0;
        failed += changed;
        if (rand() % 4 == 0) power_on(rand() % 2 ? rand() % 4 : -1);   /* it must load the last copy that took */
    }
    worn = -1;
    power_on(-1);
    check_outside();
    printf("a worn sector, %ld rounds: %lu saves, %ld changes not saved, %lu cuts during a save, %lu power-ons, each with the last "
           "copy that took; erases: %lu at power-on, %lu during a save: ok\n", rounds, saves, failed, cuts_save, boots, erase_boot,
           erase_save);
}

/* one read misread during a save (its blank check or its read-back: the slot passed over, the next one taken) or at power-on
 * (the scans, the blank checks, the copy): the copy loaded, or an older saved one (the newest misread, nothing that held it
 * erased, and the next save wins over it), or the flash left alone for that session */
#define HIST 32
static uint8_t hist[HIST][BT_VM_N][BT_VM_REC];
static unsigned nhist;
static void run_misreads(long rounds, unsigned seed)
{
    long it;
    unsigned long left = 0, older = 0, same = 0, passed = 0;
    unsigned i;
    static uint8_t ff[BT_VM_N][BT_VM_REC];
    memset(ff, 0xFF, sizeof ff);
    fresh(seed);
    for (it = 0; it < rounds; it++) {
        lib_event();
        if (rand() % 4 == 0) misread_at = rand() % 30;
        i = bt_ps_next;
        if (!service(WAIT) && changed) FAIL("misread: a change not saved");
        passed += misread_at < 0 && (bt_ps_cur + 10u - i) % 10u == 1u;
        misread_at = -1;
        memcpy(want, cur, sizeof want);
        memcpy(hist[nhist++ % HIST], cur, sizeof cur);
        if (rand() % 2) continue;
        misread_at = rand() % 90;
        boots++;
        power_up();
        in_boot = 1; bt_ps_boot(); in_boot = 0;
        misread_at = -1;
        lib_start();
        if (!bt_ps_ok) {                                        /* left alone: the records free, in RAM only, no save */
            if (memcmp(cur, ff, sizeof ff) || bt_ps.seq || bt_ps_bad != 3) FAIL("misread: the store not in use, records loaded");
            lib_pair();
            { int h = holds; clk += WAIT; bt_ps_service(clk); if (holds != h) FAIL("misread: a save, the store not in use"); }
            left++;
            power_on(-1);                                       /* read right: the copy is there */
        } else if (memcmp(cur, want, sizeof want)) {            /* an older copy: the newest misread, in the same sector */
            for (i = 0; i < HIST && memcmp(hist[i], cur, sizeof cur); i++) ;
            if (i == HIST) FAIL("misread: power-on loaded records never saved");
            older++;
            memcpy(want, cur, sizeof want);
            lib_pair();                                         /* a save from here must win over the newest misread */
            if (!service(WAIT)) FAIL("misread: no save after an older copy");
            memcpy(want, cur, sizeof want);
            power_on(-1);
        } else same++;
    }
    power_on(-1);
    check_outside();
    printf("one read misread, %ld rounds: %lu saves passed over a slot that read back wrong; %lu power-ons, %lu as if read right, "
           "%lu an older copy (then a save won over the newest), %lu the flash left alone for the session: ok\n", rounds, passed,
           boots, same, older, left);
}

/* the merged ranges give every call the RAM driver accepts (one aligned sector, or one page) the answer of one range per area */
static void check_ranges(void)
{
    uint32_t off, n, calls = 0;
    FILE *f = fopen("firmware/px_usb.h", "r");
    char line[512];
    int found = 0;
    if (!f) f = fopen("../firmware/px_usb.h", "r");
    if (!f) FAIL("firmware/px_usb.h not found (run from the repository)");
    while (fgets(line, sizeof line, f)) if (!strncmp(line, BT_RANGE_LINE, strlen(BT_RANGE_LINE))) found = 1;
    fclose(f);
    if (!found) FAIL("firmware/px_usb.h's FL_RANGE_OK for the Bluetooth audio image is not this test's: update both");
    for (off = 0; off < 0x100000u; off += 0x1000u, calls++)
        if (FL_RANGE_OK(off, 0x1000u) != (FL_RANGE_REL(off, 0x1000u) || FL_IN(off, 0x1000u, LO, HI))) FAIL("erase at %05X", off);
    for (off = 0xB0000u; off < 0x100000u; off++)
        for (n = 1; n <= 256u - (off & 0xFFu); n += (n < 4u || 256u - (off & 0xFFu) - n < 4u) ? 1u : 61u, calls++)
            if (FL_RANGE_OK(off, n) != (FL_RANGE_REL(off, n) || FL_IN(off, n, LO, HI))) FAIL("program at %05X+%u", off, n);
    printf("FL_RANGE_OK merged: %u erases and programs, the same answers as one range per area: ok\n", calls);
}

int main(int argc, char **argv)
{
    long rounds = argc > 1 ? atol(argv[1]) : 20000;
    unsigned seed = argc > 2 ? (unsigned)atol(argv[2]) : 1u;
    static const uint8_t kat[] = "123456789";
    if (bt_crc32(kat, 9) != 0xCBF43926u || crc_ref(kat, 9) != 0xCBF43926u) FAIL("CRC-32 check value");
    if (offsetof(bt_ps_slot_t, seq) != 672u || offsetof(bt_ps_slot_t, crc) != 676u || offsetof(bt_ps_slot_t, magic) != 680u)
        FAIL("slot layout");
    check_ranges();
    run_cuts(rounds, seed);
    run_cuts(rounds, seed + 1u);
    run_sessions(2000, 1, seed + 2u);
    run_sessions(2000, 5, seed + 3u);
    run_sessions(2000, 10, seed + 4u);
    run_sessions(2000, 2000, seed + 5u);
    run_worn(rounds / 4, seed + 6u);
    run_misreads(rounds / 2, seed + 7u);
    printf("pairing store: PASS\n");
    return 0;
}
