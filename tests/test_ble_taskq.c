/* SPDX-License-Identifier: GPL-3.0-only */
/* The Bluetooth images' task message queues (firmware/ble/ble_os.c os_taskq_post_type / os_taskq_pend) on a PC, with the
 * library's message sizes: the host task's events are posted inline, (length + 3) / 4 words, and read 32 words at a time
 * (btstack_task.c); the controller's task reads 14. Checks: a message comes back whole, type first, up to 32 words; a longer
 * one is cut at 32; the reader gets only the words it has room for and the next message is not disturbed; a full queue says
 * so (OS_Q_FULL) and keeps what it holds; thousands of messages of every size through the ring, wrapping, in order.
 * And the mutexes, recursive as the SDK's (the library takes one it holds: remote_device_db.c updata_last_link_key).
 * And the scheduler's clock across TIMER4's wrap (every 178.957 s): a periodic timer keeps firing, a one-shot armed before the wrap
 * fires after it, a task asleep across it wakes on time (up to test build 10 all three stopped for good at the first wrap).
 * Built a second time with OP_BT_AUDIO (the Bluetooth audio image's scheduler): inside one of the baseband's interrupts
 * (bt_isr_depth) cpu_in_irq() is 1 and no wait waits; each task's queue peak, stack depth and red zone. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
/* ble_os.c's own heap and the C library names ble_os.h declares for the pedal, renamed off the PC's (whose headers are already
 * in: their declarations stay as they are) */
#undef memset
#undef memcpy
#undef memmove
#undef strcpy
#undef strncpy
#define memset ble_t_memset
#define memmove ble_t_memmove
#define memcmp ble_t_memcmp
#define memcpy ble_t_memcpy
#define strlen ble_t_strlen
#define strcmp ble_t_strcmp
#define strcpy ble_t_strcpy
#define strncpy ble_t_strncpy
#define malloc ble_t_malloc
#define free ble_t_free
#define zalloc ble_t_zalloc
#define MAXTASK 2
#define STACK_WORDS 64u
#ifdef __APPLE__
#define section(x) used                                 /* (Mach-O takes no ELF section names; Linux, the CI, keeps them) */
#endif
#include "../firmware/ble/ble_os.c"
#if OP_BT_AUDIO
static void bt_taskq_spy(const char *task, int type, int argc, const int *argv) { (void)task; (void)type; (void)argc; (void)argv; }
void bt_isr_br(void) { }
void bt_isr_slot(void) { }
#endif
void *ble_t_memset(void *d, int c, unsigned n) { uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d; }
void *ble_t_memcpy(void *d, const void *s, unsigned n) { uint8_t *p = d; const uint8_t *q = s; while (n--) *p++ = *q++; return d; }
size_t ble_t_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int ble_t_strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
char *ble_t_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) { } return r; }
char *ble_t_strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
int ble_t_memcmp(const void *a, const void *b, unsigned n) { const uint8_t *p = a, *q = b; for (; n--; p++, q++) if (*p != *q) return *p - *q; return 0; }
void *ble_t_memmove(void *d, const void *s, size_t n) { uint8_t *p = d; const uint8_t *q = s; if (p < q) while (n--) *p++ = *q++; else for (p += n, q += n; n--;) *--p = *--q; return d; }
#undef memset
#undef memcpy
/* the context switches, for a PC: a task the scheduler runs (cur) is taken to have run and then waited for a message; a task
 * that gives up the processor (task_yield) comes straight back */
static int runs;
static uint32_t ran_at;
static uint32_t *run_paint_at;                                  /* (if set, the "task" writes this word of its stack: a deep call) */
static void ran(void) { runs++; ran_at = ble_ms(); if (run_paint_at) *run_paint_at = 0; cur->state = T_WAIT_Q; }
void ble_ctx_swap(uint32_t **save, uint32_t *load) { (void)load; if (save == &sched_sp) ran(); }
void ble_ctx_enter(uint32_t **save, uint32_t *top, void (*fn)(void)) { (void)save; (void)top; (void)fn; ran(); }
void (*ble_irq_target_ev)(void), (*ble_irq_target_rx)(void);
void ble_isr_ev(void) { }
void ble_isr_rx(void) { }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void body(void *a) { (void)a; }
static int fired_p, fired_o;
static void tick_p(void *a) { (void)a; fired_p++; }
static void tick_o(void *a) { (void)a; fired_o++; }
static int words(int n, int seed, int *w) { int i; for (i = 0; i < n; i++) w[i] = seed * 1000 + i; return n; }

