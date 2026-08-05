/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026.
 */

#include "spdk/gpu_dmabuf.h"

#include "spdk/log.h"
#include "spdk/likely.h"
#include "spdk/tree.h"
#include "spdk/util.h"

#include <cuda.h>
#include <infiniband/verbs.h>

struct gpu_dmabuf_mr {
	void *addr;
	size_t len;
	int device_id;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	RB_ENTRY(gpu_dmabuf_mr) node;
};

RB_HEAD(gpu_dmabuf_mr_tree, gpu_dmabuf_mr);

struct gpu_dmabuf_region {
	void *addr;
	size_t len;
	int device_id;
	RB_ENTRY(gpu_dmabuf_region) node;
};

RB_HEAD(gpu_dmabuf_region_tree, gpu_dmabuf_region);

struct gpu_dmabuf_cuda_ctx {
	int device_id;
	CUcontext cuda_ctx;
	TAILQ_ENTRY(gpu_dmabuf_cuda_ctx) link;
};

struct gpu_dmabuf_domain {
	struct spdk_memory_domain *domain;
	pthread_mutex_t lock;
	struct gpu_dmabuf_mr_tree mr_cache;
	struct gpu_dmabuf_region_tree regions;
	TAILQ_HEAD(, gpu_dmabuf_cuda_ctx) cuda_ctxs;
	int cuda_device_id;
	CUcontext cuda_ctx;
	bool owns_cuda_ctx;
	uint32_t rdma_access_flags;
	TAILQ_ENTRY(gpu_dmabuf_domain) link;
};

static pthread_mutex_t g_gpu_dmabuf_domains_lock = PTHREAD_MUTEX_INITIALIZER;
static TAILQ_HEAD(, gpu_dmabuf_domain) g_gpu_dmabuf_domains = TAILQ_HEAD_INITIALIZER(
			g_gpu_dmabuf_domains);

static int
gpu_dmabuf_mr_compare(struct gpu_dmabuf_mr *a, struct gpu_dmabuf_mr *b)
{
	uintptr_t a_pd = (uintptr_t)a->pd;
	uintptr_t b_pd = (uintptr_t)b->pd;
	uintptr_t a_addr = (uintptr_t)a->addr;
	uintptr_t b_addr = (uintptr_t)b->addr;

	if (a->device_id != b->device_id) {
		return a->device_id < b->device_id ? -1 : 1;
	}

	if (a_pd != b_pd) {
		return a_pd < b_pd ? -1 : 1;
	}

	if (a_addr != b_addr) {
		return a_addr < b_addr ? -1 : 1;
	}

	return 0;
}

RB_GENERATE_STATIC(gpu_dmabuf_mr_tree, gpu_dmabuf_mr, node, gpu_dmabuf_mr_compare);

static int
gpu_dmabuf_region_compare(struct gpu_dmabuf_region *a, struct gpu_dmabuf_region *b)
{
	uintptr_t a_addr = (uintptr_t)a->addr;
	uintptr_t b_addr = (uintptr_t)b->addr;

	if (a_addr != b_addr) {
		return a_addr < b_addr ? -1 : 1;
	}

	return 0;
}

RB_GENERATE_STATIC(gpu_dmabuf_region_tree, gpu_dmabuf_region, node,
		   gpu_dmabuf_region_compare);

#define GPU_DMABUF_OPTS_HAS(opts, field) \
	((opts)->size >= offsetof(struct spdk_gpu_dmabuf_memory_domain_opts, field) + sizeof((opts)->field))

static uint32_t
gpu_dmabuf_default_access_flags(void)
{
	uint32_t flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

#ifdef IBV_ACCESS_RELAXED_ORDERING
	flags |= IBV_ACCESS_RELAXED_ORDERING;
#endif

	return flags;
}

