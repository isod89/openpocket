/* SPDX-License-Identifier: GPL-3.0-only
 * The simulated pedal of an app on the base firmware: firmware/base/start.h's stand-in (tests/host/base/board.h). It is
 * the program's main(): it sets the simulated pedal up, then calls the app's own app_main(), which runs the pedal's
 * main loop for good. The host of the simulator (tools/sim.mjs, a test) talks to it as it talks to tests/edsim.c:
 * a MIDI message a line, in hex, both ways; and lines that start with '#':
 *   out  "#port NAME"            the USB port appears
 *   in   "#down NAME"            a button goes down / comes up: fs_up fs_dn push home fx mod amp cab dly rev.
 *   in   "#up NAME"              "#down ok" / "#up ok" / "... err REASON"
 *   in   "#press NAME"           down, 60 ms, up; NAME_long holds it for 1.2 s. "#press ok"
 *   in   "#knob N [fast]"        the knob turned N detents (negative: the other way), 60 ms apart, or 10 ms with fast
 *   in   "#vol N"                the MASTER knob, 0..100
 *   in   "#nan"                  the app's sound gives a NaN in its next block (base/audio.h's safety net, base/loop.h). "#nan ok"
 *   in   "#wait MS"              MS milliseconds of the pedal's time pass at once (the main loop runs MS passes). "#wait ok"
 *   in   "#sound HZ"             a sine of HZ at a quarter of full scale as the guitar (0: silence); after 0.4 s:
 *                                "#sound L R", the two outputs' RMS over the last 0.1 s, in 1/10000 of full scale
 *   in   "#status"               one JSON line: the product (its INFO descriptor), the screen's frame count, the volume,
 *                                the load, safe mode, whether every flash write was inside the app's own range, the
 *                                boot stage reached and the boot guard's "start not finished yet" mark
 *   in   "#screen"               "#screen " + 240*240 RGB565 pixels, 4 hex digits each, row by row
 *   in   "#flash PATH"           writes the 1 MiB flash to PATH
 * The pins move as a hand moves them and the pedal's own code does the rest (base/controls.h, base/input.h): a press
 * is debounced, a hold is timed. A command that takes time is answered when it is over; the main loop runs meanwhile,
 * 1 ms a pass.
 *   PROGRAM [FLASH.bin] [--persist FILE] [--dump OUT.bin] [--failed-start]
 * --persist FILE: the flash lives in FILE (read at the start when it exists, written again after each change);
 * --dump OUT.bin: the flash is written to OUT.bin when the input closes;
 * --failed-start: the start before this one did not finish, so this one is in safe mode (base/loop.h).
 */
#pragma once
#include <math.h>
#include <sys/select.h>
#include <unistd.h>

static void app_main(void);

/* the I2S driver (firmware/sdk/iis.bc on the pedal): nothing to open here, h_tick calls its two callbacks */
int  iis_open(struct iis_platform_data *pd, uint8_t index) { (void)pd; (void)index; return 0; }
int  iis_set_sample_rate(int sample_rate, uint8_t index) { (void)sample_rate; (void)index; return 0; }
void iis_channel_on(uint8_t channel, uint8_t index) { (void)channel; (void)index; }
void iis_set_enc_data_handler(void *priv, void (*cb)(void *, uint8_t *, int, uint8_t), uint8_t index) { (void)priv; (void)cb; (void)index; }
void iis_set_dec_data_handler(void *priv, void (*cb)(void *, uint8_t *, int, uint8_t), uint8_t index) { (void)priv; (void)cb; (void)index; }
void iis_irq_handler(uint8_t index) { (void)index; }

