/* SPDX-License-Identifier: GPL-3.0-only
 * USB and the flash on this computer: firmware/px_usb.h's stand-in for the simulator of an app on the base firmware
 * (tests/host/base/board.h). The update code's handshake is the pedal's own (firmware/bsp/chip/ota.c), so the
 * installer's and the flasher's identity query is answered as a pedal answers it; an update itself is not simulated
 * here (tests/devsim.c does that). MIDI comes from the simulator's input (tests/host/base/start.h): SysEx as frames,
 * the other messages through the ring usb.c fills.
 */
#pragma once
#include "base/flashmap.h"
#ifndef OP_FLASH_OWN
#define OP_FLASH_OWN(off, n) 0
#endif

/* ---- the flash: 1 MiB; the app may write its own range only (the update's staging area is the update code's) ---- */
static uint8_t h_nor[0x100000];
static int h_writes_ok = 1, h_flash_dirty;
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && (uint32_t)(n) <= (hi) - (uint32_t)(off))
static uint8_t px_flash_ok = 1;
static int ota_fread(uint32_t off, void *p, uint32_t n) { if (off + n > sizeof h_nor) return -1; memcpy(p, h_nor + off, n); return 0; }
static int fl_erase4k(uint32_t off, uint32_t *took)
{
    *took = 0;
    if (!OP_FLASH_OWN(off, 0x1000u) || (off & 0xFFFu) || !a_mute || h_irq_held != 1) { h_writes_ok = 0; return -1; }
    memset(h_nor + off, 0xFF, 0x1000u);
    h_flash_dirty = 1;
    return 0;
}
static int fl_write(uint32_t off, const uint8_t *s, uint32_t n)
{
    if (!OP_FLASH_OWN(off, n) || !a_mute || h_irq_held != 1) { h_writes_ok = 0; return -1; }
    for (uint32_t i = 0; i < n; i++) h_nor[off + i] &= s[i];
    h_flash_dirty = 1;
    return 0;
}

/* ---- SysEx frames in and out, and the ring of the other MIDI messages (usb.c's) ---- */
static uint8_t h_frame[2048];
static uint32_t h_frame_len, h_frame_full;
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!h_frame_full) return 0; *p = h_frame; *n = h_frame_len; return 1; }
static void ota_frame_done(void) { h_frame_full = 0; }
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) fprintf(stdout, "%02x", p[i]);
    fputs("\n", stdout);
    fflush(stdout);
    return 0;
}
#define MQ 64u
static uint32_t midi_in_q[MQ], midi_in_ms[MQ];
static volatile uint32_t mi_w, mi_r;
static volatile uint8_t midi_in_overflow;
#define RING_PUBLISH() ((void)0)

/* ---- the update entry: only its handshake runs here ---- */
#define FELUCCA_ID_PTR ("PocketX_9" OP_VERSION_NUM)
static uint32_t ota_now_ms(void) { return ms_now(); }
static void ota_idle(void) { }
static int ota_erase(uint32_t off) { (void)off; return -8; }
static int ota_prog(uint32_t off, const void *p, uint32_t n) { (void)off; (void)p; (void)n; return -8; }
static void ota_show(uint32_t step, int32_t code) { (void)step; (void)code; }
static void ota_commit(const uint8_t *parm) { (void)parm; }
#include "bsp/chip/ota.c"

#define BS_IRQ_OFF() ((void)0)                  /* (base/store.h: no interrupts to turn off here) */
static void px_flash_boot(void) { }
static void px_usb_start(void) { }
static void ed_service(uint32_t now);
static void h_host(void);                       /* tests/host/base/start.h: what the host of the simulator asks */
static void px_usb_service(uint32_t now)        /* every main-loop pass */
{
    h_host();
    ed_service(now);
    ota_service();
}
