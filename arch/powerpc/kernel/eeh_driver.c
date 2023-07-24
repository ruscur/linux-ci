// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PCI Error Recovery Driver for RPA-compliant PPC64 platform.
 * Copyright IBM Corp. 2004 2005
 * Copyright Linas Vepstas <linas@linas.org> 2004, 2005
 *
 * Send comments and feedback to Linas Vepstas <linas@austin.ibm.com>
 */
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <asm/eeh.h>
#include <asm/eeh_event.h>
#include <asm/ppc-pci.h>
#include <asm/pci-bridge.h>
#include <asm/rtas.h>

static atomic_t eeh_wu_id = ATOMIC_INIT(0);

struct eeh_rmv_data {
	struct list_head removed_vf_list;
	int removed_dev_count;
};

static int eeh_result_priority(enum pci_ers_result result)
{
	switch (result) {
	case PCI_ERS_RESULT_NONE:
		return 1;
	case PCI_ERS_RESULT_NO_AER_DRIVER:
		return 2;
	case PCI_ERS_RESULT_RECOVERED:
		return 3;
	case PCI_ERS_RESULT_CAN_RECOVER:
		return 4;
	case PCI_ERS_RESULT_DISCONNECT:
		return 5;
	case PCI_ERS_RESULT_NEED_RESET:
		return 6;
	default:
		WARN_ONCE(1, "Unknown pci_ers_result value: %d\n", (int)result);
		return 0;
	}
};

static const char *pci_ers_result_name(enum pci_ers_result result)
{
	switch (result) {
	case PCI_ERS_RESULT_NONE:
		return "none";
	case PCI_ERS_RESULT_CAN_RECOVER:
		return "can recover";
	case PCI_ERS_RESULT_NEED_RESET:
		return "need reset";
	case PCI_ERS_RESULT_DISCONNECT:
		return "disconnect";
	case PCI_ERS_RESULT_RECOVERED:
		return "recovered";
	case PCI_ERS_RESULT_NO_AER_DRIVER:
		return "no AER driver";
	default:
		WARN_ONCE(1, "Unknown result type: %d\n", (int)result);
		return "unknown";
	}
};

static enum pci_ers_result pci_ers_merge_result(enum pci_ers_result old,
						enum pci_ers_result new)
{
	if (eeh_result_priority(new) > eeh_result_priority(old))
		return new;
	return old;
}

static bool eeh_dev_removed(struct eeh_dev *edev)
{
	return !edev || (edev->mode & EEH_DEV_REMOVED);
}

static bool eeh_edev_actionable(struct eeh_dev *edev)
{
	if (!edev->pdev)
		return false;
	if (edev->pdev->error_state == pci_channel_io_perm_failure)
		return false;
	if (eeh_dev_removed(edev))
		return false;
	if (eeh_pe_passed(edev->pe))
		return false;

	return true;
}

/**
 * eeh_pcid_get - Get the PCI device driver
 * @pdev: PCI device
 *
 * The function is used to retrieve the PCI device driver for
 * the indicated PCI device. Besides, we will increase the reference
 * of the PCI device driver to prevent that being unloaded on
 * the fly. Otherwise, kernel crash would be seen.
 */
static inline struct pci_driver *eeh_pcid_get(struct pci_dev *pdev)
{
	if (!pdev || !pdev->dev.driver)
		return NULL;

	if (!try_module_get(pdev->dev.driver->owner))
		return NULL;

	return to_pci_driver(pdev->dev.driver);
}

/**
 * eeh_pcid_put - Dereference on the PCI device driver
 * @pdev: PCI device
 *
 * The function is called to do dereference on the PCI device
 * driver of the indicated PCI device.
 */
static inline void eeh_pcid_put(struct pci_dev *pdev)
{
	if (!pdev || !pdev->dev.driver)
		return;

	module_put(pdev->dev.driver->owner);
}

/**
 * eeh_disable_irq - Disable interrupt for the recovering device
 * @dev: PCI device
 *
 * This routine must be called when reporting temporary or permanent
 * error to the particular PCI device to disable interrupt of that
 * device. If the device has enabled MSI or MSI-X interrupt, we needn't
 * do real work because EEH should freeze DMA transfers for those PCI
 * devices encountering EEH errors, which includes MSI or MSI-X.
 */
static void eeh_disable_irq(struct eeh_dev *edev)
{
	/* Don't disable MSI and MSI-X interrupts. They are
	 * effectively disabled by the DMA Stopped state
	 * when an EEH error occurs.
	 */
	if (edev->pdev->msi_enabled || edev->pdev->msix_enabled)
		return;

	if (!irq_has_action(edev->pdev->irq))
		return;

	edev->mode |= EEH_DEV_IRQ_DISABLED;
	disable_irq_nosync(edev->pdev->irq);
}

/**
 * eeh_enable_irq - Enable interrupt for the recovering device
 * @dev: PCI device
 *
 * This routine must be called to enable interrupt while failed
 * device could be resumed.
 */
static void eeh_enable_irq(struct eeh_dev *edev)
{
	if ((edev->mode) & EEH_DEV_IRQ_DISABLED) {
		edev->mode &= ~EEH_DEV_IRQ_DISABLED;
		/*
		 * FIXME !!!!!
		 *
		 * This is just ass backwards. This maze has
		 * unbalanced irq_enable/disable calls. So instead of
		 * finding the root cause it works around the warning
		 * in the irq_enable code by conditionally calling
		 * into it.
		 *
		 * That's just wrong.The warning in the core code is
		 * there to tell people to fix their asymmetries in
		 * their own code, not by abusing the core information
		 * to avoid it.
		 *
		 * I so wish that the assymetry would be the other way
		 * round and a few more irq_disable calls render that
		 * shit unusable forever.
		 *
		 *	tglx
		 */
		if (irqd_irq_disabled(irq_get_irq_data(edev->pdev->irq)))
			enable_irq(edev->pdev->irq);
	}
}

static void eeh_dev_save_state(struct eeh_dev *edev, void *userdata)
{
	struct pci_dev *pdev;

	if (!edev)
		return;

	/*
	 * We cannot access the config space on some adapters.
	 * Otherwise, it will cause fenced PHB. We don't save
	 * the content in their config space and will restore
	 * from the initial config space saved when the EEH
	 * device is created.
	 */
	if (edev->pe && (edev->pe->state & EEH_PE_CFG_RESTRICTED))
		return;

	pdev = eeh_dev_to_pci_dev(edev);
	if (!pdev)
		return;

	pci_save_state(pdev);
}

static void eeh_set_channel_state(struct eeh_pe *root, pci_channel_state_t s)
{
	struct eeh_pe *pe;
	struct eeh_dev *edev, *tmp;

	eeh_for_each_pe(root, pe)
		eeh_pe_for_each_dev(pe, edev, tmp)
			if (eeh_edev_actionable(edev))
				edev->pdev->error_state = s;
}

