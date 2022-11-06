// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/magic.h>
#include <linux/fwsecurityfs.h>

static struct super_block *fwsecsb;
static struct vfsmount *mount;
static int mount_count;
static bool fwsecurityfs_initialized;

static void fwsecurityfs_free_inode(struct inode *inode)
{
	free_inode_nonrcu(inode);
}

static const struct super_operations fwsecurityfs_super_operations = {
	.statfs = simple_statfs,
	.free_inode = fwsecurityfs_free_inode,
};

static int fwsecurityfs_fill_super(struct super_block *sb,
				   struct fs_context *fc)
{
	static const struct tree_descr files[] = {{""}};
	int rc;

	rc = simple_fill_super(sb, FWSECURITYFS_MAGIC, files);
	if (rc)
		return rc;

	sb->s_op = &fwsecurityfs_super_operations;

	fwsecsb = sb;

	rc = arch_fwsecurityfs_init();

	if (!rc)
		fwsecurityfs_initialized = true;

	return rc;
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

static void fwsecurityfs_kill_sb(struct super_block *sb)
{
	kill_litter_super(sb);

	fwsecurityfs_initialized = false;
}

static struct file_system_type fs_type = {
	.owner =	THIS_MODULE,
	.name =		"fwsecurityfs",
	.init_fs_context = fwsecurityfs_init_fs_context,
	.kill_sb =	fwsecurityfs_kill_sb,
};

static struct dentry *fwsecurityfs_create_dentry(const char *name, umode_t mode,
						 u16 filesize,
						 struct dentry *parent,
						 struct dentry *dentry, void *data,
						 const struct file_operations *fops,
						 const struct inode_operations *iops)
{
	struct inode *inode;
	int rc;
	struct inode *dir;
	struct dentry *ldentry = dentry;

	/* Calling simple_pin_fs() while initial mount in progress results in recursive
	 * call to mount.
	 */
	if (fwsecurityfs_initialized) {
		rc = simple_pin_fs(&fs_type, &mount, &mount_count);
		if (rc)
			return ERR_PTR(rc);
	}

	dir = d_inode(parent);

	/* For userspace created files, lock is already taken. */
	if (!dentry)
		inode_lock(dir);

	if (!dentry) {
		ldentry = lookup_one_len(name, parent, strlen(name));
		if (IS_ERR(ldentry))
			goto out;

		if (d_really_is_positive(ldentry)) {
			rc = -EEXIST;
			goto out1;
		}
	}

	inode = new_inode(dir->i_sb);
	if (!inode) {
		rc = -ENOMEM;
		goto out1;
	}

	inode->i_ino = get_next_ino();
	inode->i_mode = mode;
	inode->i_atime = current_time(inode);
	inode->i_mtime = current_time(inode);
	inode->i_ctime = current_time(inode);
	inode->i_private = data;

	if (S_ISDIR(mode)) {
		inode->i_op = iops ? iops : &simple_dir_inode_operations;
		inode->i_fop = &simple_dir_operations;
		inc_nlink(inode);
		inc_nlink(dir);
	} else {
		inode->i_fop = fops ? fops : &simple_dir_operations;
	}

	if (S_ISREG(mode)) {
		inode_lock(inode);
		i_size_write(inode, filesize);
		inode_unlock(inode);
	}
	d_instantiate(ldentry, inode);

	/* dget() here is required for userspace created files. */
	if (dentry)
		dget(ldentry);

	if (!dentry)
		inode_unlock(dir);

	return ldentry;

out1:
	ldentry = ERR_PTR(rc);

out:
	if (fwsecurityfs_initialized)
		simple_release_fs(&mount, &mount_count);

	if (!dentry)
		inode_unlock(dir);

	return ldentry;
}

struct dentry *fwsecurityfs_create_file(const char *name, umode_t mode,
					u16 filesize, struct dentry *parent,
					struct dentry *dentry, void *data,
					const struct file_operations *fops)
{
	if (!parent)
		return ERR_PTR(-EINVAL);

	return fwsecurityfs_create_dentry(name, mode, filesize, parent,
					  dentry, data, fops, NULL);
}
EXPORT_SYMBOL_GPL(fwsecurityfs_create_file);

struct dentry *fwsecurityfs_create_dir(const char *name, umode_t mode,
				       struct dentry *parent,
				       const struct inode_operations *iops)
{
	if (!parent) {
		if (!fwsecsb)
			return ERR_PTR(-EIO);
		parent = fwsecsb->s_root;
	}

	return fwsecurityfs_create_dentry(name, mode, 0, parent, NULL, NULL,
					  NULL, iops);
}
EXPORT_SYMBOL_GPL(fwsecurityfs_create_dir);

static int fwsecurityfs_remove_dentry(struct dentry *dentry)
{
	struct inode *dir;

	if (!dentry || IS_ERR(dentry))
		return -EINVAL;

	dir = d_inode(dentry->d_parent);
	inode_lock(dir);
	if (simple_positive(dentry)) {
		dget(dentry);
		if (d_is_dir(dentry))
			simple_rmdir(dir, dentry);
		else
			simple_unlink(dir, dentry);
		d_delete(dentry);
		dput(dentry);
	}
	inode_unlock(dir);

	/* Once fwsecurityfs_initialized is set to true, calling this for
	 * removing files created during initial mount might result in
	 * imbalance of simple_pin_fs() and simple_release_fs() calls.
	 */
	if (fwsecurityfs_initialized)
		simple_release_fs(&mount, &mount_count);

	return 0;
}

int fwsecurityfs_remove_dir(struct dentry *dentry)
{
	if (!d_is_dir(dentry))
		return -EPERM;

	return fwsecurityfs_remove_dentry(dentry);
}
EXPORT_SYMBOL_GPL(fwsecurityfs_remove_dir);

int fwsecurityfs_remove_file(struct dentry *dentry)
{
	return fwsecurityfs_remove_dentry(dentry);
};
EXPORT_SYMBOL_GPL(fwsecurityfs_remove_file);

static int __init fwsecurityfs_init(void)
{
	int rc;

	rc = sysfs_create_mount_point(firmware_kobj, "security");
	if (rc)
		return rc;

	rc = register_filesystem(&fs_type);
	if (rc) {
		sysfs_remove_mount_point(firmware_kobj, "security");
		return rc;
	}

	return 0;
}
core_initcall(fwsecurityfs_init);
MODULE_DESCRIPTION("Firmware Security Filesystem");
MODULE_AUTHOR("Nayna Jain");
MODULE_LICENSE("GPL");
