/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_NVMF_QDLIMIT_H
#define SPDK_NVMF_QDLIMIT_H

#include "spdk/stdinc.h"

struct spdk_nvmf_transport_poll_group;
struct spdk_nvmf_request;
struct spdk_json_write_ctx;

/* Result of the admission gate. */
enum nvmf_qdlimit_status {
	NVMF_QDLIMIT_ADMIT,	/* under limit (or unlimited): proceed to get_buffers */
	NVMF_QDLIMIT_THROTTLED,	/* over limit: request parked on a per-SSD wait queue */
};

/* Per-poll-group context lifecycle. Called from the RDMA poll-group create/destroy paths. */
void nvmf_qdlimit_pg_init(struct spdk_nvmf_transport_poll_group *group);
void nvmf_qdlimit_pg_fini(struct spdk_nvmf_transport_poll_group *group);
void nvmf_qdlimit_pg_fini_drain(struct spdk_nvmf_transport_poll_group *group);

/* Admission gate. Call at NEW->NEED_BUFFER, before get_buffers, only for the queue head.
 * On NVMF_QDLIMIT_THROTTLED the module has already removed req from group->pending_buf_queue
 * and parked it on the per-SSD wait queue; the caller must stop processing this request. */
enum nvmf_qdlimit_status nvmf_qdlimit_admit(struct spdk_nvmf_transport_poll_group *group,
		struct spdk_nvmf_request *req);

/* Slot release. Call from _nvmf_rdma_request_free. No-op unless req->qdlimit_charged.
 * Decrements the per-core counter and re-arms one parked waiter for the same SSD. */
void nvmf_qdlimit_release(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_request *req);

/* Remove req from its per-SSD wait queue if parked there (transport abort path).
 * Returns true if it was parked and removed, false otherwise. */
bool nvmf_qdlimit_abort_dequeue(struct spdk_nvmf_transport_poll_group *group,
			       struct spdk_nvmf_request *req);

/* Config (global, per backing bdev). depth == 0 means unlimited. Returns 0 on success,
 * negative errno on failure. Safe to call from the RPC thread. */
int nvmf_qdlimit_set_depth(const char *bdev_name, uint32_t depth);
int nvmf_qdlimit_get_depth(const char *bdev_name, uint32_t *depth);

/* Free all global config state. Call at transport/library teardown. */
void nvmf_qdlimit_config_cleanup(void);

/* TODO: nvmf_qdlimit_get_stats — per-core in-flight counters live in per-poll-group context on
 * individual reactor threads; gathering them requires an spdk_for_each_channel()-style fan-out
 * across all poll groups. Deferred: the buffer-occupancy ceiling test (Task 7) derives occupancy
 * from the transport mempool counter (nvmf_get_stats pending_data_buffer) instead, so get_stats
 * is not on the critical path for the current feature set. */

#endif /* SPDK_NVMF_QDLIMIT_H */