static void eeh_set_irq_state(struct eeh_pe *root, bool enable)
{
	struct eeh_pe *pe;
	struct eeh_dev *edev, *tmp;

	eeh_for_each_pe(root, pe) {
		eeh_pe_for_each_dev(pe, edev, tmp) {
			if (!eeh_edev_actionable(edev))
				continue;

			if (!eeh_pcid_get(edev->pdev))
				continue;

			if (enable)
				eeh_enable_irq(edev);
			else
				eeh_disable_irq(edev);

			eeh_pcid_put(edev->pdev);
		}
	}
}

typedef enum pci_ers_result (*eeh_report_fn)(unsigned int event_id,
					     unsigned int id,
					     struct pci_dev *,
					     struct pci_driver *);
static void eeh_pe_report_pdev(unsigned int event_id,
			       unsigned int id,
			       struct pci_dev *pdev,
			       const char *fn_name, eeh_report_fn fn,
			       enum pci_ers_result *result,
			       bool late, bool removed, bool passed)
{
	struct pci_driver *driver;
	enum pci_ers_result new_result;

	/*
	 * Driver callbacks may end up calling back into EEH functions
	 * (for example by removing a PCI device) which will deadlock
	 * unless the EEH locks are released first. Note that it may be
	 * re-acquired by the report functions, if necessary.
	 */
	device_lock(&pdev->dev);
	driver = eeh_pcid_get(pdev);

	if (!driver) {
		pci_info(pdev, "EEH(%u): W%u: no driver", event_id, id);
	} else if (!driver->err_handler) {
		pci_info(pdev, "EEH(%u): W%u: driver not EEH aware", event_id, id);
	} else if (late) {
		pci_info(pdev, "EEH(%u): W%u: driver bound too late", event_id, id);
	} else {
		pci_info(pdev, "EEH(%u): EVENT=HANDLER_CALL HANDLER='%s'\n",
			 event_id, fn_name);

		new_result = fn(event_id, id, pdev, driver);

		/*
		 * It's not safe to use edev here, because the locks
		 * have been released and devices could have changed.
		 */
		pr_warn("EEH(%u): EVENT=HANDLER_RETURN RESULT='%s'\n",
			event_id, pci_ers_result_name(new_result));
		pci_info(pdev, "EEH(%u): W%u: %s driver reports: '%s'",
			 event_id, id, driver->name,
			 pci_ers_result_name(new_result));
		if (result) {
			eeh_recovery_lock();
			*result = pci_ers_merge_result(*result,
						       new_result);
			eeh_recovery_unlock();
		}
	}
	if (driver)
		eeh_pcid_put(pdev);
	device_unlock(&pdev->dev);
}

struct pci_dev **pdev_cache_list_create(struct eeh_pe *root)
{
	struct eeh_pe *pe;
	struct eeh_dev *edev, *tmp;
	struct pci_dev **pdevs;
	int i, n;

	n = 0;
	eeh_for_each_pe(root, pe) eeh_pe_for_each_dev(pe, edev, tmp) {
		if (edev->pdev)
			n++;
	}
	pdevs = kmalloc(sizeof(*pdevs) * (n + 1), GFP_KERNEL);
	if (WARN_ON_ONCE(!pdevs))
		return NULL;
	i = 0;
	eeh_for_each_pe(root, pe) eeh_pe_for_each_dev(pe, edev, tmp) {
		if (i < n) {
			get_device(&edev->pdev->dev);
			pdevs[i++] = edev->pdev;
		}
	}
	if (WARN_ON_ONCE(i < n))
		n = i;
	pdevs[n] = NULL; /* terminator */
	return pdevs;
}

static void pdev_cache_list_destroy(struct pci_dev **pdevs)
{
	struct pci_dev **pdevp;

	for (pdevp = pdevs; pdevp && *pdevp; pdevp++)
		put_device(&(*pdevp)->dev);
	kfree(pdevs);
}

struct work_unit {
	unsigned int id;
	struct work_struct work;
	unsigned int event_id;
	struct pci_dev *pdev;
	struct eeh_pe *pe;
	const char *fn_name;
	eeh_report_fn fn;
	enum pci_ers_result *result;
	atomic_t *count;
	struct completion *done;
};

static void eeh_pe_report_pdev_thread(struct work_struct *work);
/*
 * Traverse down from a PE through it's children, to find devices and enqueue
 * jobs to call the handler (fn) on them.  But do not traverse below a PE that
 * has devices, so that devices are always handled strictly before their
 * children. (Traversal is continued by the jobs after handlers are called.)
 * The recovery lock must be held.
 * TODO: Convert away from recursive descent traversal?
 */
static bool enqueue_pe_work(struct eeh_pe *root, unsigned int event_id,
			    const char *fn_name, eeh_report_fn fn,
			    enum pci_ers_result *result, atomic_t *count,
			    struct completion *done)
{
	struct eeh_pe *pe;
	struct eeh_dev *edev, *tmp;
	struct work_unit *wu;
	bool work_added = false;

	if (list_empty(&root->edevs)) {
		list_for_each_entry(pe, &root->child_list, child)
			work_added |= enqueue_pe_work(pe, event_id, fn_name,
						      fn, result, count, done);
	} else {
		eeh_pe_for_each_dev(root, edev, tmp) {
			work_added = true;
			edev->mode |= EEH_DEV_RECOVERING;
			atomic_inc(count);
			WARN_ON(!(edev->mode & EEH_DEV_RECOVERING));
			wu = kmalloc(sizeof(*wu), GFP_KERNEL);
			wu->id = (unsigned int)atomic_inc_return(&eeh_wu_id);
			wu->event_id = event_id;
			get_device(&edev->pdev->dev);
			wu->pdev = edev->pdev;
			wu->pe = root;
			wu->fn_name = fn_name;
			wu->fn = fn;
			wu->result = result;
			wu->count = count;
			wu->done = done;
			INIT_WORK(&wu->work, eeh_pe_report_pdev_thread);
			pr_debug("EEH(%u): Queue work unit W%u for device %s (count ~ %d)\n",
				 event_id, wu->id, pci_name(edev->pdev),
				 atomic_read(count));
			queue_work(system_unbound_wq, &wu->work);
		}
		/* This PE has devices, so don't traverse further now */
	}
	return work_added;
}

