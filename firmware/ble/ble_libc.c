/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* The C library routines and diagnostics hooks the controller library calls and px_audio.h does not already supply
 * (log_print, printf, config_asser, cpu_assert_debug, P33_SYSTEM_RESET, os_current_task_rom). Logging is switched off in
 * the library by its log tags (ble_stubs.c), so the print functions here do nothing. */
#include <stdint.h>
#include <stddef.h>
#include "ble_os.h"

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (uint8_t)*a - (uint8_t)*b; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    uint8_t *dp = d;
    const uint8_t *sp = s;
    if (dp < sp)
        while (n--) *dp++ = *sp++;
    else
        for (dp += n, sp += n; n--;) *--dp = *--sp;
    return d;
}

void printf_buf(const uint8_t *b, uint32_t n) { (void)b; (void)n; }
void put_buf(const uint8_t *b, uint32_t n) { (void)b; (void)n; }
int puts(const char *s) { (void)s; return 0; }
int putchar(int c) { return c; }
