/* SPDX-License-Identifier: BSD-3-Clause */

#include "spdk_cunit.h"
#include "spdk/env.h"
#include "spdk/util.h"
#include "spdk_internal/mock.h"

static unsigned char g_buffer[4096];
static int g_open_result, g_close_count, g_unmap_count;
static int g_register_result, g_unregister_result;
static int g_register_count, g_unregister_count;
static const char *g_expected_device;

DEFINE_RETURN_MOCK(l0_calloc, void *);
DEFINE_RETURN_MOCK(l0_strdup, char *);

static void *
l0_ut_calloc(size_t count, size_t size)
{
	HANDLE_RETURN_MOCK(l0_calloc);
	return calloc(count, size);
}

static char *
l0_ut_strdup(const char *name)
{
	HANDLE_RETURN_MOCK(l0_strdup);
	return strdup(name);
}

static int
l0_ut_open(const char *path, int flags, ...)
{
	CU_ASSERT_STRING_EQUAL(path, g_expected_device);
	CU_ASSERT(flags == (O_RDWR | O_CLOEXEC));
	if (g_open_result < 0) {
		errno = ENOENT;
	}
	return g_open_result;
}

static int
l0_ut_close(int fd)
{
	CU_ASSERT(fd == 42);
	g_close_count++;
	return 0;
}

static int
l0_ut_munmap(void *addr, size_t len)
{
	CU_ASSERT_PTR_EQUAL(addr, g_buffer);
	CU_ASSERT(len == sizeof(g_buffer));
	g_unmap_count++;
	return 0;
}

/* Replace only calls in the included implementation, leaving the test allocator intact. */
#define calloc l0_ut_calloc
#define strdup l0_ut_strdup
#define open l0_ut_open
#define close l0_ut_close
#define munmap l0_ut_munmap
#include "env_dpdk/l0.c"
#undef calloc
#undef strdup
#undef open
#undef close
#undef munmap

DEFINE_STUB(get_l0_fd, int, (void), 42);
DEFINE_RETURN_MOCK(mmap_alloc, void *);
DEFINE_RETURN_MOCK(vtop, unsigned long long);

void *
mmap_alloc(unsigned long size, int fd)
{
	CU_ASSERT(size == sizeof(g_buffer));
	CU_ASSERT(fd == 42);
	return MOCK_GET(mmap_alloc);
}

unsigned long long
vtop(unsigned long long addr)
{
	CU_ASSERT(addr == (uintptr_t)g_buffer);
	return MOCK_GET(vtop);
}

int
spdk_mem_register(void *addr, size_t len)
{
	CU_ASSERT_PTR_EQUAL(addr, g_buffer);
	CU_ASSERT(len == sizeof(g_buffer));
	g_register_count++;
	return g_register_result;
}

int
spdk_mem_unregister(void *addr, size_t len)
{
	CU_ASSERT_PTR_EQUAL(addr, g_buffer);
	CU_ASSERT(len == sizeof(g_buffer));
	g_unregister_count++;
	return g_unregister_result;
}

static void
reset_mocks(void)
{
	CU_ASSERT(TAILQ_EMPTY(&g_l0_regions));
	MOCK_CLEAR(l0_calloc);
	MOCK_CLEAR(l0_strdup);
	MOCK_SET(get_l0_fd, 42);
	MOCK_SET(mmap_alloc, g_buffer);
	MOCK_SET(vtop, 0x10000000ULL);
	g_open_result = 42;
	g_close_count = g_unmap_count = 0;
	g_register_count = g_unregister_count = 0;
	g_register_result = g_unregister_result = 0;
	g_expected_device = "/dev/test_l0";
	unsetenv("SPDK_NVMF_L0_DEVICE");
	memset(g_buffer, 0xa5, sizeof(g_buffer));
}