static void eeh_pe_report_pdev_thread(struct work_struct *work)
{
	struct work_unit *wu = container_of(work, struct work_unit, work);
	struct eeh_dev *edev, *oedev, *tmp;
	struct eeh_pe *pe;
	int todo;

	/*
	 * It would be convenient to continue to hold the recovery lock here
	 * but driver callbacks can take a very long time or never return at
	 * all.
	 */
	pr_debug("EEH(%u): W%u: start (device: %s)\n", wu->event_id, wu->id, pci_name(wu->pdev));
	eeh_recovery_lock();
	edev = pci_dev_to_eeh_dev(wu->pdev);
	if (edev) {
		bool late, removed, passed;

		WARN_ON(!(edev->mode & EEH_DEV_RECOVERING));
		removed = eeh_dev_removed(edev);
		passed = eeh_pe_passed(edev->pe);
		late = edev->mode & EEH_DEV_NO_HANDLER;
		if (eeh_edev_actionable(edev)) {
			eeh_recovery_unlock();
			eeh_pe_report_pdev(wu->event_id, wu->id, wu->pdev,
					   wu->fn_name, wu->fn, wu->result,
					   late, removed, passed);
			eeh_recovery_lock();
		} else {
			pci_info(wu->pdev, "EEH(%u): W%u: Not actionable (%d,%d,%d)\n",
				 wu->event_id, wu->id, !!wu->pdev, !removed, !passed);
		}
		edev = pci_dev_to_eeh_dev(wu->pdev); // Re-acquire after lock release
		if (edev)
			edev->mode &= ~EEH_DEV_RECOVERING;
		/* The edev may be lost, but not moved to a different PE! */
		WARN_ON(eeh_dev_to_pe(edev) && (eeh_dev_to_pe(edev) != wu->pe));
		todo = 0;
		eeh_pe_for_each_dev(wu->pe, oedev, tmp)
			if (oedev->mode & EEH_DEV_RECOVERING)
				todo++;
		pci_dbg(wu->pdev, "EEH(%u): W%u: Remaining devices in this PE: %d\n",
			wu->event_id, wu->id, todo);
		if (todo) {
			pr_debug("EEH(%u): W%u: Remaining work units at this PE: %d\n",
				 wu->event_id, wu->id, todo);
		} else {
			pr_debug("EEH(%u): W%u: All work for this PE complete, continuing traversal:\n",
				 wu->event_id, wu->id);
			list_for_each_entry(pe, &wu->pe->child_list, child)
				enqueue_pe_work(pe, wu->event_id, wu->fn_name,
						wu->fn, wu->result, wu->count,
						wu->done);
		}
	} else {
		pr_warn("EEH(%u): W%u: Device removed.\n", wu->event_id, wu->id);
	}
	eeh_recovery_unlock();
	if (atomic_dec_and_test(wu->count)) {
		pr_debug("EEH(%u): W%u: done\n", wu->event_id, wu->id);
		complete(wu->done);
	}
	put_device(&wu->pdev->dev);
	kfree(wu);
}

static void eeh_pe_report(unsigned int event_id, const char *name, struct eeh_pe *root,
			  eeh_report_fn fn, enum pci_ers_result *result)
{
	atomic_t count = ATOMIC_INIT(0);
	DECLARE_COMPLETION_ONSTACK(done);

	pr_info("EEH(%u): Beginning: '%s'\n", event_id, name);
	if (enqueue_pe_work(root, event_id, name, fn, result, &count, &done)) {
		pr_info("EEH(%u): Waiting for asynchronous recovery work to complete...\n",
			event_id);
		eeh_recovery_unlock();
		wait_for_completion_interruptible(&done);
		pr_info("EEH(%u): Asynchronous recovery work is complete.\n", event_id);
		eeh_recovery_lock();
	} else {
		pr_info("EEH(%u): No recovery work do.\n", event_id);
	}

	if (result)
		pr_info("EEH(%u): Finished:'%s' with aggregate recovery state:'%s'\n",
			event_id, name, pci_ers_result_name(*result));
	else
		pr_info("EEH(%u): Finished:'%s'",event_id, name);
}

/**
 * eeh_report_error - Report pci error to each device driver
 * @pdev: eeh device
 * @driver: device's PCI driver
 *
 * Report an EEH error to each device driver.
 */
static enum pci_ers_result eeh_report_error(unsigned int event_id,
					    unsigned int id,
					    struct pci_dev *pdev,
					    struct pci_driver *driver)
{
	enum pci_ers_result rc;
	struct eeh_dev *edev;
	unsigned long flags;

	if (!driver->err_handler->error_detected)
		return PCI_ERS_RESULT_NONE;

	pci_info(pdev, "EEH(%u): W%u: Invoking %s->error_detected(IO frozen)",
		 event_id, id, driver->name);
	rc = driver->err_handler->error_detected(pdev, pci_channel_io_frozen);

	eeh_serialize_lock(&flags);
	edev = pci_dev_to_eeh_dev(pdev);
	if (edev)
		edev->in_error = true;
	eeh_serialize_unlock(flags);

	pci_uevent_ers(pdev, PCI_ERS_RESULT_NONE);
	return rc;
}

/**
 * eeh_report_mmio_enabled - Tell drivers that MMIO has been enabled
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * Tells each device driver that IO ports, MMIO and config space I/O
 * are now enabled.
 */
static enum pci_ers_result eeh_report_mmio_enabled(unsigned int event_id,
						   unsigned int id,
						   struct pci_dev *pdev,
						   struct pci_driver *driver)
{
	if (!driver->err_handler->mmio_enabled)
		return PCI_ERS_RESULT_NONE;
	pci_info(pdev, "EEH(%u): W%u: Invoking %s->mmio_enabled()",
		 event_id, id, driver->name);
	return driver->err_handler->mmio_enabled(pdev);
}

/**
 * eeh_report_reset - Tell device that slot has been reset
 * @edev: eeh device
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * This routine must be called while EEH tries to reset particular
 * PCI device so that the associated PCI device driver could take
 * some actions, usually to save data the driver needs so that the
 * driver can work again while the device is recovered.
 */
static enum pci_ers_result eeh_report_reset(unsigned int event_id,
					    unsigned int id,
					    struct pci_dev *pdev,
					    struct pci_driver *driver)
{
	struct eeh_dev *edev;
	unsigned long flags;

	eeh_serialize_lock(&flags);
	edev = pci_dev_to_eeh_dev(pdev);
	if (!driver->err_handler->slot_reset || !edev->in_error) {
		eeh_serialize_unlock(flags);
		return PCI_ERS_RESULT_NONE;
	}
	eeh_serialize_unlock(flags);
	pci_info(pdev, "EEH(%u): W%u: Invoking %s->slot_reset()",
		 event_id, id, driver->name);
	return driver->err_handler->slot_reset(pdev);
}

