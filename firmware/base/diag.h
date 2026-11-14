/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): what a start leaves behind for the next one and for a bug report. The
 * crash screen, the boot stages and the audio figures kept across a reset (op_diag, in NOINIT), the supplies as found,
 * and the stacks' deepest use. base/start.h reads them at the next start.
 */
#pragma once

/* readable crash screen: vector, PC, return address, bus error register — then reboot */
static void hex8(char *b, uint32_t v){ static const char H[]="0123456789ABCDEF";
    for (int i=0;i<8;i++) b[i]=H[(v>>(28-4*i))&15u]; b[8]=0; }
static void fm1_fault(const fm1_crash_t *c)
{
    char h[9];
    lcd_rect(0,0,240,240,RED);
    lcd_text(8,8,"CRASH",WHT,RED,2);
    lcd_text(8,40,"VEC",WHT,RED,1); hex8(h,c->vec);  lcd_text(64,40,h,WHT,RED,1);
    lcd_text(8,56,"PC", WHT,RED,1); hex8(h,c->pc);   lcd_text(64,56,h,WHT,RED,1);
    lcd_text(8,72,"RETS",WHT,RED,1);hex8(h,c->rets); lcd_text(64,72,h,WHT,RED,1);
    lcd_text(8,88,"DBG",WHT,RED,1); hex8(h,c->dbg);  lcd_text(64,88,h,WHT,RED,1);
    lcd_text(8,104,"EMU",WHT,RED,1);hex8(h,c->emu);  lcd_text(64,104,h,WHT,RED,1);
    lcd_text(8,136,"Take a photo :)",WHT,RED,1);
    for (int i=0;i<6;i++){ fm1_wdt_feed(); fm1_delay_ms(1000); }
    fm1_reboot();
}
/* ===== boot diagnostics (v23b) : survive a reset in .noinit, shown at the next boot =====
 * stage = last boot step reached; blocks/load/peak/nans/c1/loops = audio state seen by the main loop. */
#define OPD_MAGIC 0x4F504442u
enum { ST_NONE, ST_INPUTS, ST_PRESET, ST_ENG_INIT, ST_ENG_SET, ST_I2S, ST_UI, ST_RUN, ST_OK, ST_N };
static const char *const ST_NAME[ST_N] = {"?", "INPUTS", "PRESET", "ENGINE INIT", "ENGINE SET", "I2S START",
                                          "UI DRAW", "FIRST 6 S", "OK"};
volatile struct { uint32_t magic, stage, blocks, load, peak, nans, c1, loops, crash_seen, safe; } op_diag
    OP_NOINIT;
static struct { uint8_t vdc0, vdc1, sys0, sys1, dcdc, rst, pinr; } op_pwr;   /* supplies, reset source, pin reset (fm1_cstart) */
/* P3_RST_SRC as read at boot: why the chip last reset (fm1_sys.h) */
static void rst_names(char *l, uint32_t r)
{
    static const char *const N[7] = {"POWER-ON", "VDDIO LOW", "WATCHDOG", "VCM", "LONG PRESS", "1.2V", "SOFT"};
    int any = 0;
    for (int i = 0; i < 7; i++)
        if (r & (1u << i)) { if (any) b_str(l, "+"); b_str(l, N[i]); any = 1; }
    if (!any) b_str(l, "NONE");
}

static void op_stage(uint32_t s, const char *txt)
{
    op_diag.stage = s;
    fm1_wdt_feed();
    (void)txt;
}
/* v40.1: the stacks' deepest use since boot. fm1_cstart paints them; the bytes at the bottom never written since are free */
#define OP_PAINT 0x5AA5C33Cu
extern char _ustack_top[];
static uint32_t stack_free(const char *lo, const char *hi)
{
    const uint32_t *p = (const uint32_t *)(const void *)lo;
    while ((const char *)p < hi && *p == OP_PAINT) p++;
    return (uint32_t)((const char *)p - lo);
}
