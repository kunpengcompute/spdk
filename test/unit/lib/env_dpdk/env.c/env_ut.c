/* SPDX-License-Identifier: BSD-3-Clause */

#include "spdk_cunit.h"
#include "spdk/env.h"
#include "spdk_internal/l0.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/assert.h"
#include <setjmp.h>
#include "rte_test_stubs.h"

static size_t g_calloc_count, g_calloc_fail_at;
static bool g_strdup_fail;
static jmp_buf g_unreachable_jmp;

static void *
env_ut_calloc(size_t count, size_t size)
{
	if (++g_calloc_count == g_calloc_fail_at) {
		errno = ENOMEM;
		return NULL;
	}
	return calloc(count, size);
}

static char *
env_ut_strdup(const char *name)
{
	if (g_strdup_fail) {
		errno = ENOMEM;
		return NULL;
	}
	return strdup(name);
}

/* Observe the invalid-backend trap without terminating the entire CUnit process. */
#undef SPDK_UNREACHABLE
#define SPDK_UNREACHABLE() longjmp(g_unreachable_jmp, 1)
#define calloc env_ut_calloc
#define strdup env_ut_strdup
#include "env_dpdk/env.c"
#undef calloc
#undef strdup

struct spdk_l0_region {
	unsigned char data[4096];
};

static struct spdk_l0_region g_region;
static struct rte_mempool g_dpdk_pool;
static unsigned char g_dpdk_objects[3][64];
static bool g_region_fail, g_dpdk_create_fail;
static size_t g_region_create_count, g_region_destroy_count, g_dpdk_free_count;
static size_t g_last_region_len;
static int g_dpdk_get_result, g_dpdk_bulk_result;
static unsigned int g_last_cache_size;
static int g_last_socket_id;
static size_t g_put_count, g_callback_count;
static bool g_callback_l0;
static struct spdk_mempool *g_callback_mp;

DEFINE_STUB(rte_lcore_count, unsigned int, (void), 2);

struct spdk_l0_region *
spdk_l0_region_create(const char *name, size_t len)
{
	g_last_region_len = len;
	g_region_create_count++;
	return g_region_fail ? NULL : &g_region;
}

void
spdk_l0_region_destroy(struct spdk_l0_region *region)
{
	CU_ASSERT_PTR_EQUAL(region, &g_region);
	g_region_destroy_count++;
}

void *
spdk_l0_region_base(struct spdk_l0_region *region)
{
	CU_ASSERT_PTR_EQUAL(region, &g_region);
	return region->data;
}

uint64_t
spdk_l0_region_phys(struct spdk_l0_region *region)
{
	CU_ASSERT_PTR_EQUAL(region, &g_region);
	return 0x40000000ULL;
}

size_t
spdk_l0_region_len(struct spdk_l0_region *region)
{
	CU_ASSERT_PTR_EQUAL(region, &g_region);
	return g_last_region_len;
}

struct rte_mempool *
rte_mempool_create(const char *name, unsigned int count, unsigned int size,
		   unsigned int cache_size, unsigned int private_size,
		   rte_mempool_ctor_t *ctor, void *ctor_arg, rte_mempool_obj_cb_t *obj_init,
		   void *obj_arg, int socket_id, unsigned int flags)
{
	/* The user's constructor must be called later with the SPDK wrapper identity. */
	CU_ASSERT_PTR_NULL(ctor);
	CU_ASSERT_PTR_NULL(ctor_arg);
	CU_ASSERT_PTR_NULL(obj_init);
	CU_ASSERT_PTR_NULL(obj_arg);
	g_last_cache_size = cache_size;
	g_last_socket_id = socket_id;
	return g_dpdk_create_fail ? NULL : &g_dpdk_pool;
}

void
rte_mempool_free(struct rte_mempool *pool)
{
	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	g_dpdk_free_count++;
}

int
rte_mempool_get(struct rte_mempool *pool, void **object)
{
	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	if (g_dpdk_get_result == 0) {
		*object = g_dpdk_objects[0];
	}
	return g_dpdk_get_result;
}

int
rte_mempool_get_bulk(struct rte_mempool *pool, void **objects, unsigned int count)
{
	unsigned int i;

	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	if (g_dpdk_bulk_result == 0) {
		for (i = 0; i < count; i++) {
			objects[i] = g_dpdk_objects[i];
		}
	}
	return g_dpdk_bulk_result;
}

