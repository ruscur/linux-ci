// SPDX-License-Identifier: GPL-2.0-only
/*
 * Platform variable operations.
 *
 * Copyright (C) 2022 IBM Corporation
 *
 * These are the accessor functions (read/write) for architecture specific
 * variables. Specific architectures can provide overrides.
 *
 */

#include <linux/kernel.h>
#include <linux/arch_vars.h>

int __weak arch_read_variable(enum arch_variable_type type, char *varname,
			      void *varbuf, u_int *varlen)
{
	return -EOPNOTSUPP;
}

int __weak arch_write_variable(enum arch_variable_type type, char *varname,
			       void *varbuf, u_int varlen)
{
	return -EOPNOTSUPP;
}
