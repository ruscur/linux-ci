/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Platform variable opearations.
 *
 * Copyright (C) 2022 IBM Corporation
 *
 * These are the accessor functions (read/write) for architecture specific
 * variables. Specific architectures can provide overrides.
 *
 */

#include <linux/kernel.h>

enum arch_variable_type {
	ARCH_VAR_OPAL_KEY      = 0,     /* SED Opal Authentication Key */
	ARCH_VAR_OTHER         = 1,     /* Other type of variable */
	ARCH_VAR_MAX           = 1,     /* Maximum type value */
};

int arch_read_variable(enum arch_variable_type type, char *varname,
		       void *varbuf, u_int *varlen);
int arch_write_variable(enum arch_variable_type type, char *varname,
			void *varbuf, u_int varlen);
