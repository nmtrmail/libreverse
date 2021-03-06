/* ref: https://github.com/bbu/userland-slab-allocator */

#pragma once
#ifndef __SLAB_H
#define __SLAB_H

#include <stdint.h>
#include <stddef.h>

//#include "dymelor.h"

#define IS_POWEROF2(x) (1UL << (1 + (63 - __builtin_clzl((x) - 1))))

struct slab_header {
	struct slab_header *prev, *next;
	uint64_t slots;
	uintptr_t refcount;
	struct slab_header *page;
	uint8_t data[] __attribute__ ((aligned(sizeof(void *))));
};

struct slab_chain {
	size_t itemsize, itemcount;
	size_t slabsize, pages_per_alloc;
	uint64_t initial_slotmask, empty_slotmask;
	uintptr_t alignment_mask;
	struct slab_header *partial, *empty, *full;
};

void slab_init(struct slab_chain *, size_t);
void *slab_alloc(struct slab_chain *);
void slab_free(struct slab_chain *, const void *);
void slab_destroy(const struct slab_chain *);

#endif /* __SLAB_H */
