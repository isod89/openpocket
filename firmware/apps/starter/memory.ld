/* SPDX-License-Identifier: GPL-3.0-only */
/* STARTER's own memory regions (firmware/app.ld includes this file; docs/BASE-FIRMWARE.md): the 432 KiB of SRAM from
 * 0x01C08000 up to the stacks. STARTER needs little of it (its engine's state is 8 KiB of the POOL); the split leaves
 * the most to an engine's memory, for the app that starts from this one. */
MEMORY
{
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 64K       /* .data, .bss */
    POOL   (rw)  : ORIGIN = 0x01C18000, LENGTH = 0x50000   /* 320 KiB for the engine (section .pool) */
    RAMTEXT (rwx): ORIGIN = 0x01C68000, LENGTH = 0xC000    /* 48 KiB of RAM code, up to the stacks */
}
