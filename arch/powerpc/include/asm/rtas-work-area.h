/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef POWERPC_RTAS_WORK_AREA_H
#define POWERPC_RTAS_WORK_AREA_H

#include <linux/types.h>

#include <asm/page.h>

/**
 * struct rtas_work_area - RTAS work area descriptor.
 *
 * Descriptor for a "work area" in PAPR terminology that satisfies
 * RTAS addressing requirements.
 */
struct rtas_work_area {
	/* private: Use the APIs provided below. */
	char *buf;
	size_t size;
};

struct rtas_work_area *rtas_work_area_alloc(size_t size);
void rtas_work_area_free(struct rtas_work_area *area);

static inline char *rtas_work_area_raw_buf(const struct rtas_work_area *area)
{
	return area->buf;
}

static inline size_t rtas_work_area_size(const struct rtas_work_area *area)
{
	return area->size;
}

static inline phys_addr_t rtas_work_area_phys(const struct rtas_work_area *area)
{
	return __pa(area->buf);
}

/*
 * Early setup for the work area allocator. Call from
 * rtas_initialize() only.
 */
int rtas_work_area_reserve_arena(phys_addr_t);

#endif /* POWERPC_RTAS_WORK_AREA_H */
