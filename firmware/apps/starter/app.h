/* SPDX-License-Identifier: GPL-3.0-only
 * STARTER, the app itself (docs/ALT-FIRMWARE.md): its sounds, its presets, its screen, and what the controls, MIDI and
 * its own editor requests do. Copy this folder to start an alternative firmware; this file is where most of it goes.
 * The pedal's file (starter.c) and the simulator (tests/appsim.c, `node tools/op.mjs sim --app starter`) both include
 * it, after the base firmware's parts it uses: nothing here touches the chip.
 *
 * What a control does (app_turn, app_button, app_master: base/input.h):
 *   the knob            the selected setting; turned quickly, in steps of 5
 *   PUSH                the next setting; held 0.8 s: SAVE the sound to its slot
 *   MOD, AMP, CAB       LEVEL, WIDTH, PAN
 *   HOME                the effect off and on
 *   the footswitches    the preset before, the preset after
 *   MASTER              the volume
 * MIDI (app_midi: base/midi.h): Program Change 0..7 a preset; Control Change 7 LEVEL, 12 WIDTH, 10 PAN, 80 off / on;
 * the clock's tempo is shown.
 * Its own editor requests (app_editor: base/product.h), protocol 1, beside INFO:
 *   5 STATE   -> slot, flags (bit 0 the sound differs from its slot's, bit 1 the slot holds a saved sound), the
 *                selected setting, the sound (16 bytes: its name 12, LEVEL, WIDTH, PAN, on)
 *   6 SET k v -> k, v as taken: setting k (0 LEVEL, 1 WIDTH, 2 PAN, 3 on) = v
 */
#pragma once
#include "engine.h"

#define AP_NSLOT 8u
#define AP_MAGIC 0x52545350u                    /* "PSTR": this app's data in flash (base/store.h). Another app, another magic */
#define AP_VER   1u                             /* its format: a change of ap_store_t gets the next number, and ap_init reads the old */
typedef struct { char name[12]; uint8_t par[SE_NPAR], on; } ap_sound_t;                       /* 16 bytes */
typedef struct { uint8_t last, saved[AP_NSLOT], pad[3]; ap_sound_t slot[AP_NSLOT]; } ap_store_t;   /* what the flash keeps */
typedef char ap_size_check[sizeof(ap_sound_t) == 16 && sizeof(ap_store_t) <= BS_MAX ? 1 : -1];

static const ap_sound_t AP_FACTORY[AP_NSLOT] = {
    {"CENTRE", {50, 0, 50}, 1}, {"WIDE", {50, 40, 50}, 1}, {"WIDER", {50, 80, 50}, 1}, {"LEFT", {50, 0, 15}, 1},
    {"RIGHT", {50, 0, 85}, 1},  {"LOUD", {70, 20, 50}, 1}, {"QUIET", {30, 20, 50}, 1}, {"OFF", {50, 0, 50}, 0},
};
static const char *const AP_PAR[SE_NPAR] = {"LEVEL", "WIDTH", "PAN"};

static se_t ap_se OP_POOL;                       /* the engine's state, in the POOL region (memory.ld) */
static ap_store_t ap_st;
static ap_sound_t ap_now;                       /* the sound that plays */
static uint32_t ap_slot, ap_sel, ap_vol = 80u, ap_bpm;
static uint32_t ap_note_ms;                     /* a word in the header's place until then */
static const char *ap_note;
static midi_tempo_t ap_clk;

/* ---- the sound ---- */
static void app_sound(const float *in, float *out) { se_run(&ap_se, in, out); }   /* in the I2S interrupt (base/audio_plain.h) */
static void ap_apply(void) { se_set(&ap_se, ap_now.par, ap_now.on); }             /* a setting moved: the engine glides there */
static void app_sound_reset(void) { ap_apply(); se_reset(&ap_se); }
static int ap_dirty(void)
{
    const ap_sound_t *ref = ap_st.saved[ap_slot] ? &ap_st.slot[ap_slot] : &AP_FACTORY[ap_slot];
    return memcmp(ref, &ap_now, sizeof ap_now) != 0;
}
static void ap_load(uint32_t k)                 /* a preset: the slot's saved sound, else its factory one */
{
    ap_slot = k;
    ap_now = ap_st.saved[k] ? ap_st.slot[k] : AP_FACTORY[k];
    a_change(app_sound_reset);                  /* (silence, the line cleared, the new sound: nothing of the old one is heard) */
}
static void ap_set(uint32_t k, uint32_t v)      /* setting k (SE_NPAR: on) = v, clamped */
{
    if (k < SE_NPAR) ap_now.par[k] = (uint8_t)(v > 100u ? 100u : v);
    else ap_now.on = v ? 1u : 0u;
    ap_apply();
}
static void ap_save(void)
{
    ap_st.slot[ap_slot] = ap_now; ap_st.saved[ap_slot] = 1; ap_st.last = (uint8_t)ap_slot;
    ap_note = bs_save(AP_MAGIC, AP_VER, &ap_st, sizeof ap_st) ? "SAVE FAILED" : "SAVED";
    ap_note_ms = 1200u;
}

