# 每盘并发控制 vbdev（qdlimit）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增一个独立的 passthrough 虚拟 bdev 模块 `qdlimit`，叠加在底层 SSD bdev 之上，对每个 (SSD, reactor 核) 的在途 IO 深度做静态上限控制，从而压制单盘过载、降低尾延迟。

**Architecture:** 完全克隆 SPDK `module/bdev/passthru` 的 vbdev 骨架（注册/examine/create/delete/io_channel/hotremove），在 IO 提交路径插入 per-channel（per-core、无锁）的"在途计数 + FIFO 等待队列"。准入判定与计数被抽成 header 中的 `static inline` 纯函数以便单测。对 nvmf/bdev/bdev_nvme 主干零改动。

**Tech Stack:** C, SPDK bdev module API（`spdk/bdev_module.h`），SPDK CUnit 单测框架（`mk/spdk.unittest.mk`），SPDK JSON-RPC，Python RPC 绑定，bdevperf 集成测试。

参考规格：`docs/superpowers/specs/2026-06-16-qdlimit-vbdev-design.md`

---

## 文件结构

**新建：**
- `module/bdev/qdlimit/vbdev_qdlimit.h` — 对外 create/delete 接口 + 节点/通道结构 + `static inline` 准入/计数纯函数
- `module/bdev/qdlimit/vbdev_qdlimit.c` — 模块核心：注册、io_channel、IO 流程（计数+排队+放行）
- `module/bdev/qdlimit/vbdev_qdlimit_rpc.c` — RPC：create / delete / set_depth
- `module/bdev/qdlimit/Makefile` — 模块构建
- `test/unit/lib/bdev/qdlimit.c/qdlimit_ut.c` — 准入/计数纯函数单测
- `test/unit/lib/bdev/qdlimit.c/Makefile` — 单测构建
- `test/bdev/qdlimit.sh` — 集成测试脚本（bdevperf over malloc）

**修改：**
- `module/bdev/Makefile` — `DIRS-y` 加入 `qdlimit`
- `mk/spdk.modules.mk` — `BLOCKDEV_MODULES_LIST` 加入 `bdev_qdlimit`
- `mk/spdk.lib_deps.mk` — 加入 `DEPDIRS-bdev_qdlimit`
- `test/unit/lib/bdev/Makefile` — `DIRS-y` 加入 `qdlimit.c`
- `scripts/rpc.py` — 加入 create/delete/set_depth CLI 子命令
- `python/spdk/rpc/bdev.py` — 加入 create/delete/set_depth client 函数

**命名约定（全计划一致）：**
- 模块结构体注册：`qdlimit_if`，`.name = "qdlimit"`
- 节点：`struct vbdev_qdlimit`，全局表 `g_qd_nodes`
- 名字关联表：`struct bdev_names` / `g_bdev_names`（含 `queue_depth`）
- 通道：`struct qdlimit_io_channel`
- per-IO：`struct qdlimit_bdev_io`
- 纯函数：`qdlimit_io_is_limited()` / `qdlimit_try_acquire()` / `qdlimit_release()`
- product_name 字符串：`"qdlimit"`

---

## Task 1: 模块骨架（纯透传，先编译通过）

先把模块作为"纯透传 + 一个未生效的 queue_depth 字段"立起来并接入构建，行为等价 passthru。后续 Task 再让限流生效。

**Files:**
- Create: `module/bdev/qdlimit/vbdev_qdlimit.h`
- Create: `module/bdev/qdlimit/vbdev_qdlimit.c`
- Create: `module/bdev/qdlimit/Makefile`
- Modify: `module/bdev/Makefile`
- Modify: `mk/spdk.modules.mk`
- Modify: `mk/spdk.lib_deps.mk`

- [ ] **Step 1: 写 header**

Create `module/bdev/qdlimit/vbdev_qdlimit.h`:

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 SPDK contributors.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_QDLIMIT_H
#define SPDK_VBDEV_QDLIMIT_H

#include "spdk/stdinc.h"

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

/* Per-core channel state. Touched only by its owning thread -> lock-free. */
struct qdlimit_io_channel {
	struct spdk_io_channel	*base_ch;	/* IO channel of base device */
	uint32_t		outstanding;	/* in-flight IO on this core */
	uint32_t		max_depth;	/* per-core cap, 0 = unlimited */
	STAILQ_HEAD(, qdlimit_bdev_io) queued_io; /* admitted-pending IO, FIFO */
};

/*
 * Return true if this IO type is subject to the depth limit. Management /
 * buffer-management ops bypass the limit and are never queued.
 */
static inline bool
qdlimit_io_is_limited(enum spdk_bdev_io_type type)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_COPY:
		return true;
	default:
		return false;
	}
}

/*
 * Try to reserve one in-flight slot. Returns true and increments the counter
 * when a slot is available (or when limiting is disabled), false otherwise.
 */
static inline bool
qdlimit_try_acquire(struct qdlimit_io_channel *qd_ch)
{
	if (qd_ch->max_depth != 0 && qd_ch->outstanding >= qd_ch->max_depth) {
		return false;
	}
	qd_ch->outstanding++;
	return true;
}

/* Release one previously-acquired in-flight slot. */
static inline void
qdlimit_release(struct qdlimit_io_channel *qd_ch)
{
	assert(qd_ch->outstanding > 0);
	qd_ch->outstanding--;
}

/**
 * Create a new qdlimit vbdev on top of an existing bdev.
 *
 * \param bdev_name Base bdev to attach to.
 * \param vbdev_name Name of the new qdlimit bdev.
 * \param queue_depth Per-core max in-flight IO (0 = unlimited).
 * \return 0 on success, negative errno on failure.
 */
int bdev_qdlimit_create_disk(const char *bdev_name, const char *vbdev_name,
			     uint32_t queue_depth);

/**
 * Delete a qdlimit vbdev.
 *
 * \param vbdev_name Name of the qdlimit bdev.
 * \param cb_fn Completion callback.
 * \param cb_arg Argument for cb_fn.
 */
void bdev_qdlimit_delete_disk(const char *vbdev_name, spdk_bdev_unregister_cb cb_fn,
			      void *cb_arg);

/**
 * Update the per-core queue depth of an existing qdlimit vbdev.
 *
 * \param vbdev_name Name of the qdlimit bdev.
 * \param queue_depth New per-core max in-flight IO (0 = unlimited).
 * \return 0 on success, negative errno on failure.
 */
int bdev_qdlimit_set_depth(const char *vbdev_name, uint32_t queue_depth);

