/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The few operating-system services the JieLi Bluetooth controller library (third_party/jieli-ble) calls, written for
 * OpenPocket's main loop: a cooperative scheduler for the controller's tasks, task message queues, semaphores and
 * mutexes, software timers, a heap, and the interrupt registration. Signatures and error numbers follow the SDK's
 * public headers (include_lib/system/os/os_api.h, os_error.h, timer.h, asm/hwi.h; Apache-2.0).
 *
 * Tasks never run by themselves: ble_poll() (called from the main loop) resumes every task that is ready and returns
 * when all of them are waiting again. A task gives up the processor only inside os_taskq_pend, os_sem_pend,
 * os_mutex_pend or os_time_dly; interrupts only post messages and semaphores. */
#include <stdint.h>
#include <stddef.h>
#include "fm1_irq.h"
#include "fm1_time.h"
#include "ble_os.h"
#include "ble_status.h"

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
int cpu_in_irq(void) { return 0; }

/* ---- heap: first fit with coalescing on free, 8-byte aligned, one static pool */
#define HEAP_BYTES (24u * 1024u)
typedef struct blk { uint32_t size; uint32_t used; } blk_t;       /* size includes this header */
static uint8_t heap[HEAP_BYTES] __attribute__((aligned(8)));
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
#define MAXTASK 3                                   /* the library starts one ("btctrler"); room for two more */
#define STACK_WORDS 1024u                           /* 4 KB each: the radio start-up replay runs on one */
#define QDEPTH 16
#define QARGS 12                                    /* type + up to 11 words */
typedef struct { int v[QARGS]; } msg_t;
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
    msg_t q[QDEPTH];
    uint32_t qh, qn;
    uint32_t stack[STACK_WORDS] __attribute__((aligned(8)));
} task_t;
static task_t tasks[MAXTASK] __attribute__((section(".bss.ble_stack")));   /* app.ld brackets it as _ble_stack_lo.._hi */
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

int task_create(void (*fn)(void *), void *arg, const char *name)
{
    int i;
    for (i = 0; i < MAXTASK; i++)
        if (tasks[i].state == T_FREE || tasks[i].state == T_DEAD)
            break;
    if (i == MAXTASK)
        return -1;
    memset(&tasks[i], 0, sizeof(task_t) - sizeof(tasks[i].stack));
    strncpy(tasks[i].name, name, sizeof(tasks[i].name) - 1);
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
/* what a task is blocked on, for the failure line: the semaphore or mutex address, or its state when it is not waiting on one */
uint32_t ble_task_waiting(const char *name)
{
    task_t *t = task_find(name);
    return !t ? 0xFFu : t->state == T_WAIT_SEM ? (uint32_t)(uintptr_t)t->wait_on : t->state;
}

int os_taskq_post_type(const char *name, int type, int argc, int *argv)
{
    task_t *t = task_find(name);
    msg_t *m;
    int i;
    if (!t)
        return OS_TASK_NOT_EXIST;
    local_irq_disable();
    if (t->qn == QDEPTH) {
        local_irq_enable();
        return OS_Q_FULL;
    }
    m = &t->q[(t->qh + t->qn++) % QDEPTH];
    m->v[0] = type;
    for (i = 0; i < argc && i < QARGS - 1; i++)
        m->v[1 + i] = argv[i];
    if (t->state == T_WAIT_Q)
        t->state = T_READY;
    local_irq_enable();
    return 0;
}

/* the SDK's os_taskq_pend blocks until a message is queued; argv[0] is the type, argv[1..] the words */
int os_taskq_pend(const char *fmt, int *argv, int argc)
{
    msg_t m;
    int i;
    (void)fmt;
    if (!cur)
        return OS_TIMEOUT;
    for (;;) {
        local_irq_disable();
        if (cur->qn) {
            m = cur->q[cur->qh];
            cur->qh = (cur->qh + 1u) % QDEPTH;
            cur->qn--;
            local_irq_enable();
            for (i = 0; i < argc && i < QARGS; i++)
                argv[i] = m.v[i];
            return OS_TASKQ;
        }
        cur->state = T_WAIT_Q;
        local_irq_enable();
        task_yield();
    }
}

void os_time_dly(int ticks)
{
    if (!cur)
        return;
    if (ticks < 1)
        ticks = 1;
    cur->wake_ms = fm1_uptime_ms() + (uint32_t)ticks * OS_TICK_MS;
    cur->state = T_SLEEP;
    task_yield();
}

/* ---- semaphores and mutexes. The SDK's OS_SEM / OS_MUTEX are opaque and at least 16 bytes: the first word is the
 * count (a mutex: 1 free, 0 taken), the second the owner for the mutex. */
typedef struct { volatile int32_t count; void *owner; } sem_t;

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
    uint32_t until = timeout ? fm1_uptime_ms() + (uint32_t)timeout * OS_TICK_MS : 0;
    for (;;) {
        local_irq_disable();
        if (sm->count > 0) {
            sm->count--;
            local_irq_enable();
            return 0;
        }
        if (!cur || (until && (int32_t)(fm1_uptime_ms() - until) >= 0)) {
            local_irq_enable();
            return OS_TIMEOUT;
        }
        cur->state = T_WAIT_SEM;
        cur->wait_on = s;
        cur->wake_ms = until;
        local_irq_enable();
        task_yield();
    }
}

