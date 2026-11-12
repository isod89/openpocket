/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The few operating-system services the JieLi Bluetooth controller library (third_party/jieli-ble) calls, written for
 * OpenPocket's main loop: a cooperative scheduler for the controller's tasks, task message queues, semaphores and
 * mutexes, software timers, a heap, and the interrupt registration. Signatures and error numbers follow the SDK's
 * public headers (include_lib/system/os/os_api.h, os_error.h, timer.h, asm/hwi.h; Apache-2.0).
 *
 * Tasks never run by themselves: ble_poll() (called from the main loop) resumes every task that is ready and returns
 * when all of them are waiting again. A task gives up the processor only inside os_taskq_pend, os_sem_pend,
 * os_mutex_pend or os_time_dly. The library's interrupt code (the classic baseband's, bt/bt_irq.c, and the LE baseband's)
 * posts messages and semaphores, deletes timers and allocates and frees memory; it never waits here: inside it cpu_in_irq()
 * is 1, the library's own test for "do not wait", and the waits refuse (the SDK's os_time_dly must not be called in an
 * interrupt). */
#include <stdint.h>
#include <stddef.h>
#include "fm1_irq.h"
#include "fm1_time.h"
#include "ble_os.h"
#include "ble_status.h"
#if OP_BT_AUDIO
#include "bt_stall.h"                               /* the stall record and the crumbs (firmware/bt) */
#define BT_CRUMB_OS(s) BT_CRUMB(s)
#ifndef BT_IRQ_PRIO
#define BT_IRQ_PRIO 5u                              /* the classic baseband's interrupts: above audio (3) and the timer (4), request_irq */
#endif
/* the priority request_irq gives them. Test builds 16-17 lowered it to 2 with DLY held at power-on (below the audio, as the stock
 * firmware has them); test build 18 no longer does (openpocket.c: test build 3 ran so and could not pair) */
static uint8_t bt_irq_prio = BT_IRQ_PRIO;
#else
#define BT_CRUMB_OS(s) ((void)0)
#endif

enum { OS_NO_ERR = 0, OS_TIMEOUT = 11, OS_TASKQ = 13, OS_TASK_NOT_EXIST = 14, OS_Q_FULL = 21, OS_Q_EMPTY = 22 };
enum { OS_TICK_MS = 10 };                    /* OS_TICKS_PER_SEC is 100 in the SDK */

extern void ble_ctx_swap(uint32_t **save, uint32_t *load);
extern void ble_ctx_enter(uint32_t **save, uint32_t *top, void (*fn)(void));

/* ---- interrupts: one level of nesting count, as the SDK's local_irq_disable / local_irq_enable */
static uint32_t irq_nest;
void local_irq_disable(void) { fm1_irq_off(); irq_nest++; }
void local_irq_enable(void) { if (irq_nest && !--irq_nest) fm1_irq_on(); }
void __local_irq_disable(void) { local_irq_disable(); }
void __local_irq_enable(void) { local_irq_enable(); }
void sys_local_irq_disable(void) { local_irq_disable(); }
void sys_local_irq_enable(void) { local_irq_enable(); }
int cpu_irq_disabled(void) { return !(fm1_icfg() & 0x200u); }   /* icfg bit 9: the global enable that cli and sti clear and set */
#if OP_BT_AUDIO
/* The library asks cpu_in_irq() before each wait it may make (bredr_link_event: a full controller queue, retried after
 * os_time_dly(2); __cmd_to_lmp_conn, lmp_malloc, bredr_tx_bulk_alloc, hci_packet_handler) and takes its "do not wait" path when it
 * is 1. The classic baseband's interrupts run library code that reaches those waits (bt/bt_irq.c), so they count themselves
 * here. Up to test build 10 this was always 0: a full queue in the interrupt would have spun for ever at its priority, above the
 * audio and the stall watch (a watchdog reset, no record), or switched tasks inside the interrupt (an audio gap of 20 ms or
 * more). The BLE image keeps 0: no interrupt of its calls the library's waits. */
volatile uint32_t bt_isr_depth;
/* Test build 28: or running on the system stack, where every interrupt runs (crt0 points ssp there; the main loop runs on
 * _ustack, the tasks on their own stacks in NOINIT; fm1_guard.h fm1_sp_in_isr, the stall record relies on it too). The LE
 * baseband's two interrupts (ble/ble_isr.S) jump into the library's own complete interrupt entries, so they cannot count
 * themselves in bt_isr_depth, and inside them the library took its task-side paths: RF_ble.c le_hw_suspend, which
 * ble_slot_schedule calls from the LE event interrupt for a scanning or initiating entity, then used ble_hw_disable, which
 * takes the baseband's spinlock (bb_hdl, testset, interrupts off) that the interrupt already holds, instead of
 * ble_hw_disable_isr. It spun there for ever with the interrupts off: test build 28 (the first build that scans) hung right
 * after Bluetooth's start, a watchdog reset with no stall record. Up to test build 27 no entity scanned or initiated, so
 * nothing in an LE interrupt asked cpu_in_irq(). */