/* ---- time and the audio's interrupt ---- */
static uint32_t h_aud_us, h_tone_hz, h_tone_at, h_lv_n, h_nan;
static double h_lv[2];
static void h_audio_block(void)                 /* a guitar block from the converter, its output block to it */
{
    static uint32_t in[2 * A_FR];
    static int32_t out[2 * A_FR];
    for (uint32_t i = 0; i < A_FR; i++, h_tone_at++) {
        const float v = h_tone_hz ? 0.25f * 8388607.0f * sinf(6.2831853f * (float)(h_tone_at % 44100u) * (float)h_tone_hz / 44100.0f) : 0.0f;
        in[2u * i] = (uint32_t)(int32_t)v & 0x00FFFFFFu;
        in[2u * i + 1u] = 0;
    }
    a_enc(0, (uint8_t *)in, (int)sizeof in, 1);
    if (h_nan && !a_hold) { h_nan = 0; a_out[0] = 0.0f / 0.0f * a_in[1]; dsp_out(); }   /* (as if the app's sound had made it) */
    a_dec(0, (uint8_t *)out, (int)sizeof out, 0);
    for (uint32_t i = 0; i < A_FR; i++) {
        const double l = out[2u * i] / 8388608.0, r = out[2u * i + 1u] / 8388608.0;
        h_lv[0] += l * l; h_lv[1] += r * r;
    }
    h_lv_n += A_FR;
}
static void h_tick(void)
{
    h_ms++;
    h_ticks += 1000u * FM1_TICKS_PER_US;
    if (!a_running || h_irq_held) return;       /* (a held interrupt makes no block) */
    for (h_aud_us += 1000u; h_aud_us >= 2902u; h_aud_us -= 2902u) h_audio_block();
}

/* ---- the host's commands: what a hand does, as steps in time ---- */
enum { H_PIN, H_ENC, H_WAIT, H_SAY, H_LEVEL0, H_LEVEL };
typedef struct { uint8_t kind; int32_t a, b; const char *say; } h_step_t;
#define H_Q 1024u
static h_step_t h_q[H_Q];
static uint32_t h_qr, h_qw, h_wait;
static int h_closed;
static const char *h_persist, *h_dump;
static void h_do(uint8_t kind, int32_t a, int32_t b, const char *say) { if (h_qw - h_qr < H_Q) h_q[h_qw++ % H_Q] = (h_step_t){kind, a, b, say}; }

