// SPDX-License-Identifier: GPL-2.0
#include <asm/code-patching.h>
#include <linux/export.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/memory.h>
#include <linux/static_call.h>
#include <linux/syscalls.h>

#if IS_MODULE(CONFIG_PPC_STATIC_CALL_KUNIT_TEST)
#include <kunit/test.h>
#endif

#ifdef CONFIG_PPC64_ELF_ABI_V2

static void* ppc_function_toc(u32 *func) {
	u32 insn1 = *func;
	u32 insn2 = *(func+1);
	u64 si1 = sign_extend64((insn1 & OP_SI_MASK) << 16, 31);
	u64 si2 = sign_extend64(insn2 & OP_SI_MASK, 15);
	u64 addr = ((u64) func + si1) + si2;

	if ((((insn1 & OP_RT_RA_MASK) == ADDIS_R2_R12) ||
	     ((insn1 & OP_RT_RA_MASK) == LIS_R2)) &&
	    ((insn2 & OP_RT_RA_MASK) == ADDI_R2_R2))
		return (void*) addr;
	else
		return NULL;
}

static bool shares_toc(void *func1, void *func2) {
	void* func1_toc;
	void* func2_toc;

	if (func1 == NULL || func2 == NULL)
		return false;

	/* Assume the kernel only uses a single TOC */
	if (core_kernel_text((unsigned long)func1) &&
	    core_kernel_text((unsigned long)func2))
		return true;

	/* Fall back to calculating the TOC from common patterns
	 * if modules are involved
	 */
	func1_toc = ppc_function_toc(func1);
	func2_toc = ppc_function_toc(func2);
	return func1_toc != NULL && func2_toc != NULL && (func1_toc == func2_toc);
}

#endif /* CONFIG_PPC64_ELF_ABI_V2 */

void arch_static_call_transform(void *site, void *tramp, void *func, bool tail)
{
	int err;
	bool is_ret0 = (func == __static_call_return0);
	unsigned long target = (unsigned long)(is_ret0 ? tramp + PPC_SCT_RET0 : func);
	bool is_short;
	void* tramp_entry;

	if (!tramp)
		return;

	tramp_entry = (void*)ppc_function_entry(tramp);

#ifdef CONFIG_PPC64_ELF_ABI_V2
	if (shares_toc(tramp, (void*)target)) {
		/* Confirm that the local entry point is in range */
		is_short = is_offset_in_branch_range(
			(long)ppc_function_entry((void*)target) - (long)tramp_entry);
	} else {
		/* Combine out-of-range with not sharing a TOC. Though it's possible an
		 * out-of-range target shares a TOC, handling this separately complicates
		 * the trampoline. It's simpler to always use the global entry point
		 * in this case.
		 */
		is_short = false;
	}
#else /* !CONFIG_PPC64_ELF_ABI_V2 */
	is_short = is_offset_in_branch_range((long)target - (long)tramp);
#endif /* CONFIG_PPC64_ELF_ABI_V2 */

	mutex_lock(&text_mutex);

	if (func && !is_short) {
		/* This assumes that the update is atomic. The current implementation uses
		 * stw/std and the store location is aligned. A sync is issued by one of the
		 * patch_instruction/patch_branch functions below.
		 */
		err = PTR_ERR_OR_ZERO(patch_memory(
			tramp + PPC_SCT_DATA, &target, sizeof(target)));
		if (err)
			goto out;
	}

	if (!func)
		err = patch_instruction(tramp_entry, ppc_inst(PPC_RAW_BLR()));
	else if (is_short)
		err = patch_branch(tramp_entry, ppc_function_entry((void*) target), 0);
	else
		err = patch_instruction(tramp_entry, ppc_inst(PPC_RAW_NOP()));

out:
	mutex_unlock(&text_mutex);

	if (err)
		panic("%s: patching failed %pS at %pS\n", __func__, func, tramp);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);


#if IS_MODULE(CONFIG_PPC_STATIC_CALL_KUNIT_TEST)

/* The following are some kernel hooks for testing the static call
 * implementation from the static_call_test module. The bulk of the
 * assertions are run in that module, except for the TOC checks that
 * must be done in the core kernel context.
 */

/* Reserve these registers for testing (same registers as in static_call_test.c) */
register void* current_toc asm ("r2");
register void* module_toc asm ("r14");
register void* actual_module_toc asm ("r15");
register void* kernel_toc asm ("r16");
register void* actual_toc asm ("r17");

static void* static_kernel_toc;
static void* static_actual_toc;

#define fixup_toc(test) \
	actual_toc = current_toc; \
	current_toc = kernel_toc; \
	KUNIT_EXPECT_PTR_EQ(test, kernel_toc, actual_toc)

void ppc_sc_kernel_toc_init(void)
{
	static_kernel_toc = kernel_toc;
	static_actual_toc = actual_toc;

	kernel_toc = current_toc;
}

void ppc_sc_kernel_toc_exit(void)
{
	kernel_toc = static_kernel_toc;
	actual_toc = static_actual_toc;
}

int ppc_sc_kernel_target_1(struct kunit* test)
{
	fixup_toc(test);
	return 1;
}

int ppc_sc_kernel_target_2(struct kunit* test)
{
	fixup_toc(test);
	return 2;
}

DEFINE_STATIC_CALL(ppc_sc_kernel, ppc_sc_kernel_target_1);

int ppc_sc_kernel_call(struct kunit* test)
{
	int ret = static_call(ppc_sc_kernel)(test);
	fixup_toc(test);
	return ret;
}

int ppc_sc_kernel_call_indirect(struct kunit* test, int (*fn)(struct kunit*))
{
	int ret = fn(test);
	fixup_toc(test);
	return ret;
}

long ppc_sc_kernel_target_big(struct kunit* test,
			      long a,
			      long b,
			      long c,
			      long d,
			      long e,
			      long f,
			      long g,
			      long h,
			      long i)
{
	fixup_toc(test);
	KUNIT_EXPECT_EQ(test, a, b);
	KUNIT_EXPECT_EQ(test, a, c);
	KUNIT_EXPECT_EQ(test, a, d);
	KUNIT_EXPECT_EQ(test, a, e);
	KUNIT_EXPECT_EQ(test, a, f);
	KUNIT_EXPECT_EQ(test, a, g);
	KUNIT_EXPECT_EQ(test, a, h);
	KUNIT_EXPECT_EQ(test, a, i);
	return ~a;
}

EXPORT_SYMBOL_GPL(ppc_sc_kernel_toc_init);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_toc_exit);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_1);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_2);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_big);
EXPORT_STATIC_CALL_GPL(ppc_sc_kernel);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_call);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_call_indirect);

#endif /* IS_MODULE(CONFIG_PPC_STATIC_CALL_KUNIT_TEST) */
