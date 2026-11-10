/* SPDX-License-Identifier: GPL-3.0-only */
/* Host stand-in for bsp/chip/fm1_time.h (tests/test_ble_taskq.c) */
#pragma once
#include <stdint.h>
static inline void fm1_delay_us(uint32_t us) { (void)us; }
