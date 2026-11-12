/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket: the Bluetooth audio image's LE links, two at once (test build 27: the Chocolate on one, the web editor on the
 * other, beside the phone's classic music link): which link a handle is, from the host's events, and when advertising is
 * wanted. No hardware here: tests/test_bt_le_links.c runs this file on a PC; bt/bt_le_midi.c drives it from the host's task.
 *
 * The controller has config_btctler_le_hw_nums (bt_cfg.c) radio entities. Advertising takes one; a central's connection
 * request turns that one into the link and ends the advertising (link_layer.c, ll_adv.c __adv_rx_post_handler); advertising
 * again takes a free one. So with two: one link and advertising, or two links. The host delivers some events to each of the
 * callbacks bt_le_midi.c registers, so every step here takes the same event twice as once.
 *
 * Test build 28: the pedal may also be a link's central (cen: the central role is on), for a footswitch that is only a
 * peripheral (the regular M-VAVE Chocolate): it scans for it and connects to it (bt_le_central.h). Scanning takes an entity
 * too, and connecting turns it into the link as advertising does. With the role on, one link is the footswitch's (role
 * LE_ROLE_CENTRAL: the pedal is its central) and one a phone's or a computer's (LE_ROLE_PERIPH: the pedal is the
 * peripheral, the editor), so the two entities give: advertising and scanning, the footswitch's link and advertising, a
 * phone's link and scanning, or both links. With the role off (the DLY key at power-on) both links are peripheral links, as
 * in test build 27. */
#pragma once
#include <stdint.h>

#define LE_LINKS 2u
enum { LE_ROLE_PERIPH = 0, LE_ROLE_CENTRAL = 1 };               /* (the role byte of LE Connection Complete: 1 slave, 0 master) */
typedef struct {
    uint16_t conn[LE_LINKS];      /* each link's handle, 0: free */
    uint16_t intv[LE_LINKS];      /* its connection interval, 1.25 ms */
    uint8_t role[LE_LINKS];       /* LE_ROLE_PERIPH: a central (a phone, the editor) connected to the pedal; LE_ROLE_CENTRAL:
                                   * the pedal connected to a peripheral (the footswitch) */
    uint8_t adv;                  /* advertising was asked for and no link has ended it since */
    uint8_t cen;                  /* test build 28: the central role is on (one link for the footswitch) */
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
static uint32_t le_links_count(const le_links_t *s, uint32_t role)   /* the links up in that role */
{
    uint32_t i, n = 0;
    for (i = 0; i < LE_LINKS; i++) n += s->conn[i] != 0u && s->role[i] == role;
    return n;
}
/* LE Connection Complete (status 0): the new link's index; -1 for the same event again, or with no room (never: the controller
 * has no third entity). A peripheral link ends the advertising that made it */
static int le_links_made(le_links_t *s, uint32_t h, uint32_t intv, uint32_t role)
{
    uint32_t i;
    if (!h || le_links_find(s, h) >= 0) return -1;
    if (role == LE_ROLE_PERIPH) s->adv = 0;
    for (i = 0; i < LE_LINKS; i++)
        if (!s->conn[i]) { s->conn[i] = (uint16_t)h; s->intv[i] = (uint16_t)intv; s->role[i] = (uint8_t)role; s->made++; return (int)i; }
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
/* advertising is wanted: a peripheral link is free and none is asked for (the caller asks, then sets adv). With the central
 * role on, one link is kept for the footswitch: one peripheral link at most */
static int le_links_want_adv(const le_links_t *s)
{
    return !s->adv && (s->cen ? le_links_count(s, LE_ROLE_PERIPH) < LE_LINKS - 1u : le_links_up(s) < LE_LINKS);
}
/* test build 28: scanning for the footswitch is wanted: the central role is on and its link is not up */
static int le_links_want_scan(const le_links_t *s) { return s->cen && !le_links_count(s, LE_ROLE_CENTRAL); }

/* test build 28: the LE side's watchdog (bt_le_midi.c bt_le_watch, from the main loop). irqs is the LE baseband's interrupts
 * counted so far, on_air whether something should be on air (advertising, scanning or a link). 1 when they have not moved for
 * limit ms while something should be: then again after each further limit ms of silence */
typedef struct { uint32_t seen, ms; } le_watch_t;
static int le_watch_due(le_watch_t *w, uint32_t irqs, uint32_t now, int on_air, uint32_t limit)
{
    if (irqs != w->seen || !on_air) { w->seen = irqs; w->ms = now; return 0; }
    if (now - w->ms < limit) return 0;
    w->ms = now;
    return 1;
}
