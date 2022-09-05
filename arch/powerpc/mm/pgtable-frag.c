// SPDX-License-Identifier: GPL-2.0

/*
 *  Handling Page Tables through page fragments
 *
 */

#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/hardirq.h>
#include <linux/hugetlb.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>
#include <asm/tlb.h>

struct pt_frag {
	struct page *page;
	struct list_head list;
	int cpu;
	unsigned int nr_free;
	void *free_ptr;
	spinlock_t locks[];
};

struct pt_frag_alloc {
	/*
	 * The lock must disable bh because pte frags can be freed by RCU
	 * when it runs in softirq context.
	 */
	spinlock_t lock;
	size_t nr_free;
	struct list_head freelist;
	/* XXX: could make a remote freelist and only that needs locking,
	 * atomic nr_allocated and the first freer would be responsible
	 * for putting it on the correct queue
	 */
};

static DEFINE_PER_CPU(struct pt_frag_alloc, pte_frag_alloc);
static DEFINE_PER_CPU(struct pt_frag_alloc, pte_frag_alloc_kernel);
static DEFINE_PER_CPU(struct pt_frag_alloc, pmd_frag_alloc);

void pte_frag_destroy(void *pte_frag)
{
}

void pt_frag_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct pt_frag_alloc *alloc;

		alloc = per_cpu_ptr(&pte_frag_alloc, cpu);
		spin_lock_init(&alloc->lock);
		INIT_LIST_HEAD(&alloc->freelist);

		alloc = per_cpu_ptr(&pte_frag_alloc_kernel, cpu);
		spin_lock_init(&alloc->lock);
		INIT_LIST_HEAD(&alloc->freelist);

		alloc = per_cpu_ptr(&pmd_frag_alloc, cpu);
		spin_lock_init(&alloc->lock);
		INIT_LIST_HEAD(&alloc->freelist);
	}
}

static unsigned long pte_frag_idx(void *frag)
{
	return ((unsigned long)frag & (PAGE_SIZE - 1)) >> PTE_FRAG_SIZE_SHIFT;
}

static unsigned long pmd_frag_idx(void *frag)
{
	return ((unsigned long)frag & (PAGE_SIZE - 1)) >> PMD_FRAG_SIZE_SHIFT;
}

static void *get_pt_from_cache(struct mm_struct *mm, bool pte, bool kernel)
{
	struct pt_frag_alloc *alloc;

	if (pte) {
		if (kernel)
			alloc = get_cpu_ptr(&pte_frag_alloc_kernel);
		else
			alloc = get_cpu_ptr(&pte_frag_alloc);
	} else {
		alloc = get_cpu_ptr(&pmd_frag_alloc);
	}

	spin_lock_bh(&alloc->lock);
	if (!list_empty(&alloc->freelist)) {
		struct pt_frag *pt_frag = list_first_entry(&alloc->freelist,
							struct pt_frag, list);
		void *frag;

		frag = pt_frag->free_ptr;
		pt_frag->free_ptr = *((void **)frag);
		*((void **)frag) = NULL;

		pt_frag->nr_free--;
		if (pt_frag->nr_free == 0)
			list_del(&pt_frag->list);
		alloc->nr_free--;
		spin_unlock_bh(&alloc->lock);
		put_cpu_ptr(alloc);

		if (pte)
			spin_lock_init(&pt_frag->locks[pte_frag_idx(frag)]);
		else
			spin_lock_init(&pt_frag->locks[pmd_frag_idx(frag)]);

		return frag;
	}

	spin_unlock_bh(&alloc->lock);
	put_cpu_ptr(alloc);

	return NULL;
}

