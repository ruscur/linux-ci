// SPDX-License-Identifier: GPL-2.0
#include <linux/memory.h>
#include <linux/static_call.h>

#include <asm/code-patching.h>

static void* ppc_function_toc(u32 *func)
{
#ifdef CONFIG_PPC64_ELF_ABI_V2
	u32 insn1 = *func;
	u32 insn2 = *(func+1);
	u64 si1 = sign_extend64((insn1 & OP_SI_MASK) << 16, 31);
	u64 si2 = sign_extend64(insn2 & OP_SI_MASK, 15);
	u64 addr = ((u64) func + si1) + si2;

	if ((((insn1 & OP_RT_RA_MASK) == ADDIS_R2_R12) ||
	     ((insn1 & OP_RT_RA_MASK) == LIS_R2)) &&
	    ((insn2 & OP_RT_RA_MASK) == ADDI_R2_R2))
		return (void*)addr;
#endif
	return NULL;
}

static bool shares_toc(void *func1, void *func2)
{
	if (IS_ENABLED(CONFIG_PPC64_ELF_ABI_V2)) {
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

	return true;
}

static void* get_inst_addr(void *tramp)
{
	return tramp + (core_kernel_text((unsigned long)tramp)
				? PPC_SCT_INST_KERNEL
				: PPC_SCT_INST_MODULE);
}

static void* get_ret0_addr(void* tramp)
{
	return tramp + (core_kernel_text((unsigned long)tramp)
				? PPC_SCT_RET0_KERNEL
				: PPC_SCT_RET0_MODULE);
}

static void* get_data_addr(void *tramp)
{
	return tramp + (core_kernel_text((unsigned long) tramp)
				? PPC_SCT_DATA_KERNEL
				: PPC_SCT_DATA_MODULE);
}

void arch_static_call_transform(void *site, void *tramp, void *func, bool tail)
{
	int err;
	bool is_ret0 = (func == __static_call_return0);
	bool is_short;
	void* target = is_ret0 ? get_ret0_addr(tramp) : func;
	void* tramp_inst = get_inst_addr(tramp);

	if (!tramp)
		return;

	if (is_ret0)
		is_short = true;
	else if (shares_toc(tramp, target))
		is_short = is_offset_in_branch_range(
			(long)ppc_function_entry(target) - (long)tramp_inst);
	else
		/* Combine out-of-range with not sharing a TOC. Though it's possible an
		 * out-of-range target shares a TOC, handling this separately complicates
		 * the trampoline. It's simpler to always use the global entry point
		 * in this case.
		 */
		is_short = false;

	mutex_lock(&text_mutex);

	if (func && !is_short) {
		err = patch_memory(get_data_addr(tramp), target);
		if (err)
			goto out;
	}

	if (!func)
		err = patch_instruction(tramp_inst, ppc_inst(PPC_RAW_BLR()));
	else if (is_short)
		err = patch_branch(tramp_inst, ppc_function_entry(target), 0);
	else
		err = patch_instruction(tramp_inst, ppc_inst(PPC_RAW_NOP()));

out:
	mutex_unlock(&text_mutex);

	if (err)
		panic("%s: patching failed %pS at %pS\n", __func__, func, tramp);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);


#if IS_MODULE(CONFIG_PPC_STATIC_CALL_KUNIT_TEST)

#include "static_call_test.h"

int ppc_sc_kernel_target_1(struct kunit* test)
{
	toc_fixup(test);
	return 1;
}

int ppc_sc_kernel_target_2(struct kunit* test)
{
	toc_fixup(test);
	return 2;
}

DEFINE_STATIC_CALL(ppc_sc_kernel, ppc_sc_kernel_target_1);

int ppc_sc_kernel_call(struct kunit* test)
{
	return PROTECTED_SC(test, int, static_call(ppc_sc_kernel)(test));
}

int ppc_sc_kernel_call_indirect(struct kunit* test, int (*fn)(struct kunit*))
{
	return PROTECTED_SC(test, int, fn(test));
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
	toc_fixup(test);
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

EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_1);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_2);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_big);
EXPORT_STATIC_CALL_GPL(ppc_sc_kernel);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_call);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_call_indirect);

#endif /* IS_MODULE(CONFIG_PPC_STATIC_CALL_KUNIT_TEST) */
