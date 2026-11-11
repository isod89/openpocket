/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — USB-MIDI + the M-UPGRADE update entry, so the web installer can update the pedal over USB.
 *
 * The USB device and the update protocol are felucca-bsp's (chip/usb.c, chip/ota.c, MPL-2.0, same chip as the FM-1):
 *   - usb_poll() runs from the TIMER5 interrupt at 2 kHz (10 kHz tick, every 5th), as in felucca-bsp's hwtest;
 *   - the main loop answers the handshake (ota_service) and, on the host's "upgrade" command, runs ota_session():
 *     it reads the package's update loader from the host, stages it at 0xE0000 (5 x 4 KiB) and resets into it;
 *     the loader (loader/) then writes the new app area. Nothing is committed before the host says "success".
 *   - the SysEx soft key F0 22 24 35 7D F7 enters the chip's UBOOT mode (rescue).
 * The identity the pedal reports ("PocketX_924", "BlackBox_924") is OP_PRODUCT, which tools/pxpkg.py writes into the
 * app of each package; an image not made by pxpkg (the SD-card image) reports the default below.
 */
#pragma once

#define FELUCCA_OTA 1
#define FELUCCA_CDC 0
#define FELUCCA_USB_PRODUCT "OpenPocket"
#define FELUCCA_USB_MANUFACTURER "OpenPocket"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
/* the app may only write the OTA staging area (its loader + update record), the preset store (px_store.h) and,
 * v37, the NAM user models (px_models.h) */
#define FL_RANGE_OK(off, n) (FL_IN(off, n, FL_OTA_LO, FL_OTA_HI) || FL_IN(off, n, 0xF0000u, 0xF4000u) || \
                             FL_IN(off, n, 0xC0000u, 0xE0000u) || FL_IN(off, n, 0xF4000u, 0xFC000u))

/* the package identity, patched by pxpkg.py ("OP-PRODUCT:" + identity, 32 bytes, exactly once in the app) */
static const volatile char OP_PRODUCT[32] __attribute__((used, aligned(4))) = "OP-PRODUCT:PocketX_9" OP_VERSION_NUM;
#define FELUCCA_ID_PTR ((const char *)OP_PRODUCT + 11)

#include "fm1_timer.h"
#if OP_BLE_TEST
static int ble_stall_tick(void);                /* ble/ble_status.h, included after this file */
#if OP_BT_AUDIO
static void bt_stall_tick(int firing);          /* bt/bt_a2dp.c: the stall record's samples (bt/bt_stall.h) */
#endif
#endif
#include "fm1_flash.h"
#include "fm1_cpu1_stop.h"
#include "usb.c"
static uint8_t *ota_scratch(void);              /* px_models.h: the upload staging buffer, idle while ota_session blocks */
#define OTA_SCRATCH ota_scratch()
#include "ota.c"

static uint8_t px_flash_ok;                     /* the flash is a 1 MiB SPI NOR: the update entry may stage */
static volatile uint32_t px_t5_sub;

#if OP_BLE_TEST
static volatile uint32_t px_t5_in_s, px_t5_in;  /* its longest stay, this second and since boot, TIMER4 ticks (the log's isr line) */
#endif
void fm1_timer5_irq(void)                       /* 10 kHz; USB at 2 kHz */
{
#if OP_BLE_TEST
    const uint32_t t0 = fm1_ticks();
#endif
    fm1_timer5_ack();
#if OP_BLE_TEST                                 /* a ble_poll that does not return: the crash screen says where (ble_status.h) */
    {
        const int stalled = ble_stall_tick();
#if OP_BT_AUDIO
        bt_stall_tick(stalled);                 /* (its last second sampled, the record completed when it fires) */
#endif
        if (stalled) {
#if DSP_CPU1
            fm1_cpu1_nobar = 1;                 /* (this crash is the watch's own, not the second core's fault: no bar) */
#endif
            FM1_VEC[FM1_IRQ_TIMER5] = (uint32_t)(uintptr_t)(fm1_fatal_stubs + 6u * FM1_IRQ_TIMER5);
        }
    }
#endif
    if (++px_t5_sub >= 5u) {
        px_t5_sub = 0;
        usb_poll();
    }
#if OP_BLE_TEST
    {
        const uint32_t d = fm1_ticks() - t0;
        if (d > px_t5_in_s) px_t5_in_s = d;
        if (d > px_t5_in) px_t5_in = d;
    }
#endif
}
extern void isr_timer5(void);

