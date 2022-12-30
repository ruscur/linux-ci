// SPDX-License-Identifier: GPL-2.0-only
/*
 * Secure variable implementation using the PowerVM LPAR Platform KeyStore (PLPKS)
 *
 * Copyright 2022, IBM Corporation
 * Authors: Russell Currey
 *          Andrew Donnellan
 *          Nayna Jain
 */

#define pr_fmt(fmt) "secvar: "fmt

#include <linux/printk.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <asm/secvar.h>
#include "plpks.h"

// Config attributes for sysfs
#define PLPKS_CONFIG_ATTR(name, fmt, func)			\
	static ssize_t name##_show(struct kobject *kobj,	\
				   struct kobj_attribute *attr,	\
				   char *buf)			\
	{							\
		return sysfs_emit(buf, fmt, func());		\
	}							\
	static struct kobj_attribute attr_##name = __ATTR_RO(name)

PLPKS_CONFIG_ATTR(version, "%u\n", plpks_get_version);
PLPKS_CONFIG_ATTR(max_object_size, "%u\n", plpks_get_maxobjectsize);
PLPKS_CONFIG_ATTR(total_size, "%u\n", plpks_get_totalsize);
PLPKS_CONFIG_ATTR(used_space, "%u\n", plpks_get_usedspace);
PLPKS_CONFIG_ATTR(supported_policies, "%08x\n", plpks_get_supportedpolicies);
PLPKS_CONFIG_ATTR(signed_update_algorithms, "%016llx\n", plpks_get_signedupdatealgorithms);

static const struct attribute *config_attrs[] = {
	&attr_version.attr,
	&attr_max_object_size.attr,
	&attr_total_size.attr,
	&attr_used_space.attr,
	&attr_supported_policies.attr,
	&attr_signed_update_algorithms.attr,
	NULL,
};

static u16 get_ucs2name(const char *name, uint8_t **ucs2_name)
{
	int namelen = strlen(name) * 2;
	*ucs2_name = kzalloc(namelen, GFP_KERNEL);

	if (!*ucs2_name)
		return 0;

	for (int i = 0; name[i]; i++) {
		(*ucs2_name)[i * 2] = name[i];
		(*ucs2_name)[i * 2 + 1] = '\0';
	}

	return namelen;
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

#define PLPKS_SECVAR_COUNT 8
static char *var_names[PLPKS_SECVAR_COUNT] = {
	"PK",
	"KEK",
	"db",
	"dbx",
	"grubdb",
	"sbat",
	"moduledb",
	"trustedcadb",
};

static int plpks_get_variable(const char *key, uint64_t key_len,
			      u8 *data, uint64_t *data_size)
{
	struct plpks_var var = {0};
	u16 ucs2_namelen;
	u8 *ucs2_name;
	int rc = 0;

	ucs2_namelen = get_ucs2name(key, &ucs2_name);
	if (!ucs2_namelen)
		return -ENOMEM;

	var.name = ucs2_name;
	var.namelen = ucs2_namelen;
	var.os = PLPKS_VAR_LINUX;
	rc = plpks_read_os_var(&var);

	if (rc)
		goto err;

	*data_size = var.datalen + sizeof(var.policy);

	// We can be called with data = NULL to just get the object size.
	if (data) {
		memcpy(data, &var.policy, sizeof(var.policy));
		memcpy(data + sizeof(var.policy), var.data, var.datalen);
	}

	kfree(var.data);
err:
	kfree(ucs2_name);
	return rc;
}

static int plpks_set_variable(const char *key, uint64_t key_len,
			      u8 *data, uint64_t data_size)
{
	struct plpks_var var = {0};
	u16 ucs2_namelen;
	u8 *ucs2_name;
	int rc = 0;
	u64 flags;

	// Secure variables need to be prefixed with 8 bytes of flags.
	// We only want to perform the write if we have at least one byte of data.
	if (data_size <= sizeof(flags))
		return -EINVAL;

	ucs2_namelen = get_ucs2name(key, &ucs2_name);
	if (!ucs2_namelen)
		return -ENOMEM;

	memcpy(&flags, data, sizeof(flags));

	var.datalen = data_size - sizeof(flags);
	var.data = kzalloc(var.datalen, GFP_KERNEL);
	if (!var.data) {
		rc = -ENOMEM;
		goto err;
	}

	memcpy(var.data, data + sizeof(flags), var.datalen);

	var.name = ucs2_name;
	var.namelen = ucs2_namelen;
	var.os = PLPKS_VAR_LINUX;
	var.policy = get_policy(key);

	rc = plpks_signed_update_var(var, flags);

	kfree(var.data);
err:
	kfree(ucs2_name);
	return rc;
}

/*
 * get_next() in the secvar API is designed for the OPAL API.
 * If *key is 0, it returns the first variable in the keystore.
 * Otherwise, you pass the name of a key and it returns next in line.
 *
 * We're going to cheat here - since we have fixed keys and don't care about
 * key_len, we can just use it as an index.
 */
static int plpks_get_next_variable(const char *key, uint64_t *key_len, uint64_t keybufsize)
{
	if (!key || !key_len)
		return -EINVAL;

	if (*key_len >= PLPKS_SECVAR_COUNT)
		return -ENOENT;

	if (strscpy((char *)key, var_names[(*key_len)++], keybufsize) < 0)
		return -E2BIG;

	return 0;
}

// PLPKS dynamic secure boot doesn't give us a format string in the same way OPAL does.
// Instead, report the format using the SB_VERSION variable in the keystore.
static ssize_t plpks_secvar_format(char *buf)
{
	struct plpks_var var = {0};
	ssize_t ret;

	var.component = NULL;
	// Only the signed variables have ucs2-encoded names, this one doesn't
	var.name = "SB_VERSION";
	var.namelen = 10;
	var.datalen = 0;
	var.data = NULL;

	// Unlike the other vars, SB_VERSION is owned by firmware instead of the OS
	ret = plpks_read_fw_var(&var);
	if (ret) {
		if (ret == -ENOENT)
			return sysfs_emit(buf, "ibm,plpks-sb-unknown\n");

		pr_err("Error %ld reading SB_VERSION from firmware\n", ret);
		return ret;
	}

	// Hypervisor defines SB_VERSION as a "1 byte unsigned integer value"
	ret = sysfs_emit(buf, "ibm,plpks-sb-%hhu\n", var.data[0]);

	kfree(var.data);
	return ret;
}

static int plpks_max_size(uint64_t *max_size)
{
	// The max object size reported by the hypervisor is accurate for the
	// object itself, but we use the first 8 bytes of data on write as the
	// signed update flags, so the max size a user can write is larger.
	*max_size = (uint64_t)plpks_get_maxobjectsize() + 8;

	return 0;
}


static const struct secvar_operations plpks_secvar_ops = {
	.get = plpks_get_variable,
	.get_next = plpks_get_next_variable,
	.set = plpks_set_variable,
	.format = plpks_secvar_format,
	.max_size = plpks_max_size,
};

static int plpks_secvar_init(void)
{
	if (!plpks_is_available())
		return -ENODEV;

	set_secvar_ops(&plpks_secvar_ops);
	set_secvar_config_attrs(config_attrs);
	return 0;
}
device_initcall(plpks_secvar_init);
