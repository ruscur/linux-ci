// SPDX-License-Identifier: GPL-2.0

#include "static_call_test.h"

#include <linux/kconfig.h>
#include <linux/module.h>
#include <linux/static_call.h>

/*
 * Tests to ensure correctness in a variety of cases for static calls.
 *
 * The tests focus on ensuring the TOC is kept consistent across the
 * module-kernel boundary, as compilers can't see that a trampoline
 * defined locally to a caller might be jumping to a function with a
 * different TOC. So it's important that these tests are compiled as
 * a module to ensure the TOC will be different to the kernel's.
 */

/* Utils to hold a copy of the old register values while we test.
 *
 * We can't use the KUnit init/exit hooks because when the hooks and
 * test cases return they will be in the KUnit context that doesn't know
 * we've reserved and modified some non-volatile registers.
 */
static void* regsaves[3];

#define SAVE_REGS() \
	regsaves[0] = actual_toc; \
	regsaves[1] = module_toc; \
	regsaves[2] = kernel_toc; \
	module_toc = current_toc; \
	kernel_toc = (void*) get_paca()->kernel_toc;

#define RESTORE_REGS() \
	actual_toc = regsaves[0]; \
	module_toc = regsaves[1]; \
	kernel_toc = regsaves[2];

static int module_target_11(struct kunit *test)
{
	toc_fixup(test);
	return 11;
}

static int module_target_12(struct kunit *test)
{
	toc_fixup(test);
	return 12;
}

DEFINE_STATIC_CALL(module_sc, module_target_11);

DEFINE_STATIC_CALL_RET0(module_sc_ret0, int(void));
DEFINE_STATIC_CALL_NULL(module_sc_null, int(int));

static int add_one(int *val)
{
	return (*val)++;
}

static void null_function_test(struct kunit *test)
{
	int val = 0;

	SAVE_REGS();

	/* Check argument unconditionally evaluated */
	static_call_cond(module_sc_null)(add_one(&val));
	KUNIT_ASSERT_EQ(test, 1, val);

	RESTORE_REGS();
}

static void return_zero_test(struct kunit *test)
{
	int ret;

	SAVE_REGS();

	ret = PROTECTED_SC(test, int, static_call(module_sc_ret0)());
	KUNIT_ASSERT_EQ(test, 0, ret);

	static_call_update(ppc_sc_kernel, (void*)__static_call_return0);
	ret = PROTECTED_SC(test, int, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 0, ret);

	static_call_update(module_sc, (void*)__static_call_return0);
	ret = PROTECTED_SC(test, int, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 0, ret);

	RESTORE_REGS();
}

static void kernel_kernel_kernel_test(struct kunit *test)
{
	SAVE_REGS();

	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_1);
	KUNIT_ASSERT_EQ(test, 1, ppc_sc_kernel_call(test));

	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_2);
	KUNIT_ASSERT_EQ(test, 2, ppc_sc_kernel_call(test));

	RESTORE_REGS();
}

static void kernel_kernel_module_test(struct kunit *test)
{
	SAVE_REGS();

	static_call_update(ppc_sc_kernel, module_target_11);
	KUNIT_ASSERT_EQ(test, 11, ppc_sc_kernel_call(test));

	static_call_update(ppc_sc_kernel, module_target_12);
	KUNIT_ASSERT_EQ(test, 12, ppc_sc_kernel_call(test));

	RESTORE_REGS();
}

static void kernel_module_kernel_test(struct kunit *test)
{
	SAVE_REGS();

	static_call_update(module_sc, ppc_sc_kernel_target_1);
	KUNIT_ASSERT_EQ(test, 1, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));

	static_call_update(module_sc, ppc_sc_kernel_target_2);
	KUNIT_ASSERT_EQ(test, 2, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));

	RESTORE_REGS();
}

static void kernel_module_module_test(struct kunit *test)
{
	SAVE_REGS();

	static_call_update(module_sc, module_target_11);
	KUNIT_ASSERT_EQ(test, 11, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));

	static_call_update(module_sc, module_target_12);
	KUNIT_ASSERT_EQ(test, 12, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));

	RESTORE_REGS();
}

static void module_kernel_kernel_test(struct kunit *test)
{
	int ret;

	SAVE_REGS();

	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_1);
	ret = PROTECTED_SC(test, int, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 1, ret);

	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_2);
	ret = PROTECTED_SC(test, int, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 2, ret);

	RESTORE_REGS();
}

static void module_kernel_module_test(struct kunit *test)
{
	int ret;

	SAVE_REGS();

	static_call_update(ppc_sc_kernel, module_target_11);
	ret = PROTECTED_SC(test, int, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 11, ret);

	static_call_update(ppc_sc_kernel, module_target_12);
	ret = PROTECTED_SC(test, int, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 12, ret);

	RESTORE_REGS();
}

static void module_module_kernel_test(struct kunit *test)
{
	int ret;

	SAVE_REGS();

	static_call_update(module_sc, ppc_sc_kernel_target_1);
	ret = PROTECTED_SC(test, int, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 1, ret);

	static_call_update(module_sc, ppc_sc_kernel_target_2);
	ret = PROTECTED_SC(test, int, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 2, ret);

	RESTORE_REGS();
}

static void module_module_module_test(struct kunit *test)
{
	int ret;

	SAVE_REGS();

	static_call_update(module_sc, module_target_11);
	ret = PROTECTED_SC(test, int, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 11, ret);

	static_call_update(module_sc, module_target_12);
	ret = PROTECTED_SC(test, int, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 12, ret);

	RESTORE_REGS();
}

DEFINE_STATIC_CALL(module_sc_stack_params, ppc_sc_kernel_target_big);

static void stack_parameters_test(struct kunit *test)
{
	long m = 0x1234567887654321;
	long ret;

	SAVE_REGS();

	ret = PROTECTED_SC(test, long, static_call(module_sc_stack_params)(test, m, m, m, m, m, m, m, m, m));
	KUNIT_ASSERT_EQ(test, ~m, ret);

	RESTORE_REGS();
}

static struct kunit_case static_call_test_cases[] = {
	KUNIT_CASE(null_function_test),
	KUNIT_CASE(return_zero_test),
	KUNIT_CASE(stack_parameters_test),
	KUNIT_CASE(kernel_kernel_kernel_test),
	KUNIT_CASE(kernel_kernel_module_test),
	KUNIT_CASE(kernel_module_kernel_test),
	KUNIT_CASE(kernel_module_module_test),
	KUNIT_CASE(module_kernel_kernel_test),
	KUNIT_CASE(module_kernel_module_test),
	KUNIT_CASE(module_module_kernel_test),
	KUNIT_CASE(module_module_module_test),
	{}
};

static struct kunit_suite ppc_static_call_test_suite = {
	.name = "ppc-static-call",
	.test_cases = static_call_test_cases,
};
kunit_test_suite(ppc_static_call_test_suite);

MODULE_AUTHOR("Benjamin Gray <bgray@linux.ibm.com>");
MODULE_LICENSE("GPL");
