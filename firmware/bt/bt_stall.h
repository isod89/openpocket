/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The Bluetooth audio image's stall record (docs/BT-AUDIO-TEST.md, "If it stalls"). The stall watch (ble/ble_status.h) ends a
 * ble_poll that has not returned in 4 s with the crash screen, but the PC that screen shows is whatever the 10 kHz tick
 * interrupted, which with sound running is mostly the audio interrupt, not the stuck task. So, for the last second before it
 * fires, the tick samples the interrupted task's own PC and RETS (bt_a2dp.c bt_stall_tick), and when it fires the record also
 * takes the task's name, the start-up stage, the heap, the last calls the library made into the firmware (the crumbs below),
 * and the classic baseband's clock registers. The record is in NOINIT: the safe-mode boot that follows shows it on the log's
 * lines (bt_a2dp.c), until Bluetooth starts again.
 *
 * Included by ble/ble_os.c (the crumbs are dropped there too), before the rest of the image. */
#pragma once
#include <stdint.h>

#define BT_STALL_MAGIC 0x42545354u                  /* "BTST" */
#define BT_STALL_N 6u                                /* distinct (PC, RETS) pairs kept */
#define BT_STALL_CRUMBS 8u
typedef struct {
    uint32_t magic;                                  /* set when the watch fires (the samples are complete) */
    uint32_t samples, other;                         /* ticks sampled; samples that found no free slot */
    uint32_t pc[BT_STALL_N], rets[BT_STALL_N], cnt[BT_STALL_N];
    uint32_t dmin, dmax;                             /* the interrupt stack's depth at the samples, in bytes */
    uint32_t stage, heap_min, heap_free, mfail, mfail_size, irq40, irq41, ev, ev_last[4], ev_i, stub;
    const char *stub_last;
    const char *crumb[BT_STALL_CRUMBS];
    uint32_t crumb_i;
    char task[12];
    uint32_t regs_ok;                                /* 0: a register read did not come back (the watchdog reset first) */
    uint32_t reg[11];                                /* bt_a2dp.c bt_stall_fire says which */
} bt_stall_t;
bt_stall_t bt_stall __attribute__((section(".noinit")));   /* (not static: the compiler must not assume it starts at zero) */

/* the crumbs: the last calls the library made into the firmware's own functions (names, pointers to constant strings: valid
 * across a reset of the same image) */
static const char *bt_crumb[BT_STALL_CRUMBS];
static uint32_t bt_crumb_i;
static void bt_crumb_add(const char *s)                  /* (a call repeated back to back is one crumb: it would push the others out) */
{
    if (bt_crumb_i && bt_crumb[(bt_crumb_i - 1u) % BT_STALL_CRUMBS] == s) return;
    bt_crumb[bt_crumb_i++ % BT_STALL_CRUMBS] = s;
}
#define BT_CRUMB(s) bt_crumb_add(s)

/* the heap's failures (ble/ble_os.c malloc): the library's own reaction to one is to stop in a loop (P33_SYSTEM_RESET) */
static uint32_t bt_heap_fails, bt_heap_fail_size;
/* the tasks' message queues (ble/ble_os.c os_taskq_post_type): messages lost (a full queue, or no such task) and the deepest
 * a queue has been (of 16) */
static uint32_t bt_q_lost, bt_q_peak;
