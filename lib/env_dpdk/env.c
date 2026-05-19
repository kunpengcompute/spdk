/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation.
 *   Copyright (c) 2023, NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/util.h"
#include "spdk/env_dpdk.h"
#include "spdk/log.h"

#include "env_internal.h"
#include "spdk_internal/l0.h"
#include "spdk_internal/assert.h"

#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_version.h>

#define SPDK_L0_MEMPOOL_REGION_ALIGN (2ULL * 1024ULL * 1024ULL)

static uint64_t
virt_to_phys(void *vaddr)
{
	uint64_t ret;

	ret = rte_malloc_virt2iova(vaddr);
	if (ret != RTE_BAD_IOVA) {
		return ret;
	}

	return spdk_vtophys(vaddr, NULL);
}

static size_t
l0_mempool_align_up(size_t value, size_t align)
{
	assert(align != 0);
	return (value + align - 1) & ~(align - 1);
}

enum spdk_mempool_backend {
	SPDK_MEMPOOL_BACKEND_DPDK = 0,
	SPDK_MEMPOOL_BACKEND_L0,
};

struct spdk_mempool {
	enum spdk_mempool_backend	backend;
	char				*name;
	TAILQ_ENTRY(spdk_mempool)	link;
};

struct spdk_dpdk_mempool {
	struct spdk_mempool	base;
	struct rte_mempool	*mp;
};

struct spdk_l0_mempool {
	struct spdk_mempool	base;
	struct spdk_l0_region	*region;
	size_t			count;
	size_t			ele_size;
	size_t			free_count;
	void			**free_list;
	pthread_mutex_t		mutex;
};

static TAILQ_HEAD(, spdk_mempool) g_spdk_mempools = TAILQ_HEAD_INITIALIZER(g_spdk_mempools);
static pthread_mutex_t g_spdk_mempools_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct spdk_dpdk_mempool *
mempool_to_dpdk(struct spdk_mempool *mp)
{
	assert(mp->backend == SPDK_MEMPOOL_BACKEND_DPDK);
	return SPDK_CONTAINEROF(mp, struct spdk_dpdk_mempool, base);
}

static struct spdk_l0_mempool *
mempool_to_l0(struct spdk_mempool *mp)
{
	assert(mp->backend == SPDK_MEMPOOL_BACKEND_L0);
	return SPDK_CONTAINEROF(mp, struct spdk_l0_mempool, base);
}

static struct spdk_mempool *
mempool_find_locked(const char *name)
{
	struct spdk_mempool *mp;

	TAILQ_FOREACH(mp, &g_spdk_mempools, link) {
		if (strcmp(mp->name, name) == 0) {
			return mp;
		}
	}

	return NULL;
}

static int
mempool_register(struct spdk_mempool *mp, const char *name)
{
	pthread_mutex_lock(&g_spdk_mempools_mutex);
	if (mempool_find_locked(name) != NULL) {
		pthread_mutex_unlock(&g_spdk_mempools_mutex);
		return -EEXIST;
	}

	mp->name = strdup(name);
	if (mp->name == NULL) {
		pthread_mutex_unlock(&g_spdk_mempools_mutex);
		return -ENOMEM;
	}

	TAILQ_INSERT_TAIL(&g_spdk_mempools, mp, link);
	pthread_mutex_unlock(&g_spdk_mempools_mutex);
	return 0;
}

static void
mempool_unregister(struct spdk_mempool *mp)
{
	pthread_mutex_lock(&g_spdk_mempools_mutex);
	TAILQ_REMOVE(&g_spdk_mempools, mp, link);
	pthread_mutex_unlock(&g_spdk_mempools_mutex);
	free(mp->name);
}

void *
spdk_malloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
	void *buf;

	if (flags == 0) {
		return NULL;
	}

	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	buf = rte_malloc_socket(NULL, size, align, socket_id);
	if (buf && phys_addr) {
#ifdef DEBUG
		SPDK_ERRLOG("phys_addr param in spdk_malloc() is deprecated\n");
#endif
		*phys_addr = virt_to_phys(buf);
	}
	return buf;
}

void *
spdk_zmalloc(size_t size, size_t align, uint64_t *phys_addr, int socket_id, uint32_t flags)
{
	void *buf;

	if (flags == 0) {
		return NULL;
	}

	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	buf = rte_zmalloc_socket(NULL, size, align, socket_id);
	if (buf && phys_addr) {
#ifdef DEBUG
		SPDK_ERRLOG("phys_addr param in spdk_zmalloc() is deprecated\n");
#endif
		*phys_addr = virt_to_phys(buf);
	}
	return buf;
}

