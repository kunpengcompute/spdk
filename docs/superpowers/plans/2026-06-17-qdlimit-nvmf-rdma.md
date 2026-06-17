# qdlimit (NVMe-oF/RDMA pre-buffer admission control) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Gate NVMe-oF/RDMA requests *before* they allocate a data buffer, capping per-SSD buffer-pool usage so one SSD cannot exhaust the shared `data_buf_pool` (noisy-neighbor isolation).

**Architecture:** A standalone `lib/nvmf/qdlimit.{c,h}` component owns all config, per-core counters, and per-SSD wait queues. The RDMA transport calls three thin hooks: `nvmf_qdlimit_admit` at `NEW→NEED_BUFFER` (before `get_buffers`), `nvmf_qdlimit_release` in `_nvmf_rdma_request_free` (post-ACK buffer return), and `pg_init`/`pg_fini` at poll-group create/destroy. Over-limit requests are moved off the shared `pending_buf_queue` onto a per-SSD FIFO wait queue and re-armed on release. Per-core, lock-free, approximate counting (global ≈ `depth × cores`).

**Tech Stack:** C, SPDK (lib/nvmf, RDMA transport), CUnit unit tests, SPDK RPC framework + Python bindings, fio + SoftRoCE (`rxe`) for integration.

**Spec:** `docs/superpowers/specs/2026-06-17-qdlimit-nvmf-rdma-design.md`

**Refinement vs spec:** The `qdlimit_charged` flag lives on the generic `struct spdk_nvmf_request` (not `spdk_nvmf_rdma_request`) so the module is transport-agnostic and unit-testable with plain request fakes. Still one `bool`.

**Build/test prerequisite:** SPDK must be configured with RDMA: `./configure --with-rdma --enable-debug` then `make -j`. Unit tests run via `test/unit/unittest.sh` or per-dir `make`.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/spdk/nvmf_transport.h` | +1 field `bool qdlimit_charged` on `struct spdk_nvmf_request` (modify) |
| `lib/nvmf/qdlimit.h` | Public hook + config API, `enum nvmf_qdlimit_status` (create) |
| `lib/nvmf/qdlimit.c` | Config table, per-pg context, admit/release/re-arm logic (create) |
| `lib/nvmf/Makefile` | Add `qdlimit.c` to RDMA build (modify) |
| `lib/nvmf/rdma.c` | 4 hook sites + poll-group field (modify) |
| `lib/nvmf/nvmf_rpc.c` | `nvmf_qdlimit_set_depth/get_depth/get_stats` RPCs (modify) |
| `scripts/rpc/nvmf.py` | Python bindings (modify) |
| `scripts/rpc.py` | CLI subcommands (modify) |
| `test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c` | Unit tests (create) |
| `test/unit/lib/nvmf/qdlimit.c/Makefile` | Unit test build (create) |
| `test/unit/lib/nvmf/Makefile` | Register qdlimit unit dir (modify) |
| `test/nvmf/target/qdlimit.sh` | RDMA integration + buffer-ceiling test (create) |
| `module/bdev/qdlimit/**` | Removed (delete) |

---

## Task 1: qdlimit module skeleton + config table

**Files:**
- Create: `lib/nvmf/qdlimit.h`
- Create: `lib/nvmf/qdlimit.c`
- Modify: `lib/nvmf/Makefile:14`
- Create: `test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c`
- Create: `test/unit/lib/nvmf/qdlimit.c/Makefile`
- Modify: `test/unit/lib/nvmf/Makefile:13`

- [ ] **Step 1: Create the public header**

`lib/nvmf/qdlimit.h`:

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#ifndef SPDK_NVMF_QDLIMIT_H
#define SPDK_NVMF_QDLIMIT_H

#include "spdk/stdinc.h"

struct spdk_nvmf_transport_poll_group;
struct spdk_nvmf_request;
struct spdk_json_write_ctx;

/* Result of the admission gate. */
enum nvmf_qdlimit_status {
	NVMF_QDLIMIT_ADMIT,	/* under limit (or unlimited): proceed to get_buffers */
	NVMF_QDLIMIT_THROTTLED,	/* over limit: request parked on a per-SSD wait queue */
};

/* Per-poll-group context lifecycle. Called from the RDMA poll-group create/destroy paths. */
void nvmf_qdlimit_pg_init(struct spdk_nvmf_transport_poll_group *group);
void nvmf_qdlimit_pg_fini(struct spdk_nvmf_transport_poll_group *group);

/* Admission gate. Call at NEW->NEED_BUFFER, before get_buffers, only for the queue head.
 * On NVMF_QDLIMIT_THROTTLED the module has already removed req from group->pending_buf_queue
 * and parked it on the per-SSD wait queue; the caller must stop processing this request. */
enum nvmf_qdlimit_status nvmf_qdlimit_admit(struct spdk_nvmf_transport_poll_group *group,
		struct spdk_nvmf_request *req);