#ifndef BLE_SP_IN_ISR
#define BLE_SP_IN_ISR() fm1_sp_in_isr()                          /* (fm1_guard.h, included by openpocket.c before this file) */
#endif
int cpu_in_irq(void) { return bt_isr_depth != 0u || BLE_SP_IN_ISR(); }
static uint32_t bt_wait_irq, bt_wait_main;                      /* waits refused inside an interrupt; waits asked by the main loop */
#define BLE_IN_IRQ() (cpu_in_irq() != 0)
#define BLE_WAIT_IRQ() (bt_wait_irq++, BT_CRUMB("wait in irq"))
#define BLE_WAIT_MAIN() (bt_wait_main++)
#else
int cpu_in_irq(void) { return 0; }
#define BLE_IN_IRQ() 0
#define BLE_WAIT_IRQ() ((void)0)
#define BLE_WAIT_MAIN() ((void)0)
#endif

/* ---- the scheduler's clock. TIMER4 / 24000 (fm1_uptime_ms) jumps from 178,956 back to 0 every 178.957 s (a 32-bit count at
 * 24 MHz), and a deadline set above 178,956 is never reached: up to test build 10 every periodic timer stopped for good at the
 * first wrap, and a task asleep across it never woke. This one extends TIMER4 at each call (ble_poll calls it far more often
 * than every 178 s) and wraps at 2^32 ms. Main loop and tasks only (no interrupt calls it: no lock) */
static uint32_t ble_ms_t4, ble_ms_n, ble_ms_rem;
uint32_t ble_ms(void)
{
    const uint32_t t = FM1_T4_CNT, d = t - ble_ms_t4;
    ble_ms_t4 = t;
    ble_ms_n += d / 24000u;
    ble_ms_rem += d % 24000u;
    if (ble_ms_rem >= 24000u) { ble_ms_rem -= 24000u; ble_ms_n++; }
    return ble_ms_n;
}
static uint32_t ble_deadline(uint32_t ms) { const uint32_t d = ble_ms() + ms; return d ? d : 1u; }   /* (0 means none) */

/* ---- heap: first fit with coalescing on free, 8-byte aligned, one static pool */
#ifndef HEAP_BYTES
#define HEAP_BYTES (24u * 1024u)                    /* (both images: the Bluetooth audio image's use, I2S's 4 KB included, was under 5 KB in
                                                     * test build 9; a build may pass -DHEAP_BYTES) */
#endif
typedef struct blk { uint32_t size; uint32_t used; } blk_t;       /* size includes this header */
#if OP_BT_AUDIO     /* (bt/app-bt.ld places it in the classic baseband's 64 KB window, with the controller's own memory) */
static uint8_t heap[HEAP_BYTES] __attribute__((aligned(8), section(".bss.bt_heap")));
#else
static uint8_t heap[HEAP_BYTES] __attribute__((aligned(8)));
#endif
static int heap_ready;
static uint32_t heap_free_bytes, heap_min_free;

static void heap_init(void)
{
    blk_t *b = (blk_t *)heap;
    b->size = HEAP_BYTES;
    b->used = 0;
    heap_free_bytes = heap_min_free = HEAP_BYTES;
    heap_ready = 1;
}

void *malloc(size_t n)
{
    uint32_t need = (uint32_t)((n + sizeof(blk_t) + 7u) & ~7u);
    uint8_t *p, *end = heap + HEAP_BYTES;
    local_irq_disable();
    if (!heap_ready)
        heap_init();
    for (p = heap; p < end; p += ((blk_t *)p)->size) {
        blk_t *b = (blk_t *)p;
        if (b->used || b->size < need)
            continue;
        if (b->size >= need + 16u) {              /* split */
            blk_t *r = (blk_t *)(p + need);
            r->size = b->size - need;
            r->used = 0;
            b->size = need;
        }
        b->used = 1;
        heap_free_bytes -= b->size;
        if (heap_free_bytes < heap_min_free)
            heap_min_free = heap_free_bytes;
        local_irq_enable();
        return p + sizeof(blk_t);
    }
#if OP_BT_AUDIO
    bt_heap_fails++; bt_heap_fail_size = (uint32_t)n;  /* (the library stops in a loop on a failed allocation: the stall record says) */
    BT_CRUMB("malloc FAILED");
#endif
    local_irq_enable();
    return 0;
}

void free(void *ptr)
{
    uint8_t *p, *end = heap + HEAP_BYTES;
    blk_t *b;
    if (!ptr)
        return;
    local_irq_disable();
    b = (blk_t *)((uint8_t *)ptr - sizeof(blk_t));
    b->used = 0;
    heap_free_bytes += b->size;
    for (p = heap; p < end;) {                    /* merge neighbouring free blocks */
        blk_t *c = (blk_t *)p, *n = (blk_t *)(p + c->size);
        if ((uint8_t *)n < end && !c->used && !n->used)
            c->size += n->size;
        else
            p += c->size;
    }
    local_irq_enable();
}

