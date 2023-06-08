// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test basic guest interrupt/exit performance.
 */

#define _GNU_SOURCE /* for program_invocation_short_name */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <signal.h>

#include "test_util.h"
#include "kvm_util.h"
#include "kselftest.h"
#include "processor.h"
#include "helpers.h"
#include "hcall.h"

static bool timeout;
static unsigned long count;
static struct kvm_vm *kvm_vm;

static void set_timer(int sec)
{
	struct itimerval timer;

	timeout = false;

	timer.it_value.tv_sec  = sec;
	timer.it_value.tv_usec = 0;
	timer.it_interval = timer.it_value;
	TEST_ASSERT(setitimer(ITIMER_REAL, &timer, NULL) == 0,
			"setitimer failed %s", strerror(errno));
}

static void sigalrm_handler(int sig)
{
	timeout = true;
	sync_global_to_guest(kvm_vm, timeout);
}

static void init_timers(void)
{
	TEST_ASSERT(signal(SIGALRM, sigalrm_handler) != SIG_ERR,
		    "Failed to register SIGALRM handler, errno = %d (%s)",
		    errno, strerror(errno));
}

static void program_interrupt_handler(struct ex_regs *regs)
{
	regs->nia += 4;
}

static void program_interrupt_guest_code(void)
{
	unsigned long nr = 0;

	while (!timeout) {
		asm volatile("trap");
		nr++;
		barrier();
	}
	count = nr;

	GUEST_DONE();
}

static void program_interrupt_test(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	/* Create VM */
	vm = vm_create_with_one_vcpu(&vcpu, program_interrupt_guest_code);
	kvm_vm = vm;
	vm_install_exception_handler(vm, 0x700, program_interrupt_handler);

	set_timer(1);

	while (!timeout) {
		vcpu_run(vcpu);
		barrier();
	}

	sync_global_from_guest(vm, count);

	kvm_vm = NULL;
	vm_install_exception_handler(vm, 0x700, NULL);

	kvm_vm_free(vm);

	printf("%lu guest interrupts per second\n", count);
	count = 0;
}

static void heai_guest_code(void)
{
	unsigned long nr = 0;

	while (!timeout) {
		asm volatile(".long 0");
		nr++;
		barrier();
	}
	count = nr;

	GUEST_DONE();
}

static void heai_test(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	/* Create VM */
	vm = vm_create_with_one_vcpu(&vcpu, heai_guest_code);
	kvm_vm = vm;
	vm_install_exception_handler(vm, 0x700, program_interrupt_handler);

	set_timer(1);

	while (!timeout) {
		vcpu_run(vcpu);
		barrier();
	}

	sync_global_from_guest(vm, count);

	kvm_vm = NULL;
	vm_install_exception_handler(vm, 0x700, NULL);

	kvm_vm_free(vm);

	printf("%lu guest exits per second\n", count);
	count = 0;
}

static void hcall_guest_code(void)
{
	for (;;)
		hcall0(H_RTAS);
}

static void hcall_test(void)
{
	struct kvm_vcpu *vcpu;
	struct kvm_vm *vm;

	/* Create VM */
	vm = vm_create_with_one_vcpu(&vcpu, hcall_guest_code);
	kvm_vm = vm;

	set_timer(1);

	while (!timeout) {
		vcpu_run(vcpu);
		count++;
		barrier();
	}

	kvm_vm = NULL;

	kvm_vm_free(vm);

	printf("%lu KVM exits per second\n", count);
	count = 0;
}

struct testdef {
	const char *name;
	void (*test)(void);
} testlist[] = {
	{ "guest interrupt test", program_interrupt_test},
	{ "guest exit test", heai_test},
	{ "KVM exit test", hcall_test},
};

int main(int argc, char *argv[])
{
	int idx;

	ksft_print_header();

	ksft_set_plan(ARRAY_SIZE(testlist));

	init_timers();

	for (idx = 0; idx < ARRAY_SIZE(testlist); idx++) {
		testlist[idx].test();
		ksft_test_result_pass("%s\n", testlist[idx].name);
	}

	ksft_finished();	/* Print results and exit() accordingly */
}
