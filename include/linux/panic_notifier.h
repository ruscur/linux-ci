/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PANIC_NOTIFIERS_H
#define _LINUX_PANIC_NOTIFIERS_H

#include <linux/notifier.h>
#include <linux/types.h>

extern struct atomic_notifier_head panic_hypervisor_list;
extern struct atomic_notifier_head panic_info_list;
extern struct atomic_notifier_head panic_pre_reboot_list;
extern struct atomic_notifier_head panic_post_reboot_list;

bool panic_notifiers_before_kdump(void);

enum panic_notifier_val {
	PANIC_UNUSED,
	PANIC_NOTIFIER = 0xDEAD,
};

#endif	/* _LINUX_PANIC_NOTIFIERS_H */
