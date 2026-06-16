/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 SPDK contributors.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_QDLIMIT_H
#define SPDK_VBDEV_QDLIMIT_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

/* Per-core channel state. Touched only by its owning thread -> lock-free. */
struct qdlimit_io_channel {
	struct spdk_io_channel	*base_ch;	/* IO channel of base device */
	uint32_t		outstanding;	/* in-flight IO on this core */
	uint32_t		max_depth;	/* per-core cap, 0 = unlimited */
	STAILQ_HEAD(, qdlimit_bdev_io) queued_io; /* admitted-pending IO, FIFO */
};

/*
 * Return true if this IO type is subject to the depth limit. Management /
 * buffer-management ops bypass the limit and are never queued.
 */
static inline bool
qdlimit_io_is_limited(enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_COPY:
		return true;
	default:
		return false;
	}
}

/*
 * Try to reserve one in-flight slot. Returns true and increments the counter
 * when a slot is available (or when limiting is disabled), false otherwise.
 */
static inline bool
qdlimit_try_acquire(struct qdlimit_io_channel *qd_ch)
{
	if (qd_ch->max_depth != 0 && qd_ch->outstanding >= qd_ch->max_depth) {
		return false;
	}
	qd_ch->outstanding++;
	return true;
}

/* Release one previously-acquired in-flight slot. */
static inline void
qdlimit_release(struct qdlimit_io_channel *qd_ch)
{
	assert(qd_ch->outstanding > 0);
	qd_ch->outstanding--;
}

/**
 * Create a new qdlimit vbdev on top of an existing bdev.
 *
 * \param bdev_name Base bdev to attach to.
 * \param vbdev_name Name of the new qdlimit bdev.
 * \param queue_depth Per-core max in-flight IO (0 = unlimited).
 * \return 0 on success, negative errno on failure.
 */
int bdev_qdlimit_create_disk(const char *bdev_name, const char *vbdev_name,
			     uint32_t queue_depth);

/**
 * Delete a qdlimit vbdev.
 *
 * \param vbdev_name Name of the qdlimit bdev.
 * \param cb_fn Completion callback.
 * \param cb_arg Argument for cb_fn.
 */
void bdev_qdlimit_delete_disk(const char *vbdev_name, spdk_bdev_unregister_cb cb_fn,
			      void *cb_arg);

/**
 * Update the per-core queue depth of an existing qdlimit vbdev.
 *
 * \param vbdev_name Name of the qdlimit bdev.
 * \param queue_depth New per-core max in-flight IO (0 = unlimited).
 * \return 0 on success, negative errno on failure.
 */
int bdev_qdlimit_set_depth(const char *vbdev_name, uint32_t queue_depth);

#endif /* SPDK_VBDEV_QDLIMIT_H */