void
spdk_gpu_dmabuf_memory_domain_get_opts(struct spdk_gpu_dmabuf_memory_domain_opts *opts)
{
	if (opts == NULL) {
		return;
	}

	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->cuda_device_id = -1;
	opts->rdma_access_flags = gpu_dmabuf_default_access_flags();
}

static struct gpu_dmabuf_domain *
gpu_dmabuf_domain_find(struct spdk_memory_domain *domain)
{
	struct gpu_dmabuf_domain *gd;

	pthread_mutex_lock(&g_gpu_dmabuf_domains_lock);
	TAILQ_FOREACH(gd, &g_gpu_dmabuf_domains, link) {
		if (gd->domain == domain) {
			pthread_mutex_unlock(&g_gpu_dmabuf_domains_lock);
			return gd;
		}
	}
	pthread_mutex_unlock(&g_gpu_dmabuf_domains_lock);

	return NULL;
}

static bool
gpu_dmabuf_range_contains(const struct gpu_dmabuf_mr *entry, void *addr, size_t len)
{
	uintptr_t entry_start = (uintptr_t)entry->addr;
	uintptr_t entry_end = entry_start + entry->len;
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end = start + len;

	return start >= entry_start && end >= start && end <= entry_end;
}

static bool
gpu_dmabuf_range_overlaps(const struct gpu_dmabuf_mr *entry, void *addr, size_t len)
{
	uintptr_t entry_start = (uintptr_t)entry->addr;
	uintptr_t entry_end = entry_start + entry->len;
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end = start + len;

	return end > start && entry_end > entry_start && start < entry_end && entry_start < end;
}

static bool
gpu_dmabuf_region_contains(const struct gpu_dmabuf_region *region, void *addr, size_t len)
{
	uintptr_t region_start = (uintptr_t)region->addr;
	uintptr_t region_end = region_start + region->len;
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end = start + len;

	return start >= region_start && end >= start && end <= region_end;
}

static bool
gpu_dmabuf_region_overlaps(const struct gpu_dmabuf_region *region, void *addr, size_t len)
{
	uintptr_t region_start = (uintptr_t)region->addr;
	uintptr_t region_end = region_start + region->len;
	uintptr_t start = (uintptr_t)addr;
	uintptr_t end = start + len;

	return end > start && region_end > region_start && start < region_end && region_start < end;
}

static int
gpu_dmabuf_get_rdma_pd(struct spdk_memory_domain *dst_domain, struct ibv_pd **pd)
{
	struct spdk_memory_domain_ctx *ctx;
	struct spdk_memory_domain_rdma_ctx *rdma_ctx;

	if (spdk_memory_domain_get_dma_device_type(dst_domain) != SPDK_DMA_DEVICE_TYPE_RDMA) {
		return -ENOTSUP;
	}

	ctx = spdk_memory_domain_get_context(dst_domain);
	if (ctx == NULL || ctx->user_ctx == NULL) {
		return -EINVAL;
	}

	rdma_ctx = ctx->user_ctx;
	if (rdma_ctx->size < offsetof(struct spdk_memory_domain_rdma_ctx, ibv_pd) +
	    sizeof(rdma_ctx->ibv_pd)) {
		return -EINVAL;
	}

	*pd = rdma_ctx->ibv_pd;
	if (*pd == NULL) {
		return -EINVAL;
	}

	return 0;
}

static int
gpu_dmabuf_get_ptr_device_id(struct gpu_dmabuf_domain *gd, void *addr, int *device_id)
{
	CUresult cu_rc;
	int detected_device_id;

	if (gd->cuda_device_id >= 0) {
		*device_id = gd->cuda_device_id;
		return 0;
	}

	cu_rc = cuPointerGetAttribute(&detected_device_id, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL,
				      (CUdeviceptr)addr);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuPointerGetAttribute(DEVICE_ORDINAL) failed: %d\n", cu_rc);
		return -EFAULT;
	}

	*device_id = detected_device_id;
	return 0;
}

