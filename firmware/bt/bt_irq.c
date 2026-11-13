/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The classic baseband's two interrupt handlers, as plain C bodies behind OpenPocket's own interrupt entry (bt/bt_isr.S, the
 * pattern of bsp/chip/fm1_isr.S: psr, rets, reti and r0-r3 saved first, csync before rti).
 *
 * Why not the library's own: its two handlers (bredr_frame.c bredr_rx_irq_handler, bredr_slot_timer.c slot_timer_irq_handler)
 * are C functions with the "interrupt" attribute, and this toolchain's clang ends those without the csync stock's handlers have
 * before rti (fm1_isr.S: the reason OpenPocket writes no interrupt handler in C). Below the audio interrupt (test build 3) they
 * only ever interrupted the main loop; above it (test build 4, for their slot deadlines) they interrupt the DSP, and the
 * reverb faulted on an invalid address 0.5 s after the DSP started (VEC 1, DBG bit 21, PC in rv_block).
 *
 * The two handlers are dispatchers, followed here step for step from the library's code (read from its LLVM IR, the AC79 SDK's
 * btctrler.a, Apache-2.0): the work stays in the library's own functions, which are ordinary ones (bredr_link_rx_irq_handler,
 * bredr_link_timeout_irq_handler, link_conn_super_timeout, and the slot timers' callbacks). The offsets are those of the
 * library's types (struct bredr_table = bd_base, 4,196 bytes; struct bredr_link_var = bredr_link_v, 104 bytes), the same as
 * bsp/chip/fm1_bredr.h's independent reading (frame n at 368 + 180 n, rx_clkn at 52 and 54 in the frame).
 *
 * Since test build 11 each handler also counts itself in bt_isr_depth (ble/ble_os.c: the library's cpu_in_irq(), so nothing it
 * runs here waits) and times itself (the log's isr line, bt_a2dp.c: the longest, and the time a second, against the audio's) */
#include <stdint.h>
#include "fm1_time.h"

extern uint8_t bd_base[];                                       /* struct bredr_table (bredr_frame.c, .bd_base) */
extern uint8_t bredr_link_v[];                                  /* struct bredr_link_var (bredr_link.c) */
extern void bredr_link_rx_irq_handler(void *frame);
extern void bredr_link_timeout_irq_handler(void);
extern void link_conn_super_timeout(void *link);
extern volatile uint32_t bt_cnt_irq_br, bt_cnt_irq_slot;        /* bt_a2dp.c (the log's IRQ 40 and 41) */
extern volatile uint32_t bt_isr_depth;                          /* ble/ble_os.c */
/* (bt_isr_t_br, bt_isr_t_slot, bt_isr_took: bt/bt_stall.h) */

#define BT_BR_IPND (*(volatile uint32_t *)0x20010u)             /* IRQ 40's pending bits */
#define BT_BR_IACK (*(volatile uint32_t *)0x20018u)             /* acknowledged by setting them (the library ORs them in) */
#define BT_STIM    (*(volatile uint32_t *)0x2FD40u)             /* slot timers: [7:0] enable, [15:8] acknowledge, [23:16] pending */

#define BT_TAB_FRAME(n) (bd_base + 368u + 180u * (n))           /* bredr_table field 3: struct ctrl_frame[4] */
#define BT_TAB_RXCLK(n) ((uint32_t *)(void *)(bd_base + 3916u + 4u * (n)))   /* field 9: the rx_clkn last seen, per frame */
#define BT_TAB_FRAMES   (bd_base[3934])                         /* field 12: the frames in use, one bit each */
#define BT_LV_TIMER(k)  ((uint32_t *)(void *)(bredr_link_v + 32u + 8u * (k)))   /* field 4: {priv, callback | 1 = one-shot} */
#define BT_LV_ONESHOT   (bredr_link_v[96])                      /* field 5 */

/* IRQ 40 (bredr_rx_irq_handler): acknowledge; a frame whose receive clock moved has a packet (the link layer's handler); the
 * alarm (bit 9) runs the link layer's timeout handler */
void bt_br_irq(void)
{
    const uint32_t t0 = fm1_ticks();
    uint32_t n;
    bt_isr_depth++;
    bt_cnt_irq_br++;
    if (BT_BR_IPND & 1u) BT_BR_IACK |= 1u;
    if (BT_BR_IPND & 2u) {
        BT_BR_IACK |= 2u;
        for (n = 0; n < 4u; n++) {
            uint8_t *f = BT_TAB_FRAME(n);
            uint32_t clk;
            if (!(BT_TAB_FRAMES & (1u << n))) continue;
            clk = (uint32_t)*(volatile uint16_t *)(void *)(f + 54) << 16 | *(volatile uint16_t *)(void *)(f + 52);
            if (clk == *BT_TAB_RXCLK(n)) continue;
            *BT_TAB_RXCLK(n) = clk;
            bredr_link_rx_irq_handler(f);
        }
    }
    if (BT_BR_IPND & 1024u) BT_BR_IACK |= 1024u;
    if (BT_BR_IPND & 512u) { BT_BR_IACK |= 512u; bredr_link_timeout_irq_handler(); }
    if (BT_BR_IPND & 2048u) BT_BR_IACK |= 2048u;
    if (BT_BR_IPND & 4u) BT_BR_IACK |= 4u;
    bt_isr_depth--;
    bt_isr_took(&bt_isr_t_br, fm1_ticks() - t0);
}

/* IRQ 41 (slot_timer_irq_handler): the link supervision check, then each slot timer that is pending: acknowledged; if it was
 * enabled, disabled and its callback run (a one-shot one also leaves the one-shot set) */
void bt_slot_irq(void)
{
    const uint32_t t0 = fm1_ticks();
    uint32_t k;
    bt_isr_depth++;
    bt_cnt_irq_slot++;
    link_conn_super_timeout(0);
    for (k = 0; k < 8u; k++) {
        uint32_t *t, fn;
        if (!(BT_STIM & (1u << (16u + k)))) continue;
        BT_STIM |= 1u << (8u + k);
        if (!(BT_STIM & (1u << k))) continue;
        BT_STIM &= ~(1u << k);
        t = BT_LV_TIMER(k);
        fn = t[1];
        if (!fn) continue;
        ((void (*)(void *))(uintptr_t)(fn & ~1u))((void *)(uintptr_t)t[0]);
        if (t[1] & 1u) BT_LV_ONESHOT = (uint8_t)(BT_LV_ONESHOT & ~(1u << k));   /* (read again: the callback may have re-armed it) */
    }
    bt_isr_depth--;
    bt_isr_took(&bt_isr_t_slot, fm1_ticks() - t0);
}
