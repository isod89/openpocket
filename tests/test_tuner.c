/* SPDX-License-Identifier: GPL-3.0-only
 * OpenPocket — host test of the tuner (firmware/px_tuner.h): bass and guitar notes, made here, through the pedal's own
 * capture and estimate, as the interrupt and the main loop call them (a 128-frame block at 44100 Hz; an estimate
 * every 100 ms). Each note is a sine and a plucked string (harmonics falling with their number and dying faster the
 * higher they are), the plucked one also with a weak fundamental (a bridge pickup), in tune and 5 to 30 cents off.
 * Every estimate from 0.2 s to 2 s after the pluck must name the note and its octave, within 2 cents up to the high E
 * string. Above it the estimate's lag is a few samples long and it reads sharp (as it always has): within 4 cents at
 * 650 Hz, 12 at 990 Hz.
 *   cc -O1 -Wall -Ifirmware tests/test_tuner.c -lm -o build/test_tuner && build/test_tuner [all]
 * (`all` prints every note's reading.)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TN_MEM
#define TN_READY 1
#include "px_tuner.h"

#define FS 44100.0
#define FR 128u
static const char *NOTE_N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static int semis(double hz) { return (int)lround(12.0 * log2(hz / 440.0)); }   /* from A4 */
static void name(int s, char *b) { sprintf(b, "%s%d", NOTE_N[((s % 12) + 12 + 9) % 12], (s + 9 + 48) / 12); }

enum { SINE, PLUCK, BRIDGE };
static const char *KIND[] = {"sine", "pluck", "bridge"};
static uint32_t rng = 1;
static double noise(void) { rng = rng * 1664525u + 1013904223u; return (double)(rng >> 8) / 8388608.0 - 1.0; }
static double sample(int kind, double f, double t)
{
    if (kind == SINE) return 0.25 * sin(2.0 * M_PI * f * t);
    double v = 0.0;
    for (int k = 1; k <= 24 && k * f < 5000.0; k++) {
        double a = (k == 1 && kind == BRIDGE ? 0.25 : 1.0) / k;
        v += a * exp(-t * (0.6 + 0.5 * k)) * sin(2.0 * M_PI * k * f * t);
    }
    return 0.3 * v * (t < 0.002 ? t / 0.002 : 1.0);
}

static int fails, checks;
/* one note: f0 the string's, read every 100 ms; prints it when `all` or when a reading is wrong */
static void note(const char *what, double f0, int kind, double tol, int all)
{
    static float x[FR];
    memset(tn_buf, 0, sizeof tn_buf); tn_w = 0;
    int want = semis(f0), bad = 0, n = 0;
    double lo = 1e9, hi = -1e9;
    char got[8] = "", wn[8];
    name(want, wn);
    for (uint32_t b = 0; b * FR < 2.0 * FS; b++) {
        for (uint32_t i = 0; i < FR; i++) x[i] = (float)(sample(kind, f0, (b * FR + i) / FS) + 3e-4 * noise());
        tn_capture(x, FR);
        double t = (b + 1) * FR / FS;
        if (t < 0.2 || (b + 1) % 34u) continue;    /* every 34 blocks: 99 ms */
        double hz = tuner_estimate(), c = hz > 0.0 ? 1200.0 * log2(hz / f0) : 1e9;
        n++;
        if (hz <= 0.0 || semis(hz) != want || fabs(c) > tol) {
            if (!bad) { if (hz > 0.0) name(semis(hz), got); else strcpy(got, "-"); }
            bad++;
        }
        if (hz > 0.0 && c < lo) lo = c;
        if (hz > 0.0 && c > hi) hi = c;
    }
    checks++;
    if (bad) fails++;
    if (bad || all)
        printf("%s %-22s %7.2f Hz %-6s %-4s: %2d of %2d readings right; cents off the string %+6.2f .. %+6.2f%s%s\n",
               bad ? "FAIL" : "ok  ", what, f0, KIND[kind], wn, n - bad, n, lo < 1e8 ? lo : 0.0, hi > -1e8 ? hi : 0.0,
               bad ? "; first wrong reading " : "", bad ? got : "");
}

int main(int argc, char **argv)
{
    int all = argc > 1 && !strcmp(argv[1], "all");
    static const struct { const char *what; double hz; double tol; } N[] = {   /* tol: cents */
        {"bass B (5-string)", 30.868, 2},
        {"bass drop D", 36.708, 2},
        {"bass E", 41.203, 2},
        {"bass F (E 1st fret)", 43.654, 2},
        {"bass G (E 3rd fret)", 48.999, 2},
        {"bass A", 55.000, 2},
        {"bass C (A 3rd fret)", 65.406, 2},
        {"bass D", 73.416, 2},
        {"bass E (A 7th fret)", 82.407, 2},
        {"bass G", 97.999, 2},
        {"bass C (G 5th fret)", 130.813, 2},
        {"guitar E", 82.407, 2},
        {"guitar A", 110.000, 2},
        {"guitar D", 146.832, 2},
        {"guitar G", 195.998, 2},
        {"guitar B", 246.942, 2},
        {"guitar E (high)", 329.628, 2},
        {"guitar E (12th fret)", 659.255, 4},
        {"guitar B (19th fret)", 987.767, 12},
    };
    static const double OFF[] = {0.0, -30.0, +17.0, -5.0};   /* cents: in tune and off it */
    for (unsigned i = 0; i < sizeof N / sizeof N[0]; i++)
        for (unsigned o = 0; o < sizeof OFF / sizeof OFF[0]; o++)
            for (int k = SINE; k <= BRIDGE; k++)
                note(N[i].what, N[i].hz * pow(2.0, OFF[o] / 1200.0), k, N[i].tol, all && o == 0);
    /* silence: no note */
    memset(tn_buf, 0, sizeof tn_buf);
    for (int b = 0; b < 400; b++) { float z[FR]; for (uint32_t i = 0; i < FR; i++) z[i] = (float)(1e-4 * noise()); tn_capture(z, FR); }
    checks++;
    if (tuner_estimate() != 0.0f) { printf("FAIL noise at -80 dBFS: a note\n"); fails++; }
    printf("tuner: %d of %d notes read right every 100 ms (bass B0 31 Hz to guitar B5 988 Hz), %s\n", checks - fails,
           checks, fails ? "FAIL" : "ok");
    return fails != 0;
}
