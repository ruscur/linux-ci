// SPDX-License-Identifier: GPL-2.0-only

#include <linux/uaccess.h>
#include <linux/kernel.h>

#include <asm/disassemble.h>
#include <asm/inst.h>
#include <asm/ppc-opcode.h>

bool copy_from_kernel_nofault_allowed(const void *unsafe_src, size_t size)
{
	return is_kernel_addr((unsigned long)unsafe_src);
}

int copy_inst_from_kernel_nofault(ppc_inst_t *inst, u32 *src)
{
	unsigned int val, suffix;

	if (unlikely(!is_kernel_addr((unsigned long)src)))
		return -ERANGE;

	pagefault_disable();
	__get_kernel_nofault(&val, src, u32, Efault);
	if (IS_ENABLED(CONFIG_PPC64) && get_op(val) == OP_PREFIX) {
		__get_kernel_nofault(&suffix, src + 1, u32, Efault);
		pagefault_enable();
		*inst = ppc_inst_prefix(val, suffix);
	} else {
		pagefault_enable();
		*inst = ppc_inst(val);
	}
	return 0;
Efault:
	pagefault_enable();
	return -EFAULT;
}
