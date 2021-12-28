// SPDX-License-Identifier: GPL-2.0-only

#define __SYSCALL_COMPAT

#include <linux/compat.h>
#include <linux/syscalls.h>
#include <asm-generic/mman-common.h>
#include <asm-generic/syscalls.h>
#include <asm/syscall.h>

#define arg_u32p(name)  u32, name##_lo, u32, name##_hi

#define arg_u64(name)   (((u64)name##_hi << 32) | \
			 ((u64)name##_lo & 0xffffffff))

COMPAT_SYSCALL_DEFINE3(truncate64, const char __user *, pathname,
		       arg_u32p(length))
{
	return ksys_truncate(pathname, arg_u64(length));
}

COMPAT_SYSCALL_DEFINE3(ftruncate64, unsigned int, fd, arg_u32p(length))
{
	return ksys_ftruncate(fd, arg_u64(length));
}

COMPAT_SYSCALL_DEFINE6(fallocate, int, fd, int, mode,
		       arg_u32p(offset), arg_u32p(len))
{
	return ksys_fallocate(fd, mode, arg_u64(offset), arg_u64(len));
}

COMPAT_SYSCALL_DEFINE5(pread64, unsigned int, fd, char __user *, buf,
		       size_t, count, arg_u32p(pos))
{
	return ksys_pread64(fd, buf, count, arg_u64(pos));
}

COMPAT_SYSCALL_DEFINE5(pwrite64, unsigned int, fd,
		       const char __user *, buf, size_t, count, arg_u32p(pos))
{
	return ksys_pwrite64(fd, buf, count, arg_u64(pos));
}

COMPAT_SYSCALL_DEFINE6(sync_file_range, int, fd, arg_u32p(offset),
		       arg_u32p(nbytes), unsigned int, flags)
{
	return ksys_sync_file_range(fd, arg_u64(offset), arg_u64(nbytes),
				    flags);
}

COMPAT_SYSCALL_DEFINE4(readahead, int, fd, arg_u32p(offset),
		       size_t, count)
{
	return ksys_readahead(fd, arg_u64(offset), count);
}

COMPAT_SYSCALL_DEFINE6(fadvise64_64, int, fd, int, advice, arg_u32p(offset),
		       arg_u32p(len))
{
	return ksys_fadvise64_64(fd, arg_u64(offset), arg_u64(len), advice);
}

#undef __SYSCALL
#define __SYSCALL(nr, call)      [nr] = (call),

asmlinkage long compat_sys_rt_sigreturn(void);

void * const compat_sys_call_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = sys_ni_syscall,
#include <asm/unistd.h>
};
