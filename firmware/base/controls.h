/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the controls. The pins of the encoder, the buttons and the footswitches,
 * the ADC (the MASTER knob, the analog keypad CAB / DLY / REV), the battery gauge, the debounce and the encoder's
 * quadrature table. Reading only: the app's main loop polls them and decides what a press means.
 *
 * PROBE and ADC_PROBE (1 = a diagnostic line at the bottom of the screen) are the app's file's to define.
 */
#pragma once

/* ---- Inputs: rotary encoder (PC0/PC1) + push button (PA2) ----
 * Reverse-engineered from the stock firmware (timer ISR reading PC_IN & 0x3). */
#ifndef PA_IN             /* (a host test or the simulator defines the ports itself: tests/hostpins.h) */
#define PA_IN   (*(volatile uint32_t *)0x50004u)
#define PA_DIR  (*(volatile uint32_t *)0x50008u)
#define PA_DIE  (*(volatile uint32_t *)0x5000Cu)
#define PA_PU   (*(volatile uint32_t *)0x50010u)
#define PB_IN   (*(volatile uint32_t *)0x50044u)
#define PB_DIE  (*(volatile uint32_t *)0x5004Cu)
#define PB_PU   (*(volatile uint32_t *)0x50050u)
#define PC_IN   (*(volatile uint32_t *)0x50084u)
#define PC_DIR  (*(volatile uint32_t *)0x50088u)
#define PC_DIE  (*(volatile uint32_t *)0x5008Cu)
#define PC_PU   (*(volatile uint32_t *)0x50090u)
#define PC_PD   (*(volatile uint32_t *)0x50094u)
/* port H (index 7): declared by Felucca and read by the stock firmware. NO port D on this chip. */
#define PH_IN   (*(volatile uint32_t *)0x501C4u)
#define PH_DIE  (*(volatile uint32_t *)0x501CCu)
#define PH_PU   (*(volatile uint32_t *)0x501D0u)
#define PH_DIR  (*(volatile uint32_t *)0x501C8u)
#endif
#define ENC_MASK 0x3u      /* PC0=A, PC1=B */
#ifndef PA_PD
#define PA_PD   (*(volatile uint32_t *)0x50014u)
#define BAT_CHG (*(volatile uint32_t *)0x16A10u)   /* (stock reads its bit 16 beside the gauge: charging?) */
#endif

/* ---- Controls mapped on the real pedal (diagnostic) ----
 * "pressed" = differs from the idle state captured at boot (works whatever the polarity) */
#define PA_FX     (1u<<0)
#define PA_HOME   (1u<<1)
#define PA_FS_UP  (1u<<2)              /* right footswitch ▲ */
#define PA_FS_DN  (1u<<3)              /* left footswitch ▼ */
#define PH_PUSH   ((1u<<4)|(1u<<6))    /* PARA knob push */
#define PH_MOD    ((1u<<7)|(1u<<8))
#define PH_AMP    (1u<<9)
#define PA_KNOWN  (PA_FX|PA_HOME|PA_FS_UP|PA_FS_DN)
#define PH_KNOWN  (PH_PUSH|PH_MOD|PH_AMP)
#define PA_MASTER (1u<<10)             /* MASTER knob = analog pot on PA10 */

static uint32_t base_pa, base_ph;      /* idle state */

static void input_init(void)
{
    PC_DIR |= ENC_MASK; PC_DIE |= ENC_MASK; PC_PU |= ENC_MASK;   /* encoder */
    PA_DIR |= PA_KNOWN; PA_DIE |= PA_KNOWN; PA_PU |= PA_KNOWN;   /* confirmed buttons */
    PH_DIR |= PH_KNOWN; PH_DIE |= PH_KNOWN; PH_PU |= PH_KNOWN;
#if PROBE
    /* unknown pins: digital input + pull-up only (direction left untouched) */
    PA_DIE |= 0xFFFFu; PA_PU |= 0xFFFFu;
    PC_DIE |= 0xFF7Cu; PC_PU |= 0xFF7Cu;                       /* except PC0/1 (encoder), PC7 (screen) */
    PH_DIE |= 0xFFFFu; PH_PU |= 0xFFFFu;
    PB_DIE |= 0xFF1Du; PB_PU |= 0xFF1Du;                       /* except PB1/5/6/7 (screen) */
#endif
    PC_DIR &= ~PC7; PC_OUT |= PC7;      /* PC7, the button lights' enable: high = off until px_leds.h turns them on */
    PC_DIR |= 1u << 9; PC_DIE &= ~(1u << 9); PC_PU &= ~(1u << 9); PC_PD &= ~(1u << 9);   /* PC9: the battery divider (analog) */
}
static void input_baseline(void)
{
    fm1_delay_ms(30);                   /* let the pull-ups settle */
    base_pa = PA_IN & 0xFFFFu; base_ph = PH_IN & 0xFFFFu;
}