void *zalloc(size_t n) { void *p = malloc(n); if (p) memset(p, 0, n); return p; }
void *ram_malloc(unsigned long n) { return malloc(n); }
void ram_free(void *p) { free(p); }
unsigned int xPortGetPhysiceMemorySize(void) { return HEAP_BYTES; }
uint32_t ble_heap_free(void) { return heap_free_bytes; }
uint32_t ble_heap_min_free(void) { return heap_min_free; }

/* ---- tasks and their message queues */
#ifndef MAXTASK
#define MAXTASK 3                                   /* the library starts one ("btctrler"); room for two more */
#endif
#ifndef STACK_WORDS
#define STACK_WORDS 1024u                           /* 4 KB each: the radio start-up replay runs on one */
#endif
/* each task's messages: a ring of words, the messages back to back as [n][type][n - 1 words], so a long one fits as well as many
 * short ones. The library's messages carry data inline: the host task's events are the event's own bytes, (length + 3) / 4
 * words (btstack_task.c hci_packet_handler), and it reads up to 32 words a message (os_taskq_pend(.., 32)); the controller's
 * task reads 14, the encryption task 16. Up to test build 7 a message held 11 words after its type, so every event longer than
 * 44 bytes reached the host cut short. The same 768 bytes a task as before. */
#define QWORDS 192u
#define QMSG_WORDS 33u                              /* the type and 32 words: the longest message kept whole */
enum { T_FREE, T_READY, T_WAIT_Q, T_WAIT_SEM, T_SLEEP, T_DEAD };
typedef struct {
    char name[16];
    void (*fn)(void *);
    void *arg;
    uint32_t *sp;                                    /* saved stack pointer while suspended */
    uint32_t state;
    uint32_t wake_ms;                                /* T_SLEEP, and the timeout of the waits (0: none) */
    void *wait_on;
    int timed_out;
    uint32_t q[QWORDS];
    uint32_t qh, qw, qn;                             /* the next message's first word, the words queued, the messages queued */
#if OP_BT_AUDIO
    uint32_t qpeak, ovf;                             /* the most messages queued; the red zone found written (an overflow) */
    const char *cname;                               /* the name task_create was given: a constant string, so a crumb of it is
                                                      * still valid after a reset (the stall record; name[] is cleared at boot) */
#endif
    uint32_t stack[STACK_WORDS] __attribute__((aligned(8)));
} task_t;
#define BLE_RED_WORDS 16u                            /* the stack's lowest words: written only by a task about to overflow */
static task_t tasks[MAXTASK] __attribute__((section(".noinit.ble_stack")));   /* in NOINIT, out of the 96 KB RAM (app.ld brackets it as _ble_stack_lo.._hi) */
static task_t *cur;                                  /* the task being run, 0 in the scheduler */
static uint32_t *sched_sp;

static task_t *task_find(const char *name)
{
    int i;
    for (i = 0; i < MAXTASK; i++)
        if (tasks[i].state != T_FREE && !strcmp(tasks[i].name, name))
            return &tasks[i];
    return 0;
}

static void task_body(void)
{
    task_t *t = cur;
    t->fn(t->arg);
    t->state = T_DEAD;
    for (;;)
        ble_ctx_swap(&t->sp, sched_sp);
}

static void task_yield(void) { ble_ctx_swap(&cur->sp, sched_sp); }

/* NOINIT is not cleared at boot: the task table is, before the main loop first polls it; the stacks are painted, so
 * ble_stack_min_free can tell how deep the tasks went */
#define BLE_PAINT 0x5AA5C33Cu
void ble_os_init(void)
{
    uint32_t i, k;
    memset(tasks, 0, sizeof tasks);
    for (i = 0; i < MAXTASK; i++)
        for (k = 0; k < STACK_WORDS; k++) tasks[i].stack[k] = BLE_PAINT;
    (void)ble_ms();                                  /* (the clock's start: TIMER4 now) */
}
static uint32_t ble_task_free(const task_t *t)       /* bytes never used at the bottom of a task's stack */
{
    uint32_t k;
    for (k = 0; k < STACK_WORDS && t->stack[k] == BLE_PAINT; k++) ;
    return k * 4u;
}
uint32_t ble_stack_min_free(void)                    /* bytes never used at the bottom of the deepest task's stack */
{
    uint32_t i, k, m = STACK_WORDS * 4u;
    for (i = 0; i < MAXTASK; i++) {
        k = ble_task_free(&tasks[i]);
        if (tasks[i].state != T_FREE && k < m) m = k;
    }
    return m;
}

int task_create(void (*fn)(void *), void *arg, const char *name)
{
    int i;
    BT_CRUMB_OS(name);                               /* (the task names are the library's constant strings) */
    for (i = 0; i < MAXTASK; i++)
        if (tasks[i].state == T_FREE || tasks[i].state == T_DEAD)
            break;
    if (i == MAXTASK)
        return -1;
    memset(&tasks[i], 0, sizeof(task_t) - sizeof(tasks[i].stack));
    strncpy(tasks[i].name, name, sizeof(tasks[i].name) - 1);
#if OP_BT_AUDIO
    tasks[i].cname = name;
#endif
    tasks[i].fn = fn;
    tasks[i].arg = arg;
    tasks[i].sp = 0;                                 /* 0: not started, ble_poll enters task_body on first run */
    tasks[i].state = T_READY;
    return 0;
}

