// SPDX-License-Identifier: GPL-2.0-only
/*
 *  linux/kernel/panic.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * This function is used through-out the kernel (including mm and fs)
 * to indicate a major problem.
 */
#include <linux/debug_locks.h>
#include <linux/sched/debug.h>
#include <linux/interrupt.h>
#include <linux/kgdb.h>
#include <linux/kmsg_dump.h>
#include <linux/kallsyms.h>
#include <linux/vt_kern.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/ftrace.h>
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/kexec.h>
#include <linux/panic_notifier.h>
#include <linux/sched.h>
#include <linux/sysrq.h>
#include <linux/init.h>
#include <linux/nmi.h>
#include <linux/console.h>
#include <linux/bug.h>
#include <linux/ratelimit.h>
#include <linux/debugfs.h>
#include <trace/events/error_report.h>
#include <asm/sections.h>

#define PANIC_TIMER_STEP 100
#define PANIC_BLINK_SPD 18

#ifdef CONFIG_SMP
/*
 * Should we dump all CPUs backtraces in an oops event?
 * Defaults to 0, can be changed via sysctl.
 */
unsigned int __read_mostly sysctl_oops_all_cpu_backtrace;
#endif /* CONFIG_SMP */

int panic_on_oops = CONFIG_PANIC_ON_OOPS_VALUE;
static unsigned long tainted_mask =
	IS_ENABLED(CONFIG_GCC_PLUGIN_RANDSTRUCT) ? (1 << TAINT_RANDSTRUCT) : 0;
static int pause_on_oops;
static int pause_on_oops_flag;
static DEFINE_SPINLOCK(pause_on_oops_lock);

int panic_on_warn __read_mostly;
bool panic_on_taint_nousertaint;
unsigned long panic_on_taint;

int panic_timeout = CONFIG_PANIC_TIMEOUT;
EXPORT_SYMBOL_GPL(panic_timeout);

/* Initialized with all notifiers set to run before kdump */
static unsigned long panic_notifiers_bits = 15;

/* Default level is 2, see kernel-parameters.txt */
unsigned int panic_notifiers_level = 2;

/* DEPRECATED in favor of panic_notifiers_level */
bool crash_kexec_post_notifiers;

ATOMIC_NOTIFIER_HEAD(panic_hypervisor_list);
EXPORT_SYMBOL(panic_hypervisor_list);

ATOMIC_NOTIFIER_HEAD(panic_info_list);
EXPORT_SYMBOL(panic_info_list);

ATOMIC_NOTIFIER_HEAD(panic_pre_reboot_list);
EXPORT_SYMBOL(panic_pre_reboot_list);

ATOMIC_NOTIFIER_HEAD(panic_post_reboot_list);
EXPORT_SYMBOL(panic_post_reboot_list);

static long no_blink(int state)
{
	return 0;
}

/* Returns how long it waited in ms */
long (*panic_blink)(int state);
EXPORT_SYMBOL(panic_blink);

/*
 * Stop ourself in panic -- architecture code may override this
 */
void __weak panic_smp_self_stop(void)
{
	while (1)
		cpu_relax();
}

/*
 * Stop ourselves in NMI context if another CPU has already panicked. Arch code
 * may override this to prepare for crash dumping, e.g. save regs info.
 */
void __weak nmi_panic_self_stop(struct pt_regs *regs)
{
	panic_smp_self_stop();
}

/*
 * Stop other CPUs in panic context.
 *
 * Architecture dependent code may override this with more suitable version.
 * For example, if the architecture supports crash dump, it should save the
 * registers of each stopped CPU and disable per-CPU features such as
 * virtualization extensions. When not overridden in arch code (and for
 * x86/xen), this is exactly the same as execute smp_send_stop(), but
 * guarded against duplicate execution.
 */