void *
spdk_realloc(void *buf, size_t size, size_t align)
{
	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	return rte_realloc(buf, size, align);
}

void
spdk_free(void *buf)
{
	rte_free(buf);
}

void *
spdk_dma_malloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	return spdk_malloc(size, align, phys_addr, socket_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
}

void *
spdk_dma_zmalloc_socket(size_t size, size_t align, uint64_t *phys_addr, int socket_id)
{
	return spdk_zmalloc(size, align, phys_addr, socket_id, (SPDK_MALLOC_DMA | SPDK_MALLOC_SHARE));
}

void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	return spdk_dma_malloc_socket(size, align, phys_addr, SPDK_ENV_SOCKET_ID_ANY);
}

void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	return spdk_dma_zmalloc_socket(size, align, phys_addr, SPDK_ENV_SOCKET_ID_ANY);
}

void *
spdk_dma_realloc(void *buf, size_t size, size_t align, uint64_t *phys_addr)
{
	void *new_buf;

	align = spdk_max(align, RTE_CACHE_LINE_SIZE);
	new_buf = rte_realloc(buf, size, align);
	if (new_buf && phys_addr) {
		*phys_addr = virt_to_phys(new_buf);
	}
	return new_buf;
}

void
spdk_dma_free(void *buf)
{
	spdk_free(buf);
}

void *
spdk_memzone_reserve_aligned(const char *name, size_t len, int socket_id,
			     unsigned flags, unsigned align)
{
	const struct rte_memzone *mz;
	unsigned dpdk_flags = 0;

	if ((flags & SPDK_MEMZONE_NO_IOVA_CONTIG) == 0) {
		dpdk_flags |= RTE_MEMZONE_IOVA_CONTIG;
	}

	if (socket_id == SPDK_ENV_SOCKET_ID_ANY) {
		socket_id = SOCKET_ID_ANY;
	}

	mz = rte_memzone_reserve_aligned(name, len, socket_id, dpdk_flags, align);

	if (mz != NULL) {
		memset(mz->addr, 0, len);
		return mz->addr;
	} else {
		return NULL;
	}
}

void *
spdk_memzone_reserve(const char *name, size_t len, int socket_id, unsigned flags)
{
	return spdk_memzone_reserve_aligned(name, len, socket_id, flags,
					    RTE_CACHE_LINE_SIZE);
}

void *
spdk_memzone_lookup(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return mz->addr;
	} else {
		return NULL;
	}
}

int
spdk_memzone_free(const char *name)
{
	const struct rte_memzone *mz = rte_memzone_lookup(name);

	if (mz != NULL) {
		return rte_memzone_free(mz);
	}

	return -1;
}

void
spdk_memzone_dump(FILE *f)
{
	rte_memzone_dump(f);
}

struct spdk_mempool *
spdk_mempool_create_ctor(const char *name, size_t count,
			 size_t ele_size, size_t cache_size, int socket_id,
			 spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg)
{
	struct spdk_dpdk_mempool *wrapper;
	struct rte_mempool *mp;
	size_t tmp;
	int rc;

	if (socket_id == SPDK_ENV_SOCKET_ID_ANY) {
		socket_id = SOCKET_ID_ANY;
	}

	/* No more than half of all elements can be in cache */
	tmp = (count / 2) / rte_lcore_count();
	if (cache_size > tmp) {
		cache_size = tmp;
	}

	if (cache_size > RTE_MEMPOOL_CACHE_MAX_SIZE) {
		cache_size = RTE_MEMPOOL_CACHE_MAX_SIZE;
	}

	mp = rte_mempool_create(name, count, ele_size, cache_size,
				0, NULL, NULL, NULL, NULL,
				socket_id, 0);
	if (mp == NULL) {
		return NULL;
	}

	wrapper = calloc(1, sizeof(*wrapper));
	if (wrapper == NULL) {
		rte_mempool_free(mp);
		return NULL;
	}

	wrapper->base.backend = SPDK_MEMPOOL_BACKEND_DPDK;
	wrapper->mp = mp;

	rc = mempool_register(&wrapper->base, name);
	if (rc != 0) {
		rte_mempool_free(mp);
		free(wrapper);
		errno = -rc;
		return NULL;
	}

	if (obj_init != NULL) {
		spdk_mempool_obj_iter(&wrapper->base, obj_init, obj_init_arg);
	}

	return &wrapper->base;
}