static int
gpu_dmabuf_get_cuda_ctx(struct gpu_dmabuf_domain *gd, int device_id, CUcontext *cuda_ctx)
{
	struct gpu_dmabuf_cuda_ctx *ctx_entry;
	CUdevice cuda_device;
	CUresult cu_rc;

	if (gd->cuda_ctx != NULL) {
		*cuda_ctx = gd->cuda_ctx;
		return 0;
	}

	if (device_id < 0) {
		return -EINVAL;
	}

	TAILQ_FOREACH(ctx_entry, &gd->cuda_ctxs, link) {
		if (ctx_entry->device_id == device_id) {
			*cuda_ctx = ctx_entry->cuda_ctx;
			return 0;
		}
	}

	ctx_entry = calloc(1, sizeof(*ctx_entry));
	if (ctx_entry == NULL) {
		return -ENOMEM;
	}

	cu_rc = cuDeviceGet(&cuda_device, device_id);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuDeviceGet(%d) failed: %d\n", device_id, cu_rc);
		free(ctx_entry);
		return -ENODEV;
	}

	cu_rc = cuDevicePrimaryCtxRetain(&ctx_entry->cuda_ctx, cuda_device);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuDevicePrimaryCtxRetain(%d) failed: %d\n", device_id, cu_rc);
		free(ctx_entry);
		return -ENODEV;
	}

	ctx_entry->device_id = device_id;
	TAILQ_INSERT_TAIL(&gd->cuda_ctxs, ctx_entry, link);
	*cuda_ctx = ctx_entry->cuda_ctx;
	return 0;
}

static const char *
gpu_dmabuf_cuda_errstr(CUresult cu_rc)
{
	const char *errstr = NULL;

	if (cuGetErrorString(cu_rc, &errstr) == CUDA_SUCCESS && errstr != NULL) {
		return errstr;
	}

	return "unknown CUDA error";
}

static int
gpu_dmabuf_set_cuda_ctx(CUcontext cuda_ctx, CUcontext *prev_ctx, bool *changed)
{
	CUresult cu_rc;

	*prev_ctx = NULL;
	*changed = false;
	if (cuda_ctx == NULL) {
		return 0;
	}

	cu_rc = cuCtxGetCurrent(prev_ctx);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuCtxGetCurrent() failed: %d (%s)\n", cu_rc,
			    gpu_dmabuf_cuda_errstr(cu_rc));
		return -EFAULT;
	}

	if (*prev_ctx == cuda_ctx) {
		return 0;
	}

	cu_rc = cuCtxSetCurrent(cuda_ctx);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuCtxSetCurrent() failed: %d (%s)\n", cu_rc,
			    gpu_dmabuf_cuda_errstr(cu_rc));
		return -EFAULT;
	}

	*changed = true;
	return 0;
}

static void
gpu_dmabuf_restore_cuda_ctx(CUcontext prev_ctx, bool changed)
{
	CUresult cu_rc;

	if (!changed) {
		return;
	}

	cu_rc = cuCtxSetCurrent(prev_ctx);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuCtxSetCurrent(previous) failed: %d (%s)\n", cu_rc,
			    gpu_dmabuf_cuda_errstr(cu_rc));
	}
}

static struct gpu_dmabuf_mr *
gpu_dmabuf_find_mr(struct gpu_dmabuf_domain *gd, int device_id, struct ibv_pd *pd, void *addr,
		   size_t len)
{
	struct gpu_dmabuf_mr find = {};
	struct gpu_dmabuf_mr *entry, *prev;

	find.device_id = device_id;
	find.pd = pd;
	find.addr = addr;

	entry = RB_NFIND(gpu_dmabuf_mr_tree, &gd->mr_cache, &find);
	if (entry != NULL && entry->device_id == device_id && entry->pd == pd &&
	    gpu_dmabuf_range_contains(entry, addr, len)) {
		return entry;
	}

	prev = entry != NULL ? RB_PREV(gpu_dmabuf_mr_tree, &gd->mr_cache, entry) :
	       RB_MAX(gpu_dmabuf_mr_tree, &gd->mr_cache);
	if (prev != NULL && prev->device_id == device_id && prev->pd == pd &&
	    gpu_dmabuf_range_contains(prev, addr, len)) {
		return prev;
	}

	return NULL;
}

