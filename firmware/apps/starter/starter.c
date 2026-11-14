/* SPDX-License-Identifier: GPL-3.0-only
 * STARTER — an alternative firmware to start from (docs/ALT-FIRMWARE.md). It is a whole small product on the base
 * firmware (docs/BASE-FIRMWARE.md): a stereo sound from its own engine, presets in its own flash, the pedal's
 * controls, a MIDI map, and an identity that no tool takes for OpenPocket. Copy this folder, rename it, and replace
 * app.h and engine.c with your own.
 *   node tools/op.mjs app starter        ->  build/apps/starter/ (the image, its two packages, the manifest)
 *   node tools/op.mjs sim --app starter  ->  the same app.h and engine.c on this computer (docs/SIMULATOR.md)
 * This file is the pedal's side only: what the app declares to the base, and the order of the base's parts. The app
 * itself is app.h.
 */
#include <stdint.h>
#define AUDIO_PROBE 0
#define DSP_CPU1 0                      /* one core: the sound is made in the I2S interrupt (base/audio_plain.h) */
#define OP_APP_ID "starter"             /* this folder's name: the product's id in its packages and for the tools */
#define OP_NAME "STARTER"               /* its name: the boot screen, the installer */
#define OP_VERSION_NUM "01"             /* the package's number: PocketX_901, BlackBox_901 (firmware/build.sh, px_usb.h) */
#define OP_VERSION "v" OP_VERSION_NUM
#define A_CH 2                          /* its sound has a left and a right side (base/audio.h) */
#include "base/board.h"                 /* the chip, the pins, the screen and its text */
#include "base/controls.h"              /* the controls' pins, the ADC, the MASTER knob */
#include "base/diag.h"                  /* the crash screen, the boot stages kept across a reset */
#include "base/audio.h"                 /* the I2S, a_input, dsp_out, dsp_load, a_output */
static void app_sound(const float *in, float *out);   /* app.h */
static void app_sound_reset(void);
#include "base/audio_plain.h"           /* a_block, a_send, a_collect: app_sound in the interrupt; a_change */
#define OP_FLASH_OWN(off, n) BS_FLASH_OWN(off, n)     /* the flash it may write: its store's (base/flashmap.h) */
#define APP_MIDI app_midi               /* its MIDI map (app.h) */
#define APP_EDITOR app_editor           /* its own editor requests (app.h), protocol 1 */
#include "px_usb.h"                     /* USB-MIDI and the update entry: the installer, the way back to OpenPocket */
#include "base/store.h"                 /* its data in flash */
#include "base/midi.h"                  /* the MIDI ring read, each message to app_midi */
#include "base/input.h"                 /* the controls as events: app_turn, app_button, app_master */
#include "base/product.h"               /* the product's descriptor, INFO, app_editor's frames */
#include "base/loop.h"                  /* base_begin, base_go, base_loop */
#include "app.h"                        /* the app */
#include "base/start.h"                 /* fm1_cstart: it calls app_main, below */

static void app_main(void)
{
    base_begin();                        /* the screen, the boot screens, the controls, the flash */
    app_init();                          /* its saved sounds (the POOL region is not zeroed: it clears its engine) */
    base_go();                           /* USB, then guitar -> app_sound -> output */
    for (;;) {
        const uint32_t now = ms_now();
        base_loop(now);                  /* the watchdog, the installer, the boot guard, the MIDI ring */
        app_step(now);                   /* the controls, the screen */
    }
}