struct spdk_mempool *
spdk_mempool_create(const char *name, size_t count,
		    size_t ele_size, size_t cache_size, int socket_id)
{
	return spdk_mempool_create_ctor(name, count, ele_size, cache_size, socket_id,
					NULL, NULL);
}

struct spdk_mempool *
spdk_l0_mempool_create(const char *name, size_t count,
		       size_t ele_size, spdk_mempool_obj_cb_t *obj_init, void *obj_init_arg)
{
	struct spdk_l0_mempool *mp;
	struct spdk_l0_region *region;
	size_t total_len, aligned_total_len, i;
	int rc;

	if (count == 0 || ele_size == 0) {
		errno = EINVAL;
		return NULL;
	}

	if (count > SIZE_MAX / ele_size) {
		errno = EOVERFLOW;
		return NULL;
	}

	total_len = count * ele_size;
	if (total_len > SIZE_MAX - (SPDK_L0_MEMPOOL_REGION_ALIGN - 1)) {
		errno = EOVERFLOW;
		return NULL;
	}

	aligned_total_len = l0_mempool_align_up(total_len, SPDK_L0_MEMPOOL_REGION_ALIGN);
	region = spdk_l0_region_create(name, aligned_total_len);
	if (region == NULL) {
		return NULL;
	}

	mp = calloc(1, sizeof(*mp));
	if (mp == NULL) {
		spdk_l0_region_destroy(region);
		return NULL;
	}

	mp->free_list = calloc(count, sizeof(void *));
	if (mp->free_list == NULL) {
		free(mp);
		spdk_l0_region_destroy(region);
		return NULL;
	}

	mp->base.backend = SPDK_MEMPOOL_BACKEND_L0;
	mp->region = region;
	mp->count = count;
	mp->ele_size = ele_size;
	mp->free_count = count;
	pthread_mutex_init(&mp->mutex, NULL);

	rc = mempool_register(&mp->base, name);
	if (rc != 0) {
		pthread_mutex_destroy(&mp->mutex);
		free(mp->free_list);
		free(mp);
		spdk_l0_region_destroy(region);
		errno = -rc;
		return NULL;
	}

	for (i = 0; i < count; i++) {
		void *obj = (char *)spdk_l0_region_base(region) + (i * ele_size);

		mp->free_list[count - i - 1] = obj;
		if (obj_init != NULL) {
			obj_init(&mp->base, obj_init_arg, obj, i);
		}
	}

	return &mp->base;
}

char *
spdk_mempool_get_name(struct spdk_mempool *mp)
{
	return mp->name;
}

void
spdk_mempool_free(struct spdk_mempool *mp)
{
	if (mp == NULL) {
		return;
	}

	mempool_unregister(mp);

	if (mp->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		struct spdk_dpdk_mempool *dpdk_mp = mempool_to_dpdk(mp);

		rte_mempool_free(dpdk_mp->mp);
		free(dpdk_mp);
		return;
	}

	if (mp->backend == SPDK_MEMPOOL_BACKEND_L0) {
		struct spdk_l0_mempool *l0_mp = mempool_to_l0(mp);

		pthread_mutex_destroy(&l0_mp->mutex);
		free(l0_mp->free_list);
		spdk_l0_region_destroy(l0_mp->region);
		free(l0_mp);
		return;
	}

	SPDK_UNREACHABLE();
}

void *
spdk_mempool_get(struct spdk_mempool *mp)
{
	struct spdk_l0_mempool *l0_mp;
	void *ele = NULL;
	int rc;

	if (mp->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		rc = rte_mempool_get(mempool_to_dpdk(mp)->mp, &ele);
		if (rc != 0) {
			return NULL;
		}
		return ele;
	}

	l0_mp = mempool_to_l0(mp);
	pthread_mutex_lock(&l0_mp->mutex);
	if (l0_mp->free_count > 0) {
		ele = l0_mp->free_list[--l0_mp->free_count];
	}
	pthread_mutex_unlock(&l0_mp->mutex);

	return ele;
}

int
spdk_mempool_get_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	struct spdk_l0_mempool *l0_mp;
	size_t i;

	if (mp->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		return rte_mempool_get_bulk(mempool_to_dpdk(mp)->mp, ele_arr, count);
	}

	l0_mp = mempool_to_l0(mp);
	pthread_mutex_lock(&l0_mp->mutex);
	if (l0_mp->free_count < count) {
		pthread_mutex_unlock(&l0_mp->mutex);
		return -1;
	}

	for (i = 0; i < count; i++) {
		ele_arr[i] = l0_mp->free_list[--l0_mp->free_count];
	}
	pthread_mutex_unlock(&l0_mp->mutex);
	return 0;
}

