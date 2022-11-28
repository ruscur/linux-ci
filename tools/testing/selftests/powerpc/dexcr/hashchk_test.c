#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "dexcr.h"
#include "utils.h"

static int require_nphie(void)
{
	SKIP_IF_MSG(!pr_aspect_supported(PR_PPC_DEXCR_NPHIE),
		    "DEXCR[NPHIE] not supported");

	if (dexcr_pro_check(DEXCR_PRO_NPHIE, EFFECTIVE))
		return 0;

	pr_aspect_edit(PR_PPC_DEXCR_NPHIE, PR_PPC_DEXCR_FORCE_SET_ASPECT);
	FAIL_IF_EXIT_MSG(!dexcr_pro_check(DEXCR_PRO_NPHIE, EFFECTIVE),
			 "failed to enable DEXCR[NPIHE]");

	return 0;
}

static void sigill_handler_enabled(int signum, siginfo_t *info, void *context)
{
	SIGSAFE_FAIL_IF_EXIT_MSG(signum != SIGILL, "wrong signal received");
	SIGSAFE_FAIL_IF_EXIT_MSG(info->si_code != ILL_ILLOPN, "wrong signal-code received");
	exit(0);
}

static void do_bad_hashchk(void)
{
	unsigned long hash = 0;
	void *hash_p = ((void *)&hash) + 8;	/* hash* offset must be at least -8 */

	asm ("li 3, 0;"			/* set r3 (pretend LR) to known value */
	     "hashst 3, -8(%1);"	/* compute good hash */
	     "addi 3, 3, 1;"		/* modify hash */
	     "hashchk 3, -8(%1);"	/* check bad hash */
	     : "+m" (hash) : "r" (hash_p) : "r3");
}

/*
 * Check that hashchk triggers when DEXCR[NPHIE] is enabled
 * and is detected as such by the kernel exception handler
 */
static int hashchk_enabled_test(void)
{
	int err;
	struct sigaction sa;

	if ((err = require_nphie()))
		return err;

	sa.sa_sigaction = sigill_handler_enabled;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;
	FAIL_IF_MSG(sigaction(SIGILL, &sa, NULL), "cannot install signal handler");

	do_bad_hashchk();

	FAIL_IF_MSG(true, "hashchk failed to trigger");
}

#define HASH_COUNT 8

static unsigned long hash_values[HASH_COUNT + 1];

static void fill_hash_values(void)
{
	for (unsigned long i = 0; i < HASH_COUNT; i++) {
		void *hash_addr = ((void*)&hash_values[i]) + 8;

		asm volatile ("hashst %2, -8(%1);"
			      : "+m" (hash_values[i]) : "r" (hash_addr), "r" (i));
	}

	hash_values[HASH_COUNT] = (unsigned long)&hash_values;
}

static unsigned int count_hash_values_matches(void)
{
	unsigned long matches = 0;

	FAIL_IF_EXIT_MSG(hash_values[HASH_COUNT] != (unsigned long)hash_values,
			 "bad address check");

	for (unsigned long i = 0; i < HASH_COUNT; i++) {
		unsigned long orig_hash = hash_values[i];
		void *hash_addr = ((void*)&hash_values[i]) + 8;

		asm volatile ("hashst %2, -8(%1);"
			      : "+m" (hash_values[i]) : "r" (hash_addr), "r" (i));

		if (hash_values[i] == orig_hash)
			matches++;
	}

	return matches;
}

static int hashchk_exec_child(void)
{
	ssize_t count;

	fill_hash_values();

	count = write(STDOUT_FILENO, hash_values, sizeof(hash_values));
	return count == sizeof(hash_values) ? 0 : EOVERFLOW;
}

/*
 * Check that new programs get different keys so a malicious process
 * can't recreate a victim's hash values.
 */
static int hashchk_exec_random_key_test(void)
{
	pid_t pid;
	int err;
	int pipefd[2];

	if ((err = require_nphie()))
		return err;

	FAIL_IF_MSG(pipe(pipefd), "failed to create pipe");

	pid = fork();
	if (pid == 0) {
		char *args[] = { "hashchk_exec_child", NULL };

		if (dup2(pipefd[1], STDOUT_FILENO) == -1)
			_exit(errno);

		execve("/proc/self/exe", args, NULL);
		_exit(errno);
	}

	await_child_success(pid);
	FAIL_IF_MSG(read(pipefd[0], hash_values, sizeof(hash_values)) != sizeof(hash_values),
		    "missing expected child output");

	/* If all hashes are the same it means (most likely) same key */
	FAIL_IF_MSG(count_hash_values_matches() == HASH_COUNT, "shared key detected");

	return 0;
}

/*
 * Check that forks share the same key so that existing hash values
 * remain valid.
 */
static int hashchk_fork_share_key_test(void)
{
	pid_t pid;
	int err;

	if ((err = require_nphie()))
		return err;

	fill_hash_values();

	pid = fork();
	if (pid == 0) {
		if (count_hash_values_matches() != HASH_COUNT)
			_exit(1);
		_exit(0);
	}

	await_child_success(pid);
	return 0;
}

#define STACK_SIZE (1024 * 1024)

static int hashchk_clone_child_fn(void *args)
{
	fill_hash_values();
	return 0;
}

/*
 * Check that threads share the same key so that existing hash values
 * remain valid.
 */
static int hashchk_clone_share_key_test(void)
{
	void *child_stack;
	pid_t pid;
	int err;

	if ((err = require_nphie()))
		return err;

	child_stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);

	FAIL_IF_MSG(child_stack == MAP_FAILED, "failed to map child stack");

	pid = clone(hashchk_clone_child_fn, child_stack + STACK_SIZE, CLONE_VM | SIGCHLD, NULL);

	await_child_success(pid);
	FAIL_IF_MSG(count_hash_values_matches() != HASH_COUNT, "different key detected");

	return 0;
}

int main(int argc, char *argv[])
{
	int err = 0;

	if (argc >= 1 && !strcmp(argv[0], "hashchk_exec_child"))
		return hashchk_exec_child();

	err |= test_harness(hashchk_enabled_test, "hashchk_enabled");
	err |= test_harness(hashchk_exec_random_key_test, "hashchk_exec_random_key");
	err |= test_harness(hashchk_fork_share_key_test, "hashchk_fork_share_key");
	err |= test_harness(hashchk_clone_share_key_test, "hashchk_clone_share_key");

	return err;
}
