/* SPDX-License-Identifier: GPL-3.0-only */
/* Host stand-in for bsp/chip/fm1_irq.h, for the tests that compile firmware/ble/ble_os.c on a PC (tests/test_ble_taskq.c) */
#pragma once
#include <stdint.h>
static uint32_t shim_icfg[32];
#define FM1_ICFG(n) (shim_icfg[(n) >> 3])
static inline void fm1_irq_off(void) { }
static inline void fm1_irq_on(void) { }
static inline uint32_t fm1_icfg(void) { return 0x200u; }
static inline uint32_t fm1_uptime_ms(void) { return 0; }
static inline void fm1_wdt_feed(void) { }
static void fm1_irq_attach(uint32_t n, void (*h)(void), uint32_t prio) { (void)n; (void)h; (void)prio; }
