/* SPDX-License-Identifier: GPL-3.0-only */
/* The Bluetooth images' task message queues (firmware/ble/ble_os.c os_taskq_post_type / os_taskq_pend) on a PC, with the
 * library's message sizes: the host task's events are posted inline, (length + 3) / 4 words, and read 32 words at a time
 * (btstack_task.c); the controller's task reads 14. Checks: a message comes back whole, type first, up to 32 words; a longer
 * one is cut at 32; the reader gets only the words it has room for and the next message is not disturbed; a full queue says
 * so (OS_Q_FULL) and keeps what it holds; thousands of messages of every size through the ring, wrapping, in order. */
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
void ble_ctx_swap(uint32_t **save, uint32_t *load) { (void)save; (void)load; }
void ble_ctx_enter(uint32_t **save, uint32_t *top, void (*fn)(void)) { (void)save; (void)top; (void)fn; }
void (*ble_irq_target_ev)(void), (*ble_irq_target_rx)(void);
void ble_isr_ev(void) { }
void ble_isr_rx(void) { }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void body(void *a) { (void)a; }
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
    printf(fails ? "ble taskq: %d FAILED\n" : "ble taskq: all passed\n", fails);
    return fails != 0;
}
