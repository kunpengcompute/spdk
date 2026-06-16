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
