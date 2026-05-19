/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define L0_DEV "/dev/hisi_l0"
#define L0_ALIGN (2UL * 1024UL * 1024UL)
#define L0_RETRY_MAX 5

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE MAP_FIXED
#endif

static inline uintptr_t
align_up_uintptr(uintptr_t value, uintptr_t align)
{
	return (value + align - 1) & ~(align - 1);
}

int
get_l0_fd(void)
{
	return open(L0_DEV, O_RDWR);
}

void *
mmap_alloc(unsigned long size, int fd)
{
	void *reserve_addr;
	void *mapped_addr;
	uintptr_t aligned_addr;
	unsigned long reserve_len;
	int retry = L0_RETRY_MAX;

	if (size == 0 || fd < 0) {
		errno = EINVAL;
		return NULL;
	}

	if (size > ULONG_MAX - L0_ALIGN) {
		errno = EOVERFLOW;
		return NULL;
	}

	reserve_len = size + L0_ALIGN;
	while (retry-- > 0) {
		reserve_addr = mmap(NULL, reserve_len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (reserve_addr == MAP_FAILED) {
			fprintf(stderr, "reserve mmap failed, errno=%d\n", errno);
			return NULL;
		}

		aligned_addr = align_up_uintptr((uintptr_t)reserve_addr, L0_ALIGN);
		if (munmap(reserve_addr, reserve_len) == -1) {
			return NULL;
		}

		mapped_addr = mmap((void *)aligned_addr, size, PROT_READ | PROT_WRITE,
				   MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
		if (mapped_addr == MAP_FAILED) {
			if (errno == EEXIST || errno == EINVAL) {
				continue;
			}
			fprintf(stderr, "L0 mmap failed, errno=%d\n", errno);
			return NULL;
		}

		if ((uintptr_t)mapped_addr != aligned_addr) {
			munmap(mapped_addr, size);
			errno = EFAULT;
			return NULL;
		}

		memset(mapped_addr, 0, size);
		return mapped_addr;
	}

	errno = EBUSY;
	return NULL;
}

unsigned long long
vtop(unsigned long long addr)
{
	unsigned long long pinfo;
	long pagesize = sysconf(_SC_PAGESIZE);
	off_t offset;
	char pagemapname[64];
	int fd;

	if (pagesize <= 0) {
		return (unsigned long long)-1;
	}

	snprintf(pagemapname, sizeof(pagemapname), "/proc/%d/pagemap", getpid());
	fd = open(pagemapname, O_RDONLY);
	if (fd == -1) {
		perror(pagemapname);
		return (unsigned long long)-1;
	}

	offset = (off_t)((addr / (unsigned long long)pagesize) * sizeof(pinfo));
	if (pread(fd, &pinfo, sizeof(pinfo), offset) != sizeof(pinfo)) {
		perror(pagemapname);
		close(fd);
		return (unsigned long long)-1;
	}

	if (close(fd) != 0) {
		return (unsigned long long)-1;
	}

	if ((pinfo & (1ull << 63)) == 0) {
		fprintf(stderr, "page not present for 0x%llx\n", addr);
		return (unsigned long long)-1;
	}

	return ((pinfo & 0x007fffffffffffffull) * (unsigned long long)pagesize) +
	       (addr & (unsigned long long)(pagesize - 1));
}
