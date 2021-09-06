// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright IBM Corp. 2012
 *
 *  Author(s):
 *    Jan Glauber <jang@linux.vnet.ibm.com>
 */

#define KMSG_COMPONENT "zpci"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/kernel.h>
#include <linux/pci.h>
#include <asm/pci_debug.h>
#include <asm/pci_dma.h>
#include <asm/sclp.h>

#include "pci_bus.h"

/* Content Code Description for PCI Function Error */
struct zpci_ccdf_err {
	u32 reserved1;
	u32 fh;				/* function handle */
	u32 fid;			/* function id */
	u32 ett		:  4;		/* expected table type */
	u32 mvn		: 12;		/* MSI vector number */
	u32 dmaas	:  8;		/* DMA address space */
	u32		:  6;
	u32 q		:  1;		/* event qualifier */
	u32 rw		:  1;		/* read/write */
	u64 faddr;			/* failing address */
	u32 reserved3;
	u16 reserved4;
	u16 pec;			/* PCI event code */
} __packed;

/* Content Code Description for PCI Function Availability */
struct zpci_ccdf_avail {
	u32 reserved1;
	u32 fh;				/* function handle */
	u32 fid;			/* function id */
	u32 reserved2;
	u32 reserved3;
	u32 reserved4;
	u32 reserved5;
	u16 reserved6;
	u16 pec;			/* PCI event code */
} __packed;

static inline bool ers_result_indicates_abort(pci_ers_result_t ers_res)
{
	switch (ers_res) {
	case PCI_ERS_RESULT_CAN_RECOVER:
	case PCI_ERS_RESULT_RECOVERED:
	case PCI_ERS_RESULT_NEED_RESET:
		return false;
	default:
		return true;
	}
}

static pci_ers_result_t zpci_event_notify_error_detected(struct pci_dev *pdev)
{
	pci_ers_result_t ers_res = PCI_ERS_RESULT_DISCONNECT;
	struct pci_driver *driver = pdev->driver;

	pr_debug("%s: calling error_detected() callback\n", pci_name(pdev));
	ers_res = driver->err_handler->error_detected(pdev,  pdev->error_state);
	if (ers_result_indicates_abort(ers_res))
		pr_info("%s: driver can't recover\n", pci_name(pdev));
	else if (ers_res == PCI_ERS_RESULT_NEED_RESET)
		pr_debug("%s: driver needs reset to recover\n", pci_name(pdev));

	return ers_res;
}

static pci_ers_result_t zpci_event_do_error_state_clear(struct pci_dev *pdev)
{
	pci_ers_result_t ers_res = PCI_ERS_RESULT_DISCONNECT;
	struct pci_driver *driver = pdev->driver;
	struct zpci_dev *zdev = to_zpci(pdev);
	int rc;

	pr_debug("%s: reset load/store blocked\n", pci_name(pdev));
	rc = zpci_reset_load_store_blocked(zdev);
	if (rc) {
		pr_err("%s: reset load/store blocked failed\n", pci_name(pdev));
		/* Let's try a full reset instead */
		return PCI_ERS_RESULT_NEED_RESET;
	}

	if (driver->err_handler->mmio_enabled) {
		pr_debug("%s: calling mmio_enabled() callback\n", pci_name(pdev));
		ers_res = driver->err_handler->mmio_enabled(pdev);
		if (ers_result_indicates_abort(ers_res)) {
			pr_info("%s: driver can't recover after enabling MMIO\n", pci_name(pdev));
			return ers_res;
		} else if (ers_res == PCI_ERS_RESULT_NEED_RESET) {
			pr_debug("%s: driver needs reset to recover\n", pci_name(pdev));
			return ers_res;
		}
	}

	pr_debug("%s: clearing error state\n", pci_name(pdev));
	rc = zpci_clear_error_state(zdev);
	if (!rc) {
		pdev->error_state = pci_channel_io_normal;
	} else {
		pr_err("%s: resetting error state failed\n", pci_name(pdev));
		/* Let's try a full reset instead */
		return PCI_ERS_RESULT_NEED_RESET;
	}

	return ers_res;
}

