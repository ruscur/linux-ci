// SPDX-License-Identifier: GPL-2.0-only
/*
 * POWER LPAR Platform KeyStore (PLPKS)
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 *
 * Provides access to variables stored in Power LPAR Platform KeyStore(PLPKS).
 */

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <asm/hvcall.h>
#include <asm/plpks.h>
#include <asm/unaligned.h>
#include <asm/machdep.h>

#define MODULE_VERS "1.0"
#define MODULE_NAME "pseries-plpks"

#define PKS_FW_OWNER   0x1
#define PKS_BOOTLOADER_OWNER   0x2
#define PKS_OS_OWNER   0x3

#define MAX_LABEL_ATTR_SIZE 16
#define MAX_NAME_SIZE 239
#define MAX_DATA_SIZE 4000

#define PKS_FLUSH_MAX_TIMEOUT	5000	//msec
#define PKS_FLUSH_SLEEP		10	//msec
#define PKS_FLUSH_SLEEP_RANGE	400

static bool configset;
static struct plpks_config *config;
static u8 *ospassword;
static u16 ospasswordlength;

struct plpks_auth {
	u8 version;
	u8 consumer;
	__be64 rsvd0;
	__be32 rsvd1;
	__be16 passwordlength;
	u8 password[];
} __packed __aligned(16);

struct label_attr {
	u8 prefix[8];
	u8 version;
	u8 os;
	u8 length;
	u8 reserved[5];
};

struct label {
	struct label_attr attr;
	u8 name[MAX_NAME_SIZE];
};

static int pseries_status_to_err(int rc)
{
	int err;

	switch (rc) {
	case H_SUCCESS:
		err = 0;
		break;
	case H_FUNCTION:
		err = -ENXIO;
		break;
	case H_P2:
	case H_P3:
	case H_P4:
	case H_P5:
	case H_P6:
		err = -EINVAL;
		break;
	case H_NOT_FOUND:
		err = -ENOENT;
		break;
	case H_BUSY:
		err = -EBUSY;
		break;
	case H_AUTHORITY:
		err = -EPERM;
		break;
	case H_NO_MEM:
		err = -ENOMEM;
		break;
	case H_RESOURCE:
		err = -EEXIST;
		break;
	case H_TOO_BIG:
		err = -EFBIG;
		break;
	default:
		err = -EINVAL;
	}

	return err;
}

static int plpks_gen_password(void)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE] = {0};
	u8 consumer = PKS_OS_OWNER;
	int rc;

	ospassword = kzalloc(config->maxpwsize, GFP_KERNEL);
	if (!ospassword)
		return -ENOMEM;

	ospasswordlength = config->maxpwsize;
	rc = plpar_hcall(H_PKS_GEN_PASSWORD,
			 retbuf,
			 consumer,
			 0,
			 virt_to_phys(ospassword),
			 config->maxpwsize);

	return pseries_status_to_err(rc);
}

static int construct_auth(u8 consumer, struct plpks_auth **auth)
{
	pr_debug("max password size is %u\n", config->maxpwsize);

	if (!auth || consumer > 3)
		return -EINVAL;

	*auth = kmalloc(struct_size(*auth, password, config->maxpwsize),
			GFP_KERNEL);
	if (!*auth)
		return -ENOMEM;

	(*auth)->version = 1;
	(*auth)->consumer = consumer;
	(*auth)->rsvd0 = 0;
	(*auth)->rsvd1 = 0;
	if (consumer == PKS_FW_OWNER || consumer == PKS_BOOTLOADER_OWNER) {
		pr_debug("consumer is bootloader or firmware\n");
		(*auth)->passwordlength = 0;
		return 0;
	}

	(*auth)->passwordlength = (__force __be16)ospasswordlength;

	memcpy((*auth)->password, ospassword,
	       flex_array_size(*auth, password,
	       (__force u16)((*auth)->passwordlength)));
	(*auth)->passwordlength = cpu_to_be16((__force u16)((*auth)->passwordlength));

	return 0;
}

/**
 * Label is combination of label attributes + name.
 * Label attributes are used internally by kernel and not exposed to the user.
 */
