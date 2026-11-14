/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the start. fm1_cstart, the first C code of an app: it paints the stacks,
 * turns the long-press reset off, arms the watchdog, counts the starts that did not finish (two in a row: the chip's
 * UBOOT, from which the pedal can always be re-flashed), sets up the sections and the hardware guard, and calls
 * app_main, which the app's file defines after this one.
 */
#pragma once
#ifndef OP_GUARD_STACK      /* hardware stack-limit guard (fm1_guard.h): the app's file or build.sh may turn it off */
#define OP_GUARD_STACK 1
#endif

static void app_main(void);
void fm1_cstart(void)
{
    uint32_t *s, *d;
    uint32_t vdc0, sys0, dcdc, rst0, pinr0;
    {   /* v40.1: paint the stacks (interrupts are still off), for their deepest use (ed_stat) */
        volatile uint32_t here;
        for (d = (uint32_t *)(void *)_ustack_lo; (char *)d < (char *)&here - 256; d++) *d = OP_PAINT;
        for (d = (uint32_t *)(void *)_sstack_lo; (char *)d < _sstack_top - 256; d++) *d = OP_PAINT;
    }
    fm1_time_init();
    /* v31, issue #1: the stock firmware's first act is P3_PINR_CON = 0, the long-press reset off (BlackBox_020
     * f_0200698a). Left on, a pin held at its active level resets the chip every 4 s (the SDK offers 4 or 8 s):
     * Megal's BlackBox reset every 4.3 s after a power cycle, never after an install (the always-on P33 kept the
     * stock firmware's 0 across the warm reset). Read first, so the boot screen can show it. */
    rst0 = fm1_p33_read(FM1_P3_RST_SRC);
    pinr0 = fm1_p33_read(0x94u);
    fm1_p33_write(0x94u, 0);
    fm1_wdt_arm(0x0D);
    /* v33: the supplies stay as the boot loader leaves them (VDC14 3, SYSVDD 11, LDO), as up to v29. v30 set them as
     * the stock firmware does (VDC14 7, SYSVDD 15, the 1.3 V supply on the DC-DC): it was not what fixed issue #1
     * (the long-press reset was), and with it a Pocket X ran its DSP ~25 % slower (DOOM SLUDGE 61 % -> 77 %) and
     * crackled. Only read, for the boot screen. */
    vdc0 = fm1_vdc14();
    sys0 = fm1_sysvdd();
    dcdc = 0;
    fm1_wdt_feed();
    if (guard.magic != 0x48575431u || guard.pending > 1u || guard.failed > 1u) {
        guard.magic = 0x48575431u; guard.failed = 0; guard.pending = 0;
    }
    if (guard.pending) guard.failed++;
    guard.pending = 1;
    if (guard.failed >= 2u) { guard.failed = 0; guard.pending = 0; fm1_enter_uboot(); }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++) *d = 0;
    for (d = _bss_hi_start; d < _bss_hi_end; d++) *d = 0;
    for (d = _pool_tail_start; d < _pool_tail_end; d++) *d = 0;
    for (d = _bss_lo_start; d < _bss_lo_end; d++) *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++) *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++) *d = *s;
    op_pwr.vdc0 = (uint8_t)vdc0; op_pwr.vdc1 = (uint8_t)fm1_vdc14();
    op_pwr.sys0 = (uint8_t)sys0; op_pwr.sys1 = (uint8_t)fm1_sysvdd(); op_pwr.dcdc = (uint8_t)dcdc;
    op_pwr.rst = (uint8_t)rst0; op_pwr.pinr = (uint8_t)pinr0;
    fm1_guard_enable((OP_GUARD_STACK ? FM1_GUARD_STACK : 0) | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    app_main();
    for (;;) ;
}
