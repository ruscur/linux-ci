// SPDX-License-Identifier: GPL-2.0+
/*
 * DEXCR infrastructure
 *
 * Copyright 2023, Benjamin Gray, IBM Corporation.
 */
#include <linux/compiler_types.h>

#include <asm/cpu_has_feature.h>
#include <asm/cputable.h>
#include <asm/disassemble.h>
#include <asm/errno.h>
#include <asm/inst.h>
#include <asm/ppc-opcode.h>
#include <asm/ptrace.h>
#include <asm/reg.h>

int check_hashchk_trap(struct pt_regs const *regs)
{
	ppc_inst_t insn;

	if (!cpu_has_feature(CPU_FTR_DEXCR_NPHIE))
		return -EINVAL;

	if (!user_mode(regs))
		return -EINVAL;

	if (get_user_instr(insn, (void __user *)regs->nip))
		return -EFAULT;

	if (ppc_inst_primary_opcode(insn) != 31 ||
	    get_xop(ppc_inst_val(insn)) != OP_31_XOP_HASHCHK)
		return -EINVAL;

	return 0;
}
