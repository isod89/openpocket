/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Memory map of a standalone FM-1 app: code and constants run in place from XIP
 * (app.bin offset 0 = 0x02000120); .data is copied to RAM by fm1_cstart, and so is .ram_text (code that runs
 * while the flash is busy: the flash driver) into RAMTEXT, the guard's RAM code window.
 * The ble build (OP_BLE=1) adds the JieLi controller's own sections: .*_code and .*_const (XIP), .*_data, .*_bss (RAM).
 * Stacks sit above the RAM with 256-byte guard bands; NOINIT survives resets.
 * 0x01C7FD50.. is reserved (boot info, mailbox, vector table). */
PROVIDE(op_guard_stack = 1);   /* build.sh passes --defsym op_guard_stack=0 with OP_GUARD_STACK=0 */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x8DFBC
    RAMTEXT (rwx): ORIGIN = 0x01C00000, LENGTH = 0x6000
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K
    POOL   (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x54000   /* free SRAM (like CLIPPER): DSP engine */
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
}
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(.text*) *(.rodata*) *(.*_code) *(.*_const) . = ALIGN(4); _etext = .; } > XIP
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*))
                  . = ALIGN(4); _c1_start = .; KEEP(*(.c1_text*)) . = ALIGN(4); _c1_end = .;   /* the second core's loop */
                  _rt_end = .; } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) *(.*_data) . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    .bss (NOLOAD) : { _bss_start = .; _ble_stack_lo = .; *(.bss.ble_stack) _ble_stack_hi = .; *(.bss*) *(.sbss*) *(.*_bss) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
    .noinit (NOLOAD) : { *(.noinit*) } > NOINIT
    .pool (NOLOAD) : { . = ALIGN(8); *(.pool*) } > POOL
    _guard0 = 0x01C74000; _ustack_lo = 0x01C74100; _ustack_top = 0x01C7A000;
    _guard1 = 0x01C7A000; _sstack_lo = 0x01C7A100; _sstack_top = 0x01C7C000;
    /* The hardware stack guard (fm1_guard.h) faults any sp outside [_ustack_lo, _sstack_top]. The Bluetooth test image's task
     * stacks (ble_os.c) are in RAM below that window, so it must be linked with op_guard_stack=0 (build.sh); with the guard on
     * this link fails instead of the pedal crash-looping. Empty in the normal firmware. */
    ASSERT(op_guard_stack == 0 || _ble_stack_lo == _ble_stack_hi || (_ble_stack_lo >= _ustack_lo && _ble_stack_hi <= _sstack_top),
           "BLE task stacks lie outside the stack-guard window while the stack guard is enabled")
    /DISCARD/ : { *(.comment) *(.note*) }
}
