/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* C library routines the controller library needs (ble_libc.c); memset and memcpy are bsp/chip/libc.c's */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
int memcmp(const void *a, const void *b, unsigned n);
void *memmove(void *d, const void *s, size_t n);
void *malloc(size_t n);
void free(void *p);

void ble_poll(void);                 /* the main loop calls this often: runs the controller's timers and tasks */
uint32_t ble_heap_free(void);
uint32_t ble_heap_min_free(void);

/* ble_test.c */
void ble_vm_load(void);              /* ble_rf.c: the stored radio records (VM 106 to 108), read only */
extern uint32_t ble_vm_have, ble_vm_area;
uint32_t ble_vm_trim(void);          /* the first two bytes of VM 106 (the crystal trim), for the status line */
void ble_test_start(void);           /* radio on, controller up, one advertisement */
void ble_test_poll(void);
void ble_test_adv_line(char *out);   /* the long advertising line: counters and baseband registers (ble_status.h) */
void ble_test_state(char *out);      /* the short status text for the screen (ble_status.h), 24 bytes */
