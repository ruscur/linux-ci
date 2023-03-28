// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenRISC Linux
 *
 * Linux architectural port borrowing liberally from similar works of
 * others.  All original copyrights apply as per the original source
 * declaration.
 *
 * Modifications for the OpenRISC architecture:
 * Copyright (C) 2003 Matjaz Breskvar <phoenix@bsemi.com>
 * Copyright (C) 2010-2011 Jonas Bonn <jonas@southpole.se>
 *
 * DMA mapping callbacks...
 */

#include <linux/dma-map-ops.h>
#include <linux/pagewalk.h>

#include <asm/cpuinfo.h>
#include <asm/spr_defs.h>
#include <asm/tlbflush.h>

static int
page_set_nocache(pte_t *pte, unsigned long addr,
		 unsigned long next, struct mm_walk *walk)
{
	unsigned long cl;
	struct cpuinfo_or1k *cpuinfo = &cpuinfo_or1k[smp_processor_id()];

	pte_val(*pte) |= _PAGE_CI;

	/*
	 * Flush the page out of the TLB so that the new page flags get
	 * picked up next time there's an access
	 */
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE);

	/* Flush page out of dcache */
	for (cl = __pa(addr); cl < __pa(next); cl += cpuinfo->dcache_block_size)
		mtspr(SPR_DCBFR, cl);

	return 0;
}

static const struct mm_walk_ops set_nocache_walk_ops = {
	.pte_entry		= page_set_nocache,
};

static int
page_clear_nocache(pte_t *pte, unsigned long addr,
		   unsigned long next, struct mm_walk *walk)
{
	pte_val(*pte) &= ~_PAGE_CI;

	/*
	 * Flush the page out of the TLB so that the new page flags get
	 * picked up next time there's an access
	 */
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE);

	return 0;
}

static const struct mm_walk_ops clear_nocache_walk_ops = {
	.pte_entry		= page_clear_nocache,
};

void *arch_dma_set_uncached(void *cpu_addr, size_t size)
{
	unsigned long va = (unsigned long)cpu_addr;
	int error;

	/*
	 * We need to iterate through the pages, clearing the dcache for
	 * them and setting the cache-inhibit bit.
	 */
	mmap_write_lock(&init_mm);
	error = walk_page_range_novma(&init_mm, va, va + size,
			&set_nocache_walk_ops, NULL, NULL);
	mmap_write_unlock(&init_mm);

	if (error)
		return ERR_PTR(error);
	return cpu_addr;
}

void arch_dma_clear_uncached(void *cpu_addr, size_t size)
{
	unsigned long va = (unsigned long)cpu_addr;

	mmap_write_lock(&init_mm);
	/* walk_page_range shouldn't be able to fail here */
	WARN_ON(walk_page_range_novma(&init_mm, va, va + size,
			&clear_nocache_walk_ops, NULL, NULL));
	mmap_write_unlock(&init_mm);
}

static inline void arch_dma_cache_wback(phys_addr_t paddr, size_t size)
{
	unsigned long cl;
	struct cpuinfo_or1k *cpuinfo = &cpuinfo_or1k[smp_processor_id()];

	/* Write back the dcache for the requested range */
	for (cl = paddr; cl < paddr + size;
	     cl += cpuinfo->dcache_block_size)
		mtspr(SPR_DCBWR, cl);
}

static inline void arch_dma_cache_inv(phys_addr_t paddr, size_t size)
{
	unsigned long cl;
	struct cpuinfo_or1k *cpuinfo = &cpuinfo_or1k[smp_processor_id()];

	/* Invalidate the dcache for the requested range */
	for (cl = paddr; cl < paddr + size;
	     cl += cpuinfo->dcache_block_size)
		mtspr(SPR_DCBIR, cl);
}

static inline void arch_dma_cache_wback_inv(phys_addr_t paddr, size_t size)
{
	unsigned long cl;
	struct cpuinfo_or1k *cpuinfo = &cpuinfo_or1k[smp_processor_id()];

	/* Flush the dcache for the requested range */
	for (cl = paddr; cl < paddr + size;
	     cl += cpuinfo->dcache_block_size)
		mtspr(SPR_DCBFR, cl);
}

static inline bool arch_sync_dma_clean_before_fromdevice(void)
{
	return false;
}

static inline bool arch_sync_dma_cpu_needs_post_dma_flush(void)
{
	return false;
}

#include <linux/dma-sync.h>