/* ---- the controls (base/input.h) ---- */
static const uint16_t AP_HOLD[IN_N] = { [IN_PUSH] = 800 };
static void app_turn(int dd, int fast)
{
    const int v = (int)ap_now.par[ap_sel] + dd * (fast ? 5 : 1);
    ap_set(ap_sel, (uint32_t)(v < 0 ? 0 : v));
}
static void app_button(uint32_t b, uint32_t ev, uint32_t held)
{
    (void)held;
    if (b == IN_PUSH && ev == IN_CLICK) ap_sel = (ap_sel + 1u) % SE_NPAR;
    if (b == IN_PUSH && ev == IN_HOLD) ap_save();
    if (ev != IN_DOWN) return;
    if (b == IN_FSUP) ap_load((ap_slot + 1u) % AP_NSLOT);
    if (b == IN_FSDN) ap_load((ap_slot + AP_NSLOT - 1u) % AP_NSLOT);
    if (b == IN_HOME) ap_set(SE_NPAR, !ap_now.on);
    if (b == IN_MOD) ap_sel = SE_LEVEL;
    if (b == IN_AMP) ap_sel = SE_WIDTH;
    if (b == IN_CAB) ap_sel = SE_PAN;
}
static void app_master(uint32_t vol) { ap_vol = vol; audio_volume(vol); }

/* ---- the MIDI map (base/midi.h) ---- */
static void app_midi(const midi_msg_t *m, uint32_t ms)
{
    const uint32_t v = ((uint32_t)m->d2 * 100u + 63u) / 127u;      /* 0..127 -> 0..100 */
    switch (m->type) {
    case MIDI_PC: if (m->d1 < AP_NSLOT) ap_load(m->d1); break;
    case MIDI_CC:
        if (m->d1 == 7u) ap_set(SE_LEVEL, v);
        else if (m->d1 == 12u) ap_set(SE_WIDTH, v);
        else if (m->d1 == 10u) ap_set(SE_PAN, v);
        else if (m->d1 == 80u) ap_set(SE_NPAR, m->d2 >= 64u);
        break;
    case MIDI_CLOCK: { const uint32_t q = midi_tempo(&ap_clk, ms); if (q) ap_bpm = (60000u + q / 2u) / q; break; }
    case MIDI_START: case MIDI_STOP: midi_tempo_reset(&ap_clk); ap_bpm = 0; break;
    }
}

/* ---- its own editor requests (base/product.h) ---- */
static void app_editor(uint32_t cmd, const uint8_t *a, uint32_t n)
{
    if (cmd == 5u) {
        ed_begin(cmd);
        ed_b(ap_slot); ed_b((ap_dirty() ? 1u : 0u) | (ap_st.saved[ap_slot] ? 2u : 0u)); ed_b(ap_sel);
        for (uint32_t i = 0; i < sizeof ap_now; i++) ed_b(((const uint8_t *)&ap_now)[i]);
        ed_send();
    } else if (cmd == 6u && n >= 2u && a[0] <= SE_NPAR) {
        ap_set(a[0], a[1]);
        ed_begin(cmd);
        ed_b(a[0]); ed_b(a[0] < SE_NPAR ? ap_now.par[a[0]] : ap_now.on);
        ed_send();
    }
}

