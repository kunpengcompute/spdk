/* SPDX-License-Identifier: BSD-3-Clause */

#include "spdk_internal/l0.h"

/* External test backend, loaded after the executable was built with unresolved
 * weak references. This fixture performs no allocation or device operations. */
bool
spdk_l0_data_pool_enabled(void)
{
	return true;
}

struct spdk_mempool *
spdk_l0_mempool_create(const char *name, size_t count, size_t ele_size,
		       spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg)
{
	return NULL;
}