int task_kill(const char *name)
{
    task_t *t = task_find(name);
    if (!t)
        return OS_TASK_NOT_EXIST;
    t->state = T_FREE;
    return 0;
}

const char *os_current_task(void) { return cur ? cur->name : "main"; }
#if OP_BT_AUDIO
/* task i for the log (bt_a2dp.c): its name, state (T_*), what it waits on (a semaphore or mutex), its queued messages, the
 * bytes of its stack never used, the most messages it had queued, and whether its red zone was written */
typedef struct { const char *name; uint32_t state; const void *wait_on; uint32_t queued, free, qpeak, ovf; } ble_task_info_t;
int ble_task_info(uint32_t i, ble_task_info_t *r)
{
    if (i >= MAXTASK) return 0;
    r->name = tasks[i].name; r->state = tasks[i].state; r->wait_on = tasks[i].wait_on; r->queued = tasks[i].qn;
    r->free = ble_task_free(&tasks[i]); r->qpeak = tasks[i].qpeak; r->ovf = tasks[i].ovf;
    return 1;
}
static uint32_t bt_stack_ovf;                        /* red zones found written, all tasks */
#endif
/* what a task is blocked on, for the failure line: the semaphore or mutex address, or its state when it is not waiting on one */
uint32_t ble_task_waiting(const char *name)
{
    task_t *t = task_find(name);
    return !t ? 0xFFu : t->state == T_WAIT_SEM ? (uint32_t)(uintptr_t)t->wait_on : t->state;
}

int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    task_t *t = task_find(name);
    uint32_t n, w, i;
    if (!t) {
#if OP_BT_AUDIO
        bt_q_lost++; BT_CRUMB("taskq: no such task");
#endif
        return OS_TASK_NOT_EXIST;
    }
    if (argc < 0) argc = 0;
    n = 1u + ((uint32_t)argc < QMSG_WORDS - 1u ? (uint32_t)argc : QMSG_WORDS - 1u);   /* the type and the words kept */
#if OP_BT_AUDIO
    if ((uint32_t)argc > QMSG_WORDS - 1u) BT_CRUMB("taskq: a message cut");
#endif
    local_irq_disable();
    if (t->qw + 1u + n > QWORDS) {
#if OP_BT_AUDIO
        bt_q_lost++; BT_CRUMB(t->cname);                        /* (a post refused: the full task's name in the crumbs; the
                                                                 * library retries some, from a task, after a sleep) */
#endif
        local_irq_enable();
        return OS_Q_FULL;
    }
    w = (t->qh + t->qw) % QWORDS;
    t->q[w] = n; w = (w + 1u) % QWORDS;
    t->q[w] = (uint32_t)type; w = (w + 1u) % QWORDS;
    for (i = 1; i < n; i++) { t->q[w] = (uint32_t)argv[i - 1u]; w = (w + 1u) % QWORDS; }
    t->qw += 1u + n;
    t->qn++;
#if OP_BT_AUDIO
    if (t->qn > bt_q_peak) bt_q_peak = t->qn;
    if (t->qn > t->qpeak) t->qpeak = t->qn;
#endif
    if (t->state == T_WAIT_Q)
        t->state = T_READY;
    local_irq_enable();
#if OP_BT_AUDIO
    bt_taskq_spy(t->name, type, argc, argv);                    /* (the HCI conversation: the host's events, the controller's commands) */
#endif
    return 0;
}

/* the SDK's os_taskq_pend blocks until a message is queued; argv[0] is the type, argv[1..] the words */
int os_taskq_pend(const char *fmt, int *argv, int argc)
{
    uint32_t n, r, i;
    (void)fmt;
    if (BLE_IN_IRQ()) { BLE_WAIT_IRQ(); return OS_Q_EMPTY; }   /* (cur is the task the interrupt cut into: not its queue) */
    if (!cur) {
        BLE_WAIT_MAIN();
        return OS_TIMEOUT;
    }
    for (;;) {
        local_irq_disable();
        if (cur->qn) {
            r = cur->qh;
            n = cur->q[r]; r = (r + 1u) % QWORDS;
            for (i = 0; i < n; i++) {                       /* (argc: the words the reader has room for) */
                if ((int)i < argc) argv[i] = (int)cur->q[r];
                r = (r + 1u) % QWORDS;
            }
            cur->qh = r;
            cur->qw -= 1u + n;
            cur->qn--;
            local_irq_enable();
            return OS_TASKQ;
        }
        cur->state = T_WAIT_Q;
        local_irq_enable();
        task_yield();
    }
}

