#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "cap.h"
#include "dexcr.h"
#include "utils.h"

/*
 * Test that an editable aspect
 * - Current prctl state reported by the getter
 * - Can be toggled on and off when process has CAP_SYS_ADMIN
 * - Can't be edited if CAP_SYS_ADMIN not present
 * - Can't be modified after force set
 */
static int dexcr_prctl_editable_aspect_test(unsigned long which)
{
	pid_t pid;

	SKIP_IF_MSG(!check_cap_sysadmin(), "must have capability CAP_SYS_ADMIN");
	SKIP_IF_MSG(!pr_aspect_supported(which), "aspect not supported");

	FAIL_IF_MSG(!(pr_aspect_get(which) & PR_PPC_DEXCR_PRCTL), "aspect not editable");

	FAIL_IF_MSG(!pr_aspect_edit(which, PR_PPC_DEXCR_CLEAR_ASPECT), "prctl failed");
	FAIL_IF_MSG(pr_aspect_check(which, UDEXCR),
		    "resetting aspect did not take effect");

	FAIL_IF_MSG(pr_aspect_get(which) != (PR_PPC_DEXCR_CLEAR_ASPECT | PR_PPC_DEXCR_PRCTL),
		    "prctl getter not reporting aspect state");

	FAIL_IF_MSG(!pr_aspect_edit(which, PR_PPC_DEXCR_SET_ASPECT), "prctl failed");
	FAIL_IF_MSG(!pr_aspect_check(which, UDEXCR),
		    "setting aspect did not take effect");

	FAIL_IF_MSG(pr_aspect_get(which) != (PR_PPC_DEXCR_SET_ASPECT | PR_PPC_DEXCR_PRCTL),
		    "prctl getter not reporting aspect state");

	FAIL_IF_MSG(!pr_aspect_edit(which, PR_PPC_DEXCR_CLEAR_ASPECT), "prctl failed");
	FAIL_IF_MSG(pr_aspect_check(which, UDEXCR),
		    "clearing aspect did not take effect");

	FAIL_IF_MSG(pr_aspect_get(which) != (PR_PPC_DEXCR_CLEAR_ASPECT | PR_PPC_DEXCR_PRCTL),
		    "prctl getter not reporting aspect state");

	pid = fork();
	if (pid == 0) {
		drop_cap_sysadmin();
		FAIL_IF_EXIT_MSG(pr_aspect_edit(which, PR_PPC_DEXCR_SET_ASPECT),
				 "prctl success when nonprivileged");
		FAIL_IF_EXIT_MSG(pr_aspect_check(which, UDEXCR),
				 "edited aspect when nonprivileged");
		_exit(0);
	}
	await_child_success(pid);

	FAIL_IF_MSG(!pr_aspect_edit(which, PR_PPC_DEXCR_FORCE_SET_ASPECT), "prctl force set failed");
	FAIL_IF_MSG(!pr_aspect_check(which, UDEXCR),
		    "force setting aspect did not take effect");

	FAIL_IF_MSG(pr_aspect_get(which) != (PR_PPC_DEXCR_FORCE_SET_ASPECT | PR_PPC_DEXCR_PRCTL),
		    "prctl getter not reporting aspect state");

	FAIL_IF_MSG(pr_aspect_edit(which, PR_PPC_DEXCR_CLEAR_ASPECT), "prctl success when forced");
	FAIL_IF_MSG(!pr_aspect_check(which, UDEXCR),
		    "edited aspect when forced");

	return 0;
}

static int dexcr_prctl_sbhe_test(void)
{
	sysctl_set_sbhe(-1);
	return dexcr_prctl_editable_aspect_test(PR_PPC_DEXCR_SBHE);
}

static int dexcr_prctl_ibrtpd_test(void)
{
	return dexcr_prctl_editable_aspect_test(PR_PPC_DEXCR_IBRTPD);
}

static int dexcr_prctl_srapd_test(void)
{
	return dexcr_prctl_editable_aspect_test(PR_PPC_DEXCR_SRAPD);
}

static int dexcr_sysctl_sbhe_test(void)
{
	SKIP_IF_MSG(!check_cap_sysadmin(), "must have capability CAP_SYS_ADMIN");
	SKIP_IF_MSG(!pr_aspect_supported(PR_PPC_DEXCR_SBHE), "aspect not supported");

	sysctl_set_sbhe(0);
	FAIL_IF_MSG(sysctl_get_sbhe() != 0, "failed to clear sysctl SBHE");
	FAIL_IF_MSG(pr_aspect_check(PR_PPC_DEXCR_SBHE, UDEXCR),
		    "SBHE failed to clear");

	sysctl_set_sbhe(1);
	FAIL_IF_MSG(sysctl_get_sbhe() != 1, "failed to set sysctl SBHE");
	FAIL_IF_MSG(!pr_aspect_check(PR_PPC_DEXCR_SBHE, UDEXCR),
		    "SBHE failed to set");

	sysctl_set_sbhe(-1);
	FAIL_IF_MSG(sysctl_get_sbhe() != -1, "failed to default sysctl SBHE");
	FAIL_IF_MSG(!pr_aspect_edit(PR_PPC_DEXCR_SBHE, PR_PPC_DEXCR_CLEAR_ASPECT), "prctl failed");
	FAIL_IF_MSG(pr_aspect_check(PR_PPC_DEXCR_SBHE, UDEXCR),
		    "SBHE failed to default to prctl clear setting");

	FAIL_IF_MSG(!pr_aspect_edit(PR_PPC_DEXCR_SBHE, PR_PPC_DEXCR_SET_ASPECT), "prctl failed");
	FAIL_IF_MSG(!pr_aspect_check(PR_PPC_DEXCR_SBHE, UDEXCR),
		    "SBHE failed to default to prctl set setting");

	sysctl_set_sbhe(0);
	FAIL_IF_MSG(sysctl_get_sbhe() != 0, "failed to clear sysctl SBHE");
	FAIL_IF_MSG(pr_aspect_check(PR_PPC_DEXCR_SBHE, UDEXCR),
		    "SBHE failed to override prctl setting");

	return 0;
}

