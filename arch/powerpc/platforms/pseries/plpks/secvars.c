// SPDX-License-Identifier: GPL-2.0-only
/*
 * POWER LPAR Platform KeyStore (PLPKS)
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 *
 */

#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/namei.h>
#include <linux/ctype.h>
#include <linux/fwsecurityfs.h>
#include <asm/plpks.h>

#include "internal.h"

static struct dentry *secvar_dir;

static const char * const names[] = {
	"PK",
	"KEK",
	"db",
	"dbx",
	"grubdb",
	"sbat",
	"moduledb",
	"trustedcadb",
	NULL
};

static int validate_name(char *name)
{
	int i = 0;

	while (names[i] != NULL) {
		if ((strlen(names[i]) == strlen(name))
		&& (strncmp(name, names[i], strlen(names[i])) == 0))
			return 0;
		i++;
	}
	pr_err("Invalid name, allowed ones are (dbx,grubdb,sbat,moduledb,trustedcadb)\n");

	return -EINVAL;
}

static u32 get_policy(char *name)
{
	if ((strncmp(name, "PK", 2) == 0)
	|| (strncmp(name, "KEK", 3) == 0)
	|| (strncmp(name, "db", 2) == 0)
	|| (strncmp(name, "dbx", 3) == 0)
	|| (strncmp(name, "grubdb", 6) == 0)
	|| (strncmp(name, "sbat", 4) == 0))
		return (WORLDREADABLE & SIGNEDUPDATE);
	else
		return SIGNEDUPDATE;
}

static ssize_t plpks_secvar_file_write(struct file *file,
				     const char __user *userbuf,
				     size_t count, loff_t *ppos)
{
	struct plpks_var var;
	void *data;
	struct inode *inode = file->f_mapping->host;
	u16 datasize = count;
	ssize_t bytes;

	data = memdup_user(userbuf, datasize);
	if (IS_ERR(data))
		return PTR_ERR(data);

	var.component = NULL;
	var.name = file->f_path.dentry->d_iname;
	var.namelen = strlen(var.name);
	var.policy = get_policy(var.name);
	var.datalen = datasize;
	var.data = data;
	bytes = plpks_signed_update_var(var);

	if (bytes) {
		pr_err("Update of the variable failed with error %ld\n", bytes);
		goto out;
	}

	inode_lock(inode);
	i_size_write(inode, datasize);
	inode->i_mtime = current_time(inode);
	inode_unlock(inode);

	bytes = count;
out:
	kfree(data);

	return bytes;

}

static ssize_t plpks_secvar_file_read(struct file *file, char __user *userbuf,
				    size_t count, loff_t *ppos)
{
	struct plpks_var var;
	char *out;
	u32 outlen;
	int rc;
	size_t size;

	var.name = file->f_path.dentry->d_iname;
	var.namelen = strlen(var.name);
	var.component = NULL;
	rc = plpks_read_os_var(&var);
	if (rc) {
		pr_err("Error reading object %d\n", rc);
		return rc;
	}

	outlen = sizeof(var.policy) + var.datalen;
	out = kzalloc(outlen, GFP_KERNEL);
	memcpy(out, &var.policy, sizeof(var.policy));

	memcpy(out + sizeof(var.policy), var.data, var.datalen);

	size = simple_read_from_buffer(userbuf, count, ppos,
				       out, outlen);
	kfree(out);
	return size;
}


static const struct file_operations plpks_secvar_file_operations = {
	.open   = simple_open,
	.read   = plpks_secvar_file_read,
	.write  = plpks_secvar_file_write,
	.llseek = no_llseek,
};

static int plpks_secvar_create(struct user_namespace *mnt_userns, struct inode *dir,
			     struct dentry *dentry, umode_t mode, bool excl)
{
	int namelen, i = 0;
	char *varname;
	int rc = 0;

	namelen = dentry->d_name.len;

	varname = kzalloc(namelen + 1, GFP_KERNEL);
	if (!varname)
		return -ENOMEM;

	for (i = 0; i < namelen; i++)
		varname[i] = dentry->d_name.name[i];
	varname[i] = '\0';

	rc = validate_name(varname);
	if (rc)
		goto out;

	rc = validate_name(varname);
	if (rc)
		goto out;

	rc = fwsecurityfs_create_file(varname, S_IFREG|0644, 0, secvar_dir,
				      dentry, &plpks_secvar_file_operations);
	if (rc)
		pr_err("Error creating file\n");

out:
	kfree(varname);

	return rc;
}

static const struct inode_operations plpks_secvar_dir_inode_operations = {
	.lookup = simple_lookup,
	.create = plpks_secvar_create,
};

static int plpks_fill_secvars(struct super_block *sb)
{
	struct plpks_var *var = NULL;
	int err;
	int i = 0;

	while (names[i] != NULL) {
		var = kzalloc(sizeof(struct plpks_var), GFP_KERNEL);
		var->name = (char *)names[i];
		var->namelen = strlen(names[i]);
		pr_debug("name is %s\n", var->name);
		var->component = NULL;
		i++;
		err = plpks_read_os_var(var);
		if (err) {
			kfree(var);
			continue;
		}

		err = fwsecurityfs_create_file(var->name, S_IFREG|0644,
					       var->datalen, secvar_dir,
					       NULL,
					       &plpks_secvar_file_operations);

		kfree(var);
		if (err) {
			pr_err("Error creating file\n");
			break;
		}
	}
	return  err;
};

int plpks_secvars_init(void)
{
	int error;

	struct super_block *sb;

	secvar_dir = fwsecurityfs_create_dir("secvars", S_IFDIR | 0755, NULL,
					     &plpks_secvar_dir_inode_operations);
	if (IS_ERR(secvar_dir)) {
		int ret = PTR_ERR(secvar_dir);

		if (ret != -ENODEV)
			pr_err("Unable to create integrity sysfs dir: %d\n",
					ret);
		secvar_dir = NULL;
		return ret;
	}

	sb = fwsecurityfs_get_superblock();
	error = plpks_fill_secvars(sb);
	if (error)
		pr_err("Filling secvars failed\n");

	return 0;
};