static void eeh_dev_restore_state(struct eeh_dev *edev, void *userdata)
{
	struct pci_dev *pdev;

	if (!edev)
		return;

	/*
	 * The content in the config space isn't saved because
	 * the blocked config space on some adapters. We have
	 * to restore the initial saved config space when the
	 * EEH device is created.
	 */
	if (edev->pe && (edev->pe->state & EEH_PE_CFG_RESTRICTED)) {
		if (list_is_last(&edev->entry, &edev->pe->edevs))
			eeh_pe_restore_bars(edev->pe);

		return;
	}

	pdev = eeh_dev_to_pci_dev(edev);
	if (!pdev)
		return;

	pci_restore_state(pdev);
}

/**
 * eeh_report_resume - Tell device to resume normal operations
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * This routine must be called to notify the device driver that it
 * could resume so that the device driver can do some initialization
 * to make the recovered device work again.
 */
static enum pci_ers_result eeh_report_resume(unsigned int event_id,
					     unsigned int id,
					     struct pci_dev *pdev,
					     struct pci_driver *driver)
{
	struct eeh_dev *edev;
	unsigned long flags;

	eeh_serialize_lock(&flags);
	edev = pci_dev_to_eeh_dev(pdev);
	if (!driver->err_handler->resume || !edev->in_error) {
		eeh_serialize_unlock(flags);
		return PCI_ERS_RESULT_NONE;
	}
	eeh_serialize_unlock(flags);

	pci_info(pdev, "EEH(%u): W%u Invoking %s->resume()",
		 event_id, id, driver->name);
	driver->err_handler->resume(pdev);

	pci_uevent_ers(pdev, PCI_ERS_RESULT_RECOVERED);
#ifdef CONFIG_PCI_IOV
	eeh_serialize_lock(&flags);
	if (eeh_ops->notify_resume)
		eeh_ops->notify_resume(edev);
	eeh_serialize_unlock(flags);
#endif
	return PCI_ERS_RESULT_NONE;
}

/**
 * eeh_report_failure - Tell device driver that device is dead.
 * @edev: eeh device
 * @driver: device's PCI driver
 *
 * This informs the device driver that the device is permanently
 * dead, and that no further recovery attempts will be made on it.
 */
static enum pci_ers_result eeh_report_failure(unsigned int event_id,
					      unsigned int id,
					      struct pci_dev *pdev,
					      struct pci_driver *driver)
{
	enum pci_ers_result rc;

	if (!driver->err_handler->error_detected)
		return PCI_ERS_RESULT_NONE;

	pci_info(pdev, "EEH(%u): W%u: Invoking %s->error_detected(permanent failure)",
		 event_id, id, driver->name);
	rc = driver->err_handler->error_detected(pdev,
						 pci_channel_io_perm_failure);

	pci_uevent_ers(pdev, PCI_ERS_RESULT_DISCONNECT);
	return rc;
}

static void *eeh_add_virt_device(struct eeh_dev *edev)
{
	struct pci_driver *driver;
	struct pci_dev *dev = eeh_dev_to_pci_dev(edev);

	if (!(edev->physfn)) {
		eeh_edev_warn(edev, "Not for VF\n");
		return NULL;
	}

	driver = eeh_pcid_get(dev);
	if (driver) {
		if (driver->err_handler) {
			eeh_pcid_put(dev);
			return NULL;
		}
		eeh_pcid_put(dev);
	}

#ifdef CONFIG_PCI_IOV
	{
		struct pci_dev *physfn = edev->physfn;
		int vf_index = edev->vf_index;

		get_device(&physfn->dev);
		eeh_recovery_unlock();
		/*
		 * This PCI operation will call back into EEH code where the
		 * recovery lock will be acquired, so it must be released here,
		 * first:
		 */
		pci_iov_add_virtfn(physfn, vf_index);
		put_device(&physfn->dev);
		eeh_recovery_lock();
	}
#endif
	return NULL;
}

static void eeh_rmv_device(unsigned int event_id,
			   struct pci_dev *pdev, void *userdata)
{
	unsigned long flags;
	struct eeh_dev *edev;
	struct pci_driver *driver;
	struct eeh_rmv_data *rmv_data = (struct eeh_rmv_data *)userdata;

	edev = pci_dev_to_eeh_dev(pdev);
	if (!edev) {
		pci_warn(pdev, "EEH(%u): Device removed during processing (#%d)\n",
			 event_id, __LINE__);
		return;
	}

	/*
	 * Actually, we should remove the PCI bridges as well.
	 * However, that's lots of complexity to do that,
	 * particularly some of devices under the bridge might
	 * support EEH. So we just care about PCI devices for
	 * simplicity here.
	 */
	if (!eeh_edev_actionable(edev) ||
	    (pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE))
		return;

	if (rmv_data) {
		driver = eeh_pcid_get(pdev);
		if (driver) {
			if (driver->err_handler &&
			    driver->err_handler->error_detected &&
			    driver->err_handler->slot_reset) {
				eeh_pcid_put(pdev);
				return;
			}
			eeh_pcid_put(pdev);
		}
	}

	/* Remove it from PCI subsystem */
	pci_info(pdev, "EEH(%u): Removing device without EEH sensitive driver\n",
		 event_id);
	edev->mode |= EEH_DEV_DISCONNECTED;
	if (rmv_data)
		rmv_data->removed_dev_count++;

	if (edev->physfn) {
#ifdef CONFIG_PCI_IOV
		eeh_recovery_unlock();
		pci_iov_remove_virtfn(edev->physfn, edev->vf_index);
		eeh_recovery_lock();
		/* Both locks are required to make changes */
		eeh_serialize_lock(&flags);
		edev->pdev = NULL;
		eeh_serialize_unlock(flags);
#endif
		if (rmv_data)
			list_add(&edev->rmv_entry, &rmv_data->removed_vf_list);
	} else {
		/*
		 * Lock ordering requires that the recovery lock be released
		 * before acquiring the PCI rescan/remove lock.
		 */
		eeh_recovery_unlock();
		pci_lock_rescan_remove();
		pci_stop_and_remove_bus_device(pdev);
		pci_unlock_rescan_remove();
		eeh_recovery_lock();
	}
}

static void *eeh_pe_detach_dev(struct eeh_pe *pe, void *userdata)
{
	struct eeh_dev *edev, *tmp;

	eeh_pe_for_each_dev(pe, edev, tmp) {
		if (!(edev->mode & EEH_DEV_DISCONNECTED))
			continue;

		edev->mode &= ~(EEH_DEV_DISCONNECTED | EEH_DEV_IRQ_DISABLED);
		eeh_pe_tree_remove(edev);
	}

	return NULL;
}

/*
 * Explicitly clear PE's frozen state for PowerNV where
 * we have frozen PE until BAR restore is completed. It's
 * harmless to clear it for pSeries. To be consistent with
 * PE reset (for 3 times), we try to clear the frozen state
 * for 3 times as well.
 */