static void
test_enable_flag(void)
{
	const char *enabled[] = {"1", "y", "YES", "TrUe", "ON"};
	const char *disabled[] = {"", "0", "no", "false", "off", "yes ", "2"};
	char *saved = getenv("SPDK_NVMF_L0_ENABLE");
	size_t i;

	saved = saved == NULL ? NULL : strdup(saved);
	unsetenv("SPDK_NVMF_L0_ENABLE");
	CU_ASSERT_FALSE(spdk_l0_data_pool_enabled());
	for (i = 0; i < SPDK_COUNTOF(enabled); i++) {
		setenv("SPDK_NVMF_L0_ENABLE", enabled[i], 1);
		CU_ASSERT_TRUE(spdk_l0_data_pool_enabled());
	}
	for (i = 0; i < SPDK_COUNTOF(disabled); i++) {
		setenv("SPDK_NVMF_L0_ENABLE", disabled[i], 1);
		CU_ASSERT_FALSE(spdk_l0_data_pool_enabled());
	}
	if (saved != NULL) {
		setenv("SPDK_NVMF_L0_ENABLE", saved, 1);
		free(saved);
	} else {
		unsetenv("SPDK_NVMF_L0_ENABLE");
	}
}

static void
test_invalid_size_and_device(void)
{
	reset_mocks();
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", 0));
	CU_ASSERT(errno == EINVAL);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", SPDK_L0_MAX_BYTES + 1));
	CU_ASSERT(errno == ENOMEM);
	MOCK_SET(get_l0_fd, -1);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	CU_ASSERT(errno == EINVAL);
	setenv("SPDK_NVMF_L0_DEVICE", "", 1);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	CU_ASSERT(errno == EINVAL);
	setenv("SPDK_NVMF_L0_DEVICE", g_expected_device, 1);
	g_open_result = -1;
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	CU_ASSERT(errno == ENOENT);
	CU_ASSERT(g_close_count == 0);
	CU_ASSERT(g_unmap_count == 0);
	CU_ASSERT(g_register_count == 0);
}

static void
test_create_failures(void)
{
	reset_mocks();
	MOCK_SET(mmap_alloc, NULL);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	CU_ASSERT(g_close_count == 1);
	CU_ASSERT(g_unmap_count == 0);
	MOCK_SET(mmap_alloc, MAP_FAILED);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	CU_ASSERT(g_close_count == 2);
	CU_ASSERT(g_unmap_count == 0);

	reset_mocks();
	MOCK_SET(vtop, SPDK_VTOPHYS_ERROR);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	CU_ASSERT(errno == EFAULT);
	CU_ASSERT(g_unmap_count == 1);
	CU_ASSERT(g_close_count == 1);
	CU_ASSERT(g_register_count == 0);

	reset_mocks();
	MOCK_SET(l0_calloc, NULL);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	MOCK_CLEAR(l0_calloc);
	CU_ASSERT(g_unmap_count == 1);
	CU_ASSERT(g_close_count == 1);
	CU_ASSERT(g_register_count == 0);

	reset_mocks();
	MOCK_SET(l0_strdup, NULL);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	MOCK_CLEAR(l0_strdup);
	CU_ASSERT(g_unmap_count == 1);
	CU_ASSERT(g_close_count == 1);
	CU_ASSERT(g_register_count == 0);

	reset_mocks();
	g_register_result = -EIO;
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("test", sizeof(g_buffer)));
	CU_ASSERT(g_register_count == 1);
	CU_ASSERT(g_unregister_count == 0);
	CU_ASSERT(g_unmap_count == 1);
	CU_ASSERT(g_close_count == 1);
	CU_ASSERT(TAILQ_EMPTY(&g_l0_regions));
}

