// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/kconfig.h>
#include <linux/module.h>
#include <linux/static_call.h>

/*
 * Tests to ensure correctness in a variety of cases for static calls.
 *
 * The tests focus on ensuring the TOC is kept consistent across the
 * module-kernel boundary, as compilers can't see that a trampoline
 * defined locally in the kernel might be jumping to a function in a
 * module. This makes it important that these tests are compiled as a
 * module, so the TOC will be different to the kernel's.
 *
 * Register variables are used to allow easy position independent
 * correction of a TOC before it is used for anything. This means
 * a failing test doesn't always crash the whole kernel. The registers
 * are initialised on entry and restored on exit of each test using
 * KUnit's init and exit hooks. The tests only call internal and
 * specially defined kernel functions, so the use of these registers
 * will not clobber anything else.
 */

extern void ppc_sc_kernel_toc_init(void);
extern void ppc_sc_kernel_toc_exit(void);
DECLARE_STATIC_CALL(ppc_sc_kernel, int(struct kunit*));
extern int ppc_sc_kernel_target_1(struct kunit* test);
extern int ppc_sc_kernel_target_2(struct kunit* test);
extern long ppc_sc_kernel_target_big(struct kunit* test,
				     long a,
				     long b,
				     long c,
				     long d,
				     long e,
				     long f,
				     long g,
				     long h,
				     long i);
extern int ppc_sc_kernel_call(struct kunit* test);
extern int ppc_sc_kernel_call_indirect(struct kunit* test, int(*fn)(struct kunit*));

/* Registers we reserve for use while testing */
register void* current_toc asm ("r2");
register void* module_toc asm ("r14");
register void* actual_toc asm ("r15");
register void* kernel_toc asm ("r16");
register void* actual_kernel_toc asm ("r17");

/* To hold a copy of the old register values while we test */
static void* static_module_toc;
static void* static_actual_toc;

#define restore_toc(test) \
	actual_toc = current_toc; \
	current_toc = module_toc

#define check_toc(test) KUNIT_EXPECT_PTR_EQ(test, module_toc, actual_toc)

/* Corrects, then asserts the original TOC was valid */
#define toc_fixup(test) \
	restore_toc(); \
	check_toc(test)

/* Wrapper around a static call to verify and correct the TOC
 * before running further code that might depend on it's value.
 */
#define PROTECTED_SC(test, call) \
({ \
	long ret; \
	ret = call; \
	toc_fixup(test); \
	ret; \
})

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

DEFINE_STATIC_CALL_RET0(module_sc_ret0, long(void));
DEFINE_STATIC_CALL_NULL(module_sc_null, long(long));

static long add_one(long *val)
{
	return (*val)++;
}

static void null_function_test(struct kunit *test)
{
	long val = 0;

	/* Check argument unconditionally evaluated */
	static_call_cond(module_sc_null)(add_one(&val));
	KUNIT_ASSERT_EQ(test, 1, val);
}

static void return_zero_test(struct kunit *test)
{
	long ret;

	ret = PROTECTED_SC(test, static_call(module_sc_ret0)());
	KUNIT_ASSERT_EQ(test, 0, ret);

	static_call_update(ppc_sc_kernel, (void*)__static_call_return0);
	ret = PROTECTED_SC(test, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 0, ret);

	static_call_update(module_sc, (void*)__static_call_return0);
	ret = PROTECTED_SC(test, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 0, ret);
}

static void kernel_kernel_kernel_test(struct kunit *test)
{
	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_1);
	KUNIT_ASSERT_EQ(test, 1, ppc_sc_kernel_call(test));

	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_2);
	KUNIT_ASSERT_EQ(test, 2, ppc_sc_kernel_call(test));
}

static void kernel_kernel_module_test(struct kunit *test)
{
	static_call_update(ppc_sc_kernel, module_target_11);
	KUNIT_ASSERT_EQ(test, 11, ppc_sc_kernel_call(test));

	static_call_update(ppc_sc_kernel, module_target_12);
	KUNIT_ASSERT_EQ(test, 12, ppc_sc_kernel_call(test));
}

static void kernel_module_kernel_test(struct kunit *test)
{
	static_call_update(module_sc, ppc_sc_kernel_target_1);
	KUNIT_ASSERT_EQ(test, 1, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));

	static_call_update(module_sc, ppc_sc_kernel_target_2);
	KUNIT_ASSERT_EQ(test, 2, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));
}

static void kernel_module_module_test(struct kunit *test)
{
	static_call_update(module_sc, module_target_11);
	KUNIT_ASSERT_EQ(test, 11, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));

	static_call_update(module_sc, module_target_12);
	KUNIT_ASSERT_EQ(test, 12, ppc_sc_kernel_call_indirect(test, static_call(module_sc)));
}

static void module_kernel_kernel_test(struct kunit *test)
{
	long ret;

	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_1);
	ret = PROTECTED_SC(test, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 1, ret);

	static_call_update(ppc_sc_kernel, ppc_sc_kernel_target_2);
	ret = PROTECTED_SC(test, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 2, ret);
}

static void module_kernel_module_test(struct kunit *test)
{
	long ret;

	static_call_update(ppc_sc_kernel, module_target_11);
	ret = PROTECTED_SC(test, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 11, ret);

	static_call_update(ppc_sc_kernel, module_target_12);
	ret = PROTECTED_SC(test, static_call(ppc_sc_kernel)(test));
	KUNIT_ASSERT_EQ(test, 12, ret);
}

static void module_module_kernel_test(struct kunit *test)
{
	long ret;

	static_call_update(module_sc, ppc_sc_kernel_target_1);
	ret = PROTECTED_SC(test, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 1, ret);

	static_call_update(module_sc, ppc_sc_kernel_target_2);
	ret = PROTECTED_SC(test, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 2, ret);
}

static void module_module_module_test(struct kunit *test)
{
	long ret;

	static_call_update(module_sc, module_target_11);
	ret = PROTECTED_SC(test, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 11, ret);

	static_call_update(module_sc, module_target_12);
	ret = PROTECTED_SC(test, static_call(module_sc)(test));
	KUNIT_ASSERT_EQ(test, 12, ret);
}

DEFINE_STATIC_CALL(module_sc_stack_params, ppc_sc_kernel_target_big);

static void stack_parameters_test(struct kunit *test)
{
	long m = 0x1234567887654321;
	long ret = PROTECTED_SC(test, static_call(module_sc_stack_params)(test, m, m, m, m, m, m, m, m, m));
	KUNIT_ASSERT_EQ(test, ~m, ret);
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

static int ppc_static_call_test_init(struct kunit *test)
{
	static_module_toc = module_toc;
	static_actual_toc = actual_toc;
	module_toc = current_toc;

	ppc_sc_kernel_toc_init();

	return 0;
}

static void ppc_static_call_test_exit(struct kunit *test)
{
	module_toc = static_module_toc;
	actual_toc = static_actual_toc;

	ppc_sc_kernel_toc_exit();
}

static struct kunit_suite ppc_static_call_test_suite = {
	.name = "ppc-static-call",
	.test_cases = static_call_test_cases,
	.init = ppc_static_call_test_init,
	.exit = ppc_static_call_test_exit,
};
kunit_test_suite(ppc_static_call_test_suite);

MODULE_AUTHOR("Benjamin Gray <bgray@linux.ibm.com>");
MODULE_LICENSE("GPL");
