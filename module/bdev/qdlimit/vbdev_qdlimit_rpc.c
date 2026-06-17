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
