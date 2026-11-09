/* SPDX-License-Identifier: MPL-2.0 */
/* The classic baseband's two interrupts in the Bluetooth audio image, counted, exactly as ble/ble_isr.S counts the LE ones: count,
 * restore the two registers used, jump to the library's own interrupt entry (it saves the registers and ends in rti).
 *
 *   bt_isr_br    IRQ 40  the BR/EDR baseband (a packet received or sent, the alarm)   counts bt_cnt_irq_br
 *   bt_isr_slot  IRQ 41  the slot timers                                               counts bt_cnt_irq_slot
 * (ble/ble_os.c request_irq sets the targets up) */
	.macro BT_ISR name, count, target
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

	BT_ISR bt_isr_br, bt_cnt_irq_br, bt_irq_target_br
	BT_ISR bt_isr_slot, bt_cnt_irq_slot, bt_irq_target_slot
