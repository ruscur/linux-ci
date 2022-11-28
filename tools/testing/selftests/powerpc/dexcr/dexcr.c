#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include "dexcr.h"
#include "reg.h"
#include "utils.h"

long sysctl_get_sbhe(void)
{
	long value;

	FAIL_IF_EXIT_MSG(read_long(SYSCTL_DEXCR_SBHE, &value, 10),
			 "failed to read " SYSCTL_DEXCR_SBHE);

	return value;
}

void sysctl_set_sbhe(long value)
{
	FAIL_IF_EXIT_MSG(write_long(SYSCTL_DEXCR_SBHE, value, 10),
			 "failed to write to " SYSCTL_DEXCR_SBHE);
}

unsigned int pr_aspect_to_dexcr_mask(unsigned long which)
{
	switch (which) {
	case PR_PPC_DEXCR_SBHE:
		return DEXCR_PRO_SBHE;
	case PR_PPC_DEXCR_IBRTPD:
		return DEXCR_PRO_IBRTPD;
	case PR_PPC_DEXCR_SRAPD:
		return DEXCR_PRO_SRAPD;
	case PR_PPC_DEXCR_NPHIE:
		return DEXCR_PRO_NPHIE;
	default:
		FAIL_IF_EXIT_MSG(true, "unknown PR aspect");
	}
}

static inline unsigned int get_dexcr_pro(void)
{
	return mfspr(SPRN_DEXCR);
}

static inline unsigned int get_dexcr_enf(void)
{
	return mfspr(SPRN_HDEXCR);
}

static inline unsigned int get_dexcr_eff(void)
{
	return get_dexcr_pro() | get_dexcr_enf();
}

unsigned int get_dexcr(enum DexcrSource source)
{
	switch (source) {
	case UDEXCR:
		return get_dexcr_pro();
	case ENFORCED:
		return get_dexcr_enf();
	case EFFECTIVE:
		return get_dexcr_eff();
	default:
		FAIL_IF_EXIT_MSG(true, "bad DEXCR source");
	}
}

bool pr_aspect_supported(unsigned long which)
{
	return prctl(PR_PPC_GET_DEXCR, which, 0, 0, 0) >= 0;
}

bool pr_aspect_editable(unsigned long which)
{
	int ret = prctl(PR_PPC_GET_DEXCR, which, 0, 0, 0);
	return ret > 0 && (ret & PR_PPC_DEXCR_PRCTL) > 0;
}

bool pr_aspect_edit(unsigned long which, unsigned long ctrl)
{
	return prctl(PR_PPC_SET_DEXCR, which, ctrl, 0, 0) == 0;
}

bool pr_aspect_check(unsigned long which, enum DexcrSource source)
{
	unsigned int dexcr = get_dexcr(source);
	unsigned int aspect = pr_aspect_to_dexcr_mask(which);
	return (dexcr & aspect) != 0;
}

int pr_aspect_get(unsigned long pr_aspect)
{
	int ret = prctl(PR_PPC_GET_DEXCR, pr_aspect, 0, 0, 0);
	FAIL_IF_EXIT_MSG(ret < 0, "prctl failed");
	return ret;
}

bool dexcr_pro_check(unsigned int pro, enum DexcrSource source)
{
	return (get_dexcr(source) & pro) != 0;
}

void await_child_success(pid_t pid)
{
	int wstatus;

	FAIL_IF_EXIT_MSG(pid == -1, "fork failed");
	FAIL_IF_EXIT_MSG(waitpid(pid, &wstatus, 0) == -1, "wait failed");
	FAIL_IF_EXIT_MSG(!WIFEXITED(wstatus), "child did not exit cleanly");
	FAIL_IF_EXIT_MSG(WEXITSTATUS(wstatus) != 0, "child exit error");
}