static struct gpu_dmabuf_mr *
gpu_dmabuf_find_overlapping_mr(struct gpu_dmabuf_domain *gd, int device_id, struct ibv_pd *pd,
			       void *addr, size_t len)
{
	struct gpu_dmabuf_mr find = {};
	struct gpu_dmabuf_mr *entry, *prev;

	find.device_id = device_id;
	find.pd = pd;
	find.addr = addr;

	entry = RB_NFIND(gpu_dmabuf_mr_tree, &gd->mr_cache, &find);
	if (entry != NULL && entry->device_id == device_id && entry->pd == pd &&
	    gpu_dmabuf_range_overlaps(entry, addr, len)) {
		return entry;
	}

	prev = entry != NULL ? RB_PREV(gpu_dmabuf_mr_tree, &gd->mr_cache, entry) :
	       RB_MAX(gpu_dmabuf_mr_tree, &gd->mr_cache);
	if (prev != NULL && prev->device_id == device_id && prev->pd == pd &&
	    gpu_dmabuf_range_overlaps(prev, addr, len)) {
		return prev;
	}

	return NULL;
}

static struct gpu_dmabuf_region *
gpu_dmabuf_find_region(struct gpu_dmabuf_domain *gd, void *addr, size_t len)
{
	struct gpu_dmabuf_region find = {};
	struct gpu_dmabuf_region *entry, *prev;

	find.addr = addr;

	entry = RB_NFIND(gpu_dmabuf_region_tree, &gd->regions, &find);
	if (entry != NULL && gpu_dmabuf_region_contains(entry, addr, len)) {
		return entry;
	}

	prev = entry != NULL ? RB_PREV(gpu_dmabuf_region_tree, &gd->regions, entry) :
	       RB_MAX(gpu_dmabuf_region_tree, &gd->regions);
	if (prev != NULL && gpu_dmabuf_region_contains(prev, addr, len)) {
		return prev;
	}

	return NULL;
}

static struct gpu_dmabuf_region *
gpu_dmabuf_find_overlapping_region(struct gpu_dmabuf_domain *gd, void *addr, size_t len)
{
	struct gpu_dmabuf_region find = {};
	struct gpu_dmabuf_region *entry, *prev;

	find.addr = addr;

	entry = RB_NFIND(gpu_dmabuf_region_tree, &gd->regions, &find);
	if (entry != NULL && gpu_dmabuf_region_overlaps(entry, addr, len)) {
		return entry;
	}

	prev = entry != NULL ? RB_PREV(gpu_dmabuf_region_tree, &gd->regions, entry) :
	       RB_MAX(gpu_dmabuf_region_tree, &gd->regions);
	if (prev != NULL && gpu_dmabuf_region_overlaps(prev, addr, len)) {
		return prev;
	}

	return NULL;
}

static int
gpu_dmabuf_add_region(struct gpu_dmabuf_domain *gd, void *addr, size_t len, int device_id,
		      struct gpu_dmabuf_region **_region)
{
	struct gpu_dmabuf_region *region, *overlap;

	region = gpu_dmabuf_find_region(gd, addr, len);
	if (region != NULL) {
		if (region->device_id != device_id) {
			return -EINVAL;
		}
		*_region = region;
		return 0;
	}

	overlap = gpu_dmabuf_find_overlapping_region(gd, addr, len);
	if (overlap != NULL) {
		return -EINVAL;
	}

	region = calloc(1, sizeof(*region));
	if (region == NULL) {
		return -ENOMEM;
	}

