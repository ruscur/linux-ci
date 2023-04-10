// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on arch/arm/mm/flush.c
 *
 * Copyright (C) 1995-2002 Russell King
 * Copyright (C) 2012 ARM Ltd.
 */

#include <linux/export.h>
#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sysctl.h>

#include <asm/cacheflush.h>
#include <asm/cache.h>
#include <asm/tlbflush.h>

void sync_icache_aliases(unsigned long start, unsigned long end)
{
	if (icache_is_aliasing()) {
		dcache_clean_pou(start, end);
		icache_inval_all_pou();
	} else {
		/*
		 * Don't issue kick_all_cpus_sync() after I-cache invalidation
		 * for user mappings.
		 */
		caches_clean_inval_pou(start, end);
	}
}

static void flush_ptrace_access(struct vm_area_struct *vma, unsigned long start,
				unsigned long end)
{
	if (vma->vm_flags & VM_EXEC)
		sync_icache_aliases(start, end);
}

/*
 * Copy user data from/to a page which is mapped into a different processes
 * address space.  Really, we want to allow our "user space" model to handle
 * this.
 */
void copy_to_user_page(struct vm_area_struct *vma, struct page *page,
		       unsigned long uaddr, void *dst, const void *src,
		       unsigned long len)
{
	memcpy(dst, src, len);
	flush_ptrace_access(vma, (unsigned long)dst, (unsigned long)dst + len);
}

void __sync_icache_dcache(pte_t pte)
{
	struct page *page = pte_page(pte);

	/*
	 * HugeTLB pages are always fully mapped, so only setting head page's
	 * PG_dcache_clean flag is enough.
	 */
	if (PageHuge(page))
		page = compound_head(page);

	if (!test_bit(PG_dcache_clean, &page->flags)) {
		sync_icache_aliases((unsigned long)page_address(page),
				    (unsigned long)page_address(page) +
					    page_size(page));
		set_bit(PG_dcache_clean, &page->flags);
	}
}
EXPORT_SYMBOL_GPL(__sync_icache_dcache);

/*
 * This function is called when a page has been modified by the kernel. Mark
 * it as dirty for later flushing when mapped in user space (if executable,
 * see __sync_icache_dcache).
 */
void flush_dcache_page(struct page *page)
{
	/*
	 * HugeTLB pages are always fully mapped and only head page will be
	 * set PG_dcache_clean (see comments in __sync_icache_dcache()).
	 */
	if (PageHuge(page))
		page = compound_head(page);

	if (test_bit(PG_dcache_clean, &page->flags))
		clear_bit(PG_dcache_clean, &page->flags);
}
EXPORT_SYMBOL(flush_dcache_page);

/*
 * Additional functions defined in assembly.
 */
EXPORT_SYMBOL(caches_clean_inval_pou);

#ifdef CONFIG_ARCH_HAS_PMEM_API
void arch_wb_cache_pmem(void *addr, size_t size)
{
	/* Ensure order against any prior non-cacheable writes */
	dmb(osh);
	dcache_clean_pop((unsigned long)addr, (unsigned long)addr + size);
}
EXPORT_SYMBOL_GPL(arch_wb_cache_pmem);

void arch_invalidate_pmem(void *addr, size_t size)
{
	dcache_inval_poc((unsigned long)addr, (unsigned long)addr + size);
}
EXPORT_SYMBOL_GPL(arch_invalidate_pmem);
#endif

#ifdef CONFIG_ARCH_WANT_BATCHED_UNMAP_TLB_FLUSH

DEFINE_STATIC_KEY_FALSE(batched_tlb_enabled);

static bool batched_tlb_flush_supported(void)
{
#ifdef CONFIG_ARM64_WORKAROUND_REPEAT_TLBI
	/*
	 * TLB flush deferral is not required on systems, which are affected with
	 * ARM64_WORKAROUND_REPEAT_TLBI, as __tlbi()/__tlbi_user() implementation
	 * will have two consecutive TLBI instructions with a dsb(ish) in between
	 * defeating the purpose (i.e save overall 'dsb ish' cost).
	 */
	if (unlikely(cpus_have_const_cap(ARM64_WORKAROUND_REPEAT_TLBI)))
		return false;
#endif
	return true;
}

int batched_tlb_enabled_handler(struct ctl_table *table, int write,
				      void *buffer, size_t *lenp, loff_t *ppos)
{
	unsigned int enabled = static_branch_unlikely(&batched_tlb_enabled);
	struct ctl_table t;
	int err;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	t = *table;
	t.data = &enabled;
	err = proc_dointvec_minmax(&t, write, buffer, lenp, ppos);
	if (!err && write) {
		if (enabled && batched_tlb_flush_supported())
			static_branch_enable(&batched_tlb_enabled);
		else
			static_branch_disable(&batched_tlb_enabled);
	}

	return err;
}

static struct ctl_table batched_tlb_sysctls[] = {
	{
		.procname	= "batched_tlb_enabled",
		.data		= NULL,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= batched_tlb_enabled_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{}
};

static int __init batched_tlb_sysctls_init(void)
{
	if (batched_tlb_flush_supported())
		static_branch_enable(&batched_tlb_enabled);

	register_sysctl_init("vm", batched_tlb_sysctls);
	return 0;
}
late_initcall(batched_tlb_sysctls_init);

#endif