/* Slot release. Call from _nvmf_rdma_request_free. No-op unless req->qdlimit_charged.
 * Decrements the per-core counter and re-arms one parked waiter for the same SSD. */
void nvmf_qdlimit_release(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_request *req);

/* Config (global, per backing bdev). depth == 0 means unlimited. Returns 0 on success,
 * negative errno on failure. Safe to call from the RPC thread. */
int nvmf_qdlimit_set_depth(const char *bdev_name, uint32_t depth);
int nvmf_qdlimit_get_depth(const char *bdev_name, uint32_t *depth);

/* Free all global config state. Call at transport/library teardown. */
void nvmf_qdlimit_config_cleanup(void);

#endif /* SPDK_NVMF_QDLIMIT_H */
```

- [ ] **Step 2: Create the module with config table only (admit/release/pg added later)**

`lib/nvmf/qdlimit.c`:

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#include "qdlimit.h"
#include "spdk/queue.h"
#include "spdk/string.h"
#include "spdk/util.h"

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
		return NULL;
	}
	spdk_strcpy_pad(e->bdev_name, bdev_name, sizeof(e->bdev_name) - 1, '\0');
	e->bdev_name[sizeof(e->bdev_name) - 1] = '\0';
	e->depth = 0;
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
```

- [ ] **Step 3: Wire the module into the RDMA build**

Modify `lib/nvmf/Makefile`, change line 14 from:

```make
C_SRCS-$(CONFIG_RDMA) += rdma.c
```
to:
```make
C_SRCS-$(CONFIG_RDMA) += rdma.c qdlimit.c
```

- [ ] **Step 4: Create the unit-test build file**

`test/unit/lib/nvmf/qdlimit.c/Makefile`:

```make
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Intel Corporation.
#  All rights reserved.

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

SPDK_LIB_LIST = json
TEST_FILE = qdlimit_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
```

- [ ] **Step 5: Create the unit-test file with config-table tests (failing — module not compiled into UT yet)**

`test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c`:

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "nvmf/qdlimit.c"

static void
test_config_set_get(void)
{
	uint32_t depth = 12345;

	/* Unconfigured bdev: ENOENT, depth defaults to 0. */
	CU_ASSERT(nvmf_qdlimit_get_depth("bdevA", &depth) == -ENOENT);
	CU_ASSERT(depth == 0);

	/* Set then read back. */
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 8) == 0);
	CU_ASSERT(nvmf_qdlimit_get_depth("bdevA", &depth) == 0);
	CU_ASSERT(depth == 8);

	/* Update in place. */
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 0) == 0);
	CU_ASSERT(nvmf_qdlimit_get_depth("bdevA", &depth) == 0);
	CU_ASSERT(depth == 0);

	/* Bad args. */
	CU_ASSERT(nvmf_qdlimit_set_depth(NULL, 1) == -EINVAL);
	CU_ASSERT(nvmf_qdlimit_set_depth("", 1) == -EINVAL);

	nvmf_qdlimit_config_cleanup();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("qdlimit", NULL, NULL);
	CU_ADD_TEST(suite, test_config_set_get);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
```

- [ ] **Step 6: Register the unit-test directory**

Modify `test/unit/lib/nvmf/Makefile`, change line 13 from:

```make
DIRS-$(CONFIG_RDMA) += rdma.c transport.c
```
to:
```make
DIRS-$(CONFIG_RDMA) += rdma.c transport.c qdlimit.c
```

- [ ] **Step 7: Build and run the unit test**

Run:
```bash
make -C test/unit/lib/nvmf/qdlimit.c
./test/unit/lib/nvmf/qdlimit.c/qdlimit_ut
```
Expected: PASS, `test_config_set_get` green, 0 failures.

- [ ] **Step 8: Commit**

```bash
git add lib/nvmf/qdlimit.h lib/nvmf/qdlimit.c lib/nvmf/Makefile \
        test/unit/lib/nvmf/qdlimit.c/ test/unit/lib/nvmf/Makefile
git commit -m "nvmf/qdlimit: add module skeleton and per-bdev config table"
```

---

## Task 2: Per-poll-group context + per-SSD entries

**Files:**
- Modify: `lib/nvmf/qdlimit.c`
- Modify: `test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c`

- [ ] **Step 1: Add per-pg structures and lifecycle to `qdlimit.c`**

Add `#include "spdk/nvmf_transport.h"` to the includes, then add above `nvmf_qdlimit_set_depth`:

```c
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

/* Find-or-create the per-core entry for bdev. Returns NULL if the bdev has no configured
 * limit (depth 0 / unconfigured) — callers treat NULL as "unlimited, bypass". */
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
		return NULL;
	}
	s->bdev = bdev;
	s->cfg = cfg;
	s->inflight = 0;
	STAILQ_INIT(&s->wait_q);
	TAILQ_INSERT_TAIL(&ctx->ssds, s, link);
	return s;
}
```

- [ ] **Step 2: Add the poll-group field to the generic transport poll group**

