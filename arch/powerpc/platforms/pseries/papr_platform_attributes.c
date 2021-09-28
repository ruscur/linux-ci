// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Platform energy and frequency attributes driver
 *
 * This driver creates a sys file at /sys/firmware/papr/ which encapsulates a
 * directory structure containing files in keyword - value pairs that specify
 * energy and frequency configuration of the system.
 *
 * The format of exposing the sysfs information is as follows:
 * /sys/firmware/papr/energy_scale_info/
 *  |-- <id>/
 *    |-- desc
 *    |-- value
 *    |-- value_desc (if exists)
 *  |-- <id>/
 *    |-- desc
 *    |-- value
 *    |-- value_desc (if exists)
 *
 * Copyright 2021 IBM Corp.
 */

#include <asm/hvcall.h>
#include <asm/machdep.h>

#include "pseries.h"

/*
 * Flag attributes to fetch either all or one attribute from the HCALL
 * flag = BE(0) => fetch all attributes with firstAttributeId = 0
 * flag = BE(1) => fetch a single attribute with firstAttributeId = id
 */
#define ESI_FLAGS_ALL		0
#define ESI_FLAGS_SINGLE	PPC_BIT(0)

#define MAX_ATTRS		3

struct papr_attr {
	u64 id;
	struct kobj_attribute kobj_attr;
};
struct papr_group {
	struct attribute_group pg;
	struct papr_attr pgattrs[MAX_ATTRS];
} *pgs;

/* /sys/firmware/papr */
struct kobject *papr_kobj;
/* /sys/firmware/papr/energy_scale_info */
struct kobject *esi_kobj;

/*
 * Extract and export the description of the energy scale attributes
 */
static ssize_t papr_show_desc(struct kobject *kobj,
			       struct kobj_attribute *kobj_attr,
			       char *buf)
{
	struct papr_attr *pattr = container_of(kobj_attr, struct papr_attr,
					       kobj_attr);
	struct h_energy_scale_info_hdr *t_hdr;
	struct energy_scale_attribute *t_esi;
	char *t_buf;
	int ret = 0;

	t_buf = kmalloc(MAX_BUF_SZ, GFP_KERNEL);
	if (t_buf == NULL)
		return -ENOMEM;

	ret = plpar_hcall_norets(H_GET_ENERGY_SCALE_INFO, ESI_FLAGS_SINGLE,
				 pattr->id, virt_to_phys(t_buf),
				 MAX_BUF_SZ);

	if (ret != H_SUCCESS) {
		pr_warn("hcall failed: H_GET_ENERGY_SCALE_INFO");
		goto out;
	}

	t_hdr = (struct h_energy_scale_info_hdr *) t_buf;
	t_esi = (struct energy_scale_attribute *)
		(t_buf + be64_to_cpu(t_hdr->array_offset));

	ret = snprintf(buf, sizeof(t_esi->desc), "%s\n", t_esi->desc);
	if (ret < 0)
		ret = -EIO;
out:
	kfree(t_buf);

	return ret;
}

/*
 * Extract and export the numeric value of the energy scale attributes
 */
static ssize_t papr_show_value(struct kobject *kobj,
				struct kobj_attribute *kobj_attr,
				char *buf)
{
	struct papr_attr *pattr = container_of(kobj_attr, struct papr_attr,
					       kobj_attr);
	struct h_energy_scale_info_hdr *t_hdr;
	struct energy_scale_attribute *t_esi;
	char *t_buf;
	int ret = 0;

	t_buf = kmalloc(MAX_BUF_SZ, GFP_KERNEL);
	if (t_buf == NULL)
		return -ENOMEM;

	ret = plpar_hcall_norets(H_GET_ENERGY_SCALE_INFO, ESI_FLAGS_SINGLE,
				 pattr->id, virt_to_phys(t_buf),
				 MAX_BUF_SZ);

	if (ret != H_SUCCESS) {
		pr_warn("hcall failed: H_GET_ENERGY_SCALE_INFO");
		goto out;
	}

	t_hdr = (struct h_energy_scale_info_hdr *) t_buf;
	t_esi = (struct energy_scale_attribute *)
		(t_buf + be64_to_cpu(t_hdr->array_offset));

	ret = snprintf(buf, sizeof(t_esi->value), "%llu\n",
		       be64_to_cpu(t_esi->value));
	if (ret < 0)
		ret = -EIO;
out:
	kfree(t_buf);

	return ret;
}

/*
 * Extract and export the value description in string format of the energy
 * scale attributes
 */
static ssize_t papr_show_value_desc(struct kobject *kobj,
				     struct kobj_attribute *kobj_attr,
				     char *buf)
{
	struct papr_attr *pattr = container_of(kobj_attr, struct papr_attr,
					       kobj_attr);
	struct h_energy_scale_info_hdr *t_hdr;
	struct energy_scale_attribute *t_esi;
	char *t_buf;
	int ret = 0;

	t_buf = kmalloc(MAX_BUF_SZ, GFP_KERNEL);
	if (t_buf == NULL)
		return -ENOMEM;

	ret = plpar_hcall_norets(H_GET_ENERGY_SCALE_INFO, ESI_FLAGS_SINGLE,
				 pattr->id, virt_to_phys(t_buf),
				 MAX_BUF_SZ);

	if (ret != H_SUCCESS) {
		pr_warn("hcall failed: H_GET_ENERGY_SCALE_INFO");
		goto out;
	}

	t_hdr = (struct h_energy_scale_info_hdr *) t_buf;
	t_esi = (struct energy_scale_attribute *)
		(t_buf + be64_to_cpu(t_hdr->array_offset));

	ret = snprintf(buf, sizeof(t_esi->value_desc), "%s\n",
		       t_esi->value_desc);
	if (ret < 0)
		ret = -EIO;
out:
	kfree(t_buf);

	return ret;
}

