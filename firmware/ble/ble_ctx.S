/* SPDX-License-Identifier: MPL-2.0 */
/* Context switch for the cooperative scheduler in ble_os.c. A task is suspended by pushing the registers a C call must
 * preserve (rets and r4-r15: every register a compiled function saves, the list its own prologue pushes) on its own stack and storing the stack
 * pointer; it is resumed by loading that pointer and popping the same list. Push and pop are the same instruction pair
 * the compiler emits, so the order in memory does not matter.
 *
 *   void ble_ctx_swap(uint32_t **save, uint32_t *load)           suspend here into *save, resume the context in load
 *   void ble_ctx_enter(uint32_t **save, uint32_t *top, void (*fn)(void))
 *                                                                suspend here into *save, call fn on the stack at top
 *                                                                (fn never returns)
 */
	.section .text.ble_ctx,"ax",@progbits
	.balign 4
	.globl ble_ctx_swap
ble_ctx_swap:
	[--sp] = {rets, r15-r4}
	r2 = sp
	[r0 + 0] = r2
	sp = r1
	{pc, r15-r4} = [sp++]

	.globl ble_ctx_enter
ble_ctx_enter:
	[--sp] = {rets, r15-r4}
	r3 = sp
	[r0 + 0] = r3
	sp = r1
	call r2
	goto .
