/* SPDX-License-Identifier: GPL-2.0 */
/*
 * IRQ flags handling
 */
#ifndef _ASM_IRQFLAGS_H
#define _ASM_IRQFLAGS_H

#ifndef __ASSEMBLY__
/*
 * Get definitions for arch_local_save_flags(x), etc.
 */
#include <asm/hw_irq.h>

#else
#ifdef CONFIG_TRACE_IRQFLAGS
/*
 * This is used by assembly code to soft-disable interrupts first and
 * reconcile irq state.
 *
 * NB: This may call C code, so the caller must be prepared for volatiles to
 * be clobbered.
 */
#define RECONCILE_IRQ_STATE(__rA, __rB)		\
	lbz	__rA,PACAIRQSOFTMASK(r13);	\
	lbz	__rB,PACAIRQHAPPENED(r13);	\
	andi.	__rA,__rA,IRQS_DISABLED;	\
	li	__rA,IRQS_DISABLED;		\
	ori	__rB,__rB,PACA_IRQ_HARD_DIS;	\
	stb	__rB,PACAIRQHAPPENED(r13);	\
	bne	44f;				\
	stb	__rA,PACAIRQSOFTMASK(r13);	\
	TRACE_DISABLE_INTS;			\
44:

#else
#define RECONCILE_IRQ_STATE(__rA, __rB)		\
	lbz	__rA,PACAIRQHAPPENED(r13);	\
	li	__rB,IRQS_DISABLED;		\
	ori	__rA,__rA,PACA_IRQ_HARD_DIS;	\
	stb	__rB,PACAIRQSOFTMASK(r13);	\
	stb	__rA,PACAIRQHAPPENED(r13)
#endif
#endif

#endif
