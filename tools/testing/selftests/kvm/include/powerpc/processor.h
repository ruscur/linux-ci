/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * powerpc processor specific defines
 */
#ifndef SELFTEST_KVM_PROCESSOR_H
#define SELFTEST_KVM_PROCESSOR_H

#include <linux/compiler.h>

struct kvm_vcpu;
extern bool (*interrupt_handler)(struct kvm_vcpu *vcpu, unsigned trap);

#endif