static int eeh_clear_pe_frozen_state(struct eeh_pe *root, bool include_passed)
{
	struct eeh_pe *pe;
	int i;

	eeh_for_each_pe(root, pe) {
		if (include_passed || !eeh_pe_passed(pe)) {
			for (i = 0; i < 3; i++)
				if (!eeh_unfreeze_pe(pe))
					break;
			if (i >= 3)
				return -EIO;
		}
	}
	eeh_pe_state_clear(root, EEH_PE_ISOLATED, include_passed);
	return 0;
}

int eeh_pe_reset_and_recover(struct eeh_pe *pe)
{
	int ret;

	/* Bail if the PE is being recovered */
	if (pe->state & EEH_PE_RECOVERING)
		return 0;

	/* Put the PE into recovery mode */
	eeh_pe_state_mark(pe, EEH_PE_RECOVERING);

	/* Save states */
	eeh_pe_dev_traverse(pe, eeh_dev_save_state, NULL);

	/* Issue reset */
	ret = eeh_pe_reset_full(pe, true);
	if (ret) {
		eeh_pe_state_clear(pe, EEH_PE_RECOVERING, true);
		return ret;
	}

	/* Unfreeze the PE */
	ret = eeh_clear_pe_frozen_state(pe, true);
	if (ret) {
		eeh_pe_state_clear(pe, EEH_PE_RECOVERING, true);
		return ret;
	}

	/* Restore device state */
	eeh_pe_dev_traverse(pe, eeh_dev_restore_state, NULL);

	/* Clear recovery mode */
	eeh_pe_state_clear(pe, EEH_PE_RECOVERING, true);

	return 0;
}

/**
 * eeh_reset_device - Perform actual reset of a pci slot
 * @driver_eeh_aware: Does the device's driver provide EEH support?
 * @pe: EEH PE
 * @bus: PCI bus corresponding to the isolcated slot
 * @rmv_data: Optional, list to record removed devices
 *
 * This routine must be called to do reset on the indicated PE.
 * During the reset, udev might be invoked because those affected
 * PCI devices will be removed and then added.
 */
static int eeh_reset_device(unsigned int event_id,
			    struct eeh_pe *pe, struct pci_bus *bus,
			    struct eeh_rmv_data *rmv_data,
			    bool driver_eeh_aware)
{
	time64_t tstamp;
	int cnt, rc;
	struct pci_dev **pdevs, **pdevp;
	struct eeh_dev *edev;
	struct eeh_pe *tmp_pe;
	bool any_passed = false;

	eeh_for_each_pe(pe, tmp_pe)
		any_passed |= eeh_pe_passed(tmp_pe);

	/* pcibios will clear the counter; save the value */
	cnt = pe->freeze_count;
	tstamp = pe->tstamp;

	/*
	 * We don't remove the corresponding PE instances because
	 * we need the information afterwords. The attached EEH
	 * devices are expected to be attached soon when calling
	 * into pci_hp_add_devices().
	 */
	eeh_pe_state_mark(pe, EEH_PE_KEEP);
	if (any_passed || driver_eeh_aware || (pe->type & EEH_PE_VF)) {
		/*
		 * eeh_rmv_device() may need to release the recovery lock to
		 * remove a PCI device so we can't rely on the PE lists staying
		 * valid:
		 */
		pdevs = pdev_cache_list_create(pe);
		/* eeh_rmv_device() may re-acquire the recovery lock */
		for (pdevp = pdevs; pdevp && *pdevp; pdevp++)
			eeh_rmv_device(event_id, *pdevp, rmv_data);
		pdev_cache_list_destroy(pdevs);

	} else {
		eeh_recovery_unlock();
		pci_lock_rescan_remove();
		pci_hp_remove_devices(bus);
		pci_unlock_rescan_remove();
		eeh_recovery_lock();
	}

	/*
	 * Reset the pci controller. (Asserts RST#; resets config space).
	 * Reconfigure bridges and devices. Don't try to bring the system
	 * up if the reset failed for some reason.
	 *
	 * During the reset, it's very dangerous to have uncontrolled PCI
	 * config accesses. So we prefer to block them. However, controlled
	 * PCI config accesses initiated from EEH itself are allowed.
	 */
	rc = eeh_pe_reset_full(pe, false);
	if (rc)
		return rc;

	/*
	 * The PCI rescan/remove lock must always be taken first, but we need
	 * both here:
	 */
	eeh_recovery_unlock();
	pci_lock_rescan_remove();
	eeh_recovery_lock();

	/* Restore PE */
	eeh_ops->configure_bridge(pe);
	eeh_pe_restore_bars(pe);

	/* Clear frozen state */
	rc = eeh_clear_pe_frozen_state(pe, false);
	pci_unlock_rescan_remove();
	if (rc)
		return rc;

	/* Give the system 5 seconds to finish running the user-space
	 * hotplug shutdown scripts, e.g. ifdown for ethernet.  Yes,
	 * this is a hack, but if we don't do this, and try to bring
	 * the device up before the scripts have taken it down,
	 * potentially weird things happen.
	 */
	if (!driver_eeh_aware || rmv_data->removed_dev_count) {
		pr_info("EEH(%u): Sleep 5s ahead of %s hotplug\n",
			event_id, (driver_eeh_aware ? "partial" : "complete"));
		eeh_recovery_unlock();
		ssleep(5);
		eeh_recovery_lock();

		/*
		 * The EEH device is still connected with its parent
		 * PE. We should disconnect it so the binding can be
		 * rebuilt when adding PCI devices.
		 */
		edev = list_first_entry(&pe->edevs, struct eeh_dev, entry);
		eeh_pe_traverse(pe, eeh_pe_detach_dev, NULL);
		if (pe->type & EEH_PE_VF) {
			eeh_add_virt_device(edev);
		} else {
			if (!driver_eeh_aware)
				eeh_pe_state_clear(pe, EEH_PE_PRI_BUS, true);
			/*
			 * Lock ordering requires that the recovery lock be
			 * released before acquiring the PCI rescan/remove
			 * lock.
			 */
			eeh_recovery_unlock();
			pci_lock_rescan_remove();
			pci_hp_add_devices(bus);
			pci_unlock_rescan_remove();
			eeh_recovery_lock();

		}
	}
	eeh_pe_state_clear(pe, EEH_PE_KEEP, true);

	pe->tstamp = tstamp;
	pe->freeze_count = cnt;

	return 0;
}

/* The longest amount of time to wait for a pci device
 * to come back on line, in seconds.
 */
#define MAX_WAIT_FOR_RECOVERY 300