#endif /* SPDK_VBDEV_QDLIMIT_H */
```

> 注意：`struct qdlimit_bdev_io` 在 `.c` 中定义并在 Task 3 用到，header 中 `STAILQ_HEAD(, qdlimit_bdev_io)` 仅引用其标签（C 允许不完整类型的链表头声明）。`set_depth` 接口此处声明、Task 5 实现，Task 1 的 `.c` 先给一个返回 0 的占位实现以便链接。

- [ ] **Step 2: 写 .c（纯透传骨架，limiting 暂不生效）**

Create `module/bdev/qdlimit/vbdev_qdlimit.c`. 这是 passthru.c 的等价克隆，差异：①节点/关联表带 `queue_depth`；②通道结构是 `qdlimit_io_channel`，create 回调初始化 `outstanding=0 / max_depth=queue_depth / STAILQ_INIT`；③`get_ctx_size` 返回 `sizeof(struct qdlimit_bdev_io)`；④`dump_info_json` / `config_json` 输出 `queue_depth`。submit_request 本任务先**纯透传**（不计数、不排队），Task 3 再替换。

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 SPDK contributors.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "vbdev_qdlimit.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"

static int vbdev_qdlimit_init(void);
static int vbdev_qdlimit_get_ctx_size(void);
static void vbdev_qdlimit_examine(struct spdk_bdev *bdev);
static void vbdev_qdlimit_finish(void);
static int vbdev_qdlimit_config_json(struct spdk_json_write_ctx *w);

static struct spdk_bdev_module qdlimit_if = {
	.name = "qdlimit",
	.module_init = vbdev_qdlimit_init,
	.get_ctx_size = vbdev_qdlimit_get_ctx_size,
	.examine_config = vbdev_qdlimit_examine,
	.module_fini = vbdev_qdlimit_finish,
	.config_json = vbdev_qdlimit_config_json,
};

SPDK_BDEV_MODULE_REGISTER(qdlimit, &qdlimit_if)

/* (vbdev_name, base_bdev_name, queue_depth) association, used by examine(). */
struct bdev_names {
	char			*vbdev_name;
	char			*bdev_name;
	uint32_t		queue_depth;
	TAILQ_ENTRY(bdev_names)	link;
};
static TAILQ_HEAD(, bdev_names) g_bdev_names = TAILQ_HEAD_INITIALIZER(g_bdev_names);

struct vbdev_qdlimit {
	struct spdk_bdev		*base_bdev;
	struct spdk_bdev_desc		*base_desc;
	struct spdk_bdev		qd_bdev;	/* the qdlimit virtual bdev */
	uint32_t			queue_depth;	/* per-core cap, 0 = unlimited */
	struct spdk_thread		*thread;
	TAILQ_ENTRY(vbdev_qdlimit)	link;
};
static TAILQ_HEAD(, vbdev_qdlimit) g_qd_nodes = TAILQ_HEAD_INITIALIZER(g_qd_nodes);

/* Per-IO context carried in bdev_io->driver_ctx (no extra allocation). */
struct qdlimit_bdev_io {
	struct spdk_io_channel		*ch;
	bool				counted;	/* holds an in-flight slot */
	STAILQ_ENTRY(qdlimit_bdev_io)	link;		/* link in queued_io */
	struct spdk_bdev_io_wait_entry	bdev_io_wait;	/* for base ENOMEM retry */
};

static void vbdev_qdlimit_submit_request(struct spdk_io_channel *ch,
		struct spdk_bdev_io *bdev_io);

static void
_device_unregister_cb(void *io_device)
{
	struct vbdev_qdlimit *qd_node = io_device;

	free(qd_node->qd_bdev.name);
	free(qd_node);
}

static void
_vbdev_qdlimit_destruct(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

static int
vbdev_qdlimit_destruct(void *ctx)
{
	struct vbdev_qdlimit *qd_node = (struct vbdev_qdlimit *)ctx;

	TAILQ_REMOVE(&g_qd_nodes, qd_node, link);
	spdk_bdev_module_release_bdev(qd_node->base_bdev);

	if (qd_node->thread && qd_node->thread != spdk_get_thread()) {
		spdk_thread_send_msg(qd_node->thread, _vbdev_qdlimit_destruct, qd_node->base_desc);
	} else {
		spdk_bdev_close(qd_node->base_desc);
	}

	spdk_io_device_unregister(qd_node, _device_unregister_cb);

	return 0;
}

/* Forward an IO to the base bdev. Does NOT touch the in-flight counter. */
static void
_qdlimit_dispatch(struct qdlimit_io_channel *qd_ch, struct spdk_bdev_io *bdev_io);

static void
_qdlimit_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}

static void
pt_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_qdlimit *qd_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_qdlimit,
				 qd_bdev);
	struct qdlimit_io_channel *qd_ch = spdk_io_channel_get_ctx(ch);
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = spdk_bdev_readv_blocks_with_md(qd_node->base_desc, qd_ch->base_ch,
					    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.md_buf,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _qdlimit_complete_io, bdev_io);
	if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io read submission, rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
_qdlimit_dispatch(struct qdlimit_io_channel *qd_ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_qdlimit *qd_node = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_qdlimit,
				 qd_bdev);
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, pt_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = spdk_bdev_writev_blocks_with_md(qd_node->base_desc, qd_ch->base_ch,
						     bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
						     bdev_io->u.bdev.md_buf,
						     bdev_io->u.bdev.offset_blocks,
						     bdev_io->u.bdev.num_blocks,
						     _qdlimit_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(qd_node->base_desc, qd_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _qdlimit_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(qd_node->base_desc, qd_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _qdlimit_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(qd_node->base_desc, qd_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _qdlimit_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(qd_node->base_desc, qd_ch->base_ch,
				     _qdlimit_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = spdk_bdev_abort(qd_node->base_desc, qd_ch->base_ch,
				     bdev_io->u.abort.bio_to_abort,
				     _qdlimit_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		rc = spdk_bdev_copy_blocks(qd_node->base_desc, qd_ch->base_ch,
					   bdev_io->u.bdev.offset_blocks,
					   bdev_io->u.bdev.copy.src_offset_blocks,
					   bdev_io->u.bdev.num_blocks,
					   _qdlimit_complete_io, bdev_io);
		break;
	default:
		SPDK_ERRLOG("qdlimit: unknown I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io submission, rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

/* Task 1: plain passthru. Replaced in Task 3 with the depth-limiting version. */
static void
vbdev_qdlimit_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct qdlimit_io_channel *qd_ch = spdk_io_channel_get_ctx(ch);
	struct qdlimit_bdev_io *io_ctx = (struct qdlimit_bdev_io *)bdev_io->driver_ctx;

	io_ctx->ch = ch;
	io_ctx->counted = false;
	_qdlimit_dispatch(qd_ch, bdev_io);
}

static bool
vbdev_qdlimit_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_qdlimit *qd_node = (struct vbdev_qdlimit *)ctx;

	return spdk_bdev_io_type_supported(qd_node->base_bdev, io_type);
}

static struct spdk_io_channel *
vbdev_qdlimit_get_io_channel(void *ctx)
{
	struct vbdev_qdlimit *qd_node = (struct vbdev_qdlimit *)ctx;

	return spdk_get_io_channel(qd_node);
}

static int
vbdev_qdlimit_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_qdlimit *qd_node = (struct vbdev_qdlimit *)ctx;

	spdk_json_write_name(w, "qdlimit");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&qd_node->qd_bdev));
	spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(qd_node->base_bdev));
	spdk_json_write_named_uint32(w, "queue_depth", qd_node->queue_depth);
	spdk_json_write_object_end(w);

	return 0;
}

static int
vbdev_qdlimit_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_qdlimit *qd_node;

	TAILQ_FOREACH(qd_node, &g_qd_nodes, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_qdlimit_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name", spdk_bdev_get_name(qd_node->base_bdev));
		spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&qd_node->qd_bdev));
		spdk_json_write_named_uint32(w, "queue_depth", qd_node->queue_depth);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

static int
qd_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct qdlimit_io_channel *qd_ch = ctx_buf;
	struct vbdev_qdlimit *qd_node = io_device;

	qd_ch->base_ch = spdk_bdev_get_io_channel(qd_node->base_desc);
	qd_ch->outstanding = 0;
	qd_ch->max_depth = qd_node->queue_depth;
	STAILQ_INIT(&qd_ch->queued_io);

	return 0;
}

static void
qd_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct qdlimit_io_channel *qd_ch = ctx_buf;

	assert(STAILQ_EMPTY(&qd_ch->queued_io));
	spdk_put_io_channel(qd_ch->base_ch);
}

static int
vbdev_qdlimit_insert_name(const char *bdev_name, const char *vbdev_name, uint32_t queue_depth)
{
	struct bdev_names *name;

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("qdlimit bdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(struct bdev_names));
	if (!name) {
		return -ENOMEM;
	}
	name->bdev_name = strdup(bdev_name);
	name->vbdev_name = strdup(vbdev_name);
	if (!name->bdev_name || !name->vbdev_name) {
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
		return -ENOMEM;
	}
	name->queue_depth = queue_depth;
	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);

	return 0;
}

static int
vbdev_qdlimit_init(void)
{
	return 0;
}

static void
vbdev_qdlimit_finish(void)
{
	struct bdev_names *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
	}
}

static int
vbdev_qdlimit_get_ctx_size(void)
{
	return sizeof(struct qdlimit_bdev_io);
}

static void
vbdev_qdlimit_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No per-bdev config needed; handled by module config_json. */
}

static int
vbdev_qdlimit_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct vbdev_qdlimit *qd_node = (struct vbdev_qdlimit *)ctx;

	return spdk_bdev_get_memory_domains(qd_node->base_bdev, domains, array_size);
}

static const struct spdk_bdev_fn_table vbdev_qdlimit_fn_table = {
	.destruct		= vbdev_qdlimit_destruct,
	.submit_request		= vbdev_qdlimit_submit_request,
	.io_type_supported	= vbdev_qdlimit_io_type_supported,
	.get_io_channel		= vbdev_qdlimit_get_io_channel,
	.dump_info_json		= vbdev_qdlimit_dump_info_json,
	.write_config_json	= vbdev_qdlimit_write_config_json,
	.get_memory_domains	= vbdev_qdlimit_get_memory_domains,
};

static void
vbdev_qdlimit_base_bdev_hotremove_cb(struct spdk_bdev *bdev_find)
{
	struct vbdev_qdlimit *qd_node, *tmp;

	TAILQ_FOREACH_SAFE(qd_node, &g_qd_nodes, link, tmp) {
		if (bdev_find == qd_node->base_bdev) {
			spdk_bdev_unregister(&qd_node->qd_bdev, NULL, NULL);
		}
	}
}

static void
vbdev_qdlimit_base_bdev_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
				 void *event_ctx)
{
	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		vbdev_qdlimit_base_bdev_hotremove_cb(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

static int
vbdev_qdlimit_register(const char *bdev_name)
{
	struct bdev_names *name;
	struct vbdev_qdlimit *qd_node;
	struct spdk_bdev *bdev;
	int rc = 0;

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev_name) != 0) {
			continue;
		}

		qd_node = calloc(1, sizeof(struct vbdev_qdlimit));
		if (!qd_node) {
			rc = -ENOMEM;
			break;
		}
		qd_node->queue_depth = name->queue_depth;
		qd_node->qd_bdev.name = strdup(name->vbdev_name);
		if (!qd_node->qd_bdev.name) {
			rc = -ENOMEM;
			free(qd_node);
			break;
		}
		qd_node->qd_bdev.product_name = "qdlimit";

		rc = spdk_bdev_open_ext(bdev_name, true, vbdev_qdlimit_base_bdev_event_cb,
					NULL, &qd_node->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("could not open bdev %s\n", bdev_name);
			}
			free(qd_node->qd_bdev.name);
			free(qd_node);
			break;
		}

		bdev = spdk_bdev_desc_get_bdev(qd_node->base_desc);
		qd_node->base_bdev = bdev;

		qd_node->qd_bdev.write_cache = bdev->write_cache;
		qd_node->qd_bdev.required_alignment = bdev->required_alignment;
		qd_node->qd_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		qd_node->qd_bdev.blocklen = bdev->blocklen;
		qd_node->qd_bdev.blockcnt = bdev->blockcnt;
		qd_node->qd_bdev.md_interleave = bdev->md_interleave;
		qd_node->qd_bdev.md_len = bdev->md_len;
		qd_node->qd_bdev.dif_type = bdev->dif_type;
		qd_node->qd_bdev.dif_is_head_of_md = bdev->dif_is_head_of_md;
		qd_node->qd_bdev.dif_check_flags = bdev->dif_check_flags;

		qd_node->qd_bdev.ctxt = qd_node;
		qd_node->qd_bdev.fn_table = &vbdev_qdlimit_fn_table;
		qd_node->qd_bdev.module = &qdlimit_if;
		TAILQ_INSERT_TAIL(&g_qd_nodes, qd_node, link);

		spdk_io_device_register(qd_node, qd_bdev_ch_create_cb, qd_bdev_ch_destroy_cb,
					sizeof(struct qdlimit_io_channel), name->vbdev_name);

		qd_node->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, qd_node->base_desc, qd_node->qd_bdev.module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", bdev_name);
			spdk_bdev_close(qd_node->base_desc);
			TAILQ_REMOVE(&g_qd_nodes, qd_node, link);
			spdk_io_device_unregister(qd_node, NULL);
			free(qd_node->qd_bdev.name);
			free(qd_node);
			break;
		}

		rc = spdk_bdev_register(&qd_node->qd_bdev);
		if (rc) {
			SPDK_ERRLOG("could not register qd_bdev\n");
			spdk_bdev_module_release_bdev(&qd_node->qd_bdev);
			spdk_bdev_close(qd_node->base_desc);
			TAILQ_REMOVE(&g_qd_nodes, qd_node, link);
			spdk_io_device_unregister(qd_node, NULL);
			free(qd_node->qd_bdev.name);
			free(qd_node);
			break;
		}
		SPDK_NOTICELOG("created qd_bdev for: %s (queue_depth=%u)\n",
			       name->vbdev_name, name->queue_depth);
	}

	return rc;
}

int
bdev_qdlimit_create_disk(const char *bdev_name, const char *vbdev_name, uint32_t queue_depth)
{
	int rc;

	rc = vbdev_qdlimit_insert_name(bdev_name, vbdev_name, queue_depth);
	if (rc) {
		return rc;
	}

	rc = vbdev_qdlimit_register(bdev_name);
	if (rc == -ENODEV) {
		SPDK_NOTICELOG("vbdev creation deferred pending base bdev arrival\n");
		rc = 0;
	}

	return rc;
}

void
bdev_qdlimit_delete_disk(const char *vbdev_name, spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct bdev_names *name;
	int rc;

	rc = spdk_bdev_unregister_by_name(vbdev_name, &qdlimit_if, cb_fn, cb_arg);
	if (rc == 0) {
		TAILQ_FOREACH(name, &g_bdev_names, link) {
			if (strcmp(name->vbdev_name, vbdev_name) == 0) {
				TAILQ_REMOVE(&g_bdev_names, name, link);
				free(name->bdev_name);
				free(name->vbdev_name);
				free(name);
				break;
			}
		}
	} else {
		cb_fn(cb_arg, rc);
	}
}

/* Placeholder; real implementation in Task 5. */
int
bdev_qdlimit_set_depth(const char *vbdev_name, uint32_t queue_depth)
{
	return 0;
}

static void
vbdev_qdlimit_examine(struct spdk_bdev *bdev)
{
	vbdev_qdlimit_register(bdev->name);
	spdk_bdev_module_examine_done(&qdlimit_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_qdlimit)
```