void
rte_mempool_put(struct rte_mempool *pool, void *object)
{
	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	CU_ASSERT_PTR_EQUAL(object, g_dpdk_objects[0]);
	g_put_count++;
}

void
rte_mempool_put_bulk(struct rte_mempool *pool, void *const *objects, unsigned int count)
{
	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	CU_ASSERT_PTR_EQUAL(objects[0], g_dpdk_objects[0]);
	g_put_count += count;
}

unsigned int
rte_mempool_avail_count(const struct rte_mempool *pool)
{
	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	return 3;
}

unsigned int
rte_mempool_obj_iter(struct rte_mempool *pool, rte_mempool_obj_cb_t *callback, void *arg)
{
	unsigned int i;

	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	for (i = 0; i < 3; i++) {
		callback(pool, arg, g_dpdk_objects[i], i);
	}
	return 3;
}

unsigned int
rte_mempool_mem_iter(struct rte_mempool *pool, rte_mempool_mem_cb_t *callback, void *arg)
{
	struct rte_mempool_memhdr hdr = {
		.addr = g_dpdk_objects, .iova = 0x50000000ULL, .len = sizeof(g_dpdk_objects)
	};

	CU_ASSERT_PTR_EQUAL(pool, &g_dpdk_pool);
	callback(pool, arg, &hdr, 0);
	return 1;
}

static void
reset_mocks(void)
{
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_mempools));
	g_calloc_count = g_calloc_fail_at = 0;
	g_strdup_fail = g_region_fail = g_dpdk_create_fail = false;
	g_region_create_count = g_region_destroy_count = g_dpdk_free_count = 0;
	g_callback_count = g_put_count = 0;
	g_callback_mp = NULL;
	g_callback_l0 = false;
	g_dpdk_get_result = g_dpdk_bulk_result = 0;
}

static void
object_callback(struct spdk_mempool *pool, void *arg, void *object, unsigned int index)
{
	CU_ASSERT_PTR_EQUAL(arg, &g_callback_count);
	CU_ASSERT(index == g_callback_count);
	if (g_callback_mp != NULL) {
		CU_ASSERT_PTR_EQUAL(pool, g_callback_mp);
	}
	g_callback_mp = pool;
	if (g_callback_l0) {
		CU_ASSERT_PTR_EQUAL(object, g_region.data + index * 64);
	} else {
		CU_ASSERT_PTR_EQUAL(object, g_dpdk_objects[index]);
	}
	g_callback_count++;
}

static void
memory_callback(struct spdk_mempool *pool, void *arg, void *addr, uint64_t iova,
		size_t len, unsigned int index)
{
	CU_ASSERT_PTR_EQUAL(pool, g_callback_mp);
	CU_ASSERT_PTR_EQUAL(arg, &g_callback_count);
	CU_ASSERT(index == 0);
	if (g_callback_l0) {
		CU_ASSERT_PTR_EQUAL(addr, g_region.data);
		CU_ASSERT(iova == 0x40000000ULL);
		CU_ASSERT(len == SPDK_L0_MEMPOOL_REGION_ALIGN);
	} else {
		CU_ASSERT_PTR_EQUAL(addr, g_dpdk_objects);
		CU_ASSERT(iova == 0x50000000ULL);
		CU_ASSERT(len == sizeof(g_dpdk_objects));
	}
	g_callback_count++;
}

