/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_STATIC_CALL_H
#define _ASM_POWERPC_STATIC_CALL_H

#if defined(CONFIG_PPC64_ELF_ABI_V2)

#define __PPC_SCT(name, inst)					\
	asm(".pushsection .text, \"ax\"				\n"	\
	    ".align 6						\n"	\
	    ".globl " STATIC_CALL_TRAMP_STR(name) "		\n"	\
	    STATIC_CALL_TRAMP_STR(name) ":			\n"	\
	    "	addis	2, 12, (.TOC.-" STATIC_CALL_TRAMP_STR(name) ")@ha \n"	\
	    "	addi	2, 2, (.TOC.-" STATIC_CALL_TRAMP_STR(name) ")@l   \n"	\
	    ".localentry " STATIC_CALL_TRAMP_STR(name) ", .-" STATIC_CALL_TRAMP_STR(name) "\n" \
	    "	" inst "					\n"	\
	    "	mflr	0					\n"	\
	    "	std	0, 16(1)				\n"	\
	    "	stdu	1, -32(1)				\n"	\
	    "	std	2, 24(1)				\n"	\
	    "	addis	12, 2, 2f@toc@ha			\n"	\
	    "	ld	12, 2f@toc@l(12)			\n"	\
	    "	mtctr	12					\n"	\
	    "	bctrl						\n"	\
	    "	ld	2, 24(1)				\n"	\
	    "	addi	1, 1, 32				\n"	\
	    "	ld	0, 16(1)				\n"	\
	    "	mtlr	0					\n"	\
	    "	blr						\n"	\
	    "1:	li	3, 0					\n"	\
	    "	blr						\n"	\
	    ".balign 8						\n"	\
	    "2:	.8byte 0					\n"	\
	    ".type " STATIC_CALL_TRAMP_STR(name) ", @function	\n"	\
	    ".size " STATIC_CALL_TRAMP_STR(name) ", . - " STATIC_CALL_TRAMP_STR(name) " \n" \
	    ".popsection					\n")

#define PPC_SCT_RET0		64		/* Offset of label 1 */
#define PPC_SCT_DATA		72		/* Offset of label 2 (aligned) */

#elif defined(PPC32)

#define __PPC_SCT(name, inst)					\
	asm(".pushsection .text, \"ax\"				\n"	\
	    ".align 5						\n"	\
	    ".globl " STATIC_CALL_TRAMP_STR(name) "		\n"	\
	    STATIC_CALL_TRAMP_STR(name) ":			\n"	\
	    "	" inst "					\n"	\
	    "	lis	12,2f@ha				\n"	\
	    "	lwz	12,2f@l(12)				\n"	\
	    "	mtctr	12					\n"	\
	    "	bctr						\n"	\
	    "1:	li	3, 0					\n"	\
	    "	blr						\n"	\
	    "2:	.long 0						\n"	\
	    ".type " STATIC_CALL_TRAMP_STR(name) ", @function	\n"	\
	    ".size " STATIC_CALL_TRAMP_STR(name) ", . - " STATIC_CALL_TRAMP_STR(name) " \n" \
	    ".popsection					\n")

#define PPC_SCT_RET0		20		/* Offset of label 1 */
#define PPC_SCT_DATA		28		/* Offset of label 2 */

#else /* !CONFIG_PPC64_ELF_ABI_V2 && !PPC32 */
#error "Unsupported ABI"
#endif /* CONFIG_PPC64_ELF_ABI_V2 */

#define ARCH_DEFINE_STATIC_CALL_TRAMP(name, func)	__PPC_SCT(name, "b " #func)
#define ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)	__PPC_SCT(name, "blr")
#define ARCH_DEFINE_STATIC_CALL_RET0_TRAMP(name)	__PPC_SCT(name, "b 1f")

#endif /* _ASM_POWERPC_STATIC_CALL_H */