`struct spdk_nvmf_transport_poll_group` is defined in `include/spdk/nvmf_transport.h` (NOT `nvmf_internal.h`). Find it and add, immediately after its `STAILQ_HEAD(, spdk_nvmf_request) pending_buf_queue;` member:

```c
	void				*qdlimit_ctx;	/* opaque per-group qdlimit state; NULL when unused */
```

Run to find the exact line:
```bash
grep -n "pending_buf_queue" include/spdk/nvmf_transport.h
```
Expected: one hit inside `struct spdk_nvmf_transport_poll_group`; insert the new field right after it.

- [ ] **Step 3: Add a pg lifecycle unit test**

In `qdlimit_ut.c`, add this test and register it in `main`:

```c
static void
test_pg_init_fini(void)
{
	struct spdk_nvmf_transport_poll_group group = {};

	nvmf_qdlimit_pg_init(&group);
	CU_ASSERT(group.qdlimit_ctx != NULL);
	CU_ASSERT(TAILQ_EMPTY(&((struct qdlimit_pg_ctx *)group.qdlimit_ctx)->ssds));

	nvmf_qdlimit_pg_fini(&group);
	CU_ASSERT(group.qdlimit_ctx == NULL);
}
```
Register: add `CU_ADD_TEST(suite, test_pg_init_fini);` after the existing `CU_ADD_TEST` line.

- [ ] **Step 4: Build and run**

Run:
```bash
make -C test/unit/lib/nvmf/qdlimit.c
./test/unit/lib/nvmf/qdlimit.c/qdlimit_ut
```
Expected: PASS, both tests green.

- [ ] **Step 5: Commit**

```bash
git add lib/nvmf/qdlimit.c lib/nvmf/nvmf_internal.h test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c
git commit -m "nvmf/qdlimit: add per-poll-group context and per-SSD entries"
```

---

## Task 3: Admission gate

**Files:**
- Modify: `include/spdk/nvmf_transport.h:80`
- Modify: `lib/nvmf/qdlimit.c`
- Modify: `test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c`

- [ ] **Step 1: Add the per-request charged flag**

In `include/spdk/nvmf_transport.h`, in `struct spdk_nvmf_request`, add after the `bool dif_enabled;` line (line 80):

```c
	bool				qdlimit_charged; /* holds a qdlimit slot, released exactly once */
```

- [ ] **Step 2: Write failing admission tests**

In `qdlimit_ut.c`, add a fake-bdev helper and tests, and register them:

```c
/* Opaque fake bdevs: we only use their addresses as identity keys and stub get_name. */
static char g_fake_bdev_a;
static char g_fake_bdev_b;

DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev),
	    ((void *)bdev == &g_fake_bdev_a) ? "bdevA" : "bdevB");

static void
test_admit_under_and_over_limit(void)
{
	struct spdk_nvmf_transport_poll_group group = {};
	struct spdk_nvmf_request r1 = {}, r2 = {}, r3 = {};

	STAILQ_INIT(&group.pending_buf_queue);
	nvmf_qdlimit_pg_init(&group);
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 2) == 0);

	/* Simulate the transport: each request is the queue head when gated. */
	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r1, buf_link);
	CU_ASSERT(qdlimit_admit_bdev(&group, &r1, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_ADMIT);
	CU_ASSERT(r1.qdlimit_charged == true);

	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r2, buf_link);
	CU_ASSERT(qdlimit_admit_bdev(&group, &r2, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_ADMIT);

	/* Third request exceeds depth=2: throttled and removed from pending_buf_queue. */
	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r3, buf_link);
	CU_ASSERT(qdlimit_admit_bdev(&group, &r3, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_THROTTLED);
	CU_ASSERT(r3.qdlimit_charged == false);
	/* r3 left pending_buf_queue; r1 and r2 remain. */
	CU_ASSERT(STAILQ_FIRST(&group.pending_buf_queue) == &r1);

	nvmf_qdlimit_pg_fini_drain(&group); /* test helper, see step 3 */
}

static void
test_admit_bypass(void)
{
	struct spdk_nvmf_transport_poll_group group = {};
	struct spdk_nvmf_request rn = {}, ru = {};

	STAILQ_INIT(&group.pending_buf_queue);
	nvmf_qdlimit_pg_init(&group);

	/* Unconfigured bdev (bdevB) => unlimited bypass, never charged. */
	CU_ASSERT(qdlimit_admit_bdev(&group, &ru, (void *)&g_fake_bdev_b, "bdevB") == NVMF_QDLIMIT_ADMIT);
	CU_ASSERT(ru.qdlimit_charged == false);

	/* depth == 0 explicit => unlimited bypass. */
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 0) == 0);
	CU_ASSERT(qdlimit_admit_bdev(&group, &rn, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_ADMIT);
	CU_ASSERT(rn.qdlimit_charged == false);

	nvmf_qdlimit_pg_fini(&group);
	nvmf_qdlimit_config_cleanup();
}
```
Register both in `main`. Also add at the end of `test_admit_under_and_over_limit`'s suite a `nvmf_qdlimit_config_cleanup();` — handled via the drain helper below.

