/* SPDX-License-Identifier: GPL-3.0-only */
/* The Bluetooth audio image's compiler runtime (firmware/bt/bt_rt.c) against the PC's own arithmetic, bit for bit: 64-bit division
 * and remainder (signed and unsigned, a million random pairs of every size), int -> double, double -> int, and double multiply
 * (a million random normal products, rounding to nearest even included; zeros, infinities, NaN, overflow). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define BT_RT_PREFIX t_
#include "../firmware/bt/bt_rt.c"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint64_t s = 0x9E3779B97F4A7C15u;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static uint64_t rnd_bits(void) { return rnd() >> (rnd() % 64u); }      /* every size, from 64 bits down to 0 */
static uint64_t bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

int main(void)
{
    uint32_t i, bad;
    for (bad = 0, i = 0; i < 1000000u; i++) {
        uint64_t n = rnd_bits(), d = rnd_bits();
        if (!d) d = 1;
        if (t___udivdi3(n, d) != n / d || t___umoddi3(n, d) != n % d) bad++;
        int64_t sn = (int64_t)n * ((rnd() & 1u) ? -1 : 1), sd = (int64_t)d * ((rnd() & 1u) ? -1 : 1);
        if (sd && t___divdi3(sn, sd) != sn / sd) bad++;
        if (sd && t___moddi3(sn, sd) != sn % sd) bad++;
    }
    CHECK(bad == 0, "64-bit division and remainder, signed and unsigned, a million random pairs (%u wrong)", bad);
    for (bad = 0, i = 0; i < 1000000u; i++) {
        uint32_t u = (uint32_t)rnd_bits();
        int32_t v = (int32_t)u;
        if (bits(t___floatunsidf(u)) != bits((double)u) || bits(t___floatsidf(v)) != bits((double)v)) bad++;
    }
    CHECK(bad == 0 && bits(t___floatunsidf(0)) == 0 && bits(t___floatsidf(INT32_MIN)) == bits(-2147483648.0),
          "int -> double, exact (%u wrong)", bad);
    for (bad = 0, i = 0; i < 1000000u; i++) {
        double d = (double)(int64_t)(rnd() >> 20) / (double)(1u + (rnd() >> 40)) * ((rnd() & 1u) ? -1.0 : 1.0);
        if (d > -2147483648.0 && d < 2147483648.0 && t___fixdfsi(d) != (int32_t)d) bad++;
        if (d >= 0.0 && d < 4294967296.0 && t___fixunsdfsi(d) != (uint32_t)d) bad++;
    }
    CHECK(bad == 0 && t___fixdfsi(1e300) == INT32_MAX && t___fixdfsi(-1e300) == INT32_MIN && t___fixunsdfsi(-3.5) == 0u &&
          t___fixdfsi(0.999) == 0 && t___fixdfsi(-0.999) == 0, "double -> int, toward zero, saturating (%u wrong)", bad);
    for (bad = 0, i = 0; i < 1000000u; i++) {
        uint64_t ua = (rnd() & 0x800FFFFFFFFFFFFFu) | ((uint64_t)(800u + rnd() % 450u) << 52);
        uint64_t ub = (rnd() & 0x800FFFFFFFFFFFFFu) | ((uint64_t)(800u + rnd() % 450u) << 52);
        double a, b;
        memcpy(&a, &ua, 8); memcpy(&b, &ub, 8);
        if (bits(t___muldf3(a, b)) != bits(a * b)) { if (bad < 3) printf("  %a x %a = %a, want %a\n", a, b, t___muldf3(a, b), a * b); bad++; }
    }
    CHECK(bad == 0, "double multiply, a million random normal products, bit for bit (%u wrong)", bad);
    {
        const double inf = 1.0 / 0.0;
        CHECK(bits(t___muldf3(65535.0, 0.8)) == bits(65535.0 * 0.8) && bits(t___muldf3(0.0, 5.0)) == 0 &&
              bits(t___muldf3(-0.0, 5.0)) == bits(-0.0) && t___muldf3(inf, 2.0) == inf && t___muldf3(-inf, 2.0) == -inf &&
              isnan(t___muldf3(inf, 0.0)) && isnan(t___muldf3(NAN, 1.0)) && t___muldf3(1e300, 1e300) == inf &&
              t___muldf3(1e-300, 1e-300) == 0.0, "the special cases: zero, signed zero, infinities, NaN, overflow, underflow to zero");
    }
    printf(fails ? "bt rt: %d FAILED\n" : "bt rt: all passed\n", fails);
    return fails != 0;
}
