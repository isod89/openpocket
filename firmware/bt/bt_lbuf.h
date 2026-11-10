/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The list-buffer allocator (lbuf) the classic controller splits its radio pools with (link_bulk.c), written for OpenPocket from the
 * SDK's public header system/generic/lbuf.h (Apache-2.0): included once, by bt_glue.c; tests/test_bt_lbuf.c runs it on a PC.
 * Needs local_irq_disable / local_irq_enable (ble/ble_os.c on the pedal). */
#pragma once
#include <stdint.h>
void local_irq_disable(void);
void local_irq_enable(void);

/* ---- the list-buffer allocator (lbuf): the radio's receive and send pools (link_bulk.c) are split into packets with it. First
 * fit, a 16-byte header before each block (its pool, its size with the header, in use, references), blocks back to back from
 * the pool's start; a free merges with free neighbours. Interrupt-safe the way the rest of the stack is (local_irq_*). */
typedef struct bt_lbuf_head { uint32_t magic, size, align, used; } bt_lbuf_head_t;      /* size: of the blocks' area */
typedef struct bt_lbuf_blk { bt_lbuf_head_t *h; uint32_t size; uint16_t busy, ref; uint32_t pad; } bt_lbuf_blk_t;
typedef struct { uint32_t avaliable, fragment, max_continue_len; int num; } bt_lbuf_state_t;  /* the SDK's struct lbuff_state */
#define BT_LBUF_MAGIC 0x4C425546u
#define BT_LBUF_HDR ((uint32_t)sizeof(bt_lbuf_blk_t))
static uint32_t bt_lbuf_need(uint32_t len) { return (len + BT_LBUF_HDR + 15u) & ~15u; }

