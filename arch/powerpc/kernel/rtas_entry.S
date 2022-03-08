/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <asm/asm-offsets.h>
#include <asm/bug.h>
#include <asm/page.h>
#include <asm/ppc_asm.h>

/*
 * RTAS is called with MSR IR, DR, EE disabled, and LR in the return address.
 *
 * Note: r3 is an input parameter to rtas, so don't trash it...
 */

#ifdef CONFIG_PPC32
_GLOBAL(enter_rtas)
	stwu	r1,-INT_FRAME_SIZE(r1)
	mflr	r0
	stw	r0,INT_FRAME_SIZE+4(r1)
	LOAD_REG_ADDR(r4, rtas)
	lis	r6,1f@ha	/* physical return address for rtas */
	addi	r6,r6,1f@l
	tophys(r6,r6)
	lwz	r8,RTASENTRY(r4)
	lwz	r4,RTASBASE(r4)
	mfmsr	r9
	stw	r9,8(r1)
	li	r9,MSR_KERNEL & ~(MSR_IR|MSR_DR)
	mtlr	r6
	stw	r1, THREAD + RTAS_SP(r2)
	mtspr	SPRN_SRR0,r8
	mtspr	SPRN_SRR1,r9
	rfi
1:
	lis	r8, 1f@h
	ori	r8, r8, 1f@l
	LOAD_REG_IMMEDIATE(r9,MSR_KERNEL)
	mtspr	SPRN_SRR0,r8
	mtspr	SPRN_SRR1,r9
	rfi			/* Reactivate MMU translation */
1:
	lwz	r8,INT_FRAME_SIZE+4(r1)	/* get return address */
	lwz	r9,8(r1)	/* original msr value */
	addi	r1,r1,INT_FRAME_SIZE
	li	r0,0
	stw	r0, THREAD + RTAS_SP(r2)
	mtlr	r8
	mtmsr	r9
	blr			/* return to caller */
_ASM_NOKPROBE_SYMBOL(enter_rtas)

#else /* CONFIG_PPC32 */
#include <asm/exception-64s.h>

/*
 * 32-bit rtas on 64-bit machines has the additional problem that RTAS may
 * not preserve the upper parts of registers it uses.
 */
_GLOBAL(enter_rtas)
	mflr	r0
	std	r0,16(r1)
	stdu	r1,-SWITCH_FRAME_SIZE(r1) /* Save SP and create stack space. */

	/* Because RTAS is running in 32b mode, it clobbers the high order half
	 * of all registers that it saves.  We therefore save those registers
	 * RTAS might touch to the stack.  (r0, r3-r12 are caller saved)
	 */
	SAVE_GPR(2, r1)			/* Save the TOC */
	SAVE_NVGPRS(r1)			/* Save the non-volatiles */

	mfcr	r4
	std	r4,_CCR(r1)
	mfctr	r5
	std	r5,_CTR(r1)
	mfspr	r6,SPRN_XER
	std	r6,_XER(r1)
	mfdar	r7
	std	r7,_DAR(r1)
	mfdsisr	r8
	std	r8,_DSISR(r1)

	/* Temporary workaround to clear CR until RTAS can be modified to
	 * ignore all bits.
	 */
	li	r0,0
	mtcr	r0

	/*
	 * The stack pointer is clobbered, so it is saved in the PACA which
	 * allows us to restore our original state after RTAS returns.
	 */
	std	r1,PACAR1(r13)

	/* Setup our real return addr */
	LOAD_REG_ADDR(r4,rtas_return_loc)
	clrldi	r4,r4,2			/* convert to realmode address */
	mtlr	r4

	LOAD_REG_IMMEDIATE(r6, MSR_ME|MSR_RI)

	LOAD_REG_ADDR(r4, rtas)
	ld	r5,RTASENTRY(r4)	/* get the rtas->entry value */
	ld	r4,RTASBASE(r4)		/* get the rtas->base value */

	li	r0,0
	mtmsrd	r0,1			/* disable RI before using SRR0/1 */

	mtspr	SPRN_SRR0,r5
	mtspr	SPRN_SRR1,r6
	RFI_TO_KERNEL
	b	.	/* prevent speculative execution */
_ASM_NOKPROBE_SYMBOL(enter_rtas)

rtas_return_loc:
	FIXUP_ENDIAN

	/* Set SF before anything. */
	LOAD_REG_IMMEDIATE(r6, MSR_KERNEL & ~(MSR_IR|MSR_DR))
	mtmsrd	r6

	GET_PACA(r13)

	ld	r1,PACAR1(r13)		/* Restore our SP */

	REST_GPR(2, r1)			/* Restore the TOC */
	REST_NVGPRS(r1)			/* Restore the non-volatiles */

	ld	r4,_CCR(r1)
	mtcr	r4
	ld	r5,_CTR(r1)
	mtctr	r5
	ld	r6,_XER(r1)
	mtspr	SPRN_XER,r6
	ld	r7,_DAR(r1)
	mtdar	r7
	ld	r8,_DSISR(r1)
	mtdsisr	r8

	addi	r1,r1,SWITCH_FRAME_SIZE	/* Unstack our frame */
	ld	r0,16(r1)		/* get return address */

	mtlr	r0
	blr				/* return to caller */
_ASM_NOKPROBE_SYMBOL(rtas_return_loc)
#endif /* CONFIG_PPC32 */
