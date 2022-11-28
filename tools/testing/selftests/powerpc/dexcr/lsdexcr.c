#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>

#include "dexcr.h"
#include "utils.h"

static unsigned int requested;
static unsigned int enforced;
static unsigned int effective;

struct dexcr_aspect {
	const char *name;
	const char *desc;
	unsigned int index;
	unsigned long pr_val;
};

static const struct dexcr_aspect aspects[] = {
	{
		.name = "SBHE",
		.desc = "Speculative branch hint enable",
		.index = 0,
		.pr_val = PR_PPC_DEXCR_SBHE,
	},
	{
		.name = "IBRTPD",
		.desc = "Indirect branch recurrent target prediction disable",
		.index = 3,
		.pr_val = PR_PPC_DEXCR_IBRTPD,
	},
	{
		.name = "SRAPD",
		.desc = "Subroutine return address prediction disable",
		.index = 4,
		.pr_val = PR_PPC_DEXCR_SRAPD,
	},
	{
		.name = "NPHIE",
		.desc = "Non-privileged hash instruction enable",
		.index = 5,
		.pr_val = PR_PPC_DEXCR_NPHIE,
	},
};

#define NUM_ASPECTS (sizeof(aspects) / sizeof(struct dexcr_aspect))

static void print_list(const char *list[], size_t len)
{
	for (size_t i = 0; i < len; i++) {
		printf("%s", list[i]);
		if (i + 1 < len)
			printf(", ");
	}
}

static void print_dexcr(char *name, unsigned int bits)
{
	const char *enabled_aspects[32] = {NULL};
	size_t j = 0;

	printf("%s: %08x", name, bits);

	if (bits == 0) {
		printf("\n");
		return;
	}

	for (size_t i = 0; i < NUM_ASPECTS; i++) {
		unsigned int mask = pr_aspect_to_dexcr_mask(aspects[i].pr_val);
		if (bits & mask) {
			enabled_aspects[j++] = aspects[i].name;
			bits &= ~mask;
		}
	}

	if (bits)
		enabled_aspects[j++] = "unknown";

	printf(" (");
	print_list(enabled_aspects, j);
	printf(")\n");
}

static void print_aspect(const struct dexcr_aspect *aspect)
{
	const char *attributes[32] = {NULL};
	size_t j = 0;
	unsigned long mask;
	int pr_status;

	/* Kernel-independent info about aspect */
	mask = pr_aspect_to_dexcr_mask(aspect->pr_val);
	if (requested & mask)
		attributes[j++] = "set";
	if (enforced & mask)
		attributes[j++] = "hypervisor enforced";
	if (!(effective & mask))
		attributes[j++] = "clear";

	/* Kernel understanding of the aspect */
	pr_status = prctl(PR_PPC_GET_DEXCR, aspect->pr_val, 0, 0, 0);
	if (pr_status == -1) {
		switch (errno) {
		case ENODEV:
			attributes[j++] = "aspect not present";
			break;
		case EINVAL:
			attributes[j++] = "unrecognised aspect";
			break;
		default:
			attributes[j++] = "unknown kernel error";
			break;
		}
	} else {
		if (pr_status & PR_PPC_DEXCR_SET_ASPECT)
			attributes[j++] = "prctl set";
		if (pr_status & PR_PPC_DEXCR_FORCE_SET_ASPECT)
			attributes[j++] = "prctl force set";
		if (pr_status & PR_PPC_DEXCR_CLEAR_ASPECT)
			attributes[j++] = "prctl clear";
		if (pr_status & PR_PPC_DEXCR_PRCTL)
			attributes[j++] = "prctl editable";
	}

	printf("%12s %c (%d): ", aspect->name, effective & mask ? '*' : ' ', aspect->index);
	print_list(attributes, j);
	printf("  \t(%s)\n", aspect->desc);
}

static void print_overrides(void) {
	long sbhe;
	int err;

	printf("Global SBHE override: ");
	if ((err = read_long(SYSCTL_DEXCR_SBHE, &sbhe, 10))) {
		printf("error reading " SYSCTL_DEXCR_SBHE ": %d (%s)\n", err, strerror(err));
	} else {
		const char *meaning;
		switch (sbhe) {
		case -1:
			meaning = "default";
			break;
		case 0:
			meaning = "clear";
			break;
		case 1:
			meaning = "set";
			break;
		default:
			meaning = "unknown";
		}

		printf("%ld (%s)\n", sbhe, meaning);
	}
}

int main(int argc, char *argv[])
{
	requested = get_dexcr(UDEXCR);
	enforced = get_dexcr(ENFORCED);
	effective = requested | enforced;

	print_dexcr("          Requested", requested);
	print_dexcr("Hypervisor enforced", enforced);
	print_dexcr("          Effective", effective);
	printf("\n");

	for (size_t i = 0; i < NUM_ASPECTS; i++)
		print_aspect(&aspects[i]);
	printf("\n");

	print_overrides();

	return 0;
}
