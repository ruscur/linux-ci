// SPDX-License-Identifier: GPL-2.0
#include <asm/code-patching.h>
#include <linux/export.h>
#include <linux/kconfig.h>
#include <linux/kernel.h>
#include <linux/memory.h>
#include <linux/static_call.h>
#include <linux/syscalls.h>

static void* ppc_function_toc(u32 *func) {
	if (IS_ENABLED(CONFIG_PPC64_ELF_ABI_V2)) {
		u32 insn1 = *func;
		u32 insn2 = *(func+1);
		u64 si1 = sign_extend64((insn1 & OP_SI_MASK) << 16, 31);
		u64 si2 = sign_extend64(insn2 & OP_SI_MASK, 15);
		u64 addr = ((u64) func + si1) + si2;

		if ((((insn1 & OP_RT_RA_MASK) == ADDIS_R2_R12) ||
		     ((insn1 & OP_RT_RA_MASK) == LIS_R2)) &&
		    ((insn2 & OP_RT_RA_MASK) == ADDI_R2_R2))
			return (void*)addr;
	}

	return NULL;
}

static bool shares_toc(void *func1, void *func2) {
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

static unsigned long get_inst_addr(unsigned long tramp) {
	return tramp + (core_kernel_text(tramp) ? PPC_SCT_INST_KERNEL : PPC_SCT_INST_MODULE);
}

static unsigned long get_ret0_addr(unsigned long tramp) {
	return tramp + (core_kernel_text(tramp) ? PPC_SCT_RET0_KERNEL : PPC_SCT_RET0_MODULE);
}

static unsigned long get_data_addr(unsigned long tramp) {
	return tramp + (core_kernel_text(tramp) ? PPC_SCT_DATA_KERNEL : PPC_SCT_DATA_MODULE);
}

void arch_static_call_transform(void *site, void *tramp, void *func, bool tail)
{
	int err;
	bool is_ret0 = (func == __static_call_return0);
	unsigned long target = is_ret0
		? get_ret0_addr((unsigned long)tramp)
		: (unsigned long)func;
	bool is_short;
	void* tramp_inst;

	if (!tramp)
		return;

	tramp_inst = (void*)get_inst_addr((unsigned long)tramp);

	if (is_ret0) {
		is_short = true;
	} else if (shares_toc(tramp, (void*)target)) {
		/* Confirm that the local entry point is in range */
		is_short = is_offset_in_branch_range(
			(long)ppc_function_entry((void*)target) - (long)tramp_inst);
	} else {
		/* Combine out-of-range with not sharing a TOC. Though it's possible an
		 * out-of-range target shares a TOC, handling this separately complicates
		 * the trampoline. It's simpler to always use the global entry point
		 * in this case.
		 */
		is_short = false;
	}

	mutex_lock(&text_mutex);

	if (func && !is_short) {
		err = patch_text_data(
			(void*)get_data_addr((unsigned long)tramp), &target, sizeof(target));
		if (err)
			goto out;
	}

	if (!func)
		err = patch_instruction(tramp_inst, ppc_inst(PPC_RAW_BLR()));
	else if (is_short)
		err = patch_branch(tramp_inst, ppc_function_entry((void*)target), 0);
	else
		err = patch_instruction(tramp_inst, ppc_inst(PPC_RAW_NOP()));

out:
	mutex_unlock(&text_mutex);

	if (err)
		panic("%s: patching failed %pS at %pS\n", __func__, func, tramp);
}
EXPORT_SYMBOL_GPL(arch_static_call_transform);


#if IS_MODULE(CONFIG_PPC_STATIC_CALL_KUNIT_TEST)

#include <kunit/test.h>

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

#define restore_toc(test) \
	actual_toc = current_toc; \
	current_toc = kernel_toc

#define check_toc(test) KUNIT_EXPECT_PTR_EQ(test, kernel_toc, actual_toc)

#define toc_fixup(test) \
	restore_toc(); \
	check_toc(test)

#define PROTECTED_SC(test, call) \
({ \
	long ret; \
	ret = call; \
	toc_fixup(test); \
	ret; \
})

void ppc_sc_kernel_toc_init(void)
{
	static_kernel_toc = kernel_toc;
	static_actual_toc = actual_toc;  /* save so we can restore when the tests finish */

	kernel_toc = current_toc;
}

void ppc_sc_kernel_toc_exit(void)
{
	kernel_toc = static_kernel_toc;
	actual_toc = static_actual_toc;
}

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
	return PROTECTED_SC(test, static_call(ppc_sc_kernel)(test));
}

int ppc_sc_kernel_call_indirect(struct kunit* test, int (*fn)(struct kunit*))
{
	return PROTECTED_SC(test, fn(test));
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

EXPORT_SYMBOL_GPL(ppc_sc_kernel_toc_init);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_toc_exit);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_1);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_2);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_target_big);
EXPORT_STATIC_CALL_GPL(ppc_sc_kernel);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_call);
EXPORT_SYMBOL_GPL(ppc_sc_kernel_call_indirect);

#endif /* IS_MODULE(CONFIG_PPC_STATIC_CALL_KUNIT_TEST) */