#if PROBE
/* 16-pins/port diagnostic: shows the NAME of the pins that change (e.g. "PA12 PH4"). "--" = none. */
static uint32_t probe_base[4], probe_prev[4], probe_t = 0;
static const uint32_t PROBE_MASK[4] = { 0xFFFFu, 0xFF1Du, 0xFF7Cu, 0xFFFFu };
static void probe_capture(void)
{
    probe_base[0]=PA_IN&0xFFFFu; probe_base[1]=PB_IN&0xFFFFu; probe_base[2]=PC_IN&0xFFFFu; probe_base[3]=PH_IN&0xFFFFu;
    for (int i=0;i<4;i++) probe_prev[i]=0xFFFFFFFFu;
}
static void probe_line(uint32_t now)
{
    uint32_t v[4] = { PA_IN&0xFFFFu, PB_IN&0xFFFFu, PC_IN&0xFFFFu, PH_IN&0xFFFFu };
    int same = 1; for (int i=0;i<4;i++) if (v[i]!=probe_prev[i]) same = 0;
    if (same && (now - probe_t) < 400u) return;
    for (int i=0;i<4;i++) probe_prev[i]=v[i];
    probe_t = now;
    char s[40]; int n = 0; static const char PN[4] = {'A','B','C','H'};
    for (int p = 0; p < 4 && n < 33; p++) {
        uint32_t d = (v[p] ^ probe_base[p]) & PROBE_MASK[p];
        for (int b = 0; b < 16 && n < 33; b++)
            if (d & (1u << b)) {
                s[n++]='P'; s[n++]=PN[p];
                if (b >= 10) { s[n++]='1'; s[n++]=(char)('0'+b-10); } else s[n++]=(char)('0'+b);
                s[n++]=' ';
            }
    }
    if (!n) { s[n++]='-'; s[n++]='-'; }
    s[n] = 0;
    lcd_rect(0,224,240,16,UI_PANEL);
    lcd_text(6,228,s,UI_ACC,UI_PANEL,1);
}
#endif

/* ---- Analog inputs (SARADC) ----
 * Felucca's driver (MPL-2.0, same chip). DO NOT call fm1_adc_init():
 * it switches PB1/PB6 to analog, and on the Pocket X those are screen pins. */
#include "fm1_adc.h"
#define ADC_CH_MASTER 2      /* MASTER knob (PA10): 0..1023 */
#define ADC_CH_KEYS   9      /* analog keypad: CAB ~617, DLY ~706, REV ~788 (measured) */
enum { KEY_NONE = 0, KEY_CAB, KEY_DLY, KEY_REV };
static int32_t keys_idle = -1;
static void adc_pins_analog(void)
{   /* PA7/PA8 (possible analog keypad), PA10 (MASTER): analog inputs, no pull */
    uint32_t m = (1u<<7) | (1u<<8) | PA_MASTER;
    PA_DIE &= ~m; PA_PU &= ~m; PA_PD &= ~m; PA_DIR |= m;
}
/* "clean" ADC read: a 1st conversion is discarded (the sampling capacitor still holds
 * the previous channel's voltage — ghosting), then the real one */
static int32_t adc_read2(uint32_t ch)
{
    (void)fm1_adc_read(ch);
    return fm1_adc_read(ch);
}

/* ---- battery (v37): as the stock firmware reads it. On the Pocket X the chip's own VBAT pin is a regulated 3.3 V (it
 * reads 3.31 V charged or not, on USB or not: v36 read it, so the gauge never moved) and its 5 V input is not the USB's.
 * The cell is on PC9 (ADC channel 8) through a 1:2 divider: PocketX_016 (f_02026f60, adc_get_value(8) = f_02025682;
 * BlackBox the same) maps raw 528 and below to 0 %, 636 and above to 100 %, (raw x 100 - 52700) / 109 between, on an
 * average of 300 readings. PC9 is an analog input (input_init). Only the ADC: nothing analog is switched (the PMU test bus
 * readings of the first v37 builds are gone). */
static int bat_mv, bat_usb_mv;         /* last readings (0: none); bat_usb_mv stays 0 (no USB sense found yet) */
static int32_t bat_raw[5] = {-1, -1, -1, -1, -1};   /* the tuner page's raw line: PC9, 0x16A10 bit 16, overruns, clips, peak */
static int bat_pct = -1;               /* the gauge (-1: no reading) */
static int32_t bat_c9q;                /* PC9 smoothed, x4 */
static void bat_read(void)
{
    int32_t c9 = 0, n = 0;
    (void)fm1_adc_read(8u);                             /* (the sampling capacitor still holds the last channel) */
    for (int k = 0; k < 8; k++) { int32_t v = fm1_adc_read(8u); if (v >= 0) { c9 += v; n++; } }
    c9 = n ? c9 / n : -1;
    bat_raw[0] = c9;
    bat_raw[1] = (int32_t)((BAT_CHG >> 16) & 1u);
    if (c9 < 100) { bat_mv = 0; bat_pct = -1; bat_c9q = 0; return; }   /* no divider there: no gauge */
    bat_c9q = bat_c9q ? (bat_c9q * 3 + c9 * 4) / 4 : c9 * 4;   /* a little smoothing */
    int r = (int)(bat_c9q / 4);
    bat_pct = r > 635 ? 100 : r < 528 ? 0 : (r * 100 - 52700) / 109;
    bat_mv = r * 6600 / 1023;                           /* the cell, roughly (3.3 V reference, 1:2): for the tuner page */
}
/* the MASTER knob: a reading mv (0..1023) moves the volume *vol (0..100). raw is the caller's, below 0 at first:
 * smoothing (x16 moving average, ~160 ms) then hysteresis of 2 on the 0..100 volume */