static void
test_l0_create_failures(void)
{
	struct spdk_mempool *pool;

	reset_mocks();
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 0, 64, NULL, NULL));
	CU_ASSERT(errno == EINVAL);
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 1, 0, NULL, NULL));
	CU_ASSERT(errno == EINVAL);
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", SIZE_MAX, 2, NULL, NULL));
	CU_ASSERT(errno == EOVERFLOW);
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 1, SIZE_MAX, NULL, NULL));
	CU_ASSERT(errno == EOVERFLOW);
	CU_ASSERT(g_region_create_count == 0);
	g_region_fail = true;
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 3, 64, NULL, NULL));
	CU_ASSERT(g_last_region_len == SPDK_L0_MEMPOOL_REGION_ALIGN);
	CU_ASSERT(g_region_destroy_count == 0);

	reset_mocks();
	g_calloc_fail_at = 1;
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 3, 64, NULL, NULL));
	CU_ASSERT(g_region_destroy_count == 1);
	reset_mocks();
	g_calloc_fail_at = 2;
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 3, 64, NULL, NULL));
	CU_ASSERT(g_region_destroy_count == 1);
	reset_mocks();
	g_strdup_fail = true;
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 3, 64, NULL, NULL));
	CU_ASSERT(errno == ENOMEM);
	CU_ASSERT(g_region_destroy_count == 1);

	reset_mocks();
	pool = spdk_l0_mempool_create("l0", 3, 64, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(pool != NULL);
	CU_ASSERT_PTR_NULL(spdk_l0_mempool_create("l0", 3, 64, NULL, NULL));
	CU_ASSERT(errno == EEXIST);
	CU_ASSERT(g_region_destroy_count == 1);
	CU_ASSERT_PTR_EQUAL(spdk_mempool_lookup("l0"), pool);
	spdk_mempool_free(pool);
	CU_ASSERT(g_region_destroy_count == 2);
}

static void
test_l0_mempool_operations(void)
{
	struct spdk_mempool *pool;
	void *first, *objects[3] = {};

	reset_mocks();
	g_callback_l0 = true;
	CU_ASSERT(l0_mempool_align_up(SPDK_L0_MEMPOOL_REGION_ALIGN,
					SPDK_L0_MEMPOOL_REGION_ALIGN) == SPDK_L0_MEMPOOL_REGION_ALIGN);
	pool = spdk_l0_mempool_create("l0", 3, 64, object_callback, &g_callback_count);
	SPDK_CU_ASSERT_FATAL(pool != NULL);
	CU_ASSERT(g_callback_count == 3);
	CU_ASSERT_PTR_EQUAL(g_callback_mp, pool);
	CU_ASSERT_STRING_EQUAL(spdk_mempool_get_name(pool), "l0");
	CU_ASSERT_PTR_EQUAL(spdk_mempool_lookup("l0"), pool);
	CU_ASSERT_PTR_NULL(spdk_mempool_lookup("missing"));
	CU_ASSERT(spdk_mempool_count(pool) == 3);
	first = spdk_mempool_get(pool);
	CU_ASSERT_PTR_EQUAL(first, g_region.data);
	CU_ASSERT(spdk_mempool_count(pool) == 2);
	CU_ASSERT(spdk_mempool_get_bulk(pool, objects, 3) == -1);
	CU_ASSERT_PTR_NULL(objects[0]);
	CU_ASSERT(spdk_mempool_count(pool) == 2);
	CU_ASSERT(spdk_mempool_get_bulk(pool, objects, 2) == 0);
	CU_ASSERT_PTR_EQUAL(objects[0], g_region.data + 64);
	CU_ASSERT_PTR_EQUAL(objects[1], g_region.data + 128);
	CU_ASSERT_PTR_NULL(spdk_mempool_get(pool));
	CU_ASSERT(spdk_mempool_count(pool) == 0);
	spdk_mempool_put(pool, first);
	spdk_mempool_put_bulk(pool, objects, 2);
	CU_ASSERT(spdk_mempool_count(pool) == 3);
	CU_ASSERT(spdk_mempool_get_bulk(pool, objects, 0) == 0);
	spdk_mempool_put_bulk(pool, objects, 0);
	g_callback_count = 0;
	CU_ASSERT(spdk_mempool_obj_iter(pool, object_callback, &g_callback_count) == 3);
	CU_ASSERT(g_callback_count == 3);
	g_callback_count = 0;
	CU_ASSERT(spdk_mempool_mem_iter(pool, memory_callback, &g_callback_count) == 1);
	CU_ASSERT(g_callback_count == 1);
	spdk_mempool_free(pool);
	CU_ASSERT(g_region_destroy_count == 1);
	CU_ASSERT_PTR_NULL(spdk_mempool_lookup("l0"));
	spdk_mempool_free(NULL);
}

