#ifndef _POWERPC_STATIC_CALL_TEST_
#define _POWERPC_STATIC_CALL_TEST_

#include <kunit/test.h>

/* Reserve these registers for testing so that a TOC error
 * doesn't necessarily crash the whole kernel.
 *
 * The tests ensure the contents are restored before returning.
 */
register void* current_toc asm ("r2");
register void* actual_toc asm ("r14");  /* Copy of TOC before fixup */
register void* module_toc asm ("r15");
register void* kernel_toc asm ("r16");

DECLARE_STATIC_CALL(ppc_sc_kernel, int(struct kunit*));
int ppc_sc_kernel_target_1(struct kunit* test);
int ppc_sc_kernel_target_2(struct kunit* test);
long ppc_sc_kernel_target_big(struct kunit* test,
				     long a,
				     long b,
				     long c,
				     long d,
				     long e,
				     long f,
				     long g,
				     long h,
				     long i);
int ppc_sc_kernel_call(struct kunit* test);
int ppc_sc_kernel_call_indirect(struct kunit* test, int(*fn)(struct kunit*));

#ifdef MODULE

#define toc_fixup(test) \
	actual_toc = current_toc; \
	current_toc = module_toc; \
	KUNIT_EXPECT_PTR_EQ(test, module_toc, actual_toc)

#else /* KERNEL */

#define toc_fixup(test) \
	actual_toc = current_toc; \
	current_toc = kernel_toc; \
	KUNIT_EXPECT_PTR_EQ(test, kernel_toc, actual_toc)

#endif /* MODULE */

#define PROTECTED_SC(test, ret_type, call) \
({ \
	ret_type ret; \
	ret = call; \
	toc_fixup(test); \
	ret; \
})

#endif /* _POWERPC_STATIC_CALL_TEST_ */
