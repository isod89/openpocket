/* SPDX-License-Identifier: GPL-3.0-only */
/* The radio pools' allocator of the Bluetooth audio image (firmware/bt/bt_lbuf.h, the SDK's lbuf calls) on a PC: 100,000 random
 * allocations, frees, references and shrinks against a model. Checks: no two live blocks overlap and none leaves the pool;
 * what is written in a block is still there when it is freed; a block referenced twice needs two frees; a shrink gives its
 * tail back; when everything is freed the pool is one free block again (no leak, no fragment left); the state calls agree. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int irq_depth;
void local_irq_disable(void) { irq_depth++; }
void local_irq_enable(void) { irq_depth--; }
#include "../firmware/bt/bt_lbuf.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define POOL 6000u
#define LIVE 64
static uint8_t pool[POOL + 64] __attribute__((aligned(16)));
static struct { uint8_t *p; uint32_t n; uint8_t tag; int refs; } live[LIVE];

int main(void)
{
    uint8_t *const buf = pool + 4;                         /* 4-byte aligned, not 16: as acl_rx_pool (0x01C02DE4) */
    void *const r = lbuf_init(buf, POOL, 4, 0);
    void *const h = buf;                                   /* the library keeps buf as the pool, not the value returned */
    bt_lbuf_state_t st, st0;
    uint32_t i, ops = 0, allocs = 0, fails_alloc = 0, overlap = 0, outside = 0, corrupt = 0, dbl = 0, shrink_ok = 0;
    srand(7);
    CHECK(r == buf, "a pool is set up in a %u-byte buffer at a 4-byte aligned address, its header at the buffer itself", POOL);
    CHECK(lbuf_init(pool + 3, POOL, 4, 0) == 0 && lbuf_init(buf, POOL, 4, 0) == buf, "a start that is not 4-byte aligned is refused");
    lbuf_state(h, &st0);
    CHECK(st0.num == 1 && st0.avaliable == st0.max_continue_len && st0.avaliable > POOL - 200u, "it starts as one free block of %u bytes", st0.avaliable);
    for (ops = 0; ops < 100000u; ops++) {
        int k = rand() % LIVE, what = rand() % 10;
        if (!live[k].p) {
            uint32_t n = 1u + (uint32_t)(rand() % 700);
            uint8_t *p = lbuf_alloc(h, n);
            if (!p) { fails_alloc++; continue; }
            allocs++;
            if (p < buf || p + n > buf + POOL || ((uintptr_t)p & 3u)) outside++;
            if (lbuf_real_size(p) < (int)n) outside++;
            for (i = 0; i < LIVE; i++)
                if (live[i].p && p < live[i].p + live[i].n && live[i].p < p + n) overlap++;
            live[k].p = p; live[k].n = n; live[k].tag = (uint8_t)(ops * 7u + 1u); live[k].refs = 1;
            memset(p, live[k].tag, n);
        } else if (what == 0) {
            lbuf_inc_ref(live[k].p); live[k].refs++;
        } else if (what == 1 && live[k].n > 40u) {
            uint32_t n2 = live[k].n / 2u;
            int before = lbuf_free_space(h);
            if (lbuf_realloc(live[k].p, (int)n2) == live[k].p) { live[k].n = n2; if (lbuf_free_space(h) >= before) shrink_ok++; }
        } else {
            for (i = 0; i < live[k].n; i++) if (live[k].p[i] != live[k].tag) { corrupt++; break; }
            int r = lbuf_free(live[k].p);
            live[k].refs--;
            if ((live[k].refs > 0) != (r == 0)) dbl++;
            if (live[k].refs == 0) live[k].p = 0;
        }
    }
    CHECK(allocs > 20000u && fails_alloc > 0u, "%u allocations (%u refused when the pool was full: 64 blocks of up to 700 bytes share 6 KB)", allocs, fails_alloc);
    CHECK(overlap == 0u && outside == 0u, "no live blocks overlap (%u), none leaves the pool, is short or is not 4-byte aligned (%u)", overlap, outside);
    CHECK(corrupt == 0u, "what is written in a block is still there when it is freed (%u corrupted)", corrupt);
    CHECK(dbl == 0u, "a block referenced n times needs n frees, the last one frees it (%u wrong)", dbl);
    CHECK(shrink_ok > 1000u, "a shrink gives its tail back (%u)", shrink_ok);
    for (i = 0; i < LIVE; i++) while (live[i].p) { lbuf_free(live[i].p); if (--live[i].refs == 0) live[i].p = 0; }
    lbuf_state(h, &st);
    CHECK(st.num == 1 && st.avaliable == st0.avaliable, "everything freed: one free block of %u bytes again (%d blocks)", st.avaliable, st.num);
    CHECK(lbuf_avaliable(h, 100) == (int)(st0.avaliable + BT_LBUF_HDR) / (int)bt_lbuf_need(100), "lbuf_avaliable counts the packets that fit (%d)", lbuf_avaliable(h, 100));
    CHECK(lbuf_free(0) == 0 && lbuf_alloc(0, 10) == 0 && lbuf_init(pool, 20, 4, 0) == 0, "NULL and a buffer too small are refused");
    CHECK(irq_depth == 0, "interrupts are given back as often as they are taken (%d)", irq_depth);
    printf(fails ? "bt lbuf: %d FAILED\n" : "bt lbuf: all passed\n", fails);
    return fails != 0;
}
