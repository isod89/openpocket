/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — a simulated pedal for the web editor tests (tests/test_editor.mjs).
 *
 * It runs, natively on the PC, the pedal's own editor code (firmware/px_editor.h), preset store (firmware/px_store.h)
 * and engine catalogue (engine/px_dsp.c), on a 1 MiB flash array. As tests/devsim.c: MIDI SysEx through
 * stdin/stdout, one message per line in hex; '#' lines are for the harness:
 *   out  "#port NAME"            the USB port appears
 *   in   "#pedal set B K V"      a knob turned on the pedal: parameter K of block B = V
 *   in   "#pedal on B V"         a block switched on/off on the pedal
 *   in   "#pedal preset N"       a footswitch: slot N (0..51)
 *   in   "#pedal focus B"        a block button
 *   in   "#dump"                 prints "#state SLOT|NAME|WRITES_OK|FOCUS|ON|TYPE|AMP P|ORDER" (the current sound)
 * Built with -DOP_BLE_TEST=1 it also answers DIAG, as the Bluetooth test image does, from the real text code (ble_diag.h):
 *   in   "#ble adv I T"          advertising; the counters i and t
 *   in   "#ble failed S C"       FAILED, step S, code C
 *   in   "#ble safe"             the boot after a watchdog reset in the first 6 s: safe mode, Bluetooth off
 *   edsim [FLASH.bin] [--dump OUT.bin]
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include "../engine/px_dsp.h"

static uint8_t nor[0x100000];
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && (uint32_t)(n) <= (hi) - (uint32_t)(off))
#define OP_VERSION "v36"
static const char *FELUCCA_ID_PTR = "PocketX_936";

static uint32_t ms_now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
}
static uint32_t ota_crc16(const uint8_t *p, uint32_t n, uint32_t c)   /* as ota.c: poly 0x1021, init 0 */
{
    uint32_t i;
    while (n--) {
        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8u; i++) c = c & 0x8000u ? (c << 1) ^ 0x1021u : c << 1;
        c &= 0xFFFFu;
    }
    return c;
}

/* ---- flash (only the preset store may be written, as FL_RANGE_OK on the pedal) ---- */
static int writes_ok = 1;
static int ota_fread(uint32_t off, void *p, uint32_t n) { if (off + n > sizeof nor) return -1; memcpy(p, nor + off, n); return 0; }
static int fl_erase4k(uint32_t off, uint32_t *took)
{
    *took = 0;
    if (!FL_IN(off, 0x1000u, 0xF0000u, 0xF4000u) || (off & 0xFFFu)) { writes_ok = 0; return -1; }
    memset(nor + off, 0xFF, 0x1000u);
    return 0;
}
static int fl_write(uint32_t off, const void *p, uint32_t n)
{
    const uint8_t *s = p;
    if (!FL_IN(off, n, 0xF0000u, 0xF4000u)) { writes_ok = 0; return -1; }
    for (uint32_t i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}

/* ---- what px_store.h's audio hold touches on the pedal ---- */
static volatile uint8_t a_mute;
static void fm1_wdt_feed(void) { }
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#define FM1_IRQ_ALNK0 0
static void fm1_irq_mask(int i) { (void)i; }
static void fm1_irq_unmask(int i) { (void)i; }
static void a_collect(void) { }
#define ST_IRQ_OFF()
static uint8_t px_flash_ok = 1;

/* ---- the pedal's UI state (openpocket.c, ui.h) ---- */
static px_preset_t PRESET;
static uint32_t cur_preset;
static void st_get(uint32_t k, px_preset_t *p);
static void load_preset(uint32_t i) { st_get(i, &PRESET); }
enum { M_CHAIN, M_BLOCK, M_EDIT, M_TUNER, M_SAVE, M_MOVE };
static uint32_t st_gen, ui_mode = M_CHAIN, ui_blk = PX_B_AMP, ui_item;
#include "../firmware/ui_colors.h"
static uint32_t ed_load(void) { return 42u; }
static uint32_t ed_flags(void) { return 2u; }

/* ---- SysEx in/out ---- */
static uint8_t frame[2048];
static uint32_t frame_len, frame_full;
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!frame_full) return 0; *p = frame; *n = frame_len; return 1; }
static void ota_frame_done(void) { frame_full = 0; }
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
    fflush(stdout);
    return 0;
}