- [ ] **Step 3: 写模块 Makefile**

Create `module/bdev/qdlimit/Makefile`:

```makefile
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 SPDK contributors.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/bdev/

C_SRCS = vbdev_qdlimit.c vbdev_qdlimit_rpc.c
LIBNAME = bdev_qdlimit

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
```

> 该 Makefile 引用 `vbdev_qdlimit_rpc.c`，在 Task 4 创建。本任务先创建一个最小空 RPC 文件让链接通过，见 Step 4。

- [ ] **Step 4: 创建最小 RPC 占位文件（Task 4 充实）**

Create `module/bdev/qdlimit/vbdev_qdlimit_rpc.c`:

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 SPDK contributors.
 *   All rights reserved.
 */

#include "vbdev_qdlimit.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* RPC methods are added in Task 4. */
```

- [ ] **Step 5: 接入构建系统**

Modify `module/bdev/Makefile` line 9 — 在 `DIRS-y` 列表加入 `qdlimit`（按字母序插在 `passthru` 后）:

```makefile
DIRS-y += delay error gpt lvol malloc null nvme passthru qdlimit raid split zone_block
```

Modify `mk/spdk.modules.mk` line 7 — 在 `BLOCKDEV_MODULES_LIST` 加入 `bdev_qdlimit`:

```makefile
BLOCKDEV_MODULES_LIST = bdev_malloc bdev_null bdev_nvme bdev_passthru bdev_qdlimit bdev_lvol
```

Modify `mk/spdk.lib_deps.mk` — 在 `DEPDIRS-bdev_passthru` 行（约 147 行）后加一行:

```makefile
DEPDIRS-bdev_qdlimit := $(BDEV_DEPS_THREAD)
```

- [ ] **Step 6: 编译验证**

Run:
```bash
./configure && make -j$(nproc) -C module/bdev/qdlimit
```
Expected: 编译链接成功，生成 `build/lib/libspdk_bdev_qdlimit.a`（或 .so）。无 warning/error。

> 若尚未 configure 过整个树，可先 `make -j$(nproc)` 整体构建一次确认无回归。

- [ ] **Step 7: Commit**

```bash
git add module/bdev/qdlimit module/bdev/Makefile mk/spdk.modules.mk mk/spdk.lib_deps.mk
git commit -m "module/bdev/qdlimit: add passthru skeleton for per-SSD queue limiter

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: 准入/计数纯函数单测（TDD）

