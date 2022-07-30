// SPDX-License-Identifier: GPL-2.0-only
/*
 * Character device for accessing pseries/PAPR platform-specific
 * facilities.
 */
#define pr_fmt(fmt) "lparctl: " fmt

#include <linux/build_bug.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/lparctl.h>
#include <asm/machdep.h>
#include <asm/rtas.h>

/**
 * lparctl_get_sysparm() - Query a PAPR system parameter.
 *
 * Retrieve the value of the parameter indicated by the @token member of
 * the &struct lparctl_get_system_parameter at @arg. If available and
 * accessible, the value of the parameter is copied to the @data member of
 * the &struct lparctl_get_system_parameter at @arg, and its @rtas_status
 * field is set to zero. Otherwise, the @rtas_status member reflects the
 * most recent RTAS call status, and the contents of @data are
 * indeterminate.
 *
 * Non-zero RTAS call statuses are not translated to conventional errno
 * values. Only kernel issues or API misuse result in an error at the
 * syscall level. This is to serve the needs of legacy software which
 * historically has accessed system parameters via the rtas() syscall,
 * which has similar behavior.
 *
 * Return:
 * * 0 - OK. Caller must examine the @rtas_status member of the returned
 *       &struct lparctl_get_system_parameter to determine whether a parameter
 *       value was copied out.
 * * -EINVAL - The copied-in &struct lparctl_get_system_parameter.rtas_status
 *             is non-zero.
 * * -EFAULT - The supplied @arg is a bad address.
 * * -ENOMEM - Allocation failure.
 */
static long lparctl_get_sysparm(struct lparctl_get_system_parameter __user *argp)
{
	struct lparctl_get_system_parameter *gsp;
	long ret;
	int fwrc;

	/*
	 * Special case to allow user space to probe the command.
	 */
	if (argp == NULL)
		return 0;

	gsp = memdup_user(argp, sizeof(*gsp));
	if (IS_ERR(gsp)) {
		ret = PTR_ERR(gsp);
		goto err_return;
	}

	ret = -EINVAL;
	if (gsp->rtas_status != 0)
		goto err_free;

	do {
		static_assert(sizeof(gsp->data) <= sizeof(rtas_data_buf));

		spin_lock(&rtas_data_buf_lock);
		memset(rtas_data_buf, 0, sizeof(rtas_data_buf));
		memcpy(rtas_data_buf, gsp->data, sizeof(gsp->data));
		fwrc = rtas_call(rtas_token("ibm,get-system-parameter"), 3, 1,
				 NULL, gsp->token, __pa(rtas_data_buf),
				 sizeof(gsp->data));
		if (fwrc == 0)
			memcpy(gsp->data, rtas_data_buf, sizeof(gsp->data));
		spin_unlock(&rtas_data_buf_lock);
	} while (rtas_busy_delay(fwrc));

	gsp->rtas_status = fwrc;
	ret = 0;
	if (copy_to_user(argp, gsp, sizeof(*gsp)))
		ret = -EFAULT;
err_free:
	kfree(gsp);
err_return:
	return ret;
}

static long lparctl_set_sysparm(struct lparctl_set_system_parameter __user *argp)
{
	struct lparctl_set_system_parameter *ssp;
	long ret;
	int fwrc;

	/*
	 * Special case to allow user space to probe the command.
	 */
	if (argp == NULL)
		return 0;

	ssp = memdup_user(argp, sizeof(*ssp));
	if (IS_ERR(ssp)) {
		ret = PTR_ERR(ssp);
		goto err_return;
	}

	ret = -EINVAL;
	if (ssp->rtas_status != 0)
		goto err_free;

	do {
		static_assert(sizeof(ssp->data) <= sizeof(rtas_data_buf));

		spin_lock(&rtas_data_buf_lock);
		memset(rtas_data_buf, 0, sizeof(rtas_data_buf));
		memcpy(rtas_data_buf, ssp->data, sizeof(ssp->data));
		fwrc = rtas_call(rtas_token("ibm,set-system-parameter"), 2, 1,
				 NULL, ssp->token, __pa(rtas_data_buf));
		spin_unlock(&rtas_data_buf_lock);
	} while (rtas_busy_delay(fwrc));

	ret = 0;
	if (copy_to_user(&argp->rtas_status, &fwrc, sizeof(argp->rtas_status)))
		ret = -EFAULT;
err_free:
	kfree(ssp);
err_return:
	return ret;
}

static long lparctl_dev_ioctl(struct file *filp, unsigned int ioctl, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	long ret;

	switch (ioctl) {
	case LPARCTL_GET_SYSPARM:
		ret = lparctl_get_sysparm(argp);
		break;
	case LPARCTL_SET_SYSPARM:
		ret = lparctl_set_sysparm(argp);
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

static const struct file_operations lparctl_ops = {
	.unlocked_ioctl = lparctl_dev_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice lparctl_dev = {
	MISC_DYNAMIC_MINOR,
	"lparctl",
	&lparctl_ops,
};

static __init int lparctl_init(void)
{
	int ret;

	ret = misc_register(&lparctl_dev);

	return ret;
}
machine_device_initcall(pseries, lparctl_init);
