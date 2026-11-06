/* The Bluetooth test image's status text and failure codes (firmware/ble/ble_status.h): which step failed, with which
 * return value, and the stage a timeout stalled in. The header has no hardware access, so it is compiled as is. */
#include <stdio.h>
#include <string.h>
#include "../firmware/ble/ble_status.h"
#include "../firmware/ble/ble_adv.h"

static int bad;
static void is(const char *what, const char *want)
{
    char b[48];
    ble_status_text(b);
    if (strcmp(b, want)) { printf("FAIL %s: \"%s\", wanted \"%s\"\n", what, b, want); bad++; }
    if (strlen(b) >= 24) { printf("FAIL %s: \"%s\" does not fit 24 bytes\n", what, b); bad++; }
}
static void reset(void) { ble_state = BLE_OFF; ble_stage = 0; ble_fail_step = 0; ble_fail_code = 0; }

int main(void)
{
    reset(); is("off", "OFF");
    ble_state = BLE_STARTING; ble_stage_set(BLE_STAGE_TASK); is("starting, task", "STARTING 1");
    ble_stage_set(BLE_STAGE_RF); is("starting, radio init", "STARTING 3");
    ble_stage_set(BLE_STAGE_READY); is("starting, waiting for the controller", "STARTING 4");

    reset(); ble_state = BLE_STARTING;
    if (ble_judge_start(0, 1) != 0 || ble_state != BLE_STARTING) { puts("FAIL: a good start was judged failed"); bad++; }
    if (ble_judge_start(-7, 0) != BLE_FAIL_INIT) { puts("FAIL: init error not judged as step 2"); bad++; }
    is("btctrler_task_init returned -7", "FAILED 2 -7");

    reset(); ble_state = BLE_STARTING;
    if (ble_judge_start(0, 0) != BLE_FAIL_READY) { puts("FAIL: not-ready not judged as step 3"); bad++; }
    is("btctrler_task_ready returned 0", "FAILED 3 0");

    reset(); ble_state = BLE_STARTING; ble_fail(BLE_FAIL_TASK_CREATE, -1); is("task_create failed", "FAILED 1 -1");
    reset(); ble_state = BLE_STARTING; ble_fail(BLE_FAIL_ADV_ENABLE, 12); is("ll_hci_adv_enable returned 12", "FAILED 4 12");

    reset(); ble_state = BLE_STARTING; ble_stage_set(BLE_STAGE_RF);
    ble_fail(BLE_FAIL_TIMEOUT, (int32_t)ble_stage); is("timeout in the radio init", "FAILED 5 3");

    reset(); ble_fail(BLE_FAIL_INIT, -2147483647 - 1); is("most negative code", "FAILED 2 -2147483648");
    reset(); ble_state = BLE_ADV; is("advertising", "ADVERTISING");
    /* the full line */
    reset(); ble_state = BLE_STARTING; ble_stage_set(BLE_STAGE_READY); ble_judge_start(-7, 0);
    { char f[96]; ble_status_full(f);
      if (strcmp(f, "FAILED 2 -7  step btctrler_task_init stage ready-wait")) { printf("FAIL full line: \"%s\"\n", f); bad++; }
      if (strlen(f) >= 96) { puts("FAIL: full line too long"); bad++; } }
    reset(); ble_state = BLE_STARTING; ble_stage_set(BLE_STAGE_RF); ble_fail(BLE_FAIL_TIMEOUT, (int32_t)ble_stage);
    { char f[96]; ble_status_full(f);
      if (strcmp(f, "FAILED 5 3  step timeout stage radio-init")) { printf("FAIL timeout line: \"%s\"\n", f); bad++; } }
    /* the marquee: "ABCDEFGH" in a window of 4, gap 3, rest 4 ticks */
    { static const struct { uint32_t tick; const char *want; } m[] = {
        {0, "ABCD"}, {3, "ABCD"},            /* the rest on the start */
        {4, "ABCD"}, {5, "BCDE"}, {8, "EFGH"},
        {9, "FGH "}, {11, "H   "}, {12, "   A"},   /* through the gap and round */
        {14, " ABC"},                        /* the last step of the cycle: one gap column left */
        {15, "ABCD"}, {19, "ABCD"},          /* a whole cycle (rest 4 + period 11 = 15 ticks) is back at the start */
      };
      int i; char o[8];
      for (i = 0; i < (int)(sizeof m / sizeof m[0]); i++) {
          ble_marquee("ABCDEFGH", m[i].tick, 4, o);
          if (strcmp(o, m[i].want)) { printf("FAIL marquee tick %u: \"%s\", wanted \"%s\"\n", m[i].tick, o, m[i].want); bad++; }
      }
      ble_marquee("ABCD", 7, 4, o);          /* fits: never moves */
      if (strcmp(o, "ABCD")) { puts("FAIL: a text that fits was scrolled"); bad++; } }
    reset(); ble_state = BLE_STARTING; ble_stage_set(BLE_STAGE_BB); ble_wait = 0x01C1A5E0u; ble_fail(BLE_FAIL_TIMEOUT, (int32_t)ble_stage);
    { char f[96]; ble_status_full(f); ble_wait = 0;
      if (strcmp(f, "FAILED 5 8  step timeout stage baseband-irq wait 01C1A5E0")) { printf("FAIL wait line: \"%s\"\n", f); bad++; } }
    /* the stall watch: silent while no poll runs, fires once a poll has run for more than BLE_STALL_TICKS ticks */
    { uint32_t i; int fired = 0;
      ble_busy(0); for (i = 0; i < 2u * BLE_STALL_TICKS; i++) fired |= ble_stall_tick();
      if (fired) { puts("FAIL: the stall watch fired with no poll running"); bad++; }
      ble_busy(1); for (i = 0; i < BLE_STALL_TICKS - 1u; i++) fired |= ble_stall_tick();
      if (fired) { puts("FAIL: the stall watch fired before its limit"); bad++; }
      ble_busy(0); ble_busy(1);                /* each poll starts the count again */
      for (i = 0; i < BLE_STALL_TICKS - 1u; i++) fired |= ble_stall_tick();
      if (fired) { puts("FAIL: the stall watch did not restart with the poll"); bad++; }
      if (!ble_stall_tick()) { puts("FAIL: the stall watch did not fire at its limit"); bad++; }
      ble_busy(0); }
    /* the proof counters: the advertising line, numbers first, registers in hex */
    { char f[96];
      ble_cnt_irq_ev = 12; ble_cnt_hci = 5; ble_cnt_irq_rx = 0; ble_cnt_timer = 340; ble_cnt_slot = 0;
      ble_adv_text(f, 0x2001u, 0x101u, 0u, 0x80u, 3u, 3u, 1u);
      if (strcmp(f, "ADV i12 e5 r0 t340 s0 CON 2001 IEN 101 IPND 0 STAT 80 IC45 3 IC29 3 BEN 1")) { printf("FAIL adv line: \"%s\"\n", f); bad++; }
      if (strlen(f) >= 96) { puts("FAIL: adv line too long"); bad++; }
      ble_cnt_irq_ev = 4000000000u; ble_adv_text(f, 0xFFFFFFFFu, 0, 0, 0, 0, 0, 0);
      if (strncmp(f, "ADV i2147483647 ", 16) || strlen(f) >= 96) { printf("FAIL big counter: \"%s\"\n", f); bad++; }
      ble_cnt_irq_ev = 0; ble_cnt_hci = 0;
      static const uint8_t evt[] = { 0x0E, 4, 1, 0x0A, 0x20, 0 };      /* a command complete event */
      ble_count_hci(4, evt); ble_count_hci(4, evt);
      if (ble_cnt_hci != 2 || ble_hci_last != 0x040Eu) { puts("FAIL: HCI packets are not counted"); bad++; }
      ble_adv_text(f, 0, 0, 0, 0, 0, 0, 0);
      if (strncmp(f, "ADV i0 e2 ", 10)) { printf("FAIL hci in the line: \"%s\"\n", f); bad++; }
      char w[16]; ble_marquee(f, 0, 11, w);
      if (strncmp(w, "ADV i0 e2 ", 10)) { printf("FAIL: the numbers are not first on screen: \"%s\"\n", w); bad++; } }
    /* the advertisement against the Core Specification (ble_adv.h) */
    { static const uint8_t uuid_le[16] = { 0x00,0xC7,0xC4,0x4E,0xE3,0x6C,0x51,0xA7,0x33,0x4B,0xE8,0xED,0x5A,0x0E,0xB8,0x03 };
      static const char name[] = "OpenPocket-BLE-TEST";
      unsigned n, i; int flags = 0, uuid = 0;
      for (n = 0; n < sizeof ble_adv_data; ) {                       /* length-type-value structures, no padding, no overrun */
          unsigned l = ble_adv_data[n];
          if (!l || n + 1u + l > sizeof ble_adv_data) { puts("FAIL: advertising data is not whole AD structures"); bad++; break; }
          if (ble_adv_data[n + 1] == 0x01 && l == 2 && ble_adv_data[n + 2] == 0x06) flags = 1;       /* general discoverable, no BR/EDR */
          if (ble_adv_data[n + 1] == 0x07 && l == 17 && !memcmp(ble_adv_data + n + 2, uuid_le, 16)) uuid = 1;
          n += 1u + l;
      }
      if (sizeof ble_adv_data > 31) { puts("FAIL: advertising data over 31 bytes"); bad++; }
      if (!flags || !uuid) { puts("FAIL: flags or the BLE-MIDI UUID missing from the advertising data"); bad++; }
      if (sizeof ble_scan_rsp > 31 || ble_scan_rsp[0] != sizeof ble_scan_rsp - 1u || ble_scan_rsp[1] != 0x09 ||
          ble_scan_rsp[0] != 1u + strlen(name) || memcmp(ble_scan_rsp + 2, name, strlen(name))) { puts("FAIL: scan response is not the complete local name"); bad++; }
      for (i = 0, n = 0; i < 6; i++) n |= ble_adv_mac[i];
      if (!n || (ble_adv_mac[5] & 1u)) { puts("FAIL: address is zero or a group address"); bad++; }
      if (BLE_ADV_SCAN_IND != 2 || BLE_ADV_INTERVAL < 0x00A0 || BLE_ADV_INTERVAL > 0x4000 || BLE_ADV_CHMAP != 7 || BLE_ADV_FILTER != 0) {
          puts("FAIL: advertising parameters out of range"); bad++; } }
    puts(bad ? "ble status: FAILED" : "ble status: ok");
    return bad != 0;
}
