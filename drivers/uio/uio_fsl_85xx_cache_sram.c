// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2022 Wang Wenhu <wenhu.wang@hotmail.com>
 * All rights reserved.
 */

#include <linux/platform_device.h>
#include <linux/uio_driver.h>
#include <linux/stringify.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/io.h>

#define DRIVER_NAME		"uio_mpc85xx_cache_sram"
#define UIO_INFO_VER	"0.0.1"
#define UIO_NAME		"uio_cache_sram"

#define L2CR_L2FI				0x40000000	/* L2 flash invalidate */
#define L2CR_L2IO				0x00200000	/* L2 instruction only */
#define L2CR_SRAM_ZERO			0x00000000	/* L2SRAM zero size */
#define L2CR_SRAM_FULL			0x00010000	/* L2SRAM full size */
#define L2CR_SRAM_HALF			0x00020000	/* L2SRAM half size */
#define L2CR_SRAM_TWO_HALFS		0x00030000	/* L2SRAM two half sizes */
#define L2CR_SRAM_QUART			0x00040000	/* L2SRAM one quarter size */
#define L2CR_SRAM_TWO_QUARTS	0x00050000	/* L2SRAM two quarter size */
#define L2CR_SRAM_EIGHTH		0x00060000	/* L2SRAM one eighth size */
#define L2CR_SRAM_TWO_EIGHTH	0x00070000	/* L2SRAM two eighth size */

#define L2SRAM_OPTIMAL_SZ_SHIFT	0x00000003	/* Optimum size for L2SRAM */

#define L2SRAM_BAR_MSK_LO18		0xFFFFC000	/* Lower 18 bits */
#define L2SRAM_BARE_MSK_HI4		0x0000000F	/* Upper 4 bits */

enum cache_sram_lock_ways {
	LOCK_WAYS_ZERO,
	LOCK_WAYS_EIGHTH,
	LOCK_WAYS_TWO_EIGHTH,
	LOCK_WAYS_HALF = 4,
	LOCK_WAYS_FULL = 8,
};

struct mpc85xx_l2ctlr {
	u32	ctl;		/* 0x000 - L2 control */
	u8	res1[0xC];
	u32	ewar0;		/* 0x010 - External write address 0 */
	u32	ewarea0;	/* 0x014 - External write address extended 0 */
	u32	ewcr0;		/* 0x018 - External write ctrl */
	u8	res2[4];
	u32	ewar1;		/* 0x020 - External write address 1 */
	u32	ewarea1;	/* 0x024 - External write address extended 1 */
	u32	ewcr1;		/* 0x028 - External write ctrl 1 */
	u8	res3[4];
	u32	ewar2;		/* 0x030 - External write address 2 */
	u32	ewarea2;	/* 0x034 - External write address extended 2 */
	u32	ewcr2;		/* 0x038 - External write ctrl 2 */
	u8	res4[4];
	u32	ewar3;		/* 0x040 - External write address 3 */
	u32	ewarea3;	/* 0x044 - External write address extended 3 */
	u32	ewcr3;		/* 0x048 - External write ctrl 3 */
	u8	res5[0xB4];
	u32	srbar0;		/* 0x100 - SRAM base address 0 */
	u32	srbarea0;	/* 0x104 - SRAM base addr reg ext address 0 */
	u32	srbar1;		/* 0x108 - SRAM base address 1 */
	u32	srbarea1;	/* 0x10C - SRAM base addr reg ext address 1 */
	u8	res6[0xCF0];
	u32	errinjhi;	/* 0xE00 - Error injection mask high */
	u32	errinjlo;	/* 0xE04 - Error injection mask low */
	u32	errinjctl;	/* 0xE08 - Error injection tag/ecc control */
	u8	res7[0x14];
	u32	captdatahi;	/* 0xE20 - Error data high capture */
	u32	captdatalo;	/* 0xE24 - Error data low capture */
	u32	captecc;	/* 0xE28 - Error syndrome */
	u8	res8[0x14];
	u32	errdet;		/* 0xE40 - Error detect */
	u32	errdis;		/* 0xE44 - Error disable */
	u32	errinten;	/* 0xE48 - Error interrupt enable */
	u32	errattr;	/* 0xE4c - Error attribute capture */
	u32	erradrrl;	/* 0xE50 - Error address capture low */
	u32	erradrrh;	/* 0xE54 - Error address capture high */
	u32	errctl;		/* 0xE58 - Error control */
	u8	res9[0x1A4];
};

static int uio_cache_sram_setup(struct platform_device *pdev,
				phys_addr_t base, u8 ways)
{
	struct mpc85xx_l2ctlr __iomem *l2ctlr = of_iomap(pdev->dev.of_node, 0);

	if (!l2ctlr) {
		dev_err(&pdev->dev, "can not map l2 controller\n");
		return -EINVAL;
	}

	/* write bits[0-17] to srbar0 */
	out_be32(&l2ctlr->srbar0, lower_32_bits(base) & L2SRAM_BAR_MSK_LO18);

	/* write bits[18-21] to srbare0 */
#ifdef CONFIG_PHYS_64BIT
	out_be32(&l2ctlr->srbarea0, upper_32_bits(base) & L2SRAM_BARE_MSK_HI4);
#endif

	clrsetbits_be32(&l2ctlr->ctl, L2CR_L2E, L2CR_L2FI);

	switch (ways) {
	case LOCK_WAYS_EIGHTH:
		setbits32(&l2ctlr->ctl, L2CR_L2E | L2CR_L2FI | L2CR_SRAM_EIGHTH);
		break;

	case LOCK_WAYS_TWO_EIGHTH:
		setbits32(&l2ctlr->ctl, L2CR_L2E | L2CR_L2FI | L2CR_SRAM_QUART);
		break;

	case LOCK_WAYS_HALF:
		setbits32(&l2ctlr->ctl, L2CR_L2E | L2CR_L2FI | L2CR_SRAM_HALF);
		break;

	case LOCK_WAYS_FULL:
	default:
		setbits32(&l2ctlr->ctl, L2CR_L2E | L2CR_L2FI | L2CR_SRAM_FULL);
		break;
	}
	eieio();

	return 0;
}

