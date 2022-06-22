// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 */

#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/lsm_hooks.h>
#include <linux/magic.h>
#include <linux/ctype.h>
#include <linux/fwsecurityfs.h>

#include "internal.h"

static struct super_block *fwsecsb;

struct super_block *fwsecurityfs_get_superblock(void)
{
	return fwsecsb;
}

static int fwsecurityfs_d_hash(const struct dentry *dir, struct qstr *this)
{
	unsigned long hash;
	int i;

	hash = init_name_hash(dir);
	for (i = 0; i < this->len; i++)
		hash = partial_name_hash(tolower(this->name[i]), hash);
	this->hash = end_name_hash(hash);

	return 0;
}

static int fwsecurityfs_d_compare(const struct dentry *dentry,
				  unsigned int len, const char *str,
				  const struct qstr *name)
{
	int i;
	int result = 1;

	if (len != name->len)
		goto out;
	for (i = 0; i < len; i++) {
		if (tolower(str[i]) != tolower(name->name[i]))
			goto out;
	}
	result = 0;
out:
	return result;
}

struct dentry *fwsecurityfs_alloc_dentry(struct dentry *parent, const char *name)
{
	struct dentry *d;
	struct qstr q;
	int err;

	q.name = name;
	q.len = strlen(name);

	err = fwsecurityfs_d_hash(parent, &q);
	if (err)
		return ERR_PTR(err);

	d = d_alloc(parent, &q);
	if (d)
		return d;

	return ERR_PTR(-ENOMEM);
}

static const struct dentry_operations fwsecurityfs_d_ops = {
	.d_compare = fwsecurityfs_d_compare,
	.d_hash = fwsecurityfs_d_hash,
	.d_delete = always_delete_dentry,
};

static const struct super_operations securityfs_super_operations = {
	.statfs = simple_statfs,
	.drop_inode = generic_delete_inode,
};

static int fwsecurityfs_fill_super(struct super_block *sb,
				   struct fs_context *fc)
{
	static const struct tree_descr files[] = {{""}};
	int error;

	error = simple_fill_super(sb, FWSECURITYFS_MAGIC, files);
	if (error)
		return error;

	sb->s_d_op = &fwsecurityfs_d_ops;

	fwsecsb = sb;

	error = arch_fwsecurity_init();
	if (error)
		pr_err("arch specific firmware initialization failed\n");

	return 0;
}

static int fwsecurityfs_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, fwsecurityfs_fill_super);
}

static const struct fs_context_operations fwsecurityfs_context_ops = {
	.get_tree	= fwsecurityfs_get_tree,
};

static int fwsecurityfs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &fwsecurityfs_context_ops;
	return 0;
}

static struct file_system_type fs_type = {
	.owner =	THIS_MODULE,
	.name =		"fwsecurityfs",
	.init_fs_context = fwsecurityfs_init_fs_context,
	.kill_sb =	kill_litter_super,
};

static int __init fwsecurityfs_init(void)
{
	int retval;

	retval = sysfs_create_mount_point(firmware_kobj, "security");
	if (retval)
		return retval;

	retval = register_filesystem(&fs_type);
	if (retval) {
		sysfs_remove_mount_point(firmware_kobj, "security");
		return retval;
	}

	return 0;
}
core_initcall(fwsecurityfs_init);
MODULE_DESCRIPTION("Firmware Security Filesystem");
MODULE_AUTHOR("Nayna Jain");
MODULE_LICENSE("GPL");