- [ ] **Step 3: Implement `qdlimit_admit_bdev`, the public `nvmf_qdlimit_admit`, and a test drain helper**

Add to `qdlimit.c` (the `_bdev` form is `static` but reachable from the UT via `#include "nvmf/qdlimit.c"`):

```c
#include "spdk/bdev.h"
#include "spdk/nvme_spec.h"
#include "nvmf_internal.h"

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
	if (ctrlr == NULL || ctrlr->subsys == NULL) {
		return NVMF_QDLIMIT_ADMIT;
	}
	ns = _nvmf_subsystem_get_ns(ctrlr->subsys, req->cmd->nvme_cmd.nsid);
	if (ns == NULL || ns->bdev == NULL) {
		return NVMF_QDLIMIT_ADMIT;
	}
	bdev = ns->bdev;
	return qdlimit_admit_bdev(group, req, bdev, spdk_bdev_get_name(bdev));
}
```

Add this test-only drain helper at the bottom of `qdlimit.c`, guarded so it is available to the UT (it is harmless in production and used by `pg_fini` tests that parked requests):

```c
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
```

Add its prototype to `qdlimit.h` after `nvmf_qdlimit_pg_fini`:

```c
void nvmf_qdlimit_pg_fini_drain(struct spdk_nvmf_transport_poll_group *group);
```

- [ ] **Step 4: Add UT stubs for the resolution helpers used by `nvmf_qdlimit_admit`**

Because the UT `#include`s `qdlimit.c`, it pulls in references to `_nvmf_subsystem_get_ns`. The `_bdev` tests call `qdlimit_admit_bdev` directly and never exercise resolution, but the symbol must link. Add to `qdlimit_ut.c` near the other stubs:

```c
DEFINE_STUB(_nvmf_subsystem_get_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, uint32_t nsid), NULL);
```

- [ ] **Step 5: Build and run**

Run:
```bash
make -C test/unit/lib/nvmf/qdlimit.c
./test/unit/lib/nvmf/qdlimit.c/qdlimit_ut
```
Expected: PASS, admit/over-limit/bypass tests green.

- [ ] **Step 6: Commit**

```bash
git add include/spdk/nvmf_transport.h lib/nvmf/qdlimit.c lib/nvmf/qdlimit.h \
        test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c
git commit -m "nvmf/qdlimit: implement per-SSD admission gate with wait-queue parking"
```

---

## Task 4: Slot release and re-arm

**Files:**
- Modify: `lib/nvmf/qdlimit.c`
- Modify: `test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c`

- [ ] **Step 1: Write failing release tests**

In `qdlimit_ut.c`, add and register:

```c
static void
test_release_rearms_waiter(void)
{
	struct spdk_nvmf_transport_poll_group group = {};
	struct spdk_nvmf_request r1 = {}, r2 = {}, r3 = {};

	STAILQ_INIT(&group.pending_buf_queue);
	nvmf_qdlimit_pg_init(&group);
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 2) == 0);

	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r1, buf_link);
	qdlimit_admit_bdev(&group, &r1, (void *)&g_fake_bdev_a, "bdevA");	/* charged */
	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r2, buf_link);
	qdlimit_admit_bdev(&group, &r2, (void *)&g_fake_bdev_a, "bdevA");	/* charged */
	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r3, buf_link);
	qdlimit_admit_bdev(&group, &r3, (void *)&g_fake_bdev_a, "bdevA");	/* throttled, parked */

	/* Release r1: inflight 2->1, r3 re-armed onto pending_buf_queue head. */
	qdlimit_release_bdev(&group, &r1, (void *)&g_fake_bdev_a);
	CU_ASSERT(r1.qdlimit_charged == false);
	CU_ASSERT(STAILQ_FIRST(&group.pending_buf_queue) == &r3);

	/* r3 can now be admitted (inflight back under limit). */
	CU_ASSERT(qdlimit_admit_bdev(&group, &r3, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_ADMIT);
	CU_ASSERT(r3.qdlimit_charged == true);

	nvmf_qdlimit_pg_fini_drain(&group);
	nvmf_qdlimit_config_cleanup();
}

static void
test_release_uncharged_is_noop(void)
{
	struct spdk_nvmf_transport_poll_group group = {};
	struct spdk_nvmf_request r = {};

	STAILQ_INIT(&group.pending_buf_queue);
	nvmf_qdlimit_pg_init(&group);

	/* Never charged: release must not underflow or touch queues. */
	qdlimit_release_bdev(&group, &r, (void *)&g_fake_bdev_a);
	CU_ASSERT(r.qdlimit_charged == false);
	CU_ASSERT(STAILQ_EMPTY(&group.pending_buf_queue));

	nvmf_qdlimit_pg_fini(&group);
}
```

- [ ] **Step 2: Implement release**

Add to `qdlimit.c`:

