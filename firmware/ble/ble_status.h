/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* What the Bluetooth test image shows on the screen (docs/BLE-TEST.md, "Reading the status text"): where start-up has got
 * to, and, when it failed, which step and which return value. No hardware access, so tests/test_ble_status.c compiles it
 * on the PC.
 *
 *   STARTING <stage>      the last stage reached (BLE_STAGE_*): a timeout names the stage it stalled in
 *   FAILED <step> <code>  the step that failed (BLE_FAIL_*) and its return value; for BLE_FAIL_TIMEOUT the code is the stage
 *   ADVERTISING           the controller accepted the command to advertise (not proof of a packet: see the proof counters below)
 * and the marquee that scrolls the full failure line across the small status area. */
#pragma once
#include <stdint.h>
#include <string.h>

enum { BLE_FAIL_NONE, BLE_FAIL_TASK_CREATE, BLE_FAIL_INIT, BLE_FAIL_READY, BLE_FAIL_ADV_ENABLE, BLE_FAIL_TIMEOUT };
enum { BLE_STAGE_IDLE, BLE_STAGE_TASK, BLE_STAGE_INIT, BLE_STAGE_RF, BLE_STAGE_READY, BLE_STAGE_ADV, BLE_STAGE_ENABLE,
       BLE_STAGE_RF_DONE, BLE_STAGE_BB };
enum { BLE_OFF, BLE_STARTING, BLE_ADV, BLE_FAILED };

static volatile uint32_t ble_state;
static volatile uint32_t ble_stage;
static volatile uint32_t ble_fail_step;
static volatile int32_t ble_fail_code;
static volatile uint32_t ble_wait;                 /* at a timeout: what the controller task is blocked on (ble_task_waiting) */

static void ble_stage_set(uint32_t stage) { ble_stage = stage; }

/* The stall watch. The scheduler is cooperative, so library code that never returns (a failed assertion parks in
 * P33_SYSTEM_RESET, a wait on a register bit) holds the main loop until the watchdog resets, and that leaves no trace.
 * The 10 kHz tick (px_usb.h) therefore counts while a ble_poll runs; one that has not returned after BLE_STALL_TICKS
 * makes the next tick the crash screen, whose PC and RETS name the code (VEC 63). 0: no poll running, or held off. */
enum { BLE_STALL_TICKS = 40000 };                  /* 4 s: before the watchdog (8 s) */
static volatile uint32_t ble_busy_ticks;
static void ble_busy(int on) { ble_busy_ticks = on ? 1u : 0u; }
static int ble_stall_tick(void) { return ble_busy_ticks && ++ble_busy_ticks > BLE_STALL_TICKS; }

static void ble_fail(uint32_t step, int32_t code)
{
    ble_fail_step = step;
    ble_fail_code = code;
    ble_state = BLE_FAILED;
}

/* the results of btctrler_task_init and btctrler_task_ready (1 when the controller task answered) -> the failing step, or 0 */
static uint32_t ble_judge_start(int init_rc, int ready_rc)
{
    if (init_rc) { ble_fail(BLE_FAIL_INIT, init_rc); return BLE_FAIL_INIT; }
    if (!ready_rc) { ble_fail(BLE_FAIL_READY, ready_rc); return BLE_FAIL_READY; }
    return 0;
}

