/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef SPDK_UT_RTE_TEST_STUBS_H
#define SPDK_UT_RTE_TEST_STUBS_H

/* Only the DPDK boundary is replaced. env.c itself is included without alteration. */
#include "spdk/stdinc.h"

#define RTE_VERSION_NUM(a, b, c, d) (((a) << 24) | ((b) << 16) | ((c) << 8) | (d))
#define RTE_VERSION RTE_VERSION_NUM(22, 11, 0, 0)
#define RTE_CACHE_LINE_SIZE 64
#define RTE_BAD_IOVA UINT64_MAX
#define RTE_MEMZONE_IOVA_CONTIG 1
#define RTE_MEMPOOL_CACHE_MAX_SIZE 512
#define SOCKET_ID_ANY (-1)
#define RTE_PROC_PRIMARY 0
#define RING_F_EXACT_SZ 1
#define RING_F_SP_ENQ 2
#define RING_F_SC_DEQ 4

struct rte_mempool { int unused; };
struct rte_ring;
struct rte_memzone { void *addr; };
struct rte_mempool_memhdr { void *addr; uint64_t iova; size_t len; };
typedef void (rte_mempool_obj_cb_t)(struct rte_mempool *, void *, void *, unsigned int);
typedef void (rte_mempool_ctor_t)(struct rte_mempool *, void *);
typedef void (rte_mempool_mem_cb_t)(struct rte_mempool *, void *,
				 struct rte_mempool_memhdr *, unsigned int);
typedef struct { uint64_t bits; } rte_cpuset_t;
#undef CPU_ZERO
#undef CPU_SET
#define CPU_ZERO(cpuset) ((cpuset)->bits = 0)
#define CPU_SET(cpu, cpuset) ((cpuset)->bits |= (1ULL << ((cpu) % 64)))

uint64_t rte_malloc_virt2iova(const void *ptr);
void *rte_malloc_socket(const char *, size_t, unsigned int, int);
void *rte_zmalloc_socket(const char *, size_t, unsigned int, int);
void *rte_realloc(void *, size_t, unsigned int);
void rte_free(void *);
const struct rte_memzone *rte_memzone_reserve_aligned(const char *, size_t, int,
		unsigned int, unsigned int);
const struct rte_memzone *rte_memzone_lookup(const char *);
int rte_memzone_free(const struct rte_memzone *);
void rte_memzone_dump(FILE *);
unsigned int rte_lcore_count(void);
struct rte_mempool *rte_mempool_create(const char *, unsigned int, unsigned int,
		unsigned int, unsigned int, rte_mempool_ctor_t *, void *,
		rte_mempool_obj_cb_t *, void *, int, unsigned int);
void rte_mempool_free(struct rte_mempool *);
int rte_mempool_get(struct rte_mempool *, void **);
int rte_mempool_get_bulk(struct rte_mempool *, void **, unsigned int);
void rte_mempool_put(struct rte_mempool *, void *);
void rte_mempool_put_bulk(struct rte_mempool *, void *const *, unsigned int);
unsigned int rte_mempool_avail_count(const struct rte_mempool *);
unsigned int rte_mempool_obj_iter(struct rte_mempool *, rte_mempool_obj_cb_t *, void *);
unsigned int rte_mempool_mem_iter(struct rte_mempool *, rte_mempool_mem_cb_t *, void *);
int rte_eal_process_type(void);
uint64_t rte_get_timer_cycles(void);
uint64_t rte_get_timer_hz(void);
void rte_delay_us(unsigned int);
void rte_pause(void);
int rte_thread_set_affinity(rte_cpuset_t *);
void rte_thread_get_affinity(rte_cpuset_t *);
struct rte_ring *rte_ring_create(const char *, unsigned int, int, unsigned int);
void rte_ring_free(struct rte_ring *);
unsigned int rte_ring_count(const struct rte_ring *);
unsigned int rte_ring_enqueue_bulk(struct rte_ring *, void *const *, unsigned int,
		unsigned int *);
unsigned int rte_ring_dequeue_burst(struct rte_ring *, void **, unsigned int, unsigned int *);
uint64_t rte_eal_get_physmem_size(void);
void rte_dump_physmem_layout(FILE *);
void rte_mempool_list_dump(FILE *);
void rte_malloc_dump_stats(FILE *, const char *);
void rte_malloc_dump_heaps(FILE *);

/* Memory-map boundary used by memory.c tests. */
enum rte_mem_event { RTE_MEM_EVENT_ALLOC, RTE_MEM_EVENT_FREE };
enum rte_iova_mode { RTE_IOVA_PA, RTE_IOVA_VA };
struct rte_memseg_list;
struct rte_memseg {
	void *addr;
	uint64_t iova;
	size_t hugepage_sz;
	unsigned int flags;
};
struct rte_mem_resource { uint64_t phys_addr; uint64_t len; void *addr; };
typedef struct { int64_t cnt; } rte_atomic64_t;
#define RTE_MEMSEG_FLAG_DO_NOT_FREE 1
#define PCI_MAX_RESOURCE 6
struct rte_memseg *rte_mem_virt2memseg(const void *, const struct rte_memseg_list *);
uint64_t rte_mem_virt2iova(const void *);
int64_t rte_atomic64_read(const rte_atomic64_t *);
int rte_memseg_get_fd_thread_unsafe(const struct rte_memseg *);
int rte_memseg_get_fd_offset_thread_unsafe(const struct rte_memseg *, uint64_t *);
int rte_mem_event_callback_register(const char *,
		void (*)(enum rte_mem_event, const void *, size_t, void *), void *);
int rte_memseg_contig_walk(int (*)(const struct rte_memseg_list *,
		const struct rte_memseg *, size_t, void *), void *);
enum rte_iova_mode rte_eal_iova_mode(void);
int rte_vfio_is_enabled(const char *);
int rte_vfio_noiommu_is_enabled(void);

#endif