void os_time_dly(int ticks)
{
    if (BLE_IN_IRQ()) { BLE_WAIT_IRQ(); return; }
    if (!cur) {
        BLE_WAIT_MAIN();                             /* (a timer's callback, run by ble_poll: it cannot wait) */
        return;
    }
    if (ticks < 1)
        ticks = 1;
    cur->wake_ms = ble_deadline((uint32_t)ticks * OS_TICK_MS);
    cur->state = T_SLEEP;
    task_yield();
}

/* ---- semaphores and mutexes. The SDK's OS_SEM / OS_MUTEX are opaque and at least 16 bytes: the first word is the
 * count (a mutex: 1 free, 0 taken), the second the owner for the mutex. */
typedef struct { volatile int32_t count; void *owner; uint32_t depth; } sem_t;   /* (depth: a mutex taken again by its owner) */

int os_sem_create(void *s, int cnt) { ((sem_t *)s)->count = cnt; ((sem_t *)s)->owner = 0; return 0; }
int os_sem_set(void *s, uint16_t cnt) { ((sem_t *)s)->count = cnt; return 0; }
int os_sem_post(void *s)
{
    int i;
    local_irq_disable();
    ((sem_t *)s)->count++;
    for (i = 0; i < MAXTASK; i++)
        if (tasks[i].state == T_WAIT_SEM && tasks[i].wait_on == s)
            tasks[i].state = T_READY;
    local_irq_enable();
    return 0;
}

int os_sem_pend(void *s, int timeout)
{
    sem_t *sm = s;
    uint32_t until = timeout ? ble_deadline((uint32_t)timeout * OS_TICK_MS) : 0;
    for (;;) {
        local_irq_disable();
        if (sm->count > 0) {
            sm->count--;
            local_irq_enable();
            return 0;
        }
        if (BLE_IN_IRQ()) {                          /* (taken if free, never waited for: the library then takes its other path) */
            local_irq_enable();
            BLE_WAIT_IRQ();
            return OS_TIMEOUT;
        }
        if (!cur || (until && (int32_t)(ble_ms() - until) >= 0)) {
            local_irq_enable();
            if (!cur) BLE_WAIT_MAIN();
            return OS_TIMEOUT;
        }
        cur->state = T_WAIT_SEM;
        cur->wait_on = s;
        cur->wake_ms = until;
        local_irq_enable();
        task_yield();
    }
}

/* The SDK's mutexes are recursive: the task that holds one may take it again, and gives it back as often. The library relies
 * on it (remote_device_db.c updata_last_link_key takes the device database's mutex, then calls get_link_key and put_link_key,
 * which take it again). Up to test build 9 a mutex was a plain semaphore here, so the host task blocked itself for good the
 * first time a phone's link key was stored, right after the link came up. The main loop (no task) counts as one owner. */
int os_mutex_create(void *m) { sem_t *x = m; os_sem_create(m, 1); x->depth = 0; return 0; }
int os_mutex_del(void *m, int force) { (void)m; (void)force; return 0; }
int os_mutex_pend(void *m, int timeout)
{
    sem_t *x = m;
    void *const me = cur ? (void *)cur : (void *)&sched_sp;
    int r;
    /* (not inside an interrupt: there cur is the task it cut into, which is not the one asking) */
    if (!BLE_IN_IRQ() && x->depth && x->owner == me) { x->depth++; return 0; }
    r = os_sem_pend(m, timeout);
    if (r == 0) { x->owner = me; x->depth = 1; }
    return r;
}
int os_mutex_post(void *m)
{
    sem_t *x = m;
    if (x->depth > 1u) { x->depth--; return 0; }
    x->depth = 0; x->owner = 0;
    return os_sem_post(m);
}

/* ---- software timers (sys_timer_add: periodic, sys_timeout_add: once), run from ble_poll */
#define MAXTIMER 16
typedef struct {
    void (*fn)(void *);
    void *priv;
    uint32_t period, due;
    uint8_t used, once;
} timer_t_;
static timer_t_ timers[MAXTIMER];

#if OP_BT_AUDIO
/* Test build 37: the timers are added and deleted from interrupts too: the LE controller's multi-link scheduler
 * (multilink_schedule.c mult_slave_or_master_event_enter, in the LE event interrupt) arms usr and sys timeouts for its
 * advertising and connection-update windows. So a slot is claimed with the interrupts off and the interrupts are then put
 * back as they were (tm_irq_save / tm_irq_restore: never a bare enable, whatever the interrupt's entry left), and an
 * interrupt never calls ble_ms() (main loop and tasks only): a timer added there starts counting at the next ble_poll
 * (start = 1). The usr timeouts have their own table, run every millisecond by the 10 kHz tick (ble_usr_tick, below): the
 * SDK's usr timer is that precise, and the scheduler's are 6 ms windows; from ble_poll they came a main-loop pass late, tens
 * of ms. BT_USR_IN_TICK 0 runs them from ble_poll again, as up to test build 36, the rest kept. The log's words, on the HCI
 * line (bt/bt_a2dp.c bt_timers_text): TS and TU each table's slots in use / the most ever, TF the adds refused (table full:
 * the callback never comes), TI the adds from an interrupt, TR the usr timeouts run, TB the times one waited because the
 * baseband's lock was taken (usr_run) */
