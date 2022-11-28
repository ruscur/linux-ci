#include <linux/cache.h>
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/kconfig.h>
#include <linux/prctl.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>

#include <asm/cpu_has_feature.h>
#include <asm/cputable.h>
#include <asm/disassemble.h>
#include <asm/inst.h>
#include <asm/ppc-opcode.h>
#include <asm/processor.h>
#include <asm/reg.h>

#define DEFAULT_DEXCR	0

/* Allow process configuration of these by default */
static unsigned long dexcr_prctl_editable __ro_after_init =
	DEXCR_PRO_SBHE | DEXCR_PRO_IBRTPD | DEXCR_PRO_SRAPD | DEXCR_PRO_NPHIE;

/*
 * Lock to protect system DEXCR override from concurrent updates.
 * RCU semantics: writers take lock, readers are unlocked.
 * Writers ensure the memory update is atomic, readers read
 * atomically.
 */
static DEFINE_SPINLOCK(dexcr_sys_enforced_write_lock);

struct mask_override {
	union {
		struct {
			unsigned int mask;
			unsigned int override;
		};

		/* Raw access for atomic read/write */
		unsigned long all;
	};
};

static struct mask_override dexcr_sys_enforced;

static int spec_branch_hint_enable = -1;

static void update_userspace_system_dexcr(unsigned int pro_mask, int value)
{
	struct mask_override update = { .all = 0 };

	switch (value) {
	case -1:  /* Clear the mask bit, clear the override bit */
		break;
	case 0:  /* Set the mask bit, clear the override bit */
		update.mask |= pro_mask;
		break;
	case 1:  /* Set the mask bit, set the override bit */
		update.mask |= pro_mask;
		update.override |= pro_mask;
		break;
	}

	spin_lock(&dexcr_sys_enforced_write_lock);

	/* Use the existing values for the non-updated bits */
	update.mask |= dexcr_sys_enforced.mask & ~pro_mask;
	update.override |= dexcr_sys_enforced.override & ~pro_mask;

	/* Atomically update system enforced aspects */
	WRITE_ONCE(dexcr_sys_enforced.all, update.all);

	spin_unlock(&dexcr_sys_enforced_write_lock);
}

static int __init dexcr_init(void)
{
	if (!early_cpu_has_feature(CPU_FTR_ARCH_31))
		return 0;

	mtspr(SPRN_DEXCR, DEFAULT_DEXCR);

	if (early_cpu_has_feature(CPU_FTR_DEXCR_SBHE))
		update_userspace_system_dexcr(DEXCR_PRO_SBHE, spec_branch_hint_enable);

	if (early_cpu_has_feature(CPU_FTR_DEXCR_NPHIE) &&
	    IS_ENABLED(CONFIG_PPC_USER_ROP_PROTECT)) {
		update_userspace_system_dexcr(DEXCR_PRO_NPHIE, 1);
		dexcr_prctl_editable &= ~DEXCR_PRO_NPHIE;
	}

	return 0;
}
early_initcall(dexcr_init);

bool is_hashchk_trap(struct pt_regs const *regs)
{
	ppc_inst_t insn;

	if (!cpu_has_feature(CPU_FTR_DEXCR_NPHIE))
		return false;

	if (get_user_instr(insn, (void __user *)regs->nip)) {
		WARN_ON(1);
		return false;
	}

	if (ppc_inst_primary_opcode(insn) == 31 &&
	    get_xop(ppc_inst_val(insn)) == OP_31_XOP_HASHCHK)
		return true;

	return false;
}

unsigned long get_thread_dexcr(struct thread_struct const *t)
{
	unsigned long dexcr = DEFAULT_DEXCR;

	/* Atomically read enforced mask & override */
	struct mask_override enforced = READ_ONCE(dexcr_sys_enforced);

	/* Apply prctl overrides */
	dexcr = (dexcr & ~t->dexcr_mask) | t->dexcr_override;

	/* Apply system overrides */
	dexcr = (dexcr & ~enforced.mask) | enforced.override;

	return dexcr;
}

static void update_dexcr_on_cpu(void *info)
{
	mtspr(SPRN_DEXCR, get_thread_dexcr(&current->thread));
}

static int dexcr_aspect_get(struct task_struct *task, unsigned int aspect)
{
	int ret = 0;

	if (aspect & dexcr_prctl_editable)
		ret |= PR_PPC_DEXCR_PRCTL;

	if (aspect & task->thread.dexcr_mask) {
		if (aspect & task->thread.dexcr_override) {
			if (aspect & task->thread.dexcr_forced)
				ret |= PR_PPC_DEXCR_FORCE_SET_ASPECT;
			else
				ret |= PR_PPC_DEXCR_SET_ASPECT;
		} else {
			ret |= PR_PPC_DEXCR_CLEAR_ASPECT;
		}
	}

	return ret;
}