static void master_follow(int32_t *raw, int32_t mv, uint32_t *vol)
{
    *raw = (*raw < 0) ? mv * 16 : *raw + mv - *raw / 16;
    int32_t avg = *raw / 16;
    uint32_t nv;
    if (avg <= 12) nv = 0u;
    else if (avg >= 1010) nv = 100u;
    else nv = (uint32_t)(avg - 12) * 100u / (1010u - 12u);
    int32_t dv = (int32_t)nv - (int32_t)*vol; if (dv < 0) dv = -dv;
    if (dv >= 2 || (nv == 0u && *vol != 0u) || (nv == 100u && *vol != 100u)) *vol = nv;
}
#define KEY_DEBUG 0          /* 1 = shows the raw value of channel 9 at the bottom right (K####) */
/* identifies the analog keypad button by the nearest value (±42 window),
 * independent of the idle state */
static int keys_decode(int32_t v)
{
    static const int32_t REF[3] = { 617, 706, 788 };   /* CAB, DLY, REV */
    if (v < 0) return KEY_NONE;
    int best = -1; int32_t bd = 100000;
    for (int i = 0; i < 3; i++) { int32_t e = v - REF[i]; if (e < 0) e = -e; if (e < bd) { bd = e; best = i; } }
    return (bd <= 42) ? best + 1 : KEY_NONE;
}
static void keys_capture_idle(void)
{
    fm1_delay_ms(20);
    keys_idle = fm1_adc_read(ADC_CH_KEYS);
    /* if a button was held at boot (value close to a button's), assume idle is 1023 */
    static const int32_t REF[3] = { 617, 706, 788 };
    for (int i = 0; i < 3 && keys_idle >= 0; i++) {
        int32_t e = keys_idle - REF[i]; if (e < 0) e = -e;
        if (e <= 45) { keys_idle = 1023; break; }
    }
}
#if ADC_PROBE
#define ADC_NCH 16
static int32_t adc_base[ADC_NCH];
static uint32_t adc_t = 0;
static char adc_last[40];
static int adc_skip(uint32_t ch) { return ch == 3u || ch == 4u; }   /* PB1/PB6 = screen */
static void adc_capture(void)
{
    fm1_delay_ms(20);
    for (uint32_t ch = 0; ch < ADC_NCH; ch++) adc_base[ch] = adc_skip(ch) ? -1 : fm1_adc_read(ch);
    adc_last[0] = 0;
}
static void adc_line(uint32_t now)
{
    if (now - adc_t < 150u) return;
    adc_t = now;
    char s[40]; int n = 0;
    for (uint32_t ch = 0; ch < ADC_NCH && n <= 20; ch++) {
        if (adc_skip(ch) || adc_base[ch] < 0) continue;
        int32_t v = fm1_adc_read(ch);
        if (v < 0) continue;
        int32_t d = v - adc_base[ch]; if (d < 0) d = -d;
        if (d > 60) {
            s[n++]='C';
            if (ch >= 10u) { s[n++]='1'; s[n++]=(char)('0'+ch-10u); } else s[n++]=(char)('0'+ch);
            s[n++]=':';
            s[n++]=(char)('0'+(v/1000)%10); s[n++]=(char)('0'+(v/100)%10);
            s[n++]=(char)('0'+(v/10)%10);   s[n++]=(char)('0'+v%10);
            s[n++]=' ';
        }
    }
    if (!n) { s[n++]='-'; s[n++]='-'; }
    s[n] = 0;
    static uint32_t drawn_t = 0;
    int same = 1; for (int i = 0; i <= n; i++) if (s[i] != adc_last[i]) { same = 0; break; }
    if (same && (now - drawn_t) < 1000u) return;   /* redraw on change, or every second */
    drawn_t = now;
    for (int i = 0; i <= n; i++) adc_last[i] = s[i];
    lcd_rect(0,224,240,16,UI_PANEL);
    lcd_text(6,228,s,UI_ACC,UI_PANEL,1);
}
#endif

/* generic debounce: returns 1 on the press edge */
typedef struct { uint8_t stab, last, cnt; } dbn_t;
static int dbn_press(dbn_t *d, int raw)
{
    if (raw == d->last) { if (d->cnt < 5u) d->cnt++; }
    else { d->cnt = 0; d->last = (uint8_t)raw; }
    if (d->cnt >= 3u && raw != d->stab) { d->stab = (uint8_t)raw; return raw; }
    return 0;
}

/* standard quadrature table: index = (old<<2)|new -> -1/0/+1 */
static const int8_t QDEC[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