#ifndef BT_USR_IN_TICK
#define BT_USR_IN_TICK 1
#endif
static uint32_t tm_irq_save(void) { const uint32_t off = (uint32_t)cpu_irq_disabled(); fm1_irq_off(); return off; }
static void tm_irq_restore(uint32_t off) { if (!off) fm1_irq_on(); }
static uint8_t timer_start[MAXTIMER];
static uint32_t tm_used, tm_peak, tm_fail, tm_irq;
static uint16_t timer_add(void *priv, void (*fn)(void *), uint32_t ms, int once)
{
    const int irq = cpu_in_irq();
    const uint32_t s = tm_irq_save();
    int i;
    if (irq) tm_irq++;
    for (i = 0; i < MAXTIMER; i++)
        if (!timers[i].used) {
            timers[i].fn = fn; timers[i].priv = priv; timers[i].period = ms ? ms : 1u;
            timer_start[i] = (uint8_t)irq;
            timers[i].due = irq ? 0u : ble_ms() + timers[i].period;
            timers[i].once = (uint8_t)once; timers[i].used = 1;
            if (++tm_used > tm_peak) tm_peak = tm_used;
            tm_irq_restore(s);
            return (uint16_t)(i + 1);
        }
    tm_fail++;
    tm_irq_restore(s);
    return 0;
}
#define USR_ID0 0x100u                              /* (usr ids: 0x101..; the sys table's are 1..MAXTIMER) */
static void usr_del(uint16_t id);
static void timer_del(uint16_t id)
{
    uint32_t s;
    if (id > USR_ID0) { usr_del(id); return; }      /* (an id from the other table: deleted there) */
    s = tm_irq_save();
    if (id && id <= MAXTIMER && timers[id - 1].used) { timers[id - 1].used = 0; tm_used--; }
    tm_irq_restore(s);
}
static void timer_restart(uint16_t id)              /* (its period again from now; from an interrupt, from the next ble_poll) */
{
    const int irq = cpu_in_irq();
    const uint32_t s = tm_irq_save();
    if (id && id <= MAXTIMER && timers[id - 1].used) {
        timer_start[id - 1] = (uint8_t)irq;
        timers[id - 1].due = irq ? 0u : ble_ms() + timers[id - 1].period;
    }
    tm_irq_restore(s);
}

/* the usr timeouts: one-shot, run in the 10 kHz tick's interrupt as the SDK's usr timer runs them in one. Their callbacks
 * (multilink_schedule.c): conn_update_timer, adv_event_open and mult_adv_event_end store m_s flags; adv_keep_open
 * (ble_hw_enable: register writes, no lock) and scan_or_init_win_close (ble_hw_disable: the baseband's lock, and a wait for
 * the radio to stop) run whole with the interrupts off. Every holder of that lock (RF_ble.c: the two LE interrupt handlers
 * from their first line to their last, ble_hw_disable) takes it after a counted local_irq_disable, so the tick, which runs
 * only with the interrupts on, never finds it taken. Should it ever, a due callback waits for the next millisecond instead
 * of spinning on it above the lock's holder (TB): the lock byte is read before each one runs */
#define MAXUSR 16u                                  /* (the SDK allocates them: the library never expects a refusal) */
#define BB_LOCK_AT 33u                              /* (RF_ble.c struct ble_baseband_layer_handle: struct ble_hw_base, 8 ids
                                                     * and 4 pointers, 32 bytes; an i8; then its struct __spinlock, one byte) */