int dexcr_prctl_get(struct task_struct *task, unsigned long which)
{
	switch (which) {
	case PR_PPC_DEXCR_SBHE:
		if (!cpu_has_feature(CPU_FTR_DEXCR_SBHE))
			return -ENODEV;
		return dexcr_aspect_get(task, DEXCR_PRO_SBHE);
	case PR_PPC_DEXCR_IBRTPD:
		if (!cpu_has_feature(CPU_FTR_DEXCR_IBRTPD))
			return -ENODEV;
		return dexcr_aspect_get(task, DEXCR_PRO_IBRTPD);
	case PR_PPC_DEXCR_SRAPD:
		if (!cpu_has_feature(CPU_FTR_DEXCR_SRAPD))
			return -ENODEV;
		return dexcr_aspect_get(task, DEXCR_PRO_SRAPD);
	case PR_PPC_DEXCR_NPHIE:
		if (!cpu_has_feature(CPU_FTR_DEXCR_NPHIE))
			return -ENODEV;
		return dexcr_aspect_get(task, DEXCR_PRO_NPHIE);
	default:
		return -ENODEV;
	}
}

static int dexcr_aspect_set(struct task_struct *task, unsigned int aspect, unsigned long ctrl)
{
	if (!(aspect & dexcr_prctl_editable))
		return -ENXIO;  /* Aspect is not allowed to be changed by prctl */

	if (aspect & task->thread.dexcr_forced)
		return -EPERM;  /* Aspect has been forced to current state */

	switch (ctrl) {
	case PR_PPC_DEXCR_SET_ASPECT:
		task->thread.dexcr_mask |= aspect;
		task->thread.dexcr_override |= aspect;
		break;
	case PR_PPC_DEXCR_FORCE_SET_ASPECT:
		task->thread.dexcr_mask |= aspect;
		task->thread.dexcr_override |= aspect;
		task->thread.dexcr_forced |= aspect;
		break;
	case PR_PPC_DEXCR_CLEAR_ASPECT:
		task->thread.dexcr_mask |= aspect;
		task->thread.dexcr_override &= ~aspect;
		break;
	default:
		return -ERANGE;
	}

	return 0;
}

int dexcr_prctl_set(struct task_struct *task, unsigned long which, unsigned long ctrl)
{
	int err = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	switch (which) {
	case PR_PPC_DEXCR_SBHE:
		if (!cpu_has_feature(CPU_FTR_DEXCR_SBHE))
			return -ENODEV;
		err = dexcr_aspect_set(task, DEXCR_PRO_SBHE, ctrl);
		break;
	case PR_PPC_DEXCR_IBRTPD:
		if (!cpu_has_feature(CPU_FTR_DEXCR_IBRTPD))
			return -ENODEV;
		err = dexcr_aspect_set(task, DEXCR_PRO_IBRTPD, ctrl);
		break;
	case PR_PPC_DEXCR_SRAPD:
		if (!cpu_has_feature(CPU_FTR_DEXCR_SRAPD))
			return -ENODEV;
		err = dexcr_aspect_set(task, DEXCR_PRO_SRAPD, ctrl);
		break;
	case PR_PPC_DEXCR_NPHIE:
		if (!cpu_has_feature(CPU_FTR_DEXCR_NPHIE))
			return -ENODEV;
		err = dexcr_aspect_set(task, DEXCR_PRO_NPHIE, ctrl);
		break;
	default:
		return -ENODEV;
	}

	if (err)
		return err;

	update_dexcr_on_cpu(NULL);

	return 0;
}

#ifdef CONFIG_SYSCTL

static const int min_sysctl_val = -1;

static int sysctl_dexcr_sbhe_handler(struct ctl_table *table, int write,
				     void *buf, size_t *lenp, loff_t *ppos)
{
	int err;
	int prev = spec_branch_hint_enable;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!cpu_has_feature(CPU_FTR_DEXCR_SBHE))
		return -ENODEV;

	err = proc_dointvec_minmax(table, write, buf, lenp, ppos);
	if (err)
		return err;

	if (prev != spec_branch_hint_enable && write) {
		update_userspace_system_dexcr(DEXCR_PRO_SBHE, spec_branch_hint_enable);
		cpus_read_lock();
		on_each_cpu(update_dexcr_on_cpu, NULL, 1);
		cpus_read_unlock();
	}

	return 0;
}

static struct ctl_table dexcr_sbhe_ctl_table[] = {
	{
		.procname	= "speculative_branch_hint_enable",
		.data		= &spec_branch_hint_enable,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= sysctl_dexcr_sbhe_handler,
		.extra1		= (void *)&min_sysctl_val,
		.extra2		= SYSCTL_ONE,
	},
	{}
};

static struct ctl_table dexcr_sbhe_ctl_root[] = {
	{
		.procname	= "kernel",
		.mode		= 0555,
		.child		= dexcr_sbhe_ctl_table,
	},
	{}
};

static int __init register_dexcr_aspects_sysctl(void)
{
	register_sysctl_table(dexcr_sbhe_ctl_root);
	return 0;
}
device_initcall(register_dexcr_aspects_sysctl);

#endif /* CONFIG_SYSCTL */