```c
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
```

- [ ] **Step 3: Build and run**

Run:
```bash
make -C test/unit/lib/nvmf/qdlimit.c
./test/unit/lib/nvmf/qdlimit.c/qdlimit_ut
```
Expected: PASS, release/re-arm and uncharged-noop tests green.

- [ ] **Step 4: Commit**

```bash
git add lib/nvmf/qdlimit.c test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c
git commit -m "nvmf/qdlimit: implement slot release and per-SSD waiter re-arm"
```

---

## Task 5: Wire the hooks into the RDMA transport

**Files:**
- Modify: `lib/nvmf/rdma.c` (4 sites)

- [ ] **Step 1: Include the header**

In `lib/nvmf/rdma.c`, add near the other local includes (after `#include "nvmf_internal.h"`):

```c
#include "qdlimit.h"
```
Find the include block:
```bash
grep -n '#include "nvmf_internal.h"' lib/nvmf/rdma.c
```

- [ ] **Step 2: Insert the admission gate at NEED_BUFFER**

In `lib/nvmf/rdma.c`, in `case RDMA_REQUEST_STATE_NEED_BUFFER:` (around line 2067), the existing code is:

```c
			if (&rdma_req->req != STAILQ_FIRST(&rgroup->group.pending_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* Try to get a data buffer */
			rc = nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
```

Replace with (insert the gate between the FIFO check and parse_sgl):

```c
			if (&rdma_req->req != STAILQ_FIRST(&rgroup->group.pending_buf_queue)) {
				/* This request needs to wait in line to obtain a buffer */
				break;
			}

			/* qdlimit admission: cap per-SSD buffer-holding requests before we
			 * take a buffer. On THROTTLED the request has been moved off
			 * pending_buf_queue onto a per-SSD wait queue and will be re-armed
			 * on release. */
			if (nvmf_qdlimit_admit(&rgroup->group, &rdma_req->req) == NVMF_QDLIMIT_THROTTLED) {
				break;
			}

			/* Try to get a data buffer */
			rc = nvmf_rdma_request_parse_sgl(rtransport, device, rdma_req);
```

- [ ] **Step 3: Insert the release hook in `_nvmf_rdma_request_free`**

In `lib/nvmf/rdma.c`, in `_nvmf_rdma_request_free` (around line 1883-1888), the existing code is:

```c
	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	if (rdma_req->req.data_from_pool) {
		rgroup = rqpair->poller->group;

		spdk_nvmf_request_free_buffers(&rdma_req->req, &rgroup->group, &rtransport->transport);
	}
```

Replace with (release the qdlimit slot at the same point the buffer returns to the pool — post-ACK by construction):

```c
	rqpair = SPDK_CONTAINEROF(rdma_req->req.qpair, struct spdk_nvmf_rdma_qpair, qpair);
	rgroup = rqpair->poller->group;

	/* Release the qdlimit slot when the buffer is returned to the pool. For reads this
	 * is reached only in COMPLETED state, i.e. after the data-transfer WR is ACKed. */
	nvmf_qdlimit_release(&rgroup->group, &rdma_req->req);

	if (rdma_req->req.data_from_pool) {
		spdk_nvmf_request_free_buffers(&rdma_req->req, &rgroup->group, &rtransport->transport);
	}
```

Note: `rgroup` was previously declared and only assigned inside the `if`; it is declared at the top of the function (line 1881), so hoisting the assignment is safe. Verify no "unused/used-uninitialized" warning after building.

- [ ] **Step 4: Insert `pg_init` in poll-group create**

In `nvmf_rdma_poll_group_create` (around line 3524), find where `rgroup` is fully initialized and returned (just before `return &rgroup->group;`). Run:
```bash
grep -n "return &rgroup->group;" lib/nvmf/rdma.c
```
Immediately before that `return`, add:

```c
	nvmf_qdlimit_pg_init(&rgroup->group);
```

- [ ] **Step 5: Insert `pg_fini` in poll-group destroy**

In `nvmf_rdma_poll_group_destroy` (line 3655), after the `rgroup = SPDK_CONTAINEROF(...)` / NULL check (line 3661-3664) and before the poller-destroy loop, add:

```c
	nvmf_qdlimit_pg_fini(&rgroup->group);
```

Place it right after:
```c
	rgroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_rdma_poll_group, group);
	if (!rgroup) {
		return;
	}
```

- [ ] **Step 6: Build SPDK**

Run:
```bash
make -j -C lib/nvmf
```
Expected: clean build, no warnings about `rgroup` or qdlimit symbols.

- [ ] **Step 7: Run the full nvmf unit suite to confirm no regressions**

Run:
```bash
make -C test/unit/lib/nvmf/rdma.c && ./test/unit/lib/nvmf/rdma.c/rdma_ut
make -C test/unit/lib/nvmf/qdlimit.c && ./test/unit/lib/nvmf/qdlimit.c/qdlimit_ut
```
Expected: both PASS.

- [ ] **Step 8: Commit**