extern uint8_t *bb_hdl;
typedef struct { void (*fn)(void *); void *priv; uint32_t due; uint8_t used; } usr_t_;
static usr_t_ usrs[MAXUSR];
static volatile uint32_t usr_ms;                    /* the usr timers' clock: ms, counted by the tick alone */
static uint32_t usr_sub, usr_used, usr_peak, usr_fail, usr_irq, usr_fired, usr_held;
uint16_t usr_timeout_add(void *p, void (*f)(void *), uint32_t ms, uint8_t prio)
{
    const int irq = cpu_in_irq();
    const uint32_t s = tm_irq_save();
    uint32_t i;
    (void)prio;
    if (irq) usr_irq++;
    for (i = 0; i < MAXUSR; i++)
        if (!usrs[i].used) {
            usrs[i].fn = f; usrs[i].priv = p; usrs[i].due = usr_ms + (ms ? ms : 1u) + 1u;   /* (+1: never less than ms) */
            usrs[i].used = 1;
            if (++usr_used > usr_peak) usr_peak = usr_used;
            tm_irq_restore(s);
            return (uint16_t)(USR_ID0 + i + 1u);
        }
    usr_fail++;
    tm_irq_restore(s);
    return 0;
}
static void usr_del(uint16_t id)
{
    const uint32_t s = tm_irq_save();
    if (id > USR_ID0 && id <= USR_ID0 + MAXUSR && usrs[id - USR_ID0 - 1u].used) { usrs[id - USR_ID0 - 1u].used = 0; usr_used--; }
    tm_irq_restore(s);
}
void usr_timeout_del(uint16_t id) { if (id > USR_ID0) usr_del(id); else timer_del(id); }
static int bb_locked(void) { const volatile uint8_t *const h = bb_hdl; return h != 0 && h[BB_LOCK_AT] != 0u; }
static void usr_run(void)                           /* the due usr timeouts: from the tick, or from ble_poll (BT_USR_IN_TICK 0) */
{
    uint32_t i;
    if (!usr_used) return;
    for (i = 0; i < MAXUSR; i++) {
        void (*fn)(void *) = 0;
        void *priv = 0;
        const uint32_t s = tm_irq_save();
        if (usrs[i].used && (int32_t)(usr_ms - usrs[i].due) >= 0) {
            if (bb_locked()) { usr_held++; tm_irq_restore(s); return; }   /* (it and the rest: the next millisecond) */
            fn = usrs[i].fn; priv = usrs[i].priv; usrs[i].used = 0; usr_used--;
        }
        tm_irq_restore(s);
        if (fn) {
            usr_fired++;
            fn(priv);                               /* (with the interrupts as the tick found them) */
            if (s) fm1_irq_off();                   /* (the callback's own counted pair ends in an enable: put back) */
        }
    }
}
static void ble_usr_tick(void)                      /* (the 10 kHz tick, bt/bt_a2dp.c bt_stall_tick: every 10th a millisecond) */
{
    if (++usr_sub < 10u) return;
    usr_sub = 0;
    usr_ms++;
#if BT_USR_IN_TICK
    usr_run();
#endif
}
uint16_t sys_timer_add(void *p, void (*f)(void *), uint32_t ms) { return timer_add(p, f, ms, 0); }
uint16_t sys_timeout_add(void *p, void (*f)(void *), uint32_t ms) { return timer_add(p, f, ms, 1); }
void sys_timer_del(uint16_t id) { timer_del(id); }
void sys_timeout_del(uint16_t id) { timer_del(id); }
void sys_timer_re_run(uint16_t id) { timer_restart(id); }
int sys_timer_modify(uint16_t id, uint32_t ms)
{
    int r = -1;
    const uint32_t s = tm_irq_save();
    if (id && id <= MAXTIMER && timers[id - 1].used) { timers[id - 1].period = ms ? ms : 1u; timer_restart(id); r = 0; }
    tm_irq_restore(s);
    return r;
}
#else
static uint16_t timer_add(void *priv, void (*fn)(void *), uint32_t ms, int once)
{
    int i;
    for (i = 0; i < MAXTIMER; i++)
        if (!timers[i].used) {
            timers[i].fn = fn; timers[i].priv = priv; timers[i].period = ms ? ms : 1u;
            timers[i].due = ble_ms() + timers[i].period; timers[i].once = (uint8_t)once; timers[i].used = 1;
            return (uint16_t)(i + 1);
        }
    return 0;
}
static void timer_del(uint16_t id) { if (id && id <= MAXTIMER) timers[id - 1].used = 0; }

uint16_t sys_timer_add(void *p, void (*f)(void *), uint32_t ms) { return timer_add(p, f, ms, 0); }
uint16_t sys_timeout_add(void *p, void (*f)(void *), uint32_t ms) { return timer_add(p, f, ms, 1); }
uint16_t usr_timeout_add(void *p, void (*f)(void *), uint32_t ms, uint8_t prio) { (void)prio; return timer_add(p, f, ms, 1); }
void sys_timer_del(uint16_t id) { timer_del(id); }
void sys_timeout_del(uint16_t id) { timer_del(id); }
void usr_timeout_del(uint16_t id) { timer_del(id); }
void sys_timer_re_run(uint16_t id) { if (id && id <= MAXTIMER && timers[id - 1].used) timers[id - 1].due = ble_ms() + timers[id - 1].period; }
int sys_timer_modify(uint16_t id, uint32_t ms)
{
    if (!id || id > MAXTIMER || !timers[id - 1].used)
        return -1;
    timers[id - 1].period = ms ? ms : 1u;
    timers[id - 1].due = ble_ms() + timers[id - 1].period;
    return 0;
}
#endif
void sys_timer_set_user_data(uint16_t id, void *p) { if (id && id <= MAXTIMER) timers[id - 1].priv = p; }
void *sys_timer_get_user_data(uint16_t id) { return id && id <= MAXTIMER ? timers[id - 1].priv : 0; }
uint32_t sys_timer_get_ms(void) { return ble_ms(); }

/* ble_isr.S: counts the baseband interrupts, then continues in the library's own interrupt function (saved here) */
void (*ble_irq_target_ev)(void), (*ble_irq_target_rx)(void);
extern void ble_isr_ev(void), ble_isr_rx(void);
#if OP_BT_AUDIO
void (*bt_irq_target_br)(void), (*bt_irq_target_slot)(void);
extern void bt_isr_br(void), bt_isr_slot(void);
#endif

