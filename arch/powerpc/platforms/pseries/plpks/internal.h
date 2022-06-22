/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 *
 */
#ifndef PKS_FWSEC_INTERNAL
#define PKS_FWSEC_INTERNAL

#ifdef CONFIG_PSERIES_PLPKS_SECVARS
int plpks_secvars_init(void);
#else
int plpks_secvars_init(void)
{
	return 0;
}
#endif
#endif