/* Walks the PE tree after processing an event to remove any stale PEs.
 *
 * NB: This needs to be recursive to ensure the leaf PEs get removed
 * before their parents do. Although this is possible to do recursively
 * we don't since this is easier to read and we need to garantee
 * the leaf nodes will be handled first.
 */
static void eeh_pe_cleanup(struct eeh_pe *pe)
{
	struct eeh_pe *child_pe, *tmp;

	list_for_each_entry_safe(child_pe, tmp, &pe->child_list, child)
		eeh_pe_cleanup(child_pe);

	if (pe->state & EEH_PE_KEEP)
		return;

	if (!(pe->state & EEH_PE_INVALID))
		return;

	if (list_empty(&pe->edevs) && list_empty(&pe->child_list)) {
		list_del(&pe->child);
		kfree(pe);
	}
}

/**
 * eeh_check_slot_presence - Check if a device is still present in a slot
 * @pdev: pci_dev to check
 *
 * This function may return a false positive if we can't determine the slot's
 * presence state. This might happen for PCIe slots if the PE containing
 * the upstream bridge is also frozen, or the bridge is part of the same PE
 * as the device.
 *
 * This shouldn't happen often, but you might see it if you hotplug a PCIe
 * switch.
 */
static bool eeh_slot_presence_check(struct pci_dev *pdev)
{
	const struct hotplug_slot_ops *ops;
	struct pci_slot *slot;
	u8 state;
	int rc;

	if (!pdev)
		return false;

	if (pdev->error_state == pci_channel_io_perm_failure)
		return false;

	slot = pdev->slot;
	if (!slot || !slot->hotplug)
		return true;

	ops = slot->hotplug->ops;
	if (!ops || !ops->get_adapter_status)
		return true;

	/* set the attention indicator while we've got the slot ops */
	if (ops->set_attention_status)
		ops->set_attention_status(slot->hotplug, 1);

	rc = ops->get_adapter_status(slot->hotplug, &state);
	if (rc)
		return true;

	return !!state;
}

static void eeh_clear_slot_attention(struct pci_dev *pdev)
{
	const struct hotplug_slot_ops *ops;
	struct pci_slot *slot;

	if (!pdev)
		return;

	if (pdev->error_state == pci_channel_io_perm_failure)
		return;

	slot = pdev->slot;
	if (!slot || !slot->hotplug)
		return;

	ops = slot->hotplug->ops;
	if (!ops || !ops->set_attention_status)
		return;

	ops->set_attention_status(slot->hotplug, 0);
}

/**
 * eeh_handle_normal_event - Handle EEH events on a specific PE
 * @pe: EEH PE - which should not be used after we return, as it may
 * have been invalidated.
 *
 * Attempts to recover the given PE.  If recovery fails or the PE has failed
 * too many times, remove the PE.
 *
 * While PHB detects address or data parity errors on particular PCI
 * slot, the associated PE will be frozen. Besides, DMA's occurring
 * to wild addresses (which usually happen due to bugs in device
 * drivers or in PCI adapter firmware) can cause EEH error. #SERR,
 * #PERR or other misc PCI-related errors also can trigger EEH errors.
 *
 * Recovery process consists of unplugging the device driver (which
 * generated hotplug events to userspace), then issuing a PCI #RST to
 * the device, then reconfiguring the PCI config space for all bridges
 * & devices under this slot, and then finally restarting the device
 * drivers (which cause a second set of hotplug events to go out to
 * userspace).
 */