	region->addr = addr;
	region->len = len;
	region->device_id = device_id;
	if (RB_INSERT(gpu_dmabuf_region_tree, &gd->regions, region) != NULL) {
		free(region);
		return -EEXIST;
	}

	*_region = region;
	return 0;
}

static int
gpu_dmabuf_register_mr(struct gpu_dmabuf_domain *gd, int device_id, struct ibv_pd *pd, void *addr,
		       size_t len, struct gpu_dmabuf_mr **_entry)
{
	struct gpu_dmabuf_mr *entry;
	CUcontext cuda_ctx;
	CUcontext prev_ctx;
	CUresult cu_rc;
	CUdeviceptr alloc_base;
	size_t alloc_size;
	uintptr_t alloc_offset;
	uint64_t dmabuf_offset;
	bool ctx_changed;
	int dmabuf_fd = -1;
	int rc;

	if (gpu_dmabuf_find_overlapping_mr(gd, device_id, pd, addr, len) != NULL) {
		return -EEXIST;
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return -ENOMEM;
	}

	rc = gpu_dmabuf_get_cuda_ctx(gd, device_id, &cuda_ctx);
	if (rc != 0) {
		free(entry);
		return rc;
	}

	rc = gpu_dmabuf_set_cuda_ctx(cuda_ctx, &prev_ctx, &ctx_changed);
	if (rc != 0) {
		free(entry);
		return rc;
	}

	cu_rc = cuMemGetAddressRange(&alloc_base, &alloc_size, (CUdeviceptr)addr);
	if (cu_rc != CUDA_SUCCESS) {
		gpu_dmabuf_restore_cuda_ctx(prev_ctx, ctx_changed);
		SPDK_ERRLOG("cuMemGetAddressRange(addr=%p, len=%zu, device_id=%d) failed: %d (%s)\n",
			    addr, len, device_id, cu_rc, gpu_dmabuf_cuda_errstr(cu_rc));
		free(entry);
		return -EFAULT;
	}

	if ((uintptr_t)addr < (uintptr_t)alloc_base) {
		gpu_dmabuf_restore_cuda_ctx(prev_ctx, ctx_changed);
		free(entry);
		return -EINVAL;
	}

	alloc_offset = (uintptr_t)addr - (uintptr_t)alloc_base;
	if (alloc_offset > alloc_size || len > alloc_size - alloc_offset) {
		gpu_dmabuf_restore_cuda_ctx(prev_ctx, ctx_changed);
		SPDK_ERRLOG("GPU buffer range is outside CUDA allocation: addr=%p len=%zu base=0x%" PRIx64
			    " alloc_size=%zu offset=%" PRIuPTR "\n",
			    addr, len, (uint64_t)alloc_base, alloc_size, alloc_offset);
		free(entry);
		return -EINVAL;
	}

	cu_rc = cuMemGetHandleForAddressRange(&dmabuf_fd, alloc_base, alloc_size,
					      CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
	gpu_dmabuf_restore_cuda_ctx(prev_ctx, ctx_changed);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuMemGetHandleForAddressRange(addr=%p, len=%zu, device_id=%d, base=0x%" PRIx64
			    ", alloc_size=%zu, offset=%" PRIuPTR ") failed: %d (%s)\n",
			    addr, len, device_id, (uint64_t)alloc_base, alloc_size, alloc_offset, cu_rc,
			    gpu_dmabuf_cuda_errstr(cu_rc));
		free(entry);
		return -EFAULT;
	}

	dmabuf_offset = (uint64_t)alloc_offset;
	entry->mr = ibv_reg_dmabuf_mr(pd, dmabuf_offset, len, (uint64_t)(uintptr_t)addr, dmabuf_fd,
				      gd->rdma_access_flags);
	rc = errno;
	close(dmabuf_fd);
	if (entry->mr == NULL) {
		SPDK_ERRLOG("ibv_reg_dmabuf_mr(addr=%p, len=%zu, device_id=%d, dmabuf_offset=%" PRIu64
			    ") failed: %d\n",
			    addr, len, device_id, dmabuf_offset, rc);
		free(entry);
		return rc ? -rc : -EFAULT;
	}

	entry->addr = addr;
	entry->len = len;
	entry->device_id = device_id;
	entry->pd = pd;
	if (RB_INSERT(gpu_dmabuf_mr_tree, &gd->mr_cache, entry) != NULL) {
		ibv_dereg_mr(entry->mr);
		free(entry);
		return -EEXIST;
	}
	*_entry = entry;

	return 0;
}