int main(void)
{
    int in[64], out[40], i, bad = 0, n, k, posted = 0, got = 0;
    ble_os_init();
    CHECK(task_create(body, 0, "btstack") == 0, "a task is created");
    cur = task_find("btstack");                          /* (the reader: os_taskq_pend reads the current task's queue) */
    for (n = 0; n <= 32; n++) {
        words(n, n + 1, in);
        if (os_taskq_post_type("btstack", 0x400001, n, in)) bad++;
        memset(out, 0x55, sizeof out);
        if (os_taskq_pend(0, out, 33) != 13 || out[0] != 0x400001) bad++;
        for (i = 0; i < n; i++) if (out[1 + i] != in[i]) bad++;
    }
    CHECK(bad == 0, "messages of 0 to 32 words come back whole, the type first (%d wrong)", bad);
    words(40, 7, in);
    os_taskq_post_type("btstack", 4, 40, in);
    memset(out, 0, sizeof out);
    os_taskq_pend(0, out, 40);
    CHECK(out[0] == 4 && out[32] == in[31] && out[33] == 0, "a message of 40 words is cut at 32");
    words(20, 3, in); os_taskq_post_type("btstack", 5, 20, in);
    words(2, 4, in); os_taskq_post_type("btstack", 6, 2, in);
    memset(out, 0, sizeof out);
    os_taskq_pend(0, out, 14);                            /* (the controller's task reads 14) */
    k = out[0] == 5 && out[13] == 3 * 1000 + 12 && out[14] == 0;
    os_taskq_pend(0, out, 14);
    CHECK(k && out[0] == 6 && out[1] == 4000 && out[2] == 4001, "a reader with room for 14 gets 14, and the next message is whole");
    for (n = 0; os_taskq_post_type("btstack", 9, 32, in) == 0; n++) ;
    CHECK(n == (int)(QWORDS / (2u + 32u)) && os_taskq_post_type("btstack", 9, 32, in) == OS_Q_FULL,
          "a full queue says so: %d messages of 32 words fit in %u words", n, QWORDS);
    while (task_find("btstack")->qn) os_taskq_pend(0, out, 33);
    bad = 0;
    for (k = 0; k < 20000; k++) {                         /* every size, through the ring's end many times, in order */
        n = (k * 7) % 33;
        words(n, k, in);
        if (os_taskq_post_type("btstack", k, n, in) == 0) posted++;
        if (k % 3 != 0) {
            while (task_find("btstack")->qn > 2u) {
                os_taskq_pend(0, out, 33);
                if (out[0] != got) bad++;
                for (i = 0; i < (got * 7) % 33; i++) if (out[1 + i] != got * 1000 + i) bad++;
                got++;
            }
        }
    }
    while (task_find("btstack")->qn) {
        os_taskq_pend(0, out, 33);
        if (out[0] != got) bad++;
        got++;
    }
    CHECK(bad == 0 && posted == 20000 && got == 20000 && task_find("btstack")->qw == 0,
          "20000 messages of every size through the ring, in order (%d posted, %d read, %d wrong)", posted, got, bad);
    {
        static uint32_t mx[20], sm[20];                     /* (the SDK's OS_MUTEX / OS_SEM: 80 bytes) */
        sem_t *const x = (sem_t *)(void *)mx;
        os_mutex_create(mx);
        k = os_mutex_pend(mx, 0) == 0 && os_mutex_pend(mx, 0) == 0 && os_mutex_pend(mx, 0) == 0 && x->count == 0 && x->depth == 3u;
        CHECK(k, "the task that holds a mutex takes it again, three deep (count %d, depth %u)", (int)x->count, (unsigned)x->depth);
        os_mutex_post(mx); os_mutex_post(mx);
        k = x->count == 0 && x->depth == 1u;
        os_mutex_post(mx);
        CHECK(k && x->count == 1 && x->depth == 0u && x->owner == 0, "it is free again only after as many gives as takes");
        os_sem_create(sm, 1);
        CHECK(os_sem_pend(sm, 0) == 0 && ((sem_t *)(void *)sm)->count == 0, "a semaphore is not a mutex: one take empties it");
    }
    {   /* the clock across TIMER4's wrap: 1 ms steps from 0.5 s before it to 1.5 s after */
        task_t *t = task_find("btstack");
        uint32_t m0, i, woke_at, slept_at;
        while (t->qn) os_taskq_pend(0, out, 33);
        t->state = T_WAIT_Q;
        cur = 0;
        shim_t4 = 0xFFFFFFFFu - 24000u * 500u;
        m0 = ble_ms();
        CHECK(sys_timer_add(0, tick_p, 100) != 0 && sys_timeout_add(0, tick_o, 700) != 0, "a periodic timer (100 ms) and a one-shot (700 ms)");
        cur = t; os_time_dly(80); cur = 0;                     /* (the task: 800 ms asleep, across the wrap) */
        slept_at = ble_ms();
        k = t->state == T_SLEEP;
        runs = 0; woke_at = 0;
        for (i = 0; i < 2000u; i++) {
            shim_t4 += 24000u;
            ble_poll();
            if (runs && !woke_at) woke_at = ran_at;
        }
        CHECK(ble_ms() - m0 == 2000u, "the clock counts 2000 ms across the wrap (%u)", (unsigned)(ble_ms() - m0));
        CHECK(fired_p == 20, "the periodic timer fired 20 times in 2 s, the wrap in between (%d)", fired_p);
        CHECK(fired_o == 1, "the one-shot armed before the wrap fired after it (%d)", fired_o);
        CHECK(k && runs == 1 && woke_at - slept_at == 800u, "the task asleep across the wrap woke once, 800 ms later (%d, %u)", runs, (unsigned)(woke_at - slept_at));
    }
#if OP_BT_AUDIO
    {   /* inside one of the baseband's interrupts nothing waits; outside, as before */
        static uint32_t sm[20], mx[20];
        task_t *t = task_find("btstack");
        uint32_t w0;
        CHECK(cpu_in_irq() == 0, "outside an interrupt cpu_in_irq() is 0");
        bt_isr_depth = 1;
        CHECK(cpu_in_irq() == 1, "inside the baseband's interrupt it is 1 (the library's \"do not wait\")");
        cur = t; t->state = T_READY; t->wake_ms = 0;            /* (the interrupt cut into this task) */
        w0 = bt_wait_irq;
        os_time_dly(2);
        CHECK(t->state == T_READY && t->wake_ms == 0u, "os_time_dly in the interrupt returns at once and leaves the task as it was");
        os_sem_create(sm, 0);
        CHECK(os_sem_pend(sm, 0) == OS_TIMEOUT && t->state == T_READY, "os_sem_pend on an empty semaphore: OS_TIMEOUT at once");
        os_sem_post(sm);
        CHECK(os_sem_pend(sm, 0) == 0 && ((sem_t *)(void *)sm)->count == 0, "a semaphore that is free is still taken");
        bt_isr_depth = 0;
        os_mutex_create(mx); os_mutex_pend(mx, 0);             /* (the task holds the mutex) */
        bt_isr_depth = 1;
        CHECK(os_mutex_pend(mx, 0) == OS_TIMEOUT && ((sem_t *)(void *)mx)->depth == 1u, "the mutex the cut task holds is not the interrupt's to take again");
        words(3, 1, in); os_taskq_post_type("btstack", 7, 3, in);
        CHECK(os_taskq_pend(0, out, 33) == OS_Q_EMPTY && t->qn == 1u, "os_taskq_pend in the interrupt takes nothing from the cut task's queue");
        CHECK(bt_wait_irq - w0 == 4u, "the four refusals are counted (%u)", (unsigned)(bt_wait_irq - w0));
        bt_isr_depth = 0;
        os_mutex_post(mx);
        while (t->qn) os_taskq_pend(0, out, 33);
        cur = 0;
        w0 = bt_wait_main;
        os_time_dly(1);
        CHECK(bt_wait_main == w0 + 1u, "a wait asked by the main loop (a timer's callback) is counted");
    }
    {   /* each task's queue peak, stack depth and red zone */
        task_t *t = task_find("btstack");
        ble_task_info_t r;
        t->qpeak = 0;
        for (i = 0; i < 5; i++) os_taskq_post_type("btstack", 1, 1, in);
        cur = t; while (t->qn) os_taskq_pend(0, out, 33); cur = 0;
        CHECK(ble_task_info(0, &r) && r.qpeak == 5u && r.free == STACK_WORDS * 4u && !r.ovf, "the queue's peak (5), the stack untouched (%u B)", (unsigned)r.free);
        t->state = T_READY; run_paint_at = &t->stack[3];        /* (the task's run writes the fourth word: 16 bytes from its end) */
        ble_poll();
        run_paint_at = 0;
        CHECK(ble_task_info(0, &r) && r.free == 12u && r.ovf && bt_stack_ovf == 1u, "a write into the red zone is caught (free %u B, OVF %u)", (unsigned)r.free, (unsigned)r.ovf);
    }
#endif
    printf(fails ? "ble taskq: %d FAILED\n" : "ble taskq: all passed\n", fails);
    return fails != 0;
}
