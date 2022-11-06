/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 */

#ifndef _FWSECURITYFS_H_
#define _FWSECURITYFS_H_

#include <linux/ctype.h>
#include <linux/fs.h>

struct dentry *fwsecurityfs_create_file(const char *name, umode_t mode,
					u16 filesize, struct dentry *parent,
					struct dentry *dentry, void *data,
					const struct file_operations *fops);

int fwsecurityfs_remove_file(struct dentry *dentry);
struct dentry *fwsecurityfs_create_dir(const char *name, umode_t mode,
				       struct dentry *parent,
				       const struct inode_operations *iops);
int fwsecurityfs_remove_dir(struct dentry *dentry);

#ifdef CONFIG_PSERIES_FWSECURITYFS_ARCH
int arch_fwsecurityfs_init(void);
#else
static int arch_fwsecurityfs_init(void)
{
	return 0;
}
#endif

#endif /* _FWSECURITYFS_H_ */
