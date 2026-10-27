/* SPDX-License-Identifier: BSD-3-Clause */

#include "spdk_cunit.h"
#include "spdk/util.h"
#include "spdk_internal/mock.h"

#define UT_L0_ALIGN (2UL * 1024UL * 1024UL)
#define UT_BUFFER_SIZE 64
static unsigned char g_storage[UT_L0_ALIGN + UT_BUFFER_SIZE];
static unsigned char *g_buffer;
static struct {
	void *addr;
	int error;
} g_maps[16];
static size_t g_map_count, g_map_pos, g_unmap_count;
static size_t g_unmap_fail_at;
static int g_open_result, g_close_result, g_close_count;
static long g_pagesize;
static uint64_t g_pinfo;
static ssize_t g_pread_result;
static off_t g_expected_offset;
static bool g_open_device;

static int
platform_ut_open(const char *path, int flags, ...)
{
	if (g_open_device) {
		CU_ASSERT_STRING_EQUAL(path, "/dev/hisi_l0");
		CU_ASSERT(flags == O_RDWR);
	} else {
		CU_ASSERT_STRING_EQUAL(path, "/proc/123/pagemap");
		CU_ASSERT(flags == O_RDONLY);
	}
	return g_open_result;
}

static int
platform_ut_close(int fd)
{
	CU_ASSERT(fd == 42);
	g_close_count++;
	return g_close_result;
}

static long
platform_ut_sysconf(int name)
{
	CU_ASSERT(name == _SC_PAGESIZE);
	return g_pagesize;
}

static pid_t
platform_ut_getpid(void)
{
	return 123;
}

static ssize_t
platform_ut_pread(int fd, void *buf, size_t size, off_t offset)
{
	CU_ASSERT(fd == 42);
	CU_ASSERT(size == sizeof(g_pinfo));
	CU_ASSERT(offset == g_expected_offset);
	memcpy(buf, &g_pinfo, size);
	return g_pread_result;
}

static void *
platform_ut_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	SPDK_CU_ASSERT_FATAL(g_map_pos < g_map_count);
	CU_ASSERT(offset == 0);
	if (addr == NULL) {
		CU_ASSERT(len == UT_BUFFER_SIZE + UT_L0_ALIGN);
		CU_ASSERT(prot == PROT_NONE);
		CU_ASSERT(flags == (MAP_PRIVATE | MAP_ANONYMOUS));
		CU_ASSERT(fd == -1);
	} else {
		CU_ASSERT_PTR_EQUAL(addr, g_buffer);
		CU_ASSERT(len == UT_BUFFER_SIZE);
		CU_ASSERT(prot == (PROT_READ | PROT_WRITE));
		CU_ASSERT((flags & MAP_SHARED) != 0);
		CU_ASSERT(fd == 42);
	}
	errno = g_maps[g_map_pos].error;
	return g_maps[g_map_pos++].addr;
}

static int
platform_ut_munmap(void *addr, size_t len)
{
	g_unmap_count++;
	if (len == UT_BUFFER_SIZE + UT_L0_ALIGN) {
		CU_ASSERT((uintptr_t)addr == (uintptr_t)g_buffer - 17);
	} else {
		CU_ASSERT_PTR_EQUAL(addr, g_buffer + 1);
		CU_ASSERT(len == UT_BUFFER_SIZE);
	}
	return g_unmap_count == g_unmap_fail_at ? -1 : 0;
}

#define open platform_ut_open
#define close platform_ut_close
#define sysconf platform_ut_sysconf
#define getpid platform_ut_getpid
#define pread platform_ut_pread
#define mmap platform_ut_mmap
#define munmap platform_ut_munmap
#include "env_dpdk/l0_mmap_platform.c"
#undef open
#undef close
#undef sysconf
#undef getpid
#undef pread
#undef mmap
#undef munmap

static void
reset_mocks(void)
{
	g_buffer = (void *)(((uintptr_t)g_storage + UT_L0_ALIGN - 1) & ~(UT_L0_ALIGN - 1));
	g_map_count = g_map_pos = g_unmap_count = g_unmap_fail_at = 0;
	g_open_result = 42;
	g_close_result = g_close_count = 0;
	g_pagesize = 4096;
	g_pinfo = (1ULL << 63) | (1ULL << 55) | 0x12345;
	g_pread_result = sizeof(g_pinfo);
	g_expected_offset = (0x12345678ULL / 4096) * sizeof(g_pinfo);
	g_open_device = false;
	memset(g_buffer, 0xa5, UT_BUFFER_SIZE);
}

static void
add_map(void *addr, int error)
{
	SPDK_CU_ASSERT_FATAL(g_map_count < SPDK_COUNTOF(g_maps));
	g_maps[g_map_count].addr = addr;
	g_maps[g_map_count++].error = error;
}

static void
add_reserve(void)
{
	add_map((void *)((uintptr_t)g_buffer - 17), 0);
}

static void
test_get_fd_and_alignment(void)
{
	reset_mocks();
	g_open_device = true;
	CU_ASSERT(get_l0_fd() == 42);
	g_open_result = -1;
	CU_ASSERT(get_l0_fd() == -1);
	CU_ASSERT(align_up_uintptr(0, L0_ALIGN) == 0);
	CU_ASSERT(align_up_uintptr(L0_ALIGN, L0_ALIGN) == L0_ALIGN);
	CU_ASSERT(align_up_uintptr(L0_ALIGN + 1, L0_ALIGN) == 2 * L0_ALIGN);
}

