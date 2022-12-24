// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Freescale P2020 board Setup
 *
 * Copyright 2007,2009,2012-2013 Freescale Semiconductor Inc.
 * Copyright 2022 Pali Rohár <pali@kernel.org>
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
#include <linux/fsl/guts.h>

#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/pci-bridge.h>
#include <mm/mmu_decl.h>
#include <asm/udbg.h>
#include <asm/mpic.h>
#include <asm/i8259.h>
#include <asm/swiotlb.h>

#include <soc/fsl/qe/qe.h>

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>
#include "smp.h"

#include "mpc85xx.h"

#undef DEBUG

#ifdef DEBUG
#define DBG(fmt, args...) printk(KERN_ERR "%s: " fmt, __func__, ## args)
#else
#define DBG(fmt, args...)
#endif

#ifdef CONFIG_PPC_I8259

static void mpc85xx_8259_cascade(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int cascade_irq = i8259_irq();

	if (cascade_irq) {
		generic_handle_irq(cascade_irq);
	}
	chip->irq_eoi(&desc->irq_data);
}

static void __init mpc85xx_8259_init(void)
{
	struct device_node *np;
	struct device_node *cascade_node = NULL;
	int cascade_irq;

	for_each_node_by_type(np, "interrupt-controller")
	    if (of_device_is_compatible(np, "chrp,iic")) {
		cascade_node = np;
		break;
	}

	if (cascade_node == NULL) {
		printk(KERN_DEBUG "Could not find i8259 PIC\n");
		return;
	}

	cascade_irq = irq_of_parse_and_map(cascade_node, 0);
	if (!cascade_irq) {
		printk(KERN_ERR "Failed to map cascade interrupt\n");
		return;
	}

	DBG("i8259: cascade mapped to irq %d\n", cascade_irq);

	i8259_init(cascade_node, 0);
	of_node_put(cascade_node);

	irq_set_chained_handler(cascade_irq, mpc85xx_8259_cascade);
}

#endif	/* CONFIG_PPC_I8259 */

static void __init p2020_pic_init(void)
{
	struct mpic *mpic;

	mpic = mpic_alloc(NULL, 0,
		  MPIC_BIG_ENDIAN |
		  MPIC_SINGLE_DEST_CPU,
		0, 256, " OpenPIC  ");

	BUG_ON(mpic == NULL);
	mpic_init(mpic);

#ifdef CONFIG_PPC_I8259
	mpc85xx_8259_init();
#endif
}

#ifdef CONFIG_PCI
extern int uli_exclude_device(struct pci_controller *hose,
				u_char bus, u_char devfn);

static struct device_node *pci_with_uli;

static int mpc85xx_exclude_device(struct pci_controller *hose,
				   u_char bus, u_char devfn)
{
	if (hose->dn == pci_with_uli)
		return uli_exclude_device(hose, bus, devfn);

	return PCIBIOS_SUCCESSFUL;
}
#endif	/* CONFIG_PCI */

static void __init mpc85xx_ds_uli_init(void)
{
#ifdef CONFIG_PCI
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
#endif
}

/*
 * Setup the architecture
 */
static void __init p2020_setup_arch(void)
{
	swiotlb_detect_4g();
	fsl_pci_assign_primary();
	mpc85xx_ds_uli_init();
	mpc85xx_smp_init();

#ifdef CONFIG_QUICC_ENGINE
	mpc85xx_qe_par_io_init();
#endif
}

machine_arch_initcall(p2020, mpc85xx_common_publish_devices);

/*
 * Called very early, device-tree isn't unflattened
 */
static int __init p2020_probe(void)
{
	struct device_node *p2020_cpu;

	/*
	 * There is no common compatible string for all P2020 boards.
	 * The only common thing is "PowerPC,P2020@0" cpu node.
	 * So check for P2020 board via this cpu node.
	 */
	p2020_cpu = of_find_node_by_path("/cpus/PowerPC,P2020@0");
	if (!p2020_cpu)
		return 0;

	of_node_put(p2020_cpu);
	return 1;
}

define_machine(p2020) {
	.name			= "Freescale P2020",
	.probe			= p2020_probe,
	.setup_arch		= p2020_setup_arch,
	.init_IRQ		= p2020_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
	.pcibios_fixup_phb	= fsl_pcibios_fixup_phb,
#endif
	.get_irq		= mpic_get_irq,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};