int os_mutex_create(void *m) { return os_sem_create(m, 1); }
int os_mutex_del(void *m, int force) { (void)m; (void)force; return 0; }
int os_mutex_pend(void *m, int timeout) { return os_sem_pend(m, timeout); }
int os_mutex_post(void *m) { return os_sem_post(m); }

/* ---- software timers (sys_timer_add: periodic, sys_timeout_add: once), run from ble_poll */
#define MAXTIMER 16
typedef struct {
    void (*fn)(void *);
    void *priv;
    uint32_t period, due;
    uint8_t used, once;
} timer_t_;
static timer_t_ timers[MAXTIMER];

static uint16_t timer_add(void *priv, void (*fn)(void *), uint32_t ms, int once)
{
    int i;
    for (i = 0; i < MAXTIMER; i++)
        if (!timers[i].used) {
            timers[i].fn = fn; timers[i].priv = priv; timers[i].period = ms ? ms : 1u;
            timers[i].due = fm1_uptime_ms() + timers[i].period; timers[i].once = (uint8_t)once; timers[i].used = 1;
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
void sys_timer_re_run(uint16_t id) { if (id && id <= MAXTIMER && timers[id - 1].used) timers[id - 1].due = fm1_uptime_ms() + timers[id - 1].period; }
int sys_timer_modify(uint16_t id, uint32_t ms)
{
    if (!id || id > MAXTIMER || !timers[id - 1].used)
        return -1;
    timers[id - 1].period = ms ? ms : 1u;
    timers[id - 1].due = fm1_uptime_ms() + timers[id - 1].period;
    return 0;
}
void sys_timer_set_user_data(uint16_t id, void *p) { if (id && id <= MAXTIMER) timers[id - 1].priv = p; }
void *sys_timer_get_user_data(uint16_t id) { return id && id <= MAXTIMER ? timers[id - 1].priv : 0; }
uint32_t sys_timer_get_ms(void) { return fm1_uptime_ms(); }

/* ble_isr.S: counts the baseband interrupts, then continues in the library's own interrupt function (saved here) */
void (*ble_irq_target_ev)(void), (*ble_irq_target_rx)(void);
extern void ble_isr_ev(void), ble_isr_rx(void);

/* ---- interrupt registration: the library hands over its own interrupt functions (already complete interrupt entries) */
void request_irq(uint8_t idx, uint8_t prio, void (*handler)(void), uint8_t cpu)
{
    (void)cpu;
    ble_stage_set(BLE_STAGE_BB);                    /* le_hw_init: the baseband is set up and asks for its interrupts */
    if (idx == 45u) { ble_irq_target_ev = handler; handler = ble_isr_ev; }   /* ble_isr.S counts, then runs the library's entry */
    else if (idx == 29u) { ble_irq_target_rx = handler; handler = ble_isr_rx; }
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
    uint32_t now = fm1_uptime_ms();
    for (i = 0; i < MAXTIMER; i++) {
        timer_t_ *t = &timers[i];
        if (t->used && (int32_t)(now - t->due) >= 0) {
            if (t->once)
                t->used = 0;
            else
                t->due = now + t->period;
            ble_cnt_timer++;
            t->fn(t->priv);
        }
    }
    do {
        ran = 0;
        now = fm1_uptime_ms();
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
        }
    } while (ran);
}