void
spdk_mempool_put(struct spdk_mempool *mp, void *ele)
{
	struct spdk_l0_mempool *l0_mp;

	if (mp->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		rte_mempool_put(mempool_to_dpdk(mp)->mp, ele);
		return;
	}

	l0_mp = mempool_to_l0(mp);
	pthread_mutex_lock(&l0_mp->mutex);
	assert(l0_mp->free_count < l0_mp->count);
	l0_mp->free_list[l0_mp->free_count++] = ele;
	pthread_mutex_unlock(&l0_mp->mutex);
}

void
spdk_mempool_put_bulk(struct spdk_mempool *mp, void **ele_arr, size_t count)
{
	struct spdk_l0_mempool *l0_mp;
	size_t i;

	if (mp->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		rte_mempool_put_bulk(mempool_to_dpdk(mp)->mp, ele_arr, count);
		return;
	}

	l0_mp = mempool_to_l0(mp);
	pthread_mutex_lock(&l0_mp->mutex);
	assert(l0_mp->free_count + count <= l0_mp->count);
	for (i = 0; i < count; i++) {
		l0_mp->free_list[l0_mp->free_count++] = ele_arr[i];
	}
	pthread_mutex_unlock(&l0_mp->mutex);
}

size_t
spdk_mempool_count(const struct spdk_mempool *pool)
{
	struct spdk_l0_mempool *l0_mp;
	size_t count;

	if (pool->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		return rte_mempool_avail_count(((const struct spdk_dpdk_mempool *)pool)->mp);
	}

	l0_mp = SPDK_CONTAINEROF(pool, struct spdk_l0_mempool, base);
	pthread_mutex_lock(&l0_mp->mutex);
	count = l0_mp->free_count;
	pthread_mutex_unlock(&l0_mp->mutex);
	return count;
}

struct env_mempool_obj_iter_ctx {
	struct spdk_mempool *mp;
	spdk_mempool_obj_cb_t *user_cb;
	void *user_arg;
};

static void
mempool_obj_iter_remap(struct rte_mempool *rte_mp, void *opaque, void *obj,
		       unsigned obj_idx)
{
	struct env_mempool_obj_iter_ctx *ctx = opaque;

	(void)rte_mp;
	ctx->user_cb(ctx->mp, ctx->user_arg, obj, obj_idx);
}

uint32_t
spdk_mempool_obj_iter(struct spdk_mempool *mp, spdk_mempool_obj_cb_t obj_cb,
		      void *obj_cb_arg)
{
	struct spdk_l0_mempool *l0_mp;
	struct env_mempool_obj_iter_ctx ctx;
	uint32_t i;

	if (mp->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		ctx.mp = mp;
		ctx.user_cb = obj_cb;
		ctx.user_arg = obj_cb_arg;
		return rte_mempool_obj_iter(mempool_to_dpdk(mp)->mp, mempool_obj_iter_remap, &ctx);
	}

	l0_mp = mempool_to_l0(mp);
	for (i = 0; i < l0_mp->count; i++) {
		void *obj = (char *)spdk_l0_region_base(l0_mp->region) + ((size_t)i * l0_mp->ele_size);
		obj_cb(mp, obj_cb_arg, obj, i);
	}

	return (uint32_t)l0_mp->count;
}

struct env_mempool_mem_iter_ctx {
	struct spdk_mempool *mp;
	spdk_mempool_mem_cb_t *user_cb;
	void *user_arg;
};

static void
mempool_mem_iter_remap(struct rte_mempool *mp, void *opaque, struct rte_mempool_memhdr *memhdr,
		       unsigned mem_idx)
{
	struct env_mempool_mem_iter_ctx *ctx = opaque;

	(void)mp;
	ctx->user_cb(ctx->mp, ctx->user_arg, memhdr->addr, memhdr->iova, memhdr->len,
		     mem_idx);
}

