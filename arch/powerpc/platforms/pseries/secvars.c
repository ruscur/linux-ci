// SPDX-License-Identifier: GPL-2.0-only
/*
 * Expose secure(authenticated) variables for user key management.
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 *
 */

#include <linux/fwsecurityfs.h>
#include "plpks.h"

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

static u16 get_ucs2name(const char *name, uint8_t **ucs2_name)
{
	int i = 0;
	int j = 0;
	int namelen = 0;

	namelen = strlen(name) * 2;

	*ucs2_name = kzalloc(namelen, GFP_KERNEL);
	if (!*ucs2_name)
		return 0;

	while (name[i]) {
		(*ucs2_name)[j++] = name[i];
		(*ucs2_name)[j++] = '\0';
		pr_debug("ucs2name is %c\n", (*ucs2_name)[j - 2]);
		i++;
	}

	return namelen;
}

static int validate_name(const char *name)
{
	int i = 0;

	while (names[i]) {
		if ((strcmp(name, names[i]) == 0))
			return 0;
		i++;
	}
	pr_err("Invalid name, allowed ones are (PK,KEK,db,dbx,grubdb,sbat,moduledb,trustedcadb)\n");

	return -EINVAL;
}

static u32 get_policy(const char *name)
{
	if ((strcmp(name, "db") == 0) ||
	    (strcmp(name, "dbx") == 0) ||
	    (strcmp(name, "grubdb") == 0) ||
	    (strcmp(name, "sbat") == 0))
		return (WORLDREADABLE | SIGNEDUPDATE);
	else
		return SIGNEDUPDATE;
}

static ssize_t plpks_secvar_file_write(struct file *file,
				       const char __user *userbuf,
				       size_t count, loff_t *ppos)
{
	struct plpks_var var;
	void *data;
	u16 ucs2_namelen;
	u8 *ucs2_name = NULL;
	u64 flags;
	ssize_t rc;
	bool exist = true;
	u16 datasize = count;
	struct inode *inode = file->f_mapping->host;

	if (count <= sizeof(flags))
		return -EINVAL;

	ucs2_namelen = get_ucs2name(file_dentry(file)->d_iname, &ucs2_name);
	if (ucs2_namelen == 0)
		return -ENOMEM;

	rc = copy_from_user(&flags, userbuf, sizeof(flags));
	if (rc)
		return -EFAULT;

	datasize = count - sizeof(flags);

	data = memdup_user(userbuf + sizeof(flags), datasize);
	if (IS_ERR(data))
		return PTR_ERR(data);

	var.component = NULL;
	var.name = ucs2_name;
	var.namelen = ucs2_namelen;
	var.os = PLPKS_VAR_LINUX;
	var.datalen = 0;
	var.data = NULL;

	/* If PKS variable doesn't exist, it implies first time creation */
	rc = plpks_read_os_var(&var);
	if (rc) {
		if (rc == -ENOENT) {
			exist = false;
		} else {
			pr_err("Reading variable %s failed with error %ld\n",
			       file_dentry(file)->d_iname, rc);
			goto out;
		}
	}

	var.datalen = datasize;
	var.data = data;
	var.policy = get_policy(file_dentry(file)->d_iname);
	rc = plpks_signed_update_var(var, flags);
	if (rc) {
		pr_err("Update of the variable %s failed with error %ld\n",
		       file_dentry(file)->d_iname, rc);
		if (!exist)
			fwsecurityfs_remove_file(file_dentry(file));
		goto out;
	}

	/* Read variable again to get updated size of the object */
	var.datalen = 0;
	var.data = NULL;
	rc = plpks_read_os_var(&var);
	if (rc)
		pr_err("Error updating file size\n");

	inode_lock(inode);
	i_size_write(inode, var.datalen);
	inode->i_mtime = current_time(inode);
	inode_unlock(inode);

	rc = count;
out:
	kfree(data);
	kfree(ucs2_name);

	return rc;
}

static ssize_t __secvar_os_file_read(char *name, char **out, u32 *outlen)
{
	struct plpks_var var;
	int rc;
	u8 *ucs2_name = NULL;
	u16 ucs2_namelen;

	ucs2_namelen = get_ucs2name(name, &ucs2_name);
	if (ucs2_namelen == 0)
		return -ENOMEM;

	var.component = NULL;
	var.name = ucs2_name;
	var.namelen = ucs2_namelen;
	var.os = PLPKS_VAR_LINUX;
	var.datalen = 0;
	var.data = NULL;
	rc = plpks_read_os_var(&var);
	if (rc) {
		pr_err("Error %d reading object %s from firmware\n", rc, name);
		kfree(ucs2_name);
		return rc;
	}

	*outlen = sizeof(var.policy) + var.datalen;
	*out = kzalloc(*outlen, GFP_KERNEL);
	if (!*out) {
		rc = -ENOMEM;
		goto err;
	}

	memcpy(*out, &var.policy, sizeof(var.policy));

	memcpy(*out + sizeof(var.policy), var.data, var.datalen);

err:
	kfree(ucs2_name);
	kfree(var.data);
	return rc;
}

