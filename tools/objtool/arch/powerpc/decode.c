// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <objtool/check.h>
#include <objtool/elf.h>
#include <objtool/arch.h>
#include <objtool/warn.h>
#include <objtool/builtin.h>
#include <objtool/endianness.h>

bool arch_ftrace_match(char *name)
{
	if (!strcmp(name, "_mcount"))
		return true;

	return false;
}

unsigned long arch_dest_reloc_offset(int addend)
{
	return addend;
}

bool arch_callee_saved_reg(unsigned char reg)
{
	return false;
}

int arch_decode_hint_reg(u8 sp_reg, int *base)
{
	exit(-1);
}

const char *arch_nop_insn(int len)
{
	exit(-1);
}

const char *arch_ret_insn(int len)
{
	exit(-1);
}

int arch_decode_instruction(struct objtool_file *file, const struct section *sec,
			    unsigned long offset, unsigned int maxlen,
			    unsigned int *len, enum insn_type *type,
			    unsigned long *immediate,
			    struct list_head *ops_list)
{
	u32 insn;
	unsigned int opcode;

	*immediate = 0;
	insn = bswap_if_needed(file->elf, *(u32 *)(sec->data->d_buf + offset));
	*len = 4;
	*type = INSN_OTHER;

	opcode = insn >> 26;

	switch (opcode) {
	case 18: /* bl */
		if ((insn & 3) == 1) {
			*type = INSN_CALL;
			*immediate = insn & 0x3fffffc;
			if (*immediate & 0x2000000)
				*immediate -= 0x4000000;
		}
		break;
	}

	return 0;
}

unsigned long arch_jump_destination(struct instruction *insn)
{
	return insn->offset +  insn->immediate;
}

void arch_initial_func_cfi_state(struct cfi_init_state *state)
{
	int i;

	for (i = 0; i < CFI_NUM_REGS; i++) {
		state->regs[i].base = CFI_UNDEFINED;
		state->regs[i].offset = 0;
	}

	/* initial CFA (call frame address) */
	state->cfa.base = CFI_SP;
	state->cfa.offset = 0;

	/* initial LR (return address) */
	state->regs[CFI_RA].base = CFI_CFA;
	state->regs[CFI_RA].offset = 0;
}