static void
test_region_lifetime_and_translation(void)
{
	struct spdk_l0_region *region, *found = NULL;
	uint64_t offset = UINT64_MAX, size = 0;
	unsigned char zero[sizeof(g_buffer)] = {};

	reset_mocks();
	MOCK_SET(get_l0_fd, -1);
	setenv("SPDK_NVMF_L0_DEVICE", g_expected_device, 1);
	region = spdk_l0_region_create("test", sizeof(g_buffer));
	SPDK_CU_ASSERT_FATAL(region != NULL);
	CU_ASSERT_STRING_EQUAL(region->name, "test");
	CU_ASSERT_PTR_EQUAL(spdk_l0_region_base(region), g_buffer);
	CU_ASSERT(spdk_l0_region_len(region) == sizeof(g_buffer));
	CU_ASSERT(spdk_l0_region_phys(region) == 0x10000000ULL);
	CU_ASSERT(spdk_l0_region_fd(region) == 42);
	CU_ASSERT(memcmp(g_buffer, zero, sizeof(g_buffer)) == 0);
	CU_ASSERT(g_register_count == 1);
	CU_ASSERT_PTR_NULL(spdk_l0_region_create("second", sizeof(g_buffer)));
	CU_ASSERT(errno == EBUSY);

	CU_ASSERT_TRUE(spdk_l0_find_region(g_buffer + 17, &found, &offset));
	CU_ASSERT_PTR_EQUAL(found, region);
	CU_ASSERT(offset == 17);
	CU_ASSERT_TRUE(spdk_l0_find_region(g_buffer, NULL, NULL));
	CU_ASSERT_FALSE(spdk_l0_find_region((void *)((uintptr_t)g_buffer - 1), &found, &offset));
	CU_ASSERT_PTR_NULL(found);
	CU_ASSERT_FALSE(spdk_l0_find_region(g_buffer + sizeof(g_buffer), NULL, NULL));
	CU_ASSERT(spdk_l0_vtophys(g_buffer + 17, &size) == 0x10000011ULL);
	CU_ASSERT(size == sizeof(g_buffer) - 17);
	CU_ASSERT(spdk_l0_vtophys(g_buffer, NULL) == 0x10000000ULL);
	CU_ASSERT(spdk_l0_vtophys(g_buffer + sizeof(g_buffer), &size) == SPDK_VTOPHYS_ERROR);
	CU_ASSERT(size == sizeof(g_buffer) - 17);

	spdk_l0_region_destroy(NULL);
	CU_ASSERT(g_unregister_count == 0);
	spdk_l0_region_destroy(region);
	CU_ASSERT(g_unregister_count == 1);
	CU_ASSERT(g_unmap_count == 1);
	CU_ASSERT(g_close_count == 1);
	CU_ASSERT_FALSE(spdk_l0_find_region(g_buffer, &found, &offset));
	CU_ASSERT_PTR_NULL(found);
	CU_ASSERT(TAILQ_EMPTY(&g_l0_regions));
}

static void
test_destroy_unregister_error(void)
{
	struct spdk_l0_region *region;

	reset_mocks();
	region = spdk_l0_region_create("test", sizeof(g_buffer));
	SPDK_CU_ASSERT_FATAL(region != NULL);
	g_unregister_result = -EIO;
	/* Exercise cleanup of a region whose descriptor has already been invalidated. */
	region->fd = -1;
	spdk_l0_region_destroy(region);
	CU_ASSERT(g_unregister_count == 1);
	CU_ASSERT(g_unmap_count == 1);
	CU_ASSERT(g_close_count == 0);
	CU_ASSERT(TAILQ_EMPTY(&g_l0_regions));
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int failures;
	char *saved_device = getenv("SPDK_NVMF_L0_DEVICE");

	saved_device = saved_device == NULL ? NULL : strdup(saved_device);
	if (CU_initialize_registry() != CUE_SUCCESS) {
		free(saved_device);
		return CU_get_error();
	}
	suite = CU_add_suite("l0", NULL, NULL);
	CU_ADD_TEST(suite, test_enable_flag);
	CU_ADD_TEST(suite, test_invalid_size_and_device);
	CU_ADD_TEST(suite, test_create_failures);
	CU_ADD_TEST(suite, test_region_lifetime_and_translation);
	CU_ADD_TEST(suite, test_destroy_unregister_error);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	if (saved_device != NULL) {
		setenv("SPDK_NVMF_L0_DEVICE", saved_device, 1);
		free(saved_device);
	} else {
		unsetenv("SPDK_NVMF_L0_DEVICE");
	}
	return failures;
}
