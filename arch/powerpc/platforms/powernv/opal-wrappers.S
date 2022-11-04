/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PowerNV OPAL API wrappers
 *
 * Copyright 2011 IBM Corp.
 */

#include <linux/jump_label.h>
#include <asm/ppc_asm.h>
#include <asm/hvcall.h>
#include <asm/asm-offsets.h>
#include <asm/opal.h>
#include <asm/asm-compat.h>
#include <asm/feature-fixups.h>

	.section ".text"

/*
 * r3-r10		- OPAL call arguments
 * STK_PARAM(R11)	- OPAL opcode
 * STK_PARAM(R12)	- MSR to restore
 */
_GLOBAL_TOC(__opal_call)
	mflr	r0
	std	r0,PPC_LR_STKOFF(r1)
	LOAD_REG_ADDR(r11, opal_return)
	mtlr	r11
	LOAD_REG_ADDR(r11, opal)
	ld	r2,0(r11)
	ld	r11,8(r11)
	mtspr	SPRN_HSRR0,r11

	/* set token to r0 */
	ld	r0,STK_PARAM(R11)(r1)

	/*
	 * We need to keep the MSR value in a register that is preserved by
	 * OPAL, so that we don't need to access the stack before we restore
	 * the MSR, as the stack may be vmalloced and thus require MMU.
	 *
	 * Move the PACA from R13 into the stack red zone, and put MSR in R13.
	 */
	std	r13,-8(r1)
	ld	r13,STK_PARAM(R12)(r1)

	/* Switch off MMU, LE */
	li	r11,MSR_IR|MSR_DR|MSR_LE
	andc	r11,r13,r11

	mtspr	SPRN_HSRR1,r11
	hrfid
opal_return:
	/*
	 * Restore MSR on OPAL return. The MSR is set to big-endian.
	 */
#ifdef __BIG_ENDIAN__
	mtmsrd	r13
#else
	/* Endian can only be switched with rfi, must byte reverse MSR load */
	.long 0x05009f42 /* bcl 20,31,$+4   (LR <- next insn addr)	*/
	.long 0xa602487d /* mflr r10					*/
	.long 0x14004a39 /* addi r10,r10,20 (r10 <- addr after #endif)	*/
	.long 0xa64b5a7d /* mthsrr0 r10	    (new NIP)			*/
	.long 0xa64bbb7d /* mthsrr1 r13	    (new MSR)			*/
	.long 0x2402004c /* hrfid					*/
#endif
	/* Restore PACA */
	ld	r13,-8(r1)
	LOAD_PACA_TOC()
	ld	r0,PPC_LR_STKOFF(r1)
	mtlr	r0
	blr
