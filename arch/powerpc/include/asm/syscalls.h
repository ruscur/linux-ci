/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_POWERPC_SYSCALLS_H
#define __ASM_POWERPC_SYSCALLS_H
#ifdef __KERNEL__

#include <linux/compiler.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/compat.h>

#include <asm/syscall.h>
#ifdef CONFIG_PPC64
#include <asm/ppc32.h>
#endif
#include <asm/unistd.h>
#include <asm/ucontext.h>

#ifndef CONFIG_ARCH_HAS_SYSCALL_WRAPPER
asmlinkage long sys_ni_syscall(void);
#else
asmlinkage long sys_ni_syscall(const struct pt_regs *regs);
#endif

struct rtas_args;

#ifndef CONFIG_ARCH_HAS_SYSCALL_WRAPPER

/*
 * PowerPC architecture-specific syscalls
 */

asmlinkage long sys_rtas(struct rtas_args __user *uargs);

#ifdef CONFIG_PPC64
asmlinkage long sys_ppc64_personality(unsigned long personality);
#ifdef CONFIG_COMPAT
asmlinkage long compat_sys_ppc64_personality(unsigned long personality);
#endif /* CONFIG_COMPAT */
#endif /* CONFIG_PPC64 */

/* Parameters are reordered for powerpc to avoid padding */
asmlinkage long sys_ppc_fadvise64_64(int fd, int advice,
				     u32 offset_high, u32 offset_low,
				     u32 len_high, u32 len_low);
asmlinkage long sys_swapcontext(struct ucontext __user *old_ctx,
				struct ucontext __user *new_ctx, long ctx_size);
asmlinkage long sys_mmap(unsigned long addr, size_t len,
			 unsigned long prot, unsigned long flags,
			 unsigned long fd, off_t offset);
asmlinkage long sys_mmap2(unsigned long addr, size_t len,
			  unsigned long prot, unsigned long flags,
			  unsigned long fd, unsigned long pgoff);
asmlinkage long sys_switch_endian(void);

#ifdef CONFIG_PPC32
asmlinkage long sys_sigreturn(void);
asmlinkage long sys_debug_setcontext(struct ucontext __user *ctx, int ndbg,
				     struct sig_dbg_op __user *dbg);
#endif

asmlinkage long sys_rt_sigreturn(void);

asmlinkage long sys_subpage_prot(unsigned long addr,
				 unsigned long len, u32 __user *map);

#ifdef CONFIG_COMPAT
asmlinkage long compat_sys_swapcontext(struct ucontext32 __user *old_ctx,
				       struct ucontext32 __user *new_ctx,
				       int ctx_size);
asmlinkage long compat_sys_old_getrlimit(unsigned int resource,
					 struct compat_rlimit __user *rlim);
asmlinkage long compat_sys_sigreturn(void);
asmlinkage long compat_sys_rt_sigreturn(void);

/* Architecture-specific implementations in sys_ppc32.c */

asmlinkage long compat_sys_mmap2(unsigned long addr, size_t len,
				     unsigned long prot, unsigned long flags,
				     unsigned long fd, unsigned long pgoff);
asmlinkage long compat_sys_ppc_pread64(unsigned int fd,
				       char __user *ubuf, compat_size_t count,
				       u32 reg6, u32 pos1, u32 pos2);
asmlinkage long compat_sys_ppc_pwrite64(unsigned int fd,
					const char __user *ubuf, compat_size_t count,
					u32 reg6, u32 pos1, u32 pos2);
asmlinkage long compat_sys_ppc_readahead(int fd, u32 r4,
					 u32 offset1, u32 offset2, u32 count);
asmlinkage long compat_sys_ppc_truncate64(const char __user *path, u32 reg4,
					  unsigned long len1, unsigned long len2);
asmlinkage long compat_sys_ppc_fallocate(int fd, int mode, u32 offset1, u32 offset2,
					 u32 len1, u32 len2);
asmlinkage long compat_sys_ppc_ftruncate64(unsigned int fd, u32 reg4,
					   unsigned long len1, unsigned long len2);
asmlinkage long compat_sys_ppc32_fadvise64(int fd, u32 unused, u32 offset1, u32 offset2,
					   size_t len, int advice);
asmlinkage long compat_sys_ppc_sync_file_range2(int fd, unsigned int flags,
						unsigned int offset1,
						unsigned int offset2,
						unsigned int nbytes1,
						unsigned int nbytes2);
#endif /* CONFIG_COMPAT */

#else

#define __SYSCALL_WITH_COMPAT(nr, native, compat)	__SYSCALL(nr, native)
#define __SYSCALL(nr, entry) \
	asmlinkage long __powerpc_##entry(const struct pt_regs *regs);

#ifdef CONFIG_PPC64
#include <asm/syscall_table_64.h>
#else
#include <asm/syscall_table_32.h>
#endif /* CONFIG_PPC64 */

#ifdef CONFIG_COMPAT
#undef __SYSCALL_WITH_COMPAT
#define __SYSCALL_WITH_COMPAT(nr, native, compat)	__SYSCALL(nr, compat)
#include <asm/syscall_table_32.h>
#endif /* CONFIG_COMPAT */

#endif /* CONFIG_ARCH_HAS_SYSCALL_WRAPPER */

#endif /* __KERNEL__ */
#endif /* __ASM_POWERPC_SYSCALLS_H */
