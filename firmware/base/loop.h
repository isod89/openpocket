/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the start and the main loop's duties, for an app on the plain schedule
 * (base/audio_plain.h). They are what keeps a pedal recoverable whatever the app does: USB is up before the app's
 * sound runs, the watchdog is fed, the boot guard is told that the start finished, the MIDI ring is read.
 *   base_begin()     the screen, the boot screen and the last start's report, the controls, the flash. Interrupts
 *                    are still off: the app reads its store next (base/store.h: bs_load)
 *   base_go()        USB-MIDI and the update entry (the way back to OpenPocket), then the audio
 *   base_loop(now)   first in every main-loop pass
 * base_safe: 1 = this start is in safe mode (the one before did not finish): the app's sound stays off, the guitar
 * passes, USB works. A start counts as finished after 6 s in the loop. One that does not get there makes the next one
 * safe, and two in a row enter the chip's UBOOT (base/start.h).
 * Included after px_usb.h, base/midi.h and base/audio_plain.h. (OpenPocket's own start and loop: openpocket.c.)
 */
#pragma once

static int base_safe;

static void base_begin(void)
{
    lcd_init();
    {
#include "base/boot_screens.h"          /* (statements) they declare safe */
        base_safe = safe;
    }
    op_stage(ST_INPUTS, "INPUTS...");
    input_init();
    adc_pins_analog();
    input_baseline();
    a_hold = 1;                          /* the app's sound held off until the start is through (in safe mode: for good) */
    px_flash_boot();                     /* the flash's size; a staging area an update left */
    op_stage(ST_PRESET, "PRESET...");
}

static void base_go(void)
{
    px_usb_start();
    audio_io_start();
    op_stage(ST_UI, "SCREEN...");
}

static void base_loop(uint32_t now)
{
    static uint32_t t_diag;
    static uint8_t running, boots_ok;
    fm1_wdt_feed();
    if (!running) { running = 1; op_stage(ST_RUN, 0); }
    px_usb_service(now);                 /* the installer: handshake, update, UBOOT soft key; INFO and the app's frames */
    midi_poll();                         /* the MIDI ring emptied: left full, it holds the installer's SysEx back */
    if (!boots_ok && now > 6000u) { boots_ok = 1; guard.pending = 0; guard.failed = 0; op_diag.stage = ST_OK; }
    if (a_hold && !base_safe && now > 1500u) a_hold = 0;
    a_nan_poll();
    op_diag.loops++;
    if (now - t_diag >= 100u) {          /* the audio's figures, for the next start's report */
        t_diag = now;
        op_diag.blocks = a_blocks; op_diag.load = a_load; op_diag.peak = a_peak; op_diag.nans = a_nan_count;
    }
}