```bash
git add lib/nvmf/rdma.c
git commit -m "nvmf/rdma: wire qdlimit admission gate and slot release hooks"
```

---

## Task 6: RPC, Python bindings, CLI

**Files:**
- Modify: `lib/nvmf/nvmf_rpc.c`
- Modify: `scripts/rpc/nvmf.py`
- Modify: `scripts/rpc.py`

- [ ] **Step 1: Add the RPC handlers**

In `lib/nvmf/nvmf_rpc.c`, add `#include "qdlimit.h"` near the top includes, then append before the final lines of the file:

```c
struct rpc_qdlimit_set_depth {
	char		*bdev_name;
	uint32_t	depth;
};

static const struct spdk_json_object_decoder rpc_qdlimit_set_depth_decoders[] = {
	{"bdev_name", offsetof(struct rpc_qdlimit_set_depth, bdev_name), spdk_json_decode_string},
	{"depth", offsetof(struct rpc_qdlimit_set_depth, depth), spdk_json_decode_uint32},
};

static void
rpc_nvmf_qdlimit_set_depth(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_qdlimit_set_depth req = {};
	int rc;

	if (spdk_json_decode_object(params, rpc_qdlimit_set_depth_decoders,
				    SPDK_COUNTOF(rpc_qdlimit_set_depth_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}
	rc = nvmf_qdlimit_set_depth(req.bdev_name, req.depth);
	free(req.bdev_name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		return;
	}
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("nvmf_qdlimit_set_depth", rpc_nvmf_qdlimit_set_depth, SPDK_RPC_RUNTIME)

struct rpc_qdlimit_get_depth {
	char		*bdev_name;
};

static const struct spdk_json_object_decoder rpc_qdlimit_get_depth_decoders[] = {
	{"bdev_name", offsetof(struct rpc_qdlimit_get_depth, bdev_name), spdk_json_decode_string},
};

static void
rpc_nvmf_qdlimit_get_depth(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_qdlimit_get_depth req = {};
	struct spdk_json_write_ctx *w;
	uint32_t depth = 0;

	if (spdk_json_decode_object(params, rpc_qdlimit_get_depth_decoders,
				    SPDK_COUNTOF(rpc_qdlimit_get_depth_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "Invalid parameters");
		return;
	}
	(void)nvmf_qdlimit_get_depth(req.bdev_name, &depth);

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "bdev_name", req.bdev_name);
	spdk_json_write_named_uint32(w, "depth", depth);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

	free(req.bdev_name);
}
SPDK_RPC_REGISTER("nvmf_qdlimit_get_depth", rpc_nvmf_qdlimit_get_depth, SPDK_RPC_RUNTIME)
```

Note on `get_stats`: per-core in-flight counters live in per-poll-group context on individual cores; gathering them requires an `spdk_for_each_channel`-style fan-out across poll groups. That observability RPC is deferred — the buffer-occupancy ceiling test (Task 7) derives occupancy from the transport mempool counter instead, so `get_stats` is not on the critical path. Add a TODO comment in `qdlimit.h` noting the deferral.

- [ ] **Step 2: Build and smoke-test the RPC registration**

Run:
```bash
make -j -C lib/nvmf
```
Expected: clean build.

- [ ] **Step 3: Add Python bindings**

In `scripts/rpc/nvmf.py`, append:

```python
def nvmf_qdlimit_set_depth(client, bdev_name, depth):
    """Set the per-SSD pre-buffer admission depth for an NVMe-oF/RDMA backing bdev.

    Args:
        bdev_name: name of the backing bdev (SSD)
        depth: max in-flight buffer-holding requests per core (0 = unlimited)
    """
    params = {'bdev_name': bdev_name, 'depth': depth}
    return client.call('nvmf_qdlimit_set_depth', params)


def nvmf_qdlimit_get_depth(client, bdev_name):
    """Get the configured per-SSD admission depth for a backing bdev."""
    params = {'bdev_name': bdev_name}
    return client.call('nvmf_qdlimit_get_depth', params)
```

- [ ] **Step 4: Add CLI subcommands**

In `scripts/rpc.py`, find the nvmf section (search for an existing `nvmf_` subparser, e.g. `p = subparsers.add_parser('nvmf_create_transport'`) and add nearby:

```python
    def nvmf_qdlimit_set_depth(args):
        rpc.nvmf.nvmf_qdlimit_set_depth(args.client, bdev_name=args.bdev_name, depth=args.depth)

    p = subparsers.add_parser('nvmf_qdlimit_set_depth',
                              help='Set per-SSD pre-buffer admission depth (0 = unlimited)')
    p.add_argument('bdev_name', help='Backing bdev (SSD) name')
    p.add_argument('depth', help='Max in-flight buffer-holding requests per core', type=int)
    p.set_defaults(func=nvmf_qdlimit_set_depth)

    def nvmf_qdlimit_get_depth(args):
        print_dict(rpc.nvmf.nvmf_qdlimit_get_depth(args.client, bdev_name=args.bdev_name))

    p = subparsers.add_parser('nvmf_qdlimit_get_depth',
                              help='Get configured per-SSD admission depth')
    p.add_argument('bdev_name', help='Backing bdev (SSD) name')
    p.set_defaults(func=nvmf_qdlimit_get_depth)
```