void __weak crash_smp_send_stop(void)
{
	static int cpus_stopped;

	/*
	 * This function can be called twice in panic path, but obviously
	 * we execute this only once.
	 */
	if (cpus_stopped)
		return;

	/*
	 * Note smp_send_stop is the usual smp shutdown function, which
	 * unfortunately means it may not be hardened to work in a panic
	 * situation.
	 */
	smp_send_stop();
	cpus_stopped = 1;
}

atomic_t panic_cpu = ATOMIC_INIT(PANIC_CPU_INVALID);

/*
 * A variant of panic() called from NMI context. We return if we've already
 * panicked on this CPU. If another CPU already panicked, loop in
 * nmi_panic_self_stop() which can provide architecture dependent code such
 * as saving register state for crash dump.
 */
void nmi_panic(struct pt_regs *regs, const char *msg)
{
	int old_cpu, cpu;

	cpu = raw_smp_processor_id();
	old_cpu = atomic_cmpxchg(&panic_cpu, PANIC_CPU_INVALID, cpu);

	if (old_cpu == PANIC_CPU_INVALID)
		panic("%s", msg);
	else if (old_cpu != cpu)
		nmi_panic_self_stop(regs);
}
EXPORT_SYMBOL(nmi_panic);

/*
 * Helper that accumulates all console flushing routines executed on panic.
 */
static void console_flushing(void)
{
#ifdef CONFIG_VT
	unblank_screen();
#endif
	console_unblank();

	/*
	 * In this point, we may have disabled other CPUs, hence stopping the
	 * CPU holding the lock while still having some valuable data in the
	 * console buffer.
	 *
	 * Try to acquire the lock then release it regardless of the result.
	 * The release will also print the buffers out. Locks debug should
	 * be disabled to avoid reporting bad unlock balance when panic()
	 * is not being called from OOPS.
	 */
	debug_locks_off();
	console_flush_on_panic(CONSOLE_FLUSH_PENDING);

	/* In case users wish to replay the full log buffer... */
	if (panic_console_replay) {
		pr_warn("Replaying the log buffer from the beginning\n");
		console_flush_on_panic(CONSOLE_REPLAY_ALL);
	}
}

#define PN_HYPERVISOR_BIT	0
#define PN_INFO_BIT		1
#define PN_PRE_REBOOT_BIT	2
#define PN_POST_REBOOT_BIT	3

/*
 * Determine the order of panic notifiers with regards to kdump.
 *
 * This function relies in the "panic_notifiers_level" kernel parameter
 * to determine how to order the notifiers with regards to kdump. We
 * have currently 5 levels. For details, please check the kernel docs for
 * "panic_notifiers_level" at Documentation/admin-guide/kernel-parameters.txt.
 *
 * Default level is 2, which means the panic hypervisor and informational
 * (unless we don't have any kmsg_dumper) lists will execute before kdump.
 */
static void order_panic_notifiers_and_kdump(void)
{
	/*
	 * The parameter "crash_kexec_post_notifiers" is deprecated, but
	 * valid. Users that set it want really all panic notifiers to
	 * execute before kdump, so it's effectively the same as setting
	 * the panic notifiers level to 4.
	 */
	if (panic_notifiers_level >= 4 || crash_kexec_post_notifiers)
		return;

	/*
	 * Based on the level configured (smaller than 4), we clear the
	 * proper bits in "panic_notifiers_bits". Notice that this bitfield
	 * is initialized with all notifiers set.
	 */
	switch (panic_notifiers_level) {
	case 3:
		clear_bit(PN_PRE_REBOOT_BIT, &panic_notifiers_bits);
		break;
	case 2:
		clear_bit(PN_PRE_REBOOT_BIT, &panic_notifiers_bits);

		if (!kmsg_has_dumpers())
			clear_bit(PN_INFO_BIT, &panic_notifiers_bits);
		break;
	case 1:
		clear_bit(PN_PRE_REBOOT_BIT, &panic_notifiers_bits);
		clear_bit(PN_INFO_BIT, &panic_notifiers_bits);
		break;
	case 0:
		clear_bit(PN_PRE_REBOOT_BIT, &panic_notifiers_bits);
		clear_bit(PN_INFO_BIT, &panic_notifiers_bits);
		clear_bit(PN_HYPERVISOR_BIT, &panic_notifiers_bits);
		break;
	}
}

