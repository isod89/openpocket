/* SPDX-License-Identifier: MPL-2.0 */
/* The classic baseband's two interrupts in the Bluetooth audio image: OpenPocket's own entry, as bsp/chip/fm1_isr.S's FM1_ISR (save
 * what a C call may clobber, flags first; call the plain C body; restore; csync; rti), around the C bodies of firmware/bt/bt_irq.c,
 * which follow the library's two handlers and count the interrupts.
 *
 *   bt_isr_br    IRQ 40  the BR/EDR baseband (a packet received or sent, the alarm)   bt_br_irq
 *   bt_isr_slot  IRQ 41  the slot timers                                               bt_slot_irq
 *
 * (Test builds 1 to 4 counted here and jumped into the library's own handlers, compiled with the "interrupt" attribute: those end
 * without csync, and the counting stub's add ran before anything saved psr. Harmless below the audio interrupt; with the radio
 * above it, the DSP they interrupted faulted. ble/ble_os.c request_irq installs these.) */
	.macro BT_ISR name, body
	.section .text.\name,"ax",@progbits
	.globl \name
\name:
	[--sp] = {psr, rets, reti}
	[--sp] = {r3-r0}
	call \body
	{r3-r0} = [sp++]
	{psr, rets, reti} = [sp++]
	csync
	rti
	.endm

	BT_ISR bt_isr_br, bt_br_irq
	BT_ISR bt_isr_slot, bt_slot_irq