#ifndef PX_LIGHTS_OFF                           /* the button lights off while an update runs (openpocket.c: px_lights.h) */
#define PX_LIGHTS_OFF() ((void)0)
#endif

/* ---- ota.c hooks ---- */
static uint32_t ota_now_ms(void) { return ms_now(); }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n)
{
    uint8_t *d = p;
    while (n) {                                 /* 256-byte windows, interrupts off (the SFC is switched off) */
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
#if DSP_CPU1 && OP_BT_AUDIO
        (void)fm1_cpu1_wait();                  /* (the Bluetooth audio image reads here with the audio running: the second
                                                 * core's job in flight may read the flash, and no new one starts; a core that
                                                 * faulted is held: its handler parks in the flash) */
#endif
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK", "LOADER", "CONFIRM", "RESTART"};
    PX_LIGHTS_OFF();
    lcd_rect(0, 0, 240, 240, UI_BG);
    ft_box(0, 92, 240, "UPDATE", &FT_MD, UI_ACC, UI_BG, 1);
    if (step < 9u) {
        ft_box(0, 124, 240, STEP[step < 6u ? step : 0], &FT_SM, UI_TXT, UI_BG, 1);
        ft_box(0, 150, 240, "DO NOT UNPLUG", &FT_SM, UI_DIM, UI_BG, 1);
        return;
    }
    {
        char b[16] = "FAILED -";
        uint32_t k = 8, v = (uint32_t)(code < 0 ? -code : code);
        if (v >= 10u) b[k++] = (char)('0' + v / 10u);
        b[k++] = (char)('0' + v % 10u);
        b[k] = 0;
        ft_box(0, 124, 240, b, &FT_SM, RED, UI_BG, 1);
    }
    for (int i = 0; i < 15; i++) { fm1_wdt_feed(); fm1_delay_ms(100); }
}
static void ota_commit(const uint8_t *parm)
{
    guard.pending = 0;                          /* an intentional reset, not a failed boot */
    guard.failed = 0;
    op_diag.stage = ST_OK;
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);                     /* record into RAM, core reset: the SPL runs the staged loader */
}

/* ---- boot / main loop ---- */
static void px_usb_start(void)                  /* interrupts still off (before audio_start) */
{
    uint32_t f = irq_save(), id = FL_FAR(fl_jedec_ram)();
    irq_restore(f);
    __asm__ volatile("cli" ::: "memory");       /* (irq_restore turned them on: back off until audio_start) */
    px_flash_ok = (id & 0xFFu) == 0x14u && id != 0xFFFFFFu && id != 0u;   /* 2^0x14 = 1 MiB */
    if (px_flash_ok)
        ota_boot_cleanup();                     /* a staging area left by an update: the SPL must not re-enter it */
    usb_start();
    fm1_timer5_start(isr_timer5, 4);            /* above ALNK0 (3), as felucca-bsp */
}
static void px_audio_halt(void)                 /* before any flash write: nothing may run from XIP but us */
{
    fm1_irq_mask(FM1_IRQ_ALNK0);
    a_collect();
    gpio_direction_output(G_AMP_EN, 0);         /* amp off: no noise while the flash is busy */
#if DSP_CPU1
    fm1_cpu1_stop();
    a_c1 = 0;
#endif
}
static void ed_service(uint32_t now);
static void px_usb_service(uint32_t now)        /* every main-loop pass */
{
    usb_retry(now);
    if (usb.uboot_req) {                        /* SysEx soft key: the chip's UBOOT (rescue) */
        px_audio_halt();
        lcd_rect(0, 0, 240, 240, UI_BG);
        ft_box(0, 110, 240, "UBOOT (USB)", &FT_MD, UI_ACC, UI_BG, 1);
        guard.pending = 0;
        op_diag.stage = ST_OK;
        usb_detach();
        fm1_delay_ms(30);
        fm1_enter_uboot();
    }
    ed_service(now);                            /* the web editor (px_editor.h): its frames first */
    ota_service();                              /* the installer's handshake */
    if (usb.ota_req) {                          /* its upgrade command: commits and resets, or comes back */
        usb.ota_req = 0;
        if (px_flash_ok) {
            px_audio_halt();
            ota_session();                      /* returns only when nothing was committed */
            guard.pending = 0;
            op_diag.stage = ST_OK;
            fm1_reboot();                       /* back to a clean start (audio, second core) */
        }
    }
}
