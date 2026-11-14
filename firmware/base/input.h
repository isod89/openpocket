/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): the controls as events, for an app that does not poll the pins itself.
 * in_poll(now), called in every main-loop pass, reads the encoder, the buttons, the footswitches, the analog keypad,
 * the MASTER knob and the battery (base/controls.h) and calls the app, which decides what each one means:
 *   app_turn(dd, fast)        the encoder moved one detent: dd +1 or -1; fast: within 45 ms of the detent before
 *   app_button(b, ev, held)   button b (IN_FSUP .. IN_REV) and what it did (below); held: the buttons down now, bit 1 << b
 *   app_master(vol)           the MASTER knob is at vol (0..100): once after in_start, then when it moves
 * What a button does:
 *   IN_DOWN      it went down. Every press gives this one, at once (a tap tempo, a footswitch)
 *   IN_HOLD      it has been down for its hold time (in_start's table; 0: this button has none). Once a press
 *   IN_CLICK     it came up before its hold time, or it has none
 *   IN_RELEASE   it came up after an IN_HOLD (the end of something that lasts while it is held)
 * Two buttons together: the second one's IN_DOWN has the first one in `held`.
 * (OpenPocket polls the pins in its own main loop, openpocket.c, and times its gestures in ui.h.)
 */
#pragma once

enum { IN_FSUP, IN_FSDN, IN_PUSH, IN_HOME, IN_FX, IN_MOD, IN_AMP, IN_CAB, IN_DLY, IN_REV, IN_N };
enum { IN_DOWN, IN_HOLD, IN_CLICK, IN_RELEASE };

static void app_turn(int dd, int fast);
static void app_button(uint32_t b, uint32_t ev, uint32_t held);
static void app_master(uint32_t vol);

static struct {
    const uint16_t *hold_ms;                    /* each button's hold time (in_start) */
    uint32_t down, t[IN_N];                     /* the buttons down, and since when */
    uint8_t fired[IN_N];                        /* its IN_HOLD was given for this press */
} in_g;

static void in_buttons(uint32_t now, uint32_t down)   /* down: the buttons that are down, debounced, bit 1 << b */
{
    for (uint32_t b = 0; b < IN_N; b++) {
        const uint32_t bit = 1u << b, hold = in_g.hold_ms ? in_g.hold_ms[b] : 0u;
        if ((down & bit) && !(in_g.down & bit)) {
            in_g.down |= bit; in_g.t[b] = now; in_g.fired[b] = 0;
            app_button(b, IN_DOWN, in_g.down);
        } else if (!(down & bit) && (in_g.down & bit)) {
            in_g.down &= ~bit;
            app_button(b, in_g.fired[b] ? IN_RELEASE : IN_CLICK, in_g.down);
        } else if ((down & bit) && hold && !in_g.fired[b] && now - in_g.t[b] >= hold) {
            in_g.fired[b] = 1;
            app_button(b, IN_HOLD, in_g.down);
        }
    }
}

static struct {
    dbn_t d[IN_N];
    uint8_t enc, vol_told;
    int32_t accum, master_raw;
    uint32_t t_knob, t_adc, t_bat, vol;
} in_hw;

/* after input_init(), adc_pins_analog() and input_baseline(). hold_ms: IN_N hold times in ms, 0 for a button without one */
static void in_start(const uint16_t *hold_ms)
{
    in_g.hold_ms = hold_ms;
    in_hw.enc = (uint8_t)(PC_IN & ENC_MASK);
    in_hw.master_raw = -100;
    in_hw.vol = 80u;                            /* until the knob is read (the level every app starts at) */
}

static void in_poll(uint32_t now)
{
    /* the encoder, as OpenPocket reads it: four steps a detent, its direction inverted */
    const uint8_t s = (uint8_t)(PC_IN & ENC_MASK);
    if (s != in_hw.enc) {
        in_hw.accum -= QDEC[(in_hw.enc << 2) | s];
        in_hw.enc = s;
        int dd = 0;
        if (in_hw.accum >= 4) { in_hw.accum -= 4; dd = 1; } else if (in_hw.accum <= -4) { in_hw.accum += 4; dd = -1; }
        if (dd) {
            const int fast = (now - in_hw.t_knob) < 45u;
            in_hw.t_knob = now;
            app_turn(dd, fast);
        }
    }
    /* the buttons: pressed = differs from the idle state taken at the start */
    const uint32_t pa = (PA_IN & 0xFFFFu) ^ base_pa, ph = (PH_IN & 0xFFFFu) ^ base_ph;
    dbn_press(&in_hw.d[IN_FSUP], (pa & PA_FS_UP) != 0);
    dbn_press(&in_hw.d[IN_FSDN], (pa & PA_FS_DN) != 0);
    dbn_press(&in_hw.d[IN_PUSH], (ph & PH_PUSH) != 0);
    dbn_press(&in_hw.d[IN_HOME], (pa & PA_HOME) != 0);
    dbn_press(&in_hw.d[IN_FX], (pa & PA_FX) != 0);
    dbn_press(&in_hw.d[IN_MOD], (ph & PH_MOD) != 0);
    dbn_press(&in_hw.d[IN_AMP], (ph & PH_AMP) != 0);
    if (now - in_hw.t_adc >= 10u) {             /* the ADC every 10 ms: the battery (every 2 s), the keypad, MASTER */
        in_hw.t_adc = now;
        if (!in_hw.t_bat || now - in_hw.t_bat >= 2000u) { in_hw.t_bat = now ? now : 1u; bat_read(); }
        const int k = keys_decode(adc_read2(ADC_CH_KEYS));
        dbn_press(&in_hw.d[IN_CAB], k == KEY_CAB);
        dbn_press(&in_hw.d[IN_DLY], k == KEY_DLY);
        dbn_press(&in_hw.d[IN_REV], k == KEY_REV);
        const int32_t mv = adc_read2(ADC_CH_MASTER);
        const uint32_t was = in_hw.vol;
        if (mv >= 0) master_follow(&in_hw.master_raw, mv, &in_hw.vol);
        if (in_hw.vol != was || !in_hw.vol_told) { in_hw.vol_told = 1; app_master(in_hw.vol); }
    }
    uint32_t down = 0;
    for (uint32_t b = 0; b < IN_N; b++) if (in_hw.d[b].stab) down |= 1u << b;
    in_buttons(now, down);
}
