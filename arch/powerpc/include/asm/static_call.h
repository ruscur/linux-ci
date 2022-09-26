/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_POWERPC_STATIC_CALL_H
#define _ASM_POWERPC_STATIC_CALL_H

#ifdef CONFIG_PPC64_ELF_ABI_V2

#ifdef MODULE

#define __PPC_SCT(name, inst)					\
	asm(".pushsection .text, \"ax\"				\n"	\
	    ".align 6						\n"	\
	    ".globl " STATIC_CALL_TRAMP_STR(name) "		\n"	\
	    ".localentry " STATIC_CALL_TRAMP_STR(name) ", 1	\n"	\
	    STATIC_CALL_TRAMP_STR(name) ":			\n"	\
	    "	mflr	11					\n"	\
	    "	bcl	20, 31, $+4				\n"	\
	    "0:	mflr	12					\n"	\
	    "	mtlr	11					\n"	\
	    "	addi	12, 12, (" STATIC_CALL_TRAMP_STR(name) " - 0b)	\n"	\
	    "	addis 2, 12, (.TOC.-" STATIC_CALL_TRAMP_STR(name) ")@ha	\n"	\
	    "	addi 2, 2, (.TOC.-" STATIC_CALL_TRAMP_STR(name) ")@l	\n"	\
	    "	" inst "					\n"	\
	    "	ld	12, (2f - " STATIC_CALL_TRAMP_STR(name) ")(12)	\n"	\
	    "	mtctr	12					\n"	\
	    "	bctr						\n"	\
	    "1:	li	3, 0					\n"	\
	    "	blr						\n"	\
	    ".balign 8						\n"	\
	    "2:	.8byte 0					\n"	\
	    ".type " STATIC_CALL_TRAMP_STR(name) ", @function	\n"	\
	    ".size " STATIC_CALL_TRAMP_STR(name) ", . - " STATIC_CALL_TRAMP_STR(name) " \n" \
	    ".popsection					\n")

#else /* KERNEL */

#define __PPC_SCT(name, inst)					\
	asm(".pushsection .text, \"ax\"				\n"	\
	    ".align 5						\n"	\
	    ".globl " STATIC_CALL_TRAMP_STR(name) "		\n"	\
	    ".localentry " STATIC_CALL_TRAMP_STR(name) ", 1	\n"	\
	    STATIC_CALL_TRAMP_STR(name) ":			\n"	\
	    "	ld	2, 16(13)				\n"	\
	    "	" inst "					\n"	\
	    "	addis	12, 2, 2f@toc@ha			\n"	\
	    "	ld	12, 2f@toc@l(12)			\n"	\
	    "	mtctr	12					\n"	\
	    "	bctr						\n"	\
	    "1:	li	3, 0					\n"	\
	    "	blr						\n"	\
	    ".balign 8						\n"	\
	    "2:	.8byte 0					\n"	\
	    ".type " STATIC_CALL_TRAMP_STR(name) ", @function	\n"	\
	    ".size " STATIC_CALL_TRAMP_STR(name) ", . - " STATIC_CALL_TRAMP_STR(name) " \n" \
	    ".popsection					\n")

#endif /* MODULE */

#define PPC_SCT_INST_MODULE		28		/* Offset of instruction to update */
#define PPC_SCT_RET0_MODULE		44		/* Offset of label 1 */
#define PPC_SCT_DATA_MODULE		56		/* Offset of label 2 (aligned) */

#define PPC_SCT_INST_KERNEL		4		/* Offset of instruction to update */
#define PPC_SCT_RET0_KERNEL		24		/* Offset of label 1 */
#define PPC_SCT_DATA_KERNEL		32		/* Offset of label 2 (aligned) */

#elif defined(CONFIG_PPC32)

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

#define PPC_SCT_INST_MODULE		0		/* Offset of instruction to update */
#define PPC_SCT_RET0_MODULE		20		/* Offset of label 1 */
#define PPC_SCT_DATA_MODULE		28		/* Offset of label 2 */

#define PPC_SCT_INST_KERNEL		PPC_SCT_INST_MODULE
#define PPC_SCT_RET0_KERNEL		PPC_SCT_RET0_MODULE
#define PPC_SCT_DATA_KERNEL		PPC_SCT_DATA_MODULE

#else /* !CONFIG_PPC64_ELF_ABI_V2 && !CONFIG_PPC32 */
#error "Unsupported ABI"
#endif /* CONFIG_PPC64_ELF_ABI_V2 */

#define ARCH_DEFINE_STATIC_CALL_TRAMP(name, func)	__PPC_SCT(name, "b " #func)
#define ARCH_DEFINE_STATIC_CALL_NULL_TRAMP(name)	__PPC_SCT(name, "blr")
#define ARCH_DEFINE_STATIC_CALL_RET0_TRAMP(name)	__PPC_SCT(name, "b 1f")

#endif /* _ASM_POWERPC_STATIC_CALL_H */
