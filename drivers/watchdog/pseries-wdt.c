// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2022 International Business Machines, Inc.
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define DRV_NAME "pseries-wdt"

/*
 * The PAPR's MSB->LSB bit ordering is 0->63.  These macros simplify
 * defining bitfields as described in the PAPR without needing to
 * transpose values to the more C-like 63->0 ordering.
 */
#define SETFIELD(_v, _b, _e)	\
	(((unsigned long)(_v) << PPC_BITLSHIFT(_e)) & PPC_BITMASK((_b), (_e)))
#define GETFIELD(_v, _b, _e)	\
	(((unsigned long)(_v) & PPC_BITMASK((_b), (_e))) >> PPC_BITLSHIFT(_e))

/*
 * H_WATCHDOG Hypercall Input
 *
 * R4: "flags":
 *
 *     A 64-bit value structured as follows:
 *
 *         Bits 0-46: Reserved (must be zero).
 */
#define PSERIES_WDTF_RESERVED	PPC_BITMASK(0, 46)

/*
 *         Bit 47: "leaveOtherWatchdogsRunningOnTimeout"
 *
 *             0  Stop outstanding watchdogs on timeout.
 *             1  Leave outstanding watchdogs running on timeout.
 */
#define PSERIES_WDTF_LEAVE_OTHER	PPC_BIT(47)

/*
 *         Bits 48-55: "operation"
 *
 *             0x01  Start Watchdog
 *             0x02  Stop Watchdog
 *             0x03  Query Watchdog Capabilities
 *             0x04  Query Watchdog LPM Requirement
 */
#define PSERIES_WDTF_OP(op)		SETFIELD((op), 48, 55)
#define PSERIES_WDTF_OP_START		PSERIES_WDTF_OP(0x1)
#define PSERIES_WDTF_OP_STOP		PSERIES_WDTF_OP(0x2)
#define PSERIES_WDTF_OP_QUERY		PSERIES_WDTF_OP(0x3)
#define PSERIES_WDTF_OP_QUERY_LPM	PSERIES_WDTF_OP(0x4)

/*
 *         Bits 56-63: "timeoutAction"
 *
 *             0x01  Hard poweroff
 *             0x02  Hard restart
 *             0x03  Dump restart
 */
#define PSERIES_WDTF_ACTION(ac)			SETFIELD(ac, 56, 63)
#define PSERIES_WDTF_ACTION_HARD_POWEROFF	PSERIES_WDTF_ACTION(0x1)
#define PSERIES_WDTF_ACTION_HARD_RESTART	PSERIES_WDTF_ACTION(0x2)
#define PSERIES_WDTF_ACTION_DUMP_RESTART	PSERIES_WDTF_ACTION(0x3)

/*
 * R5: "watchdogNumber":
 *
 *     The target watchdog.  Watchdog numbers are 1-based.  The
 *     maximum supported watchdog number may be obtained via the
 *     "Query Watchdog Capabilities" operation.
 *
 *     This input is ignored for the "Query Watchdog Capabilities"
 *     operation.
 *
 * R6: "timeoutInMs":
 *
 *     The timeout in milliseconds.  The minimum supported timeout may
 *     be obtained via the "Query Watchdog Capabilities" operation.
 *
 *     This input is ignored for the "Stop Watchdog", "Query Watchdog
 *     Capabilities", and "Query Watchdog LPM Requirement" operations.
 */

/*
 * H_WATCHDOG Hypercall Output
 *
 * R3: Return code
 *
 *     H_SUCCESS    The operation completed.
 *
 *     H_BUSY	    The hypervisor is too busy; retry the operation.
 *
 *     H_PARAMETER  The given "flags" are somehow invalid.  Either the
 *                  "operation" or "timeoutAction" is invalid, or a
 *                  reserved bit is set.
 *
 *     H_P2         The given "watchdogNumber" is zero or exceeds the
 *                  supported maximum value.
 *
 *     H_P3         The given "timeoutInMs" is below the supported
 *                  minimum value.
 *
 *     H_NOOP       The given "watchdogNumber" is already stopped.
 *
 *     H_HARDWARE   The operation failed for ineffable reasons.
 *
 *     H_FUNCTION   The H_WATCHDOG hypercall is not supported by this
 *                  hypervisor.
 *
 * R4:
 *
 * - For the "Query Watchdog Capabilities" operation, a 64-bit
 *   value structured as follows:
 *
 *       Bits  0-15: The minimum supported timeout in milliseconds.
 *       Bits 16-31: The number of watchdogs supported.
 *       Bits 32-63: Reserved.
 */
