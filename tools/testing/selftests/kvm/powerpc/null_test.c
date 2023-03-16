// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tests for guest creation, run, ucall, interrupt, and vm dumping.
 */

#define _GNU_SOURCE /* for program_invocation_short_name */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include "test_util.h"
#include "kvm_util.h"
#include "kselftest.h"
#include "processor.h"

extern void guest_code_asm(void);
asm(".global guest_code_asm");
asm(".balign 4");
asm("guest_code_asm:");
asm("li 3,0"); // H_UCALL
asm("li 4,0"); // UCALL_R4_SIMPLE
asm("sc 1");

static void guest_code_ucall(void)
{
	GUEST_DONE();
}

static void guest_code_trap(void)
{
	asm volatile("trap");
}

static void guest_code_dsi(void)
{
	*(volatile int *)0;
}

static void test_asm(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code_asm);

	ret = _vcpu_run(vcpu);

	TEST_ASSERT(ret == 0, "vcpu_run failed: %d\n", ret);
	TEST_ASSERT(get_ucall(vcpu, NULL) == UCALL_NONE,
		    "Invalid guest done status: exit_reason=%s\n",
		    exit_reason_str(vcpu->run->exit_reason));

	kvm_vm_free(vm);
}

static void test_ucall(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code_ucall);

	ret = _vcpu_run(vcpu);

	TEST_ASSERT(ret == 0, "vcpu_run failed: %d\n", ret);
	TEST_ASSERT(get_ucall(vcpu, NULL) == UCALL_DONE,
		    "Invalid guest done status: exit_reason=%s\n",
		    exit_reason_str(vcpu->run->exit_reason));

	kvm_vm_free(vm);
}

static bool got_trap;
static bool trap_handler(struct kvm_vcpu *vcpu, unsigned trap)
{
	if (trap == 0x700) {
		got_trap = true;
		return true;
	}
	return false;
}

static void test_trap(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int ret;

	interrupt_handler = trap_handler;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code_trap);

	ret = _vcpu_run(vcpu);

	TEST_ASSERT(ret == 0, "vcpu_run failed: %d\n", ret);
	TEST_ASSERT(got_trap,
		    "Invalid guest done status: exit_reason=%s\n",
		    exit_reason_str(vcpu->run->exit_reason));

	kvm_vm_free(vm);

	interrupt_handler = NULL;
}

static bool got_dsi;
static bool dsi_handler(struct kvm_vcpu *vcpu, unsigned trap)
{
	if (trap == 0x300) {
		got_dsi = true;
		return true;
	}
	return false;
}

static void test_dsi(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int ret;

	interrupt_handler = dsi_handler;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code_dsi);

	ret = _vcpu_run(vcpu);

	TEST_ASSERT(ret == 0, "vcpu_run failed: %d\n", ret);
	TEST_ASSERT(got_dsi,
		    "Invalid guest done status: exit_reason=%s\n",
		    exit_reason_str(vcpu->run->exit_reason));

	vm_dump(stderr, vm, 2);

	kvm_vm_free(vm);

	interrupt_handler = NULL;
}

static void test_dump(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;
	int ret;

	vm = vm_create_with_one_vcpu(&vcpu, guest_code_ucall);

	ret = _vcpu_run(vcpu);
	TEST_ASSERT(ret == 0, "vcpu_run failed: %d\n", ret);

	printf("Testing vm_dump...\n");
	vm_dump(stderr, vm, 2);

	kvm_vm_free(vm);
}


struct testdef {
	const char *name;
	void (*test)(void);
} testlist[] = {
	{ "null asm test", test_asm},
	{ "null ucall test", test_ucall},
	{ "trap test", test_trap},
	{ "page fault test", test_dsi},
	{ "vm dump test", test_dump},
};

int main(int argc, char *argv[])
{
	int idx;

	ksft_print_header();

	ksft_set_plan(ARRAY_SIZE(testlist));

	for (idx = 0; idx < ARRAY_SIZE(testlist); idx++) {
		testlist[idx].test();
		ksft_test_result_pass("%s\n", testlist[idx].name);
	}

	ksft_finished();	/* Print results and exit() accordingly */
}
