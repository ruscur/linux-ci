// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2013-2015, Michael Ellerman, IBM Corp.
 */

#define _GNU_SOURCE	/* For CPU_ZERO etc. */

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <link.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <asm/unistd.h>
#include <linux/limits.h>

#include "utils.h"

static char auxv[4096];

int read_file(const char *path, char *buf, size_t count, size_t *len)
{
	ssize_t rc;
	int fd;
	int err;
	char eof;

	if ((fd = open(path, O_RDONLY)) < 0)
		return errno;

	if ((rc = read(fd, buf, count)) < 0) {
		err = errno;
		goto out;
	}

	if (len)
		*len = rc;

	/* Overflow if there are still more bytes after filling the buffer */
	if (rc == count && (rc = read(fd, &eof, 1)) != 0) {
		err = EOVERFLOW;
		goto out;
	}

	err = 0;

out:
	close(fd);
	return err;
}

int read_file_alloc(const char *path, char **buf, size_t *len)
{
	ssize_t rc;
	char *buffer;
	size_t read_offset;
	size_t length;
	int fd;
	int err;


	if ((fd = open(path, O_RDONLY)) < 0)
		return -errno;

	/*
	 * We don't use stat & preallocate st_size because some non-files
	 * report 0 file size. Instead just dynamically grow the buffer
	 * as needed.
	 */
	length = 4096;
	buffer = malloc(length);
	read_offset = 0;

	if (!buffer) {
		err = errno;
		goto out;
	}

	while (1) {
		if ((rc = read(fd, buffer + read_offset, length - read_offset)) < 0) {
			err = errno;
			goto out;
		}

		if (rc == 0)
			break;

		read_offset += rc;

		if (read_offset > length / 2) {
			char *next_buffer;

			length *= 2;
			next_buffer = realloc(buffer, length);
			if (!next_buffer) {
				err = errno;
				free(buffer);
				goto out;
			}
			buffer = next_buffer;
		}
	}

	*buf = buffer;
	if (len)
		*len = read_offset;

	err = 0;

out:
	close(fd);
	return err;
}

int write_file(const char *path, const char *buf, size_t count)
{
	int fd;
	int err;
	ssize_t rc;

	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC)) < 0)
		return errno;

	if ((rc = write(fd, buf, count)) < 0) {
		err = errno;
		goto out;
	}

	if (rc != count) {
		err = EOVERFLOW;
		goto out;
	}

	err = 0;

out:
	close(fd);
	return err;
}

int read_auxv(char *buf, ssize_t buf_size)
{
	int err;

	if ((err = read_file("/proc/self/auxv", buf, buf_size, NULL))) {
		fprintf(stderr, "Error reading auxv: %s\n", strerror(err));
		return err;
	}

	return 0;
}

int read_debugfs_file(const char *subpath, char *buf, size_t count)
{
	char path[PATH_MAX] = "/sys/kernel/debug/";

	strncat(path, subpath, sizeof(path) - strlen(path) - 1);

	return read_file(path, buf, count, NULL);
}

int write_debugfs_file(const char *subpath, const char *buf, size_t count)
{
	char path[PATH_MAX] = "/sys/kernel/debug/";

	strncat(path, subpath, sizeof(path) - strlen(path) - 1);

	return write_file(path, buf, count);
}

#define TYPE_MIN(x)				\
	_Generic((x),				\
		int:		INT_MIN,	\
		long:		LONG_MIN,	\
		unsigned int:	0,		\
		unsigned long:	0)

#define TYPE_MAX(x)				\
	_Generic((x),				\
		int:		INT_MAX,	\
		long:		LONG_MAX,	\
		unsigned int:	INT_MAX,	\
		unsigned long:	LONG_MAX)

#define define_parse_number(fn, type, super_type)				\
	int fn(const char *buffer, size_t count, type *result, int base)	\
	{									\
		char *end;							\
		super_type parsed;						\
										\
		errno = 0;							\
		parsed = _Generic(parsed,					\
				  intmax_t:	strtoimax,			\
				  uintmax_t:	strtoumax)(buffer, &end, base);	\
										\
		if (errno == ERANGE ||						\
		    parsed < TYPE_MIN(*result) || parsed > TYPE_MAX(*result))	\
			return ERANGE;						\
										\
		/* Require at least one digit */				\
		if (end == buffer)						\
			return EINVAL;						\
										\
		/* Require all remaining characters be whitespace-ish */	\
		for (; end < buffer + count; end++)				\
			if (!(*end == ' ' || *end == '\n' || *end == '\0'))	\
				return EINVAL;					\
										\
		*result = parsed;						\
		return 0;							\
	}

define_parse_number(parse_int, int, intmax_t);
define_parse_number(parse_long, long, intmax_t);
define_parse_number(parse_uint, unsigned int, uintmax_t);
define_parse_number(parse_ulong, unsigned long, uintmax_t);

int read_long(const char *path, long *result, int base)
{
	int err;
	char buffer[32] = {0};

	if ((err = read_file(path, buffer, sizeof(buffer) - 1, NULL)))
		return err;

	return parse_long(buffer, sizeof(buffer), result, base);
}

int read_ulong(const char *path, unsigned long *result, int base)
{
	int err;
	char buffer[32] = {0};

	if ((err = read_file(path, buffer, sizeof(buffer) - 1, NULL)))
		return err;

	return parse_ulong(buffer, sizeof(buffer), result, base);
}