#define PSERIES_WDTQ_MIN_TIMEOUT(cap)	GETFIELD((cap), 0, 15)
#define PSERIES_WDTQ_MAX_NUMBER(cap)	GETFIELD((cap), 16, 31)
#define PSERIES_WDTQ_RESERVED		PPC_BITMASK(32, 63)

/*
 * - For the "Query Watchdog LPM Requirement" operation:
 *
 *       1  The given "watchdogNumber" must be stopped prior to
 *          suspending.
 *
 *       2  The given "watchdogNumber" does not have to be stopped
 *          prior to suspending.
 */
#define PSERIES_WDTQL_MUST_STOP		1
#define PSERIES_WDTQL_NEED_NOT_STOP	2

static unsigned long action = PSERIES_WDTF_ACTION_HARD_RESTART;

static int action_get(char *buf, const struct kernel_param *kp)
{
	int val;

	switch (action) {
	case PSERIES_WDTF_ACTION_HARD_POWEROFF:
		val = 1;
		break;
	case PSERIES_WDTF_ACTION_HARD_RESTART:
		val = 2;
		break;
	case PSERIES_WDTF_ACTION_DUMP_RESTART:
		val = 3;
		break;
	default:
		return -EINVAL;
	}
	return sprintf(buf, "%d\n", val);
}

static int action_set(const char *val, const struct kernel_param *kp)
{
	int choice;

	if (kstrtoint(val, 10, &choice))
		return -EINVAL;
	switch (choice) {
	case 1:
		action = PSERIES_WDTF_ACTION_HARD_POWEROFF;
		return 0;
	case 2:
		action = PSERIES_WDTF_ACTION_HARD_RESTART;
		return 0;
	case 3:
		action = PSERIES_WDTF_ACTION_DUMP_RESTART;
		return 0;
	}
	return -EINVAL;
}

static const struct kernel_param_ops action_ops = {
	.get = action_get,
	.set = action_set,
};
module_param_cb(action, &action_ops, NULL, 0444);
MODULE_PARM_DESC(action, "Action taken when watchdog expires (default=2)");

static bool nowayout = WATCHDOG_NOWAYOUT;
module_param(nowayout, bool, 0444);
MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default="
		 __MODULE_STRING(WATCHDOG_NOWAYOUT) ")");

#define WATCHDOG_TIMEOUT 60
static unsigned int timeout = WATCHDOG_TIMEOUT;
module_param(timeout, uint, 0444);
MODULE_PARM_DESC(timeout, "Initial watchdog timeout in seconds (default="
		 __MODULE_STRING(WATCHDOG_TIMEOUT) ")");

struct pseries_wdt {
	struct watchdog_device wd;
	unsigned long num;		/* Watchdog numbers are 1-based */
};

static int pseries_wdt_start(struct watchdog_device *wdd)
{
	struct device *dev = wdd->parent;
	struct pseries_wdt *pw = watchdog_get_drvdata(wdd);
	unsigned long flags, msecs;
	long rc;

	flags = action | PSERIES_WDTF_OP_START;
	msecs = wdd->timeout * 1000UL;
	rc = plpar_hcall_norets(H_WATCHDOG, flags, pw->num, msecs);
	if (rc != H_SUCCESS) {
		dev_crit(dev, "H_WATCHDOG: %ld: failed to start timer %lu",
			 rc, pw->num);
		return -EIO;
	}
	return 0;
}

