/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2015-2017 Josh Poimboeuf <jpoimboe@redhat.com>
 */

#ifndef _OBJTOOL_ORC_H
#define _OBJTOOL_ORC_H

#include <asm/orc_types.h>
#include <objtool/check.h>

int init_orc_entry(struct orc_entry *orc, struct cfi_state *cfi,
		   struct instruction *insn);
const char *orc_type_name(unsigned int type);
void orc_print_reg(unsigned int reg, int offset);

#endif /* _OBJTOOL_ORC_H */
