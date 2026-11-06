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

/* the circular buffer of the host's command queue (SDK system/generic/circular_buf.h; the layout is the library's) */
typedef struct { uint8_t *begin, *end, *read_ptr, *write_ptr, *tmp_ptr; uint32_t tmp_len, data_len, total_len, lock; } cbuffer_t;
void cbuf_clear(cbuffer_t *c) { c->read_ptr = c->write_ptr = c->begin; c->data_len = 0; }
void cbuf_init(cbuffer_t *c, void *buf, uint32_t size)
{
    c->begin = buf; c->end = c->begin + size; c->total_len = size;
    cbuf_clear(c);
}
uint32_t cbuf_get_data_size(cbuffer_t *c) { return c->data_len; }
uint32_t cbuf_write(cbuffer_t *c, const void *buf, uint32_t len)
{
    const uint8_t *s = buf;
    uint32_t i;
    if (len > c->total_len - c->data_len) return 0;
    for (i = 0; i < len; i++) { *c->write_ptr++ = s[i]; if (c->write_ptr == c->end) c->write_ptr = c->begin; }
    c->data_len += len;
    return len;
}
uint32_t cbuf_read(cbuffer_t *c, void *buf, uint32_t len)
{
    uint8_t *d = buf;
    uint32_t i;
    if (len > c->data_len) return 0;
    for (i = 0; i < len; i++) { d[i] = *c->read_ptr++; if (c->read_ptr == c->end) c->read_ptr = c->begin; }
    c->data_len -= len;
    return len;
}
