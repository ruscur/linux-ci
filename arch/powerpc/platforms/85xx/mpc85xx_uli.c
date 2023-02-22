// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MPC85xx PCI functions for DS Board Setup
 *
 * Author Xianghua Xiao (x.xiao@freescale.com)
 * Roy Zang <tie-fei.zang@freescale.com>
 * 	- Add PCI/PCI Exprees support
 * Copyright 2007 Freescale Semiconductor Inc.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/kdev_t.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>

#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/pci-bridge.h>
#include <mm/mmu_decl.h>
#include <asm/udbg.h>
#include <asm/mpic.h>
#include <asm/i8259.h>
#include <asm/swiotlb.h>
#include <asm/ppc-pci.h>

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>
#include "smp.h"

#include "mpc85xx.h"

static struct device_node *pci_with_uli;

static int mpc85xx_exclude_device(struct pci_controller *hose,
				   u_char bus, u_char devfn)
{
	if (hose->dn == pci_with_uli)
		return uli_exclude_device(hose, bus, devfn);

	return PCIBIOS_SUCCESSFUL;
}

void __init mpc85xx_ds_uli_init(void)
{
	struct device_node *node;

	/* See if we have a ULI under the primary */

	node = of_find_node_by_name(NULL, "uli1575");
	while ((pci_with_uli = of_get_parent(node))) {
		of_node_put(node);
		node = pci_with_uli;

		if (pci_with_uli == fsl_pci_primary) {
			ppc_md.pci_exclude_device = mpc85xx_exclude_device;
			break;
		}
	}
}
