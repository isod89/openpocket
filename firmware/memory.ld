/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* OpenPocket's own memory regions (firmware/app.ld includes this file; docs/BASE-FIRMWARE.md): the 432 KiB of SRAM
 * from 0x01C08000 up to the stacks, split for the amp modeller. */
MEMORY
{
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K
    POOL   (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x3E000   /* free SRAM (like CLIPPER): DSP engine (v40: was 0x54000) */
    RAMTEXT (rwx): ORIGIN = 0x01C5E000, LENGTH = 0x16000   /* v40: the RAM code, audio path included (was 24 KB at 0x01C00000) */
}
