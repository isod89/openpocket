/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the board. The chip's support (firmware/bsp), the pins, the time, the
 * boot guard's record, the screen (ST7789 over SPI1 with DMA, or software SPI) and what draws on it: rectangles, a
 * pixel window, the 8x8 font and the anti-aliased one (ui_text.h).
 *
 * The app's file defines before this one: AUDIO_PROBE (1 = the audio test in place of the app) and DSP_CPU1 (1 = the
 * second core is started for the DSP; firmware/build.sh reads that line to link its entry).
 */
#pragma once
#if DSP_CPU1 && !AUDIO_PROBE
#define FM1_CPU1 1         /* fm1_irq.h hands 2nd-core faults to fm1_cpu1.h */
#define FM1_CPU1_FALLBACK 1
#else
#undef DSP_CPU1
#define DSP_CPU1 0
#endif
#define FELUCCA_ID "FM-1_900"
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#if DSP_CPU1
#include "fm1_cpu1.h"
#endif
#include "font8x8.h"
#include "libc.c"           /* memset / memcpy / memcmp (felucca-bsp, MPL-2.0): also used by the JieLi I2S driver */

/* ---- GPIO port B/C (base 0x50000 + port*0x40 ; OUT +0x00, DIR +0x08 ; DIR 0=output) ---- */
#define PB_OUT (*(volatile uint32_t *)0x50040u)
#define PB_DIR (*(volatile uint32_t *)0x50048u)
#define PC_OUT (*(volatile uint32_t *)0x50080u)
#define PC_DIR (*(volatile uint32_t *)0x50088u)
#define CS  (1u << 1)   /* PB1 */
#define DC  (1u << 5)   /* PB5 */
#define CLK (1u << 6)   /* PB6 */
#define DO  (1u << 7)   /* PB7 */
#define PC7 (1u << 7)

#define PX_ROW_OFFSET 0u
#define C(c) ((uint16_t)(((c) >> 8) | ((c) << 8)))
enum { RED=0xF800, GRN=0x07E0, BLU=0x001F, WHT=0xFFFF, BLK=0x0000, CYN=0x07FF, YEL=0xFFE0, ORG=0xFD20 };

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[], _bss_hi_start[], _bss_hi_end[], _pool_tail_start[], _pool_tail_end[],
                _bss_lo_start[], _bss_lo_end[];
/* where an object lives (the simulator of an app, tests/host, has no such sections: it defines both empty) */
#ifndef OP_NOINIT
#define OP_NOINIT __attribute__((section(".noinit")))   /* kept across a reset */
#define OP_POOL   __attribute__((section(".pool")))     /* the app's POOL region (memory.ld): its engine's state, not zeroed at the start */
#endif
static volatile struct { uint32_t magic, pending, failed; } guard OP_NOINIT;

static uint32_t ms_now(void) { return (uint32_t)(fm1_ticks() / (FM1_TICKS_PER_US * 1000u)); }

/* ---- screen link: HARDWARE SPI1 + DMA (LCD_HW 1) or software SPI (LCD_HW 0, the old code, fallback) ----
 * PB6 = SPI1_CLK(A), PB7 = SPI1_DO(A) per the AC791x datasheets in the JieLi SDK; IOMAP_CON1 bit 4 = 0 -> group A
 * (as in the SDK's SPI1_PLATFORM_DATA). PB5 (D/C) stays a GPIO, like the FM-1's D/C on the group B DI pin.
 * SPI1 registers and CON = 0x4021 setting: Felucca's screen driver (bsp/boards/fm1/fm1_lcd_hw.h). */
#ifndef LCD_HW
#define LCD_HW 1
#endif
#define LCD_BAUD 2u                     /* SPI1 clock = LSB / (BAUD + 1): 60 MHz / 3 = 20 MHz */
#if LCD_HW
#define LSPI_CON  (*(volatile uint32_t *)0x11D00u)
#define LSPI_BAUD (*(volatile uint32_t *)0x11D04u)
#define LSPI_BUF  (*(volatile uint32_t *)0x11D08u)
#define LSPI_ADR  (*(volatile uint32_t *)0x11D0Cu)
#define LSPI_CNT  (*(volatile uint32_t *)0x11D10u)
#define IOMAP_CON1 (*(volatile uint32_t *)0x51020u)
static uint32_t lcd_timeouts;
static int lspi_busy;                   /* a DMA transfer is in progress */
static void lspi_wait(void)
{
    uint32_t n;
    if (!lspi_busy) return;
    for (n = 0; n < 2000000u && !(LSPI_CON & 0x8000u); n++) ;
    if (n == 2000000u) lcd_timeouts++;
    LSPI_CON |= 0x4000u;                /* clear the done flag */
    lspi_busy = 0;
}
static void lspi_init(void)
{
    IOMAP_CON1 &= ~0x10u;               /* SPI1 on group A: PB6/PB7 */
    LSPI_CON = 0x4021u;
    LSPI_BAUD = LCD_BAUD;
    lspi_busy = 0;
}
static inline void spi_byte(uint8_t b)  /* one byte, blocking */
{
    lspi_wait();
    LSPI_CON |= 0x4000u;
    LSPI_BUF = b;
    lspi_busy = 1;
    lspi_wait();
}
static void lspi_dma(const void *p, uint32_t n)   /* start n bytes from RAM (does not wait) */
{
    lspi_wait();
    LSPI_CON |= 0x4000u;
    LSPI_ADR = (uint32_t)(uintptr_t)p;
    LSPI_CNT = n;
    lspi_busy = 1;
}
#else
static inline void spi_byte(uint8_t b)
{
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 0x80u) PB_OUT |= DO; else PB_OUT &= ~DO;
        PB_OUT &= ~CLK;
        PB_OUT |= CLK;             /* rising edge */
        b = (uint8_t)(b << 1);
    }
    PB_OUT &= ~CLK;
}
#endif
static void lcd_cmd(uint8_t c, const uint8_t *data, uint32_t n)
{
    PB_OUT &= ~CS;                 /* select */
    PB_OUT &= ~DC;                 /* command */
    spi_byte(c);
    if (n) {
        uint32_t i;
        PB_OUT |= DC;              /* data */
        for (i = 0; i < n; i++) spi_byte(data[i]);
    }
    PB_OUT |= CS;                  /* deselect */
}

