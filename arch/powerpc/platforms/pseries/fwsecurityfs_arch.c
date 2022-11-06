// SPDX-License-Identifier: GPL-2.0-only
/*
 * Initialize fwsecurityfs with POWER LPAR Platform KeyStore (PLPKS)
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 *
 */

#include <linux/fwsecurityfs.h>
#include "plpks.h"

static struct dentry *plpks_dir;

static ssize_t plpks_config_file_read(struct file *file, char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	u8 out[4];
	u32 outlen;
	size_t size;
	char *name;
	u32 data;

	name = file_dentry(file)->d_iname;

	if (strcmp(name, "max_object_size") == 0) {
		outlen = sizeof(u16);
		data = plpks_get_maxobjectsize();
	} else if (strcmp(name, "max_object_label_size") == 0) {
		outlen = sizeof(u16);
		data = plpks_get_maxobjectlabelsize();
	} else if (strcmp(name, "total_size") == 0) {
		outlen = sizeof(u32);
		data = plpks_get_totalsize();
	} else if (strcmp(name, "used_space") == 0) {
		outlen = sizeof(u32);
		data = plpks_get_usedspace();
	} else if (strcmp(name, "version") == 0) {
		outlen = sizeof(u8);
		data = plpks_get_version();
	} else {
		return -EINVAL;
	}

	memcpy(out, &data, outlen);

	size = simple_read_from_buffer(userbuf, count, ppos, out, outlen);

	return size;
}

static const struct file_operations plpks_config_file_operations = {
	.open   = simple_open,
	.read   = plpks_config_file_read,
	.llseek = no_llseek,
};

static int create_plpks_dir(void)
{
	struct dentry *config_dir;
	struct dentry *fdentry;
	int rc;

	if (!IS_ENABLED(CONFIG_PSERIES_PLPKS) || !plpks_is_available()) {
		pr_warn("Platform KeyStore is not available on this LPAR\n");
		return 0;
	}

	plpks_dir = fwsecurityfs_create_dir("plpks", S_IFDIR | 0755, NULL,
					    NULL);
	if (IS_ERR(plpks_dir)) {
		pr_err("Unable to create PLPKS dir: %ld\n", PTR_ERR(plpks_dir));
		return PTR_ERR(plpks_dir);
	}

	config_dir = fwsecurityfs_create_dir("config", S_IFDIR | 0755, plpks_dir, NULL);
	if (IS_ERR(config_dir)) {
		pr_err("Unable to create config dir: %ld\n", PTR_ERR(config_dir));
		return PTR_ERR(config_dir);
	}

	fdentry = fwsecurityfs_create_file("max_object_size", S_IFREG | 0444,
					   sizeof(u16), config_dir, NULL, NULL,
					   &plpks_config_file_operations);
	if (IS_ERR(fdentry))
		pr_err("Could not create max object size %ld\n", PTR_ERR(fdentry));

	fdentry = fwsecurityfs_create_file("max_object_label_size", S_IFREG | 0444,
					   sizeof(u16), config_dir, NULL, NULL,
					   &plpks_config_file_operations);
	if (IS_ERR(fdentry))
		pr_err("Could not create max object label size %ld\n", PTR_ERR(fdentry));

	fdentry = fwsecurityfs_create_file("total_size", S_IFREG | 0444,
					   sizeof(u32), config_dir, NULL, NULL,
					   &plpks_config_file_operations);
	if (IS_ERR(fdentry))
		pr_err("Could not create total size %ld\n", PTR_ERR(fdentry));

	fdentry = fwsecurityfs_create_file("used_space", S_IFREG | 0444,
					   sizeof(u32), config_dir, NULL, NULL,
					   &plpks_config_file_operations);
	if (IS_ERR(fdentry))
		pr_err("Could not create used space %ld\n", PTR_ERR(fdentry));

	fdentry = fwsecurityfs_create_file("version", S_IFREG | 0444,
					   sizeof(u8), config_dir, NULL, NULL,
					   &plpks_config_file_operations);
	if (IS_ERR(fdentry))
		pr_err("Could not create version %ld\n", PTR_ERR(fdentry));

	if (IS_ENABLED(CONFIG_PSERIES_PLPKS_SECVARS)) {
		rc = plpks_secvars_init(plpks_dir);
		if (rc)
			pr_err("Secure Variables initialization failed with error %d\n", rc);
		return rc;
	}

	return 0;
}

int arch_fwsecurityfs_init(void)
{
	return create_plpks_dir();
}
