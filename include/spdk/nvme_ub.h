/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Huawei Technologies Co., Ltd.
 */

/** \file
 * NVMe over UB transport extensions.
 */

#ifndef SPDK_NVME_UB_H
#define SPDK_NVME_UB_H

#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_nvme_ctrlr;

/**
 * Description of an NPU UB endpoint that can access registered NPU memory.
 *
 * rjetty_context points to the serialized urma_rjetty_t (including any
 * provider extension bytes) produced by the NPU-side control plane.  SPDK
 * copies the bytes before this function returns.
 */
struct spdk_nvme_ub_npu_endpoint_info {
	size_t size;
	uint32_t endpoint_id;
	uint32_t token;
	const void *rjetty_context;
	uint32_t rjetty_context_size;
};

/**
 * Description of an NPU HBM region registered with the NPU UB device.
 *
 * user_base is the address seen by the initiator application.  segment_context
 * points to the serialized urma_seg_t (including provider extension bytes)
 * returned by NPU-side memory registration.  The segment's UBVA is advertised
 * to the target; user_base is used only to translate application I/O buffers.
 */
struct spdk_nvme_ub_npu_region_info {
	size_t size;
	uint32_t endpoint_id;
	uint32_t token;
	uint64_t user_base;
	uint64_t length;
	const void *segment_context;
	uint32_t segment_context_size;
};

/**
 * Register an NPU endpoint on a UB controller.
 *
 * Registration must be completed before any I/O qpair is allocated. Endpoint
 * IDs are scoped to the controller and therefore may be reused by other
 * initiators.
 */
int spdk_nvme_ub_register_npu_endpoint(struct spdk_nvme_ctrlr *ctrlr,
					       const struct spdk_nvme_ub_npu_endpoint_info *info);

/** Unregister an NPU endpoint.  Fails if a region still references it. */
int spdk_nvme_ub_unregister_npu_endpoint(struct spdk_nvme_ctrlr *ctrlr,
						 uint32_t endpoint_id);

/**
 * Register an NPU HBM region and allocate its controller-local region ID.
 *
 * The returned ID is carried in the vendor-specific NVMe SGL descriptor.  It
 * is scoped by the target qpair/session, so different initiators can safely
 * use the same numeric ID.
 */
int spdk_nvme_ub_register_npu_region(struct spdk_nvme_ctrlr *ctrlr,
					     const struct spdk_nvme_ub_npu_region_info *info,
					     uint32_t *region_id);

/** Unregister an NPU HBM region. */
int spdk_nvme_ub_unregister_npu_region(struct spdk_nvme_ctrlr *ctrlr,
					       uint32_t region_id);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_NVME_UB_H */