static const uint8_t PX_INIT[] = {   /* EXACT sequence from the Pocket X firmware (21 commands) */
    0x01,0, 0x11,0,
    0x2A,4, 0x00,0x00,0x00,0xEF,
    0x2B,4, 0x00,0x28,0x01,0x17,
    0xB2,5, 0x0C,0x0C,0x0C,0x00,0x33,
    0x20,0,
    0xB7,1, 0x56, 0xBB,1, 0x18, 0xC0,1, 0x2C,
    0xC2,1, 0x01, 0xC3,1, 0x1F, 0xC4,1, 0x20, 0xC6,1, 0x0F,
    0xD0,2, 0xA6,0xA1,
    0xE0,14, 0xD0,0x0D,0x14,0x0B,0x0B,0x07,0x3A,0x44,0x50,0x08,0x13,0x13,0x2D,0x32,
    0xE1,14, 0xD0,0x0D,0x14,0x0B,0x0B,0x07,0x3A,0x44,0x50,0x08,0x13,0x13,0x2D,0x32,
    0x36,1, 0x00, 0x3A,1, 0x55, 0xE7,1, 0x00,
    0x51,1, 0xFF,                  /* max brightness */
    0x21,0, 0x29,0,                /* INVON, DISPON */
};
static void lcd_init(void)
{
    const uint8_t *p = PX_INIT, *end = PX_INIT + sizeof PX_INIT;
    PB_DIR &= ~(CS | DC | CLK | DO);   /* outputs */
    PC_DIR &= ~PC7;
    PB_OUT |= CS; PB_OUT &= ~CLK;      /* CS high, CLK low */
    PC_OUT |= PC7;                     /* PC7 high: the button lights off (their enable, low = on: px_leds.h) */
#if LCD_HW
    lspi_init();
#endif
    fm1_delay_ms(50);
    while (p < end) {
        uint8_t cmd = *p++, len = *p++;
        lcd_cmd(cmd, p, len);
        p += len;
        if (cmd == 0x01) fm1_delay_ms(150);
        else if (cmd == 0x11) fm1_delay_ms(120);
    }
}
/* DMA pixel buffers (2: fill one while the other is being sent) */
#define LBUF_PX 1024u
#if LCD_HW
static uint8_t lbuf[2][LBUF_PX * 2u] __attribute__((aligned(4)));
static uint32_t lb_i;
#endif
static uint32_t lb_n;
/* filled rectangle (16-bit coords + 40-line offset) */
static void lcd_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t rgb)
{
    if (w == 0u || h == 0u || x >= 240u || y >= 240u) return;   /* v23: a negative size (huge unsigned) used to */
    if (x + w > 240u) w = 240u - x;                            /* stream 4 billion pixels = black screen forever */
    if (y + h > 240u) h = 240u - y;
    uint8_t a[4]; uint32_t i, yo = y + PX_ROW_OFFSET, x1 = x + w - 1u, y1 = yo + h - 1u, px = w * h;
    uint16_t sw = rgb;   /* sent high byte first, no swap */
    a[0]=(uint8_t)(x>>8);  a[1]=(uint8_t)x;  a[2]=(uint8_t)(x1>>8); a[3]=(uint8_t)x1;  lcd_cmd(0x2A,a,4);
    a[0]=(uint8_t)(yo>>8); a[1]=(uint8_t)yo; a[2]=(uint8_t)(y1>>8); a[3]=(uint8_t)y1;  lcd_cmd(0x2B,a,4);
    PB_OUT &= ~CS; PB_OUT &= ~DC; spi_byte(0x2C); PB_OUT |= DC;   /* RAMWR, then data */
#if LCD_HW
    {   /* one buffer filled with the colour, resent as many times as needed (constant contents) */
        uint8_t *b = lbuf[0];
        uint32_t fill = px < LBUF_PX ? px : LBUF_PX;
        for (i = 0; i < fill; i++) { b[2u * i] = (uint8_t)(sw >> 8); b[2u * i + 1u] = (uint8_t)sw; }
        while (px) {
            uint32_t n = px < LBUF_PX ? px : LBUF_PX;
            lspi_dma(b, n * 2u);
            px -= n;
            fm1_wdt_feed();
        }
        lspi_wait();
    }
#else
    for (i = 0; i < px; i++) {
        spi_byte((uint8_t)(sw >> 8));
        spi_byte((uint8_t)sw);
        if ((i & 0x3FFu) == 0) fm1_wdt_feed();   /* feed the watchdog during large fills */
    }
#endif
    PB_OUT |= CS;
}