static const struct vm_operations_struct uio_cache_sram_vm_ops = {
#ifdef CONFIG_HAVE_IOREMAP_PROT
	.access = generic_access_phys,
#endif
};

static int uio_cache_sram_mmap(struct uio_info *info,
				struct vm_area_struct *vma)
{
	struct uio_mem *mem = info->mem;

	if (mem->addr & ~PAGE_MASK)
		return -ENODEV;

	if ((vma->vm_end - vma->vm_start > mem->size) ||
		(mem->size == 0) ||
		(mem->memtype != UIO_MEM_PHYS))
		return -EINVAL;

	vma->vm_ops = &uio_cache_sram_vm_ops;
	vma->vm_page_prot = pgprot_cached(vma->vm_page_prot);

	return remap_pfn_range(vma,
						   vma->vm_start,
						   mem->addr >> PAGE_SHIFT,
						   vma->vm_end - vma->vm_start,
						   vma->vm_page_prot);
}

static int uio_cache_sram_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct uio_info *info;
	struct uio_mem *uiomem;
	const char *dt_name;
	phys_addr_t mem_base;
	u32 l2cache_size;
	u32 mem_size;
	u32 rem;
	u8 ways;
	int ret;

	if (!node) {
		dev_err(&pdev->dev, "device's of_node is null\n");
		return -EINVAL;
	}

	/* alloc uio_info for one device */
	info = devm_kzalloc(&pdev->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	/* get optional uio name */
	if (of_property_read_string(node, "uio_name", &dt_name))
		dt_name = UIO_NAME;

	info->name = devm_kstrdup(&pdev->dev, dt_name, GFP_KERNEL);
	if (!info->name)
		return -ENOMEM;

	ret = of_property_read_u32(node, "cache-mem-size", &mem_size);
	if (ret) {
		dev_err(&pdev->dev, "missing cache-mem-size\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "cache-mem-base", &mem_base);
	if (ret) {
		dev_err(&pdev->dev, "missing cache-mem-base\n");
		return -EINVAL;
	}

	if (mem_size == 0) {
		dev_err(&pdev->dev, "cache-mem-size should not be 0\n");
		return -EINVAL;
	}

	ret = of_property_read_u32(node, "cache-size", &l2cache_size);
	if (ret) {
		dev_err(&pdev->dev, "missing l2cache-size\n");
		return -EINVAL;
	}

	rem = l2cache_size % mem_size;
	ways = LOCK_WAYS_FULL * mem_size / l2cache_size;
	if (rem || (ways & (ways - 1))) {
		dev_err(&pdev->dev, "illegal cache-sram-size parameter\n");
		return -EINVAL;
	}

	ret = uio_cache_sram_setup(pdev, mem_base, ways);
	if (ret)
		return ret;

	if (!request_mem_region(mem_base, mem_size, "fsl_85xx_cache_sram")) {
		dev_err(&pdev->dev, "uio_cache_sram request memory failed\n");
		ret = -ENXIO;
	}

	info->irq = UIO_IRQ_NONE;
	info->version = UIO_INFO_VER;
	info->mmap = uio_cache_sram_mmap;
	uiomem = info->mem;
	uiomem->memtype = UIO_MEM_PHYS;
	uiomem->addr = mem_base;
	uiomem->size = mem_size;
	uiomem->name = devm_kstrdup(&pdev->dev, node->name, GFP_KERNEL);
	uiomem->internal_addr = ioremap_coherent(mem_base, mem_size);
	if (!uiomem->internal_addr) {
		dev_err(&pdev->dev, "cache ioremep_coherent failed\n");
		ret = -ENOMEM;
	}

	/* register uio device */
	if (uio_register_device(&pdev->dev, info)) {
		dev_err(&pdev->dev, "error uio,cache-sram registration failed\n");
		ret = -ENODEV;
		goto err_out;
	}

	platform_set_drvdata(pdev, info);
	return 0;

err_out:
	iounmap(info->mem[0].internal_addr);
	return ret;
}

static int uio_cache_sram_remove(struct platform_device *pdev)
{
	struct uio_info *info = platform_get_drvdata(pdev);

	uio_unregister_device(info);
	iounmap(info->mem[0].internal_addr);

	return 0;
}

static const struct of_device_id uio_cache_sram_of_match[] = {
	{ .compatible = "fsl,p2020-l2-cache-sram-uio", },
	{}
};
MODULE_DEVICE_TABLE(of, uio_cache_sram_of_match);

static struct platform_driver uio_fsl_85xx_cache_sram = {
	.probe = uio_cache_sram_probe,
	.remove = uio_cache_sram_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table	= uio_cache_sram_of_match,
	},
};

module_platform_driver(uio_fsl_85xx_cache_sram);

MODULE_AUTHOR("Wang Wenhu <wenhu.wang@hotmail.com>");
MODULE_DESCRIPTION("Freescale MPC85xx Cache-Sram UIO Platform Driver");
MODULE_ALIAS("platform:" DRIVER_NAME);
MODULE_LICENSE("GPL");