static void
test_dpdk_create_failures(void)
{
	struct spdk_mempool *pool;

	reset_mocks();
	g_dpdk_create_fail = true;
	CU_ASSERT_PTR_NULL(spdk_mempool_create("dpdk", 3, 64, 0, SPDK_ENV_SOCKET_ID_ANY));
	CU_ASSERT(g_dpdk_free_count == 0);
	reset_mocks();
	g_calloc_fail_at = 1;
	CU_ASSERT_PTR_NULL(spdk_mempool_create("dpdk", 3, 64, 0, 1));
	CU_ASSERT(g_dpdk_free_count == 1);
	reset_mocks();
	g_strdup_fail = true;
	CU_ASSERT_PTR_NULL(spdk_mempool_create("dpdk", 3, 64, 0, 1));
	CU_ASSERT(errno == ENOMEM);
	CU_ASSERT(g_dpdk_free_count == 1);
	reset_mocks();
	pool = spdk_mempool_create("dpdk", 3, 64, 0, 1);
	SPDK_CU_ASSERT_FATAL(pool != NULL);
	CU_ASSERT_PTR_NULL(spdk_mempool_create("dpdk", 3, 64, 0, 1));
	CU_ASSERT(errno == EEXIST);
	CU_ASSERT(g_dpdk_free_count == 1);
	spdk_mempool_free(pool);
	CU_ASSERT(g_dpdk_free_count == 2);
}

static void
test_dpdk_mempool_operations(void)
{
	struct spdk_mempool *pool;
	void *objects[3] = {};

	reset_mocks();
	pool = spdk_mempool_create_ctor("dpdk", 4096, 64, 2048, SPDK_ENV_SOCKET_ID_ANY,
					object_callback, &g_callback_count);
	SPDK_CU_ASSERT_FATAL(pool != NULL);
	CU_ASSERT(g_last_cache_size == RTE_MEMPOOL_CACHE_MAX_SIZE);
	CU_ASSERT(g_last_socket_id == SOCKET_ID_ANY);
	CU_ASSERT(g_callback_count == 3);
	CU_ASSERT_PTR_EQUAL(g_callback_mp, pool);
	CU_ASSERT_STRING_EQUAL(spdk_mempool_get_name(pool), "dpdk");
	CU_ASSERT_PTR_EQUAL(spdk_mempool_get(pool), g_dpdk_objects[0]);
	g_dpdk_get_result = -ENOENT;
	CU_ASSERT_PTR_NULL(spdk_mempool_get(pool));
	CU_ASSERT(spdk_mempool_get_bulk(pool, objects, 3) == 0);
	CU_ASSERT_PTR_EQUAL(objects[2], g_dpdk_objects[2]);
	g_dpdk_bulk_result = -ENOENT;
	CU_ASSERT(spdk_mempool_get_bulk(pool, objects, 3) == -ENOENT);
	spdk_mempool_put(pool, objects[0]);
	spdk_mempool_put_bulk(pool, objects, 3);
	CU_ASSERT(g_put_count == 4);
	CU_ASSERT(spdk_mempool_count(pool) == 3);
	g_callback_count = 0;
	CU_ASSERT(spdk_mempool_obj_iter(pool, object_callback, &g_callback_count) == 3);
	CU_ASSERT(g_callback_count == 3);
	g_callback_count = 0;
	CU_ASSERT(spdk_mempool_mem_iter(pool, memory_callback, &g_callback_count) == 1);
	CU_ASSERT(g_callback_count == 1);
	spdk_mempool_free(pool);
	CU_ASSERT(g_dpdk_free_count == 1);
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_mempools));
}

static void
test_invalid_backend_traps(void)
{
	struct spdk_mempool pool = {.backend = 99};

	reset_mocks();
	SPDK_CU_ASSERT_FATAL(mempool_register(&pool, "invalid") == 0);
	if (setjmp(g_unreachable_jmp) == 0) {
		spdk_mempool_free(&pool);
		CU_FAIL("Invalid backend must reach SPDK_UNREACHABLE");
	}
	CU_ASSERT(TAILQ_EMPTY(&g_spdk_mempools));
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}
	suite = CU_add_suite("env_mempool", NULL, NULL);
	CU_ADD_TEST(suite, test_l0_create_failures);
	CU_ADD_TEST(suite, test_l0_mempool_operations);
	CU_ADD_TEST(suite, test_dpdk_create_failures);
	CU_ADD_TEST(suite, test_dpdk_mempool_operations);
	CU_ADD_TEST(suite, test_invalid_backend_traps);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return failures;
}