uint32_t
spdk_mempool_mem_iter(struct spdk_mempool *mp, spdk_mempool_mem_cb_t mem_cb,
		      void *mem_cb_arg)
{
	struct env_mempool_mem_iter_ctx ctx = {
		.mp = mp,
		.user_cb = mem_cb,
		.user_arg = mem_cb_arg
	};

	if (mp->backend == SPDK_MEMPOOL_BACKEND_DPDK) {
		return rte_mempool_mem_iter(mempool_to_dpdk(mp)->mp, mempool_mem_iter_remap, &ctx);
	}

	mem_cb(mp, mem_cb_arg, spdk_l0_region_base(mempool_to_l0(mp)->region),
	       spdk_l0_region_phys(mempool_to_l0(mp)->region),
	       spdk_l0_region_len(mempool_to_l0(mp)->region), 0);
	return 1;
}

struct spdk_mempool *
spdk_mempool_lookup(const char *name)
{
	struct spdk_mempool *mp;

	pthread_mutex_lock(&g_spdk_mempools_mutex);
	mp = mempool_find_locked(name);
	pthread_mutex_unlock(&g_spdk_mempools_mutex);

	return mp;
}

bool
spdk_process_is_primary(void)
{
	return (rte_eal_process_type() == RTE_PROC_PRIMARY);
}

uint64_t
spdk_get_ticks(void)
{
	return rte_get_timer_cycles();
}

uint64_t
spdk_get_ticks_hz(void)
{
	return rte_get_timer_hz();
}

void
spdk_delay_us(unsigned int us)
{
	rte_delay_us(us);
}

void
spdk_pause(void)
{
	rte_pause();
}

void
spdk_unaffinitize_thread(void)
{
	rte_cpuset_t new_cpuset;
	long num_cores, i;

	CPU_ZERO(&new_cpuset);

	num_cores = sysconf(_SC_NPROCESSORS_CONF);

	/* Create a mask containing all CPUs */
	for (i = 0; i < num_cores; i++) {
		CPU_SET(i, &new_cpuset);
	}

	rte_thread_set_affinity(&new_cpuset);
}

void *
spdk_call_unaffinitized(void *cb(void *arg), void *arg)
{
	rte_cpuset_t orig_cpuset;
	void *ret;

	if (cb == NULL) {
		return NULL;
	}

	rte_thread_get_affinity(&orig_cpuset);

	spdk_unaffinitize_thread();

	ret = cb(arg);

	rte_thread_set_affinity(&orig_cpuset);

	return ret;
}

struct spdk_ring *
spdk_ring_create(enum spdk_ring_type type, size_t count, int socket_id)
{
	char ring_name[64];
	static uint32_t ring_num = 0;
	unsigned flags = RING_F_EXACT_SZ;

	switch (type) {
	case SPDK_RING_TYPE_SP_SC:
		flags |= RING_F_SP_ENQ | RING_F_SC_DEQ;
		break;
	case SPDK_RING_TYPE_MP_SC:
		flags |= RING_F_SC_DEQ;
		break;
	case SPDK_RING_TYPE_MP_MC:
		flags |= 0;
		break;
	default:
		return NULL;
	}

	snprintf(ring_name, sizeof(ring_name), "ring_%u_%d",
		 __atomic_fetch_add(&ring_num, 1, __ATOMIC_RELAXED), getpid());

	return (struct spdk_ring *)rte_ring_create(ring_name, count, socket_id, flags);
}

void
spdk_ring_free(struct spdk_ring *ring)
{
	rte_ring_free((struct rte_ring *)ring);
}

size_t
spdk_ring_count(struct spdk_ring *ring)
{
	return rte_ring_count((struct rte_ring *)ring);
}

size_t
spdk_ring_enqueue(struct spdk_ring *ring, void **objs, size_t count,
		  size_t *free_space)
{
	return rte_ring_enqueue_bulk((struct rte_ring *)ring, objs, count,
				     (unsigned int *)free_space);
}

size_t
spdk_ring_dequeue(struct spdk_ring *ring, void **objs, size_t count)
{
	return rte_ring_dequeue_burst((struct rte_ring *)ring, objs, count, NULL);
}

void
spdk_env_dpdk_dump_mem_stats(FILE *file)
{
	fprintf(file, "DPDK memory size %" PRIu64 "\n", rte_eal_get_physmem_size());
	fprintf(file, "DPDK memory layout\n");
	rte_dump_physmem_layout(file);
	fprintf(file, "DPDK memzones.\n");
	rte_memzone_dump(file);
	fprintf(file, "DPDK mempools.\n");
	rte_mempool_list_dump(file);
	fprintf(file, "DPDK malloc stats.\n");
	rte_malloc_dump_stats(file, NULL);
	fprintf(file, "DPDK malloc heaps.\n");
	rte_malloc_dump_heaps(file);
}
