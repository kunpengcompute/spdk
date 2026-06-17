/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#include "qdlimit.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* Global per-SSD config. Entries are created on first set/use and never freed at runtime
 * (only at config_cleanup), so the per-pg hot path can cache a stable pointer and read
 * ->depth without locking. depth == 0 means unlimited. */
struct qdlimit_config_entry {
	char				bdev_name[256];
	uint32_t			depth;
	TAILQ_ENTRY(qdlimit_config_entry) link;
};

static TAILQ_HEAD(, qdlimit_config_entry) g_qdlimit_config =
	TAILQ_HEAD_INITIALIZER(g_qdlimit_config);
static pthread_mutex_t g_qdlimit_config_lock = PTHREAD_MUTEX_INITIALIZER;

/* Caller must hold g_qdlimit_config_lock. */
static struct qdlimit_config_entry *
qdlimit_config_find(const char *bdev_name)
{
	struct qdlimit_config_entry *e;

	TAILQ_FOREACH(e, &g_qdlimit_config, link) {
		if (strcmp(e->bdev_name, bdev_name) == 0) {
			return e;
		}
	}
	return NULL;
}

/* Caller must hold g_qdlimit_config_lock. Creates the entry if absent. */
static struct qdlimit_config_entry *
qdlimit_config_get_or_create(const char *bdev_name)
{
	struct qdlimit_config_entry *e = qdlimit_config_find(bdev_name);

	if (e != NULL) {
		return e;
	}
	e = calloc(1, sizeof(*e));
	if (e == NULL) {
		SPDK_ERRLOG("Failed to allocate qdlimit config entry for %s\n", bdev_name);
		return NULL;
	}
	spdk_strcpy_pad(e->bdev_name, bdev_name, sizeof(e->bdev_name), '\0');
	TAILQ_INSERT_TAIL(&g_qdlimit_config, e, link);
	return e;
}

int
nvmf_qdlimit_set_depth(const char *bdev_name, uint32_t depth)
{
	struct qdlimit_config_entry *e;

	if (bdev_name == NULL || bdev_name[0] == '\0') {
		return -EINVAL;
	}
	if (strlen(bdev_name) >= sizeof(e->bdev_name)) {
		return -ENAMETOOLONG;
	}

	pthread_mutex_lock(&g_qdlimit_config_lock);
	e = qdlimit_config_get_or_create(bdev_name);
	if (e == NULL) {
		pthread_mutex_unlock(&g_qdlimit_config_lock);
		return -ENOMEM;
	}
	e->depth = depth;
	pthread_mutex_unlock(&g_qdlimit_config_lock);
	return 0;
}

int
nvmf_qdlimit_get_depth(const char *bdev_name, uint32_t *depth)
{
	struct qdlimit_config_entry *e;

	if (bdev_name == NULL || depth == NULL) {
		return -EINVAL;
	}
	pthread_mutex_lock(&g_qdlimit_config_lock);
	e = qdlimit_config_find(bdev_name);
	*depth = (e != NULL) ? e->depth : 0;
	pthread_mutex_unlock(&g_qdlimit_config_lock);
	return (e != NULL) ? 0 : -ENOENT;
}

void
nvmf_qdlimit_config_cleanup(void)
{
	struct qdlimit_config_entry *e, *tmp;

	pthread_mutex_lock(&g_qdlimit_config_lock);
	TAILQ_FOREACH_SAFE(e, &g_qdlimit_config, link, tmp) {
		TAILQ_REMOVE(&g_qdlimit_config, e, link);
		free(e);
	}
	pthread_mutex_unlock(&g_qdlimit_config_lock);
}