int
spdk_gpu_dmabuf_memory_domain_register(struct spdk_memory_domain *domain,
				       struct spdk_memory_domain *rdma_domain, void *addr, size_t len)
{
	struct gpu_dmabuf_domain *gd;
	struct gpu_dmabuf_mr *entry;
	struct gpu_dmabuf_region *region;
	struct ibv_pd *pd;
	int device_id;
	int rc;

	if (addr == NULL || len == 0) {
		return -EINVAL;
	}

	gd = gpu_dmabuf_domain_find(domain);
	if (gd == NULL) {
		return -EINVAL;
	}

	rc = gpu_dmabuf_get_rdma_pd(rdma_domain, &pd);
	if (rc != 0) {
		return rc;
	}

	rc = gpu_dmabuf_get_ptr_device_id(gd, addr, &device_id);
	if (rc != 0) {
		return rc;
	}

	pthread_mutex_lock(&gd->lock);
	rc = gpu_dmabuf_add_region(gd, addr, len, device_id, &region);
	if (rc != 0) {
		pthread_mutex_unlock(&gd->lock);
		return rc;
	}

	entry = gpu_dmabuf_find_mr(gd, region->device_id, pd, region->addr, region->len);
	if (entry == NULL) {
		rc = gpu_dmabuf_register_mr(gd, region->device_id, pd, region->addr, region->len,
					    &entry);
	}
	pthread_mutex_unlock(&gd->lock);

	return rc;
}

static int
gpu_dmabuf_translate(struct spdk_memory_domain *src_domain, void *src_domain_ctx,
		     struct spdk_memory_domain *dst_domain,
		     struct spdk_memory_domain_translation_ctx *dst_domain_ctx, void *addr, size_t len,
		     struct spdk_memory_domain_translation_result *result)
{
	struct gpu_dmabuf_domain *gd;
	struct gpu_dmabuf_mr *entry;
	struct gpu_dmabuf_region *region;
	struct ibv_pd *pd;
	int rc;

	(void)src_domain_ctx;
	(void)dst_domain_ctx;

	gd = gpu_dmabuf_domain_find(src_domain);
	if (gd == NULL) {
		return -EINVAL;
	}

	rc = gpu_dmabuf_get_rdma_pd(dst_domain, &pd);
	if (rc != 0) {
		return rc;
	}

	pthread_mutex_lock(&gd->lock);
	region = gpu_dmabuf_find_region(gd, addr, len);
	if (region == NULL) {
		pthread_mutex_unlock(&gd->lock);
		return -EINVAL;
	}

	entry = gpu_dmabuf_find_mr(gd, region->device_id, pd, addr, len);
	if (entry == NULL) {
		rc = gpu_dmabuf_register_mr(gd, region->device_id, pd, region->addr, region->len,
					    &entry);
		if (rc != 0) {
			pthread_mutex_unlock(&gd->lock);
			return rc;
		}
	}

	result->size = sizeof(*result);
	result->iov_count = 1;
	result->iov.iov_base = addr;
	result->iov.iov_len = len;
	result->dst_domain = dst_domain;
	result->rdma.lkey = entry->mr->lkey;
	result->rdma.rkey = entry->mr->rkey;
	pthread_mutex_unlock(&gd->lock);

	return 0;
}

