/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_L0_H
#define SPDK_INTERNAL_L0_H

#include "spdk/stdinc.h"
#include "spdk/env.h"

struct spdk_l0_region;

bool spdk_l0_data_pool_enabled(void);

struct spdk_l0_region *spdk_l0_region_create(const char *name, size_t len);
void spdk_l0_region_destroy(struct spdk_l0_region *region);

bool spdk_l0_find_region(const void *addr, struct spdk_l0_region **region, uint64_t *offset);
uint64_t spdk_l0_vtophys(const void *buf, uint64_t *size);

void *spdk_l0_region_base(struct spdk_l0_region *region);
size_t spdk_l0_region_len(struct spdk_l0_region *region);
uint64_t spdk_l0_region_phys(struct spdk_l0_region *region);
int spdk_l0_region_fd(struct spdk_l0_region *region);

struct spdk_mempool *spdk_l0_mempool_create(const char *name, size_t count,
		size_t ele_size, spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg);

#endif