/* each pool's highest use and its failed allocations, for the log (bt_a2dp.c): kept beside the pools, not in their header */
#define BT_LBUF_POOLS 4u
typedef struct { bt_lbuf_head_t *h; uint32_t peak, fails; } bt_lbuf_stat_t;
static bt_lbuf_stat_t bt_lbuf_stat[BT_LBUF_POOLS];
static bt_lbuf_stat_t *bt_lbuf_stat_of(bt_lbuf_head_t *h, int add)   /* add: a new pool (lbuf_init) takes a free entry */
{
    uint32_t i;
    for (i = 0; i < BT_LBUF_POOLS; i++)
        if (bt_lbuf_stat[i].h == h) { if (add) bt_lbuf_stat[i].peak = bt_lbuf_stat[i].fails = 0; return &bt_lbuf_stat[i]; }
    if (!add) return 0;
    for (i = 0; i < BT_LBUF_POOLS; i++)
        if (!bt_lbuf_stat[i].h) { bt_lbuf_stat[i].h = h; bt_lbuf_stat[i].peak = bt_lbuf_stat[i].fails = 0; return &bt_lbuf_stat[i]; }
    return 0;
}
static bt_lbuf_blk_t *bt_lbuf_first(bt_lbuf_head_t *h) { return (bt_lbuf_blk_t *)(void *)(h + 1); }
static bt_lbuf_blk_t *bt_lbuf_next(bt_lbuf_blk_t *b) { return (bt_lbuf_blk_t *)(void *)((uint8_t *)b + b->size); }
static int bt_lbuf_in(bt_lbuf_head_t *h, bt_lbuf_blk_t *b) { return (uint8_t *)b < (uint8_t *)bt_lbuf_first(h) + h->size; }
static void bt_lbuf_merge(bt_lbuf_head_t *h)
{
    bt_lbuf_blk_t *b = bt_lbuf_first(h), *n;
    while (bt_lbuf_in(h, b)) {
        n = bt_lbuf_next(b);
        if (!b->busy && bt_lbuf_in(h, n) && !n->busy) b->size += n->size;
        else b = n;
    }
}
void *lbuf_init(void *buf, uint32_t len, int align, int priv_head_len)
{
    uintptr_t a = ((uintptr_t)buf + 15u) & ~(uintptr_t)15u;
    bt_lbuf_head_t *h = (bt_lbuf_head_t *)a;
    bt_lbuf_blk_t *b;
    (void)align; (void)priv_head_len;                           /* (every block is 16-byte aligned, more than the 4 asked) */
    if (len < (uint32_t)(a - (uintptr_t)buf) + sizeof *h + 2u * BT_LBUF_HDR) return 0;
    h->magic = BT_LBUF_MAGIC;
    h->size = (len - (uint32_t)(a - (uintptr_t)buf) - (uint32_t)sizeof *h) & ~15u;
    h->align = 16u; h->used = 0;
    b = bt_lbuf_first(h);
    b->h = h; b->size = h->size; b->busy = 0; b->ref = 0;
    bt_lbuf_stat_of(h, 1);
    return h;
}
void *lbuf_alloc(void *head, uint32_t len)
{
    bt_lbuf_head_t *h = head;
    bt_lbuf_blk_t *b;
    uint32_t need = bt_lbuf_need(len);
    if (!h || h->magic != BT_LBUF_MAGIC) return 0;
    local_irq_disable();
    for (b = bt_lbuf_first(h); bt_lbuf_in(h, b); b = bt_lbuf_next(b)) {
        if (b->busy || b->size < need) continue;
        if (b->size >= need + 2u * BT_LBUF_HDR) {               /* split: the rest stays free */
            bt_lbuf_blk_t *r = (bt_lbuf_blk_t *)(void *)((uint8_t *)b + need);
            r->h = h; r->size = b->size - need; r->busy = 0; r->ref = 0;
            b->size = need;
        }
        b->h = h; b->busy = 1; b->ref = 1;
        h->used += b->size;
        { bt_lbuf_stat_t *s = bt_lbuf_stat_of(h, 0); if (s && h->used > s->peak) s->peak = h->used; }
        local_irq_enable();
        return b + 1;
    }
    { bt_lbuf_stat_t *s = bt_lbuf_stat_of(h, 0); if (s) s->fails++; }
    local_irq_enable();
    return 0;
}
int lbuf_free(void *p)
{
    bt_lbuf_blk_t *b;
    if (!p) return 0;
    b = (bt_lbuf_blk_t *)p - 1;
    if (!b->h || b->h->magic != BT_LBUF_MAGIC || !b->busy) return 0;
    local_irq_disable();
    if (b->ref > 1u) { b->ref--; local_irq_enable(); return 0; }  /* still read by another channel */
    b->busy = 0; b->ref = 0;
    b->h->used -= b->size;
    bt_lbuf_merge(b->h);
    local_irq_enable();
    return 1;
}
void *lbuf_realloc(void *p, int size)                           /* smaller only (the SDK's rule): the tail goes back free */
{
    bt_lbuf_blk_t *b;
    uint32_t need;
    if (!p || size < 0) return 0;
    b = (bt_lbuf_blk_t *)p - 1;
    need = bt_lbuf_need((uint32_t)size);
    if (need > b->size) return 0;
    local_irq_disable();
    if (b->size >= need + 2u * BT_LBUF_HDR) {
        bt_lbuf_blk_t *r = (bt_lbuf_blk_t *)(void *)((uint8_t *)b + need);
        r->h = b->h; r->size = b->size - need; r->busy = 0; r->ref = 0;
        b->h->used -= r->size;
        b->size = need;
        bt_lbuf_merge(b->h);
    }
    local_irq_enable();
    return p;
}
int lbuf_real_size(void *p) { return p ? (int)(((bt_lbuf_blk_t *)p - 1)->size - BT_LBUF_HDR) : 0; }
void lbuf_inc_ref(void *p) { if (p) { local_irq_disable(); ((bt_lbuf_blk_t *)p - 1)->ref++; local_irq_enable(); } }
static void bt_lbuf_scan(bt_lbuf_head_t *h, uint32_t need, bt_lbuf_state_t *st, int *fit)
{
    bt_lbuf_blk_t *b;
    st->avaliable = st->fragment = st->max_continue_len = 0; st->num = 0; *fit = 0;
    if (!h || h->magic != BT_LBUF_MAGIC) return;
    local_irq_disable();
    for (b = bt_lbuf_first(h); bt_lbuf_in(h, b); b = bt_lbuf_next(b)) {
        if (b->busy) continue;
        st->avaliable += b->size - BT_LBUF_HDR;
        st->fragment++; st->num++;
        if (b->size - BT_LBUF_HDR > st->max_continue_len) st->max_continue_len = b->size - BT_LBUF_HDR;
        if (need) *fit += (int)(b->size / need);
    }
    local_irq_enable();
}
uint32_t lbuf_free_space(void *h) { bt_lbuf_state_t st; int f; bt_lbuf_scan(h, 0, &st, &f); return st.max_continue_len; }
int lbuf_avaliable(void *h, int size) { bt_lbuf_state_t st; int f; bt_lbuf_scan(h, bt_lbuf_need((uint32_t)size), &st, &f); return f; }
void lbuf_state(void *h, void *state) { int f; bt_lbuf_scan(h, 0, (bt_lbuf_state_t *)state, &f); }
void lbuf_dump(void *h)                                         /* (the library calls it when bt_malloc finds its pool full) */
{
    (void)h;
#ifdef BT_CRUMB
    BT_CRUMB("send pool full");                                 /* (the stall record, bt_stall.h) */
#endif
}
void lbuf_free_check(void *p, uint32_t rets) { (void)p; (void)rets; }

