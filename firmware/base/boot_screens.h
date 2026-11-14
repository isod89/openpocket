/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the screens of a start. Statements, not functions: app_main includes this
 * file in its body, after lcd_init(). They draw the boot screen (OP_NAME, OP_VERSION, the supplies, the reset's
 * cause), then, after a start that did not finish or crashed, its report for 12 s. They declare
 *     int safe;     1 = this start is in safe mode: the app keeps its DSP off, the pedal stays usable and re-flashable
 * (They are statements so that OpenPocket's app_main compiles to the code it was before the base was split off:
 * as functions they changed its stack layout.)
 */
    /* boot screen with the version: shows which image is running */
    lcd_rect(0,0,240,240,UI_BG);
    ft_box(0, 98, 240, OP_NAME, &FT_MD, UI_ACC, UI_BG, 1);
#if OP_BLE_TEST
    ft_box(0, 126, 240, OP_VERSION " BLE TEST", &FT_SM, UI_DIM, UI_BG, 1);
#else
    ft_box(0, 126, 240, OP_VERSION, &FT_SM, UI_DIM, UI_BG, 1);
#endif
#if !AUDIO_PROBE
    ft_box(0, 150, 240, "STARTING", &FT_SM, UI_TXT, UI_BG, 1);   /* v42: a plain word first, the numbers small below */
    {   /* the supplies as found (fm1_cstart) and why the chip last reset */
        char l[32]; l[0] = 0;
        b_str(l, "PWR "); b_num(l, op_pwr.vdc0); b_str(l, " "); b_num(l, op_pwr.sys0); b_str(l, " (BOOT LEVELS)");
        ft_box(0, 196, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
        char h[9]; l[0] = 0;
        b_str(l, "RST "); rst_names(l, op_pwr.rst); b_str(l, " PINR "); hex8(h, op_pwr.pinr); b_str(l, h + 6);
        ft_box(0, 214, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
    }
#endif
    for (int i = 0; i < 6; i++) { fm1_wdt_feed(); fm1_delay_ms(100); }
    /* --- report of the previous boot, if it did not finish (or crashed) --- */
    int safe = 0;
    {
        uint32_t cc = (fm1_crash.magic == FM1_CRASH_MAGIC) ? fm1_crash.count : 0u;
        if (op_diag.magic != OPD_MAGIC) {
            op_diag.magic = OPD_MAGIC; op_diag.stage = ST_OK; op_diag.crash_seen = 0; op_diag.safe = 0;   /* a crash left by an older build is shown once */
        }
        int crashed = cc != op_diag.crash_seen;
        uint32_t st = op_diag.stage < ST_N ? op_diag.stage : 0u;
        if ((st != ST_OK || crashed) && !op_diag.safe) {
            char l[48], h[9];
            safe = 1;                    /* this boot: DSP bypassed, so the pedal stays usable */
            lcd_rect(0, 0, 240, 240, UI_BG);   /* v42: what happened and what to do first, the numbers for a bug report below */
            ft_box(0, 6, 240, "LAST START FAILED", &FT_MD, RED, UI_BG, 1);
            ft_box(0, 34, 240, "THE EFFECTS ARE OFF FOR THIS START", &FT_SM, UI_TXT, UI_BG, 1);
            ft_box(0, 52, 240, "SWITCH OFF AND ON TO TRY AGAIN", &FT_SM, UI_TXT, UI_BG, 1);
            l[0] = 0; b_str(l, "STOPPED AT: "); b_str(l, ST_NAME[st]);
            ft_box(0, 78, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
            if (!crashed) {
                l[0] = 0; b_str(l, "RESET BY: "); rst_names(l, op_pwr.rst);
                ft_box(0, 96, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
            }
            if (crashed) {
                l[0] = 0; b_str(l, "CRASH  PC "); hex8(h, fm1_crash.pc); b_str(l, h);
                ft_box(0, 96, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
                l[0] = 0; b_str(l, "RETS "); hex8(h, fm1_crash.rets); b_str(l, h); b_str(l, "  VEC "); b_num(l, fm1_crash.vec);
                ft_box(0, 114, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
#ifdef OP_RAM_CODE
                l[0] = 0; b_str(l, "DSP CODE AT "); hex8(h, (uint32_t)(uintptr_t)OP_RAM_CODE); b_str(l, h);
                ft_box(0, 132, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
#endif
                l[0] = 0; b_str(l, "UPTIME "); b_num(l, fm1_crash.uptime_ms); b_str(l, " MS  DBG "); hex8(h, fm1_crash.dbg); b_str(l, h);
                ft_box(0, 150, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
            }
            l[0] = 0; b_str(l, "BLOCKS "); b_num(l, op_diag.blocks); b_str(l, "  LOAD "); b_num(l, op_diag.load);
            b_str(l, "%  PEAK "); b_num(l, op_diag.peak); b_str(l, "%");
            ft_box(0, 168, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
            l[0] = 0; b_str(l, "NAN "); b_num(l, op_diag.nans); b_str(l, "  CPU1 "); b_num(l, op_diag.c1);
            b_str(l, "  LOOPS "); b_num(l, op_diag.loops);
            ft_box(0, 186, 240, l, &FT_SM, UI_DIM, UI_BG, 1);
            ft_box(0, 212, 240, "PHOTO THIS FOR A BUG REPORT", &FT_SM, UI_ACC, UI_BG, 1);
            for (int i = 0; i < 120; i++) { fm1_wdt_feed(); fm1_delay_ms(100); }   /* 12 s */
            lcd_rect(0, 0, 240, 240, UI_BG);
            ft_box(0, 98, 240, OP_NAME, &FT_MD, UI_ACC, UI_BG, 1);
            ft_box(0, 126, 240, OP_VERSION " SAFE MODE", &FT_SM, UI_DIM, UI_BG, 1);
        }
#if OP_BLE_TEST
        ble_last = (ble_boot_t){ st != ST_OK || crashed, crashed, st, op_diag.blocks, op_diag.load, op_diag.peak, op_diag.nans, op_diag.c1,
                                 op_diag.loops, fm1_crash.vec, fm1_crash.pc, fm1_crash.rets, fm1_crash.dbg, fm1_crash.emu, fm1_crash.uptime_ms };
#endif
        op_diag.crash_seen = cc; op_diag.safe = (uint32_t)safe;
        op_diag.blocks = op_diag.load = op_diag.peak = op_diag.nans = op_diag.c1 = op_diag.loops = 0;
    }