#include "../firmware/px_midi.h"
#include "../firmware/px_store.h"
#if OP_BLE_TEST
#include "../firmware/ble/ble_diag.h"
#define ED_DIAG_LINES 4u
#define ED_DIAG_LEN 224u
static uint32_t sim_safe, sim_rst = 0x40u;
static void ed_diag_line(uint32_t i, char *out)
{
    if (i == 0u) ble_boot_text(out, 30000u, sim_safe, 8u, sim_rst, 0u);
    else if (i == 1u) ble_last_text(out, &ble_last);
    else if (i == 3u) { static const uint8_t last[3] = {2, 24, 127}, peer[6] = {1, 2, 3, 4, 5, 6}; ble_link_text(out, 1u, 2u, peer, 48u, 0u, 400u, 7u, 1u, 0x13u, last, 3u, "lmp_hci_reset"); }
    else if (ble_state == BLE_ADV) ble_adv_text(out, 7u, 0xA3u, 0x12ABu, 0x2001u, 0x101u, 0u, 0x80u, 3u, 3u, 1u);
    else ble_status_full(out);
}
static void sim_ble(const char *l)
{
    int a, b;
    if (sscanf(l, "adv %d %d", &a, &b) == 2) { ble_state = BLE_ADV; ble_cnt_irq_ev = (uint32_t)a; ble_cnt_timer = (uint32_t)b; }
    else if (sscanf(l, "failed %d %d", &a, &b) == 2) ble_fail((uint32_t)a, b);
    else if (!strcmp(l, "safe")) { ble_state = BLE_OFF; sim_safe = 1; sim_rst = 4u; ble_last = (ble_boot_t){ .failed = 1, .stage = 7, .blocks = 9 }; }
}
#endif
#include "../firmware/px_editor.h"

static int hexv(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static int closed;
static void pedal(const char *l)
{
    int b, k, v;
    if (sscanf(l, "set %d %d %d", &b, &k, &v) == 3 && b >= 0 && b < PX_NBLK && k >= 0 && k < PX_NPAR) PRESET.p[b][k] = (uint8_t)v;
    else if (sscanf(l, "on %d %d", &b, &v) == 2 && b >= 0 && b < PX_NBLK) PRESET.on[b] = (uint8_t)(v != 0);
    else if (sscanf(l, "preset %d", &v) == 1 && v >= 0 && v < (int)ST_NSLOT) { cur_preset = (uint32_t)v; load_preset(cur_preset); }
    else if (sscanf(l, "focus %d", &b) == 1 && b >= 0 && b < PX_NBLK) { ui_blk = (uint32_t)b; ui_mode = M_BLOCK; }
}
static void poll_in(uint32_t ms)
{
    static char line[8192];
    static size_t ll;
    fd_set fs;
    struct timeval tv = {0, (long)ms * 1000};
    if (frame_full || closed) return;
    FD_ZERO(&fs);
    FD_SET(0, &fs);
    if (select(1, &fs, 0, 0, &tv) <= 0) return;
    for (;;) {
        char c;
        ssize_t r = read(0, &c, 1);
        if (r <= 0) { closed = 1; return; }
        if (c == '\n') break;
        if (ll < sizeof line - 1) line[ll++] = c;
    }
    line[ll] = 0;
    ll = 0;
    if (line[0] == '#') {
        if (!strncmp(line, "#pedal ", 7)) pedal(line + 7);
#if OP_BLE_TEST
        else if (!strncmp(line, "#ble ", 5)) sim_ble(line + 5);
#endif
        else if (!strcmp(line, "#dump")) {           /* "#state SLOT|NAME|WRITES_OK|FOCUS|ON..|TYPE..|P(AMP)..|ORDER.." */
            int i;
            printf("#state %u|%s|%d|%u|", cur_preset, PRESET.name, writes_ok, ui_blk);
            for (i = 0; i < PX_NBLK; i++) printf("%d%s", PRESET.on[i], i < PX_NBLK - 1 ? "," : "|");
            for (i = 0; i < PX_NBLK; i++) printf("%d%s", PRESET.type[i], i < PX_NBLK - 1 ? "," : "|");
            for (i = 0; i < PX_NPAR; i++) printf("%d%s", PRESET.p[PX_B_AMP][i], i < PX_NPAR - 1 ? "," : "|");
            for (i = 0; i < PX_NBLK; i++) printf("%d%s", PRESET.order[i], i < PX_NBLK - 1 ? "," : "\n");
            fflush(stdout);
        }
        return;
    }
    {
        uint32_t n = 0, i;
        uint8_t m[4096];
        for (i = 0; line[i] && line[i + 1] && n < sizeof m; i += 2) m[n++] = (uint8_t)(hexv(line[i]) << 4 | hexv(line[i + 1]));
        if (n >= 2 && m[0] == 0xF0 && m[n - 1] == 0xF7 && n - 2 <= sizeof frame) {   /* usb.c: the bytes between F0 and F7 */
            memcpy(frame, m + 1, n - 2);
            frame_len = n - 2;
            frame_full = 1;
        }
    }
}

int main(int argc, char **argv)
{
    const char *dump = 0;
    memset(nor, 0xFF, sizeof nor);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        else { FILE *f = fopen(argv[i], "rb"); if (f) { if (fread(nor, 1, sizeof nor, f)) { } fclose(f); } }
    }
    st_ok = 1;
    st_load();
    cur_preset = st_last;
    load_preset(cur_preset);
    printf("#port OpenPocket\n");
    fflush(stdout);
    while (!closed) {
        poll_in(5);
        ed_service(ms_now());
    }
    if (dump) { FILE *f = fopen(dump, "wb"); if (f) { fwrite(nor, 1, sizeof nor, f); fclose(f); } }
    return 0;
}
