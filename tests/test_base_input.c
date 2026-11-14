/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — host test of the base firmware's controls as events (firmware/base/input.h over base/controls.h), as an
 * app's main loop uses them (firmware/apps/starter). The pedal's own code runs here; only the chip's ports and ADC
 * are stood in for (tests/host/pins.h): the test sets a pin or an ADC reading as a hand would, lets the main loop
 * pass, and reads the events the app was given.
 *   cc -O1 -Wall -Wno-unused-function -Wno-unused-variable -Itests/host -Ifirmware tests/test_base_input.c -o build/test_base_input
 */
#include <stdio.h>
#include <string.h>
#include "pins.h"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#define PROBE 0
#define ADC_PROBE 0
#include "base/controls.h"
#include "base/input.h"

/* ---- the app: it writes down what it was given ---- */
static char ev[512];
static uint32_t vol_now = 999u, vol_calls;
static void app_turn(int dd, int fast) { snprintf(ev + strlen(ev), 16, "T%+d%s ", dd, fast ? "f" : ""); }
static void app_button(uint32_t b, uint32_t e, uint32_t held)
{
    static const char *const B[IN_N] = {"FSUP", "FSDN", "PUSH", "HOME", "FX", "MOD", "AMP", "CAB", "DLY", "REV"};
    static const char *const E[4] = {"down", "hold", "click", "release"};
    snprintf(ev + strlen(ev), 32, "%s.%s/%x ", B[b], E[e], (unsigned)held);
}
static void app_master(uint32_t vol) { vol_now = vol; vol_calls++; }

static int fails;
static uint32_t now = 1000u;
static void run(uint32_t ms) { while (ms--) in_poll(now++); }      /* the main loop, one pass a millisecond */
static void check(const char *want, const char *what)
{
    if (strcmp(ev, want)) { printf("FAIL %s\n     got  \"%s\"\n     want \"%s\"\n", what, ev, want); fails++; }
    ev[0] = 0;
}
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } } while (0)

/* a button's pins, as the pedal has them: (port, mask). Its footswitches, FX and HOME are on port A, the rest on port H */
typedef struct { uint32_t *port; uint32_t mask; } pin_t;
static const pin_t PIN[7] = { {&h_pa_in, 1u << 2}, {&h_pa_in, 1u << 3}, {&h_ph_in, 1u << 4}, {&h_pa_in, 1u << 1},
                              {&h_pa_in, 1u << 0}, {&h_ph_in, 1u << 7}, {&h_ph_in, 1u << 9} };
/* a pin's level when its button is up: high (the pull-ups), except FX's in this test, which idles low: "pressed" is a
 * pin that differs from its level at the start, whichever that was */
static int idle_high(uint32_t b) { return b != IN_FX; }
static void pin(uint32_t b, int down)
{
    if ((down != 0) == idle_high(b)) *PIN[b].port &= ~PIN[b].mask; else *PIN[b].port |= PIN[b].mask;
}
static void enc_step(int dir)                   /* one quadrature step of the encoder's two pins */
{
    static const uint8_t SEQ[4] = {0, 1, 3, 2};
    static uint32_t at;
    at = (at + (dir > 0 ? 1u : 3u)) & 3u;
    h_pc_in = (h_pc_in & ~3u) | SEQ[at];
    run(1);
}

