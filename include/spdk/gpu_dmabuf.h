/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026.
 */

/** \file
 * GPU dma-buf memory domain helpers.
 */

#ifndef SPDK_GPU_DMABUF_H
#define SPDK_GPU_DMABUF_H

#include "spdk/dma.h"
#include "spdk/stdinc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Identifier for the SPDK GPU dma-buf memory domain.
 */
#define SPDK_GPU_DMABUF_DMA_DEVICE "SPDK_GPU_DMABUF_DMA_DEVICE"

/**
 * Options for creating a GPU dma-buf memory domain.
 */
struct spdk_gpu_dmabuf_memory_domain_opts {
	/** Size of this structure in bytes. */
	size_t size;

	/**
	 * Optional identifier for the memory domain. If NULL,
	 * SPDK_GPU_DMABUF_DMA_DEVICE is used.
	 */
	const char *id;

	/**
	 * CUDA device id whose primary context should be used for dma-buf export.
	 * Set to -1 to infer the CUDA device from each GPU pointer at registration
	 * or translation time.
	 */
	int cuda_device_id;

	/**
	 * Optional CUDA context handle. This is a CUcontext stored as an opaque pointer
	 * to avoid requiring CUDA headers in this public header.
	 */
	void *cuda_context;

	/**
	 * Optional RDMA access flags for ibv_reg_dmabuf_mr(). If 0, the implementation
	 * enables local write, remote read and remote write.
	 */
	uint32_t rdma_access_flags;
};

/**
 * Initialize GPU dma-buf memory domain options to defaults.
 *
 * \param opts Options structure to initialize.
 */
void spdk_gpu_dmabuf_memory_domain_get_opts(struct spdk_gpu_dmabuf_memory_domain_opts *opts);

/**
 * Create a GPU dma-buf memory domain.
 *
 * The created domain can be passed as spdk_nvme_ns_cmd_ext_io_opts::memory_domain.
 * Its translation callback exports CUDA memory as a dma-buf fd and registers it
 * with the destination RDMA memory domain using ibv_reg_dmabuf_mr().
 *
 * \param domain Output memory domain.
 * \param opts Creation options. May be NULL to use defaults.
 * \return 0 on success, negated errno on failure.
 */
int spdk_gpu_dmabuf_memory_domain_create(struct spdk_memory_domain **domain,
		const struct spdk_gpu_dmabuf_memory_domain_opts *opts);

/**
 * Pre-register a GPU address range with an RDMA memory domain.
 *
 * This exports the GPU address range as a dma-buf fd, registers it with the
 * RDMA protection domain associated with \b rdma_domain, and caches the
 * resulting RDMA memory region. Later NVMe-oF RDMA I/O using the same GPU
 * range and RDMA domain can reuse the cached lkey/rkey in the memory-domain
 * translation callback.
 *
 * \param domain GPU dma-buf memory domain.
 * \param rdma_domain RDMA memory domain returned by the SPDK RDMA transport.
 * \param addr GPU virtual address to register.
 * \param len Length in bytes.
 * \return 0 on success, negated errno on failure.
 */
int spdk_gpu_dmabuf_memory_domain_register(struct spdk_memory_domain *domain,
		struct spdk_memory_domain *rdma_domain, void *addr, size_t len);

/**
 * Drop cached RDMA registrations overlapping a GPU address range.
 *
 * This can be used before freeing or reusing GPU memory whose dma-buf MR has
 * previously been cached by this memory domain.
 *
 * \param domain GPU dma-buf memory domain.
 * \param addr Start address.
 * \param len Length in bytes.
 */
void spdk_gpu_dmabuf_memory_domain_invalidate(struct spdk_memory_domain *domain, void *addr,
		size_t len);

/**
 * Destroy a GPU dma-buf memory domain and release all cached RDMA registrations.
 *
 * \param domain Memory domain returned by spdk_gpu_dmabuf_memory_domain_create().
 */
void spdk_gpu_dmabuf_memory_domain_destroy(struct spdk_memory_domain *domain);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_GPU_DMABUF_H */