static int construct_label(char *component, u8 varos, u8 *name, u16 namelen, u8 **label)
{
	int varlen;
	int len = 0;
	int llen = 0;
	int i;
	int rc = 0;
	u8 labellength = MAX_LABEL_ATTR_SIZE;

	if (!label)
		return -EINVAL;

	varlen = namelen + sizeof(struct label_attr);
	*label = kzalloc(varlen, GFP_KERNEL);

	if (!*label)
		return -ENOMEM;

	if (component) {
		len = strlen(component);
		memcpy(*label, component, len);
	}
	llen = len;

	if (component)
		len = 8 - strlen(component);
	else
		len = 8;

	memset(*label + llen, 0, len);
	llen = llen + len;

	((*label)[llen]) = 0;
	llen = llen + 1;

	memcpy(*label + llen, &varos, 1);
	llen = llen + 1;

	memcpy(*label + llen, &labellength, 1);
	llen = llen + 1;

	memset(*label + llen, 0, 5);
	llen = llen + 5;

	memcpy(*label + llen, name, namelen);
	llen = llen + namelen;

	for (i = 0; i < llen; i++)
		pr_debug("%c", (*label)[i]);

	rc = llen;
	return rc;
}

static int _plpks_get_config(void)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE] = {0};
	int rc;
	size_t size = sizeof(struct plpks_config);

	config = kzalloc(size, GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	rc = plpar_hcall(H_PKS_GET_CONFIG,
			 retbuf,
			 virt_to_phys(config),
			 size);

	if (rc != H_SUCCESS)
		return pseries_status_to_err(rc);

	config->rsvd0 = be32_to_cpu((__force __be32)config->rsvd0);
	config->maxpwsize = be16_to_cpu((__force __be16)config->maxpwsize);
	config->maxobjlabelsize = be16_to_cpu((__force __be16)config->maxobjlabelsize);
	config->maxobjsize = be16_to_cpu((__force __be16)config->maxobjsize);
	config->totalsize = be32_to_cpu((__force __be32)config->totalsize);
	config->usedspace = be32_to_cpu((__force __be32)config->usedspace);
	config->supportedpolicies = be32_to_cpu((__force __be32)config->supportedpolicies);
	config->rsvd1 = be64_to_cpu((__force __be64)config->rsvd1);

	configset = true;

	return 0;
}

static int plpks_confirm_object_flushed(u8 *label, u16 labellen)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE] = {0};
	int rc;
	u64 timeout = 0;
	struct plpks_auth *auth;
	u8 status;
	int i;

	rc = construct_auth(PKS_OS_OWNER, &auth);
	if (rc)
		return rc;

	for (i = 0; i < labellen; i++)
		pr_debug("%02x ", label[i]);

	do {
		rc = plpar_hcall(H_PKS_CONFIRM_OBJECT_FLUSHED,
				 retbuf,
				 virt_to_phys(auth),
				 virt_to_phys(label),
				 labellen);

		status = retbuf[0];
		if (rc) {
			pr_info("rc is %d, status is %d\n", rc, status);
			if (rc == H_NOT_FOUND && status == 1)
				rc = 0;
			break;
		}

		pr_debug("rc is %d, status is %d\n", rc, status);

		if (!rc && status == 1)
			break;

		usleep_range(PKS_FLUSH_SLEEP, PKS_FLUSH_SLEEP + PKS_FLUSH_SLEEP_RANGE);
		timeout = timeout + PKS_FLUSH_SLEEP;
		pr_debug("timeout is %llu\n", timeout);

	} while (timeout < PKS_FLUSH_MAX_TIMEOUT);

	rc = pseries_status_to_err(rc);

	kfree(auth);

	return rc;
}

int plpks_write_var(struct plpks_var var)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE] = {0};
	int rc;
	u8 *label;
	u16 varlen;
	u8 *data = var.data;
	struct plpks_auth *auth;

	if (!var.component || !data || var.datalen <= 0 ||
	    var.namelen > MAX_NAME_SIZE ||
	    var.datalen > MAX_DATA_SIZE)
		return -EINVAL;

	if (var.policy & SIGNEDUPDATE)
		return -EINVAL;

	rc = construct_auth(PKS_OS_OWNER, &auth);
	if (rc)
		return rc;

	rc = construct_label(var.component, var.os, var.name, var.namelen,
			     &label);
	if (rc <= 0)
		goto out;

	varlen =  rc;
	pr_debug("Name to be written is of label size %d\n", varlen);
	rc = plpar_hcall(H_PKS_WRITE_OBJECT,
			 retbuf,
			 virt_to_phys(auth),
			 virt_to_phys(label),
			 varlen,
			 var.policy,
			 virt_to_phys(data),
			 var.datalen);

	if (!rc)
		rc = plpks_confirm_object_flushed(label, varlen);

	rc = pseries_status_to_err(rc);
	kfree(label);