static struct papr_ops_info {
	const char *attr_name;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *kobj_attr,
			char *buf);
} ops_info[MAX_ATTRS] = {
	{ "desc", papr_show_desc },
	{ "value", papr_show_value },
	{ "value_desc", papr_show_value_desc },
};

static void add_attr(u64 id, int index, struct papr_attr *attr)
{
	attr->id = id;
	sysfs_attr_init(&attr->kobj_attr.attr);
	attr->kobj_attr.attr.name = ops_info[index].attr_name;
	attr->kobj_attr.attr.mode = 0444;
	attr->kobj_attr.show = ops_info[index].show;
}

static int add_attr_group(u64 id, struct papr_group *pg, bool show_val_desc)
{
	int i;

	for (i = 0; i < MAX_ATTRS; i++) {
		if (!strcmp(ops_info[i].attr_name, "value_desc") &&
		    !show_val_desc) {
			continue;
		}
		add_attr(id, i, &pg->pgattrs[i]);
		pg->pg.attrs[i] = &pg->pgattrs[i].kobj_attr.attr;
	}

	return sysfs_create_group(esi_kobj, &pg->pg);
}

static int __init papr_init(void)
{
	struct h_energy_scale_info_hdr *esi_hdr;
	struct energy_scale_attribute *esi_attrs;
	uint64_t num_attrs;
	int ret, idx, i;
	char *esi_buf;

	if (!firmware_has_feature(FW_FEATURE_LPAR))
		return -ENXIO;

	esi_buf = kmalloc(MAX_BUF_SZ, GFP_KERNEL);
	if (esi_buf == NULL)
		return -ENOMEM;
	/*
	 * hcall(
	 * uint64 H_GET_ENERGY_SCALE_INFO,  // Get energy scale info
	 * uint64 flags,            // Per the flag request
	 * uint64 firstAttributeId, // The attribute id
	 * uint64 bufferAddress,    // Guest physical address of the output buffer
	 * uint64 bufferSize);      // The size in bytes of the output buffer
	 */
	ret = plpar_hcall_norets(H_GET_ENERGY_SCALE_INFO, ESI_FLAGS_ALL, 0,
				 virt_to_phys(esi_buf), MAX_BUF_SZ);
	if (ret != H_SUCCESS) {
		pr_warn("hcall failed: H_GET_ENERGY_SCALE_INFO");
		goto out;
	}

	esi_hdr = (struct h_energy_scale_info_hdr *) esi_buf;
	if (esi_hdr->data_header_version != ESI_VERSION) {
		pr_warn("H_GET_ENERGY_SCALE_INFO VER MISMATCH - EXP: 0x%x, REC: 0x%x",
			ESI_VERSION, esi_hdr->data_header_version);
	}

	num_attrs = be64_to_cpu(esi_hdr->num_attrs);
	esi_attrs = (struct energy_scale_attribute *)
		    (esi_buf + be64_to_cpu(esi_hdr->array_offset));

	pgs = kcalloc(num_attrs, sizeof(*pgs), GFP_KERNEL);
	if (!pgs)
		goto out;

	papr_kobj = kobject_create_and_add("papr", firmware_kobj);
	if (!papr_kobj) {
		pr_warn("kobject_create_and_add papr failed\n");
		goto out_pgs;
	}

	esi_kobj = kobject_create_and_add("energy_scale_info", papr_kobj);
	if (!esi_kobj) {
		pr_warn("kobject_create_and_add energy_scale_info failed\n");
		goto out_kobj;
	}

	for (idx = 0; idx < num_attrs; idx++) {
		bool show_val_desc = true;

		pgs[idx].pg.attrs = kcalloc(MAX_ATTRS + 1,
					    sizeof(*pgs[idx].pg.attrs),
					    GFP_KERNEL);
		if (!pgs[idx].pg.attrs) {
			goto out_pgattrs;
		}

		pgs[idx].pg.name = kasprintf(GFP_KERNEL, "%lld",
					     be64_to_cpu(esi_attrs[idx].id));
		if (pgs[idx].pg.name == NULL) {
			goto out_pgattrs;
		}
		/* Do not add the value description if it does not exist */
		if (strnlen(esi_attrs[idx].value_desc,
			    sizeof(esi_attrs[idx].value_desc)) == 0)
			show_val_desc = false;

		if (add_attr_group(be64_to_cpu(esi_attrs[idx].id), &pgs[idx],
				   show_val_desc)) {
			pr_warn("Failed to create papr attribute group %s\n",
				pgs[idx].pg.name);
			goto out_pgattrs;
		}
	}

	kfree(esi_buf);
	return 0;

out_pgattrs:
	for (i = 0; i < idx ; i++) {
		kfree(pgs[i].pg.attrs);
		kfree(pgs[i].pg.name);
	}
	kobject_put(esi_kobj);
out_kobj:
	kobject_put(papr_kobj);
out_pgs:
	kfree(pgs);
out:
	kfree(esi_buf);

	return -ENOMEM;
}

machine_device_initcall(pseries, papr_init);
