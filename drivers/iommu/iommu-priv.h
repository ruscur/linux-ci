/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 */
#ifndef __IOMMU_PRIV_H
#define __IOMMU_PRIV_H

#include <linux/iommu.h>

int iommu_device_register_bus(struct iommu_device *iommu,
			      const struct iommu_ops *ops, struct bus_type *bus,
			      struct notifier_block *nb);
void iommu_device_unregister_bus(struct iommu_device *iommu,
				 struct bus_type *bus,
				 struct notifier_block *nb);

#endif