static pci_ers_result_t zpci_event_do_reset(struct pci_dev *pdev)
{
	pci_ers_result_t ers_res = PCI_ERS_RESULT_DISCONNECT;
	struct pci_driver *driver = pdev->driver;

	pr_info("%s: initiating reset\n", pci_name(pdev));
	if (zpci_hot_reset_device(to_zpci(pdev))) {
		pr_err("%s: resetting function failed\n", pci_name(pdev));
		return ers_res;
	}
	pdev->error_state = pci_channel_io_normal;
	if (driver->err_handler->slot_reset) {
		ers_res = driver->err_handler->slot_reset(pdev);
		if (ers_result_indicates_abort(ers_res)) {
			pr_info("%s: driver can't recover after slot reset\n", pci_name(pdev));
			return ers_res;
		}
	}

	return ers_res;
}

/* zpci_event_attempt_error_recovery - Try to recover the given PCI function
 * @pdev: PCI function to recover currently in the error state
 *
 * We follow the scheme outlined in Documentation/PCI/pci-error-recovery.rst.
 * With the simplification that recovery always happens per function
 * and the platform determines which functions are affected for
 * multi-function devices.
 */
static pci_ers_result_t zpci_event_attempt_error_recovery(struct pci_dev *pdev)
{
	pci_ers_result_t ers_res = PCI_ERS_RESULT_DISCONNECT;
	struct pci_driver *driver;

	/*
	 * Ensure that the PCI function is not removed concurrently, no driver
	 * is unbound or probed and that userspace can't access its
	 * configuration space while we perform recovery.
	 */
	pci_dev_lock(pdev);
	/*
	 * Between getting the pdev and locking it the PCI device may have been
	 * removed e.g. by a concurrent call to recover_store().
	 */
	if (!pci_dev_is_added(pdev))
		goto out_unlock;
	if (pdev->error_state == pci_channel_io_perm_failure) {
		ers_res = PCI_ERS_RESULT_DISCONNECT;
		goto out_unlock;
	}
	pdev->error_state = pci_channel_io_frozen;

	driver = pdev->driver;
	if (!driver || !driver->err_handler || !driver->err_handler->error_detected) {
		pr_info("%s: driver does not support error recovery\n", pci_name(pdev));
		goto out_unlock;
	}

	ers_res = zpci_event_notify_error_detected(pdev);
	if (ers_result_indicates_abort(ers_res))
		goto out_unlock;

	if (ers_res == PCI_ERS_RESULT_CAN_RECOVER) {
		ers_res = zpci_event_do_error_state_clear(pdev);
		if (ers_result_indicates_abort(ers_res))
			goto out_unlock;
	}

	if (ers_res == PCI_ERS_RESULT_NEED_RESET)
		ers_res = zpci_event_do_reset(pdev);

	if (ers_res != PCI_ERS_RESULT_RECOVERED) {
		pr_err("%s: recovery failed\n", pci_name(pdev));
		goto out_unlock;
	}

	pr_info("%s: resuming operations\n", pci_name(pdev));
	if (driver->err_handler->resume)
		driver->err_handler->resume(pdev);
out_unlock:
	pci_dev_unlock(pdev);

	return ers_res;
}

/* zpci_event_io_failure - Report IO failure state es to driver
 * @pdev: PCI function to report as failed
 */
static void zpci_event_io_failure(struct pci_dev *pdev, pci_channel_state_t es)
{
	struct pci_driver *driver;

	pci_dev_lock(pdev);
	if (!pci_dev_is_added(pdev))
		goto out_unlock;
	pdev->error_state = es;
	driver = pdev->driver;
	if (driver && driver->err_handler && driver->err_handler->error_detected)
		driver->err_handler->error_detected(pdev, pdev->error_state);
out_unlock:
	pci_dev_unlock(pdev);
}

