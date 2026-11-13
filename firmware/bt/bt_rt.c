/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The compiler runtime routines the classic Bluetooth stack needs and the JieLi toolchain does not have (the firmware links no
 * runtime library: docs/HARDWARE-HANDBOOK.md, "64-bit"). Read off the linked members' code: the pairing crypto (bigint.c) divides
 * 64-bit numbers; the link manager (lmp.c) scales a 16-bit value by a double. 64-bit multiply and shifts are native on this CPU;
 * only division and the double-precision steps are here, written with 32-bit and 64-bit integer operations only (never a
 * double operation: that would call these very functions). Also the variable 64-bit shifts, should the compiler call them. tests/test_bt_rt.c checks them against the PC's own arithmetic.
 * BT_RT_PREFIX renames them for that test. */
#include <stdint.h>

#ifndef BT_RT_PREFIX
#define BT_RT(name) name
#else
#define BT_RT_CAT(a, b) a##b
#define BT_RT_XCAT(a, b) BT_RT_CAT(a, b)
#define BT_RT(name) BT_RT_XCAT(BT_RT_PREFIX, name)
#endif

/* ---- 64-bit shifts by a variable count, in case the compiler calls them (32-bit halves only: never a 64-bit shift, which could
 * call these very functions). Counts outside 0..63 are undefined in C; here they give the count & 63. */
typedef union { uint64_t u; int64_t s; struct { uint32_t lo, hi; } w; } bt_rt_dw;   /* (little-endian) */
uint64_t BT_RT(__ashldi3)(uint64_t a, int b)
{
    bt_rt_dw x, r;
    b &= 63; x.u = a;
    if (!b) return a;
    if (b >= 32) { r.w.lo = 0; r.w.hi = x.w.lo << (b - 32); }
    else { r.w.lo = x.w.lo << b; r.w.hi = (x.w.hi << b) | (x.w.lo >> (32 - b)); }
    return r.u;
}
uint64_t BT_RT(__lshrdi3)(uint64_t a, int b)
{
    bt_rt_dw x, r;
    b &= 63; x.u = a;
    if (!b) return a;
    if (b >= 32) { r.w.hi = 0; r.w.lo = x.w.hi >> (b - 32); }
    else { r.w.hi = x.w.hi >> b; r.w.lo = (x.w.lo >> b) | (x.w.hi << (32 - b)); }
    return r.u;
}
int64_t BT_RT(__ashrdi3)(int64_t a, int b)
{
    bt_rt_dw x, r;
    b &= 63; x.s = a;
    if (!b) return a;
    if (b >= 32) { r.w.hi = (uint32_t)((int32_t)x.w.hi >> 31); r.w.lo = (uint32_t)((int32_t)x.w.hi >> (b - 32)); }
    else { r.w.hi = (uint32_t)((int32_t)x.w.hi >> b); r.w.lo = (x.w.lo >> b) | (x.w.hi << (32 - b)); }
    return r.s;
}

/* ---- 64-bit division: shift and subtract over the dividend's significant bits (32-bit operands take the native divide) */
static int bt_rt_clz64(uint64_t v)
{
    int n = 0;
    if (!(v >> 32)) { n += 32; v <<= 32; }
    if (!(v >> 48)) { n += 16; v <<= 16; }
    if (!(v >> 56)) { n += 8; v <<= 8; }
    if (!(v >> 60)) { n += 4; v <<= 4; }
    if (!(v >> 62)) { n += 2; v <<= 2; }
    if (!(v >> 63)) n += 1;
    return n;
}
static uint64_t bt_rt_udivmod(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t q = 0, r = 0;
    int i;
    if (!d) { if (rem) *rem = n; return ~(uint64_t)0; }      /* (undefined in C; no trap here) */
    if (!(n >> 32) && !(d >> 32)) {
        uint32_t a = (uint32_t)n, b = (uint32_t)d;
        if (rem) *rem = a % b;
        return a / b;
    }
    if (d > n) { if (rem) *rem = n; return 0; }
    for (i = 63 - bt_rt_clz64(n); i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= (uint64_t)1 << i; }
    }
    if (rem) *rem = r;
    return q;
}
uint64_t BT_RT(__udivdi3)(uint64_t n, uint64_t d) { return bt_rt_udivmod(n, d, 0); }
uint64_t BT_RT(__umoddi3)(uint64_t n, uint64_t d) { uint64_t r; bt_rt_udivmod(n, d, &r); return r; }
int64_t BT_RT(__divdi3)(int64_t n, int64_t d)
{
    const int neg = (n < 0) != (d < 0);
    const uint64_t q = bt_rt_udivmod(n < 0 ? 0u - (uint64_t)n : (uint64_t)n, d < 0 ? 0u - (uint64_t)d : (uint64_t)d, 0);
    return neg ? (int64_t)(0u - q) : (int64_t)q;
}
int64_t BT_RT(__moddi3)(int64_t n, int64_t d)
{
    uint64_t r;
    bt_rt_udivmod(n < 0 ? 0u - (uint64_t)n : (uint64_t)n, d < 0 ? 0u - (uint64_t)d : (uint64_t)d, &r);
    return n < 0 ? (int64_t)(0u - r) : (int64_t)r;              /* the remainder takes the dividend's sign */
}