static int dexcr_test_inherit_execve(char expected_dexcr)
{
	switch (expected_dexcr) {
	case '0':
		FAIL_IF_EXIT_MSG(pr_aspect_get(PR_PPC_DEXCR_IBRTPD) !=
				 (PR_PPC_DEXCR_CLEAR_ASPECT | PR_PPC_DEXCR_PRCTL),
				 "clearing IBRTPD across exec not inherited");

		FAIL_IF_EXIT_MSG(pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
				 "clearing IBRTPD across exec not applied");
		break;
	case '1':
		FAIL_IF_EXIT_MSG(pr_aspect_get(PR_PPC_DEXCR_IBRTPD) !=
				 (PR_PPC_DEXCR_SET_ASPECT | PR_PPC_DEXCR_PRCTL),
				 "setting IBRTPD across exec not inherited");

		FAIL_IF_EXIT_MSG(!pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
				 "setting IBRTPD across exec not applied");
		break;
	case '2':
		FAIL_IF_EXIT_MSG(pr_aspect_get(PR_PPC_DEXCR_IBRTPD) !=
				 (PR_PPC_DEXCR_FORCE_SET_ASPECT | PR_PPC_DEXCR_PRCTL),
				 "force setting IBRTPD across exec not inherited");

		FAIL_IF_EXIT_MSG(!pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
				 "force setting IBRTPD across exec not applied");
		break;
	}

	return 0;
}

/*
 * Check that a child process inherits the DEXCR over fork and execve
 */
static int dexcr_inherit_test(void)
{
	pid_t pid;

	SKIP_IF_MSG(!check_cap_sysadmin(), "must have capability CAP_SYS_ADMIN");
	SKIP_IF_MSG(!pr_aspect_supported(PR_PPC_DEXCR_IBRTPD), "IBRTPD not supported");

	pr_aspect_edit(PR_PPC_DEXCR_IBRTPD, PR_PPC_DEXCR_CLEAR_ASPECT);
	FAIL_IF_MSG(pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
		    "IBRTPD failed to clear");

	pid = fork();
	if (pid == 0) {
		char *args[] = { "dexcr_test_inherit_execve", "0", NULL };

		FAIL_IF_EXIT_MSG(pr_aspect_get(PR_PPC_DEXCR_IBRTPD) !=
				 (PR_PPC_DEXCR_CLEAR_ASPECT | PR_PPC_DEXCR_PRCTL),
				 "clearing IBRTPD not inherited");

		FAIL_IF_EXIT_MSG(pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
				 "clearing IBRTPD not applied");

		execve("/proc/self/exe", args, NULL);
		_exit(errno);
	}
	await_child_success(pid);

	pr_aspect_edit(PR_PPC_DEXCR_IBRTPD, PR_PPC_DEXCR_SET_ASPECT);
	FAIL_IF_MSG(!pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
		    "IBRTPD failed to set");

	pid = fork();
	if (pid == 0) {
		char *args[] = { "dexcr_test_inherit_execve", "1", NULL };

		FAIL_IF_EXIT_MSG(pr_aspect_get(PR_PPC_DEXCR_IBRTPD) !=
				 (PR_PPC_DEXCR_SET_ASPECT | PR_PPC_DEXCR_PRCTL),
				 "setting IBRTPD not inherited");

		FAIL_IF_EXIT_MSG(!pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
				 "setting IBRTPD not applied");

		execve("/proc/self/exe", args, NULL);
		_exit(errno);
	}
	await_child_success(pid);

	pr_aspect_edit(PR_PPC_DEXCR_IBRTPD, PR_PPC_DEXCR_FORCE_SET_ASPECT);
	FAIL_IF_MSG(!pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
		    "IBRTPD failed to force set");

	pid = fork();
	if (pid == 0) {
		char *args[] = { "dexcr_test_inherit_execve", "2", NULL };

		FAIL_IF_EXIT_MSG(pr_aspect_get(PR_PPC_DEXCR_IBRTPD) !=
				 (PR_PPC_DEXCR_FORCE_SET_ASPECT | PR_PPC_DEXCR_PRCTL),
				 "force setting IBRTPD not inherited");

		FAIL_IF_EXIT_MSG(!pr_aspect_check(PR_PPC_DEXCR_IBRTPD, UDEXCR),
				 "force setting IBRTPD not applied");

		execve("/proc/self/exe", args, NULL);
		_exit(errno);
	}
	await_child_success(pid);

	return 0;
}

int main(int argc, char *argv[])
{
	int err = 0;

	if (argc >= 2 && strcmp(argv[0], "dexcr_test_inherit_execve") == 0)
		return dexcr_test_inherit_execve(argv[1][0]);

	err |= test_harness(dexcr_prctl_sbhe_test, "dexcr_prctl_sbhe");
	err |= test_harness(dexcr_prctl_ibrtpd_test, "dexcr_prctl_ibrtpd");
	err |= test_harness(dexcr_prctl_srapd_test, "dexcr_prctl_srapd");
	err |= test_harness(dexcr_sysctl_sbhe_test, "dexcr_sysctl_sbhe");
	err |= test_harness(dexcr_inherit_test, "dexcr_inherit");

	return err;
}
