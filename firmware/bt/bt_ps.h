/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The Bluetooth audio image's pairing records: in RAM while the pedal runs, and since test build 14 in the flash across
 * power-off (bt_glue.c includes this file; tests/test_bt_ps.c runs it on a model of the flash with power cuts at every step).
 *
 * The classic host keeps them through the SDK's VM file interface (database_file.c, through set_bt_vm_interface): 21 records of
 * 32 bytes with ids 62 to 82 (62 + its three tables' starts 0, 10 and 20 + the record), each access open(id, 32), seek(0, 0),
 * read or write, close (the SDK's struct syscfg_file, its member order). A record starts as erased flash, all 0xFF: free, and its
 * check fails. The library writes a record only when it differs from its newest one: when a phone pairs (the link key), when a
 * known phone that was not the last one reconnects, when a key fails (then erased) and when it learns a phone's type. So one or
 * two writes at a pairing, none when the last phone reconnects. Up to test build 10 there was no store (NULL): the library then
 * skipped every read but reported it done, and compared a stale stack buffer as if it were a record. Test builds 11 to 13 kept
 * the records in RAM only: after a power-off the phone had to forget the pedal and pair again.
 *
 * The flash copy: two 4 KiB sectors at 0xEE000 and 0xEF000, just below the preset store (px_store.h, 0xF0000), where the stock
 * firmware kept cabinets and OpenPocket keeps nothing; only this image may write them (px_usb.h FL_RANGE_OK). A sector holds 5
 * slots of 684 bytes: the 21 records, then seq, a CRC-32 of the records and seq, and the mark "OPBT", programmed from the RAM copy
 * itself in that order, so the mark goes last. At power-on the valid slot with the highest seq is loaded and the other sector is
 * erased if it is not blank (bt_ps_boot, before the audio and the radio start): a session then has the blank slots after the one
 * loaded and the other sector's 5 before a save needs an erase. A save goes to the next slot (one a power cut left torn is passed
 * over, and each slot tried takes a seq of its own), is read back, and is made from the main loop 1 s after the library's last
 * write, with the audio held as for a preset save (bt_ps_service). A power cut during a save loses that save only; the sector
 * that holds the newest good copy is never erased. Bytes 0xF00..0xFFF of each sector are never written: there the update loader
 * looks for its records (loader/ldr_core.c), so an update leaves the store as it is.
 *
 * From the includer: <stdint.h>, <stddef.h>, memset, ble_ms (ble_os.h), BT_CRUMB, ota_fread (px_usb.h), fl_write and fl_erase4k
 * (fm1_flash.h), st_audio_hold (px_store.h), fm1_wdt_feed. */
#pragma once

/* the CRC-32 (IEEE, reflected) of a slot, and of the flash's unique ID for this pedal's address (bt_glue.c bt_mac_make) */
static __attribute__((noinline)) uint32_t bt_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1; }
    return ~c;
}

