// SPDX-License-Identifier: GPL-2.0-only
/*
 * KVM selftest powerpc library code - CPU-related functions (page tables...)
 */

#include "processor.h"
#include "kvm_util.h"
#include "kvm_util_base.h"
#include "hcall.h"

#define RTS ((0x2UL << 61) | (0x5UL << 5)) // 52-bits
#define RADIX_PGD_INDEX_SIZE 13

void virt_arch_pgd_alloc(struct kvm_vm *vm)
{
	struct kvm_ppc_mmuv3_cfg mmu_cfg;
	vm_paddr_t prtb, pgtb;
	uint64_t *proc_table, *page_table;
	size_t pgd_pages;

	TEST_ASSERT(vm->mode == VM_MODE_P52V52_64K, "Attempt to use "
		"unknown or unsupported guest mode, mode: 0x%x", vm->mode);

	/* If needed, create page table */
	if (vm->pgd_created)
		return;

	prtb = vm_phy_page_alloc(vm, KVM_GUEST_PAGE_TABLE_MIN_PADDR,
				 vm->memslots[MEM_REGION_PT]);
	proc_table = addr_gpa2hva(vm, prtb);
	memset(proc_table, 0, vm->page_size);
	vm->prtb = prtb;

	pgd_pages = 1UL << ((RADIX_PGD_INDEX_SIZE + 3) >> vm->page_shift);
	TEST_ASSERT(pgd_pages == 1, "PGD allocation must be single page");
	pgtb = vm_phy_page_alloc(vm, KVM_GUEST_PAGE_TABLE_MIN_PADDR,
				 vm->memslots[MEM_REGION_PT]);
	page_table = addr_gpa2hva(vm, pgtb);
	memset(page_table, 0, vm->page_size * pgd_pages);
	vm->pgd = pgtb;

	/* Set the base page directory in the proc table */
	proc_table[0] = cpu_to_be64(pgtb | RTS | RADIX_PGD_INDEX_SIZE);

	mmu_cfg.process_table = prtb | 0x8000000000000000UL | 0x4; // 64K size
	mmu_cfg.flags = KVM_PPC_MMUV3_RADIX | KVM_PPC_MMUV3_GTSE;

	vm_ioctl(vm, KVM_PPC_CONFIGURE_V3_MMU, &mmu_cfg);

	vm->pgd_created = true;
}

static int pt_shift(struct kvm_vm *vm, int level)
{
	switch (level) {
	case 1:
		return 13;
	case 2:
	case 3:
		return 9;
	case 4:
		return 5;
	default:
		TEST_ASSERT(false, "Invalid page table level %d\n", level);
		return 0;
	}
}

static uint64_t pt_entry_coverage(struct kvm_vm *vm, int level)
{
	uint64_t size = vm->page_size;

	if (level == 4)
		return size;
	size <<= pt_shift(vm, 4);
	if (level == 3)
		return size;
	size <<= pt_shift(vm, 3);
	if (level == 2)
		return size;
	size <<= pt_shift(vm, 2);
	return size;
}

static int pt_idx(struct kvm_vm *vm, uint64_t vaddr, int level, uint64_t *nls)
{
	switch (level) {
	case 1:
		*nls = 0x9;
		return (vaddr >> 39) & 0x1fff;
	case 2:
		*nls = 0x9;
		return (vaddr >> 30) & 0x1ff;
	case 3:
// 4K		*nls = 0x9;
		*nls = 0x5;
		return (vaddr >> 21) & 0x1ff;
	case 4:
// 4K		return (vaddr >> 12) & 0x1ff;
		return (vaddr >> 16) & 0x1f;
	default:
		TEST_ASSERT(false, "Invalid page table level %d\n", level);
		return 0;
	}
}

static uint64_t *virt_get_pte(struct kvm_vm *vm, vm_paddr_t pt,
			  uint64_t vaddr, int level, uint64_t *nls)
{
	uint64_t *page_table = addr_gpa2hva(vm, pt);
	int idx = pt_idx(vm, vaddr, level, nls);

	return &page_table[idx];
}

#define PTE_VALID	0x8000000000000000ull
#define PTE_LEAF	0x4000000000000000ull
#define PTE_REFERENCED	0x0000000000000100ull
#define PTE_CHANGED	0x0000000000000080ull
#define PTE_PRIV	0x0000000000000008ull
#define PTE_READ	0x0000000000000004ull
#define PTE_RW		0x0000000000000002ull
#define PTE_EXEC	0x0000000000000001ull
#define PTE_PAGE_MASK	0x01fffffffffff000ull

