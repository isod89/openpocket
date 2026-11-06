/* SPDX-License-Identifier: MPL-2.0 */
/* The two baseband interrupts of the Bluetooth test image, counted. The library's own interrupt functions are complete
 * interrupt entries (they save the registers and end in rti), so this cannot be a C function that calls them: it counts,
 * restores the two registers it used and jumps to the library's function, which then runs as if it had been entered
 * directly. The jump target is on the stack below the saved registers so that no register is left over. It uses add, load
 * and store only (the compiler's own compares are fused with their branches), so the flags the library saves are those
 * of the interrupted code.
 *
 *   ble_isr_ev   IRQ 45  counts ble_cnt_irq_ev, jumps to *ble_irq_target_ev
 *   ble_isr_rx   IRQ 29  counts ble_cnt_irq_rx, jumps to *ble_irq_target_rx   (ble_os.c request_irq sets them up) */
	.macro BLE_ISR name, count, target
	.section .text.\name,"ax",@progbits
	.globl \name
\name:
	sp += -4
	[--sp] = {r1-r0}
	r0 = \count
	r1 = [r0+0]
	r1 += 1
	[r0+0] = r1
	r0 = \target
	r0 = [r0+0]
	[sp+8] = r0
	{r1-r0} = [sp++]
	{pc} = [sp++]
	.endm

	BLE_ISR ble_isr_ev, ble_cnt_irq_ev, ble_irq_target_ev
	BLE_ISR ble_isr_rx, ble_cnt_irq_rx, ble_irq_target_rx