static void
gpu_dmabuf_dereg_mr(struct gpu_dmabuf_mr *entry)
{
	if (entry->mr != NULL) {
		ibv_dereg_mr(entry->mr);
	}

	free(entry);
}

static void
gpu_dmabuf_release_cuda_ctxs(struct gpu_dmabuf_domain *gd)
{
	struct gpu_dmabuf_cuda_ctx *ctx_entry;

	while ((ctx_entry = TAILQ_FIRST(&gd->cuda_ctxs)) != NULL) {
		TAILQ_REMOVE(&gd->cuda_ctxs, ctx_entry, link);
		cuDevicePrimaryCtxRelease(ctx_entry->device_id);
		free(ctx_entry);
	}
}

static void
gpu_dmabuf_invalidate_range(struct gpu_dmabuf_domain *gd, void *addr, size_t len)
{
	struct gpu_dmabuf_mr *entry, *tmp;
	struct gpu_dmabuf_region *region, *region_tmp;

	if (addr == NULL || len == 0) {
		return;
	}

	pthread_mutex_lock(&gd->lock);
	RB_FOREACH_SAFE(entry, gpu_dmabuf_mr_tree, &gd->mr_cache, tmp) {
		if (gpu_dmabuf_range_overlaps(entry, addr, len)) {
			RB_REMOVE(gpu_dmabuf_mr_tree, &gd->mr_cache, entry);
			gpu_dmabuf_dereg_mr(entry);
		}
	}
	RB_FOREACH_SAFE(region, gpu_dmabuf_region_tree, &gd->regions, region_tmp) {
		if (gpu_dmabuf_region_overlaps(region, addr, len)) {
			RB_REMOVE(gpu_dmabuf_region_tree, &gd->regions, region);
			free(region);
		}
	}
	pthread_mutex_unlock(&gd->lock);
}

int
spdk_gpu_dmabuf_memory_domain_create(struct spdk_memory_domain **domain,
				     const struct spdk_gpu_dmabuf_memory_domain_opts *opts)
{
	struct spdk_gpu_dmabuf_memory_domain_opts local_opts;
	struct gpu_dmabuf_domain *gd;
	CUdevice cuda_device;
	CUresult cu_rc;
	int rc;

	if (domain == NULL) {
		return -EINVAL;
	}

	spdk_gpu_dmabuf_memory_domain_get_opts(&local_opts);
	if (opts != NULL) {
		if (opts->size == 0) {
			return -EINVAL;
		}
		if (GPU_DMABUF_OPTS_HAS(opts, id)) {
			local_opts.id = opts->id;
		}
		if (GPU_DMABUF_OPTS_HAS(opts, cuda_device_id)) {
			local_opts.cuda_device_id = opts->cuda_device_id;
		}
		if (GPU_DMABUF_OPTS_HAS(opts, cuda_context)) {
			local_opts.cuda_context = opts->cuda_context;
		}
		if (GPU_DMABUF_OPTS_HAS(opts, rdma_access_flags) && opts->rdma_access_flags != 0) {
			local_opts.rdma_access_flags = opts->rdma_access_flags;
		}
	}

	if (local_opts.cuda_device_id < -1) {
		return -EINVAL;
	}

	gd = calloc(1, sizeof(*gd));
	if (gd == NULL) {
		return -ENOMEM;
	}

	pthread_mutex_init(&gd->lock, NULL);
	RB_INIT(&gd->mr_cache);
	RB_INIT(&gd->regions);
	TAILQ_INIT(&gd->cuda_ctxs);
	gd->cuda_device_id = local_opts.cuda_device_id;
	gd->cuda_ctx = (CUcontext)local_opts.cuda_context;
	gd->rdma_access_flags = local_opts.rdma_access_flags;

	cu_rc = cuInit(0);
	if (cu_rc != CUDA_SUCCESS) {
		SPDK_ERRLOG("cuInit() failed: %d\n", cu_rc);
		rc = -ENODEV;
		goto err_free;
	}

	if (gd->cuda_ctx == NULL && gd->cuda_device_id >= 0) {
		cu_rc = cuDeviceGet(&cuda_device, gd->cuda_device_id);
		if (cu_rc != CUDA_SUCCESS) {
			SPDK_ERRLOG("cuDeviceGet(%d) failed: %d\n", gd->cuda_device_id, cu_rc);
			rc = -ENODEV;
			goto err_free;
		}

		cu_rc = cuDevicePrimaryCtxRetain(&gd->cuda_ctx, cuda_device);
		if (cu_rc != CUDA_SUCCESS) {
			SPDK_ERRLOG("cuDevicePrimaryCtxRetain(%d) failed: %d\n", gd->cuda_device_id, cu_rc);
			rc = -ENODEV;
			goto err_free;
		}
		gd->owns_cuda_ctx = true;
	}

	rc = spdk_memory_domain_create(&gd->domain, SPDK_DMA_DEVICE_VENDOR_SPECIFIC_TYPE_START,
				       NULL, local_opts.id ? local_opts.id : SPDK_GPU_DMABUF_DMA_DEVICE);
	if (rc != 0) {
		goto err_release_cuda_ctx;
	}

	spdk_memory_domain_set_translation(gd->domain, gpu_dmabuf_translate);

	pthread_mutex_lock(&g_gpu_dmabuf_domains_lock);
	TAILQ_INSERT_TAIL(&g_gpu_dmabuf_domains, gd, link);
	pthread_mutex_unlock(&g_gpu_dmabuf_domains_lock);

	*domain = gd->domain;

	return 0;

err_release_cuda_ctx:
	if (gd->owns_cuda_ctx) {
		cuDevicePrimaryCtxRelease(gd->cuda_device_id);
	}
err_free:
	pthread_mutex_destroy(&gd->lock);
	free(gd);
	return rc;
}