static const char *const H_BTN[IN_N] = {"fs_up", "fs_dn", "push", "home", "fx", "mod", "amp", "cab", "dly", "rev"};
static void h_pin(uint32_t b, int down)         /* the button's pin, or the keypad's voltage (base/controls.h) */
{
    static const struct { uint32_t *port; uint32_t mask; } P[7] = { {&h_pa_in, PA_FS_UP}, {&h_pa_in, PA_FS_DN}, {&h_ph_in, 1u << 4},
        {&h_pa_in, PA_HOME}, {&h_pa_in, PA_FX}, {&h_ph_in, 1u << 7}, {&h_ph_in, PH_AMP} };
    static const int32_t KEY[3] = {617, 706, 788};
    if (b >= IN_CAB) h_adc[ADC_CH_KEYS] = down ? KEY[b - IN_CAB] : 1023;
    else if (down) *P[b].port &= ~P[b].mask;
    else *P[b].port |= P[b].mask;
}
static void h_save_flash(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(h_nor, 1, sizeof h_nor, f); fclose(f); }
}
static int h_button(const char *name)
{
    for (int b = 0; b < (int)IN_N; b++) if (!strcmp(name, H_BTN[b])) return b;
    return -1;
}
static void h_command(char *l)
{
    char name[32] = "";
    int n = 0, b;
    if (sscanf(l, "#down %31s", name) == 1 || sscanf(l, "#up %31s", name) == 1) {
        const int down = l[1] == 'd';
        if ((b = h_button(name)) < 0) { fprintf(stdout, "#%s err no such button\n", down ? "down" : "up"); return; }
        h_do(H_PIN, b, down, 0); h_do(H_WAIT, 30, 0, 0); h_do(H_SAY, 0, 0, down ? "#down ok" : "#up ok");
    } else if (sscanf(l, "#press %31s", name) == 1) {
        size_t k = strlen(name);
        const int lng = k > 5 && !strcmp(name + k - 5, "_long");
        if (lng) name[k - 5] = 0;
        if ((b = h_button(name)) < 0) { fputs("#press err no such control\n", stdout); return; }
        h_do(H_PIN, b, 1, 0); h_do(H_WAIT, lng ? 1200 : 60, 0, 0); h_do(H_PIN, b, 0, 0); h_do(H_WAIT, 40, 0, 0); h_do(H_SAY, 0, 0, "#press ok");
    } else if (sscanf(l, "#knob %d", &n) == 1) {
        const int fast = strstr(l, "fast") != 0, dir = n < 0 ? -1 : 1;
        if (n < -200 || n > 200) { fputs("#knob err -200..200\n", stdout); return; }
        for (int i = 0; i < (n < 0 ? -n : n); i++) {
            for (int s = 0; s < 4; s++) { h_do(H_ENC, dir, 0, 0); h_do(H_WAIT, 1, 0, 0); }
            h_do(H_WAIT, fast ? 6 : 56, 0, 0);
        }
        h_do(H_SAY, 0, 0, "#knob ok");
    } else if (sscanf(l, "#vol %d", &n) == 1) {
        if (n < 0 || n > 100) { fputs("#vol err 0..100\n", stdout); return; }
        h_adc[ADC_CH_MASTER] = n >= 100 ? 1023 : n <= 0 ? 0 : 12 + (n * 998 + 99) / 100;   /* (the knob's hysteresis may leave it 1 off) */
        h_do(H_WAIT, 700, 0, 0); h_do(H_SAY, 0, 0, "#vol ok");
    } else if (!strcmp(l, "#nan")) {
        h_nan = 1;
        h_do(H_WAIT, 20, 0, 0); h_do(H_SAY, 0, 0, "#nan ok");
    } else if (sscanf(l, "#wait %d", &n) == 1) {
        if (n < 0 || n > 600000) { fputs("#wait err 0..600000\n", stdout); return; }
        h_do(H_WAIT, n, 0, 0); h_do(H_SAY, 0, 0, "#wait ok");
    } else if (sscanf(l, "#sound %d", &n) == 1) {
        h_tone_hz = (uint32_t)(n < 0 ? 0 : n);
        h_do(H_WAIT, 300, 0, 0); h_do(H_LEVEL0, 0, 0, 0); h_do(H_WAIT, 100, 0, 0); h_do(H_LEVEL, 0, 0, 0);
    } else if (!strcmp(l, "#status")) {
        fprintf(stdout, "#status {\"app\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"caps\":%u,\"editor\":%u,\"identity\":\"%s\","
                "\"frame\":%u,\"volume\":%u,\"load\":%u,\"bypass\":%u,\"safe\":%d,\"hold\":%u,\"blocks\":%u,\"writes_ok\":%s,\"ms\":%u,"
                "\"start\":\"%s\",\"guard_pending\":%u,\"nans\":%u,\"nan_now\":%u}\n",
                (const char *)OP_APP.id, (const char *)OP_APP.name, (const char *)OP_APP.version, OP_APP.caps, OP_APP.editor,
                FELUCCA_ID_PTR, fb_gen, in_hw.vol, a_load, a_bypass, base_safe, a_hold, a_blocks, h_writes_ok ? "true" : "false", h_ms,
                ST_NAME[op_diag.stage < ST_N ? op_diag.stage : 0u], guard.pending, a_nan_count, a_nan);
    } else if (!strcmp(l, "#screen")) {
        static char hex[240 * 240 * 4 + 16];
        static const char H[] = "0123456789abcdef";
        char *p = hex;
        for (uint32_t i = 0; i < 240u * 240u; i++) { const uint16_t c = fb[i]; *p++ = H[c >> 12]; *p++ = H[c >> 8 & 15]; *p++ = H[c >> 4 & 15]; *p++ = H[c & 15]; }
        *p = 0;
        fputs("#screen ", stdout); fputs(hex, stdout); fputs("\n", stdout);
    } else if (!strncmp(l, "#flash ", 7)) {
        h_save_flash(l + 7);
        fputs("#flash ok\n", stdout);
    }
    fflush(stdout);
}
/* a line of MIDI in hex: SysEx is a frame for the update code and the app's INFO and editor requests; any other
 * message goes into the ring, as usb.c queues it (one USB-MIDI event packet, with the millisecond it arrived) */
