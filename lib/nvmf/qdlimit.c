/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#include "qdlimit.h"
#include "spdk/bdev.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_transport.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "nvmf_internal.h"

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

/* One entry per backing SSD (bdev) seen on this poll group's core. Touched only by the
 * owning poll thread, so inflight is updated lock-free. */
struct qdlimit_pg_ssd {
	struct spdk_bdev		*bdev;		/* identity key for this core */
	struct qdlimit_config_entry	*cfg;		/* stable pointer; read ->depth on hot path */
	uint32_t			inflight;	/* admitted, buffer-holding requests on this core */
	STAILQ_HEAD(, spdk_nvmf_request) wait_q;	/* throttled requests, FIFO (uses req->buf_link) */
	TAILQ_ENTRY(qdlimit_pg_ssd)	link;
};

struct qdlimit_pg_ctx {
	TAILQ_HEAD(, qdlimit_pg_ssd)	ssds;
};

void
nvmf_qdlimit_pg_init(struct spdk_nvmf_transport_poll_group *group)
{
	struct qdlimit_pg_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx == NULL) {
		SPDK_ERRLOG("Failed to allocate qdlimit poll-group context\n");
		group->qdlimit_ctx = NULL;
		return;
	}
	TAILQ_INIT(&ctx->ssds);
	group->qdlimit_ctx = ctx;
}

void
nvmf_qdlimit_pg_fini(struct spdk_nvmf_transport_poll_group *group)
{
	struct qdlimit_pg_ctx *ctx = group->qdlimit_ctx;
	struct qdlimit_pg_ssd *s, *tmp;

	if (ctx == NULL) {
		return;
	}
	TAILQ_FOREACH_SAFE(s, &ctx->ssds, link, tmp) {
		/* Parked requests must have been drained by the transport teardown path
		 * (it walks pending_buf_queue + wait queues). Assert none leaked. */
		assert(STAILQ_EMPTY(&s->wait_q));
		TAILQ_REMOVE(&ctx->ssds, s, link);
		free(s);
	}
	free(ctx);
	group->qdlimit_ctx = NULL;
}

/* Find-or-create the per-core entry for bdev. Returns NULL only if the bdev has never been
 * configured (no config entry exists); callers treat NULL as "unlimited, bypass". Note a
 * configured-but-zero depth still returns a valid entry — the depth==0 bypass is applied at
 * the admission gate (cfg->depth == 0), not here. */
static struct qdlimit_pg_ssd *
qdlimit_pg_get_ssd(struct qdlimit_pg_ctx *ctx, struct spdk_bdev *bdev, const char *bdev_name)
{
	struct qdlimit_pg_ssd *s;
	struct qdlimit_config_entry *cfg;

	TAILQ_FOREACH(s, &ctx->ssds, link) {
		if (s->bdev == bdev) {
			return s;
		}
	}

	pthread_mutex_lock(&g_qdlimit_config_lock);
	cfg = qdlimit_config_find(bdev_name);
	pthread_mutex_unlock(&g_qdlimit_config_lock);
	if (cfg == NULL) {
		return NULL;	/* never configured: unlimited */
	}

	s = calloc(1, sizeof(*s));
	if (s == NULL) {
		SPDK_ERRLOG("Failed to allocate qdlimit per-SSD entry for %s\n", bdev_name);
		return NULL;
	}
	s->bdev = bdev;
	s->cfg = cfg;
	STAILQ_INIT(&s->wait_q);
	TAILQ_INSERT_TAIL(&ctx->ssds, s, link);
	return s;
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

static enum nvmf_qdlimit_status
qdlimit_admit_bdev(struct spdk_nvmf_transport_poll_group *group,
		   struct spdk_nvmf_request *req, struct spdk_bdev *bdev, const char *bdev_name)
{
	struct qdlimit_pg_ctx *ctx = group->qdlimit_ctx;
	struct qdlimit_pg_ssd *s;

	if (ctx == NULL) {
		return NVMF_QDLIMIT_ADMIT;	/* module not active on this group */
	}
	s = qdlimit_pg_get_ssd(ctx, bdev, bdev_name);
	if (s == NULL || s->cfg->depth == 0) {
		return NVMF_QDLIMIT_ADMIT;	/* unconfigured / unlimited */
	}
	if (s->inflight >= s->cfg->depth) {
		/* Over limit: take it off the shared queue so other SSDs are not blocked,
		 * and park it on this SSD's FIFO wait queue (reuses req->buf_link). */
		STAILQ_REMOVE(&group->pending_buf_queue, req, spdk_nvmf_request, buf_link);
		STAILQ_INSERT_TAIL(&s->wait_q, req, buf_link);
		req->qdlimit_charged = false;
		return NVMF_QDLIMIT_THROTTLED;
	}
	s->inflight++;
	req->qdlimit_charged = true;
	return NVMF_QDLIMIT_ADMIT;
}

/* Resolve nsid -> ns -> bdev using existing helpers, then gate. Bypass anything we cannot
 * resolve or that carries no data. */
enum nvmf_qdlimit_status
nvmf_qdlimit_admit(struct spdk_nvmf_transport_poll_group *group, struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_ns *ns;
	struct spdk_bdev *bdev;

	if (req->xfer == SPDK_NVME_DATA_NONE) {
		return NVMF_QDLIMIT_ADMIT;
	}
	if (req->qpair == NULL) {
		return NVMF_QDLIMIT_ADMIT;
	}
	ctrlr = req->qpair->ctrlr;
	if (ctrlr == NULL || ctrlr->subsys == NULL || req->cmd == NULL) {
		return NVMF_QDLIMIT_ADMIT;
	}
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, req->cmd->nvme_cmd.nsid);
	if (ns == NULL || ns->bdev == NULL) {
		return NVMF_QDLIMIT_ADMIT;
	}
	bdev = ns->bdev;
	return qdlimit_admit_bdev(group, req, bdev, spdk_bdev_get_name(bdev));
}

