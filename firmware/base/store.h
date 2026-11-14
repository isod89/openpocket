/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): an app's own data in flash (its presets, its settings). It is kept across
 * a firmware update whichever image's loader runs it, and it is never OpenPocket's: another place, another magic.
 *
 * The app keeps its data as one block of at most BS_MAX bytes in RAM and calls:
 *   bs_load(magic, data, max, &len, &ver)   at the start, after px_flash_boot() and before px_usb_start() (interrupts
 *       still off): 1 = data holds the newest whole copy written with this magic, len its length and ver its format,
 *       for an app that reads its own older formats; 0 = none, and data is untouched (the app's factory state stands)
 *   bs_save(magic, ver, data, len)          0 = written and read back; the sound is muted for the write
 * and its file defines, before px_usb.h, the flash it may write (base/flashmap.h):
 *   #define OP_FLASH_OWN(off, n) BS_FLASH_OWN(off, n)
 *
 * Where: 0xE5000..0xEF000, ten 4 KiB sectors between the update's staging area and OpenPocket's presets, which
 * OpenPocket never writes (its own data: 0xC0000..0xE0000 and 0xF0000..0xFC000). Two copies of five sectors, written in
 * turn. A copy's header goes last, with a sequence number and a CRC16 of the header and the data: at the start the
 * whole copy with the highest sequence wins, so a power cut during a save leaves the copy before it.
 * Nothing is written before the first bs_save: an app that was only installed and tried leaves this flash as it was
 * (on a pedal that came from the official firmware it holds part of the official sound bank).
 *
 * The last 256 bytes of every sector stay erased. The boot code and every update loader look there, at each 4 KiB
 * boundary - 256 between 0xA3000 and 0xFF000, for the record of an update in progress, and a loader erases a sector
 * that ends like one (loader/ldr_core.c). OpenPocket's loader skips OpenPocket's own ranges by address. It does not
 * know this one, so these bytes are what keeps an app's data through a stay on OpenPocket.
 */
#pragma once

#include "base/flashmap.h"                      /* BS_LO, BS_HI, BS_FLASH_OWN */
#define BS_NSEC    5u                           /* sectors a copy */
#define BS_USE     0xF00u                       /* bytes used in a sector: the rest stays erased (see above) */
#define BS_MAX     (BS_NSEC * BS_USE - 16u)     /* 19184: the most an app can keep */
#ifndef BS_IRQ_OFF
#define BS_IRQ_OFF() __asm__ volatile("cli" ::: "memory")
#endif

typedef struct { uint32_t magic, seq, len; uint16_t crc; uint8_t ver, pad; } bs_hdr_t;   /* 16 B, at a copy's start */
static uint32_t bs_seq, bs_cur;                 /* the sequence of the copy in use, its base (0: none yet) */

/* byte i of a copy (the header first, then the data) is in its sector i / BS_USE */
static uint32_t bs_off(uint32_t base, uint32_t i) { return base + (i / BS_USE) * 0x1000u + i % BS_USE; }
/* a copy's CRC16: of its header's magic, sequence and length, started from its version, then of its data */
static uint32_t bs_crc(const bs_hdr_t *h, const uint8_t *data)
{
    return ota_crc16(data, h->len, ota_crc16((const uint8_t *)h, 12u, h->ver));
}
/* n bytes of a copy from byte i on, to or from the flash, sector by sector; 0 = ok */
static int bs_io(uint32_t base, uint32_t i, uint8_t *p, uint32_t n, int write)
{
    while (n) {
        uint32_t k = BS_USE - i % BS_USE;
        if (k > n) k = n;
        if (write ? fl_write(bs_off(base, i), p, k) : ota_fread(bs_off(base, i), p, k)) return -1;
        if (write) fm1_wdt_feed();
        i += k; p += k; n -= k;
    }
    return 0;
}

/* copy `base` is whole: its header's magic is this one, its length fits, its CRC is right */
static int bs_whole(uint32_t base, uint32_t magic, uint32_t max, bs_hdr_t *h)
{
    static uint8_t b[256];
    uint32_t i, k, c;
    if (ota_fread(base, h, sizeof *h) || h->magic != magic || h->len > max || h->len > BS_MAX) return 0;
    c = ota_crc16((const uint8_t *)h, 12u, h->ver);
    for (i = 0; i < h->len; i += k) {
        k = h->len - i > sizeof b ? sizeof b : h->len - i;
        if (bs_io(base, sizeof *h + i, b, k, 0)) return 0;
        c = ota_crc16(b, k, c);
    }
    return c == h->crc;
}
static int bs_load(uint32_t magic, void *data, uint32_t max, uint32_t *len, uint32_t *ver)
{
    static const uint32_t BASE[2] = {BS_LO, BS_LO + BS_NSEC * 0x1000u};
    bs_hdr_t h, best;
    uint32_t i, found = 0;
    for (i = 0; px_flash_ok && i < 2u; i++)
        if (bs_whole(BASE[i], magic, max, &h) && (!found || (int32_t)(h.seq - best.seq) > 0)) { best = h; found = BASE[i]; }
    if (found && bs_io(found, sizeof best, data, best.len, 0)) found = 0;
    BS_IRQ_OFF();                               /* (ota_fread turned them on) */
    if (!found) return 0;
    bs_cur = found; bs_seq = best.seq;
    *len = best.len; *ver = best.ver;
    return 1;
}

/* audio off the flash: muted output, the interrupt held, nothing in progress on another core (as px_store.h's) */
static void bs_audio_hold(int on)
{
    if (on) {
        a_mute = 1;
        for (int i = 0; i < 12; i++) { fm1_wdt_feed(); fm1_delay_ms(1); }   /* > 2 blocks: the DMA plays zeros */
        fm1_irq_mask(FM1_IRQ_ALNK0);
        a_collect();
    } else {
        fm1_irq_unmask(FM1_IRQ_ALNK0);
        a_mute = 0;
    }
}

static int bs_save(uint32_t magic, uint32_t ver, const void *data, uint32_t len)
{
    const uint32_t base = bs_cur == BS_LO ? BS_LO + BS_NSEC * 0x1000u : BS_LO;
    bs_hdr_t h = {magic, bs_seq + 1u, len, 0, (uint8_t)ver, 0}, r;
    static uint8_t chk[256];
    uint32_t i, k;
    int rc = 0;
    if (!px_flash_ok || len > BS_MAX) return -1;
    h.crc = (uint16_t)bs_crc(&h, data);
    bs_audio_hold(1);
    for (i = 0; !rc && i * BS_USE < sizeof h + len; i++) {   /* the sectors this copy needs */
        fm1_wdt_feed();
        if (fl_erase4k(base + i * 0x1000u, &k)) rc = -2;
    }
    if (!rc && bs_io(base, sizeof h, (uint8_t *)(uintptr_t)data, len, 1)) rc = -3;
    if (!rc && fl_write(base, (const uint8_t *)&h, sizeof h)) rc = -4;      /* the header last */
    if (!rc && (ota_fread(base, &r, sizeof r) || memcmp(&r, &h, sizeof h))) rc = -5;
    for (i = 0; !rc && i < len; i += k) {       /* read back */
        k = len - i > sizeof chk ? sizeof chk : len - i;
        if (bs_io(base, sizeof h + i, chk, k, 0) || memcmp(chk, (const uint8_t *)data + i, k)) rc = -5;
    }
    bs_audio_hold(0);
    if (!rc) { bs_cur = base; bs_seq = h.seq; }
    return rc;
}