#define PDE_VALID	PTE_VALID
#define PDE_NLS		0x0000000000000011ull
#define PDE_PT_MASK	0x0fffffffffffff00ull

void virt_arch_pg_map(struct kvm_vm *vm, uint64_t gva, uint64_t gpa)
{
	vm_paddr_t pt = vm->pgd;
	uint64_t *ptep, pte;
	int level;

	for (level = 1; level <= 3; level++) {
		uint64_t nls;
		uint64_t *pdep = virt_get_pte(vm, pt, gva, level, &nls);
		uint64_t pde = be64_to_cpu(*pdep);
		uint64_t *page_table;

		if (pde) {
			TEST_ASSERT((pde & PDE_VALID) && !(pde & PTE_LEAF),
				"Invalid PDE at level: %u gva: 0x%lx pde:0x%lx\n",
				level, gva, pde);
			pt = pde & PDE_PT_MASK;
			continue;
		}

		// XXX: 64K geometry does not require full pages!
		pt = vm_phy_page_alloc(vm,
				       KVM_GUEST_PAGE_TABLE_MIN_PADDR,
				       vm->memslots[MEM_REGION_PT]);
		page_table = addr_gpa2hva(vm, pt);
		memset(page_table, 0, vm->page_size);
		pde = PDE_VALID | nls | pt;
		*pdep = cpu_to_be64(pde);
	}

	ptep = virt_get_pte(vm, pt, gva, level, NULL);
	pte = be64_to_cpu(*ptep);

	TEST_ASSERT(!pte,
		"PTE already present at level: %u gva: 0x%lx pte:0x%lx\n",
		level, gva, pte);

	pte = PTE_VALID | PTE_LEAF | PTE_REFERENCED | PTE_CHANGED | PTE_PRIV | PTE_READ | PTE_RW | PTE_EXEC | (gpa & PTE_PAGE_MASK);
	*ptep = cpu_to_be64(pte);
}

vm_paddr_t addr_arch_gva2gpa(struct kvm_vm *vm, vm_vaddr_t gva)
{
	vm_paddr_t pt = vm->pgd;
	uint64_t *ptep, pte;
	int level;

	for (level = 1; level <= 3; level++) {
		uint64_t nls;
		uint64_t *pdep = virt_get_pte(vm, pt, gva, level, &nls);
		uint64_t pde = be64_to_cpu(*pdep);

		TEST_ASSERT((pde & PDE_VALID) && !(pde & PTE_LEAF),
			"PDE not present at level: %u gva: 0x%lx pde:0x%lx\n",
			level, gva, pde);
		pt = pde & PDE_PT_MASK;
	}

	ptep = virt_get_pte(vm, pt, gva, level, NULL);
	pte = be64_to_cpu(*ptep);

	TEST_ASSERT(pte,
		"PTE not present at level: %u gva: 0x%lx pte:0x%lx\n",
		level, gva, pte);

	TEST_ASSERT((pte & PTE_VALID) && (pte & PTE_LEAF) && (pte & PTE_READ) && (pte & PTE_RW) && (pte & PTE_EXEC),
		"PTE not valid at level: %u gva: 0x%lx pte:0x%lx\n",
		level, gva, pte);

	return (pte & PTE_PAGE_MASK) + (gva & (vm->page_size - 1));
}

static void virt_arch_dump_pt(FILE *stream, struct kvm_vm *vm, vm_paddr_t pt, vm_vaddr_t va, int level, uint8_t indent)
{
	uint64_t *page_table;
	int size, idx;

	page_table = addr_gpa2hva(vm, pt);
	size = 1U << pt_shift(vm, level);
	for (idx = 0; idx < size; idx++) {
		uint64_t pte = be64_to_cpu(page_table[idx]);
		if (pte & PTE_VALID) {
			if (pte & PTE_LEAF) {
				fprintf(stream, "%*sgVA:0x%016lx -> gRA:0x%016llx\n", indent, "", va, pte & PTE_PAGE_MASK);
			} else {
				virt_arch_dump_pt(stream, vm, pte & PDE_PT_MASK, va, level + 1, indent);
			}
		}
		va += pt_entry_coverage(vm, level);
	}

}

void virt_arch_dump(FILE *stream, struct kvm_vm *vm, uint8_t indent)
{
	vm_paddr_t pt = vm->pgd;

	if (!vm->pgd_created)
		return;

	virt_arch_dump_pt(stream, vm, pt, 0, 1, indent);
}