`qdlimit_io_is_limited` / `qdlimit_try_acquire` / `qdlimit_release` 是 header 中的纯函数，可脱离 bdev 机器直接测。

**Files:**
- Create: `test/unit/lib/bdev/qdlimit.c/qdlimit_ut.c`
- Create: `test/unit/lib/bdev/qdlimit.c/Makefile`
- Modify: `test/unit/lib/bdev/Makefile`

- [ ] **Step 1: 写失败的单测**

Create `test/unit/lib/bdev/qdlimit.c/qdlimit_ut.c`:

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 SPDK contributors.
 *   All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk/stdinc.h"

/* Forward-declare so the header's STAILQ_HEAD(, qdlimit_bdev_io) compiles. */
struct qdlimit_bdev_io;

#include "bdev/qdlimit/vbdev_qdlimit.h"

static void
test_acquire_release_unlimited(void)
{
	struct qdlimit_io_channel qd_ch = {};

	qd_ch.max_depth = 0; /* unlimited */
	qd_ch.outstanding = 0;

	/* Always admits when unlimited, counter still tracks in-flight. */
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);
	CU_ASSERT(qd_ch.outstanding == 1);
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);
	CU_ASSERT(qd_ch.outstanding == 2);

	qdlimit_release(&qd_ch);
	CU_ASSERT(qd_ch.outstanding == 1);
	qdlimit_release(&qd_ch);
	CU_ASSERT(qd_ch.outstanding == 0);
}

static void
test_acquire_caps_at_max_depth(void)
{
	struct qdlimit_io_channel qd_ch = {};

	qd_ch.max_depth = 2;
	qd_ch.outstanding = 0;

	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);  /* 1 */
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);  /* 2 */
	CU_ASSERT(qd_ch.outstanding == 2);

	/* At cap -> rejected, counter unchanged. */
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == false);
	CU_ASSERT(qd_ch.outstanding == 2);

	/* Free one slot -> next acquire succeeds. */
	qdlimit_release(&qd_ch);
	CU_ASSERT(qd_ch.outstanding == 1);
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);
	CU_ASSERT(qd_ch.outstanding == 2);
}