/* ================= OpenPocket UI (on-device sound editor) ================= */
/* palette (RGB565) matching the simulator: dark background + orange accent */
enum { UI_BG=0x0841, UI_PANEL=0x18E3, UI_SEL=0xFB00, UI_ACC=0xFD20,
       UI_TXT=0xEF7D, UI_DIM=0x8410, UI_OK=0x4E6B, UI_OFF=0x2124 };

/* write window + pixel stream (CS stays low between pixels) */
static void lcd_win(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint8_t a[4]; uint32_t yo=y+PX_ROW_OFFSET, x1=x+w-1u, y1=yo+h-1u;
    a[0]=(uint8_t)(x>>8);a[1]=(uint8_t)x;a[2]=(uint8_t)(x1>>8);a[3]=(uint8_t)x1; lcd_cmd(0x2A,a,4);
    a[0]=(uint8_t)(yo>>8);a[1]=(uint8_t)yo;a[2]=(uint8_t)(y1>>8);a[3]=(uint8_t)y1; lcd_cmd(0x2B,a,4);
    PB_OUT &= ~CS; PB_OUT &= ~DC; spi_byte(0x2C); PB_OUT |= DC;
    lb_n = 0;
}
#if LCD_HW
static void lb_flush(void)
{
    if (!lb_n) return;
    lspi_dma(lbuf[lb_i], lb_n * 2u);    /* waits for the previous transfer (the other buffer), then starts this one */
    lb_i ^= 1u; lb_n = 0;
}
static inline void lcd_px(uint16_t c)
{
    uint8_t *p = &lbuf[lb_i][lb_n * 2u];
    p[0] = (uint8_t)(c >> 8); p[1] = (uint8_t)c;
    if (++lb_n == LBUF_PX) lb_flush();
}
static inline void lcd_end(void){ lb_flush(); lspi_wait(); PB_OUT |= CS; }
#else
static inline void lcd_px(uint16_t c){ spi_byte((uint8_t)(c>>8)); spi_byte((uint8_t)c); }
static inline void lcd_end(void){ PB_OUT |= CS; }
#endif

#include "ui_text.h"                    /* anti-aliased text (Inter), rounded boxes */

/* one character (scale sx,sy), fg on bg */
static void lcd_char(uint32_t x, uint32_t y, char ch, uint16_t fg, uint16_t bg, uint32_t s)
{
    if (ch < 0x20 || ch > 0x7E) ch = ' ';
    const uint8_t *g = FONT8X8[ch - 0x20];
    lcd_win(x, y, 8u*s, 8u*s);
    for (uint32_t row=0; row<8u; row++)
        for (uint32_t ry=0; ry<s; ry++)
            for (uint32_t col=0; col<8u; col++) {
                uint16_t c = (g[row] & (0x80u>>col)) ? fg : bg;
                for (uint32_t rx=0; rx<s; rx++) lcd_px(c);
            }
    lcd_end();
}
static void lcd_text(uint32_t x, uint32_t y, const char *str, uint16_t fg, uint16_t bg, uint32_t s)
{
    while (*str) { lcd_char(x, y, *str++, fg, bg, s); x += 8u*s; }
}
/* ---- interrupt entries the vector table names and this board does not use ---- */
#if AUDIO_PROBE
void fm1_timer5_irq(void) { }
#endif
#if !AUDIO_PROBE
void op_audio_irq(void) { }           /* internal DAC: not used by the Pocket X */
#endif
/* ---- text for a line of the screen: a number or a string appended to d ---- */
static void b_num(char *d, uint32_t v)
{
    char t[10]; int n = 0;
    while (*d) d++;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 9);
    while (n) *d++ = t[--n];
    *d = 0;
}
static void b_str(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }
static void b_fix2(char *d, uint32_t v100)           /* 123 -> "1.23" */
{
    b_num(d, v100 / 100u);
    b_str(d, ".");
    if (v100 % 100u < 10u) b_str(d, "0");
    b_num(d, v100 % 100u);
}