static void __zpci_event_error(struct zpci_ccdf_err *ccdf)
{
	struct zpci_dev *zdev = get_zdev_by_fid(ccdf->fid);
	struct pci_dev *pdev = NULL;
	pci_ers_result_t ers_res;

	zpci_err("error CCDF:\n");
	zpci_err_hex(ccdf, sizeof(*ccdf));

	if (zdev)
		zpci_update_fh(zdev, ccdf->fh);

	if (zdev->zbus->bus)
		pdev = pci_get_slot(zdev->zbus->bus, zdev->devfn);

	pr_err("%s: Event 0x%x reports an error for PCI function 0x%x\n",
	       pdev ? pci_name(pdev) : "n/a", ccdf->pec, ccdf->fid);

	if (!pdev)
		return;

	switch (ccdf->pec) {
	case 0x003a: /* Service Action or Error Recovery Successful */
		ers_res = zpci_event_attempt_error_recovery(pdev);
		if (ers_res != PCI_ERS_RESULT_RECOVERED)
			zpci_event_io_failure(pdev, pci_channel_io_perm_failure);
		break;
	default:
		/*
		 * Mark as frozen not permanently failed because the device
		 * could be subsequently recovered by the platform.
		 */
		zpci_event_io_failure(pdev, pci_channel_io_frozen);
		break;
	}
	pci_dev_put(pdev);
}

void zpci_event_error(void *data)
{
	if (zpci_is_enabled())
		__zpci_event_error(data);
}

static void zpci_event_hard_deconfigured(struct zpci_dev *zdev, u32 fh)
{
	zpci_update_fh(zdev, fh);
	/* Give the driver a hint that the function is
	 * already unusable.
	 */
	zpci_bus_remove_device(zdev, true);
	/* Even though the device is already gone we still
	 * need to free zPCI resources as part of the disable.
	 */
	if (zdev->dma_table)
		zpci_dma_exit_device(zdev);
	if (zdev_enabled(zdev))
		zpci_disable_device(zdev);
	zdev->state = ZPCI_FN_STATE_STANDBY;
}

static void __zpci_event_availability(struct zpci_ccdf_avail *ccdf)
{
	struct zpci_dev *zdev = get_zdev_by_fid(ccdf->fid);
	enum zpci_state state;

	zpci_err("avail CCDF:\n");
	zpci_err_hex(ccdf, sizeof(*ccdf));

	switch (ccdf->pec) {
	case 0x0301: /* Reserved|Standby -> Configured */
		if (!zdev) {
			zdev = zpci_create_device(ccdf->fid, ccdf->fh, ZPCI_FN_STATE_CONFIGURED);
			if (IS_ERR(zdev))
				break;
		} else {
			/* the configuration request may be stale */
			if (zdev->state != ZPCI_FN_STATE_STANDBY)
				break;
			zdev->state = ZPCI_FN_STATE_CONFIGURED;
		}
		zpci_scan_configured_device(zdev, ccdf->fh);
		break;
	case 0x0302: /* Reserved -> Standby */
		if (!zdev)
			zpci_create_device(ccdf->fid, ccdf->fh, ZPCI_FN_STATE_STANDBY);
		else
			zpci_update_fh(zdev, ccdf->fh);
		break;
	case 0x0303: /* Deconfiguration requested */
		if (zdev) {
			/* The event may have been queued before we confirgured
			 * the device.
			 */
			if (zdev->state != ZPCI_FN_STATE_CONFIGURED)
				break;
			zpci_update_fh(zdev, ccdf->fh);
			zpci_deconfigure_device(zdev);
		}
		break;
	case 0x0304: /* Configured -> Standby|Reserved */
		if (zdev) {
			/* The event may have been queued before we confirgured
			 * the device.:
			 */
			if (zdev->state == ZPCI_FN_STATE_CONFIGURED)
				zpci_event_hard_deconfigured(zdev, ccdf->fh);
			/* The 0x0304 event may immediately reserve the device */
			if (!clp_get_state(zdev->fid, &state) &&
			    state == ZPCI_FN_STATE_RESERVED) {
				zpci_zdev_put(zdev);
			}
		}
		break;
	case 0x0306: /* 0x308 or 0x302 for multiple devices */
		zpci_remove_reserved_devices();
		clp_scan_pci_devices();
		break;
	case 0x0308: /* Standby -> Reserved */
		if (!zdev)
			break;
		zpci_zdev_put(zdev);
		break;
	default:
		break;
	}
}

void zpci_event_availability(void *data)
{
	if (zpci_is_enabled())
		__zpci_event_availability(data);
}