static void
test_io_is_limited(void)
{
	/* Data-path ops are limited. */
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_READ) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_WRITE) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_UNMAP) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_WRITE_ZEROES) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_FLUSH) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_COPY) == true);

	/* Management / buffer ops bypass the limit. */
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_RESET) == false);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_ABORT) == false);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_ZCOPY) == false);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("qdlimit", NULL, NULL);

	CU_ADD_TEST(suite, test_acquire_release_unlimited);
	CU_ADD_TEST(suite, test_acquire_caps_at_max_depth);
	CU_ADD_TEST(suite, test_io_is_limited);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
```

- [ ] **Step 2: 写单测 Makefile**

Create `test/unit/lib/bdev/qdlimit.c/Makefile`:

```makefile
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 SPDK contributors.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../../..)

TEST_FILE = qdlimit_ut.c

include $(SPDK_ROOT_DIR)/mk/spdk.unittest.mk
```

- [ ] **Step 3: 接入单测构建**

Modify `test/unit/lib/bdev/Makefile` line 9 — `DIRS-y` 加入 `qdlimit.c`（插在 `part.c` 后）:

```makefile
DIRS-y = bdev.c part.c qdlimit.c scsi_nvme.c gpt vbdev_lvol.c mt raid bdev_zone.c vbdev_zone_block.c nvme
```

- [ ] **Step 4: 运行单测，确认通过**

Run:
```bash
make -C test/unit/lib/bdev/qdlimit.c
./test/unit/lib/bdev/qdlimit.c/qdlimit_ut
```
Expected: 3 个用例全部 PASS，`num_failures = 0`。

> 由于被测函数是 header inline 纯函数、Task 1 已存在，单测应直接通过（这里的 TDD 价值在于把准入语义钉成可回归的契约；若先于 Task 1 运行则会因找不到 header 而编译失败）。

- [ ] **Step 5: Commit**

```bash
git add test/unit/lib/bdev/qdlimit.c test/unit/lib/bdev/Makefile
git commit -m "test/unit/bdev: unit-test qdlimit admission/counter helpers

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: 让深度限制生效（计数 + 排队 + 放行）

替换 Task 1 的纯透传 `submit_request`，接入准入判定与等待队列，并在完成回调里释放槽位、放行下一个。

**Files:**
- Modify: `module/bdev/qdlimit/vbdev_qdlimit.c`

- [ ] **Step 1: 替换完成回调为"释放并放行"版本**

在 `vbdev_qdlimit.c` 中，将 Task 1 的 `_qdlimit_complete_io` 函数整体替换为下面的实现，并在其上方新增 `_qdlimit_release_and_drain`、`vbdev_qdlimit_resubmit_io`、`vbdev_qdlimit_queue_io_wait` 三个函数。找到原文本：

```c
static void
_qdlimit_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_io_complete(orig_io, status);
	spdk_bdev_free_io(bdev_io);
}
```

替换为：

```c
/*
 * Release the in-flight slot held by io_ctx (if any) and admit one queued IO.
 * Only counted (limited) IO hold a slot, so bypass IO never trigger a drain.
 */
static void
_qdlimit_release_and_drain(struct qdlimit_io_channel *qd_ch, struct qdlimit_bdev_io *io_ctx)
{
	struct qdlimit_bdev_io *next_ctx;
	bool acquired;

	if (!io_ctx->counted) {
		return;
	}

	qdlimit_release(qd_ch);
	io_ctx->counted = false;

	next_ctx = STAILQ_FIRST(&qd_ch->queued_io);
	if (next_ctx == NULL) {
		return;
	}
	STAILQ_REMOVE_HEAD(&qd_ch->queued_io, link);

	/* We just freed a slot, so this acquire must succeed. */
	acquired = qdlimit_try_acquire(qd_ch);
	assert(acquired);
	(void)acquired;
	next_ctx->counted = true;

	_qdlimit_dispatch(qd_ch, spdk_bdev_io_from_ctx(next_ctx));
}

static void
_qdlimit_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct qdlimit_bdev_io *io_ctx = (struct qdlimit_bdev_io *)orig_io->driver_ctx;
	struct qdlimit_io_channel *qd_ch = spdk_io_channel_get_ctx(io_ctx->ch);
	int status = success ? SPDK_BDEV_IO_STATUS_SUCCESS : SPDK_BDEV_IO_STATUS_FAILED;

	spdk_bdev_free_io(bdev_io);
	_qdlimit_release_and_drain(qd_ch, io_ctx);
	spdk_bdev_io_complete(orig_io, status);
}

/* Re-dispatch an IO whose base submission previously failed with -ENOMEM.
 * The slot stays reserved across the wait, so we go straight to dispatch.
 */
static void
vbdev_qdlimit_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct qdlimit_bdev_io *io_ctx = (struct qdlimit_bdev_io *)bdev_io->driver_ctx;
	struct qdlimit_io_channel *qd_ch = spdk_io_channel_get_ctx(io_ctx->ch);

	_qdlimit_dispatch(qd_ch, bdev_io);
}

static void
vbdev_qdlimit_queue_io_wait(struct qdlimit_io_channel *qd_ch, struct spdk_bdev_io *bdev_io)
{
	struct qdlimit_bdev_io *io_ctx = (struct qdlimit_bdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->bdev_io_wait.bdev = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn = vbdev_qdlimit_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, qd_ch->base_ch, &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed in vbdev_qdlimit_queue_io_wait, rc=%d.\n", rc);
		_qdlimit_release_and_drain(qd_ch, io_ctx);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}
```

- [ ] **Step 2: 让 dispatch 在 -ENOMEM 时走 io_wait 重试（不丢槽位）**

在 `_qdlimit_dispatch` 末尾，将 Task 1 的硬失败处理：

```c
	if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io submission, rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
```

替换为：

```c
	if (rc != 0) {
		struct qdlimit_bdev_io *io_ctx = (struct qdlimit_bdev_io *)bdev_io->driver_ctx;

		if (rc == -ENOMEM) {
			vbdev_qdlimit_queue_io_wait(qd_ch, bdev_io);
		} else {
			SPDK_ERRLOG("ERROR on bdev_io submission, rc=%d\n", rc);
			_qdlimit_release_and_drain(qd_ch, io_ctx);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
```

同样，将 `pt_read_get_buf_cb` 中的读提交失败处理：

```c
	if (rc != 0) {
		SPDK_ERRLOG("ERROR on bdev_io read submission, rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
```

替换为：

```c
	if (rc != 0) {
		if (rc == -ENOMEM) {
			vbdev_qdlimit_queue_io_wait(qd_ch, bdev_io);
		} else {
			struct qdlimit_bdev_io *io_ctx =
				(struct qdlimit_bdev_io *)bdev_io->driver_ctx;
			SPDK_ERRLOG("ERROR on bdev_io read submission, rc=%d\n", rc);
			_qdlimit_release_and_drain(qd_ch, io_ctx);
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
```