struct kvm_vcpu *vm_arch_vcpu_add(struct kvm_vm *vm, uint32_t vcpu_id,
				  void *guest_code)
{
	size_t stack_size =  64*1024;
	uint64_t stack_vaddr;
	struct kvm_regs regs;
	struct kvm_vcpu *vcpu;
	uint64_t lpcr;

	TEST_ASSERT(vm->page_size == 64*1024, "Unsupported page size: 0x%x",
		    vm->page_size);

	stack_vaddr = __vm_vaddr_alloc(vm, stack_size,
				       DEFAULT_GUEST_STACK_VADDR_MIN,
				       MEM_REGION_DATA);

	vcpu = __vm_vcpu_add(vm, vcpu_id);

	vcpu_enable_cap(vcpu, KVM_CAP_PPC_PAPR, 1);

	/* Setup guest registers */
	vcpu_regs_get(vcpu, &regs);
	vcpu_get_reg(vcpu, KVM_REG_PPC_LPCR_64, &lpcr);

	regs.pc = (uintptr_t)guest_code;
	regs.gpr[12] = (uintptr_t)guest_code;
	regs.msr = 0x8000000002103032ull;
	regs.gpr[1] = stack_vaddr + stack_size - 256;

	if (BYTE_ORDER == LITTLE_ENDIAN) {
		regs.msr |= 0x1; // LE
		lpcr |= 0x0000000002000000; // ILE
	} else {
		lpcr &= ~0x0000000002000000; // !ILE
	}

	vcpu_regs_set(vcpu, &regs);
	vcpu_set_reg(vcpu, KVM_REG_PPC_LPCR_64, lpcr);

	return vcpu;
}

void vcpu_args_set(struct kvm_vcpu *vcpu, unsigned int num, ...)
{
	va_list ap;
	struct kvm_regs regs;
	int i;

	TEST_ASSERT(num >= 1 && num <= 5, "Unsupported number of args,\n"
		    "  num: %u\n",
		    num);

	va_start(ap, num);
	vcpu_regs_get(vcpu, &regs);

	for (i = 0; i < num; i++)
		regs.gpr[i + 3] = va_arg(ap, uint64_t);

	vcpu_regs_set(vcpu, &regs);
	va_end(ap);
}

void vcpu_arch_dump(FILE *stream, struct kvm_vcpu *vcpu, uint8_t indent)
{
	struct kvm_regs regs;

	vcpu_regs_get(vcpu, &regs);

	fprintf(stream, "%*sNIA: 0x%016llx  MSR: 0x%016llx\n", indent, "", regs.pc, regs.msr);
	fprintf(stream, "%*sLR:  0x%016llx  CTR :0x%016llx\n", indent, "", regs.lr, regs.ctr);
	fprintf(stream, "%*sCR:  0x%08llx          XER :0x%016llx\n", indent, "", regs.cr, regs.xer);
}

void kvm_arch_vm_post_create(struct kvm_vm *vm)
{
	uint32_t stub[] = {
		0x38600000,		     // li	r3,0
		0x38800000 | UCALL_R4_EXCPT, // li	r4,UCALL_R4_EXCPT
		0x38a00000,		     // li	r5,0
		0x7cda02a6,		     // mfspr	r5,SRR0
		0x7cfb02a6,		     // mfspr	r6,SRR1
		0x44000022,		     // sc	1
	};
	void *mem;
	int i;

	vm_paddr_t excp_paddr = vm_phy_page_alloc(vm, 0,
				 vm->memslots[MEM_REGION_DATA]);
	TEST_ASSERT(excp_paddr == 0, "excp_paddr = 0x%lx\n", excp_paddr);

	mem = addr_gpa2hva(vm, excp_paddr);

	/* Fill with branch-to-self so SRR0/1 don't get lost */
	/* XXX: this requires 2 pages on 4K */
	for (i = 0x100; i < 0x2000; i += 0x20) {
		stub[2] = 0x38a00000 | i;	// li	r5,i
		memcpy(mem + i, stub, sizeof(stub));
	}
}

bool (*interrupt_handler)(struct kvm_vcpu *vcpu, unsigned trap);

void assert_on_unhandled_exception(struct kvm_vcpu *vcpu)
{
	struct kvm_run *run = vcpu->run;
	struct kvm_regs regs;

	if (!(run->exit_reason == KVM_EXIT_PAPR_HCALL &&
	    run->papr_hcall.nr == H_UCALL)) {
		return;
	}
	vcpu_regs_get(vcpu, &regs);
	if (regs.gpr[4] != UCALL_R4_EXCPT)
		return;

	if (interrupt_handler) {
		if (interrupt_handler(vcpu, regs.gpr[5]))
			return; // handled
	}

	TEST_FAIL("Unhandled exception 0x%llx at NIA:0x%016llx MSR:0x%016llx\n",
			regs.gpr[5], regs.gpr[6], regs.gpr[7]);
}
