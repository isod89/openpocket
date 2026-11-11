/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The memory map of the Bluetooth audio test image (OP_BT=1 sh firmware/build.sh, docs/BT-AUDIO-TEST.md): firmware/app.ld with
 *  - the flash window up to the package's app.bin slot (0x9D740 bytes; app.ld keeps 0x8DFBC, a figure from the first release):
 *    the classic stack is bigger than the LE one;
 *  - the classic stack's own sections: the A2DP host's handler tables (flash), the SDP records (RAM, the server links them),
 *    the baseband's table .bd_base and the radio's receive pool (in the SRAM below the RAM), its send pool, the controller's
 *    variables and the Bluetooth heap (first in the RAM: the baseband reaches 64 KB above .bd_base, see .bss);
 *  - .bss_lo2: 7.5 KB more of the SRAM below the RAM, for the decoded music.
 * The release image and the BLE image still link with firmware/app.ld. */
PROVIDE(op_guard_stack = 1);   /* build.sh passes --defsym op_guard_stack=0 with OP_GUARD_STACK=0 */
MEMORY
{
    XIP    (rx)  : ORIGIN = 0x02000120, LENGTH = 0x9D740   /* the package's whole app.bin slot (tools/pxpkg.py APP_SLOT) */
    RAM    (rwx) : ORIGIN = 0x01C08000, LENGTH = 96K
    POOL   (rw)  : ORIGIN = 0x01C20000, LENGTH = 0x3E000   /* free SRAM (like CLIPPER): DSP engine (v40: was 0x54000) */
    RAMTEXT (rwx): ORIGIN = 0x01C5E000, LENGTH = 0x16000   /* v40: the RAM code, audio path included (was 24 KB at 0x01C00000) */
    NOINIT (rw)  : ORIGIN = 0x01C7C000, LENGTH = 0x3D50
    LOWRAM (rw)  : ORIGIN = 0x01C00000, LENGTH = 0x6000    /* v40.1: v37's RAM code window, free since v40 (see .bss_lo) */
    LOWRAM2 (rw) : ORIGIN = 0x01C06000, LENGTH = 0x1E00    /* above it, below crt0's status words (0x01C07F00): see .bss_lo2 */
    CRAM   (rw)  : ORIGIN = 0x01F28000, LENGTH = 0x7000    /* PXC_BEGIN..PXC_END (px_cacheram.h) */
}
SECTIONS
{
    .text : { KEEP(*(.text.crt0)) *(EXCLUDE_FILE(*libjl.a:*) .text*) *(EXCLUDE_FILE(*libjl.a:*) .rodata*) _etext_fw = .;
              *libjl.a:*(.text* .rodata*) *(.*_code) *(.*_const)
              /* the host's handler tables, constant (the A2DP sink's codec and events, the stack's suspend hooks) */
              . = ALIGN(4); a2dp_sink_media_codec_begin = .; KEEP(*(.a2dp_sink_media_codec)) a2dp_sink_media_codec_end = .;
              . = ALIGN(4); a2dp_sink_media_probe_begin = .; KEEP(*(.a2dp_sink_media_probe)) a2dp_sink_media_probe_end = .;
              . = ALIGN(4); a2dp_source_media_codec_begin = .; KEEP(*(.a2dp_source_media_codec)) a2dp_source_media_codec_end = .;
              . = ALIGN(4); a2dp_event_handler_begin = .; KEEP(*(.a2dp_event_handler)) a2dp_event_handler_end = .;
              . = ALIGN(4); bt_sleep_begin = .; KEEP(*(.bt_sleep)) bt_sleep_end = .;
              . = ALIGN(4); KEEP(*(.lp_target)) KEEP(*(.tws_func_stub))
              . = ALIGN(4); _etext = .; } > XIP
    /* (_etext_fw: the end of the firmware's own flash code; the Bluetooth controller library's (libjl.a) comes after
     * it and never calls the RAM code: firmware/build.sh sends its memcpy / memset to flash copies) */
    .ram_text : { _rt_start = .; KEEP(*(.ram_text*))
                  . = ALIGN(4); _c1_start = .; KEEP(*(.c1_text*)) . = ALIGN(4); _c1_end = .;   /* the second core's loop */
                  _rt_end = .; } > RAMTEXT AT > XIP
    _rt_load = LOADADDR(.ram_text);
    /* v40.1: data in the RAM code window's free end, after the code (the PC guard covers only _rt_start.._rt_end), out of
     * the 96 KB RAM. Zeroed by fm1_cstart like .bss. v40.4 (app.ld) moved the engine's buffers marked PX_LO (px_dsp.c: the
     * IR's spectra, the oversampling's and the lanes' buffers, 14.6 KB) to .bss_lo; here they stay in this window, as in test
     * builds 12 and 13: .bss_lo holds the baseband's table and the radio's receive pool (this image runs px_nam.c's hot
     * routines from the flash instead, firmware/build.sh PXN_HOT_RAM). A wild store past them still meets _guard0 */
    .bss_hi (NOLOAD) : { . = ALIGN(8); _bss_hi_start = .; *(.bss.hi.*) *px_dsp.o(.bss.lo.*) . = ALIGN(4); _bss_hi_end = .; } > RAMTEXT
    /* v40.1, the Bluetooth test image only: data in the first 24 KB of the SRAM below the RAM, which v37's RAM code used
     * at run time and v40 leaves free (the boot ROM loads the boot loader there before the app runs). What goes there is
     * a buffer whose contents are checked before use (nm_stage: CRC, read-back). Zeroed by fm1_cstart like .bss. */
    /* the Bluetooth audio image: the classic baseband's table (DMA: zeroed at boot with the rest of .bss_lo) and the radio's
     * receive pool (link_bulk.c: the phone's packets wait there, 280 ms of SBC at 345 kb/s) */
    .bss_lo (NOLOAD) : { . = ALIGN(8); _bss_lo_start = .; *(EXCLUDE_FILE(*px_dsp.o) .bss.lo.*) . = ALIGN(8); *(.bd_base)
                         . = ALIGN(4); acl_rx_pool = .; . += 12288; acl_rx_pool_end = .;
                         . = ALIGN(4); _bss_lo_end = .; } > LOWRAM
    /* the decoded music (bt_a2dp.c bt_ring_mem): CPU only, set up by the code that uses it (not zeroed at boot). This 7.5 KB
     * was never used by OpenPocket at run time; if anything else did write there, the music would click, nothing more */
    .bss_lo2 (NOLOAD) : { . = ALIGN(8); _bss_lo2_start = .; *(.bss.lo2.*) . = ALIGN(4); _bss_lo2_end = .; } > LOWRAM2
    .data : { . = ALIGN(4); _data_start = .; *(.data*) *(.sdata*) *(.*_data)
              . = ALIGN(4); sdp_record_item_begin = .; KEEP(*(.sdp_record_item)) sdp_record_item_end = .;   /* (the SDP server links them) */
              . = ALIGN(4); _data_end = .; } > RAM AT > XIP
    _data_load = LOADADDR(.data);
    /* The classic baseband addresses memory as 16-bit offsets from its base register 0x2FC44, which bredr_bd_init sets to
     * bd_base: every buffer the library hands it must lie less than 64 KB above bd_base. The library asserts it
     * (__write_reg_txericntl and its siblings) and a failed assertion stops in a loop: the first test build's stall, with the
     * send pool at the end of the RAM, 118 KB above bd_base. So, first in the RAM, as the SDK's btctler_lib_bss.ld keeps them
     * next to bd_base: the send pool (bt_malloc's page and inquiry scan buffers come from it too: a sink sends little), the
     * controller's own variables, and the Bluetooth heap (ble_os.c). The check below the sections holds the line. */
    .bss (NOLOAD) : { _bss_start = .;
                      . = ALIGN(4); acl_tx_pool = .; . += 2048; acl_tx_pool_end = .;
                      *(.bt_rf_bss) *(.hci_controller_bss) *(.hci_interface_bss) *(.device_manager_bss) *(.vendor_manager_bss)
                      *(.ble_hci_bss) *(.ble_ll_bss) *(.ble_rf_bss) *(.classic_*_bss)
                      . = ALIGN(8); *(.bss.bt_heap)
                      _bt_window_end = .;
                      *(.bss*) *(.sbss*) *(.*_bss) *(COMMON)
                      . = ALIGN(4); _bss_end = .; } > RAM
    ASSERT(acl_rx_pool >= bd_base && acl_tx_pool >= bd_base && acl_rx_pool_end - bd_base < 0xFFFF
           && _bt_window_end - bd_base < 0xFFFF && _data_end - bd_base < 0xFFFF,
           "the classic baseband's memory (its pools, the controller's variables, the Bluetooth heap) must lie within 64 KB above bd_base")
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
    /DISCARD/ : { *(.comment) *(.note*) }
}