void eeh_handle_normal_event(unsigned int event_id, struct eeh_pe *pe)
{
	struct eeh_pe *tmp_pe;
	struct pci_controller *phb = pe->phb;
	struct pci_bus *bus;
	struct eeh_dev *edev, *tmp;
	struct pci_dev **pdevs, **pdevp;
	int rc = 0;
	enum pci_ers_result result = PCI_ERS_RESULT_NONE;
	struct eeh_rmv_data rmv_data =
		{LIST_HEAD_INIT(rmv_data.removed_vf_list), 0};
	int devices = 0;

	eeh_recovery_lock();
	bus = eeh_pe_bus_get(pe);
	if (!bus) {
		pr_err("EEH(%u): %s: Cannot find PCI bus for PHB#%x-PE#%x\n",
			event_id, __func__, phb->global_number, pe->addr);
		eeh_recovery_unlock();
		return;
	}

	/*
	 * When devices are hot-removed we might get an EEH due to
	 * a driver attempting to touch the MMIO space of a removed
	 * device. In this case we don't have a device to recover
	 * so suppress the event if we can't find any present devices.
	 *
	 * The hotplug driver should take care of tearing down the
	 * device itself.
	 */
	eeh_for_each_pe(pe, tmp_pe)
		eeh_pe_for_each_dev(tmp_pe, edev, tmp)
			if (eeh_slot_presence_check(edev->pdev))
				devices++;

	if (!devices) {
		pr_debug("EEH(%u): Frozen PHB#%x-PE#%x is empty!\n",
			event_id, phb->global_number, pe->addr);
		goto out; /* nothing to recover */
	}

	pe->freeze_count++;
	pr_warn("EEH(%u): EVENT=RECOVERY_START TYPE=%s PHB=%#x PE=%#x COUNT=%d\n",
		event_id, ((pe->type & EEH_PE_PHB) ? "PHB" : "PE"),
		pe->phb->global_number, pe->addr, pe->freeze_count);

	/* Log the event */
	if (pe->type & EEH_PE_PHB) {
		pr_err("EEH(%u): Recovering PHB#%x, location: %s\n",
			event_id, phb->global_number, eeh_pe_loc_get(pe));
	} else {
		struct eeh_pe *phb_pe = eeh_phb_pe_get(phb);

		pr_err("EEH(%u): Recovering PHB#%x-PE#%x\n",
		       event_id, phb->global_number, pe->addr);
		pr_err("EEH(%u): PE location: %s, PHB location: %s\n",
		       event_id, eeh_pe_loc_get(pe), eeh_pe_loc_get(phb_pe));
	}

#ifdef CONFIG_STACKTRACE
	/*
	 * Print the saved stack trace now that we've verified there's
	 * something to recover.
	 */
	if (pe->trace_entries) {
		void **ptrs = (void **) pe->stack_trace;
		int i;

		pr_err("EEH(%u): Frozen PHB#%x-PE#%x detected\n",
		       event_id, phb->global_number, pe->addr);

		/* FIXME: Use the same format as dump_stack() */
		pr_err("EEH(%u): Call Trace:\n", event_id);
		for (i = 0; i < pe->trace_entries; i++)
			pr_err("EEH(%u): [%pK] %pS\n", event_id, ptrs[i], ptrs[i]);

		pe->trace_entries = 0;
	}
#endif /* CONFIG_STACKTRACE */

	eeh_for_each_pe(pe, tmp_pe)
		eeh_pe_for_each_dev(tmp_pe, edev, tmp)
			edev->mode &= ~EEH_DEV_NO_HANDLER;

	eeh_pe_update_time_stamp(pe);
	if (pe->freeze_count > eeh_max_freezes) {
		pr_err("EEH(%u): PHB#%x-PE#%x has failed %d times in the last hour and has been permanently disabled.\n",
		       event_id, phb->global_number, pe->addr,
		       pe->freeze_count);

		goto recover_failed;
	}

	/* Walk the various device drivers attached to this slot through
	 * a reset sequence, giving each an opportunity to do what it needs
	 * to accomplish the reset.  Each child gets a report of the
	 * status ... if any child can't handle the reset, then the entire
	 * slot is dlpar removed and added.
	 *
	 * When the PHB is fenced, we have to issue a reset to recover from
	 * the error. Override the result if necessary to have partially
	 * hotplug for this case.
	 */
	pr_warn("EEH(%u): This PCI device has failed %d times in the last hour and will be permanently disabled after %d failures.\n",
		event_id, pe->freeze_count, eeh_max_freezes);
	pr_info("EEH(%u): Notify device drivers to shutdown\n", event_id);
	eeh_set_channel_state(pe, pci_channel_io_frozen);
	eeh_set_irq_state(pe, false);
	eeh_pe_report(event_id, "error_detected(IO frozen)", pe,
		      eeh_report_error, &result);
	if (result == PCI_ERS_RESULT_DISCONNECT)
		goto recover_failed;

	/*
	 * Error logged on a PHB are always fences which need a full
	 * PHB reset to clear so force that to happen.
	 */
	if ((pe->type & EEH_PE_PHB) && result != PCI_ERS_RESULT_NONE)
		result = PCI_ERS_RESULT_NEED_RESET;

	/* Get the current PCI slot state. This can take a long time,
	 * sometimes over 300 seconds for certain systems.
	 */
	rc = eeh_wait_state(pe, MAX_WAIT_FOR_RECOVERY * 1000, true);
	if (rc < 0 || rc == EEH_STATE_NOT_SUPPORT) {
		pr_warn("EEH(%u): Permanent failure\n", event_id);
		goto recover_failed;
	}

	/* Since rtas may enable MMIO when posting the error log,
	 * don't post the error log until after all dev drivers
	 * have been informed.
	 */
	pr_info("EEH(%u): Collect temporary log\n", event_id);
	eeh_slot_error_detail(event_id, pe, EEH_LOG_TEMP);

	/* If all device drivers were EEH-unaware, then shut
	 * down all of the device drivers, and hope they
	 * go down willingly, without panicing the system.
	 */
	if (result == PCI_ERS_RESULT_NONE) {
		pr_info("EEH(%u): Reset with hotplug activity\n", event_id);
		rc = eeh_reset_device(event_id, pe, bus, NULL, false);
		if (rc) {
			pr_warn("%s: Unable to reset, err=%d\n", __func__, rc);
			goto recover_failed;
		}
	}

	/* If all devices reported they can proceed, then re-enable MMIO */
	if (result == PCI_ERS_RESULT_CAN_RECOVER) {
		pr_info("EEH(%u): Enable I/O for affected devices\n", event_id);
		rc = eeh_pci_enable(pe, EEH_OPT_THAW_MMIO);
		if (rc < 0)
			goto recover_failed;

		if (rc) {
			result = PCI_ERS_RESULT_NEED_RESET;
		} else {
			pr_info("EEH(%u): Notify device drivers to resume I/O\n", event_id);
			eeh_pe_report(event_id, "mmio_enabled", pe,
				      eeh_report_mmio_enabled, &result);
		}
	}
	if (result == PCI_ERS_RESULT_CAN_RECOVER) {
		pr_info("EEH(%u): Enabled DMA for affected devices\n", event_id);
		rc = eeh_pci_enable(pe, EEH_OPT_THAW_DMA);
		if (rc < 0)
			goto recover_failed;

		if (rc) {
			result = PCI_ERS_RESULT_NEED_RESET;
		} else {
			/*
			 * We didn't do PE reset for the case. The PE
			 * is still in frozen state. Clear it before
			 * resuming the PE.
			 */
			eeh_pe_state_clear(pe, EEH_PE_ISOLATED, true);
			result = PCI_ERS_RESULT_RECOVERED;
		}
	}

	/* If any device called out for a reset, then reset the slot */
	if (result == PCI_ERS_RESULT_NEED_RESET) {
		pr_info("EEH(%u): Reset without hotplug activity\n", event_id);
		rc = eeh_reset_device(event_id, pe, bus, &rmv_data, true);
		if (rc) {
			pr_warn("%s: Cannot reset, err=%d\n", __func__, rc);
			goto recover_failed;
		}

		result = PCI_ERS_RESULT_NONE;
		eeh_set_channel_state(pe, pci_channel_io_normal);
		eeh_set_irq_state(pe, true);
		eeh_pe_report(event_id, "slot_reset", pe, eeh_report_reset,
			      &result);
	}

	if ((result == PCI_ERS_RESULT_RECOVERED) ||
	    (result == PCI_ERS_RESULT_NONE)) {
		/*
		 * For those hot removed VFs, we should add back them after PF
		 * get recovered properly.
		 */
		list_for_each_entry_safe(edev, tmp, &rmv_data.removed_vf_list,
					 rmv_entry) {
			eeh_add_virt_device(edev);
			list_del(&edev->rmv_entry);
		}

		/* Tell all device drivers that they can resume operations */
		pr_info("EEH(%u): Notify device driver to resume\n", event_id);
		eeh_set_channel_state(pe, pci_channel_io_normal);
		eeh_set_irq_state(pe, true);
		eeh_pe_report(event_id, "resume", pe, eeh_report_resume, NULL);
		eeh_for_each_pe(pe, tmp_pe) {
			eeh_pe_for_each_dev(tmp_pe, edev, tmp) {
				edev->mode &= ~EEH_DEV_NO_HANDLER;
				edev->in_error = false;
			}
		}

		pr_info("EEH(%u): Recovery successful.\n", event_id);
		goto out;
	}

recover_failed:
	/*
	 * About 90% of all real-life EEH failures in the field
	 * are due to poorly seated PCI cards. Only 10% or so are
	 * due to actual, failed cards.
	 */
	pr_err("EEH(%u): Unable to recover from failure from PHB#%x-PE#%x.\n"
		"Please try reseating or replacing it\n",
		event_id, phb->global_number, pe->addr);

	eeh_slot_error_detail(event_id, pe, EEH_LOG_PERM);

	/* Notify all devices that they're about to go down. */
	eeh_set_irq_state(pe, false);
	eeh_pe_report(event_id, "error_detected(permanent failure)", pe,
		      eeh_report_failure, NULL);
	eeh_set_channel_state(pe, pci_channel_io_perm_failure);
	pr_crit("EEH(%u): EVENT=RECOVERY_END RESULT=failure\n", event_id);

	/* Mark the PE to be removed permanently */
	eeh_pe_state_mark(pe, EEH_PE_REMOVED);

	/*
	 * Shut down the device drivers for good. We mark
	 * all removed devices correctly to avoid access
	 * the their PCI config any more.
	 */
	if (pe->type & EEH_PE_VF) {
		pdevs = pdev_cache_list_create(pe);
		for (pdevp = pdevs; pdevp && *pdevp; pdevp++)
			eeh_rmv_device(event_id, *pdevp, NULL);
		pdev_cache_list_destroy(pdevs);
		eeh_pe_dev_mode_mark(pe, EEH_DEV_REMOVED);
	} else {
		eeh_pe_state_clear(pe, EEH_PE_PRI_BUS, true);
		eeh_pe_dev_mode_mark(pe, EEH_DEV_REMOVED);

		eeh_recovery_unlock();
		pci_lock_rescan_remove();
		pci_hp_remove_devices(bus);
		pci_unlock_rescan_remove();
		/* The passed PE should no longer be used */
		return;
	}

	pr_info("EEH(%u): EVENT=RECOVERY_END RESULT=success\n", event_id);

out:
	/*
	 * Clean up any PEs without devices. While marked as EEH_PE_RECOVERYING
	 * we don't want to modify the PE tree structure so we do it here.
	 */
	eeh_pe_cleanup(pe);

	/* clear the slot attention LED for all recovered devices */
	eeh_for_each_pe(pe, tmp_pe)
		eeh_pe_for_each_dev(tmp_pe, edev, tmp)
			eeh_clear_slot_attention(edev->pdev);

	eeh_pe_state_clear(pe, EEH_PE_RECOVERING, true);
	eeh_recovery_unlock();
}