static void
qdlimit_release_bdev(struct spdk_nvmf_transport_poll_group *group,
		     struct spdk_nvmf_request *req, struct spdk_bdev *bdev)
{
	struct qdlimit_pg_ctx *ctx = group->qdlimit_ctx;
	struct qdlimit_pg_ssd *s;
	struct spdk_nvmf_request *next;

	if (ctx == NULL || !req->qdlimit_charged) {
		return;
	}
	req->qdlimit_charged = false;

	TAILQ_FOREACH(s, &ctx->ssds, link) {
		if (s->bdev == bdev) {
			break;
		}
	}
	if (s == NULL) {
		return;		/* should not happen for a charged request */
	}

	assert(s->inflight > 0);
	s->inflight--;

	/* Re-arm the oldest waiter for this SSD by putting it back at the head of the shared
	 * queue; the next poll re-runs the gate, which now passes (inflight < depth). */
	if (!STAILQ_EMPTY(&s->wait_q)) {
		next = STAILQ_FIRST(&s->wait_q);
		STAILQ_REMOVE_HEAD(&s->wait_q, buf_link);
		STAILQ_INSERT_HEAD(&group->pending_buf_queue, next, buf_link);
	}
}

void
nvmf_qdlimit_release(struct spdk_nvmf_transport_poll_group *group, struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ctrlr *ctrlr;
	struct spdk_nvmf_ns *ns;

	if (!req->qdlimit_charged) {
		return;
	}
	if (req->qpair == NULL || (ctrlr = req->qpair->ctrlr) == NULL || ctrlr->subsys == NULL) {
		req->qdlimit_charged = false;	/* lost the mapping; just clear to avoid leaks */
		return;
	}
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, req->cmd->nvme_cmd.nsid);
	if (ns == NULL || ns->bdev == NULL) {
		req->qdlimit_charged = false;
		return;
	}
	qdlimit_release_bdev(group, req, ns->bdev);
}

/* Remove req from a per-SSD wait queue if it is currently parked there. Returns true if it
 * was parked (and has now been removed), false if it was not on any wait queue. Used by the
 * transport abort path: a throttled request in NEED_BUFFER state lives on a wait_q, NOT on
 * pending_buf_queue, so the abort handler must consult this before doing its own
 * STAILQ_REMOVE(pending_buf_queue). A throttled request is never charged, so no counter
 * adjustment is needed here. */
bool
nvmf_qdlimit_abort_dequeue(struct spdk_nvmf_transport_poll_group *group,
			   struct spdk_nvmf_request *req)
{
	struct qdlimit_pg_ctx *ctx = group->qdlimit_ctx;
	struct qdlimit_pg_ssd *s;
	struct spdk_nvmf_request *w;

	if (ctx == NULL) {
		return false;
	}
	TAILQ_FOREACH(s, &ctx->ssds, link) {
		STAILQ_FOREACH(w, &s->wait_q, buf_link) {
			if (w == req) {
				STAILQ_REMOVE(&s->wait_q, req, spdk_nvmf_request, buf_link);
				return true;
			}
		}
	}
	return false;
}

/* Drain any parked requests (used by teardown and tests) by completing them back onto
 * pending_buf_queue head; the transport's own teardown then fails/flushes them. */
void
nvmf_qdlimit_pg_fini_drain(struct spdk_nvmf_transport_poll_group *group)
{
	struct qdlimit_pg_ctx *ctx = group->qdlimit_ctx;
	struct qdlimit_pg_ssd *s;
	struct spdk_nvmf_request *req;

	if (ctx == NULL) {
		return;
	}
	TAILQ_FOREACH(s, &ctx->ssds, link) {
		while (!STAILQ_EMPTY(&s->wait_q)) {
			req = STAILQ_FIRST(&s->wait_q);
			STAILQ_REMOVE_HEAD(&s->wait_q, buf_link);
			STAILQ_INSERT_HEAD(&group->pending_buf_queue, req, buf_link);
		}
	}
	nvmf_qdlimit_pg_fini(group);
}
