// SPDX-License-Identifier: GPL-2.0-only
/*
 * POWER Platform arch specific code for SED
 * Copyright (C) 2022 IBM Corporation
 *
 * Define operations for generic kernel subsystems to read/write keys
 * from POWER LPAR Platform KeyStore(PLPKS).
 *
 * List of subsystems/usecase using PLPKS:
 * - Self Encrypting Drives(SED)
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ioctl.h>
#include <uapi/linux/sed-opal.h>
#include <linux/sed-opal.h>
#include <linux/arch_vars.h>
#include "plpks.h"

/*
 * variable structure that contains all SED data
 */
struct plpks_sed_object_data {
	u_char version;
	u_char pad1[7];
	u_long authority;
	u_long range;
	u_int  key_len;
	u_char key[32];
};

/*
 * ext_type values
 *     00        no extension exists
 *     01-1F     common
 *     20-3F     AIX
 *     40-5F     Linux
 *     60-7F     IBMi
 */

/*
 * This extension is optional for version 1 sed_object_data
 */
struct sed_object_extension {
	u8 ext_type;
	u8 rsvd[3];
	u8 ext_data[64];
};

#define PKS_SED_OBJECT_DATA_V1          1
#define PKS_SED_MANGLED_LABEL           "/default/pri"
#define PLPKS_SED_COMPONENT             "sed-opal"

#define PLPKS_ARCHVAR_POLICY            WORLDREADABLE
#define PLPKS_ARCHVAR_OS_COMMON         4

/*
 * Read the variable data from PKS given the label
 */
int arch_read_variable(enum arch_variable_type type, char *varname,
		       void *varbuf, u_int *varlen)
{
	struct plpks_var var;
	struct plpks_sed_object_data *data;
	u_int offset = 0;
	char *buf = (char *)varbuf;
	int ret;

	var.name = varname;
	var.namelen = strlen(varname);
	var.policy = PLPKS_ARCHVAR_POLICY;
	var.os = PLPKS_ARCHVAR_OS_COMMON;
	var.data = NULL;
	var.datalen = 0;

	switch (type) {
	case ARCH_VAR_OPAL_KEY:
		var.component = PLPKS_SED_COMPONENT;
#ifdef OPAL_AUTH_KEY
		if (strcmp(OPAL_AUTH_KEY, varname) == 0) {
			var.name = PKS_SED_MANGLED_LABEL;
			var.namelen = strlen(varname);
		}
#endif
		offset = offsetof(struct plpks_sed_object_data, key);
		break;
	case ARCH_VAR_OTHER:
		var.component = "";
		break;
	}

	ret = plpks_read_os_var(&var);
	if (ret != 0)
		return ret;

	if (offset > var.datalen)
		offset = 0;

	switch (type) {
	case ARCH_VAR_OPAL_KEY:
		data = (struct plpks_sed_object_data *)var.data;
		*varlen = data->key_len;
		break;
	case ARCH_VAR_OTHER:
		*varlen = var.datalen;
		break;
	}

	if (var.data) {
		memcpy(varbuf, var.data + offset, var.datalen - offset);
		buf[*varlen] = '\0';
		kfree(var.data);
	}

	return 0;
}

/*
 * Write the variable data to PKS given the label
 */
int arch_write_variable(enum arch_variable_type type, char *varname,
			void *varbuf, u_int varlen)
{
	struct plpks_var var;
	struct plpks_sed_object_data data;
	struct plpks_var_name vname;

	var.name = varname;
	var.namelen = strlen(varname);
	var.policy = PLPKS_ARCHVAR_POLICY;
	var.os = PLPKS_ARCHVAR_OS_COMMON;
	var.datalen = varlen;
	var.data = varbuf;

	switch (type) {
	case ARCH_VAR_OPAL_KEY:
		var.component = PLPKS_SED_COMPONENT;
#ifdef OPAL_AUTH_KEY
		if (strcmp(OPAL_AUTH_KEY, varname) == 0) {
			var.name = PKS_SED_MANGLED_LABEL;
			var.namelen = strlen(varname);
		}
#endif
		var.datalen = sizeof(struct plpks_sed_object_data);
		var.data = (u8 *)&data;

		/* initialize SED object */
		data.version = PKS_SED_OBJECT_DATA_V1;
		data.authority = 0;
		data.range = 0;
		data.key_len = varlen;
		memcpy(data.key, varbuf, varlen);
		break;
	case ARCH_VAR_OTHER:
		var.component = "";
		break;
	}

	/* variable update requires delete first */
	vname.namelen = var.namelen;
	vname.name = var.name;
	(void)plpks_remove_var(var.component, var.os, vname);

	return plpks_write_var(var);
}