static void *__alloc_for_ptcache(struct mm_struct *mm, bool pte, bool kernel)
{
	size_t frag_size, frag_nr;
	struct pt_frag_alloc *alloc;
	void *frag;
	struct page *page;
	struct pt_frag *pt_frag;
	unsigned long i;

	if (pte) {
		frag_size = PTE_FRAG_SIZE;
		frag_nr = PTE_FRAG_NR;

		if (!kernel) {
			page = alloc_page(PGALLOC_GFP | __GFP_ACCOUNT);
			if (!page)
				return NULL;
			if (!pgtable_pte_page_ctor(page)) {
				__free_page(page);
				return NULL;
			}
		} else {
			page = alloc_page(PGALLOC_GFP);
			if (!page)
				return NULL;
		}

	} else {
		/* This is slightly different from PTE, for some reason */
		gfp_t gfp = GFP_KERNEL_ACCOUNT | __GFP_ZERO;

		frag_size = PMD_FRAG_SIZE;
		frag_nr = PMD_FRAG_NR;

		if (kernel)
			gfp &= ~__GFP_ACCOUNT;
		page = alloc_page(gfp);
		if (!page)
			return NULL;
		if (!pgtable_pmd_page_ctor(page)) {
			__free_page(page);
			return NULL;
		}
	}

	pt_frag = kmalloc(sizeof(struct pt_frag) + sizeof(spinlock_t) * frag_nr, GFP_KERNEL);
	if (!pt_frag) {
		if (!pte)
			pgtable_pmd_page_dtor(page);
		else if (!kernel)
			pgtable_pte_page_dtor(page);
		__free_page(page);
		return NULL;
	}

	pt_frag->page = page;
	pt_frag->nr_free = frag_nr - 1;

	frag = page_address(page);

	for (i = frag_size; i < PAGE_SIZE - frag_size; i += frag_size)
		*((void **)(frag + i)) = frag + i + frag_size;
	/* Last one will be NULL */

	pt_frag->free_ptr = frag + frag_size;

	page->pt_frag = pt_frag;

	if (pte) {
		if (kernel)
			alloc = get_cpu_ptr(&pte_frag_alloc_kernel);
		else
			alloc = get_cpu_ptr(&pte_frag_alloc);
	} else {
		alloc = get_cpu_ptr(&pmd_frag_alloc);
	}

	/* XXX: Confirm CPU (or at least node) here */

	pt_frag->cpu = smp_processor_id();

	spin_lock_bh(&alloc->lock);
	alloc->nr_free += frag_nr - 1;
	list_add_tail(&pt_frag->list, &alloc->freelist);
	spin_unlock_bh(&alloc->lock);

	put_cpu_ptr(alloc);

	spin_lock_init(&pt_frag->locks[0]);

	return frag;
}

static void *pt_fragment_alloc(struct mm_struct *mm, bool pte, bool kernel)
{
	void *pt;

	pt = get_pt_from_cache(mm, pte, kernel);
	if (unlikely(!pt))
		pt = __alloc_for_ptcache(mm, pte, kernel);
	return pt;
}

static void pt_fragment_free(void *frag, bool pte, bool kernel)
{
	struct pt_frag_alloc *alloc;
	struct page *page;
	struct pt_frag *pt_frag;
	size_t frag_nr;

	page = virt_to_page(frag);
	pt_frag = page->pt_frag;

	if (pte) {
		frag_nr = PTE_FRAG_NR;

		if (unlikely(PageReserved(page)))
			return free_reserved_page(page);

		if (kernel)
			alloc = per_cpu_ptr(&pte_frag_alloc_kernel, pt_frag->cpu);
		else
			alloc = per_cpu_ptr(&pte_frag_alloc, pt_frag->cpu);
	} else {
		frag_nr = PMD_FRAG_NR;

		alloc = per_cpu_ptr(&pmd_frag_alloc, pt_frag->cpu);
	}

	spin_lock_bh(&alloc->lock);

	if (pt_frag->nr_free == 0)
		list_add_tail(&pt_frag->list, &alloc->freelist);

	pt_frag->nr_free++;

	*((void **)frag) = pt_frag->free_ptr;
	pt_frag->free_ptr = frag;

	alloc->nr_free++;

	if (alloc->nr_free >= frag_nr * 2 && pt_frag->nr_free == frag_nr) {
		list_del(&pt_frag->list);
		alloc->nr_free -= frag_nr;
		spin_unlock_bh(&alloc->lock);
		if (!pte)
			pgtable_pmd_page_dtor(page);
		else if (!kernel)
			pgtable_pte_page_dtor(page);
		__free_page(page);
		kfree(pt_frag);
	} else {
		spin_unlock_bh(&alloc->lock);
	}
}

pte_t *pte_fragment_alloc(struct mm_struct *mm, int kernel)
{
	return pt_fragment_alloc(mm, true, !!kernel);
}

void pte_fragment_free(unsigned long *pte, int kernel)
{
	pt_fragment_free(pte, true, !!kernel);
}

pmd_t *pmd_fragment_alloc(struct mm_struct *mm, unsigned long vmaddr)
{
	bool kernel = (mm == &init_mm);

	return pt_fragment_alloc(mm, false, kernel);
}

void pmd_fragment_free(unsigned long *pmd)
{
	pt_fragment_free(pmd, false, false /* XXX? */);
}

spinlock_t *pte_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	struct page *page;
	struct pt_frag *pt_frag;
	void *frag;

	frag = (void *)pmd_page_vaddr(*pmd);
	page = virt_to_page(frag);
	pt_frag = page->pt_frag;

	return &pt_frag->locks[pte_frag_idx(frag)];
}

spinlock_t *pmd_lockptr(struct mm_struct *mm, pmd_t *pmd)
{
	struct page *page;
	struct pt_frag *pt_frag;
	void *frag;

	frag = (void *)pmd;
	page = pmd_to_page(pmd);
	pt_frag = page->pt_frag;

	return &pt_frag->locks[pmd_frag_idx(frag)];
}

bool ptlock_init(struct page *page)
{
	return true;
}