void eeh_handle_normal_event_work(struct work_struct *work)
{
	unsigned long flags;
	struct eeh_event *event = container_of(work, struct eeh_event, work);
	struct pci_controller *phb = event->pe->phb;

	eeh_handle_normal_event(event->id, event->pe);

	kfree(event);
	spin_lock_irqsave(&phb->eeh_eventlist_lock, flags);
	WARN_ON_ONCE(!phb->eeh_in_progress);
	if (list_empty(&phb->eeh_eventlist)) {
		phb->eeh_in_progress = false;
		pr_debug("EEH(%u): No more work to do\n", event->id);
	} else {
		pr_warn("EEH(%u): More work to do\n", event->id);
		event = list_entry(phb->eeh_eventlist.next,
				   struct eeh_event, list);
		list_del(&event->list);
		queue_work(system_unbound_wq, &event->work);
	}
	spin_unlock_irqrestore(&phb->eeh_eventlist_lock, flags);
}

/**
 * eeh_handle_special_event - Handle EEH events without a specific failing PE
 *
 * Called when an EEH event is detected but can't be narrowed down to a
 * specific PE.  Iterates through possible failures and handles them as
 * necessary.
 */
void eeh_handle_special_event(void)
{
	struct eeh_pe *pe, *phb_pe, *tmp_pe;
	struct eeh_dev *edev, *tmp_edev;
	struct pci_bus *bus;
	struct pci_controller *hose;
	unsigned long flags;
	int rc;


	do {
		rc = eeh_ops->next_error(&pe);

		switch (rc) {
		case EEH_NEXT_ERR_DEAD_IOC:
			/* Mark all PHBs in dead state */
			eeh_serialize_lock(&flags);

			/* Purge all events */
			eeh_remove_event(NULL, true);

			list_for_each_entry(hose, &hose_list, list_node) {
				phb_pe = eeh_phb_pe_get(hose);
				if (!phb_pe) continue;

				eeh_pe_mark_isolated(phb_pe);
			}

			eeh_serialize_unlock(flags);

			break;
		case EEH_NEXT_ERR_FROZEN_PE:
		case EEH_NEXT_ERR_FENCED_PHB:
		case EEH_NEXT_ERR_DEAD_PHB:
			/* Mark the PE in fenced state */
			eeh_serialize_lock(&flags);

			/* Purge all events of the PHB */
			eeh_remove_event(pe, true);

			if (rc != EEH_NEXT_ERR_DEAD_PHB)
				eeh_pe_state_mark(pe, EEH_PE_RECOVERING);
			eeh_pe_mark_isolated(pe);

			eeh_serialize_unlock(flags);

			break;
		case EEH_NEXT_ERR_NONE:
			return;
		default:
			pr_warn("%s: Invalid value %d from next_error()\n",
				__func__, rc);
			return;
		}

		/*
		 * For fenced PHB and frozen PE, it's handled as normal
		 * event. We have to remove the affected PHBs for dead
		 * PHB and IOC
		 */
		if (rc == EEH_NEXT_ERR_FROZEN_PE ||
		    rc == EEH_NEXT_ERR_FENCED_PHB) {
			eeh_phb_event(pe);
		} else {
			eeh_for_each_pe(pe, tmp_pe)
				eeh_pe_for_each_dev(tmp_pe, edev, tmp_edev)
					edev->mode &= ~EEH_DEV_NO_HANDLER;

			/* Notify all devices to be down */
			eeh_pe_state_clear(pe, EEH_PE_PRI_BUS, true);
			eeh_pe_report(0,
				"error_detected(permanent failure)", pe,
				eeh_report_failure, NULL);
			eeh_set_channel_state(pe, pci_channel_io_perm_failure);

			pci_lock_rescan_remove();
			list_for_each_entry(hose, &hose_list, list_node) {
				phb_pe = eeh_phb_pe_get(hose);
				if (!phb_pe ||
				    !(phb_pe->state & EEH_PE_ISOLATED) ||
				    (phb_pe->state & EEH_PE_RECOVERING))
					continue;

				bus = eeh_pe_bus_get(phb_pe);
				if (!bus) {
					pr_err("%s: Cannot find PCI bus for "
					       "PHB#%x-PE#%x\n",
					       __func__,
					       pe->phb->global_number,
					       pe->addr);
					break;
				}
				pci_hp_remove_devices(bus);
			}
			pci_unlock_rescan_remove();
		}

		/*
		 * If we have detected dead IOC, we needn't proceed
		 * any more since all PHBs would have been removed
		 */
		if (rc == EEH_NEXT_ERR_DEAD_IOC)
			break;
	} while (rc != EEH_NEXT_ERR_NONE);
}