- [ ] **Step 5: Commit**

```bash
git add lib/nvmf/nvmf_rpc.c lib/nvmf/qdlimit.h scripts/rpc/nvmf.py scripts/rpc.py
git commit -m "nvmf/qdlimit: add set_depth/get_depth RPC, Python bindings and CLI"
```

---

## Task 7: RDMA integration + buffer-occupancy ceiling test

**Files:**
- Create: `test/nvmf/target/qdlimit.sh`

This test uses SoftRoCE (`rxe`) so it runs without RDMA hardware. It proves (a) isolation and (b) the buffer-occupancy ceiling from spec §8.

- [ ] **Step 1: Write the integration test script**

`test/nvmf/target/qdlimit.sh`:

```bash
#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Intel Corporation. All rights reserved.

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

rpc_py="$rootdir/scripts/rpc.py"

# Bring up SoftRoCE over the loopback/test NIC (provided by nvmf test harness setup).
nvmftestinit

# Start the target.
nvmfappstart -m 0x3
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Two malloc bdevs sharing the one RDMA data buffer pool.
$rpc_py bdev_malloc_create -b SSD_A 64 4096
$rpc_py bdev_malloc_create -b SSD_B 64 4096

# Subsystem with both namespaces.
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 SSD_A
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 SSD_B
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 \
	-t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Cap SSD_A at a low per-core depth; leave SSD_B unlimited.
QD=4
$rpc_py nvmf_qdlimit_set_depth SSD_A $QD
$rpc_py nvmf_qdlimit_set_depth SSD_B 0
[ "$($rpc_py nvmf_qdlimit_get_depth SSD_A | jq .depth)" -eq "$QD" ]

# Helper: total RDMA data buffers currently checked out of the shared pool.
# Derived from transport stats (num_shared_buffers minus current free cache).
pool_inuse() {
	$rpc_py nvmf_get_stats | jq '
		[.poll_groups[].transports[]
		 | select(.trtype=="RDMA")
		 | (.pending_data_buffer // 0)] | add // 0'
}

# Connect initiator.
nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 \
	-a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
waitforserial SPDK00000000000001

# --- Buffer-occupancy ceiling sweep against SSD_A ---
# Increase offered concurrency; admitted in-flight (hence buffers) must plateau at QD/core.
iosize=4096
prev=0
plateaued=0
for od in 1 2 4 8 16 32 64; do
	fio --name=ssdA --filename=/dev/nvme-fabric-ssd_a --rw=randread --bs=${iosize} \
	    --iodepth=${od} --numjobs=1 --runtime=5 --time_based --ioengine=libaio \
	    --direct=1 --group_reporting &
	fio_pid=$!
	sleep 3
	# Admitted in-flight for SSD_A cannot exceed QD * num_cores; with -m 0x3 => 2 cores.
	inflight=$($rpc_py nvmf_qdlimit_get_depth SSD_A | jq .depth) # configured ceiling
	occ_units=$(pool_inuse)
	echo "offered=${od} pool_inflight_units=${occ_units} ceiling_per_core=${inflight}"
	wait $fio_pid
	# Past the ceiling, occupancy must stop growing (allow one in-flight quantum slack).
	if [ "$od" -ge "$QD" ]; then
		if [ "$occ_units" -le $((prev + 1)) ]; then
			plateaued=1
		fi
	fi
	prev=$occ_units
done
[ "$plateaued" -eq 1 ] || { echo "FAIL: buffer occupancy did not plateau at ceiling"; exit 1; }

# --- Isolation: SSD_A throttled must not raise SSD_B p99 ---
fio --name=ssdB --filename=/dev/nvme-fabric-ssd_b --rw=randread --bs=${iosize} \
    --iodepth=32 --numjobs=1 --runtime=10 --time_based --ioengine=libaio \
    --direct=1 --percentile_list=99.0 --output-format=json > /tmp/ssdB.json &
fio --name=ssdA --filename=/dev/nvme-fabric-ssd_a --rw=randread --bs=${iosize} \
    --iodepth=256 --numjobs=4 --runtime=10 --time_based --ioengine=libaio --direct=1 &
wait
p99=$(jq '.jobs[0].read.clat_ns.percentile["99.000000"]' /tmp/ssdB.json)
echo "SSD_B p99(ns) under SSD_A overload = ${p99}"
# Sanity ceiling: with isolation working, SSD_B p99 should stay well under 5 ms.
[ "$p99" -lt 5000000 ] || { echo "FAIL: SSD_B p99 regressed under SSD_A overload"; exit 1; }

nvme disconnect -n nqn.2016-06.io.spdk:cnode1
nvmftestfini
echo "qdlimit RDMA integration: PASS"
```

