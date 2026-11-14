/* SPDX-License-Identifier: GPL-3.0-only
 * The base firmware (docs/BASE-FIRMWARE.md): which product an image is. An app other than OpenPocket says so in two
 * places, from the one descriptor below, so that no tool takes it for OpenPocket:
 *   - in its image: the descriptor is found by its mark. tools/pxpkg.py reads it and names the packages and the
 *     manifest after it (starter-PocketX_901.fwsc, "app": {"id": "starter", ...});
 *   - on the pedal: it answers the INFO request of OpenPocket's editor protocol, F0 7D 4F 50 01 F7
 *     (docs/EDITOR-PROTOCOL.md), with protocol 0, which no OpenPocket has: "this is not OpenPocket", then who it is.
 *     The installer, the flasher and the web editor ask it (docs/pxota.js).
 * The identity of the update handshake stays Model_9NN (px_usb.h): every installer, old or new, finds the pedal and
 * can put OpenPocket back.
 *
 * The app's file defines OP_APP_ID (its folder's name under firmware/apps: lowercase letters, digits, "-"), OP_NAME and
 * OP_VERSION, includes px_usb.h and what it uses of base/store.h and base/midi.h, then this file. It may define
 *   #define APP_EDITOR my_frames     its own requests in the same framing (every command but INFO), protocol number
 *   #define APP_EDITOR_PROTO 1       APP_EDITOR_PROTO (1..127), which INFO reports:
 *   static void my_frames(uint32_t cmd, const uint8_t *args, uint32_t n)
 * and answers a request with ed_begin(cmd), ed_b / ed_str for each value, ed_send(), as px_editor.h does.
 * This file provides ed_service, which px_usb.h calls in every main-loop pass.
 * A host program that only reads descriptors (tests/devsim.c) defines OP_PRODUCT_READER: the types and
 * ed_info_reply, nothing of an image's own.
 */
#pragma once

#define OP_CAP_STEREO    1u                     /* its sound has a left and a right side (A_CH 2) */
#define OP_CAP_USB_AUDIO 2u                     /* it can be a USB sound card (OP_UAUDIO) */
#define OP_CAP_MIDI      4u                     /* it has a MIDI map (APP_MIDI) */
#define OP_CAP_STORE     8u                     /* it keeps its own data in flash (base/store.h) */
typedef struct {
    char mark[8];                               /* "OP-APP:" */
    char id[16], name[16], version[8];          /* 0-ended */
    uint8_t caps, editor, pad[2];               /* OP_CAP_*; the protocol of its own editor requests, 0 = none */
} op_app_t;                                     /* 52 bytes, once in an app's image */

#define ED_H0 0x7Du
#define ED_H1 0x4Fu
#define ED_H2 0x50u
#define ED_INFO 1u
static uint8_t ed_out[256];
static uint32_t ed_n;
static void ed_begin(uint32_t cmd)
{
    ed_out[0] = 0xF0; ed_out[1] = ED_H0; ed_out[2] = ED_H1; ed_out[3] = ED_H2; ed_out[4] = (uint8_t)cmd;
    ed_n = 5;
}
static void ed_b(uint32_t v) { if (ed_n < sizeof ed_out - 1u) ed_out[ed_n++] = (uint8_t)(v & 0x7Fu); }
static void ed_str(const char *s, uint32_t max)
{
    for (uint32_t i = 0; s && s[i] && i < max; i++) ed_b((uint8_t)s[i]);
    ed_b(0);
}
static void ed_send(void) { ed_out[ed_n++] = 0xF7; ota_wire_send(ed_out, ed_n); }

/* INFO from an app: protocol 0, its version, the update identity (Model_9NN), then its id, its name, what it can do
 * and its own editor protocol */
static void ed_info_reply(const volatile op_app_t *a, const char *identity)
{
    ed_begin(ED_INFO);
    ed_b(0); ed_str((const char *)a->version, 7); ed_str(identity, 27);
    ed_str((const char *)a->id, 15); ed_str((const char *)a->name, 15);
    ed_b(a->caps); ed_b(a->editor);
    ed_send();
}

#ifndef OP_PRODUCT_READER
#ifdef APP_EDITOR
static void APP_EDITOR(uint32_t cmd, const uint8_t *args, uint32_t n);
#ifndef APP_EDITOR_PROTO
#define APP_EDITOR_PROTO 1
#endif
#else
#define APP_EDITOR_PROTO 0
#endif
#ifndef A_CH
#define A_CH 1u
#endif
#ifndef OP_UAUDIO
#define OP_UAUDIO 0
#endif
#ifdef APP_MIDI
#define OP_CAP_MIDI_ OP_CAP_MIDI
#else
#define OP_CAP_MIDI_ 0u
#endif
#ifdef BS_MAX                                   /* (base/store.h was included) */
#define OP_CAP_STORE_ OP_CAP_STORE
#else
#define OP_CAP_STORE_ 0u
#endif
static const volatile op_app_t OP_APP __attribute__((used, aligned(4))) = {
    "OP-APP:", OP_APP_ID, OP_NAME, OP_VERSION,
    (uint8_t)((A_CH == 2 ? OP_CAP_STEREO : 0u) | (OP_UAUDIO ? OP_CAP_USB_AUDIO : 0u) | OP_CAP_MIDI_ | OP_CAP_STORE_),
    APP_EDITOR_PROTO, {0, 0}
};

static void ed_service(uint32_t now)            /* main loop, before ota_service: the frames with this header */
{
    static uint8_t f[160];
    const uint8_t *p;
    uint32_t n, k;
    (void)now;
    if (!ota_frame_get(&p, &n) || n < 4u || p[0] != ED_H0 || p[1] != ED_H1 || p[2] != ED_H2)
        return;                                 /* (another frame stays for ota_service) */
    k = n > sizeof f ? sizeof f : n;
    for (uint32_t i = 0; i < k; i++) f[i] = p[i];
    ota_frame_done();                           /* the frame buffer is free before the reply */
    if (f[3] == ED_INFO) ed_info_reply(&OP_APP, FELUCCA_ID_PTR);
#ifdef APP_EDITOR
    else APP_EDITOR(f[3], f + 4, k - 4u);
#endif
}
#endif
