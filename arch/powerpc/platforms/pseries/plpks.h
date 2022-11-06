/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 *
 * Platform keystore for pseries LPAR(PLPKS).
 */

#ifndef _PSERIES_PLPKS_H
#define _PSERIES_PLPKS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/dcache.h>

#define OSSECBOOTAUDIT 0x40000000
#define OSSECBOOTENFORCE 0x20000000
#define WORLDREADABLE 0x08000000
#define SIGNEDUPDATE 0x01000000

#define PLPKS_VAR_LINUX	0x01
#define PLPKS_VAR_COMMON	0x04

struct plpks_var {
	char *component;
	u8 *name;
	u8 *data;
	u32 policy;
	u16 namelen;
	u16 datalen;
	u8 os;
};

struct plpks_var_name {
	u8  *name;
	u16 namelen;
};

struct plpks_var_name_list {
	u32 varcount;
	struct plpks_var_name varlist[];
};

/**
 * Updates the authenticated variable. It expects NULL as the component.
 */
int plpks_signed_update_var(struct plpks_var var, u64 flags);

/**
 * Writes the specified var and its data to PKS.
 * Any caller of PKS driver should present a valid component type for
 * their variable.
 */
int plpks_write_var(struct plpks_var var);

/**
 * Removes the specified var and its data from PKS.
 */
int plpks_remove_var(char *component, u8 varos,
		     struct plpks_var_name vname);

/**
 * Returns the data for the specified os variable.
 */
int plpks_read_os_var(struct plpks_var *var);

/**
 * Returns the data for the specified firmware variable.
 */
int plpks_read_fw_var(struct plpks_var *var);

/**
 * Returns the data for the specified bootloader variable.
 */
int plpks_read_bootloader_var(struct plpks_var *var);

/**
 * Returns if PKS is available on this LPAR.
 */
bool plpks_is_available(void);

/**
 * Returns version of the Platform KeyStore.
 */
u8 plpks_get_version(void);

/**
 * Returns maximum object size supported by Platform KeyStore.
 */
u16 plpks_get_maxobjectsize(void);

/**
 * Returns maximum object label size supported by Platform KeyStore.
 */
u16 plpks_get_maxobjectlabelsize(void);

/**
 * Returns total size of the configured Platform KeyStore.
 */
u32 plpks_get_totalsize(void);

/**
 * Returns used space from the total size of the Platform KeyStore.
 */
u32 plpks_get_usedspace(void);

int plpks_secvars_init(struct dentry *parent);

#endif
