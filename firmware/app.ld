/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Memory map of a standalone FM-1 app: code and constants run in place from XIP
 * (app.bin offset 0 = 0x02000120); .data is copied to RAM by fm1_cstart, and so is .ram_text (code that runs
 * while the flash is busy: the flash driver) into RAMTEXT, the guard's RAM code window.
 * v40: the audio path runs from RAM too, as the stock firmware's does (tools/ramcode.py moves its functions into
 * .ram_text.dsp.*). RAMTEXT sits at the top of the free SRAM, high enough to be inside the call instruction's
 * +-4 MiB of all the flash code (0x02000120..0x0205E000): the flash and the audio path call each other directly.
 * The delay line at half rate (v40) left the room for it in the old POOL; 0x01C00000..0x01C08000 is free.
 * The ble build (OP_BLE=1) adds the JieLi controller's own sections: .*_code and .*_const (XIP), .*_data, .*_bss (RAM).
 * Stacks sit above the RAM with 256-byte guard bands; NOINIT survives resets.
 * 0x01C7FD50.. is reserved (boot info, mailbox, vector table). */
PROVIDE(op_guard_stack = 1);   /* build.sh passes --defsym op_guard_stack=0 with OP_GUARD_STACK=0 */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x8DFBC
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
    LOWRAM (rw)  : ORIGIN = 0x01C00000, LENGTH = 0x6000    /* v40.1: v37's RAM code window, free since v40 (see .bss_lo) */
    CRAM   (rw)  : ORIGIN = 0x01F28000, LENGTH = 0x7000    /* PXC_BEGIN..PXC_END (px_cacheram.h) */
}
/* the app's own regions, between LOWRAM and the stacks: RAM (.data, .bss), POOL (its engine's memory), RAMTEXT (the RAM
 * code). firmware/memory.ld holds OpenPocket's, firmware/apps/<name>/memory.ld another app's (build.sh: -L) */
INCLUDE memory.ld
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(EXCLUDE_FILE(*libjl.a:*) .text*) *(EXCLUDE_FILE(*libjl.a:*) .rodata*) _etext_fw = .;
              *libjl.a:*(.text* .rodata*) *(.*_code) *(.*_const) . = ALIGN(4); _etext = .; } > XIP
    /* (_etext_fw: the end of the firmware's own flash code; the Bluetooth controller library's (libjl.a) comes after
     * it and never calls the RAM code: firmware/build.sh sends its memcpy / memset to flash copies) */
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*))
                  . = ALIGN(4); _c1_start = .; KEEP(*(.c1_text*)) . = ALIGN(4); _c1_end = .;   /* the second core's loop */
                  _rt_end = .; } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    /* v40.1: data in the RAM code window's free end, after the code (the PC guard covers only _rt_start.._rt_end), out of
     * the 96 KB RAM. Zeroed by fm1_cstart like .bss. (v40.4: the engine's buffers moved to .bss_lo; the code needs the room.) */
    .bss_hi (NOLOAD) : { . = ALIGN(8); _bss_hi_start = .; *(.bss.hi.*) . = ALIGN(4); _bss_hi_end = .; } > RAMTEXT
    /* v40.1: data in the first 24 KB of the SRAM below the RAM, which v37's RAM code used at run time and v40 leaves free
     * (the boot ROM loads the boot loader there before the app runs). Zeroed by fm1_cstart like .bss. The Bluetooth test
     * image's upload staging buffer (nm_stage: checked before use by CRC and read-back); v40.4: the engine's buffers marked
     * PX_LO (px_dsp.c: the IR's spectra, the oversampling's and the lanes' buffers, 13.8 KB), which were in .bss_hi. */
    .bss_lo (NOLOAD) : { . = ALIGN(8); _bss_lo_start = .; *(.bss.lo.*) . = ALIGN(4); _bss_lo_end = .; } > LOWRAM
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) *(.*_data) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; *(.bss*) *(.sbss*) *(.*_bss) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .noinit (NOLOAD) : { *(.noinit) *(.noinit.ble) . = ALIGN(8); _ble_stack_lo = .; *(.noinit.ble_stack) _ble_stack_hi = .; *(.noinit*) } > NOINIT
    .cram (NOLOAD) : { _cram_lo = .; *(.cram*) . = ALIGN(8); _cram_hi = .; } > CRAM   /* CPU-only data, zeroed by pxc_init */
    .pool (NOLOAD) : { . = ALIGN(8); *(.pool) . = ALIGN(8); _pool_tail_start = .; *(.pool.tail.*) . = ALIGN(4); _pool_tail_end = .; } > POOL
    /* (.pool.tail.*: engine buffers marked PX_POOL_TAIL in the pool's free end, zeroed by fm1_cstart like .bss) */
    _guard0 = 0x01C74000; _ustack_lo = 0x01C74100; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /* The hardware stack guard (fm1_guard.h) faults any sp outside [_ustack_lo, _sstack_top]. The Bluetooth test image's task
     * stacks (ble_os.c) are in NOINIT above that window, so it must be linked with op_guard_stack=0 (build.sh); with the guard on
     * this link fails instead of the pedal crash-looping. Empty in the normal firmware. */
    ASSERT(op_guard_stack == 0 || _ble_stack_lo == _ble_stack_hi || (_ble_stack_lo >= _ustack_lo && _ble_stack_hi <= _sstack_top),
           "BLE task stacks lie outside the stack-guard window while the stack guard is enabled")
    /* v40: the flash code and the RAM code must stay within the call instruction's reach of each other (+-4 MiB: the
     * linker would refuse a call out of it, naming one) */
    ASSERT(_etext_fw - ORIGIN(RAMTEXT) < 0x400000 && 0x02000120 - (ORIGIN(RAMTEXT) + LENGTH(RAMTEXT)) < 0x400000,
           "the flash code has grown out of the RAM code's call range: move RAMTEXT up (firmware/app.ld)")
    ASSERT(ORIGIN(RAM) >= ORIGIN(LOWRAM) + LENGTH(LOWRAM) && ORIGIN(RAM) + LENGTH(RAM) <= ORIGIN(POOL) &&
           ORIGIN(POOL) + LENGTH(POOL) <= ORIGIN(RAMTEXT) && ORIGIN(RAMTEXT) + LENGTH(RAMTEXT) <= _guard0,
           "the app's RAM, POOL and RAMTEXT (memory.ld) must lie in that order between LOWRAM and the stacks")
    /DISCARD/ : { *(.comment) *(.note*) }
}