> `pt_read_get_buf_cb` 已通过 `qd_ch = spdk_io_channel_get_ctx(ch)` 拿到通道，可直接复用。

- [ ] **Step 3: 替换 submit_request 为限流版本**

将 Task 1 的：

```c
/* Task 1: plain passthru. Replaced in Task 3 with the depth-limiting version. */
static void
vbdev_qdlimit_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct qdlimit_io_channel *qd_ch = spdk_io_channel_get_ctx(ch);
	struct qdlimit_bdev_io *io_ctx = (struct qdlimit_bdev_io *)bdev_io->driver_ctx;

	io_ctx->ch = ch;
	io_ctx->counted = false;
	_qdlimit_dispatch(qd_ch, bdev_io);
}
```

替换为：

```c
static void
vbdev_qdlimit_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct qdlimit_io_channel *qd_ch = spdk_io_channel_get_ctx(ch);
	struct qdlimit_bdev_io *io_ctx = (struct qdlimit_bdev_io *)bdev_io->driver_ctx;

	io_ctx->ch = ch;
	io_ctx->counted = false;

	/* Management / buffer ops bypass the depth limit. */
	if (!qdlimit_io_is_limited(bdev_io->type)) {
		_qdlimit_dispatch(qd_ch, bdev_io);
		return;
	}

	if (qdlimit_try_acquire(qd_ch)) {
		io_ctx->counted = true;
		_qdlimit_dispatch(qd_ch, bdev_io);
	} else {
		/* At per-core cap: hold the IO until a slot frees up. */
		STAILQ_INSERT_TAIL(&qd_ch->queued_io, io_ctx, link);
	}
}
```

- [ ] **Step 4: 编译验证**

Run:
```bash
make -j$(nproc) -C module/bdev/qdlimit
```
Expected: 编译链接成功，无 warning/error。

- [ ] **Step 5: 回归运行 Task 2 单测**

Run:
```bash
make -C test/unit/lib/bdev/qdlimit.c && ./test/unit/lib/bdev/qdlimit.c/qdlimit_ut
```
Expected: 仍全部 PASS（纯函数语义未变）。

- [ ] **Step 6: Commit**

```bash
git add module/bdev/qdlimit/vbdev_qdlimit.c
git commit -m "module/bdev/qdlimit: enforce per-core in-flight depth with FIFO queue

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: RPC create / delete + config_json

**Files:**
- Modify: `module/bdev/qdlimit/vbdev_qdlimit_rpc.c`
- Modify: `scripts/rpc.py`
- Modify: `python/spdk/rpc/bdev.py`

- [ ] **Step 1: 写 RPC create/delete 实现**

将 `module/bdev/qdlimit/vbdev_qdlimit_rpc.c` 的占位内容整体替换为：

```c
/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 SPDK contributors.
 *   All rights reserved.
 */

#include "vbdev_qdlimit.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

struct rpc_bdev_qdlimit_create {
	char		*base_bdev_name;
	char		*name;
	uint32_t	queue_depth;
};

static void
free_rpc_bdev_qdlimit_create(struct rpc_bdev_qdlimit_create *r)
{
	free(r->base_bdev_name);
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_qdlimit_create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_bdev_qdlimit_create, base_bdev_name), spdk_json_decode_string},
	{"name", offsetof(struct rpc_bdev_qdlimit_create, name), spdk_json_decode_string},
	{"queue_depth", offsetof(struct rpc_bdev_qdlimit_create, queue_depth), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_qdlimit_create(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_qdlimit_create req = {NULL};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_qdlimit_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_qdlimit_create_decoders),
				    &req)) {
		SPDK_DEBUGLOG(vbdev_qdlimit, "spdk_json_decode_object failed\n");
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_qdlimit_create_disk(req.base_bdev_name, req.name, req.queue_depth);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_qdlimit_create(&req);
}
SPDK_RPC_REGISTER("bdev_qdlimit_create", rpc_bdev_qdlimit_create, SPDK_RPC_RUNTIME)

struct rpc_bdev_qdlimit_delete {
	char *name;
};

static void
free_rpc_bdev_qdlimit_delete(struct rpc_bdev_qdlimit_delete *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_qdlimit_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_qdlimit_delete, name), spdk_json_decode_string},
};

static void
rpc_bdev_qdlimit_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		spdk_jsonrpc_send_bool_response(request, true);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_qdlimit_delete(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_bdev_qdlimit_delete req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_qdlimit_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_qdlimit_delete_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	bdev_qdlimit_delete_disk(req.name, rpc_bdev_qdlimit_delete_cb, request);

cleanup:
	free_rpc_bdev_qdlimit_delete(&req);
}
SPDK_RPC_REGISTER("bdev_qdlimit_delete", rpc_bdev_qdlimit_delete, SPDK_RPC_RUNTIME)
```

> `set_depth` 的 RPC 在 Task 5 追加到本文件末尾（保留 `SPDK_LOG_REGISTER_COMPONENT` 不在 rpc 文件，组件已在 .c 注册）。

- [ ] **Step 2: 加 Python client 函数**

Modify `python/spdk/rpc/bdev.py` — 在 `bdev_passthru_delete` 函数后插入：

```python
def bdev_qdlimit_create(client, base_bdev_name, name, queue_depth=None):
    """Construct a queue-depth limiting block device on top of a base bdev.

    Args:
        base_bdev_name: name of the existing bdev
        name: name of the new qdlimit bdev
        queue_depth: per-core max in-flight IO (0 = unlimited, default 0)

    Returns:
        Name of created block device.
    """
    params = {
        'base_bdev_name': base_bdev_name,
        'name': name,
    }
    if queue_depth is not None:
        params['queue_depth'] = queue_depth
    return client.call('bdev_qdlimit_create', params)


def bdev_qdlimit_delete(client, name):
    """Remove a qdlimit bdev from the system.

    Args:
        name: name of the qdlimit bdev to delete
    """
    params = {'name': name}
    return client.call('bdev_qdlimit_delete', params)
```

- [ ] **Step 3: 加 rpc.py CLI 子命令**

Modify `scripts/rpc.py` — 在 `bdev_passthru_delete` 的 `set_defaults` 块后插入：

```python
    def bdev_qdlimit_create(args):
        print_json(rpc.bdev.bdev_qdlimit_create(args.client,
                                                base_bdev_name=args.base_bdev_name,
                                                name=args.name,
                                                queue_depth=args.queue_depth))

    p = subparsers.add_parser('bdev_qdlimit_create',
                              help='Add a per-core queue-depth limiting bdev on an existing bdev')
    p.add_argument('-b', '--base-bdev-name', help="Name of the existing bdev", required=True)
    p.add_argument('-p', '--name', help="Name of the qdlimit bdev", required=True)
    p.add_argument('-d', '--queue-depth', type=int, default=0,
                   help="Per-core max in-flight IO (0 = unlimited)")
    p.set_defaults(func=bdev_qdlimit_create)

    def bdev_qdlimit_delete(args):
        rpc.bdev.bdev_qdlimit_delete(args.client, name=args.name)

    p = subparsers.add_parser('bdev_qdlimit_delete', help='Delete a qdlimit bdev')
    p.add_argument('name', help='qdlimit bdev name')
    p.set_defaults(func=bdev_qdlimit_delete)
