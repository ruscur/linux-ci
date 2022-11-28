#include <linux/capability.h>
#include <string.h>
#include <sys/syscall.h>

#include "cap.h"
#include "utils.h"

struct kernel_capabilities {
	struct __user_cap_header_struct header;

	struct __user_cap_data_struct data[_LINUX_CAPABILITY_U32S_3];
};

static void get_caps(struct kernel_capabilities *caps)
{
	FAIL_IF_EXIT_MSG(syscall(SYS_capget, &caps->header, &caps->data),
			 "cannot get capabilities");
}

static void set_caps(struct kernel_capabilities *caps)
{
	FAIL_IF_EXIT_MSG(syscall(SYS_capset, &caps->header, &caps->data),
			 "cannot set capabilities");
}

static void init_caps(struct kernel_capabilities *caps, pid_t pid)
{
	memset(caps, 0, sizeof(*caps));

	caps->header.version = _LINUX_CAPABILITY_VERSION_3;
	caps->header.pid = pid;

	get_caps(caps);
}

static bool has_cap(struct kernel_capabilities *caps, size_t cap)
{
	size_t data_index = cap / 32;
	size_t offset = cap % 32;

	FAIL_IF_EXIT_MSG(data_index >= ARRAY_SIZE(caps->data), "cap out of range");

	return caps->data[data_index].effective & (1 << offset);
}

static void drop_cap(struct kernel_capabilities *caps, size_t cap)
{
	size_t data_index = cap / 32;
	size_t offset = cap % 32;

	FAIL_IF_EXIT_MSG(data_index >= ARRAY_SIZE(caps->data), "cap out of range");

	caps->data[data_index].effective &= ~(1 << offset);
}

bool check_cap_sysadmin(void)
{
	struct kernel_capabilities caps;

	init_caps(&caps, 0);

	return has_cap(&caps, CAP_SYS_ADMIN);
}

void drop_cap_sysadmin(void)
{
	struct kernel_capabilities caps;

	init_caps(&caps, 0);
	drop_cap(&caps, CAP_SYS_ADMIN);
	set_caps(&caps);
}
