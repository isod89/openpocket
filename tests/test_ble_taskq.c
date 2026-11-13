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
 * (bt_isr_depth) cpu_in_irq() is 1 and no wait waits; each task's queue peak, stack depth and red zone; test build 37's timers
 * (added from interrupts, the interrupts put back as they were, the usr timeouts run by the 10 kHz tick, never on a taken
 * baseband lock). A third time with BT_USR_IN_TICK=0: the usr timeouts run from ble_poll. */
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
#if OP_BT_AUDIO
static int t_on_sys_stack;                              /* (test build 28: "the code runs on the system stack", an LE interrupt) */
#define BLE_SP_IN_ISR() (t_on_sys_stack != 0)
#endif
#include "../firmware/ble/ble_os.c"
#if OP_BT_AUDIO
static int fired37_sys, fired37_usr;                    /* (test build 37's timer callbacks) */
static void cb37_sys(void *p) { (void)p; fired37_sys++; }
static void cb37_usr(void *p) { fired37_usr += p ? (int)(intptr_t)p : 1; }
uint8_t *bb_hdl;                                        /* (RF_ble.c's baseband handle: its lock byte at BB_LOCK_AT) */
static uint8_t fake_bb[36];
static int lib_runs, lib_off_at_entry;                  /* a callback as the library's own: a counted pair inside */
static void cb37_lib(void *p) { (void)p; lib_runs++; lib_off_at_entry = cpu_irq_disabled(); local_irq_disable(); local_irq_enable(); }
#define USR_RUNS() do { if (!BT_USR_IN_TICK) ble_poll(); } while (0)   /* (BT_USR_IN_TICK 0: they run from ble_poll) */
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
        /* test build 28: the LE baseband's interrupts do not count themselves (ble_isr.S jumps into the library's own entry);
         * they run on the system stack, and that alone makes cpu_in_irq() 1 (RF_ble.c le_hw_suspend then takes
         * ble_hw_disable_isr, not the spinlock its interrupt holds) */
        t_on_sys_stack = 1;
        CHECK(cpu_in_irq() == 1 && bt_isr_depth == 0u, "on the system stack (an LE interrupt), with no depth counted: cpu_in_irq() is 1");
        cur = t; t->state = T_READY; t->wake_ms = 0;
        w0 = bt_wait_irq;
        os_time_dly(2);
        CHECK(t->state == T_READY && t->wake_ms == 0u && bt_wait_irq == w0 + 1u, "os_time_dly in an LE interrupt returns at once, refused and counted");
        t_on_sys_stack = 0;
        CHECK(cpu_in_irq() == 0, "back on a task's or the main loop's stack: 0 again");
        cur = 0;
    }
    {   /* test build 37: timers added from an interrupt, and the usr timeouts run by the 10 kHz tick */
        uint16_t id, ids[MAXTIMER + 1u], uid, uids[MAXUSR + 1u];
        uint32_t u0, n0, f0, i0, x0;
        cur = 0;
        n0 = tm_used; i0 = tm_irq;
        t_on_sys_stack = 1;                                     /* (in an interrupt) */
        id = sys_timeout_add(0, cb37_sys, 50);
        t_on_sys_stack = 0;
        CHECK(id && timer_start[id - 1] && timers[id - 1].due == 0u && tm_irq == i0 + 1u && tm_used == n0 + 1u,
              "a sys timeout added in an interrupt: no clock read there, it starts at the next ble_poll (counted)");
        shim_t4 += 24000u * 7u; ble_poll();                     /* (7 ms later: its 50 ms count from here) */
        CHECK(!timer_start[id - 1] && fired37_sys == 0, "the next ble_poll starts it");
        shim_t4 += 24000u * 49u; ble_poll();
        CHECK(fired37_sys == 0, "49 ms after that: not yet");
        shim_t4 += 24000u; ble_poll();
        CHECK(fired37_sys == 1 && tm_used == n0, "50 ms after that: once, and its slot free again");
        for (i = 0, k = 0; i < MAXTIMER; i++) k += timers[i].used;
        CHECK((uint32_t)k == n0, "the slots-in-use count matches the table (%u)", (unsigned)n0);
        f0 = tm_fail;
        for (k = 0; k < (int)(MAXTIMER - n0); k++) ids[k] = sys_timer_add(0, cb37_sys, 1000);
        ids[k] = sys_timer_add(0, cb37_sys, 1000);
        CHECK(ids[k - 1] && !ids[k] && tm_used == MAXTIMER && tm_peak == MAXTIMER && tm_fail == f0 + 1u,
              "a full sys table: the add refused and counted");
        for (k = 0; k < (int)(MAXTIMER - n0); k++) sys_timer_del(ids[k]);
        CHECK(tm_used == n0, "deleted: the slots in use back to before");
        id = sys_timer_add(0, cb37_sys, 30);                    /* (a periodic one, restarted from an interrupt) */
        shim_t4 += 24000u * 20u; ble_poll();
        t_on_sys_stack = 1;
        sys_timer_re_run(id);
        t_on_sys_stack = 0;
        CHECK(timer_start[id - 1] && timers[id - 1].due == 0u, "sys_timer_re_run in an interrupt: no clock read, restarted at the next ble_poll");
        k = fired37_sys;
        shim_t4 += 24000u * 15u; ble_poll();                    /* (35 ms from its add: it would have fired) */
        shim_t4 += 24000u * 29u; ble_poll();
        CHECK(fired37_sys == k, "29 ms after that ble_poll: not yet");
        shim_t4 += 24000u; ble_poll();
        CHECK(fired37_sys == k + 1, "30 ms: fired");
        sys_timer_del(id);
        CHECK(tm_used == n0, "deleted");
        id = sys_timer_add(0, cb37_sys, 1000);
        shim_t4 += 24000u * 500u; ble_poll();
        k = fired37_sys;
        CHECK(sys_timer_modify(id, 40) == 0 && sys_timer_modify(0, 40) == -1, "sys_timer_modify: a timer in use changed, none refused");
        shim_t4 += 24000u * 39u; ble_poll();
        CHECK(fired37_sys == k, "39 ms after the change: not yet");
        shim_t4 += 24000u; ble_poll();
        CHECK(fired37_sys == k + 1, "40 ms: fired, its new period counted from the change");
        sys_timer_del(id);
        x0 = usr_fired; i0 = usr_irq;
        t_on_sys_stack = 1;
        uid = usr_timeout_add((void *)3, cb37_usr, 6, 1);
        t_on_sys_stack = 0;
        CHECK(uid > USR_ID0 && usr_used == 1u && usr_irq == i0 + 1u, "a usr timeout added in an interrupt: its own table (id %u)", (unsigned)uid);
        u0 = usr_ms;
        for (k = 0; k < 69; k++) ble_usr_tick();
        USR_RUNS();
        CHECK(fired37_usr == 0 && usr_ms - u0 >= 6u, "6.9 ms of ticks: not yet (it waits 6 ms whole, from any point in a ms)");
        ble_usr_tick();
#if !BT_USR_IN_TICK
        CHECK(fired37_usr == 0 && usr_used == 1u, "BT_USR_IN_TICK 0: due, but the tick does not run it");
        ble_poll();
#endif
        CHECK(fired37_usr == 3 && usr_fired == x0 + 1u && usr_used == 0u, "7 ms: run (%s), once, its slot free",
              BT_USR_IN_TICK ? "by the tick" : "by the next ble_poll");
        uid = usr_timeout_add(0, cb37_usr, 2, 1);
        usr_timeout_del(uid);
        for (k = 0; k < 50; k++) ble_usr_tick();
        USR_RUNS();
        CHECK(fired37_usr == 3 && usr_used == 0u, "deleted before it is due: never runs");
        uid = usr_timeout_add(0, cb37_usr, 2, 1);
        sys_timeout_del(uid);
        CHECK(usr_used == 0u, "deleted through the sys call (an id from the other table): gone from the usr table");
        f0 = usr_fail;
        for (k = 0; k < (int)MAXUSR + 1; k++) uids[k] = usr_timeout_add(0, cb37_usr, 1000, 1);
        CHECK(!uids[k - 1] && usr_fail == f0 + 1u && usr_peak == MAXUSR, "a full usr table: the add refused and counted");
        for (k = 0; k < (int)MAXUSR; k++) usr_timeout_del(uids[k]);
        CHECK(usr_used == 0u, "all deleted");
        {   /* the interrupts put back as they were, never a bare enable: in an interrupt entered with them off (irq_nest 0),
             * inside the library's own counted section (the LE handlers hold the baseband's lock in one), in the main loop */
            uint16_t a, b, c;
            int bad37 = 0;
            t_on_sys_stack = 1; fm1_irq_off();
            a = sys_timeout_add(0, cb37_sys, 10); bad37 += shim_ie != 0u;
            b = usr_timeout_add(0, cb37_usr, 10, 1); bad37 += shim_ie != 0u;
            c = sys_timer_add(0, cb37_sys, 10); bad37 += shim_ie != 0u;
            sys_timer_re_run(c); bad37 += shim_ie != 0u;
            bad37 += sys_timer_modify(c, 20) != 0 || shim_ie != 0u;
            sys_timeout_del(b); bad37 += shim_ie != 0u;        /* (a usr id through the sys call) */
            usr_timeout_del(a); bad37 += shim_ie != 0u;        /* (a sys id through the usr call) */
            sys_timer_del(c); bad37 += shim_ie != 0u;
            CHECK(bad37 == 0 && irq_nest == 0u && tm_used == n0 && usr_used == 0u,
                  "in an interrupt entered with the interrupts off: every add, restart, change and delete leaves them off (%d)", bad37);
            fm1_irq_on();
            local_irq_disable();
            a = usr_timeout_add(0, cb37_usr, 10, 1); usr_timeout_del(a);
            a = sys_timeout_add(0, cb37_sys, 10); sys_timeout_del(a);
            CHECK(shim_ie == 0u && irq_nest == 1u, "inside the library's counted section: still off, its count untouched");
            local_irq_enable();
            CHECK(shim_ie == 1u && irq_nest == 0u, "its own enable turns them on");
            t_on_sys_stack = 0;
            a = sys_timeout_add(0, cb37_sys, 10); b = usr_timeout_add(0, cb37_usr, 10, 1);
            CHECK(shim_ie == 1u, "in the main loop: on after an add");
            sys_timeout_del(a); usr_timeout_del(b);
            CHECK(shim_ie == 1u && irq_nest == 0u && tm_used == n0 && usr_used == 0u, "and after a delete");
        }
        {   /* a callback as the library's (its own counted pair inside): run with the interrupts as found, and left so */
            lib_runs = 0;
            usr_timeout_add(0, cb37_lib, 1, 1);
#if BT_USR_IN_TICK
            t_on_sys_stack = 1; fm1_irq_off();                  /* (the tick's interrupt, entered with the interrupts off) */
            for (k = 0; k < 30; k++) ble_usr_tick();
            CHECK(lib_runs == 1 && lib_off_at_entry == 1 && shim_ie == 0u && irq_nest == 0u,
                  "the tick entered with the interrupts off: the callback runs with them off, the tick leaves them off after its pair");
            fm1_irq_on();
            usr_timeout_add(0, cb37_lib, 1, 1);
            for (k = 0; k < 30; k++) ble_usr_tick();
            t_on_sys_stack = 0;
            CHECK(lib_runs == 2 && lib_off_at_entry == 0 && shim_ie == 1u && irq_nest == 0u,
                  "the tick entered with them on: the callback runs with them on, and they stay on");
#else
            for (k = 0; k < 30; k++) ble_usr_tick();
            CHECK(lib_runs == 0, "BT_USR_IN_TICK 0: the tick runs no callback");
            ble_poll();
            CHECK(lib_runs == 1 && lib_off_at_entry == 0 && shim_ie == 1u && irq_nest == 0u, "ble_poll runs it, with the interrupts on");
#endif
        }
        {   /* the baseband's lock taken: a due usr timeout waits, never spins on it. Byte 33 of the handle, from the library's
             * own types (RF_ble.c: struct ble_hw_base, 32 bytes, an i8, then struct __spinlock), written here as a number so
             * that BB_LOCK_AT is checked against it */
            uint32_t h0 = usr_held, x1 = usr_fired;
            bb_hdl = fake_bb; fake_bb[33] = 1u;
            usr_timeout_add(0, cb37_usr, 1, 1);
            for (k = 0; k < 30; k++) ble_usr_tick();
            USR_RUNS();
            CHECK(usr_fired == x1 && usr_used == 1u && usr_held > h0, "the lock taken: due, it waits (TB %u)", (unsigned)(usr_held - h0));
            fake_bb[33] = 0u; fake_bb[32] = 0xFFu;              /* (free; the byte before it is not the lock) */
            for (k = 0; k < 10; k++) ble_usr_tick();
            USR_RUNS();
            CHECK(usr_fired == x1 + 1u && usr_used == 0u, "the lock free again: it runs at the next millisecond");
            bb_hdl = 0;
        }
        CHECK(irq_nest == 0u && shim_ie == 1u, "every interrupts-off paired with its interrupts-on");
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
