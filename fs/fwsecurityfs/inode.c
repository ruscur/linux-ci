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

int fwsecurityfs_remove_file(struct dentry *dentry)
{
	drop_nlink(d_inode(dentry));
	dput(dentry);
	return 0;
};
EXPORT_SYMBOL_GPL(fwsecurityfs_remove_file);

int fwsecurityfs_create_file(const char *name, umode_t mode,
					u16 filesize, struct dentry *parent,
					struct dentry *dentry,
					const struct file_operations *fops)
{
	struct inode *inode;
	int error;
	struct inode *dir;

	if (!parent)
		return -EINVAL;

	dir = d_inode(parent);
	pr_debug("securityfs: creating file '%s'\n", name);

	inode = new_inode(dir->i_sb);
	if (!inode) {
		error = -ENOMEM;
		goto out1;
	}

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

	if (fops)
		inode->i_fop = fops;
	else
		inode->i_fop = &simple_dir_operations;

	if (!dentry) {
		dentry = fwsecurityfs_alloc_dentry(parent, name);
		if (IS_ERR(dentry)) {
			error = PTR_ERR(dentry);
			goto out;
		}
	}

	inode_lock(inode);
	i_size_write(inode, filesize);
	d_instantiate(dentry, inode);
	dget(dentry);
	d_add(dentry, inode);
	inode_unlock(inode);
	return 0;

out1:
	if (dentry)
		dput(dentry);
out:
	return error;
}
EXPORT_SYMBOL_GPL(fwsecurityfs_create_file);

struct dentry *fwsecurityfs_create_dir(const char *name, umode_t mode,
				       struct dentry *parent,
				       const struct inode_operations *iops)
{
	struct dentry *dentry;
	struct inode *inode;
	int error;
	struct inode *dir;
	struct super_block *fwsecsb;

	if (!parent) {
		fwsecsb = fwsecurityfs_get_superblock();
		if (!fwsecsb)
			return ERR_PTR(-EIO);
		parent = fwsecsb->s_root;
	}

	dir = d_inode(parent);

	inode_lock(dir);
	dentry = lookup_one_len(name, parent, strlen(name));
	if (IS_ERR(dentry))
		goto out;

	inode = new_inode(dir->i_sb);
	if (!inode) {
		error = -ENOMEM;
		goto out1;
	}

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
	if (iops)
		inode->i_op = iops;
	else
		inode->i_op = &simple_dir_inode_operations;
	inode->i_fop = &simple_dir_operations;
	inc_nlink(inode);
	inc_nlink(dir);
	d_instantiate(dentry, inode);
	dget(dentry);
	inode_unlock(dir);
	return dentry;

out1:
	dput(dentry);
	dentry = ERR_PTR(error);
out:
	inode_unlock(dir);
	return dentry;
}
EXPORT_SYMBOL_GPL(fwsecurityfs_create_dir);

int fwsecurityfs_remove_dir(struct dentry *dentry)
{
	struct inode *dir;

	if (!dentry || IS_ERR(dentry))
		return -EINVAL;

	if (!d_is_dir(dentry))
		return -EPERM;

	dir = d_inode(dentry->d_parent);
	inode_lock(dir);
	if (simple_positive(dentry)) {
		simple_rmdir(dir, dentry);
		dput(dentry);
	}
	inode_unlock(dir);

	return 0;
}
EXPORT_SYMBOL_GPL(fwsecurityfs_remove_dir);