static int pseries_wdt_stop(struct watchdog_device *wdd)
{
	struct device *dev = wdd->parent;
	struct pseries_wdt *pw = watchdog_get_drvdata(wdd);
	long rc;

	rc = plpar_hcall_norets(H_WATCHDOG, PSERIES_WDTF_OP_STOP, pw->num);
	if (rc != H_SUCCESS && rc != H_NOOP) {
		dev_crit(dev, "H_WATCHDOG: %ld: failed to stop timer %lu",
			 rc, pw->num);
		return -EIO;
	}
	return 0;
}

static struct watchdog_info pseries_wdt_info = {
	.identity = DRV_NAME,
	.options = WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE | WDIOF_SETTIMEOUT
	    | WDIOF_PRETIMEOUT,
};

static const struct watchdog_ops pseries_wdt_ops = {
	.owner = THIS_MODULE,
	.start = pseries_wdt_start,
	.stop = pseries_wdt_stop,
};

static int pseries_wdt_probe(struct platform_device *pdev)
{
	unsigned long ret[PLPAR_HCALL_BUFSIZE] = { 0 };
	unsigned long cap, min_timeout_ms;
	long rc;
	struct pseries_wdt *pw;
	int err;

	rc = plpar_hcall(H_WATCHDOG, ret, PSERIES_WDTF_OP_QUERY);
	if (rc != H_SUCCESS)
		return rc == H_FUNCTION ? -ENODEV : -EIO;
	cap = ret[0];

	pw = devm_kzalloc(&pdev->dev, sizeof(*pw), GFP_KERNEL);
	if (!pw)
		return -ENOMEM;

	/*
	 * Assume watchdogNumber 1 for now.  If we ever support
	 * multiple timers we will need to devise a way to choose a
	 * distinct watchdogNumber for each platform device at device
	 * registration time.
	 */
	pw->num = 1;

	pw->wd.parent = &pdev->dev;
	pw->wd.info = &pseries_wdt_info;
	pw->wd.ops = &pseries_wdt_ops;
	min_timeout_ms = PSERIES_WDTQ_MIN_TIMEOUT(cap);
	pw->wd.min_timeout = roundup(min_timeout_ms, 1000) / 1000;
	pw->wd.max_timeout = UINT_MAX;
	watchdog_init_timeout(&pw->wd, timeout, NULL);
	watchdog_set_nowayout(&pw->wd, nowayout);
	watchdog_stop_on_reboot(&pw->wd);
	watchdog_stop_on_unregister(&pw->wd);
	watchdog_set_drvdata(&pw->wd, pw);

	err = devm_watchdog_register_device(&pdev->dev, &pw->wd);
	if (err)
		return err;

	platform_set_drvdata(pdev, &pw->wd);

	return 0;
}

static int pseries_wdt_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct watchdog_device *wd = platform_get_drvdata(pdev);

	if (watchdog_active(wd))
		return pseries_wdt_stop(wd);
	return 0;
}

static int pseries_wdt_resume(struct platform_device *pdev)
{
	struct watchdog_device *wd = platform_get_drvdata(pdev);

	if (watchdog_active(wd))
		return pseries_wdt_start(wd);
	return 0;
}

static const struct platform_device_id pseries_wdt_id[] = {
	{ .name = "pseries-wdt" },
	{}
};
MODULE_DEVICE_TABLE(platform, pseries_wdt_id);

static struct platform_driver pseries_wdt_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
	},
	.id_table = pseries_wdt_id,
	.probe = pseries_wdt_probe,
	.resume = pseries_wdt_resume,
	.suspend = pseries_wdt_suspend,
};
module_platform_driver(pseries_wdt_driver);

MODULE_AUTHOR("Alexey Kardashevskiy <aik@ozlabs.ru>");
MODULE_AUTHOR("Scott Cheloha <cheloha@linux.ibm.com>");
MODULE_DESCRIPTION("POWER Architecture Platform Watchdog Driver");
MODULE_LICENSE("GPL");