/*
 * Set of helpers to execute the panic notifiers only once.
 * Just the informational notifier cares about the return.
 */
static inline bool notifier_run_once(struct atomic_notifier_head head,
				     char *buf, long bit)
{
	if (test_and_change_bit(bit, &panic_notifiers_bits)) {
		atomic_notifier_call_chain(&head, PANIC_NOTIFIER, buf);
		return true;
	}
	return false;
}

#define panic_notifier_hypervisor_once(buf)\
	notifier_run_once(panic_hypervisor_list, buf, PN_HYPERVISOR_BIT)

#define panic_notifier_info_once(buf)\
	notifier_run_once(panic_info_list, buf, PN_INFO_BIT)

#define panic_notifier_pre_reboot_once(buf)\
	notifier_run_once(panic_pre_reboot_list, buf, PN_PRE_REBOOT_BIT)

#define panic_notifier_post_reboot_once(buf)\
	notifier_run_once(panic_post_reboot_list, buf, PN_POST_REBOOT_BIT)

/**
 *	panic - halt the system
 *	@fmt: The text string to print
 *
 *	Display a message, then perform cleanups.
 *
 *	This function never returns.
 */
void panic(const char *fmt, ...)
{
	static char buf[1024];
	va_list args;
	long i, i_next = 0, len;
	int state = 0;
	int old_cpu, this_cpu;

	/*
	 * This thread may hit another WARN() in the panic path, so
	 * resetting this option prevents additional WARN() from
	 * re-panicking the system here.
	 */
	panic_on_warn = 0;

	/*
	 * Disable local interrupts. This will prevent panic_smp_self_stop
	 * from deadlocking the first cpu that invokes the panic, since there
	 * is nothing to prevent an interrupt handler (that runs after setting
	 * panic_cpu) from invoking panic() again. Also disables preemption
	 * here - notice it's not safe to rely on interrupt disabling to avoid
	 * preemption, since any cond_resched() or cond_resched_lock() might
	 * trigger a reschedule if the preempt count is 0 (for reference, see
	 * Documentation/locking/preempt-locking.rst). Some functions called
	 * from here want preempt disabled, so no point enabling it later.
	 */
	local_irq_disable();
	preempt_disable_notrace();

	/*
	 * Only one CPU is allowed to execute the panic code from here. For
	 * multiple parallel invocations of panic, all other CPUs either
	 * stop themself or will wait until they are stopped by the 1st CPU
	 * with smp_send_stop().
	 *
	 * `old_cpu == PANIC_CPU_INVALID' means this is the 1st CPU which
	 * comes here, so go ahead.
	 * `old_cpu == this_cpu' means we came from nmi_panic() which sets
	 * panic_cpu to this CPU.  In this case, this is also the 1st CPU.
	 */
	this_cpu = raw_smp_processor_id();
	old_cpu  = atomic_cmpxchg(&panic_cpu, PANIC_CPU_INVALID, this_cpu);

	if (old_cpu != PANIC_CPU_INVALID && old_cpu != this_cpu)
		panic_smp_self_stop();

	console_verbose();
	bust_spinlocks(1);
	va_start(args, fmt);
	len = vscnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (len && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	pr_emerg("Kernel panic - not syncing: %s\n", buf);
#ifdef CONFIG_DEBUG_BUGVERBOSE
	/*
	 * Avoid nested stack-dumping if a panic occurs during oops processing
	 */
	if (!test_taint(TAINT_DIE) && oops_in_progress <= 1)
		dump_stack();
#endif

	/*
	 * If kgdb is enabled, give it a chance to run before we stop all
	 * the other CPUs or else we won't be able to debug processes left
	 * running on them.
	 */
	kgdb_panic(buf);

	/*
	 * Here lies one of the most subtle parts of the panic path,
	 * the panic notifiers and their order with regards to kdump.
	 * We currently have 4 sets of notifiers:
	 *
	 *  - the hypervisor list is composed by callbacks that are related
	 *  to warn the FW / hypervisor about panic, or non-invasive LED
	 *  controlling functions - (hopefully) low-risk for kdump, should
	 *  run early if possible.
	 *
	 *  - the informational list is composed by functions dumping data
	 *  like kernel offsets, device error registers or tracing buffer;
	 *  also log flooding prevention callbacks fit in this list. It is
	 *  relatively safe to run before kdump.
	 *
	 *  - the pre_reboot list basically is everything else, all the
	 *  callbacks that don't fit in the 2 previous lists. It should
	 *  run *after* kdump if possible, as it contains high-risk
	 *  functions that may break kdump.
	 *
	 *  - we also have a 4th list of notifiers, the post_reboot
	 *  callbacks. This is not strongly related to kdump since it's
	 *  always executed late in the panic path, after the restart
	 *  mechanism (if set); its goal is to provide a way for
	 *  architecture code effectively power-off/disable the system.
	 *
	 *  The kernel provides the "panic_notifiers_level" parameter
	 *  to adjust the ordering in which these notifiers should run
	 *  with regards to kdump - the default level is 2, so both the
	 *  hypervisor and informational notifiers should execute before
	 *  the __crash_kexec(); the info notifier won't run by default
	 *  unless there's some kmsg_dumper() registered. For details
	 *  about it, check Documentation/admin-guide/kernel-parameters.txt.
	 *
	 *  Notice that the code relies in bits set/clear operations to
	 *  determine the ordering, functions *_once() execute only one
	 *  time, as their name implies. The goal is to prevent too much
	 *  if conditionals and more confusion. Finally, regarding CPUs
	 *  disabling: unless NO panic notifier executes before kdump,
	 *  we always disable secondary CPUs before __crash_kexec() and
	 *  the notifiers execute.
	 */
	order_panic_notifiers_and_kdump();

	/* If no level, we should kdump ASAP. */
	if (!panic_notifiers_level)
		__crash_kexec(NULL);

	crash_smp_send_stop();
	panic_notifier_hypervisor_once(buf);

	if (panic_notifier_info_once(buf))
		kmsg_dump(KMSG_DUMP_PANIC);

	panic_notifier_pre_reboot_once(buf);

	__crash_kexec(NULL);

	panic_notifier_hypervisor_once(buf);

	if (panic_notifier_info_once(buf))
		kmsg_dump(KMSG_DUMP_PANIC);

	panic_notifier_pre_reboot_once(buf);

	console_flushing();
	if (!panic_blink)
		panic_blink = no_blink;

	if (panic_timeout > 0) {
		/*
		 * Delay timeout seconds before rebooting the machine.
		 * We can't use the "normal" timers since we just panicked.
		 */
		pr_emerg("Rebooting in %d seconds..\n", panic_timeout);

		for (i = 0; i < panic_timeout * 1000; i += PANIC_TIMER_STEP) {
			touch_nmi_watchdog();
			if (i >= i_next) {
				i += panic_blink(state ^= 1);
				i_next = i + 3600 / PANIC_BLINK_SPD;
			}
			mdelay(PANIC_TIMER_STEP);
		}
	}
	if (panic_timeout != 0) {
		/*
		 * This will not be a clean reboot, with everything
		 * shutting down.  But if there is a chance of
		 * rebooting the system it will be rebooted.
		 */
		if (panic_reboot_mode != REBOOT_UNDEFINED)
			reboot_mode = panic_reboot_mode;
		emergency_restart();
	}

	panic_notifier_post_reboot_once(buf);

	pr_emerg("---[ end Kernel panic - not syncing: %s ]---\n", buf);

	/* Do not scroll important messages printed above */
	suppress_printk = 1;
	local_irq_enable();
	for (i = 0; ; i += PANIC_TIMER_STEP) {
		touch_softlockup_watchdog();
		if (i >= i_next) {
			i += panic_blink(state ^= 1);
			i_next = i + 3600 / PANIC_BLINK_SPD;
		}
		mdelay(PANIC_TIMER_STEP);
	}
}

EXPORT_SYMBOL(panic);

/*
 * Helper used in the kexec code, to validate if any
 * panic notifier is set to execute early, before kdump.
 */
inline bool panic_notifiers_before_kdump(void)
{
	return panic_notifiers_level || crash_kexec_post_notifiers;
}

/*
 * TAINT_FORCED_RMMOD could be a per-module flag but the module
 * is being removed anyway.
 */
const struct taint_flag taint_flags[TAINT_FLAGS_COUNT] = {
	[ TAINT_PROPRIETARY_MODULE ]	= { 'P', 'G', true },
	[ TAINT_FORCED_MODULE ]		= { 'F', ' ', true },
	[ TAINT_CPU_OUT_OF_SPEC ]	= { 'S', ' ', false },
	[ TAINT_FORCED_RMMOD ]		= { 'R', ' ', false },
	[ TAINT_MACHINE_CHECK ]		= { 'M', ' ', false },
	[ TAINT_BAD_PAGE ]		= { 'B', ' ', false },
	[ TAINT_USER ]			= { 'U', ' ', false },
	[ TAINT_DIE ]			= { 'D', ' ', false },
	[ TAINT_OVERRIDDEN_ACPI_TABLE ]	= { 'A', ' ', false },
	[ TAINT_WARN ]			= { 'W', ' ', false },
	[ TAINT_CRAP ]			= { 'C', ' ', true },
	[ TAINT_FIRMWARE_WORKAROUND ]	= { 'I', ' ', false },
	[ TAINT_OOT_MODULE ]		= { 'O', ' ', true },
	[ TAINT_UNSIGNED_MODULE ]	= { 'E', ' ', true },
	[ TAINT_SOFTLOCKUP ]		= { 'L', ' ', false },
	[ TAINT_LIVEPATCH ]		= { 'K', ' ', true },
	[ TAINT_AUX ]			= { 'X', ' ', true },
	[ TAINT_RANDSTRUCT ]		= { 'T', ' ', true },
};

/**
 * print_tainted - return a string to represent the kernel taint state.
 *
 * For individual taint flag meanings, see Documentation/admin-guide/sysctl/kernel.rst
 *
 * The string is overwritten by the next call to print_tainted(),
 * but is always NULL terminated.
 */
const char *print_tainted(void)
{
	static char buf[TAINT_FLAGS_COUNT + sizeof("Tainted: ")];

	BUILD_BUG_ON(ARRAY_SIZE(taint_flags) != TAINT_FLAGS_COUNT);

	if (tainted_mask) {
		char *s;
		int i;

		s = buf + sprintf(buf, "Tainted: ");
		for (i = 0; i < TAINT_FLAGS_COUNT; i++) {
			const struct taint_flag *t = &taint_flags[i];
			*s++ = test_bit(i, &tainted_mask) ?
					t->c_true : t->c_false;
		}
		*s = 0;
	} else
		snprintf(buf, sizeof(buf), "Not tainted");

	return buf;
}

int test_taint(unsigned flag)
{
	return test_bit(flag, &tainted_mask);
}
EXPORT_SYMBOL(test_taint);

unsigned long get_taint(void)
{
	return tainted_mask;
}

/**
 * add_taint: add a taint flag if not already set.
 * @flag: one of the TAINT_* constants.
 * @lockdep_ok: whether lock debugging is still OK.
 *
 * If something bad has gone wrong, you'll want @lockdebug_ok = false, but for
 * some notewortht-but-not-corrupting cases, it can be set to true.
 */
void add_taint(unsigned flag, enum lockdep_ok lockdep_ok)
{
	if (lockdep_ok == LOCKDEP_NOW_UNRELIABLE && __debug_locks_off())
		pr_warn("Disabling lock debugging due to kernel taint\n");

	set_bit(flag, &tainted_mask);

	if (tainted_mask & panic_on_taint) {
		panic_on_taint = 0;
		panic("panic_on_taint set ...");
	}
}
EXPORT_SYMBOL(add_taint);

static void spin_msec(int msecs)
{
	int i;

	for (i = 0; i < msecs; i++) {
		touch_nmi_watchdog();
		mdelay(1);
	}
}

/*
 * It just happens that oops_enter() and oops_exit() are identically
 * implemented...
 */
static void do_oops_enter_exit(void)
{
	unsigned long flags;
	static int spin_counter;

	if (!pause_on_oops)
		return;

	spin_lock_irqsave(&pause_on_oops_lock, flags);
	if (pause_on_oops_flag == 0) {
		/* This CPU may now print the oops message */
		pause_on_oops_flag = 1;
	} else {
		/* We need to stall this CPU */
		if (!spin_counter) {
			/* This CPU gets to do the counting */
			spin_counter = pause_on_oops;
			do {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(MSEC_PER_SEC);
				spin_lock(&pause_on_oops_lock);
			} while (--spin_counter);
			pause_on_oops_flag = 0;
		} else {
			/* This CPU waits for a different one */
			while (spin_counter) {
				spin_unlock(&pause_on_oops_lock);
				spin_msec(1);
				spin_lock(&pause_on_oops_lock);
			}
		}
	}
	spin_unlock_irqrestore(&pause_on_oops_lock, flags);
}

/*
 * Return true if the calling CPU is allowed to print oops-related info.
 * This is a bit racy..
 */
bool oops_may_print(void)
{
	return pause_on_oops_flag == 0;
}

/*
 * Called when the architecture enters its oops handler, before it prints
 * anything.  If this is the first CPU to oops, and it's oopsing the first
 * time then let it proceed.
 *
 * This is all enabled by the pause_on_oops kernel boot option.  We do all
 * this to ensure that oopses don't scroll off the screen.  It has the
 * side-effect of preventing later-oopsing CPUs from mucking up the display,
 * too.
 *
 * It turns out that the CPU which is allowed to print ends up pausing for
 * the right duration, whereas all the other CPUs pause for twice as long:
 * once in oops_enter(), once in oops_exit().
 */
void oops_enter(void)
{
	tracing_off();
	/* can't trust the integrity of the kernel anymore: */
	debug_locks_off();
	do_oops_enter_exit();

	if (sysctl_oops_all_cpu_backtrace)
		trigger_all_cpu_backtrace();
}

static void print_oops_end_marker(void)
{
	pr_warn("---[ end trace %016llx ]---\n", 0ULL);
}

/*
 * Called when the architecture exits its oops handler, after printing
 * everything.
 */
void oops_exit(void)
{
	do_oops_enter_exit();
	print_oops_end_marker();
	kmsg_dump(KMSG_DUMP_OOPS);
}

struct warn_args {
	const char *fmt;
	va_list args;
};

void __warn(const char *file, int line, void *caller, unsigned taint,
	    struct pt_regs *regs, struct warn_args *args)
{
	disable_trace_on_warning();

	if (file)
		pr_warn("WARNING: CPU: %d PID: %d at %s:%d %pS\n",
			raw_smp_processor_id(), current->pid, file, line,
			caller);
	else
		pr_warn("WARNING: CPU: %d PID: %d at %pS\n",
			raw_smp_processor_id(), current->pid, caller);

	if (args)
		vprintk(args->fmt, args->args);

	print_modules();

	if (regs)
		show_regs(regs);

	if (panic_on_warn)
		panic("panic_on_warn set ...\n");

	if (!regs)
		dump_stack();

	print_irqtrace_events(current);

	print_oops_end_marker();
	trace_error_report_end(ERROR_DETECTOR_WARN, (unsigned long)caller);

	/* Just a warning, don't kill lockdep. */
	add_taint(taint, LOCKDEP_STILL_OK);
}

#ifndef __WARN_FLAGS
void warn_slowpath_fmt(const char *file, int line, unsigned taint,
		       const char *fmt, ...)
{
	struct warn_args args;

	pr_warn(CUT_HERE);

	if (!fmt) {
		__warn(file, line, __builtin_return_address(0), taint,
		       NULL, NULL);
		return;
	}

	args.fmt = fmt;
	va_start(args.args, fmt);
	__warn(file, line, __builtin_return_address(0), taint, NULL, &args);
	va_end(args.args);
}
EXPORT_SYMBOL(warn_slowpath_fmt);
#else
void __warn_printk(const char *fmt, ...)
{
	va_list args;

	pr_warn(CUT_HERE);

	va_start(args, fmt);
	vprintk(fmt, args);
	va_end(args);
}
EXPORT_SYMBOL(__warn_printk);
#endif

#ifdef CONFIG_BUG

/* Support resetting WARN*_ONCE state */

static int clear_warn_once_set(void *data, u64 val)
{
	generic_bug_clear_once();
	memset(__start_once, 0, __end_once - __start_once);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(clear_warn_once_fops, NULL, clear_warn_once_set,
			 "%lld\n");

static __init int register_warn_debugfs(void)
{
	/* Don't care about failure */
	debugfs_create_file_unsafe("clear_warn_once", 0200, NULL, NULL,
				   &clear_warn_once_fops);
	return 0;
}

device_initcall(register_warn_debugfs);
#endif

#ifdef CONFIG_STACKPROTECTOR

/*
 * Called when gcc's -fstack-protector feature is used, and
 * gcc detects corruption of the on-stack canary value
 */
__visible noinstr void __stack_chk_fail(void)
{
	instrumentation_begin();
	panic("stack-protector: Kernel stack is corrupted in: %pB",
		__builtin_return_address(0));
	instrumentation_end();
}
EXPORT_SYMBOL(__stack_chk_fail);

#endif

core_param(panic, panic_timeout, int, 0644);
core_param(pause_on_oops, pause_on_oops, int, 0644);
core_param(panic_on_warn, panic_on_warn, int, 0644);
core_param(panic_notifiers_level, panic_notifiers_level, uint, 0644);

/* DEPRECATED in favor of panic_notifiers_level */
core_param(crash_kexec_post_notifiers, crash_kexec_post_notifiers, bool, 0644);

static int __init oops_setup(char *s)
{
	if (!s)
		return -EINVAL;
	if (!strcmp(s, "panic"))
		panic_on_oops = 1;
	return 0;
}
early_param("oops", oops_setup);

static int __init panic_on_taint_setup(char *s)
{
	char *taint_str;

	if (!s)
		return -EINVAL;

	taint_str = strsep(&s, ",");
	if (kstrtoul(taint_str, 16, &panic_on_taint))
		return -EINVAL;

	/* make sure panic_on_taint doesn't hold out-of-range TAINT flags */
	panic_on_taint &= TAINT_FLAGS_MAX;

	if (!panic_on_taint)
		return -EINVAL;

	if (s && !strcmp(s, "nousertaint"))
		panic_on_taint_nousertaint = true;

	pr_info("panic_on_taint: bitmask=0x%lx nousertaint_mode=%sabled\n",
		panic_on_taint, panic_on_taint_nousertaint ? "en" : "dis");

	return 0;
}
early_param("panic_on_taint", panic_on_taint_setup);
