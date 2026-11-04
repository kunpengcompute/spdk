/* SPDX-License-Identifier: BSD-3-Clause */

#include "spdk_cunit.h"
#include "spdk_internal/mock.h"
#include "env_dpdk/memory.c"

static uint64_t g_l0_phys, g_l0_size, g_l0_offset;
static unsigned int g_l0_translate_calls, g_l0_find_calls;
static bool g_l0_found;
static char g_region_identity;
static void *g_expected_addr;
static struct rte_memseg g_segment;

DEFINE_STUB(rte_mem_virt2memseg, struct rte_memseg *,
	    (const void *addr, const struct rte_memseg_list *list), NULL);
DEFINE_STUB(rte_memseg_get_fd_thread_unsafe, int, (const struct rte_memseg *seg), 42);

int
spdk_l0_region_fd(struct spdk_l0_region *region)
{
	CU_ASSERT_PTR_EQUAL(region, &g_region_identity);
	return 43;
}

int
rte_memseg_get_fd_offset_thread_unsafe(const struct rte_memseg *seg, uint64_t *offset)
{
	CU_ASSERT_PTR_EQUAL(seg, &g_segment);
	*offset = 8192;
	return 0;
}

uint64_t
spdk_l0_vtophys(const void *addr, uint64_t *size)
{
	CU_ASSERT_PTR_EQUAL(addr, g_expected_addr);
	g_l0_translate_calls++;
	*size = g_l0_size;
	return g_l0_phys;
}

bool
spdk_l0_find_region(const void *addr, struct spdk_l0_region **region, uint64_t *offset)
{
	CU_ASSERT_PTR_EQUAL(addr, g_expected_addr);
	g_l0_find_calls++;
	*region = g_l0_found ? (void *)&g_region_identity : NULL;
	*offset = g_l0_offset;
	return g_l0_found;
}

static void
test_vtophys_l0_fallback(void)
{
	uint64_t size;

	g_expected_addr = (void *)0x2001234ULL;
	g_vtophys_map = spdk_mem_map_alloc(SPDK_VTOPHYS_ERROR, NULL, NULL);
	SPDK_CU_ASSERT_FATAL(g_vtophys_map != NULL);
	g_l0_translate_calls = 0;
	g_l0_phys = 0x80001234ULL;
	g_l0_size = 12345;
	size = UINT64_MAX;
	CU_ASSERT(spdk_vtophys(g_expected_addr, &size) == g_l0_phys);
	CU_ASSERT(size == g_l0_size);
	CU_ASSERT(g_l0_translate_calls == 1);
	CU_ASSERT(spdk_vtophys(g_expected_addr, NULL) == g_l0_phys);
	CU_ASSERT(g_l0_translate_calls == 2);
	g_l0_phys = SPDK_VTOPHYS_ERROR;
	CU_ASSERT(spdk_vtophys(g_expected_addr, &size) == SPDK_VTOPHYS_ERROR);
	CU_ASSERT(g_l0_translate_calls == 3);

	/* A normal DPDK-backed translation must bypass the L0 fallback. */
	CU_ASSERT(spdk_mem_map_set_translation(g_vtophys_map, 0x2000000ULL,
					       VALUE_2MB, 0x4000000ULL) == 0);
	CU_ASSERT(spdk_vtophys(g_expected_addr, &size) == 0x4001234ULL);
	CU_ASSERT(g_l0_translate_calls == 3);
	spdk_mem_map_free(&g_vtophys_map);
	CU_ASSERT_PTR_NULL(g_vtophys_map);
}

static void
test_fd_and_offset_l0_fallback(void)
{
	uint64_t offset = UINT64_MAX;

	g_expected_addr = (void *)0x2001234ULL;
	g_l0_find_calls = 0;
	g_l0_found = false;
	MOCK_SET(rte_mem_virt2memseg, NULL);
	CU_ASSERT(spdk_mem_get_fd_and_offset(g_expected_addr, &offset) == -ENOENT);
	CU_ASSERT(offset == UINT64_MAX);
	CU_ASSERT(g_l0_find_calls == 1);
	g_l0_found = true;
	g_l0_offset = 0x1234;
	CU_ASSERT(spdk_mem_get_fd_and_offset(g_expected_addr, &offset) == 43);
	CU_ASSERT(offset == 0x1234);
	CU_ASSERT(g_l0_find_calls == 2);

	/* Preserve the original DPDK fd/offset path and its failure semantics. */
	MOCK_SET(rte_mem_virt2memseg, &g_segment);
	CU_ASSERT(spdk_mem_get_fd_and_offset(g_expected_addr, &offset) == 42);
	CU_ASSERT(offset == 8192);
	CU_ASSERT(g_l0_find_calls == 2);
	MOCK_SET(rte_memseg_get_fd_thread_unsafe, -EBADF);
	CU_ASSERT(spdk_mem_get_fd_and_offset(g_expected_addr, &offset) == -EBADF);
	MOCK_SET(rte_memseg_get_fd_thread_unsafe, 42);
	MOCK_SET(rte_mem_virt2memseg, NULL);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}
	suite = CU_add_suite("env_memory_l0", NULL, NULL);
	CU_ADD_TEST(suite, test_vtophys_l0_fallback);
	CU_ADD_TEST(suite, test_fd_and_offset_l0_fallback);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return failures;
}