/* ---- IEEE 754 double: the bits, through a union (little-endian, as this CPU) */
typedef union { double d; uint64_t u; } bt_rt_du;
#define BT_RT_MANT ((uint64_t)1 << 52)
#define BT_RT_FRAC (BT_RT_MANT - 1u)

double BT_RT(__floatunsidf)(uint32_t a)
{
    bt_rt_du r;
    int e;
    if (!a) { r.u = 0; return r.d; }
    e = 63 - bt_rt_clz64(a);                                    /* 0..31: exact, 32 bits fit in the 53 of a double */
    r.u = ((uint64_t)(e + 1023) << 52) | ((((uint64_t)a) << (52 - e)) & BT_RT_FRAC);
    return r.d;
}
double BT_RT(__floatsidf)(int32_t a)
{
    bt_rt_du r;
    if (a >= 0) return BT_RT(__floatunsidf)((uint32_t)a);
    r.d = BT_RT(__floatunsidf)(0u - (uint32_t)a);
    r.u |= (uint64_t)1 << 63;
    return r.d;
}

/* toward zero; out of range saturates (as the PC's conversions do for the values that are defined) */
static uint32_t bt_rt_fix(uint64_t bits, int *neg, int *over)
{
    const int e = (int)((bits >> 52) & 0x7FFu) - 1023;
    uint64_t m = (bits & BT_RT_FRAC) | BT_RT_MANT;
    *neg = (int)(bits >> 63);
    *over = 0;
    if (((bits >> 52) & 0x7FFu) == 0x7FFu) { *over = 1; return 0xFFFFFFFFu; }   /* inf, NaN */
    if (e < 0) return 0;
    if (e > 31) { *over = 1; return 0xFFFFFFFFu; }
    return (uint32_t)(m >> (52 - e));
}
uint32_t BT_RT(__fixunsdfsi)(double a)
{
    bt_rt_du x;
    int neg, over;
    uint32_t v;
    x.d = a;
    v = bt_rt_fix(x.u, &neg, &over);
    return neg ? 0u : v;
}
int32_t BT_RT(__fixdfsi)(double a)
{
    bt_rt_du x;
    int neg, over;
    uint32_t v;
    x.d = a;
    v = bt_rt_fix(x.u, &neg, &over);
    if (over || v > 0x7FFFFFFFu) return neg ? (int32_t)0x80000000u : 0x7FFFFFFF;
    return neg ? -(int32_t)v : (int32_t)v;
}

/* a x b, rounded to nearest even; zeros, infinities and NaN handled; subnormal inputs are taken as zero and a result below the
 * normal range flushes to zero (the stack scales small integers: never near either end) */
double BT_RT(__muldf3)(double a, double b)
{
    bt_rt_du x, y, r;
    uint64_t ma, mb, hi, lo, p0, p1, p2, p3, mid, sign;
    int ea, eb, e, sh;
    x.d = a; y.d = b;
    sign = (x.u ^ y.u) & ((uint64_t)1 << 63);
    ea = (int)((x.u >> 52) & 0x7FFu); eb = (int)((y.u >> 52) & 0x7FFu);
    if (ea == 0x7FF || eb == 0x7FF) {
        const int nan_a = ea == 0x7FF && (x.u & BT_RT_FRAC), nan_b = eb == 0x7FF && (y.u & BT_RT_FRAC);
        if (nan_a || nan_b || (ea == 0x7FF && !eb) || (eb == 0x7FF && !ea)) { r.u = 0x7FF8000000000000u; return r.d; }   /* NaN, inf x 0 */
        r.u = sign | 0x7FF0000000000000u;
        return r.d;
    }
    if (!ea || !eb) { r.u = sign; return r.d; }
    ma = (x.u & BT_RT_FRAC) | BT_RT_MANT;                       /* 53 bits each */
    mb = (y.u & BT_RT_FRAC) | BT_RT_MANT;
    /* the 106-bit product from 32-bit halves: hi:lo */
    p0 = (ma & 0xFFFFFFFFu) * (mb & 0xFFFFFFFFu);
    p1 = (ma & 0xFFFFFFFFu) * (mb >> 32);
    p2 = (ma >> 32) * (mb & 0xFFFFFFFFu);
    p3 = (ma >> 32) * (mb >> 32);
    mid = (p0 >> 32) + (p1 & 0xFFFFFFFFu) + (p2 & 0xFFFFFFFFu);
    lo = (p0 & 0xFFFFFFFFu) | (mid << 32);
    hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
    /* the product is in [2^104, 2^106): keep 53 bits, round on the rest */
    e = ea + eb - 1023;
    sh = (hi >> 41) ? 53 : 52;                                  /* bit 105 set: one more to shift */
    if (sh == 53) e++;
    {
        /* the 53 kept bits: (hi:lo) >> sh; the round bit and the sticky bits below it */
        const uint64_t keep = (hi << (64 - sh)) | (lo >> sh);
        const uint64_t round = (lo >> (sh - 1)) & 1u;
        const uint64_t sticky = (lo & (((uint64_t)1 << (sh - 1)) - 1u)) != 0;
        uint64_t m = keep;
        if (round && (sticky || (m & 1u))) {
            m++;
            if (m >> 53) { m >>= 1; e++; }
        }
        if (e >= 0x7FF) { r.u = sign | 0x7FF0000000000000u; return r.d; }
        if (e <= 0) { r.u = sign; return r.d; }
        r.u = sign | ((uint64_t)e << 52) | (m & BT_RT_FRAC);
    }
    return r.d;
}
