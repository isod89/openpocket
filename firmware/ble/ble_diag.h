/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The Bluetooth test image's log channel (DIAG in docs/EDITOR-PROTOCOL.md, node tools/op.mjs log): this boot and the boot
 * before it as text, so that the "LAST BOOT FAILED" report can be read from a computer as well as photographed. The
 * Bluetooth line itself is ble_test_line, the text of the status area. No hardware access, so tests/test_ble_status.c
 * compiles it on the PC. */
#pragma once
#include "ble_status.h"

/* the previous boot, kept before app_main clears op_diag for this one (the fields of its report; stage: ST_*) */
typedef struct { uint32_t failed, crashed, stage, blocks, load, peak, nans, c1, loops, vec, pc, rets, dbg, emu, uptime; } ble_boot_t;
static ble_boot_t ble_last;

/* "UP 12345 SAFE 0 STAGE 8 RST 40 GUARD 0" (out: 64 bytes): ms_now (it wraps every 179 s), safe mode, ST_*, the reset source bits, the failed boots counted */
static void ble_boot_text(char *out, uint32_t up, uint32_t safe, uint32_t stage, uint32_t rst, uint32_t guard_failed)
{
    char *o = out;
    o = ble__put(o, "UP "); o = ble__dec(o, up);
    o = ble__put(o, " SAFE "); o = ble__dec(o, safe);
    o = ble__put(o, " STAGE "); o = ble__dec(o, stage);
    o = ble__put(o, " RST "); o = ble__hex(o, rst);
    o = ble__put(o, " GUARD "); o = ble__dec(o, guard_failed);
    *o = 0;
}

/* "LAST OK", or "LAST FAILED STAGE 7 BLOCKS 9 LOAD 42 PEAK 61 NAN 0 CPU1 1 LOOPS 0", which a crash continues with
 * " CRASH VEC 3F PC 2000DC8 RETS 2001234 DBG 0 EMU 0 UP 4100" (out: 224 bytes, for the widest values) */
static void ble_last_text(char *out, const ble_boot_t *b)
{
    char *o = out;
    if (!b->failed) { ble__put(o, "LAST OK")[0] = 0; return; }
    o = ble__put(o, "LAST FAILED STAGE "); o = ble__dec(o, b->stage);
    o = ble__put(o, " BLOCKS "); o = ble__dec(o, b->blocks);
    o = ble__put(o, " LOAD "); o = ble__dec(o, b->load);
    o = ble__put(o, " PEAK "); o = ble__dec(o, b->peak);
    o = ble__put(o, " NAN "); o = ble__dec(o, b->nans);
    o = ble__put(o, " CPU1 "); o = ble__dec(o, b->c1);
    o = ble__put(o, " LOOPS "); o = ble__dec(o, b->loops);
    if (b->crashed) {
        o = ble__put(o, " CRASH VEC "); o = ble__hex(o, b->vec);
        o = ble__put(o, " PC "); o = ble__hex(o, b->pc);
        o = ble__put(o, " RETS "); o = ble__hex(o, b->rets);
        o = ble__put(o, " DBG "); o = ble__hex(o, b->dbg);
        o = ble__put(o, " EMU "); o = ble__hex(o, b->emu);
        o = ble__put(o, " UP "); o = ble__dec(o, b->uptime);
    }
    *o = 0;
}
