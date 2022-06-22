/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2022 IBM Corporation
 * Author: Nayna Jain <nayna@linux.ibm.com>
 */

#ifndef __FWSECURITYFS_INTERNAL_H
#define __FWSECURITYFS_INTERNAL_H

struct dentry *fwsecurityfs_alloc_dentry(struct dentry *parent,
					 const char *name);

#endif