/* ---- interrupt registration: the library hands over its own interrupt functions (already complete interrupt entries) */
void request_irq(uint8_t idx, uint8_t prio, void (*handler)(void), uint8_t cpu)
{
    (void)cpu;
    ble_stage_set(BLE_STAGE_BB);                    /* le_hw_init: the baseband is set up and asks for its interrupts */
    if (idx == 45u) { ble_irq_target_ev = handler; handler = ble_isr_ev; }   /* ble_isr.S counts, then runs the library's entry */
    else if (idx == 29u) { ble_irq_target_rx = handler; handler = ble_isr_rx; }
#if OP_BT_AUDIO                                     /* the classic baseband's two lines, counted the same way (bt/bt_isr.S) */
    /* ... and above the audio interrupt. The library asks for 2, stock's number, below stock's audio interrupt (4), which runs
     * its dry chain in blocks of 64 frames (1.45 ms); OpenPocket's runs its DSP work in blocks of 128 (px_audio.h ALNK0, at 3),
     * up to 2.9 ms, twice the wait. The
     * classic handlers have slot deadlines (625 us: the page response, the alarm the link scheduler arms for its next task);
     * one handled late arms the next alarm in the past and the baseband falls silent (test build 3: IRQ 40 stopped, no link).
     * Above the 10 kHz timer (4) too: its USB poll can take a while. The handlers are short. Since test build 5 the line enters
     * OpenPocket's own entry and the C bodies of bt/bt_irq.c (the library's handler, kept in the target, is not called: see there) */
    else if (idx == 40u) { bt_irq_target_br = handler; handler = bt_isr_br; prio = bt_irq_prio; BT_CRUMB("request_irq 40"); }
    else if (idx == 41u) { bt_irq_target_slot = handler; handler = bt_isr_slot; prio = bt_irq_prio; BT_CRUMB("request_irq 41"); }
    else BT_CRUMB("request_irq");
#endif
    fm1_irq_attach(idx, handler, prio);
}
void unrequest_irq(uint8_t idx, uint8_t cpu) { (void)cpu; FM1_ICFG(idx) &= ~(1u << ((idx & 7u) * 4u)); }
void bit_set_ie(uint8_t idx, uint8_t cpu) { (void)cpu; FM1_ICFG(idx) |= 1u << ((idx & 7u) * 4u); }
void bit_clr_ie(uint8_t idx, uint8_t cpu) { (void)cpu; FM1_ICFG(idx) &= ~(1u << ((idx & 7u) * 4u)); }
uint8_t irq_read(uint32_t idx) { return (uint8_t)((FM1_ICFG(idx) >> ((idx & 7u) * 4u)) & 0xFu); }

/* ---- the scheduler */
void ble_poll(void)
{
    int i, ran;
    fm1_wdt_feed();
    uint32_t now = ble_ms();
    for (i = 0; i < MAXTIMER; i++) {
        timer_t_ *t = &timers[i];
        void (*fn)(void *) = 0;
        void *priv = 0;
        local_irq_disable();                         /* (claimed whole: an interrupt may delete a timer, sys_timeout_del) */
#if OP_BT_AUDIO
        if (t->used && timer_start[i]) { t->due = now + t->period; timer_start[i] = 0; }   /* (test build 37: added from an
                                                                                         * interrupt: its period from now) */
#endif
        if (t->used && (int32_t)(now - t->due) >= 0) {
            fn = t->fn; priv = t->priv;
            if (t->once)
                t->used = 0;
            else
                t->due = now + t->period;
#if OP_BT_AUDIO
            if (t->once) tm_used--;
#endif
        }
        local_irq_enable();
        if (fn) {
            ble_cnt_timer++;
            fn(priv);
        }
    }
#if OP_BT_AUDIO && !BT_USR_IN_TICK
    usr_run();                                      /* (the usr timeouts, as up to test build 36: see usr_run) */
#endif
    do {
        ran = 0;
        now = ble_ms();
        for (i = 0; i < MAXTASK; i++) {
            task_t *t = &tasks[i];
            if ((t->state == T_SLEEP || t->state == T_WAIT_SEM) && t->wake_ms && (int32_t)(now - t->wake_ms) >= 0)
                t->state = T_READY;
            if (t->state != T_READY)
                continue;
            cur = t;
            ran = 1;
            if (!t->sp)
                ble_ctx_enter(&sched_sp, t->stack + STACK_WORDS, task_body);
            else
                ble_ctx_swap(&sched_sp, t->sp);
            cur = 0;
#if OP_BT_AUDIO
            if (!t->ovf) {                           /* the red zone: still painted, or the task came within 64 bytes of its end */
                uint32_t k;
                for (k = 0; k < BLE_RED_WORDS && t->stack[k] == BLE_PAINT; k++) ;
                if (k < BLE_RED_WORDS) { t->ovf = 1; bt_stack_ovf++; BT_CRUMB_OS(t->cname); BT_CRUMB_OS("stack red zone written"); }
            }
#endif
        }
    } while (ran);
}