static char *ble__put(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

static char *ble__num(char *o, int32_t v)
{
    char t[12]; int n = 0;
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    if (v < 0) *o++ = '-';
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *o++ = t[--n];
    return o;
}

/* the status text into out (at least 24 bytes) */
static void ble_status_text(char *out)
{
    char *o = out;
    if (ble_state == BLE_ADV) o = ble__put(o, "ADVERTISING");
    else if (ble_state == BLE_STARTING) { o = ble__put(o, "STARTING "); o = ble__num(o, (int32_t)ble_stage); }
    else if (ble_state == BLE_FAILED) {
        o = ble__put(o, "FAILED "); o = ble__num(o, (int32_t)ble_fail_step);
        *o++ = ' '; o = ble__num(o, ble_fail_code);
    } else o = ble__put(o, "OFF");
    *o = 0;
}

/* The full failure line, for the marquee: the short code first, then the step and the stage in words (out: 96 bytes) */
static void ble_status_full(char *out)
{
    static const char *const step[] = { "none", "task_create", "btctrler_task_init", "btctrler_task_ready", "ll_hci_adv_enable", "timeout" };
    static const char *const stage[] = { "idle", "task", "init", "radio-init", "ready-wait", "adv-setup", "enable", "radio-done", "baseband-irq" };
    char *o;
    ble_status_text(out);
    if (ble_state != BLE_FAILED) return;
    o = out + strlen(out);
    o = ble__put(o, "  step ");
    o = ble__put(o, ble_fail_step < 6 ? step[ble_fail_step] : "?");
    o = ble__put(o, " stage ");
    o = ble__put(o, ble_stage < 9 ? stage[ble_stage] : "?");
    if (ble_wait) {                                              /* look the address up in firmware/build-ble/symbols.txt */
        int i;
        o = ble__put(o, " wait ");
        for (i = 28; i >= 0; i -= 4) *o++ = "0123456789ABCDEF"[(ble_wait >> i) & 15u];
    }
    *o = 0;
}

/* A marquee: the text, then a gap, looping; tick counts the 500 ms redraws. It rests for BLE_MARQUEE_PAUSE ticks on the
 * start, then moves one character per tick (2 a second). win characters go to out, which gets a terminating 0. */
enum { BLE_MARQUEE_PAUSE = 4, BLE_MARQUEE_GAP = 3 };
static void ble_marquee(const char *text, uint32_t tick, int win, char *out)
{
    uint32_t len = (uint32_t)strlen(text), period = len + BLE_MARQUEE_GAP, phase, off;
    int i;
    if (len <= (uint32_t)win) { memcpy(out, text, len + 1u); return; }      /* it fits: no scroll */
    phase = tick % (BLE_MARQUEE_PAUSE + period);
    off = phase < BLE_MARQUEE_PAUSE ? 0u : phase - BLE_MARQUEE_PAUSE;
    for (i = 0; i < win; i++) {
        uint32_t k = (off + (uint32_t)i) % period;
        out[i] = k < len ? text[k] : ' ';
    }
    out[win] = 0;
}

/* ---- proof counters: does the radio run at all? Shown next to ADVERTISING (docs/BLE-TEST.md, "Reading the status text").
 * ble_isr.S counts the two baseband interrupts as they arrive; the others are counted where the library calls us. */
uint32_t ble_cnt_irq_ev;                  /* IRQ 45: baseband event (an advertising event, a transmit done) */
uint32_t ble_cnt_irq_rx;                  /* IRQ 29: a packet received (a scanner's request) */
static volatile uint32_t ble_cnt_timer;   /* the library's software timers that fired */
static volatile uint32_t ble_cnt_hci;     /* HCI packets the controller returned (hci_packet_handler) */
static volatile uint32_t ble_cnt_slot;    /* calls of the slot-timer stand-ins: the link scheduler wanting a timer we do not have */
static volatile uint32_t ble_hci_last;    /* type << 8 | event code of the last HCI packet */

static int ble_count_hci(uint8_t type, const uint8_t *pkt)
{
    ble_cnt_hci++;
    ble_hci_last = ((uint32_t)type << 8) | (pkt ? pkt[0] : 0u);
    return 0;
}

static char *ble__dec(char *o, uint32_t v) { return ble__num(o, (int32_t)(v > 0x7FFFFFFFu ? 0x7FFFFFFF : v)); }
static char *ble__hex(char *o, uint32_t v)
{
    int i = 28;
    while (i > 0 && !((v >> i) & 15u)) i -= 4;
    for (; i >= 0; i -= 4) *o++ = "0123456789ABCDEF"[(v >> i) & 15u];
    return o;
}

/* the link line (out: 128 bytes): "LINK 1 n2 A A1B2C3D4E5F6 I48 L0 T400 W7 P1 D0 LAST 2 24 127 STUB 3 lmp_hci_reset". LINK 1 while connected; n links so far;
 * A the peer's address; I interval (1.25 ms), L latency, T timeout (10 ms) of the link; W writes to the MIDI characteristic; P Just Works pairings accepted; D the
 * reason of the last disconnection; LAST the last message (PXM_PC or PXM_CC, data 1, data 2); STUB the calls of the stand-ins and the last one's name */
static void ble_link_text(char *out, uint32_t up, uint32_t links, const uint8_t *peer, uint32_t ivl, uint32_t lat, uint32_t tmo, uint32_t writes, uint32_t pairs,
                          uint32_t reason, const uint8_t *last, uint32_t stubs, const char *stub)
{
    char *o = out;
    o = ble__put(o, "LINK "); o = ble__dec(o, up);
    o = ble__put(o, " n"); o = ble__dec(o, links);
    o = ble__put(o, " A ");
    for (int i = 5; i >= 0; i--) { *o++ = "0123456789ABCDEF"[peer[i] >> 4]; *o++ = "0123456789ABCDEF"[peer[i] & 15u]; }
    o = ble__put(o, " I"); o = ble__dec(o, ivl);
    o = ble__put(o, " L"); o = ble__dec(o, lat);
    o = ble__put(o, " T"); o = ble__dec(o, tmo);
    o = ble__put(o, " W"); o = ble__dec(o, writes);
    o = ble__put(o, " P"); o = ble__dec(o, pairs);
    o = ble__put(o, " D"); o = ble__hex(o, reason);
    o = ble__put(o, " LAST "); o = ble__dec(o, last[0]);
    o = ble__put(o, " "); o = ble__dec(o, last[1]);
    o = ble__put(o, " "); o = ble__dec(o, last[2]);
    o = ble__put(o, " STUB "); o = ble__dec(o, stubs);
    if (stub) { o = ble__put(o, " "); o = ble__put(o, stub); }
    *o = 0;
}

/* the advertising line, the numbers first so that they are on the screen before the marquee moves:
 * "ADV i12 e5 r0 t340 s0 V 7 A3 12AB CON 2001 IEN 101 IPND 0 STAT 80 IC45 3 IC29 3 BEN 1" (out: 96 bytes; the registers are read by the caller)\n *   V  the stored radio records: bit mask of VM 106, 107, 108 found, the active VM area (flash address / 4096, 0 = none), the first two bytes of VM 106
 *   i interrupts 45   e HCI packets returned   r interrupts 29   t library timers   s slot-timer stand-in calls
 *   CON/IEN/IPND/STAT baseband 0x28000/28/30/38   IC45, IC29 the interrupt configuration nibble (bit 0 = enabled, bits 3:1 =
 *   priority; 0 = the line is off)   BEN the BLE block enable 0x2FC80 (bit 0) */
static void ble_adv_text(char *out, uint32_t vmhave, uint32_t vmarea, uint32_t vmtrim, uint32_t con, uint32_t ien, uint32_t ipnd, uint32_t stat, uint32_t ic45, uint32_t ic29, uint32_t ben)
{
    char *o = out;
    o = ble__put(o, "ADV i"); o = ble__dec(o, ble_cnt_irq_ev);
    o = ble__put(o, " e"); o = ble__dec(o, ble_cnt_hci);
    o = ble__put(o, " r"); o = ble__dec(o, ble_cnt_irq_rx);
    o = ble__put(o, " t"); o = ble__dec(o, ble_cnt_timer);
    o = ble__put(o, " s"); o = ble__dec(o, ble_cnt_slot);
    o = ble__put(o, " V "); o = ble__hex(o, vmhave);
    o = ble__put(o, " "); o = ble__hex(o, vmarea);
    o = ble__put(o, " "); o = ble__hex(o, vmtrim);
    o = ble__put(o, " CON "); o = ble__hex(o, con);
    o = ble__put(o, " IEN "); o = ble__hex(o, ien);
    o = ble__put(o, " IPND "); o = ble__hex(o, ipnd);
    o = ble__put(o, " STAT "); o = ble__hex(o, stat);
    o = ble__put(o, " IC45 "); o = ble__hex(o, ic45);
    o = ble__put(o, " IC29 "); o = ble__hex(o, ic29);
    o = ble__put(o, " BEN "); o = ble__hex(o, ben);
    *o = 0;
}
