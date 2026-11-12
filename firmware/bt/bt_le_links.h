/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket: the Bluetooth audio image's LE links, two at once (test build 27: the Chocolate on one, the web editor on the
 * other, beside the phone's classic music link): which link a handle is, from the host's events, and when advertising is
 * wanted. No hardware here: tests/test_bt_le_links.c runs this file on a PC; bt/bt_le_midi.c drives it from the host's task.
 *
 * The controller has config_btctler_le_hw_nums (bt_cfg.c) radio entities. Advertising takes one; a central's connection
 * request turns that one into the link and ends the advertising (link_layer.c, ll_adv.c __adv_rx_post_handler); advertising
 * again takes a free one. So with two: one link and advertising, or two links. The host delivers some events to each of the
 * callbacks bt_le_midi.c registers, so every step here takes the same event twice as once. */
#pragma once
#include <stdint.h>

#define LE_LINKS 2u
typedef struct {
    uint16_t conn[LE_LINKS];      /* each link's handle, 0: free */
    uint16_t intv[LE_LINKS];      /* its connection interval, 1.25 ms */
    uint8_t adv;                  /* advertising was asked for and no link has ended it since */
    uint32_t made;                /* links made since power-on */
} le_links_t;

static int le_links_find(const le_links_t *s, uint32_t h)     /* the link with this handle, or -1 */
{
    uint32_t i;
    for (i = 0; h && i < LE_LINKS; i++) if (s->conn[i] == h) return (int)i;
    return -1;
}
static uint32_t le_links_up(const le_links_t *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < LE_LINKS; i++) n += s->conn[i] != 0u;
    return n;
}
/* LE Connection Complete (status 0): the new link's index; -1 for the same event again, or with no room (never: the controller
 * has no third entity). The advertising that made it has ended */
static int le_links_made(le_links_t *s, uint32_t h, uint32_t intv)
{
    uint32_t i;
    if (!h || le_links_find(s, h) >= 0) return -1;
    s->adv = 0;
    for (i = 0; i < LE_LINKS; i++)
        if (!s->conn[i]) { s->conn[i] = (uint16_t)h; s->intv[i] = (uint16_t)intv; s->made++; return (int)i; }
    return -1;
}
static void le_links_update(le_links_t *s, uint32_t h, uint32_t intv)   /* LE Connection Update Complete */
{
    const int i = le_links_find(s, h);
    if (i >= 0) s->intv[i] = (uint16_t)intv;
}
/* Disconnection Complete: the link's index; -1 when the handle is not an LE link of ours (the classic link's, or the same
 * event again) */
static int le_links_ended(le_links_t *s, uint32_t h)
{
    const int i = le_links_find(s, h);
    if (i >= 0) { s->conn[i] = 0; s->intv[i] = 0; }
    return i;
}
/* advertising is wanted: a link is free and none is asked for (the caller asks, then sets adv) */
static int le_links_want_adv(const le_links_t *s) { return !s->adv && le_links_up(s) < LE_LINKS; }
