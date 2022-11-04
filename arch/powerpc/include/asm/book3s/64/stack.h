// SPDX-License-Identifier: GPL-2.0-or-later

// Helpers for VMAP_STACK on book3s64
// Copyright (C) 2022 IBM Corporation (Andrew Donnellan)

#ifndef _ASM_POWERPC_BOOK3S_64_STACK_H
#define _ASM_POWERPC_BOOK3S_64_STACK_H

#include <asm/thread_info.h>

#if defined(CONFIG_VMAP_STACK) && defined(CONFIG_PPC_BOOK3S_64)

#ifdef __ASSEMBLY__
// Switch the current stack pointer in r1 between a linear map address and a
// vmalloc address. Used when we need to go in and out of real mode with
// CONFIG_VMAP_STACK enabled.
//
// tmp: scratch register that can be clobbered

#define SWAP_STACK_LINEAR(tmp)			\
	ld	tmp, PACAKSTACK_LINEAR_BASE(r13);	\
	andi.	r1, r1, THREAD_SIZE - 1;		\
	or	r1, r1, tmp;
#define SWAP_STACK_VMALLOC(tmp)			\
	ld	tmp, PACAKSTACK_VMALLOC_BASE(r13);	\
	andi.	r1, r1, THREAD_SIZE - 1;		\
	or	r1, r1, tmp;

#else // __ASSEMBLY__

#include <asm/paca.h>
#include <asm/reg.h>
#include <linux/mm.h>

#define stack_pa(ptr) (is_vmalloc_addr((ptr)) ? (void *)vmalloc_to_phys((void *)(ptr)) : (void *)ptr)

static __always_inline void swap_stack_linear(void)
{
	current_stack_pointer = get_paca()->kstack_linear_base |	\
		(current_stack_pointer & (THREAD_SIZE - 1));
}

static __always_inline void swap_stack_vmalloc(void)
{
	current_stack_pointer = get_paca()->kstack_vmalloc_base |	\
		(current_stack_pointer & (THREAD_SIZE - 1));
}

#endif // __ASSEMBLY__

#else // CONFIG_VMAP_STACK && CONFIG_PPC_BOOK3S_64

#define SWAP_STACK_LINEAR(tmp)
#define SWAP_STACK_VMALLOC(tmp)

static __always_inline void *stack_pa(void *ptr)
{
	return ptr;
}

static __always_inline void swap_stack_linear(void)
{
}

static __always_inline void swap_stack_vmalloc(void)
{
}

#endif // CONFIG_VMAP_STACK && CONFIG_PPC_BOOK3S_64

#endif // _ASM_POWERPC_BOOK3S_64_STACK_H