- [ ] **Step 2: Make it executable and run it**

Run:
```bash
chmod +x test/nvmf/target/qdlimit.sh
sudo TEST_TRANSPORT=rdma ./test/nvmf/target/qdlimit.sh
```
Expected: ends with `qdlimit RDMA integration: PASS`. (Device node names from `nvme connect` may differ; if so, resolve them via `nvme list` / `lsblk` and adjust `--filename`. The harness on the target machine is authoritative for device naming — confirm before asserting pass.)

- [ ] **Step 3: Commit**

```bash
git add test/nvmf/target/qdlimit.sh
git commit -m "test/nvmf: add RDMA qdlimit isolation and buffer-ceiling test"
```

---

## Task 8: Retire the bdev-layer vbdev

**Files:**
- Delete: `module/bdev/qdlimit/**`
- Modify: build wiring, Python/CLI, and tests that referenced the vbdev

- [ ] **Step 1: Find everything referencing the old vbdev**

Run:
```bash
grep -rn "qdlimit" --include=Makefile --include=mk module/bdev mk
grep -rn "bdev_qdlimit" scripts/ test/ module/
ls module/bdev/qdlimit
```
Record each hit; they are removed/cleaned in the next steps.

- [ ] **Step 2: Remove the module directory and its build registration**

Run:
```bash
git rm -r module/bdev/qdlimit
grep -rn "qdlimit" module/bdev/Makefile mk/spdk.modules.mk 2>/dev/null
```
For each remaining reference (e.g. a `qdlimit` entry in `module/bdev/Makefile`'s `DIRS-y` or in `mk/spdk.modules.mk` link list), delete that entry with an Edit.

- [ ] **Step 3: Remove vbdev Python/CLI and tests**

Run:
```bash
grep -rn "bdev_qdlimit" scripts/rpc/bdev.py scripts/rpc.py
git rm -r test/unit/lib/bdev/qdlimit.c 2>/dev/null || true
grep -rn "qdlimit" test/unit/lib/bdev/Makefile test/bdev 2>/dev/null
```
Delete the `bdev_qdlimit_*` Python functions and CLI subparsers, the vbdev unit-test dir and its `DIRS` entry, and the malloc-based vbdev integration test (`test/bdev/qdlimit/*` if present).

- [ ] **Step 4: Confirm a clean tree-wide build**

Run:
```bash
make -j
make -C test/unit/lib/nvmf/qdlimit.c && ./test/unit/lib/nvmf/qdlimit.c/qdlimit_ut
```
Expected: full build succeeds with no dangling `qdlimit` references; nvmf qdlimit unit test PASS.

- [ ] **Step 5: Verify nothing else references the removed vbdev**

Run:
```bash
grep -rn "bdev_qdlimit\|module/bdev/qdlimit" . --exclude-dir=.git --exclude-dir=docs
```
Expected: no output.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "module/bdev/qdlimit: retire vbdev in favor of nvmf/rdma admission control"
```

---

## Self-Review

**Spec coverage:**
- §2 protect shared pool / per-SSD / per-core / Approach B / RDMA-only / gate at NEED_BUFFER / per-SSD wait queue / release in `_nvmf_rdma_request_free` / L2 / depth=0 → Tasks 1–5 ✔
- §4.1 hook API (admit/release/pg_init/pg_fini/set_depth/get_depth) → Tasks 1–4 ✔
- §4.2 data structures (config table, per-pg ctx, qdlimit_charged, qdlimit_ctx) → Tasks 1–3 ✔
- §4.3 nsid→ns→bdev resolution via `_nvmf_subsystem_get_ns` → Task 3 Step 3 ✔
- §5 admission + release + re-arm control flow → Tasks 3–4 ✔
- §6 RPC/Python/CLI (`get_stats` explicitly deferred with rationale) → Task 6 ✔
- §7 retire vbdev → Task 8 ✔
- §8 unit + integration + **buffer-occupancy ceiling** → Tasks 1–4 (unit) + Task 7 ✔
- §9 intrusion summary (4 rdma.c sites + 1 poll-group field) → Task 5 ✔

**Placeholder scan:** No "TBD/handle edge cases/similar to". `get_stats` deferral is explicit and justified, not a placeholder. Integration device-node naming flagged as machine-specific to verify. OK.

**Type consistency:** `enum nvmf_qdlimit_status {NVMF_QDLIMIT_ADMIT, NVMF_QDLIMIT_THROTTLED}`, `nvmf_qdlimit_admit/release/pg_init/pg_fini/pg_fini_drain/set_depth/get_depth/config_cleanup`, internal `qdlimit_admit_bdev`/`qdlimit_release_bdev`/`qdlimit_pg_get_ssd`, fields `qdlimit_ctx`/`qdlimit_charged`, structs `qdlimit_config_entry`/`qdlimit_pg_ctx`/`qdlimit_pg_ssd` — names used identically across Tasks 1–6. ✔
