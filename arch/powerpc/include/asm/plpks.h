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

#define OSSECBOOTAUDIT 0x40000000
#define OSSECBOOTENFORCE 0x20000000
#define WORLDREADABLE 0x08000000
#define SIGNEDUPDATE 0x01000000

#define PLPKS_VAR_LINUX	0x01
#define PLPKS_VAR_COMMON	0x04

struct plpks_var {
	char *component;
	u8 os;
	u8 *name;
	u16 namelen;
	u32 policy;
	u16 datalen;
	u8 *data;
};

struct plpks_var_name {
	u16 namelen;
	u8  *name;
};

struct plpks_var_name_list {
	u32 varcount;
	struct plpks_var_name varlist[];
};

struct plpks_config {
	u8 version;
	u8 flags;
	u32 rsvd0;
	u16 maxpwsize;
	u16 maxobjlabelsize;
	u16 maxobjsize;
	u32 totalsize;
	u32 usedspace;
	u32 supportedpolicies;
	u64 rsvd1;
} __packed;

/**
 * Successful return from this API  implies PKS is available.
 * This is used to initialize kernel driver and user interfaces.
 */
struct plpks_config *plpks_get_config(void);

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

#endif
