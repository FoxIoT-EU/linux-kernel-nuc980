/*
 * Copyright 2020 Veiko Rütter <rebane@alkohol.ee>
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#define DEBUG_LL_UART_REG_THR    0x00
#define DEBUG_LL_UART_REG_FSR    0x18
#define DEBUG_LL_TX_EMPTY        0x00400000
#define DEBUG_LL_TE_FLAG         0x10000000
#define DEBUG_LL_TX_IDLE         (DEBUG_LL_TX_EMPTY | DEBUG_LL_TE_FLAG)
#define DEBUG_LL_NUC980_VA_UART0 (0xF0000000 + 0x00070000)
#define DEBUG_LL_NUC980_PA_UART0 (0xB0070000)

#if defined(CONFIG_DEBUG_NUC980_UART0)
	.macro	addruart, rp, rv, tmp
	ldr	\rp, =DEBUG_LL_NUC980_PA_UART0
	ldr	\rv, =DEBUG_LL_NUC980_VA_UART0
	.endm

	.macro	senduart,rd,rx
	str	\rd, [\rx, #(DEBUG_LL_UART_REG_THR)]
	.endm

	.macro	waituartcts,rd,rx
	.endm

	.macro	waituarttxrdy,rd,rx
	.endm

	.macro	busyuart,rd,rx
1688:	ldr	\rd, [\rx, #(DEBUG_LL_UART_REG_FSR)]
	and	\rd, \rd, #DEBUG_LL_TX_IDLE
	tst	\rd, #DEBUG_LL_TX_IDLE
	beq	1688b
	.endm
#endif
