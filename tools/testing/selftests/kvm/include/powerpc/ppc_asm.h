/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * powerpc asm specific defines
 */
#ifndef SELFTEST_KVM_PPC_ASM_H
#define SELFTEST_KVM_PPC_ASM_H

#define STACK_FRAME_MIN_SIZE	112 /* Could be 32 on ELFv2 */
#define STACK_REDZONE_SIZE	512

#define INT_FRAME_SIZE		(STACK_FRAME_MIN_SIZE + STACK_REDZONE_SIZE)

#define SPR_SRR0	0x1a
#define SPR_SRR1	0x1b
#define SPR_CFAR	0x1c

#endif