/* ---- the records, as the library reads and writes them */
#define BT_VM_FIRST 62
#define BT_VM_N 21
#define BT_VM_REC 32
typedef struct {
    int (*open)(int id, int len);
    int (*read)(void *buf, int len);
    int (*write)(void *buf, int len);
    int (*seek)(int whence, int off);
    int (*close)(void);
} bt_vm_file_t;
/* the records and, behind them, the trailer of their copy in the flash: a save programs this struct as it is, the mark last */
typedef struct { uint8_t rec[BT_VM_N][BT_VM_REC]; uint32_t seq, crc, magic; } bt_ps_slot_t;
static bt_ps_slot_t bt_ps __attribute__((aligned(4)));
#define bt_vm_rec bt_ps.rec
static int bt_vm_cur = -1, bt_vm_pos;
static uint32_t bt_vm_reads, bt_vm_writes;                      /* (the log's VM r/w: bt_a2dp.c bt_isr_line) */
static uint32_t bt_ps_t;                                        /* the library's last write (ms) */
static uint16_t bt_ps_saves;                                    /* copies saved since power-on */
static uint8_t bt_ps_next, bt_ps_cur;                           /* the slot the next save goes to; the newest good copy's */
static uint8_t bt_ps_dirty, bt_ps_ok, bt_ps_bad;                /* a change not saved yet; the flash copy in use; failed rounds */
static int bt_vm_open(int id, int len)
{
    if (id < BT_VM_FIRST || id >= BT_VM_FIRST + BT_VM_N || len <= 0 || len > BT_VM_REC) { bt_vm_cur = -1; return 0; }
    bt_vm_cur = id - BT_VM_FIRST; bt_vm_pos = 0;
    return 1;
}
static int bt_vm_rw(void *buf, int len, int wr)
{
    uint8_t *b = buf, *r;
    int i, n;
    if (bt_vm_cur < 0 || !buf || len <= 0 || bt_vm_pos >= BT_VM_REC) return 0;
    n = len < BT_VM_REC - bt_vm_pos ? len : BT_VM_REC - bt_vm_pos;
    r = bt_vm_rec[bt_vm_cur] + bt_vm_pos;
    for (i = 0; i < n; i++) { if (!wr) b[i] = r[i]; else if (r[i] != b[i]) { r[i] = b[i]; bt_ps_dirty = 1; } }
    bt_vm_pos += n;
    return n;
}
static int bt_vm_read(void *buf, int len) { bt_vm_reads++; return bt_vm_rw(buf, len, 0); }
static int bt_vm_write(void *buf, int len) { bt_vm_writes++; bt_ps_t = ble_ms(); return bt_vm_rw(buf, len, 1); }   /* (in a task) */
static int bt_vm_seek(int whence, int off)
{
    int p = whence == 0 ? off : whence == 1 ? bt_vm_pos + off : BT_VM_REC - off;
    bt_vm_pos = p < 0 ? 0 : p > BT_VM_REC ? BT_VM_REC : p;
    return 0;
}
static int bt_vm_close(void) { bt_vm_cur = -1; return 0; }
static const bt_vm_file_t bt_vm_file = { bt_vm_open, bt_vm_read, bt_vm_write, bt_vm_seek, bt_vm_close };
void *bt_vm_interface(void)                                     /* (btstack_main.c, once, before the database is read) */
{
    BT_CRUMB("bt_vm_interface");
    if (!bt_ps_ok) memset(bt_vm_rec, 0xFF, sizeof bt_vm_rec);   /* (no flash copy in use, bt_ps_boot: every record free, in RAM only) */
    return (void *)(uintptr_t)&bt_vm_file;
}

/* ---- their copy in the flash */
#define BT_PS_A     0xEE000u                                    /* the first sector; the second at + 0x1000 */
#define BT_PS_N     5u                                          /* slots in a sector: 10 in all, numbered 0..9 */
#define BT_PS_NONE  0xFFu
#define BT_PS_MAGIC 0x5442504Fu                                 /* "OPBT" */
#define BT_PS_WAIT  1000u                                       /* ms after the library's last write */
typedef char bt_ps_size_check[(sizeof(bt_ps_slot_t) == 684u && BT_PS_N * sizeof(bt_ps_slot_t) <= 0xF00u) ? 1 : -1];
static uint32_t bt_ps_off(uint32_t i) { return BT_PS_A + (i / BT_PS_N) * 0x1000u + (i % BT_PS_N) * (uint32_t)sizeof bt_ps; }
static __attribute__((noinline)) uint32_t bt_ps_crc(void) { return bt_crc32((const uint8_t *)&bt_ps, (uint32_t)offsetof(bt_ps_slot_t, crc)); }
static int bt_ps_valid(void) { return bt_ps.magic == BT_PS_MAGIC && bt_ps.crc == bt_ps_crc(); }
static int bt_ps_same(uint32_t off, uint32_t n, const uint32_t *ref)   /* 1: the n bytes at off are ref's, erased if ref is NULL
                                                                         * (n a multiple of 4) */
{
    uint32_t b[16], i, k;
    for (; n; n -= k, off += k) {
        k = n < sizeof b ? n : (uint32_t)sizeof b;
        if (ota_fread(off, b, k)) return 0;
        for (i = 0; i < k / 4u; i++) if (b[i] != (ref ? *ref++ : 0xFFFFFFFFu)) return 0;
    }
    return 1;
}
static uint32_t bt_ps_scan(void)                                /* the valid slot with the highest seq, BT_PS_NONE if none (bt_ps
                                                                 * then holds the last slot read) */
{
    uint32_t i, best = BT_PS_NONE, bseq = 0;
    for (i = 0; i < 2u * BT_PS_N; i++)
        if (!ota_fread(bt_ps_off(i), &bt_ps, sizeof bt_ps) && bt_ps_valid() && (best == BT_PS_NONE || (int32_t)(bt_ps.seq - bseq) > 0)) {
            best = i; bseq = bt_ps.seq;
        }
    return best;
}