/* ---- the screen: the preset, three tiles, a status line. Only what changed is drawn again ---- */
static void ap_draw(uint32_t now)
{
    static uint32_t seen[5] = {~0u, ~0u, ~0u, ~0u, ~0u}, t_status, t_last;
    char l[40];
    uint32_t h, k;
    if (ap_note_ms) { const uint32_t dt = now - t_last; ap_note_ms = ap_note_ms > dt ? ap_note_ms - dt : 0u; }
    t_last = now;
    h = ap_slot | (uint32_t)ap_dirty() << 8 | (uint32_t)ap_now.on << 9 | (ap_note_ms ? 1u << 10 : 0u) | (uint32_t)(uintptr_t)ap_note << 11;
    if (h != seen[0]) {
        seen[0] = h;
        l[0] = 0;
        if (ap_note_ms) b_str(l, ap_note);
        else { b_num(l, ap_slot + 1u); b_str(l, "  "); b_str(l, ap_now.name); if (ap_dirty()) b_str(l, " *"); }
        ft_box(0, 14, 240, l, &FT_MD, ap_note_ms ? UI_ACC : UI_TXT, UI_BG, 1);
        ft_box(0, 44, 240, ap_now.on ? OP_NAME " " OP_VERSION : "OFF: THE GUITAR AS IT IS", &FT_SM, ap_now.on ? UI_DIM : UI_ACC, UI_BG, 1);
    }
    for (k = 0; k < SE_NPAR; k++) {             /* a tile a setting: its name, its value; the selected one lit */
        const uint32_t x = 8u + k * 77u, sel = k == ap_sel;
        h = ap_now.par[k] | sel << 8;
        if (h == seen[1u + k]) continue;
        seen[1u + k] = h;
        lcd_rect(x, 76, 70, 72, sel ? UI_SEL : UI_PANEL);
        ft_box(x, 84, 70, AP_PAR[k], &FT_SM, sel ? UI_BG : UI_DIM, sel ? UI_SEL : UI_PANEL, 1);
        l[0] = 0; b_num(l, ap_now.par[k]);
        ft_box(x, 110, 70, l, &FT_MD, sel ? UI_BG : UI_TXT, sel ? UI_SEL : UI_PANEL, 1);
    }
    if (now - t_status >= 500u || seen[4] == ~0u) {   /* twice a second: the load, the volume, the clock's tempo */
        t_status = now ? now : 1u;
        l[0] = 0;
        b_str(l, base_safe ? "SAFE MODE  " : a_bypass ? "OVERLOAD  " : "DSP "); if (!base_safe && !a_bypass) { b_num(l, a_load); b_str(l, "%  "); }
        b_str(l, "VOL "); b_num(l, ap_vol);
        if (ap_bpm) { b_str(l, "  "); b_num(l, ap_bpm); b_str(l, " BPM"); }
        for (h = 0, k = 0; l[k]; k++) h = h * 31u + (uint8_t)l[k];
        if (h != seen[4]) {
            seen[4] = h;
            ft_box(0, 176, 240, l, &FT_SM, UI_TXT, UI_BG, 1);
            ft_box(0, 212, 240, "PUSH: NEXT   HOLD: SAVE   HOME: OFF", &FT_SM, UI_DIM, UI_BG, 1);
        }
    }
}

/* ---- the start and the main loop (starter.c's app_main, the simulator's) ---- */
static void app_init(void)                      /* after base_begin(): the saved sounds, the slot to start on. Interrupts off */
{
    ap_store_t t;
    uint32_t len = 0, ver = 0, k, i;
    int ok = bs_load(AP_MAGIC, &t, sizeof t, &len, &ver) && ver == AP_VER && len == sizeof t && t.last < AP_NSLOT;
    for (k = 0; ok && k < AP_NSLOT; k++) {      /* what the flash held is checked like anything from outside */
        for (i = 0; i < SE_NPAR; i++) if (t.slot[k].par[i] > 100u) ok = 0;
        if (t.saved[k] > 1u || t.slot[k].on > 1u || t.slot[k].name[11]) ok = 0;
        for (i = 0; i < 11u && t.slot[k].name[i]; i++) if (t.slot[k].name[i] < 32 || t.slot[k].name[i] > 126) ok = 0;
    }
    if (ok) ap_st = t;
    ap_slot = ap_st.last;
    ap_now = ap_st.saved[ap_slot] ? ap_st.slot[ap_slot] : AP_FACTORY[ap_slot];
    app_sound_reset();
    in_start(AP_HOLD);
    lcd_rect(0, 0, 240, 240, UI_BG);
}
static void app_step(uint32_t now)              /* every main-loop pass, after base_loop(now) */
{
    in_poll(now);
    ap_draw(now);
}