```

- [ ] **Step 4: 编译验证 + 启动冒烟**

Run:
```bash
make -j$(nproc) -C module/bdev/qdlimit
make -j$(nproc)
```
Expected: 全树编译成功。

Run（手动冒烟，确认 RPC 注册成功）:
```bash
build/bin/spdk_tgt &
sleep 3
scripts/rpc.py bdev_malloc_create -b Malloc0 64 512
scripts/rpc.py bdev_qdlimit_create -b Malloc0 -p QD0 -d 8
scripts/rpc.py bdev_get_bdevs -b QD0
scripts/rpc.py bdev_qdlimit_delete QD0
scripts/rpc.py spdk_kill_instance SIGTERM
```
Expected: `bdev_get_bdevs -b QD0` 输出含 `"product_name": "qdlimit"` 与 `"queue_depth": 8`；create/delete 均返回成功。

- [ ] **Step 5: Commit**

```bash
git add module/bdev/qdlimit/vbdev_qdlimit_rpc.c scripts/rpc.py python/spdk/rpc/bdev.py
git commit -m "module/bdev/qdlimit: add create/delete RPC and python bindings

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: set_depth 运行时调参

把 Task 1 的占位 `bdev_qdlimit_set_depth` 换成真实现：更新节点 `queue_depth`，并通过 `spdk_for_each_channel` 把新阈值广播到各核通道的 `max_depth`；同时加 RPC。

**Files:**
- Modify: `module/bdev/qdlimit/vbdev_qdlimit.c`
- Modify: `module/bdev/qdlimit/vbdev_qdlimit_rpc.c`
- Modify: `scripts/rpc.py`
- Modify: `python/spdk/rpc/bdev.py`

- [ ] **Step 1: 实现 set_depth + per-channel 广播**

在 `vbdev_qdlimit.c` 中，将占位实现：

```c
/* Placeholder; real implementation in Task 5. */
int
bdev_qdlimit_set_depth(const char *vbdev_name, uint32_t queue_depth)
{
	return 0;
}
```

替换为：

```c
struct qdlimit_set_depth_ctx {
	uint32_t queue_depth;
};

static void
_qdlimit_set_depth_on_channel(struct spdk_io_channel_iter *i)
{
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct qdlimit_io_channel *qd_ch = spdk_io_channel_get_ctx(ch);
	struct qdlimit_set_depth_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	/*
	 * Lowering the cap below current outstanding does not abort in-flight
	 * IO; the channel simply admits nothing new until it drains under the
	 * new cap. Queued IO are released by completions as usual.
	 */
	qd_ch->max_depth = ctx->queue_depth;

	spdk_for_each_channel_continue(i, 0);
}

static void
_qdlimit_set_depth_done(struct spdk_io_channel_iter *i, int status)
{
	struct qdlimit_set_depth_ctx *ctx = spdk_io_channel_iter_get_ctx(i);

	free(ctx);
}

int
bdev_qdlimit_set_depth(const char *vbdev_name, uint32_t queue_depth)
{
	struct vbdev_qdlimit *qd_node = NULL, *tmp;
	struct qdlimit_set_depth_ctx *ctx;

	TAILQ_FOREACH(tmp, &g_qd_nodes, link) {
		if (strcmp(tmp->qd_bdev.name, vbdev_name) == 0) {
			qd_node = tmp;
			break;
		}
	}
	if (qd_node == NULL) {
		return -ENODEV;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->queue_depth = queue_depth;

	/* Update the node so new channels and config_json see the new value. */
	qd_node->queue_depth = queue_depth;

	spdk_for_each_channel(qd_node, _qdlimit_set_depth_on_channel, ctx,
			      _qdlimit_set_depth_done);

	return 0;
}
```

> 同时更新名字关联表里的 `queue_depth`，使 delete+examine 重建后仍保持新值。在上面 `qd_node->queue_depth = queue_depth;` 之后追加：
>
> ```c
> 	{
> 		struct bdev_names *name;
> 		TAILQ_FOREACH(name, &g_bdev_names, link) {
> 			if (strcmp(name->vbdev_name, vbdev_name) == 0) {
> 				name->queue_depth = queue_depth;
> 				break;
> 			}
> 		}
> 	}
> ```

- [ ] **Step 2: 加 set_depth RPC**

在 `module/bdev/qdlimit/vbdev_qdlimit_rpc.c` 末尾追加：

```c
struct rpc_bdev_qdlimit_set_depth {
	char		*name;
	uint32_t	queue_depth;
};

static void
free_rpc_bdev_qdlimit_set_depth(struct rpc_bdev_qdlimit_set_depth *req)
{
	free(req->name);
}

static const struct spdk_json_object_decoder rpc_bdev_qdlimit_set_depth_decoders[] = {
	{"name", offsetof(struct rpc_bdev_qdlimit_set_depth, name), spdk_json_decode_string},
	{"queue_depth", offsetof(struct rpc_bdev_qdlimit_set_depth, queue_depth), spdk_json_decode_uint32},
};

static void
rpc_bdev_qdlimit_set_depth(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_bdev_qdlimit_set_depth req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_qdlimit_set_depth_decoders,
				    SPDK_COUNTOF(rpc_bdev_qdlimit_set_depth_decoders),
				    &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}

	rc = bdev_qdlimit_set_depth(req.name, req.queue_depth);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free_rpc_bdev_qdlimit_set_depth(&req);
}
SPDK_RPC_REGISTER("bdev_qdlimit_set_depth", rpc_bdev_qdlimit_set_depth, SPDK_RPC_RUNTIME)
```

- [ ] **Step 3: 加 Python 绑定 + CLI**

Modify `python/spdk/rpc/bdev.py` — 在 `bdev_qdlimit_delete` 后插入：

```python
def bdev_qdlimit_set_depth(client, name, queue_depth):
    """Update the per-core queue depth of an existing qdlimit bdev.

    Args:
        name: name of the qdlimit bdev
        queue_depth: new per-core max in-flight IO (0 = unlimited)
    """
    params = {'name': name, 'queue_depth': queue_depth}
    return client.call('bdev_qdlimit_set_depth', params)
```

Modify `scripts/rpc.py` — 在 `bdev_qdlimit_delete` 子命令后插入：

