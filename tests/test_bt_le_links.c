/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket: the Bluetooth audio image's two LE links (firmware/bt/bt_le_links.h, test build 27) on a PC: which link a handle
 * is from the host's events, each event taken as often as the host delivers it (up to three callbacks), and when advertising is
 * wanted: while a link is free and none is asked for; never with both links up; again after either ends.
 *   cc -O1 -Wall -Ifirmware/bt -o build/test_bt_le_links tests/test_bt_le_links.c && build/test_bt_le_links */
#include <stdio.h>
#include <string.h>
#include "bt_le_links.h"

static int failed;
static void ok(int c, const char *what) { printf("%-76s %s\n", what, c ? "ok" : "FAIL"); if (!c) failed++; }

/* bt_le_midi.c's way through an event: a link made or ended, then advertising if wanted (asked -> adv set) */
static int asked;
static void adv_more(le_links_t *s) { if (le_links_want_adv(s)) { asked++; s->adv = 1; } }
static int made_as(le_links_t *s, uint32_t h, uint32_t intv, int times, uint32_t role)
{
    int i = -1, k;
    for (k = 0; k < times; k++) { const int r = le_links_made(s, h, intv, role); if (r >= 0) { i = r; adv_more(s); } }
    return i;
}
static int made(le_links_t *s, uint32_t h, uint32_t intv, int times) { return made_as(s, h, intv, times, LE_ROLE_PERIPH); }
static int ended(le_links_t *s, uint32_t h, int times)
{
    int i = -1, k;
    for (k = 0; k < times; k++) { const int r = le_links_ended(s, h); if (r >= 0) { i = r; adv_more(s); } }
    return i;
}

int main(void)
{
    le_links_t s;
    memset(&s, 0, sizeof s);
    adv_more(&s);
    ok(asked == 1 && s.adv && !le_links_up(&s), "start: no link, advertising asked once");
    adv_more(&s);
    ok(asked == 1, "asked again while it runs: nothing");
    ok(made(&s, 0x50, 12, 3) == 0 && s.made == 1 && le_links_up(&s) == 1 && s.intv[0] == 12,
       "the Chocolate connects (the event three times): link 0, counted once");
    ok(asked == 2 && s.adv, "its link ended that advertising: asked again, for the second link");
    ok(made(&s, 0x51, 24, 2) == 1 && le_links_up(&s) == 2 && s.made == 2 && asked == 2 && !le_links_want_adv(&s),
       "the editor's phone connects: link 1; both up, no advertising");
    le_links_update(&s, 0x51, 12);
    le_links_update(&s, 0x77, 6);
    ok(s.intv[1] == 12 && s.intv[0] == 12, "an update moves its link's interval; an unknown handle's moves nothing");
    ok(le_links_find(&s, 0x50) == 0 && le_links_find(&s, 0x51) == 1 && le_links_find(&s, 0x30) < 0 && le_links_find(&s, 0) < 0,
       "handles to links; the classic link's (0x30) and 0 are none");
    ok(ended(&s, 0x30, 1) < 0 && le_links_up(&s) == 2 && asked == 2, "the phone's classic link ends: not ours, nothing asked");
    ok(ended(&s, 0x50, 3) == 0 && le_links_up(&s) == 1 && asked == 3 && s.adv, "the Chocolate goes (three times): advertising asked once");
    ok(ended(&s, 0x51, 1) == 1 && !le_links_up(&s) && asked == 3, "the editor goes too: advertising already on, not asked again");
    ok(made(&s, 0x52, 12, 1) == 0 && asked == 4 && made(&s, 0x53, 12, 1) == 1 && asked == 4,
       "two new links: link 0 and 1 again, advertising asked only between them");
    ok(made(&s, 0x54, 12, 1) < 0 && le_links_up(&s) == 2, "a third (the controller has no entity for it): refused");
    s.adv = 0;                                          /* (an enable that failed: bt_le_midi.c leaves adv clear, retries) */
    ok(ended(&s, 0x53, 1) == 1 && asked == 5 && s.adv, "a link ends after a failed ask: asked again");

    /* ---- test build 28: the central role on, one link for the footswitch (the pedal its central), one for a phone */
    memset(&s, 0, sizeof s); asked = 0; s.cen = 1;
    adv_more(&s);
    ok(asked == 1 && le_links_want_scan(&s), "central role: at start advertising and scanning (two entities)");
    ok(made_as(&s, 0x60, 24, 3, LE_ROLE_CENTRAL) == 0 && s.role[0] == LE_ROLE_CENTRAL && !le_links_want_scan(&s) && s.adv && asked == 1,
       "central role: the footswitch linked (the event three times): no scanning, advertising goes on, not asked again");
    ok(made_as(&s, 0x61, 12, 2, LE_ROLE_PERIPH) == 1 && !s.adv && !le_links_want_adv(&s) && asked == 1,
       "central role: a phone connects: both links up, no advertising");
    ok(ended(&s, 0x61, 2) == 1 && asked == 2 && s.adv && !le_links_want_scan(&s), "central role: the phone goes: advertising again, the footswitch stays");
    ok(ended(&s, 0x60, 1) == 0 && le_links_want_scan(&s) && asked == 2, "central role: the footswitch goes: scanning again (advertising already on)");
    ok(made_as(&s, 0x62, 12, 1, LE_ROLE_PERIPH) == 0 && !s.adv && !le_links_want_adv(&s) && le_links_want_scan(&s) && asked == 2,
       "central role: a phone first: its link and scanning, no advertising for a second phone");
    ok(made_as(&s, 0x63, 24, 1, LE_ROLE_CENTRAL) == 1 && le_links_count(&s, LE_ROLE_CENTRAL) == 1 && le_links_count(&s, LE_ROLE_PERIPH) == 1,
       "central role: then the footswitch: one link of each role");
    s.cen = 0;
    ok(ended(&s, 0x63, 1) == 1 && asked == 3 && !le_links_want_scan(&s), "role off (DLY): a free link is advertised for, nothing scanned, as test build 27");
    printf("\n%s\n", failed ? "LE link tests FAILED" : "all LE link tests passed");
    return failed ? 1 : 0;
}
