/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * POWER Dynamic Execution Control Facility (DEXCR)
 *
 * This header file contains helper functions and macros
 * required for all the DEXCR related test cases.
 */
#ifndef _SELFTESTS_POWERPC_DEXCR_DEXCR_H
#define _SELFTESTS_POWERPC_DEXCR_DEXCR_H

#include <stdbool.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "reg.h"
#include "utils.h"

#define DEXCR_PRO_MASK(aspect)	__MASK(63 - (32 + (aspect)))
#define DEXCR_PRO_SBHE		DEXCR_PRO_MASK(0)
#define DEXCR_PRO_IBRTPD	DEXCR_PRO_MASK(3)
#define DEXCR_PRO_SRAPD		DEXCR_PRO_MASK(4)
#define DEXCR_PRO_NPHIE		DEXCR_PRO_MASK(5)

#define SYSCTL_DEXCR_SBHE	"/proc/sys/kernel/speculative_branch_hint_enable"

enum DexcrSource {
	UDEXCR,		/* Userspace DEXCR value */
	ENFORCED,	/* Enforced by hypervisor */
	EFFECTIVE,	/* Bitwise OR of requested and enforced DEXCR bits */
};

unsigned int get_dexcr(enum DexcrSource source);

bool pr_aspect_supported(unsigned long which);

bool pr_aspect_editable(unsigned long which);

bool pr_aspect_edit(unsigned long which, unsigned long ctrl);

bool pr_aspect_check(unsigned long which, enum DexcrSource source);

int pr_aspect_get(unsigned long which);

unsigned int pr_aspect_to_dexcr_mask(unsigned long which);

bool dexcr_pro_check(unsigned int pro, enum DexcrSource source);

long sysctl_get_sbhe(void);

void sysctl_set_sbhe(long value);

void await_child_success(pid_t pid);

#endif  /* _SELFTESTS_POWERPC_DEXCR_DEXCR_H */