int write_long(const char *path, long result, int base)
{
	int len;
	char buffer[32];

	/* Decimal only; we don't have a format specifier for signed hex values */
	if (base != 10)
		return EINVAL;

	len = snprintf(buffer, sizeof(buffer), "%ld", result);
	if (len < 0 || len >= sizeof(buffer))
		return EOVERFLOW;

	return write_file(path, buffer, len);
}

int write_ulong(const char *path, unsigned long result, int base)
{
	int len;
	char buffer[32];
	char *fmt;

	switch (base) {
	case 10:
		fmt = "%lu";
		break;
	case 16:
		fmt = "%lx";
		break;
	default:
		return EINVAL;
	}

	len = snprintf(buffer, sizeof(buffer), fmt, result);
	if (len < 0 || len >= sizeof(buffer))
		return -1;

	return write_file(path, buffer, len);
}

void *find_auxv_entry(int type, char *auxv)
{
	ElfW(auxv_t) *p;

	p = (ElfW(auxv_t) *)auxv;

	while (p->a_type != AT_NULL) {
		if (p->a_type == type)
			return p;

		p++;
	}

	return NULL;
}

void *get_auxv_entry(int type)
{
	ElfW(auxv_t) *p;

	if (read_auxv(auxv, sizeof(auxv)))
		return NULL;

	p = find_auxv_entry(type, auxv);
	if (p)
		return (void *)p->a_un.a_val;

	return NULL;
}

int pick_online_cpu(void)
{
	int ncpus, cpu = -1;
	cpu_set_t *mask;
	size_t size;

	ncpus = get_nprocs_conf();
	size = CPU_ALLOC_SIZE(ncpus);
	mask = CPU_ALLOC(ncpus);
	if (!mask) {
		perror("malloc");
		return -1;
	}

	CPU_ZERO_S(size, mask);

	if (sched_getaffinity(0, size, mask)) {
		perror("sched_getaffinity");
		goto done;
	}

	/* We prefer a primary thread, but skip 0 */
	for (cpu = 8; cpu < ncpus; cpu += 8)
		if (CPU_ISSET_S(cpu, size, mask))
			goto done;

	/* Search for anything, but in reverse */
	for (cpu = ncpus - 1; cpu >= 0; cpu--)
		if (CPU_ISSET_S(cpu, size, mask))
			goto done;

	printf("No cpus in affinity mask?!\n");

done:
	CPU_FREE(mask);
	return cpu;
}

bool is_ppc64le(void)
{
	struct utsname uts;
	int rc;

	errno = 0;
	rc = uname(&uts);
	if (rc) {
		perror("uname");
		return false;
	}

	return strcmp(uts.machine, "ppc64le") == 0;
}

int read_sysfs_file(char *fpath, char *result, size_t result_size)
{
	char path[PATH_MAX] = "/sys/";

	strncat(path, fpath, PATH_MAX - strlen(path) - 1);

	return read_file(path, result, result_size, NULL);
}

int read_debugfs_int(const char *debugfs_file, int *result)
{
	int err;
	char value[16] = {0};

	if ((err = read_debugfs_file(debugfs_file, value, sizeof(value) - 1)))
		return err;

	return parse_int(value, sizeof(value), result, 10);
}

int write_debugfs_int(const char *debugfs_file, int result)
{
	char value[16];

	snprintf(value, 16, "%d", result);

	return write_debugfs_file(debugfs_file, value, strlen(value));
}

static long perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
		int cpu, int group_fd, unsigned long flags)
{
	return syscall(__NR_perf_event_open, hw_event, pid, cpu,
		      group_fd, flags);
}

static void perf_event_attr_init(struct perf_event_attr *event_attr,
					unsigned int type,
					unsigned long config)
{
	memset(event_attr, 0, sizeof(*event_attr));

	event_attr->type = type;
	event_attr->size = sizeof(struct perf_event_attr);
	event_attr->config = config;
	event_attr->read_format = PERF_FORMAT_GROUP;
	event_attr->disabled = 1;
	event_attr->exclude_kernel = 1;
	event_attr->exclude_hv = 1;
	event_attr->exclude_guest = 1;
}

int perf_event_open_counter(unsigned int type,
			    unsigned long config, int group_fd)
{
	int fd;
	struct perf_event_attr event_attr;

	perf_event_attr_init(&event_attr, type, config);

	fd = perf_event_open(&event_attr, 0, -1, group_fd, 0);

	if (fd < 0)
		perror("perf_event_open() failed");

	return fd;
}

int perf_event_enable(int fd)
{
	if (ioctl(fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) == -1) {
		perror("error while enabling perf events");
		return -1;
	}

	return 0;
}

int perf_event_disable(int fd)
{
	if (ioctl(fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) == -1) {
		perror("error disabling perf events");
		return -1;
	}

	return 0;
}

int perf_event_reset(int fd)
{
	if (ioctl(fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) == -1) {
		perror("error resetting perf events");
		return -1;
	}

	return 0;
}

int using_hash_mmu(bool *using_hash)
{
	char line[128];
	FILE *f;
	int rc;

	f = fopen("/proc/cpuinfo", "r");
	FAIL_IF(!f);

	rc = 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		if (!strcmp(line, "MMU		: Hash\n") ||
		    !strcmp(line, "platform	: Cell\n") ||
		    !strcmp(line, "platform	: PowerMac\n")) {
			*using_hash = true;
			goto out;
		}

		if (strcmp(line, "MMU		: Radix\n") == 0) {
			*using_hash = false;
			goto out;
		}
	}

	rc = -1;
out:
	fclose(f);
	return rc;
}