static ssize_t __secvar_fw_file_read(char *name, char **out, u32 *outlen)
{
	struct plpks_var var;
	int rc;

	var.component = NULL;
	var.name = name;
	var.namelen = strlen(name);
	var.datalen = 0;
	var.data = NULL;
	rc = plpks_read_fw_var(&var);
	if (rc) {
		if (rc == -ENOENT) {
			var.datalen = 1;
			var.data = kzalloc(var.datalen, GFP_KERNEL);
			rc = 0;
		} else {
			pr_err("Error %d reading object %s from firmware\n",
			       rc, name);
			return rc;
		}
	}

	*outlen = var.datalen;
	*out = kzalloc(*outlen, GFP_KERNEL);
	if (!*out) {
		kfree(var.data);
		return -ENOMEM;
	}

	memcpy(*out, var.data, var.datalen);

	kfree(var.data);
	return 0;
}

static ssize_t plpks_secvar_file_read(struct file *file, char __user *userbuf,
				      size_t count, loff_t *ppos)
{
	int rc;
	char *out = NULL;
	u32 outlen;
	char *fname = file_dentry(file)->d_iname;

	if (strcmp(fname, "SB_VERSION") == 0)
		rc = __secvar_fw_file_read(fname, &out, &outlen);
	else
		rc = __secvar_os_file_read(fname, &out, &outlen);
	if (!rc)
		rc = simple_read_from_buffer(userbuf, count, ppos,
					     out, outlen);

	kfree(out);

	return rc;
}

static const struct file_operations plpks_secvar_file_operations = {
	.open   = simple_open,
	.read   = plpks_secvar_file_read,
	.write  = plpks_secvar_file_write,
	.llseek = no_llseek,
};

static int plpks_secvar_create(struct user_namespace *mnt_userns,
			       struct inode *dir, struct dentry *dentry,
			       umode_t mode, bool excl)
{
	const char *varname;
	struct dentry *ldentry;
	int rc;

	varname = dentry->d_name.name;

	rc = validate_name(varname);
	if (rc)
		goto out;

	ldentry = fwsecurityfs_create_file(varname, S_IFREG | 0644, 0,
					   secvar_dir, dentry, NULL,
					   &plpks_secvar_file_operations);
	if (IS_ERR(ldentry)) {
		rc = PTR_ERR(ldentry);
		pr_err("Creation of variable %s failed with error %d\n",
		       varname, rc);
	}

out:
	return rc;
}

static const struct inode_operations plpks_secvar_dir_inode_operations = {
	.lookup = simple_lookup,
	.create = plpks_secvar_create,
};

static int plpks_fill_secvars(void)
{
	struct plpks_var var;
	int rc = 0;
	int i = 0;
	u8 *ucs2_name = NULL;
	u16 ucs2_namelen;
	struct dentry *dentry;

	dentry = fwsecurityfs_create_file("SB_VERSION", S_IFREG | 0444, 1,
					  secvar_dir, NULL, NULL,
					  &plpks_secvar_file_operations);
	if (IS_ERR(dentry)) {
		rc = PTR_ERR(dentry);
		pr_err("Creation of variable SB_VERSION failed with error %d\n", rc);
		return rc;
	}

	while (names[i]) {
		ucs2_namelen = get_ucs2name(names[i], &ucs2_name);
		if (ucs2_namelen == 0) {
			i++;
			continue;
		}

		i++;
		var.component = NULL;
		var.name = ucs2_name;
		var.namelen = ucs2_namelen;
		var.os = PLPKS_VAR_LINUX;
		var.datalen = 0;
		var.data = NULL;
		rc = plpks_read_os_var(&var);
		kfree(ucs2_name);
		if (rc) {
			rc = 0;
			continue;
		}

		dentry = fwsecurityfs_create_file(names[i - 1], S_IFREG | 0644,
						  var.datalen, secvar_dir,
						  NULL, NULL,
						  &plpks_secvar_file_operations);

		kfree(var.data);
		if (IS_ERR(dentry)) {
			rc = PTR_ERR(dentry);
			pr_err("Creation of variable %s failed with error %d\n",
			       names[i - 1], rc);
			break;
		}
	}

	return rc;
};

int plpks_secvars_init(struct dentry *parent)
{
	int rc;

	secvar_dir = fwsecurityfs_create_dir("secvars", S_IFDIR | 0755, parent,
					     &plpks_secvar_dir_inode_operations);
	if (IS_ERR(secvar_dir)) {
		rc = PTR_ERR(secvar_dir);
		pr_err("Unable to create secvars dir: %d\n", rc);
		return rc;
	}

	rc = plpks_fill_secvars();
	if (rc)
		pr_err("Filling secvars failed %d\n", rc);

	return rc;
};