out:
	kfree(auth);

	return rc;
}
EXPORT_SYMBOL(plpks_write_var);

int plpks_remove_var(char *component, u8 varos, struct plpks_var_name vname)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE] = {0};
	int rc;
	u8 *label;
	u16 varlen;
	struct plpks_auth *auth;

	if (!component || vname.namelen > MAX_NAME_SIZE)
		return -EINVAL;

	rc = construct_auth(PKS_OS_OWNER, &auth);
	if (rc)
		return rc;

	rc = construct_label(component, varos, vname.name, vname.namelen, &label);
	if (rc <= 0)
		goto out;

	varlen = rc;
	pr_debug("Name to be written is of label size %d\n", varlen);
	rc = plpar_hcall(H_PKS_REMOVE_OBJECT,
			 retbuf,
			 virt_to_phys(auth),
			 virt_to_phys(label),
			 varlen);

	if (!rc)
		rc = plpks_confirm_object_flushed(label, varlen);

	rc = pseries_status_to_err(rc);
	kfree(label);

out:
	kfree(auth);

	return rc;
}
EXPORT_SYMBOL(plpks_remove_var);

static int plpks_read_var(u8 consumer, struct plpks_var *var)
{
	unsigned long retbuf[PLPAR_HCALL_BUFSIZE] = {0};
	int rc;
	u16 outlen = config->maxobjsize;
	u8 *label;
	u8 *out;
	u16 varlen;
	struct plpks_auth *auth;

	if (var->namelen > MAX_NAME_SIZE)
		return -EINVAL;

	rc = construct_auth(PKS_OS_OWNER, &auth);
	if (rc)
		return rc;

	rc = construct_label(var->component, var->os, var->name, var->namelen,
			     &label);
	if (rc <= 0)
		goto out;

	varlen = rc;
	pr_debug("Name to be written is of label size %d\n", varlen);
	out = kzalloc(outlen, GFP_KERNEL);
	if (!out)
		goto out1;

	rc = plpar_hcall(H_PKS_READ_OBJECT,
			 retbuf,
			 virt_to_phys(auth),
			 virt_to_phys(label),
			 varlen,
			 virt_to_phys(out),
			 outlen);

	if (rc != H_SUCCESS) {
		pr_err("Failed to read %d\n", rc);
		rc = pseries_status_to_err(rc);
		goto out2;
	}

	if (var->datalen == 0 || var->datalen > retbuf[0])
		var->datalen = retbuf[0];

	var->data = kzalloc(var->datalen, GFP_KERNEL);
	if (!var->data) {
		rc = -ENOMEM;
		goto out2;
	}
	var->policy = retbuf[1];

	memcpy(var->data, out, var->datalen);

out2:
	kfree(out);

out1:
	kfree(label);

out:
	kfree(auth);

	return rc;
}

int plpks_read_os_var(struct plpks_var *var)
{
	return plpks_read_var(PKS_OS_OWNER, var);
}
EXPORT_SYMBOL(plpks_read_os_var);

int plpks_read_fw_var(struct plpks_var *var)
{
	return plpks_read_var(PKS_FW_OWNER, var);
}
EXPORT_SYMBOL(plpks_read_fw_var);

int plpks_read_bootloader_var(struct plpks_var *var)
{
	return plpks_read_var(PKS_BOOTLOADER_OWNER, var);
}
EXPORT_SYMBOL(plpks_read_bootloader_var);

struct plpks_config *plpks_get_config(void)
{
	if (!configset) {
		if (_plpks_get_config())
			return NULL;
	}

	return config;
}
EXPORT_SYMBOL(plpks_get_config);

static __init int pseries_plpks_init(void)
{
	int rc = 0;

	rc = _plpks_get_config();

	if (rc) {
		pr_err("Error initializing plpks\n");
		return rc;
	}

	rc = plpks_gen_password();
	if (rc) {
		if (rc == H_IN_USE) {
			rc = 0;
		} else {
			pr_err("Failed setting password %d\n", rc);
			rc = pseries_status_to_err(rc);
			return rc;
		}
	}

	pr_info("POWER LPAR Platform Keystore initialized successfully\n");

	return rc;
}
arch_initcall(pseries_plpks_init);
