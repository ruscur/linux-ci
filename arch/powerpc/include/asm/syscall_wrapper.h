/* SPDX-License-Identifier: GPL-2.0 */
/*
 * syscall_wrapper.h - powerpc specific wrappers to syscall definitions
 *
 * Based on arch/{x86,arm64}/include/asm/syscall_wrapper.h
 */

#ifndef __ASM_SYSCALL_WRAPPER_H
#define __ASM_SYSCALL_WRAPPER_H

struct pt_regs;

#define SC_POWERPC_REGS_TO_ARGS(x, ...)				\
	__MAP(x,__SC_ARGS					\
	      ,,regs->gpr[3],,regs->gpr[4],,regs->gpr[5]	\
	      ,,regs->gpr[6],,regs->gpr[7],,regs->gpr[8])

#ifdef CONFIG_COMPAT

#define COMPAT_SYSCALL_DEFINEx(x, name, ...)						\
	asmlinkage long __powerpc_compat_sys##name(const struct pt_regs *regs);		\
	ALLOW_ERROR_INJECTION(__powerpc_compat_sys##name, ERRNO);			\
	static long __se_compat_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));		\
	static inline long __do_compat_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));	\
	asmlinkage long __powerpc_compat_sys##name(const struct pt_regs *regs)		\
	{										\
		return __se_compat_sys##name(SC_POWERPC_REGS_TO_ARGS(x,__VA_ARGS__));	\
	}										\
	static long __se_compat_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))		\
	{										\
		return __do_compat_sys##name(__MAP(x,__SC_DELOUSE,__VA_ARGS__));	\
	}										\
	static inline long __do_compat_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))

#define COMPAT_SYSCALL_DEFINE0(sname)							\
	asmlinkage long __powerpc_compat_sys_##sname(const struct pt_regs *__unused);	\
	ALLOW_ERROR_INJECTION(__powerpc_compat_sys_##sname, ERRNO);			\
	asmlinkage long __powerpc_compat_sys_##sname(const struct pt_regs *__unused)

#define COND_SYSCALL_COMPAT(name)							\
	long __powerpc_compat_sys_##name(const struct pt_regs *regs);			\
	asmlinkage long __weak __powerpc_compat_sys_##name(const struct pt_regs *regs)	\
	{										\
		return sys_ni_syscall();						\
	}
#define COMPAT_SYS_NI(name) \
	SYSCALL_ALIAS(__powerpc_compat_sys_##name, sys_ni_posix_timers);

#endif /* CONFIG_COMPAT */

#define __SYSCALL_DEFINEx(x, name, ...)						\
	asmlinkage long __powerpc_sys##name(const struct pt_regs *regs);	\
	ALLOW_ERROR_INJECTION(__powerpc_sys##name, ERRNO);			\
	long sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));				\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));		\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));	\
	asmlinkage long __powerpc_sys##name(const struct pt_regs *regs)		\
	{									\
		return __se_sys##name(SC_POWERPC_REGS_TO_ARGS(x,__VA_ARGS__));	\
	}									\
	long sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))				\
	{									\
		return __do_sys##name(__MAP(x,__SC_CAST,__VA_ARGS__));		\
	}									\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))		\
	{									\
		long ret = __do_sys##name(__MAP(x,__SC_CAST,__VA_ARGS__));	\
		__MAP(x,__SC_TEST,__VA_ARGS__);					\
		__PROTECT(x, ret,__MAP(x,__SC_ARGS,__VA_ARGS__));		\
		return ret;							\
	}									\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))

#define SYSCALL_DEFINE0(sname)							\
	SYSCALL_METADATA(_##sname, 0);						\
	long sys_##sname(void);							\
	asmlinkage long __powerpc_sys_##sname(const struct pt_regs *__unused);	\
	ALLOW_ERROR_INJECTION(__powerpc_sys_##sname, ERRNO);			\
	long sys_##sname(void)							\
	{									\
		return __powerpc_sys_##sname(NULL);				\
	}									\
	asmlinkage long __powerpc_sys_##sname(const struct pt_regs *__unused)

#define COND_SYSCALL(name)							\
	long __powerpc_sys_##name(const struct pt_regs *regs);			\
	asmlinkage long __weak __powerpc_sys_##name(const struct pt_regs *regs)	\
	{									\
		return sys_ni_syscall();					\
	}

#define SYS_NI(name) SYSCALL_ALIAS(__powerpc_sys_##name, sys_ni_posix_timers);

#endif /* __ASM_SYSCALL_WRAPPER_H */