static void h_midi(const char *l)
{
    uint8_t m[2048];
    uint32_t n = 0;
    for (; l[0] && l[1] && n < sizeof m; l += 2) { unsigned v; if (sscanf(l, "%2x", &v) != 1) return; m[n++] = (uint8_t)v; }
    if (n >= 2u && m[0] == 0xF0u && m[n - 1u] == 0xF7u) {
        if (h_frame_full || n - 2u > sizeof h_frame) return;
        memcpy(h_frame, m + 1, n - 2u); h_frame_len = n - 2u; h_frame_full = 1;
    } else if (n >= 1u && n <= 3u && (m[0] & 0x80u) && mi_w - mi_r < MQ) {
        const uint32_t cin = m[0] >= 0xF8u ? 0xFu : (uint32_t)m[0] >> 4;
        midi_in_q[mi_w % MQ] = cin | (uint32_t)m[0] << 8 | (uint32_t)(n > 1u ? m[1] : 0u) << 16 | (uint32_t)(n > 2u ? m[2] : 0u) << 24;
        midi_in_ms[mi_w % MQ] = h_ms;
        mi_w++;
    }
}
static void h_host(void)                        /* once a main-loop pass (tests/host/px_usb.h) */
{
    static char line[8192];
    static size_t ll;
    if (h_flash_dirty && h_persist) { h_save_flash(h_persist); h_flash_dirty = 0; }
    if (h_wait) { h_wait--; return; }           /* a step that takes time: this pass is 1 ms of it */
    while (h_qr != h_qw) {
        const h_step_t s = h_q[h_qr++ % H_Q];
        if (s.kind == H_PIN) h_pin((uint32_t)s.a, s.b);
        else if (s.kind == H_ENC) {             /* one step of the encoder's two pins (four a detent) */
            static const uint8_t SEQ[4] = {0, 1, 3, 2};
            static uint32_t at;
            at = (at + (s.a > 0 ? 1u : 3u)) & 3u;
            h_pc_in = (h_pc_in & ~3u) | SEQ[at];
        } else if (s.kind == H_SAY) { fputs(s.say, stdout); fputs("\n", stdout); fflush(stdout); }
        else if (s.kind == H_LEVEL0) { h_lv[0] = h_lv[1] = 0.0; h_lv_n = 0; }
        else if (s.kind == H_LEVEL) {
            const double n = h_lv_n ? (double)h_lv_n : 1.0;
            fprintf(stdout, "#sound %d %d\n", (int)(sqrt(h_lv[0] / n) * 10000.0 + 0.5), (int)(sqrt(h_lv[1] / n) * 10000.0 + 0.5));
            fflush(stdout);
        } else if (s.kind == H_WAIT) { h_wait = (uint32_t)s.a; return; }
    }
    {   /* nothing in progress: a line from the host, if one comes within a millisecond */
        fd_set fs;
        struct timeval tv = {0, 1000};
        FD_ZERO(&fs);
        FD_SET(0, &fs);
        if (h_closed || select(1, &fs, 0, 0, &tv) <= 0) return;
        for (;;) {
            char c;
            if (read(0, &c, 1) <= 0) { h_closed = 1; break; }
            if (c == '\n') break;
            if (ll < sizeof line - 1) line[ll++] = c;
        }
        if (h_closed) {                         /* the host is gone: the flash is kept, the program ends */
            if (h_dump) h_save_flash(h_dump);
            if (h_persist) h_save_flash(h_persist);
            exit(h_writes_ok ? 0 : 1);
        }
        line[ll] = 0; ll = 0;
        if (line[0] == '#') h_command(line); else h_midi(line);
    }
}

int main(int argc, char **argv)
{
    const char *image = 0;
    int failed = 0;
    FILE *f;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--persist") && i + 1 < argc) h_persist = argv[++i];
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) h_dump = argv[++i];
        else if (!strcmp(argv[i], "--failed-start")) failed = 1;
        else if (argv[i][0] != '-') image = argv[i];
    }
    memset(h_nor, 0xFF, sizeof h_nor);
    if (!image && h_persist) image = h_persist;
    if (image && (f = fopen(image, "rb")) != 0) {
        if (fread(h_nor, 1, sizeof h_nor, f) != sizeof h_nor) { fprintf(stderr, "%s: need 1 MiB\n", image); return 2; }
        fclose(f);
    } else if (image && image != h_persist) { fprintf(stderr, "%s: not found\n", image); return 2; }
    h_pa_in = h_ph_in = 0xFFFFu;                /* every button up, the keypad idle, MASTER at 80 */
    h_adc[ADC_CH_KEYS] = 1023; h_adc[ADC_CH_MASTER] = 811; h_adc[8] = 600;
    if (failed) { op_diag.magic = OPD_MAGIC; op_diag.stage = ST_RUN; op_diag.safe = 0; }   /* (stopped in its first 6 s) */
    guard.pending = 1;                          /* (fm1_cstart, firmware/base/start.h: this start has not finished yet) */
    fputs("#port OpenPocket\n", stdout);
    fflush(stdout);
    app_main();
    return 0;
}
