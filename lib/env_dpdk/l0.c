/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk_internal/l0.h"

#define SPDK_L0_MAX_BYTES (64ULL * 1024ULL * 1024ULL)

struct spdk_l0_region {
	TAILQ_ENTRY(spdk_l0_region)	link;
	char				*name;
	void				*vaddr;
	size_t				len;
	uint64_t			phys_addr;
	int				fd;
};

static TAILQ_HEAD(, spdk_l0_region) g_l0_regions = TAILQ_HEAD_INITIALIZER(g_l0_regions);
static pthread_mutex_t g_l0_mutex = PTHREAD_MUTEX_INITIALIZER;

int get_l0_fd(void);
void *mmap_alloc(unsigned long size, int fd);
unsigned long long vtop(unsigned long long addr);

static bool
l0_env_flag_enabled(const char *name)
{
	const char *value;

	value = getenv(name);
	if (value == NULL) {
		return false;
	}

	return strcasecmp(value, "1") == 0 ||
	       strcasecmp(value, "y") == 0 ||
	       strcasecmp(value, "yes") == 0 ||
	       strcasecmp(value, "true") == 0 ||
	       strcasecmp(value, "on") == 0;
}

bool
spdk_l0_data_pool_enabled(void)
{
	return l0_env_flag_enabled("SPDK_NVMF_L0_ENABLE");
}

static struct spdk_l0_region *
l0_find_region_locked(const void *addr, uint64_t *offset)
{
	struct spdk_l0_region *region;
	uintptr_t value = (uintptr_t)addr;
	uintptr_t start, end;

	TAILQ_FOREACH(region, &g_l0_regions, link) {
		start = (uintptr_t)region->vaddr;
		end = start + region->len;
		if (value >= start && value < end) {
			if (offset != NULL) {
				*offset = value - start;
			}
			return region;
		}
	}

	return NULL;
}

struct spdk_l0_region *
spdk_l0_region_create(const char *name, size_t len)
{
	struct spdk_l0_region *region;
	const char *device_path;
	unsigned long long phys_addr;
	void *vaddr;
	int fd;

	if (len == 0) {
		errno = EINVAL;
		return NULL;
	}

	if (len > SPDK_L0_MAX_BYTES) {
		SPDK_ERRLOG("Requested L0 pool size %zu exceeds 64MiB limit\n", len);
		errno = ENOMEM;
		return NULL;
	}

	pthread_mutex_lock(&g_l0_mutex);
	if (!TAILQ_EMPTY(&g_l0_regions)) {
		pthread_mutex_unlock(&g_l0_mutex);
		SPDK_ERRLOG("Only one L0 region is supported by this patch\n");
		errno = EBUSY;
		return NULL;
	}
	pthread_mutex_unlock(&g_l0_mutex);

	fd = get_l0_fd();
	if (fd < 0) {
		device_path = getenv("SPDK_NVMF_L0_DEVICE");
		if (device_path == NULL || device_path[0] == '\0') {
			SPDK_ERRLOG("L0 fd getter is unavailable and SPDK_NVMF_L0_DEVICE is not set\n");
			errno = EINVAL;
			return NULL;
		}

		fd = open(device_path, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			SPDK_ERRLOG("Failed to open L0 device %s: %s\n", device_path, strerror(errno));
			return NULL;
		}
	}

	vaddr = mmap_alloc(len, fd);
	if (vaddr == NULL || vaddr == MAP_FAILED) {
		SPDK_ERRLOG("mmap_alloc(%zu) failed: %s\n", len, strerror(errno));
		close(fd);
		return NULL;
	}

	memset(vaddr, 0, len);

	phys_addr = vtop((unsigned long long)(uintptr_t)vaddr);
	if (phys_addr == SPDK_VTOPHYS_ERROR) {
		SPDK_ERRLOG("vtop(%p) failed for L0 region\n", vaddr);
		munmap(vaddr, len);
		close(fd);
		errno = EFAULT;
		return NULL;
	}

	region = calloc(1, sizeof(*region));
	if (region == NULL) {
		munmap(vaddr, len);
		close(fd);
		return NULL;
	}

	region->name = strdup(name);
	if (region->name == NULL) {
		free(region);
		munmap(vaddr, len);
		close(fd);
		return NULL;
	}

	region->vaddr = vaddr;
	region->len = len;
	region->phys_addr = phys_addr;
	region->fd = fd;

	if (spdk_mem_register(region->vaddr, region->len) != 0) {
		SPDK_ERRLOG("spdk_mem_register(%p, %zu) failed for L0 region\n",
			    region->vaddr, region->len);
		free(region->name);
		free(region);
		munmap(vaddr, len);
		close(fd);
		return NULL;
	}

	pthread_mutex_lock(&g_l0_mutex);
	TAILQ_INSERT_TAIL(&g_l0_regions, region, link);
	pthread_mutex_unlock(&g_l0_mutex);

	SPDK_NOTICELOG("Allocated L0 region %s vaddr=%p phys=0x%" PRIx64 " len=%zu\n",
		       region->name, region->vaddr, region->phys_addr, region->len);
	return region;
}

void
spdk_l0_region_destroy(struct spdk_l0_region *region)
{
	if (region == NULL) {
		return;
	}

	pthread_mutex_lock(&g_l0_mutex);
	TAILQ_REMOVE(&g_l0_regions, region, link);
	pthread_mutex_unlock(&g_l0_mutex);

	if (spdk_mem_unregister(region->vaddr, region->len) != 0) {
		SPDK_ERRLOG("spdk_mem_unregister(%p, %zu) failed for L0 region\n",
			    region->vaddr, region->len);
	}

	munmap(region->vaddr, region->len);
	if (region->fd >= 0) {
		close(region->fd);
	}
	free(region->name);
	free(region);
}

bool
spdk_l0_find_region(const void *addr, struct spdk_l0_region **region, uint64_t *offset)
{
	struct spdk_l0_region *tmp;

	pthread_mutex_lock(&g_l0_mutex);
	tmp = l0_find_region_locked(addr, offset);
	pthread_mutex_unlock(&g_l0_mutex);

	if (region != NULL) {
		*region = tmp;
	}

	return tmp != NULL;
}

uint64_t
spdk_l0_vtophys(const void *buf, uint64_t *size)
{
	struct spdk_l0_region *region;
	uint64_t offset = 0;

	pthread_mutex_lock(&g_l0_mutex);
	region = l0_find_region_locked(buf, &offset);
	if (region != NULL && size != NULL) {
		*size = region->len - offset;
	}
	pthread_mutex_unlock(&g_l0_mutex);

	if (region == NULL) {
		return SPDK_VTOPHYS_ERROR;
	}

	return region->phys_addr + offset;
}

void *
spdk_l0_region_base(struct spdk_l0_region *region)
{
	return region->vaddr;
}

size_t
spdk_l0_region_len(struct spdk_l0_region *region)
{
	return region->len;
}

uint64_t
spdk_l0_region_phys(struct spdk_l0_region *region)
{
	return region->phys_addr;
}

int
spdk_l0_region_fd(struct spdk_l0_region *region)
{
	return region->fd;
}