int main(void)
{
    static const uint16_t HOLD[IN_N] = { [IN_PUSH] = 800, [IN_HOME] = 700, [IN_FSDN] = 500 };
    static const char *const NAME[7] = {"FSUP", "FSDN", "PUSH", "HOME", "FX", "MOD", "AMP"};
    char want[64];
    uint32_t b;

    /* the start: every button up, the keypad idle, MASTER where the volume starts (80) */
    h_pa_in = 0xFFFFu; h_ph_in = 0xFFFFu; h_adc[ADC_CH_KEYS] = 1023; h_adc[ADC_CH_MASTER] = 811; h_adc[8] = 582;
    pin(IN_FX, 0);
    input_init(); adc_pins_analog(); input_baseline();
    in_start(HOLD);
    run(300);
    check("", "nothing pressed: no event");
    CHECK(vol_calls == 1u && vol_now == 80u, "MASTER is told once after the start, even where the volume already was (80)");
    CHECK(bat_pct == 50, "the battery gauge was read (raw 582: 50 %)");
    h_adc[ADC_CH_MASTER] = 511; run(600);
    CHECK(vol_calls > 1u && vol_now >= 49u && vol_now <= 51u, "MASTER moved to the middle of its travel: about 50");

    /* each button's own pins give its own event; a press is debounced */
    for (b = 0; b < 7u; b++) {
        pin(b, 1); run(2); pin(b, 0); run(20);
        check("", "a contact shorter than the debounce: no event");
        pin(b, 1); run(20);
        snprintf(want, sizeof want, "%s.down/%x ", NAME[b], 1u << b);
        check(want, "a button down: its event at once, itself among the buttons held");
        pin(b, 0); run(20);
        snprintf(want, sizeof want, "%s.click/0 ", NAME[b]);
        check(want, "up again before any hold time: a click");
    }
    /* PUSH and MOD each have two pins: either one is the button */
    h_ph_in &= ~(1u << 6); run(20); h_ph_in |= 1u << 6; run(20);
    check("PUSH.down/4 PUSH.click/0 ", "PUSH's second pin");
    h_ph_in &= ~(1u << 8); run(20); h_ph_in |= 1u << 8; run(20);
    check("MOD.down/20 MOD.click/0 ", "MOD's second pin");

    /* the analog keypad: CAB, DLY and REV by their voltage */
    h_adc[ADC_CH_KEYS] = 617; run(60); check("CAB.down/80 ", "the keypad at CAB's voltage");
    h_adc[ADC_CH_KEYS] = 1023; run(60); check("CAB.click/0 ", "and idle again");
    h_adc[ADC_CH_KEYS] = 706 + 40; run(60); h_adc[ADC_CH_KEYS] = 1023; run(60);
    check("DLY.down/100 DLY.click/0 ", "DLY, 40 steps off its voltage");
    h_adc[ADC_CH_KEYS] = 788; run(60); h_adc[ADC_CH_KEYS] = 1023; run(60);
    check("REV.down/200 REV.click/0 ", "REV");
    h_adc[ADC_CH_KEYS] = 661; run(60); h_adc[ADC_CH_KEYS] = 1023; run(60);
    check("", "a voltage between two keys: no key");

    /* a hold: once, after the button's own time; the release that follows is not a click */
    pin(IN_PUSH, 1); run(700);
    check("PUSH.down/4 ", "PUSH down for 0.7 s: not held yet (its time is 0.8 s)");
    run(200);
    check("PUSH.hold/4 ", "0.9 s: held");
    run(2000);
    check("", "held on: no second hold");
    pin(IN_PUSH, 0); run(20);
    check("PUSH.release/0 ", "up after a hold: a release, not a click");
    pin(IN_FX, 1); run(3000); pin(IN_FX, 0); run(20);
    check("FX.down/10 FX.click/0 ", "a button without a hold time: a click however long it was down");

    /* two together: the second one's event names the first as held */
    pin(IN_FSUP, 1); run(20); pin(IN_FSDN, 1); run(20);
    check("FSUP.down/1 FSDN.down/3 ", "both footswitches: the second sees the first held");
    pin(IN_FSUP, 0); run(20); pin(IN_FSDN, 0); run(20);
    check("FSUP.click/2 FSDN.click/0 ", "and up, one after the other");

    /* the encoder: four steps a detent, slow and fast, both ways; a bounce is no detent. Its pins going 00, 01, 11, 10
     * count up, as in OpenPocket's main loop (openpocket.c: the same table, the same sign) */
    for (b = 0; b < 4u; b++) enc_step(+1);
    run(100);
    for (b = 0; b < 4u; b++) enc_step(+1);
    for (b = 0; b < 4u; b++) enc_step(+1);
    check("T+1 T+1 T+1f ", "three detents one way: the third within 45 ms of the second is fast");
    run(100);
    for (b = 0; b < 8u; b++) enc_step(-1);
    check("T-1 T-1f ", "two detents the other way");
    run(100);
    enc_step(+1); enc_step(-1); enc_step(+1); enc_step(-1);
    check("", "a contact that bounces between two steps: no detent");

    /* MASTER: its ends, and no report for a reading that only trembles */
    h_adc[ADC_CH_MASTER] = 516; run(600);
    b = vol_calls;
    h_adc[ADC_CH_MASTER] = 517; run(400); h_adc[ADC_CH_MASTER] = 515; run(400);
    CHECK(vol_calls == b, "MASTER trembling by a step or two: nothing new is told");
    h_adc[ADC_CH_MASTER] = 1023; run(600);
    CHECK(vol_now == 100u, "MASTER fully up: 100");
    h_adc[ADC_CH_MASTER] = 0; run(600);
    CHECK(vol_now == 0u, "MASTER fully down: 0");
    h_adc[ADC_CH_MASTER] = 261; run(600);
    snprintf(want, sizeof want, "a quarter of its travel: about 25 (it reads %u)", (unsigned)vol_now);
    CHECK(vol_now >= 23u && vol_now <= 26u, want);
    check("", "the knob gives no button event");

    printf(fails ? "base input: %d FAILURES\n" : "base input: ok (0 failures)\n", fails);
    return fails ? 1 : 0;
}