static void
test_mmap_invalid_arguments(void)
{
	reset_mocks();
	CU_ASSERT_PTR_NULL(mmap_alloc(0, 42));
	CU_ASSERT(errno == EINVAL);
	CU_ASSERT_PTR_NULL(mmap_alloc(UT_BUFFER_SIZE, -1));
	CU_ASSERT(errno == EINVAL);
	CU_ASSERT_PTR_NULL(mmap_alloc(ULONG_MAX - L0_ALIGN + 1, 42));
	CU_ASSERT(errno == EOVERFLOW);
	CU_ASSERT(g_map_pos == 0);
	CU_ASSERT(g_unmap_count == 0);
}

static void
test_mmap_failures(void)
{
	reset_mocks();
	add_map(MAP_FAILED, ENOMEM);
	CU_ASSERT_PTR_NULL(mmap_alloc(UT_BUFFER_SIZE, 42));
	CU_ASSERT(errno == ENOMEM);
	CU_ASSERT(g_map_pos == 1 && g_unmap_count == 0);

	reset_mocks();
	add_reserve();
	g_unmap_fail_at = 1;
	CU_ASSERT_PTR_NULL(mmap_alloc(UT_BUFFER_SIZE, 42));
	CU_ASSERT(g_map_pos == 1 && g_unmap_count == 1);

	reset_mocks();
	add_reserve();
	add_map(MAP_FAILED, EACCES);
	CU_ASSERT_PTR_NULL(mmap_alloc(UT_BUFFER_SIZE, 42));
	CU_ASSERT(errno == EACCES);
	CU_ASSERT(g_map_pos == 2 && g_unmap_count == 1);

	reset_mocks();
	add_reserve();
	add_map(g_buffer + 1, 0);
	CU_ASSERT_PTR_NULL(mmap_alloc(UT_BUFFER_SIZE, 42));
	CU_ASSERT(errno == EFAULT);
	CU_ASSERT(g_map_pos == 2 && g_unmap_count == 2);
}

static void
test_mmap_retry_and_success(void)
{
	unsigned char zero[UT_BUFFER_SIZE] = {};
	size_t i;

	reset_mocks();
	add_reserve();
	add_map(MAP_FAILED, EEXIST);
	add_reserve();
	add_map(MAP_FAILED, EINVAL);
	add_reserve();
	add_map(g_buffer, 0);
	CU_ASSERT_PTR_EQUAL(mmap_alloc(UT_BUFFER_SIZE, 42), g_buffer);
	CU_ASSERT(g_map_pos == 6 && g_unmap_count == 3);
	CU_ASSERT(memcmp(g_buffer, zero, UT_BUFFER_SIZE) == 0);

	reset_mocks();
	for (i = 0; i < L0_RETRY_MAX; i++) {
		add_reserve();
		add_map(MAP_FAILED, EEXIST);
	}
	CU_ASSERT_PTR_NULL(mmap_alloc(UT_BUFFER_SIZE, 42));
	CU_ASSERT(errno == EBUSY);
	CU_ASSERT(g_map_pos == 2 * L0_RETRY_MAX);
	CU_ASSERT(g_unmap_count == L0_RETRY_MAX);
}

static void
test_vtop_failures_and_translation(void)
{
	reset_mocks();
	g_pagesize = -1;
	CU_ASSERT(vtop(0x12345678ULL) == ULLONG_MAX);
	g_pagesize = 0;
	CU_ASSERT(vtop(0x12345678ULL) == ULLONG_MAX);
	CU_ASSERT(g_close_count == 0);
	g_pagesize = 4096;
	g_open_result = -1;
	CU_ASSERT(vtop(0x12345678ULL) == ULLONG_MAX);
	CU_ASSERT(g_close_count == 0);
	g_open_result = 42;
	g_pread_result = -1;
	CU_ASSERT(vtop(0x12345678ULL) == ULLONG_MAX);
	CU_ASSERT(g_close_count == 1);
	g_pread_result = sizeof(g_pinfo) - 1;
	CU_ASSERT(vtop(0x12345678ULL) == ULLONG_MAX);
	CU_ASSERT(g_close_count == 2);
	g_pread_result = sizeof(g_pinfo);
	g_close_result = -1;
	CU_ASSERT(vtop(0x12345678ULL) == ULLONG_MAX);
	CU_ASSERT(g_close_count == 3);
	g_close_result = 0;
	g_pinfo &= ~(1ULL << 63);
	CU_ASSERT(vtop(0x12345678ULL) == ULLONG_MAX);
	CU_ASSERT(g_close_count == 4);
	g_pinfo |= 1ULL << 63;
	CU_ASSERT(vtop(0x12345678ULL) == 0x12345678ULL);
	CU_ASSERT(g_close_count == 5);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int failures;

	if (CU_initialize_registry() != CUE_SUCCESS) {
		return CU_get_error();
	}
	suite = CU_add_suite("l0_mmap_platform", NULL, NULL);
	CU_ADD_TEST(suite, test_get_fd_and_alignment);
	CU_ADD_TEST(suite, test_mmap_invalid_arguments);
	CU_ADD_TEST(suite, test_mmap_failures);
	CU_ADD_TEST(suite, test_mmap_retry_and_success);
	CU_ADD_TEST(suite, test_vtop_failures_and_translation);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return failures;
}