```python
    def bdev_qdlimit_set_depth(args):
        rpc.bdev.bdev_qdlimit_set_depth(args.client,
                                        name=args.name,
                                        queue_depth=args.queue_depth)

    p = subparsers.add_parser('bdev_qdlimit_set_depth',
                              help='Update per-core queue depth of a qdlimit bdev')
    p.add_argument('name', help='qdlimit bdev name')
    p.add_argument('queue_depth', type=int, help='per-core max in-flight IO (0 = unlimited)')
    p.set_defaults(func=bdev_qdlimit_set_depth)
```

- [ ] **Step 4: 编译验证 + 冒烟**

Run:
```bash
make -j$(nproc)
```
Expected: 全树编译成功。

Run:
```bash
build/bin/spdk_tgt &
sleep 3
scripts/rpc.py bdev_malloc_create -b Malloc0 64 512
scripts/rpc.py bdev_qdlimit_create -b Malloc0 -p QD0 -d 8
scripts/rpc.py bdev_qdlimit_set_depth QD0 16
scripts/rpc.py bdev_get_bdevs -b QD0   # 期望 queue_depth=16
scripts/rpc.py spdk_kill_instance SIGTERM
```
Expected: `set_depth` 返回 true，`bdev_get_bdevs` 显示 `"queue_depth": 16`。

- [ ] **Step 5: Commit**

```bash
git add module/bdev/qdlimit scripts/rpc.py python/spdk/rpc/bdev.py
git commit -m "module/bdev/qdlimit: add bdev_qdlimit_set_depth runtime tuning RPC

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: 集成测试脚本（bdevperf over malloc）

无需硬件，用 malloc bdev 作底层，验证 qdlimit 端到端可创建、跑 IO、可摘除。

**Files:**
- Create: `test/bdev/qdlimit.sh`

- [ ] **Step 1: 写集成测试脚本**

Create `test/bdev/qdlimit.sh`:

```bash
#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 SPDK contributors.
#  All rights reserved.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"
bdevperf=$rootdir/build/examples/bdevperf

# Start bdevperf in RPC mode (no JSON config; we create bdevs via RPC).
"$bdevperf" -z -q 32 -o 4096 -t 5 -w randrw -M 50 &
bdevperf_pid=$!
trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid

# Build: Malloc0 -> QD0 (per-core depth 8).
$rpc_py bdev_malloc_create -b Malloc0 64 512
$rpc_py bdev_qdlimit_create -b Malloc0 -p QD0 -d 8

# Verify the vbdev exists with expected properties.
$rpc_py bdev_get_bdevs -b QD0 | grep -q '"product_name": "qdlimit"'
$rpc_py bdev_get_bdevs -b QD0 | grep -q '"queue_depth": 8'

# Drive IO through QD0 and verify it completes without error.
$rpc_py framework_get_subsystems > /dev/null
PYTHONPATH=$PYTHONPATH:$rootdir/python "$rootdir/examples/bdev/bdevperf/bdevperf.py" \
	perform_tests

# Runtime depth change must succeed.
$rpc_py bdev_qdlimit_set_depth QD0 16
$rpc_py bdev_get_bdevs -b QD0 | grep -q '"queue_depth": 16'

# Teardown.
$rpc_py bdev_qdlimit_delete QD0
$rpc_py bdev_malloc_delete Malloc0

trap - SIGINT SIGTERM EXIT
killprocess $bdevperf_pid
```

> 说明：bdevperf 以 `-z`（zcopy/等待 RPC 触发）+ RPC 模式启动；`bdevperf.py perform_tests` 触发实际 IO。脚本验证 ①vbdev 属性正确 ②IO 端到端通过 ③运行时调参生效 ④可干净摘除。深度数值与 bdevperf 的 `-q 32` 配合，确保跨越阈值触发排队路径。

- [ ] **Step 2: 赋可执行权限**

Run:
```bash
chmod +x test/bdev/qdlimit.sh
```

- [ ] **Step 3: 运行集成测试**

Run:
```bash
make -j$(nproc)
sudo ./test/bdev/qdlimit.sh
```
Expected: 脚本退出码 0；各 `grep -q` 断言通过；bdevperf 跑完 5s randrw 无 IO 错误；无 trap 报错。

> 若环境无 hugepages，先 `sudo scripts/setup.sh`。

- [ ] **Step 4: Commit**

```bash
git add test/bdev/qdlimit.sh
git commit -m "test/bdev: add qdlimit end-to-end integration test over malloc

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-Review 记录

**1. Spec 覆盖检查：**
- 目标(压尾延迟/防过载) → Task 1+3 的 per-core 在途上限 ✓
- per-core 无锁近似 → `qdlimit_io_channel` 仅本核访问 + inline 计数 ✓
- 独立 vbdev、零改主干 → Task 1 新增目录 + 仅改构建/RPC 注册表 ✓
- 静态阈值 + 可选运行时调参 → Task 4 create 带 `queue_depth`、Task 5 `set_depth` ✓
- queue_depth=0 不限流 → `qdlimit_try_acquire` 的 `max_depth != 0` 判定 ✓ + Task 2 单测覆盖 ✓
- RESET/ABORT 绕过 → `qdlimit_io_is_limited` + submit_request 分支 ✓ + 单测 ✓
- 底层 ENOMEM → `vbdev_qdlimit_queue_io_wait`（与深度队列分离，槽位保留）✓
- 底层 hotremove → `vbdev_qdlimit_base_bdev_event_cb` ✓
- 通道销毁断言队列空 → `qd_bdev_ch_destroy_cb` 的 `assert(STAILQ_EMPTY)` ✓
- config_json 持久化 → `vbdev_qdlimit_config_json` 输出 create + queue_depth ✓
- 测试(单测/集成/RPC) → Task 2 / Task 6 / Task 4-5 冒烟 ✓

**2. 占位符扫描：** Task 1 Step 2/4 与 Task 1 的 `set_depth` 是**有意的、带说明的临时实现**，分别在 Task 4/Task 5 被完整替换，非计划缺口。其余无 TBD/TODO。

**3. 类型/命名一致性：** `vbdev_qdlimit` / `qdlimit_io_channel` / `qdlimit_bdev_io` / `qdlimit_if` / `g_qd_nodes` / `g_bdev_names` / `qdlimit_try_acquire` / `qdlimit_release` / `qdlimit_io_is_limited` / `_qdlimit_dispatch` / `_qdlimit_complete_io` / `_qdlimit_release_and_drain` / `bdev_qdlimit_create_disk` / `bdev_qdlimit_delete_disk` / `bdev_qdlimit_set_depth` — 全计划统一。RPC 方法名 `bdev_qdlimit_create/delete/set_depth` 三处（C/python/CLI）一致。
