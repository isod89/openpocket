/* SPDX-License-Identifier: GPL-3.0-only */
/* Host stand-in for bsp/chip/fm1_time.h (tests/test_ble_taskq.c): TIMER4's count is a variable the test moves, so the scheduler's
 * clock (ble_os.c ble_ms) can be taken across the counter's wrap */
#pragma once
#include <stdint.h>
static uint32_t shim_t4;
#define FM1_T4_CNT shim_t4
#define FM1_TICKS_PER_US 24u
static inline uint32_t fm1_ticks(void) { return shim_t4; }
static inline void fm1_delay_us(uint32_t us) { (void)us; }