/* power-on (bt_a2dp.c bt_flash_reads, the flash being the expected part), before the audio and the radio start: the newest valid
 * copy into RAM, the other sector blank for this session (an erase holds the interrupts off for its time, tens of ms), and the
 * slot the next save goes to: the first of the blank ones that end the copy's sector, else the other sector's first. Each slot a
 * save tries takes the next seq (bt_ps_service), and seq here counts the slots passed over, so no two slots ever share a seq and
 * a torn slot that reads valid one day is never taken for a later save. The other sector is erased, and a power-on that finds
 * no valid copy goes on, only when a second reading of every slot finds the same: the newest slot misread the first time
 * (taken for a torn one) may lie in that sector, or be the only copy. With no valid copy the second sector is used first: the
 * first may still hold the stock firmware's data, and is erased at the power-on after the first save. Slots that read otherwise
 * the second time leave the flash alone: the records stay in RAM for this session, as up to test build 13 (the log's f 3) */
static __attribute__((noinline)) void bt_ps_boot(void)
{
    uint32_t i, best = bt_ps_scan(), oth = best == BT_PS_NONE ? 1u : (best / BT_PS_N) ^ 1u, took;
    int ok = 1, er = !bt_ps_same(BT_PS_A + oth * 0x1000u, BT_PS_N * (uint32_t)sizeof bt_ps, 0);   /* the sector without the copy */
    if (er || best == BT_PS_NONE) ok = bt_ps_scan() == best;
    if (ok && er) { fm1_wdt_feed(); (void)fl_erase4k(BT_PS_A + oth * 0x1000u, &took); fm1_wdt_feed(); }
    if (ok && best != BT_PS_NONE) ok = !ota_fread(bt_ps_off(best), &bt_ps, sizeof bt_ps) && bt_ps_valid();
    if (!ok || best == BT_PS_NONE) {
        memset(&bt_ps, 0xFF, sizeof bt_ps);                     /* every record free */
        bt_ps.seq = 0;
        if (!ok) { bt_ps_bad = 3; return; }
    }
    bt_ps_next = (uint8_t)(oth * BT_PS_N);
    for (i = (best / BT_PS_N) * BT_PS_N + BT_PS_N - 1u; best != BT_PS_NONE && i > best && bt_ps_same(bt_ps_off(i), sizeof bt_ps, 0); i--)
        bt_ps_next = (uint8_t)i;
    if (best != BT_PS_NONE) bt_ps.seq += (bt_ps_next + 2u * BT_PS_N - best) % (2u * BT_PS_N) - 1u;
    bt_ps_cur = (uint8_t)best;
    bt_ps_ok = 1;
}

/* the main loop (bt_a2dp.c ble_test_poll, after ble_poll: the library's tasks are waiting): a change saved 1 s after the
 * library's last write. The audio is held as for a preset save (px_store.h st_audio_hold: 12 ms of silence, its interrupt
 * masked, the second core idle in its RAM loop), then 3 or 4 page programs of up to 256 bytes, interrupts off for each and on
 * between them (the radio's run there), and the read-back: about 25 ms of silence, once per pairing. An erase only when this
 * session's saves have used every free slot, and never of the sector that holds the newest good copy. A round tries two slots;
 * a failed round is tried again 1 s later, twice; after that only a new change of the library's starts another (the log's
 * VM r/w/s/f/q) */
static void bt_ps_service(uint32_t now)
{
    uint32_t t, k, off;
    int ok = 0;
    if (!bt_ps_dirty || !bt_ps_ok || now - bt_ps_t < BT_PS_WAIT) return;
    bt_ps_dirty = 0;
    st_audio_hold(1);
    for (t = 0; t < 2u && !ok; t++) {
        off = bt_ps_off(bt_ps_next);
        if (!(bt_ps_next % BT_PS_N) && !bt_ps_same(off, BT_PS_N * (uint32_t)sizeof bt_ps, 0)) {   /* a used sector: erased first */
            if (bt_ps_cur != BT_PS_NONE && bt_ps_cur / BT_PS_N == bt_ps_next / BT_PS_N) break;   /* (the other one's slots failed) */
            (void)fl_erase4k(off, &k);
        }
        bt_ps.seq++; bt_ps.magic = BT_PS_MAGIC; bt_ps.crc = bt_ps_crc();   /* (a seq for each slot tried) */
        ok = !fl_write(off, (const uint8_t *)&bt_ps, sizeof bt_ps) && bt_ps_same(off, sizeof bt_ps, (const uint32_t *)(const void *)&bt_ps);
        if (ok) bt_ps_cur = bt_ps_next;
        bt_ps_next = (uint8_t)((bt_ps_next + 1u) % (2u * BT_PS_N));
    }
    st_audio_hold(0);
    if (ok) bt_ps_saves++;
    else if (++bt_ps_bad < 3u) { bt_ps_dirty = 1; bt_ps_t = now; }
}