void
spdk_gpu_dmabuf_memory_domain_invalidate(struct spdk_memory_domain *domain, void *addr, size_t len)
{
	struct gpu_dmabuf_domain *gd;

	gd = gpu_dmabuf_domain_find(domain);
	if (gd == NULL) {
		return;
	}

	gpu_dmabuf_invalidate_range(gd, addr, len);
}

void
spdk_gpu_dmabuf_memory_domain_destroy(struct spdk_memory_domain *domain)
{
	struct gpu_dmabuf_domain *gd;
	struct gpu_dmabuf_mr *entry, *tmp;
	struct gpu_dmabuf_region *region, *region_tmp;

	gd = gpu_dmabuf_domain_find(domain);
	if (gd == NULL) {
		return;
	}

	pthread_mutex_lock(&g_gpu_dmabuf_domains_lock);
	TAILQ_REMOVE(&g_gpu_dmabuf_domains, gd, link);
	pthread_mutex_unlock(&g_gpu_dmabuf_domains_lock);

	pthread_mutex_lock(&gd->lock);
	RB_FOREACH_SAFE(entry, gpu_dmabuf_mr_tree, &gd->mr_cache, tmp) {
		RB_REMOVE(gpu_dmabuf_mr_tree, &gd->mr_cache, entry);
		gpu_dmabuf_dereg_mr(entry);
	}
	RB_FOREACH_SAFE(region, gpu_dmabuf_region_tree, &gd->regions, region_tmp) {
		RB_REMOVE(gpu_dmabuf_region_tree, &gd->regions, region);
		free(region);
	}
	pthread_mutex_unlock(&gd->lock);

	spdk_memory_domain_destroy(gd->domain);

	if (gd->owns_cuda_ctx) {
		cuDevicePrimaryCtxRelease(gd->cuda_device_id);
	}
	gpu_dmabuf_release_cuda_ctxs(gd);

	pthread_mutex_destroy(&gd->lock);
	free(gd);
}

SPDK_LOG_REGISTER_COMPONENT(gpu_dmabuf)
