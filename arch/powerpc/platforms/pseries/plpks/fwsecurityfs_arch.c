// SPDX-License-Identifier: GPL-2.0-only
/*
 * POWER LPAR Platform KeyStore (PLPKS)
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 *
 */

#include <linux/fwsecurityfs.h>

#include "internal.h"

int arch_fwsecurity_init(void)
{
	return plpks_secvars_init();
}
