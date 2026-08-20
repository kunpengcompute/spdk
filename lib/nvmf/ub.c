/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/config.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvmf_transport.h"
#include "spdk/string.h"
#include "spdk/trace.h"
#include "spdk/tree.h"
#include "spdk/util.h"
#include "spdk/sock.h"
#include "spdk_internal/nvme_ub.h"

#include "spdk_internal/assert.h"
#include "spdk/log.h"

/* URMA APi includes */
#include "urma_api.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk_internal/trace_defs.h"

SPDK_STATIC_ASSERT(URMA_EID_SIZE == SPDK_NVME_UB_EID_SIZE,
		   "URMA EID size does not match UB OOB protocol");
SPDK_STATIC_ASSERT(SPDK_NVME_UB_MAX_SGL_DESCRIPTORS <= SPDK_NVMF_MAX_SGL_ENTRIES,
		   "UB descriptor limit exceeds the NVMf request SGL capacity");

#ifndef PAGE_SIZE
#define PAGE_SIZE (0x1 << 12) /* 4KB */
#endif

#define MSG_SIZE 4096
#define URMA_DEVICE_NAME_ENV "URMA_DEVICE_NAME"
#define URMA_DEFAULT_DEVICE_NAME "bonding_dev_0"
#define URMA_EID_INDEX_ENV "URMA_EID_INDEX"
#define URMA_DEFAULT_EID_INDEX 1
#define URMA_BONDING_DEVICE_PREFIX "bonding_dev_"

static bool
nvmf_ub_device_uses_multipath(const char *dev_name)
{
	return dev_name != NULL &&
	       strncmp(dev_name, URMA_BONDING_DEVICE_PREFIX,
	               sizeof(URMA_BONDING_DEVICE_PREFIX) - 1) == 0;
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_ub;
static const struct spdk_mem_map_ops g_nvmf_ub_mem_map_ops;

static bool g_nvmf_urma_initialized;

#define SPDK_NVMF_UB_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_UB_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_UB_DEFAULT_MAX_QPAIRS_PER_CTRLR 8 /* Limit to 8 queues for now. */
#define SPDK_NVMF_UB_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE (128 * 1024)
#define SPDK_NVMF_UB_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_UB_DEFAULT_ABORT_TIMEOUT_SEC 1
#define SPDK_NVMF_UB_DEFAULT_DATA_WR_POOL_SIZE 4095
#define SPDK_NVMF_UB_MIN_IO_BUFFER_SIZE (SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE / SPDK_NVMF_MAX_SGL_ENTRIES)
#define SPDK_NVMF_UB_DEFAULT_NUM_SHARED_BUFFERS 4095
#define SPDK_NVMF_UB_DEFAULT_BUFFER_CACHE_SIZE UINT32_MAX

#define NVMF_DEFAULT_TX_SGE		SPDK_NVMF_MAX_SGL_ENTRIES

/* UB transport specific constants */
#define SPDK_NVMF_UB_MAX_ACCEPT_SOCK_ONE_TIME 16
#define NVMF_UB_INVALID_RECV_SLOT UINT32_MAX

/* UB transport qpair state */
enum spdk_nvmf_ub_qpair_state {
	UB_QPAIR_STATE_CONNECTING = 1,
	UB_QPAIR_STATE_RUNNING = 2,
	UB_QPAIR_STATE_DISCONNECTING = 3,
	UB_QPAIR_STATE_DISCONNECTED = 4,
};

struct spdk_nvmf_ub_poll_group {
	struct spdk_nvmf_transport_poll_group		group;
	/* Scratch array for urma_poll_jfc() results */
	urma_cr_t                      *crs;
	uint32_t                        max_crs;

	TAILQ_HEAD(, spdk_nvmf_ub_qpair)	qpairs;
};

struct spdk_nvmf_ub_transport {
	/* Must be first */
	struct spdk_nvmf_transport	transport;

	/* URMA context */
	urma_context_t				*urma_ctx;
	urma_jfce_t				*jfce;
	struct spdk_mem_map			*mem_map;
	bool					multi_path;

	/* Pending connections sock_group for handling connect requests */
	struct spdk_sock_group		*listen_sock_group;
	struct spdk_poller			*accept_poller;
	struct spdk_sock_group		*pending_sock_group;
	struct spdk_poller			*pending_poller;

	/* List of ports */
	TAILQ_HEAD(, spdk_nvmf_ub_port)	ports;
};

static inline struct spdk_nvmf_ub_transport *
nvmf_ub_get_transport(struct spdk_nvmf_transport *transport)
{
	return SPDK_CONTAINEROF(transport, struct spdk_nvmf_ub_transport, transport);
}

static inline struct spdk_nvmf_ub_poll_group *
nvmf_ub_get_poll_group(struct spdk_nvmf_transport_poll_group *group)
{
	return SPDK_CONTAINEROF(group, struct spdk_nvmf_ub_poll_group, group);
}

enum spdk_nvmf_ub_req_ub_state {
	UB_REQ_UB_STATE_NONE = 0,
	UB_REQ_UB_STATE_WAIT_READ,	/* Waiting for UB READ completion */
	UB_REQ_UB_STATE_WAIT_WRITE,	/* Waiting for UB WRITE completion */
	UB_REQ_UB_STATE_WAIT_RESPONSE,	/* Waiting for a response SEND context */
};

struct spdk_nvmf_ub_remote_sge {
	uint64_t				remote_addr;
	uint32_t				length;
	uint32_t				key;
	urma_target_seg_t			*tseg;
	urma_target_jetty_t			*tjetty;
};

struct spdk_nvmf_ub_request {
	struct spdk_nvmf_request		req;

	/* Buffer index in the qpair's registered data region. */
	uint32_t				buf_idx;

	/* Remote address and key for UB operations */
	uint64_t				remote_addr;
	uint32_t				remote_key;
	urma_target_seg_t			*remote_data_tseg;
	urma_target_jetty_t			*remote_data_tjetty;
	struct spdk_nvmf_ub_remote_sge		remote_sges[SPDK_NVME_UB_MAX_SGL_DESCRIPTORS];
	uint32_t				num_remote_sges;
	uint32_t				num_outstanding_data_wr;
	bool					data_transfer_failed;

	/* Flag indicating if data needs to be fetched from remote */
	bool					data_from_remote;

	/* Flag indicating urma_read was issued and we're waiting for completion
	 * before calling exec(). Only used for HOST_TO_CONTROLLER transfers.
	 */
	bool					awaiting_ub_read_completion;
	bool					awaiting_ub_write_completion;

	/* Async state machine: tracks which URMA completion we are waiting for */
	enum spdk_nvmf_ub_req_ub_state		ub_state;
	uint32_t				recv_slot;
	bool					in_use;
	bool					zcopy_abort;
	STAILQ_ENTRY(spdk_nvmf_ub_request)		link;
};

struct spdk_nvmf_ub_response {
	/* SEND-owned completion storage remains stable after its request is free. */
	uint32_t				buf_idx;
	bool					in_use;
	STAILQ_ENTRY(spdk_nvmf_ub_response)	link;
};

struct spdk_nvmf_ub_remote_seg {
	uint32_t				token_id;
	uint64_t				addr;
	uint64_t				length;
	urma_target_seg_t			*tseg;
	TAILQ_ENTRY(spdk_nvmf_ub_remote_seg)	link;
};

struct spdk_nvmf_ub_npu_endpoint {
	uint32_t				endpoint_id;
	urma_target_jetty_t			*tjetty;
	TAILQ_ENTRY(spdk_nvmf_ub_npu_endpoint)	link;
};

struct spdk_nvmf_ub_npu_region {
	uint32_t				region_id;
	uint32_t				endpoint_id;
	uint64_t				remote_base;
	uint64_t				length;
	urma_target_seg_t			*tseg;
	urma_target_jetty_t			*tjetty;
	TAILQ_ENTRY(spdk_nvmf_ub_npu_region)	link;
};

struct spdk_nvmf_ub_qpair {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_ub_poll_group		*group;

	/* URMA resources */
	urma_jetty_t				*jetty;
	urma_target_jetty_t			*target_jetty;
	urma_jfc_t				*send_jfc;
	urma_jfr_t			*jfr;

	uint32_t		depth;
	/* Connection state */
	enum spdk_nvmf_ub_qpair_state		state;

	/* UB resources for this qpair */
	struct spdk_nvmf_ub_resources		*resources;

	/* Queue identification */
	uint16_t				qid;

	/* Remote jetty info for import */
	urma_jetty_id_t				remote_jetty_id;
	uint32_t				remote_uasid;
	urma_eid_t				remote_eid;

	void					*va;
	urma_target_seg_t			*local_tseg;
	urma_target_seg_t			*remote_tseg;
	TAILQ_HEAD(, spdk_nvmf_ub_remote_seg)	remote_segs;
	TAILQ_HEAD(, spdk_nvmf_ub_npu_endpoint)	npu_endpoints;
	TAILQ_HEAD(, spdk_nvmf_ub_npu_region)	npu_regions;
	size_t				rsp_offset;
	size_t				data_offset;
	size_t				seg_len;

	/* Controller-to-host fallback copy statistics. */
	uint64_t			read_copy_ios;
	uint64_t			read_copy_bytes;
	uint64_t			zcopy_read_ios;
	uint64_t			zcopy_write_ios;
	uint64_t			zcopy_map_failures;

	/* Callback for qpair destruction */
	spdk_nvmf_transport_qpair_fini_cb	fini_cb_fn;
	void					*fini_cb_arg;

	TAILQ_ENTRY(spdk_nvmf_ub_qpair)		link;

	/* Control-plane identity of the socket that accepted this qpair. */
	struct spdk_nvme_transport_id		listen_trid;
};

/* UB transport resources - pre-allocated requests for a qpair */
struct spdk_nvmf_ub_resources {
	/* Array of size "max_queue_depth" containing UB requests. */
	struct spdk_nvmf_ub_request		*reqs;
	struct spdk_nvmf_ub_response		*responses;

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	union nvmf_c2h_msg			*cpls;
	uint32_t				depth;

	/* Queue to track free requests */
	STAILQ_HEAD(, spdk_nvmf_ub_request)	free_queue;
	/* Response SEND contexts are independent from request/receive credits. */
	STAILQ_HEAD(, spdk_nvmf_ub_response)	free_response_queue;
	/* Completed requests wait here only while every SEND context is busy. */
	STAILQ_HEAD(, spdk_nvmf_ub_request)	pending_response_queue;
	uint32_t				pending_response_count;
	uint32_t				pending_response_high_watermark;
};

struct spdk_nvmf_ub_port {
	struct spdk_nvme_transport_id	*trid;
	struct spdk_nvmf_transport		*transport;
	struct spdk_sock			*listen_sock;
	TAILQ_ENTRY(spdk_nvmf_ub_port)	link;
};

/* Pending connection waiting for connect request */
struct spdk_nvmf_ub_pending_conn {
	struct spdk_nvmf_ub_qpair		*uqpair;
	struct spdk_nvme_ub_oob_header		header;
	uint8_t					*request;
	size_t					req_offset;
};

static void
nvmf_ub_opts_init(struct spdk_nvmf_transport_opts *opts)
{
	opts->max_queue_depth =		SPDK_NVMF_UB_DEFAULT_MAX_QUEUE_DEPTH;
	opts->max_qpairs_per_ctrlr =	SPDK_NVMF_UB_DEFAULT_MAX_QPAIRS_PER_CTRLR;
	opts->in_capsule_data_size =	SPDK_NVMF_UB_DEFAULT_IN_CAPSULE_DATA_SIZE;
	opts->max_io_size =		SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE;
	opts->io_unit_size =		SPDK_NVMF_UB_MIN_IO_BUFFER_SIZE;
	opts->max_aq_depth =		SPDK_NVMF_UB_DEFAULT_AQ_DEPTH;
	opts->num_shared_buffers =	SPDK_NVMF_UB_DEFAULT_NUM_SHARED_BUFFERS;
	opts->buf_cache_size =		SPDK_NVMF_UB_DEFAULT_BUFFER_CACHE_SIZE;
	opts->dif_insert_or_strip =	SPDK_NVMF_UB_DIF_INSERT_OR_STRIP;
	opts->abort_timeout_sec =	SPDK_NVMF_UB_DEFAULT_ABORT_TIMEOUT_SEC;
	opts->transport_specific =		NULL;
	opts->data_wr_pool_size	=	SPDK_NVMF_UB_DEFAULT_DATA_WR_POOL_SIZE;
	opts->kas	=	0xFFFF;
}

static void
nvmf_ub_destroy(struct spdk_nvmf_transport *transport,
		  spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_ub_transport	*utransport;

	utransport = SPDK_CONTAINEROF(transport, struct spdk_nvmf_ub_transport, transport);

	if (utransport->mem_map != NULL) {
		spdk_mem_map_free(&utransport->mem_map);
	}

	free(utransport);

	if (cb_fn) {
		cb_fn(cb_arg);
	}
}

static int
nvmf_ub_get_eid_index(urma_device_t *dev)
{
	urma_eid_info_t *eid_list;
	const char *value;
	uint32_t eid_cnt;
	long eid_index;

	eid_list = urma_get_eid_list(dev, &eid_cnt);
	if (eid_list == NULL) {
		SPDK_ERRLOG("Failed to get EID list for URMA device %s\n", dev->name);
		return -ENODEV;
	}

	urma_free_eid_list(eid_list);

	if (eid_cnt == 0) {
		SPDK_ERRLOG("URMA device %s has no EIDs\n", dev->name);
		return -ENODEV;
	}

	value = getenv(URMA_EID_INDEX_ENV);
	if (value == NULL || value[0] == '\0') {
		return URMA_DEFAULT_EID_INDEX;
	}

	eid_index = spdk_strtol(value, 10);
	if (eid_index < 0 || eid_index > INT_MAX) {
		SPDK_ERRLOG("Invalid %s value '%s'; expected an integer between 0 and %d\n",
			    URMA_EID_INDEX_ENV, value, INT_MAX);
		return -EINVAL;
	}

	return (int)eid_index;
}

static int
nvmf_ub_create_urma(struct spdk_nvmf_ub_transport *utransport)
{
	urma_init_attr_t init_attr = {};
	int eid_index;
	int rc;

	if (g_nvmf_urma_initialized) {
		SPDK_NOTICELOG("URMA library already initialized\n");
		return 0;
	}

	SPDK_NOTICELOG("Initializing URMA library\n");

	/* Configure URMA initialization attributes */
	init_attr.uasid = 0; /* 0 means auto-assign by system */

	/* Initialize URMA library */
	rc = urma_init(&init_attr);
	if (rc == EEXIST) {
		/*
		 * URMA is process-wide and may already have been initialized by
		 * another transport or runtime.  Reuse the existing instance.
		 */
		SPDK_INFOLOG(ub, "URMA library was already initialized; reusing the existing instance\n");
	} else if (rc != URMA_SUCCESS) {
		SPDK_ERRLOG("Failed to initialize URMA library: %d (%s)\n",
			    rc, strerror(rc));
		return -1;
	}

	g_nvmf_urma_initialized = true;

	const char *dev_name = getenv(URMA_DEVICE_NAME_ENV);
	if (dev_name == NULL || dev_name[0] == '\0') {
		dev_name = URMA_DEFAULT_DEVICE_NAME;
	}

	urma_device_t *dev = urma_get_device_by_name(dev_name);
	if (dev == NULL) {
		SPDK_ERRLOG("Failed to get URMA device %s.\n", dev_name);
		return -1;
	}
	utransport->multi_path = nvmf_ub_device_uses_multipath(dev->name);

	urma_device_attr_t dev_attr;
	if (urma_query_device(dev, &dev_attr) != URMA_SUCCESS) {
		SPDK_ERRLOG("Failed to query device %s.\n", dev_name);
		return -1;
	}

	SPDK_INFOLOG(ub, "Using URMA device %s, multi_path=%d\n",
		       dev->name, utransport->multi_path);

	eid_index = nvmf_ub_get_eid_index(dev);
	if (eid_index < 0) {
		SPDK_ERRLOG("Failed to determine EID index\n");
		return -1;
	}
	SPDK_INFOLOG(ub, "Using EID index %d\n", eid_index);

	/* Create the URMA context using the selected endpoint. */
	utransport->urma_ctx = urma_create_context(dev, eid_index);
	if (utransport->urma_ctx == NULL) {
		SPDK_ERRLOG("Failed to create URMA context\n");
		return -1;
	}
	SPDK_NOTICELOG("Created URMA context successfully\n");

	/* Create JFCE (Jetty Completion Event Channel) - 参考 urma_server/client */
	utransport->jfce = urma_create_jfce(utransport->urma_ctx);
	if (utransport->jfce == NULL) {
		SPDK_ERRLOG("Failed to create JFCE\n");
		return -1;
	}
	SPDK_INFOLOG(ub, "URMA context ready: device=%s, EID index=%d\n",
		       dev->name, eid_index);
	return 0;
}

static struct spdk_nvmf_transport *
nvmf_ub_create(struct spdk_nvmf_transport_opts *opts)
{
	SPDK_NOTICELOG("*** UB Transport Init ***\n");
	struct spdk_iobuf_opts opts_iobuf = {};
	struct spdk_nvmf_ub_transport *utransport;
	uint32_t			sge_count;

	utransport = calloc(1, sizeof(*utransport));
	if (!utransport) {
		return NULL;
	}

	TAILQ_INIT(&utransport->ports);

	utransport->transport.ops = &spdk_nvmf_transport_ub;

	SPDK_INFOLOG(ub, "*** UB Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, abort_timeout_sec=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     opts->abort_timeout_sec);

	spdk_iobuf_get_opts(&opts_iobuf, sizeof(opts_iobuf));
	sge_count = opts->max_io_size / opts_iobuf.large_bufsize;
	if (sge_count > NVMF_DEFAULT_TX_SGE) {
		SPDK_ERRLOG("Unsupported max_io_size specified, %d bytes\n", opts->max_io_size);
		free(utransport);
		return NULL;
	}

	if (nvmf_ub_create_urma(utransport) != 0) {
		free(utransport);
		return NULL;
	}

	if (opts->zcopy) {
		utransport->mem_map = spdk_mem_map_alloc(0, &g_nvmf_ub_mem_map_ops, utransport);
		if (utransport->mem_map == NULL) {
			SPDK_WARNLOG("Unable to create UB zcopy memory map; disabling zcopy\n");
			opts->zcopy = false;
		}
	}
	return &utransport->transport;
}

static int
nvmf_ub_trsvcid_to_int(const char *trsvcid)
{
	unsigned long long ull;
	char *end = NULL;

	ull = strtoull(trsvcid, &end, 10);
	if (end == NULL || end == trsvcid || *end != '\0') {
		return -1;
	}

	/* Valid TCP/IP port numbers are in [1, 65535] */
	if (ull == 0 || ull > 65535) {
		return -1;
	}

	return (int)ull;
}

static int
nvmf_ub_create_jetty(struct spdk_nvmf_ub_qpair *uqpair, bool is_admin_qpair)
{
	struct spdk_nvmf_ub_transport *utransport;
	urma_token_t token = { .token = 0xABCD };

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);

	uqpair->depth = is_admin_qpair ? utransport->transport.opts.max_aq_depth :
			 utransport->transport.opts.max_queue_depth;

	/* Create a single shared JFC using transport's JFCE - 参考 urma_server/client */
	urma_jfc_cfg_t jfc_cfg = {
		/* Up to depth receive completions and 2 * depth JFS completions
		 * (response SENDs plus data READ/WRITEs) share this JFC. */
		.depth = uqpair->depth * 3,
		.jfce  = utransport->jfce,
	};

	uqpair->send_jfc = urma_create_jfc(utransport->urma_ctx, &jfc_cfg);
	if (uqpair->send_jfc == NULL) {
		SPDK_ERRLOG("urma_create_jfc failed for qid %u\n", uqpair->qid);
		return -1;
	}

	/* JFR configuration - 使用 URMA_TM_RM 模式，参考 urma_server/client */
	urma_jfr_cfg_t jfr_cfg = {
		.depth          = uqpair->depth,
		.trans_mode     = URMA_TM_RM,
		.min_rnr_timer  = URMA_TYPICAL_MIN_RNR_TIMER * 10,
		.jfc            = uqpair->send_jfc,
		.token_value    = token,
		.max_sge        = 1,
	};

	uqpair->jfr = urma_create_jfr(utransport->urma_ctx, &jfr_cfg);
	if (uqpair->jfr == NULL) {
		SPDK_ERRLOG("Failed to create jfr for qid %u\n", uqpair->qid);
		urma_delete_jfc(uqpair->send_jfc);
		uqpair->send_jfc = NULL;
		return -1;
	}

	/* JFS configuration - 使用 URMA_TM_RM 模式 */
	urma_jfs_cfg_t jfs_cfg = {
		/* A response SEND may still await its local completion after the host
		 * submits a replacement command that needs a data READ or WRITE. */
		.depth           = uqpair->depth * 2,
		.trans_mode      = URMA_TM_RM,
		.priority        = URMA_MAX_PRIORITY,
		.max_sge         = 1,
		.rnr_retry       = URMA_TYPICAL_RNR_RETRY,
		.err_timeout     = URMA_TYPICAL_ERR_TIMEOUT,
		.jfc             = uqpair->send_jfc,
	};
	jfs_cfg.flag.bs.multi_path = utransport->multi_path;

	urma_jetty_cfg_t jetty_cfg = {
		.flag.bs.share_jfr = 1,
		.jfs_cfg           = jfs_cfg,
		.shared.jfr        = uqpair->jfr,
	};

	uqpair->jetty = urma_create_jetty(utransport->urma_ctx, &jetty_cfg);
	if (uqpair->jetty == NULL) {
		urma_delete_jfr(uqpair->jfr);
		uqpair->jfr = NULL;
		urma_delete_jfc(uqpair->send_jfc);
		uqpair->send_jfc = NULL;
		SPDK_ERRLOG("Failed to create jetty\n");
		return -1;
	}

	SPDK_INFOLOG(ub, "Created UB jetty: qid=%u, depth=%u, jetty_id=%u\n",
		       uqpair->qid, uqpair->depth, uqpair->jetty->jetty_id.id);
	return 0;
}

static int
nvmf_ub_register_seg(struct spdk_nvmf_ub_qpair *uqpair)
{
	struct spdk_nvmf_ub_transport *utransport;
	urma_reg_seg_flag_t flag = {
		.bs.token_policy = URMA_TOKEN_NONE,
		.bs.cacheable = URMA_NON_CACHEABLE,
		.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
		.bs.token_id_valid = 0,
		.bs.reserved = 0
	};
	urma_token_t token = { .token = 0xABCD };
	urma_seg_cfg_t seg_cfg = {0};
	urma_sge_t src_sge = {0};
	urma_sg_t src_sg = {0};
	urma_jfr_wr_t *bad_wr = NULL;
	uint32_t i;

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);
	if (utransport == NULL || utransport->urma_ctx == NULL) {
		SPDK_ERRLOG("Invalid utransport or urma_ctx\n");
		return -1;
	}

	/* Keep receive capsules, response capsules, and per-request data in
	 * non-overlapping regions of one registered segment. */
	uqpair->rsp_offset = (size_t)uqpair->depth * MSG_SIZE;
	uqpair->data_offset = SPDK_ALIGN_CEIL(uqpair->rsp_offset +
					       (size_t)uqpair->depth * sizeof(union nvmf_c2h_msg), PAGE_SIZE);
	uqpair->seg_len = uqpair->data_offset +
			   (size_t)uqpair->depth * SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE;

	/*
	 * This segment is used both by URMA and as the payload for requests
	 * submitted to backend devices.  Allocate it from SPDK DMA memory so
	 * PCIe backends can translate it with spdk_vtophys().
	 */
	uqpair->va = spdk_dma_zmalloc(uqpair->seg_len, PAGE_SIZE, NULL);
	if (uqpair->va == NULL) {
		SPDK_ERRLOG("Failed to allocate DMA buffer for qid %u, len=%zu\n",
			    uqpair->qid, uqpair->seg_len);
		return -1;
	}

	seg_cfg.va = (uint64_t)(uintptr_t)uqpair->va;
	seg_cfg.len = uqpair->seg_len;
	seg_cfg.token_id = NULL;
	seg_cfg.token_value = token;
	seg_cfg.flag = flag;
	seg_cfg.user_ctx = 0x5678;
	seg_cfg.iova = 0;

	uqpair->local_tseg = urma_register_seg(utransport->urma_ctx, &seg_cfg);
	if (uqpair->local_tseg == NULL) {
		SPDK_ERRLOG("Failed to register UB segment for qid %u\n", uqpair->qid);
		spdk_dma_free(uqpair->va);
		uqpair->va = NULL;
		return -1;
	}

	for (i = 0; i < uqpair->depth; i++) {
		urma_jfr_wr_t wr = {0};
		src_sge.addr = (uint64_t)(uintptr_t)uqpair->va + (size_t)i * MSG_SIZE;
		src_sge.len = MSG_SIZE;
		src_sge.tseg = uqpair->local_tseg;
		src_sg.sge = &src_sge;
		src_sg.num_sge = 1;
		wr.src = src_sg;
		wr.user_ctx = i;
		wr.next = NULL;
		if (urma_post_jetty_recv_wr(uqpair->jetty, &wr, &bad_wr) != URMA_SUCCESS) {
			SPDK_ERRLOG("Failed to post initial recv WR %u for qid %u\n", i, uqpair->qid);
			urma_unregister_seg(uqpair->local_tseg);
			uqpair->local_tseg = NULL;
			spdk_dma_free(uqpair->va);
			uqpair->va = NULL;
			return -1;
		}
	}

	SPDK_INFOLOG(ub, "Registered UB segment: qid=%u, address=%p, length=%zu, recv_depth=%u\n",
		       uqpair->qid, uqpair->va, uqpair->seg_len, uqpair->depth);
	return 0;
}

static struct spdk_nvmf_ub_resources *
nvmf_ub_resources_create(struct spdk_nvmf_ub_qpair *uqpair, uint32_t max_queue_depth)
{
	struct spdk_nvmf_ub_resources *resources;
	struct spdk_nvmf_ub_request *ub_req;
	struct spdk_nvmf_ub_response *ub_rsp;
	uint32_t i;

	resources = calloc(1, sizeof(*resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate ub_resources for qpair %u\n", uqpair->qid);
		return NULL;
	}

	resources->reqs = spdk_zmalloc(max_queue_depth * sizeof(*resources->reqs),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->responses = calloc(max_queue_depth, sizeof(*resources->responses));
	resources->cpls = spdk_zmalloc(max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (!resources->reqs || !resources->responses || !resources->cpls) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for UB qpair %u\n", uqpair->qid);
		spdk_free(resources->reqs);
		free(resources->responses);
		spdk_free(resources->cpls);
		free(resources);
		return NULL;
	}

	STAILQ_INIT(&resources->free_queue);
	STAILQ_INIT(&resources->free_response_queue);
	STAILQ_INIT(&resources->pending_response_queue);
	resources->depth = max_queue_depth;

	for (i = 0; i < max_queue_depth; i++) {
		ub_req = &resources->reqs[i];

		/* Set up memory to send responses */
		ub_req->req.qpair = &uqpair->qpair;
		ub_req->req.cmd = NULL;
		ub_req->req.rsp = &resources->cpls[i];
		ub_req->req.iovcnt = 0;
		ub_req->req.length = 0;
		ub_req->req.stripped_data = NULL;
		ub_req->req.data_from_pool = false;
		ub_req->buf_idx = i;
		ub_req->recv_slot = NVMF_UB_INVALID_RECV_SLOT;

		STAILQ_INSERT_TAIL(&resources->free_queue, ub_req, link);

		ub_rsp = &resources->responses[i];
		ub_rsp->buf_idx = i;
		STAILQ_INSERT_TAIL(&resources->free_response_queue, ub_rsp, link);
	}

	SPDK_DEBUGLOG(ub, "Created UB resources for qpair %u with %u requests\n",
			      uqpair->qid, max_queue_depth);

	return resources;
}

static urma_target_seg_t *
nvmf_ub_import_remote_seg(struct spdk_nvmf_ub_qpair *uqpair, uint64_t addr, uint64_t length,
			  uint32_t attr, uint32_t token_id)
{
	urma_seg_t remote_seg = {0};
	urma_token_t token = { .token = 0xABCD };
	urma_import_seg_flag_t flag = {0};

	remote_seg.ubva.eid = uqpair->remote_eid;
	remote_seg.ubva.uasid = uqpair->remote_uasid;
	remote_seg.ubva.va = addr;
	remote_seg.len = length;
	remote_seg.attr.value = attr;
	remote_seg.token_id = token_id;

	flag.bs.cacheable = URMA_NON_CACHEABLE;
	flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
	flag.bs.mapping = URMA_SEG_NOMAP;

	return urma_import_seg(uqpair->jetty->urma_ctx, &remote_seg, &token, 0, flag);
}

static urma_target_seg_t *
nvmf_ub_get_remote_tseg(struct spdk_nvmf_ub_qpair *uqpair, uint64_t addr, uint32_t length,
			uint32_t token_id)
{
	struct spdk_nvmf_ub_remote_seg *entry;
	urma_target_seg_t *tseg;
	urma_seg_attr_t attr = {0};

	if (uqpair->remote_tseg != NULL && uqpair->remote_tseg->seg.token_id == token_id &&
	    spdk_nvme_ub_range_contains(uqpair->remote_tseg->seg.ubva.va,
					uqpair->remote_tseg->seg.len, addr, length)) {
		return uqpair->remote_tseg;
	}

	TAILQ_FOREACH(entry, &uqpair->remote_segs, link) {
		if (entry->token_id == token_id &&
		    spdk_nvme_ub_range_contains(entry->addr, entry->length, addr, length)) {
			return entry->tseg;
		}
	}

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}

	attr.bs.token_policy = URMA_TOKEN_NONE;
	attr.bs.cacheable = URMA_NON_CACHEABLE;
	tseg = nvmf_ub_import_remote_seg(uqpair, addr, length, attr.value, token_id);
	if (tseg == NULL) {
		free(entry);
		return NULL;
	}

	entry->token_id = token_id;
	entry->addr = addr;
	entry->length = length;
	entry->tseg = tseg;
	TAILQ_INSERT_TAIL(&uqpair->remote_segs, entry, link);
	return tseg;
}

static urma_target_seg_t *
nvmf_ub_get_local_tseg(struct spdk_nvmf_ub_qpair *uqpair, void *addr, size_t length)
{
	struct spdk_nvmf_ub_transport *utransport;
	uint64_t translated_length = length;
	urma_target_seg_t *tseg;

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);
	if (utransport->mem_map == NULL || addr == NULL || length == 0) {
		return NULL;
	}

	tseg = (urma_target_seg_t *)(uintptr_t)
		spdk_mem_map_translate(utransport->mem_map, (uint64_t)(uintptr_t)addr,
				       &translated_length);
	if (tseg == NULL || translated_length < length) {
		return NULL;
	}

	return tseg;
}

static void
nvmf_ub_unimport_remote_segs(struct spdk_nvmf_ub_qpair *uqpair)
{
	struct spdk_nvmf_ub_remote_seg *entry, *tmp;

	if (uqpair->remote_tseg != NULL) {
		urma_unimport_seg(uqpair->remote_tseg);
		uqpair->remote_tseg = NULL;
	}

	TAILQ_FOREACH_SAFE(entry, &uqpair->remote_segs, link, tmp) {
		TAILQ_REMOVE(&uqpair->remote_segs, entry, link);
		urma_unimport_seg(entry->tseg);
		free(entry);
	}
}

static void
nvmf_ub_unimport_npu_resources(struct spdk_nvmf_ub_qpair *uqpair)
{
	struct spdk_nvmf_ub_npu_endpoint *endpoint, *endpoint_tmp;
	struct spdk_nvmf_ub_npu_region *region, *region_tmp;

	TAILQ_FOREACH_SAFE(region, &uqpair->npu_regions, link, region_tmp) {
		TAILQ_REMOVE(&uqpair->npu_regions, region, link);
		if (region->tseg != NULL) {
			urma_unimport_seg(region->tseg);
		}
		free(region);
	}
	TAILQ_FOREACH_SAFE(endpoint, &uqpair->npu_endpoints, link, endpoint_tmp) {
		TAILQ_REMOVE(&uqpair->npu_endpoints, endpoint, link);
		if (endpoint->tjetty != NULL) {
			urma_unimport_jetty(endpoint->tjetty);
		}
		free(endpoint);
	}
}


static struct spdk_nvmf_ub_npu_endpoint *
nvmf_ub_find_npu_endpoint(struct spdk_nvmf_ub_qpair *uqpair, uint32_t endpoint_id)
{
	struct spdk_nvmf_ub_npu_endpoint *endpoint;

	TAILQ_FOREACH(endpoint, &uqpair->npu_endpoints, link) {
		if (endpoint->endpoint_id == endpoint_id) {
			return endpoint;
		}
	}

	return NULL;
}

static struct spdk_nvmf_ub_npu_region *
nvmf_ub_find_npu_region(struct spdk_nvmf_ub_qpair *uqpair, uint32_t region_id)
{
	struct spdk_nvmf_ub_npu_region *region;

	TAILQ_FOREACH(region, &uqpair->npu_regions, link) {
		if (region->region_id == region_id) {
			return region;
		}
	}

	return NULL;
}

static int
nvmf_ub_import_npu_resources(struct spdk_nvmf_ub_qpair *uqpair,
			     const struct spdk_nvme_ub_oob_header *header,
			     const uint8_t *cursor, const uint8_t *end)
{
	struct spdk_nvmf_ub_transport *utransport;
	urma_import_seg_flag_t import_flag = {};
	uint32_t i;

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);
	if (header->qid == 0 && (header->endpoint_count != 0 || header->region_count != 0)) {
		return -EPROTO;
	}

	for (i = 0; i < header->endpoint_count; i++) {
		const struct spdk_nvme_ub_oob_endpoint *wire;
		struct spdk_nvmf_ub_npu_endpoint *endpoint;
		urma_token_t token;
		void *context;

		if ((size_t)(end - cursor) < sizeof(*wire)) {
			return -EPROTO;
		}
		wire = (const void *)cursor;
		if (wire->endpoint_id == 0 ||
		    wire->rjetty_context_size < sizeof(urma_rjetty_t) ||
		    wire->rjetty_context_size > SPDK_NVME_UB_OOB_MAX_CONTEXT_SIZE ||
		    wire->record_size != sizeof(*wire) + wire->rjetty_context_size ||
		    wire->record_size > (size_t)(end - cursor) ||
		    nvmf_ub_find_npu_endpoint(uqpair, wire->endpoint_id) != NULL) {
			return -EPROTO;
		}

		endpoint = calloc(1, sizeof(*endpoint));
		context = malloc(wire->rjetty_context_size);
		if (endpoint == NULL || context == NULL) {
			free(endpoint);
			free(context);
			return -ENOMEM;
		}
		memcpy(context, wire + 1, wire->rjetty_context_size);
		token.token = wire->token;
		endpoint->tjetty = urma_import_jetty(utransport->urma_ctx, context, &token);
		free(context);
		if (endpoint->tjetty == NULL) {
			free(endpoint);
			SPDK_ERRLOG("Failed to import NPU endpoint %u: errno=%d (%s)\n",
				    wire->endpoint_id, errno, strerror(errno));
			return -EIO;
		}
		endpoint->endpoint_id = wire->endpoint_id;
		TAILQ_INSERT_TAIL(&uqpair->npu_endpoints, endpoint, link);
		cursor += wire->record_size;
	}

	import_flag.bs.cacheable = URMA_NON_CACHEABLE;
	import_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
	import_flag.bs.mapping = URMA_SEG_NOMAP;
	for (i = 0; i < header->region_count; i++) {
		const struct spdk_nvme_ub_oob_region *wire;
		struct spdk_nvmf_ub_npu_endpoint *endpoint;
		struct spdk_nvmf_ub_npu_region *region;
		const urma_seg_t *remote_seg;
		urma_token_t token;
		void *context;

		if ((size_t)(end - cursor) < sizeof(*wire)) {
			return -EPROTO;
		}
		wire = (const void *)cursor;
		endpoint = nvmf_ub_find_npu_endpoint(uqpair, wire->endpoint_id);
		if (wire->region_id == 0 || endpoint == NULL || wire->length == 0 ||
		    wire->remote_base > UINT64_MAX - wire->length ||
		    wire->segment_context_size < sizeof(urma_seg_t) ||
		    wire->segment_context_size > SPDK_NVME_UB_OOB_MAX_CONTEXT_SIZE ||
		    wire->record_size != sizeof(*wire) + wire->segment_context_size ||
		    wire->record_size > (size_t)(end - cursor) ||
		    nvmf_ub_find_npu_region(uqpair, wire->region_id) != NULL) {
			return -EPROTO;
		}

		context = malloc(wire->segment_context_size);
		region = calloc(1, sizeof(*region));
		if (context == NULL || region == NULL) {
			free(context);
			free(region);
			return -ENOMEM;
		}
		memcpy(context, wire + 1, wire->segment_context_size);
		remote_seg = context;
		if (remote_seg->ubva.va != wire->remote_base || remote_seg->len < wire->length) {
			free(context);
			free(region);
			return -EPROTO;
		}

		token.token = wire->token;
		region->tseg = urma_import_seg(utransport->urma_ctx, context, &token, 0,
					     import_flag);
		free(context);
		if (region->tseg == NULL) {
			SPDK_ERRLOG("Failed to import NPU region %u: errno=%d (%s)\n",
				    wire->region_id, errno, strerror(errno));
			free(region);
			return -EIO;
		}
		region->region_id = wire->region_id;
		region->endpoint_id = wire->endpoint_id;
		region->remote_base = wire->remote_base;
		region->length = wire->length;
		region->tjetty = endpoint->tjetty;
		TAILQ_INSERT_TAIL(&uqpair->npu_regions, region, link);
		cursor += wire->record_size;
	}

	return cursor == end ? 0 : -EPROTO;
}

static int
nvmf_ub_sock_write_all(struct spdk_sock *sock, const void *buf, size_t length)
{
	uint64_t deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();
	size_t offset = 0;

	while (offset < length) {
		struct iovec iov = {
			.iov_base = (uint8_t *)buf + offset,
			.iov_len = length - offset,
		};
		ssize_t rc = spdk_sock_writev(sock, &iov, 1);

		if (rc > 0) {
			offset += rc;
			continue;
		}
		if (rc < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			return -errno;
		}
		if (spdk_get_ticks() >= deadline) {
			return -ETIMEDOUT;
		}
		spdk_delay_us(100);
	}

	return 0;
}

static int
nvmf_ub_send_connect_response(struct spdk_nvmf_ub_qpair *uqpair, struct spdk_sock *sock,
			      uint16_t qid, int status, uint64_t registry_generation)
{
	uint8_t response[sizeof(struct spdk_nvme_ub_oob_header) +
			 sizeof(struct spdk_nvme_ub_oob_cpu_info)] = {};
	struct spdk_nvme_ub_oob_header *header = (void *)response;
	struct spdk_nvme_ub_oob_cpu_info *cpu = (void *)(header + 1);

	header->magic = SPDK_NVME_UB_OOB_MAGIC;
	header->version = SPDK_NVME_UB_OOB_VERSION;
	header->msg_type = SPDK_NVME_UB_OOB_CONNECT_RSP;
	header->length = sizeof(response);
	header->status = status;
	header->qid = qid;
	header->registry_generation = registry_generation;

	if (status == 0) {
		memcpy(cpu->seg_eid, uqpair->local_tseg->seg.ubva.eid.raw,
		       SPDK_NVME_UB_EID_SIZE);
		cpu->seg_uasid = uqpair->local_tseg->seg.ubva.uasid;
		cpu->seg_va = uqpair->local_tseg->seg.ubva.va;
		cpu->seg_len = uqpair->local_tseg->seg.len;
		cpu->seg_flag = uqpair->local_tseg->seg.attr.value;
		cpu->seg_token_id = uqpair->local_tseg->seg.token_id;
		memcpy(cpu->jetty_eid, uqpair->jetty->jetty_id.eid.raw,
		       SPDK_NVME_UB_EID_SIZE);
		cpu->jetty_uasid = uqpair->jetty->jetty_id.uasid;
		cpu->jetty_id = uqpair->jetty->jetty_id.id;
		cpu->trans_mode = URMA_TM_RM;
	}

	return nvmf_ub_sock_write_all(sock, response, sizeof(response));
}

static int
nvmf_ub_handle_connect_v2(struct spdk_nvmf_ub_qpair *uqpair, const uint8_t *request,
			  struct spdk_sock *sock)
{
	const struct spdk_nvme_ub_oob_header *header = (const void *)request;
	const struct spdk_nvme_ub_oob_cpu_info *cpu = (const void *)(header + 1);
	struct spdk_nvmf_ub_transport *utransport;
	urma_rjetty_t rjetty = {};
	urma_token_t token = { .token = 0xABCD };
	const uint8_t *cursor, *end;
	int rc;

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);
	uqpair->qid = header->qid;
	uqpair->remote_uasid = cpu->seg_uasid;
	memcpy(uqpair->remote_eid.raw, cpu->seg_eid, SPDK_NVME_UB_EID_SIZE);
	memcpy(uqpair->remote_jetty_id.eid.raw, cpu->jetty_eid, SPDK_NVME_UB_EID_SIZE);
	uqpair->remote_jetty_id.uasid = cpu->jetty_uasid;
	uqpair->remote_jetty_id.id = cpu->jetty_id;
	if (cpu->trans_mode != URMA_TM_RM || cpu->seg_len == 0 ||
	    cpu->seg_va > UINT64_MAX - cpu->seg_len) {
		rc = -EPROTO;
		goto error;
	}

	rc = nvmf_ub_create_jetty(uqpair, uqpair->qid == 0);
	if (rc != 0) {
		goto error;
	}
	rc = nvmf_ub_register_seg(uqpair);
	if (rc != 0) {
		goto error;
	}

	rjetty.jetty_id = uqpair->remote_jetty_id;
	rjetty.trans_mode = URMA_TM_RM;
	rjetty.type = URMA_JETTY;
	rjetty.tp_type = URMA_CTP;
	uqpair->target_jetty = urma_import_jetty(utransport->urma_ctx, &rjetty, &token);
	if (uqpair->target_jetty == NULL) {
		SPDK_ERRLOG("Failed to import remote jetty for qid %u: errno=%d (%s)\n",
			    uqpair->qid, errno, strerror(errno));
		rc = -EIO;
		goto error;
	}

	uqpair->remote_tseg = nvmf_ub_import_remote_seg(uqpair, cpu->seg_va, cpu->seg_len,
						      cpu->seg_flag, cpu->seg_token_id);
	if (uqpair->remote_tseg == NULL) {
		SPDK_WARNLOG("Unable to pre-import initiator CPU segment for qid %u\n", uqpair->qid);
	}

	cursor = (const uint8_t *)(cpu + 1);
	end = request + header->length;
	rc = nvmf_ub_import_npu_resources(uqpair, header, cursor, end);
	if (rc != 0) {
		goto error;
	}

	uqpair->resources = nvmf_ub_resources_create(uqpair, uqpair->depth);
	if (uqpair->resources == NULL) {
		rc = -ENOMEM;
		goto error;
	}

	rc = nvmf_ub_send_connect_response(uqpair, sock, uqpair->qid, 0,
					   header->registry_generation);
	if (rc != 0) {
		goto error;
	}

	spdk_nvmf_tgt_new_qpair(utransport->transport.tgt, &uqpair->qpair);
	uqpair->state = UB_QPAIR_STATE_RUNNING;
	SPDK_NOTICELOG("UB qpair connected: qid=%u, remote_jetty_id=%u, NPU endpoints=%u, "
		       "NPU regions=%u, generation=%" PRIu64 "\n",
		       uqpair->qid, uqpair->remote_jetty_id.id, header->endpoint_count,
		       header->region_count, header->registry_generation);
	return 0;

error:
	if (rc >= 0) {
		rc = -EIO;
	}
	if (nvmf_ub_send_connect_response(uqpair, sock, header->qid, rc,
					  header->registry_generation) != 0) {
		SPDK_ERRLOG("Failed to send OOB v2 error response for qid %u\n", header->qid);
	}
	return rc;
}

static void
nvmf_ub_resources_destroy(struct spdk_nvmf_ub_resources *resources)
{
	if (!resources) {
		return;
	}

	SPDK_DEBUGLOG(ub, "Destroying UB resources: pending responses=%u, high watermark=%u\n",
		      resources->pending_response_count,
		      resources->pending_response_high_watermark);

	spdk_free(resources->cpls);
	free(resources->responses);
	spdk_free(resources->reqs);
	free(resources);
}

static int
nvmf_ub_qpair_destroy(struct spdk_nvmf_ub_qpair *uqpair)
{
	SPDK_INFOLOG(ub, "UB qpair %u payload stats: read_copy_ios=%" PRIu64
		       " read_copy_bytes=%" PRIu64 " zcopy_read_ios=%" PRIu64
		       " zcopy_write_ios=%" PRIu64 " zcopy_map_failures=%" PRIu64 "\n",
		       uqpair->qid, uqpair->read_copy_ios, uqpair->read_copy_bytes,
		       uqpair->zcopy_read_ios, uqpair->zcopy_write_ios,
		       uqpair->zcopy_map_failures);

	nvmf_ub_unimport_remote_segs(uqpair);
	nvmf_ub_unimport_npu_resources(uqpair);

	if (uqpair->target_jetty) {
		urma_unimport_jetty(uqpair->target_jetty);
		uqpair->target_jetty = NULL;
	}

	if (uqpair->jetty) {
		urma_delete_jetty(uqpair->jetty);
		uqpair->jetty = NULL;
	}

	if (uqpair->jfr) {
		urma_delete_jfr(uqpair->jfr);
		uqpair->jfr = NULL;
	}

	if (uqpair->send_jfc) {
		urma_delete_jfc(uqpair->send_jfc);
		uqpair->send_jfc = NULL;
	}

	if (uqpair->local_tseg) {
		urma_unregister_seg(uqpair->local_tseg);
		uqpair->local_tseg = NULL;
	}

	spdk_dma_free(uqpair->va);
	uqpair->va = NULL;

	if (uqpair->resources) {
		nvmf_ub_resources_destroy(uqpair->resources);
		uqpair->resources = NULL;
	}

	free(uqpair);

	return 0;
}

static void
_nvmf_ub_qpair_destroy(void *ctx)
{
	struct spdk_nvmf_ub_qpair *uqpair = ctx;
	spdk_nvmf_transport_qpair_fini_cb cb_fn = uqpair->fini_cb_fn;
	void *cb_arg = uqpair->fini_cb_arg;

	uqpair->state = UB_QPAIR_STATE_DISCONNECTED;
	nvmf_ub_qpair_destroy(uqpair);

	if (cb_fn != NULL) {
		cb_fn(cb_arg);
	}
}

static void
nvmf_ub_close_qpair(struct spdk_nvmf_qpair *qpair,
			spdk_nvmf_transport_qpair_fini_cb cb_fn, void *cb_arg)
{
	struct spdk_nvmf_ub_qpair *uqpair;

	uqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_ub_qpair, qpair);
	assert(uqpair->fini_cb_fn == NULL);
	uqpair->fini_cb_fn = cb_fn;
	uqpair->fini_cb_arg = cb_arg;
	uqpair->state = UB_QPAIR_STATE_DISCONNECTING;

	/* qpair_fini can be reached from a completion callback.  Defer the
	 * actual destruction so the current UB poll pass cannot access a freed
	 * qpair or request after spdk_nvmf_qpair_disconnect() returns. */
	spdk_thread_send_msg(spdk_get_thread(), _nvmf_ub_qpair_destroy, uqpair);
}

static void
nvmf_ub_pending_sock_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock);

static void
nvmf_ub_handle_accept(struct spdk_nvmf_ub_port *port, struct spdk_sock *sock)
{
	struct spdk_nvmf_transport *transport = port->transport;
	struct spdk_nvmf_ub_transport *utransport;
	struct spdk_nvmf_ub_pending_conn *pending;
	int rc;

	utransport = nvmf_ub_get_transport(transport);

	pending = calloc(1, sizeof(*pending));
	if (pending == NULL) {
		SPDK_ERRLOG("Failed to allocate pending_conn\n");
		spdk_sock_close(&sock);
		return;
	}

	pending->uqpair = calloc(1, sizeof(*pending->uqpair));
	if (pending->uqpair == NULL) {
		SPDK_ERRLOG("Failed to allocate ub_qpair\n");
		free(pending);
		spdk_sock_close(&sock);
		return;
	}

	pending->uqpair->state = UB_QPAIR_STATE_CONNECTING;
	pending->uqpair->qpair.state = SPDK_NVMF_QPAIR_CONNECTING;
	pending->uqpair->qpair.transport = transport;
	pending->uqpair->listen_trid = *port->trid;
	TAILQ_INIT(&pending->uqpair->remote_segs);
	TAILQ_INIT(&pending->uqpair->npu_endpoints);
	TAILQ_INIT(&pending->uqpair->npu_regions);
	pending->req_offset = 0;

	rc = spdk_sock_group_add_sock(utransport->pending_sock_group, sock,
				      nvmf_ub_pending_sock_cb, pending);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_add_sock failed: %d\n", rc);
		free(pending->uqpair);
		free(pending);
		spdk_sock_close(&sock);
		return;
	}

	SPDK_INFOLOG(ub, "Accepted UB connection on %s:%s; waiting for OOB request\n",
		       port->trid->traddr, port->trid->trsvcid);
}

static int
nvmf_ub_pending_poll(void *ctx)
{
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_ub_transport *utransport;
	int rc;

	utransport = nvmf_ub_get_transport(transport);

	if (utransport->pending_sock_group == NULL) {
		return SPDK_POLLER_IDLE;
	}

	rc = spdk_sock_group_poll(utransport->pending_sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_poll failed: %d\n", rc);
		return SPDK_POLLER_IDLE;
	}

	return SPDK_POLLER_BUSY;
}


static void
nvmf_ub_pending_sock_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_ub_pending_conn *pending = ctx;
	void *destination;
	size_t remaining;
	ssize_t ret;
	int rc;

	if (pending->request == NULL) {
		destination = (uint8_t *)&pending->header + pending->req_offset;
		remaining = sizeof(pending->header) - pending->req_offset;
	} else {
		destination = pending->request + pending->req_offset;
		remaining = pending->header.length - pending->req_offset;
	}

	ret = spdk_sock_recv(sock, destination, remaining);
	if (ret < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			return;
		}
		SPDK_ERRLOG("Failed to receive OOB v2 request: errno=%d (%s)\n",
			    errno, strerror(errno));
		goto cleanup;
	}
	if (ret == 0) {
		SPDK_ERRLOG("OOB socket closed by peer\n");
		goto cleanup;
	}
	pending->req_offset += ret;

	if (pending->request == NULL && pending->req_offset == sizeof(pending->header)) {
		if (pending->header.magic != SPDK_NVME_UB_OOB_MAGIC ||
		    pending->header.version != SPDK_NVME_UB_OOB_VERSION ||
		    pending->header.msg_type != SPDK_NVME_UB_OOB_CONNECT ||
		    pending->header.status != 0 ||
		    pending->header.length < sizeof(pending->header) +
		    sizeof(struct spdk_nvme_ub_oob_cpu_info) ||
		    pending->header.length > SPDK_NVME_UB_OOB_MAX_SIZE ||
		    pending->header.endpoint_count > SPDK_NVME_UB_OOB_MAX_ENDPOINTS ||
		    pending->header.region_count > SPDK_NVME_UB_OOB_MAX_REGIONS) {
			SPDK_ERRLOG("Invalid OOB v2 header\n");
			goto cleanup;
		}

		pending->request = malloc(pending->header.length);
		if (pending->request == NULL) {
			goto cleanup;
		}
		memcpy(pending->request, &pending->header, sizeof(pending->header));
		if (pending->req_offset < pending->header.length) {
			return;
		}
	}

	if (pending->request == NULL || pending->req_offset < pending->header.length) {
		return;
	}
	if (pending->req_offset != pending->header.length) {
		goto cleanup;
	}

	rc = nvmf_ub_handle_connect_v2(pending->uqpair, pending->request, sock);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to handle OOB v2 connect: %d\n", rc);
		goto cleanup;
	}

	rc = spdk_sock_group_remove_sock(group, sock);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to remove connected UB socket from pending group: %d\n", rc);
	}
	spdk_sock_close(&sock);
	free(pending->request);
	free(pending);
	return;

cleanup:
	rc = spdk_sock_group_remove_sock(group, sock);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to remove UB socket from pending group: %d\n", rc);
	}
	nvmf_ub_qpair_destroy(pending->uqpair);
	free(pending->request);
	free(pending);
	spdk_sock_close(&sock);
}

static void
nvmf_ub_accept_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *listen_sock)
{
	struct spdk_nvmf_ub_port *port = ctx;
	struct spdk_sock *sock;
	int i;

	assert(port->listen_sock == listen_sock);

	for (i = 0; i < SPDK_NVMF_UB_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(listen_sock);
		if (sock == NULL) {
			break;
		}
		nvmf_ub_handle_accept(port, sock);
	}
}

static int
nvmf_ub_mem_map_notify(void *cb_ctx, struct spdk_mem_map *map,
		       enum spdk_mem_map_notify_action action, void *vaddr, size_t size)
{
	struct spdk_nvmf_ub_transport *utransport = cb_ctx;
	urma_target_seg_t *tseg;
	int rc;

	switch (action) {
	case SPDK_MEM_MAP_NOTIFY_REGISTER: {
		urma_token_t token = { .token = 0xABCD };
		urma_reg_seg_flag_t flag = {
			.bs.token_policy = URMA_TOKEN_NONE,
			.bs.cacheable = URMA_NON_CACHEABLE,
			.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
			.bs.token_id_valid = 0,
		};
		urma_seg_cfg_t seg_cfg = {
			.va = (uint64_t)(uintptr_t)vaddr,
			.len = size,
			.token_value = token,
			.flag = flag,
		};

		tseg = urma_register_seg(utransport->urma_ctx, &seg_cfg);
		if (tseg == NULL) {
			SPDK_WARNLOG("Unable to register zcopy memory %p/%zu with URMA\n",
				     vaddr, size);
			return -EFAULT;
		}

		rc = spdk_mem_map_set_translation(map, (uint64_t)(uintptr_t)vaddr, size,
					  (uint64_t)(uintptr_t)tseg);
		if (rc != 0) {
			urma_unregister_seg(tseg);
		}
		return rc;
	}
	case SPDK_MEM_MAP_NOTIFY_UNREGISTER:
		tseg = (urma_target_seg_t *)(uintptr_t)
			spdk_mem_map_translate(map, (uint64_t)(uintptr_t)vaddr, NULL);
		if (tseg != NULL && urma_unregister_seg(tseg) != URMA_SUCCESS) {
			SPDK_WARNLOG("Unable to unregister zcopy memory %p/%zu from URMA\n",
				     vaddr, size);
		}
		return spdk_mem_map_clear_translation(map, (uint64_t)(uintptr_t)vaddr, size);
	default:
		SPDK_UNREACHABLE();
	}

	return -EINVAL;
}

static int
nvmf_ub_mem_map_are_contiguous(uint64_t addr_1, uint64_t addr_2)
{
	return addr_1 == addr_2;
}

static const struct spdk_mem_map_ops g_nvmf_ub_mem_map_ops = {
	.notify_cb = nvmf_ub_mem_map_notify,
	.are_contiguous = nvmf_ub_mem_map_are_contiguous,
};

static int
nvmf_ub_accept(void *ctx)
{
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_ub_transport *utransport = nvmf_ub_get_transport(transport);
	int rc;

	rc = spdk_sock_group_poll(utransport->listen_sock_group);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to poll UB listen sock group: %d\n", rc);
		return SPDK_POLLER_IDLE;
	}

	return rc > 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static struct spdk_nvmf_ub_port *
nvmf_ub_find_port(struct spdk_nvmf_ub_transport *utransport, const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_ub_port *port;

	TAILQ_FOREACH(port, &utransport->ports, link) {
		if (strcmp(port->trid->traddr, trid->traddr) == 0 &&
		    strcmp(port->trid->trsvcid, trid->trsvcid) == 0) {
			return port;
		}
	}

	return NULL;
}

static int
nvmf_ub_listen(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		 struct spdk_nvmf_listen_opts *listen_opts)
{
	SPDK_NOTICELOG("*** nvmf_ub_listen ***\n");
	struct spdk_nvmf_ub_transport *utransport;
	struct spdk_nvmf_ub_port *port;
	int trsvcid_int;
	bool connection_infra_created = false;
	int rc;

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -1;
	}

	utransport = nvmf_ub_get_transport(transport);
	if (nvmf_ub_find_port(utransport, trid) != NULL) {
		return 0;
	}

	trsvcid_int = nvmf_ub_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		SPDK_ERRLOG("Invalid trsvcid '%s'\n", trid->trsvcid);
		return -1;
	}

	if (utransport->pending_sock_group == NULL) {
		utransport->listen_sock_group = spdk_sock_group_create(NULL);
		if (utransport->listen_sock_group == NULL) {
			SPDK_ERRLOG("spdk_sock_group_create for listeners failed\n");
			return -1;
		}

		utransport->pending_sock_group = spdk_sock_group_create(NULL);
		if (utransport->pending_sock_group == NULL) {
			SPDK_ERRLOG("spdk_sock_group_create for pending failed\n");
			spdk_sock_group_close(&utransport->listen_sock_group);
			utransport->listen_sock_group = NULL;
			return -1;
		}

		utransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_ub_accept, transport, 0);
		if (utransport->accept_poller == NULL) {
			SPDK_ERRLOG("Failed to register UB accept poller\n");
			spdk_sock_group_close(&utransport->pending_sock_group);
			spdk_sock_group_close(&utransport->listen_sock_group);
			utransport->pending_sock_group = NULL;
			utransport->listen_sock_group = NULL;
			return -1;
		}

		utransport->pending_poller = SPDK_POLLER_REGISTER(nvmf_ub_pending_poll, transport, 0);
		if (utransport->pending_poller == NULL) {
			SPDK_ERRLOG("Failed to register UB pending connection poller\n");
			spdk_poller_unregister(&utransport->accept_poller);
			spdk_sock_group_close(&utransport->pending_sock_group);
			spdk_sock_group_close(&utransport->listen_sock_group);
			utransport->pending_sock_group = NULL;
			utransport->listen_sock_group = NULL;
			return -1;
		}
		connection_infra_created = true;
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		goto cleanup_connection_infra;
	}

	port->trid = malloc(sizeof(*port->trid));
	if (port->trid == NULL) {
		SPDK_ERRLOG("Failed to allocate trid copy\n");
		free(port);
		goto cleanup_connection_infra;
	}
	*port->trid = *trid;
	port->transport = transport;
	port->listen_sock = spdk_sock_listen(trid->traddr, trsvcid_int, NULL);
	if (port->listen_sock == NULL) {
		SPDK_ERRLOG("spdk_sock_listen(%s, %d) failed\n", trid->traddr, trsvcid_int);
		free(port->trid);
		free(port);
		goto cleanup_connection_infra;
	}

	rc = spdk_sock_group_add_sock(utransport->listen_sock_group, port->listen_sock,
				      nvmf_ub_accept_cb, port);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to add UB listen socket %s:%s to sock group: %d\n",
			    trid->traddr, trid->trsvcid, rc);
		spdk_sock_close(&port->listen_sock);
		free(port->trid);
		free(port);
		goto cleanup_connection_infra;
	}

	TAILQ_INSERT_TAIL(&utransport->ports, port, link);
	SPDK_NOTICELOG("NVMe/UB target listening on %s:%s\n",
		       trid->traddr, trid->trsvcid);

	return 0;

cleanup_connection_infra:
	if (connection_infra_created) {
		spdk_poller_unregister(&utransport->accept_poller);
		spdk_poller_unregister(&utransport->pending_poller);
		spdk_sock_group_close(&utransport->pending_sock_group);
		spdk_sock_group_close(&utransport->listen_sock_group);
		utransport->pending_sock_group = NULL;
		utransport->listen_sock_group = NULL;
	}
	return -1;
}

static void
nvmf_ub_stop_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	SPDK_NOTICELOG("*** nvmf_ub_stop_listen ***\n");
	struct spdk_nvmf_ub_transport *utransport;
	struct spdk_nvmf_ub_port *port;
	int rc;

	utransport = nvmf_ub_get_transport(transport);

	port = nvmf_ub_find_port(utransport, trid);
	if (port) {
		rc = spdk_sock_group_remove_sock(utransport->listen_sock_group, port->listen_sock);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to remove UB listen socket %s:%s from sock group: %d\n",
				    trid->traddr, trid->trsvcid, rc);
		}
		spdk_sock_close(&port->listen_sock);
		TAILQ_REMOVE(&utransport->ports, port, link);
		free(port->trid);
		free(port);
		SPDK_NOTICELOG("NVMe/UB target stopped listening on %s:%s\n",
			       trid->traddr, trid->trsvcid);
	}

	if (TAILQ_EMPTY(&utransport->ports)) {
		if (utransport->accept_poller) {
			spdk_poller_unregister(&utransport->accept_poller);
			utransport->accept_poller = NULL;
		}
		if (utransport->pending_poller) {
			spdk_poller_unregister(&utransport->pending_poller);
			utransport->pending_poller = NULL;
		}
		if (utransport->pending_sock_group) {
			spdk_sock_group_close(&utransport->pending_sock_group);
			utransport->pending_sock_group = NULL;
		}
		if (utransport->listen_sock_group) {
			spdk_sock_group_close(&utransport->listen_sock_group);
			utransport->listen_sock_group = NULL;
		}
	}
}


static void
nvmf_ub_cdata_init(struct spdk_nvmf_transport *transport, struct spdk_nvmf_subsystem *subsystem,
		     struct spdk_nvmf_ctrlr_data *cdata)
{
	SPDK_NOTICELOG("*** nvmf_ub_cdata_init ***\n");
	cdata->nvmf_specific.msdbd = 1;
	cdata->nvmf_specific.ioccsz = sizeof(struct spdk_nvme_cmd) / 16;
}

static void
nvmf_ub_discover(struct spdk_nvmf_transport *transport,
		   struct spdk_nvme_transport_id *trid,
		   struct spdk_nvmf_discovery_log_page_entry *entry)
{
	SPDK_NOTICELOG("*** nvmf_ub_discover ***\n");
	entry->trtype = SPDK_NVMF_TRTYPE_UB;
	entry->adrfam = trid->adrfam;
	entry->treq.secure_channel = SPDK_NVMF_TREQ_SECURE_CHANNEL_NOT_REQUIRED;

	spdk_strcpy_pad(entry->trsvcid, trid->trsvcid, sizeof(entry->trsvcid), ' ');
	spdk_strcpy_pad(entry->traddr, trid->traddr, sizeof(entry->traddr), ' ');

	/* UB transport-specific address subtype - currently no specific fields needed */
	memset(entry->tsas.raw, 0, sizeof(entry->tsas.raw));
}

static struct spdk_nvmf_transport_poll_group *
nvmf_ub_poll_group_create(struct spdk_nvmf_transport *transport,
			    struct spdk_nvmf_poll_group *group)
{
	SPDK_NOTICELOG("*** nvmf_ub_poll_group_create ***\n");
	struct spdk_nvmf_ub_poll_group	*ugroup;

	ugroup = calloc(1, sizeof(*ugroup));
	if (!ugroup) {
		return NULL;
	}

	#define NVMF_UB_MAX_POLL_CRS  16
	ugroup->max_crs = NVMF_UB_MAX_POLL_CRS;
	ugroup->crs = calloc(ugroup->max_crs, sizeof(urma_cr_t));

	if (!ugroup->crs) {
		SPDK_ERRLOG("UB poll_group_create: crs allocation failed\n");
		return NULL;
	}

	TAILQ_INIT(&ugroup->qpairs);

	return &ugroup->group;
}

static void
nvmf_ub_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	SPDK_NOTICELOG("*** nvmf_ub_poll_group_destroy ***\n");
	struct spdk_nvmf_ub_poll_group	*ugroup;

	ugroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_ub_poll_group, group);
	free(ugroup->crs);
	free(ugroup);
}

static int
nvmf_ub_poll_group_add(struct spdk_nvmf_transport_poll_group *group,
			 struct spdk_nvmf_qpair *qpair)
{
	SPDK_NOTICELOG("*** nvmf_ub_poll_group_add ***\n");
	struct spdk_nvmf_ub_poll_group	*ugroup;
	struct spdk_nvmf_ub_qpair		*uqpair;
	struct spdk_nvmf_ub_resources	*resources;

	ugroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_ub_poll_group, group);
	uqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_ub_qpair, qpair);

	resources = uqpair->resources;
	if (resources == NULL) {
		return -EINVAL;
	}

	uqpair->group = ugroup;
	TAILQ_INSERT_TAIL(&ugroup->qpairs, uqpair, link);

	return 0;
}

static int
nvmf_ub_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	struct spdk_nvmf_ub_poll_group *ugroup = nvmf_ub_get_poll_group(group);
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(qpair,
		struct spdk_nvmf_ub_qpair, qpair);

	if (uqpair->group == ugroup) {
		TAILQ_REMOVE(&ugroup->qpairs, uqpair, link);
		uqpair->group = NULL;
	}

	return 0;
}

static void nvmf_ub_post_send_response(struct spdk_nvmf_ub_request *ub_req);
static int nvmf_ub_post_send_response_ctx(struct spdk_nvmf_ub_request *ub_req,
		struct spdk_nvmf_ub_response *ub_rsp);

static int
nvmf_ub_post_data_transfer(struct spdk_nvmf_ub_request *ub_req, void *local_buf,
			   urma_target_seg_t *local_tseg, urma_opcode_t opcode)
{
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(ub_req->req.qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	uint64_t local_offset = 0;
	uint32_t i;

	if ((opcode != URMA_OPC_READ && opcode != URMA_OPC_WRITE) ||
	    ub_req->num_remote_sges == 0 || local_tseg == NULL || local_buf == NULL) {
		return -EINVAL;
	}
	for (i = 0; i < ub_req->num_remote_sges; i++) {
		struct spdk_nvmf_ub_remote_sge *remote = &ub_req->remote_sges[i];

		if (remote->tseg == NULL || remote->tjetty == NULL || remote->length == 0 ||
		    local_offset + remote->length > ub_req->req.length) {
			return -EINVAL;
		}
		local_offset += remote->length;
	}
	if (local_offset != ub_req->req.length) {
		return -EINVAL;
	}
	local_offset = 0;

	ub_req->num_outstanding_data_wr = 0;
	ub_req->data_transfer_failed = false;
	ub_req->awaiting_ub_read_completion = opcode == URMA_OPC_READ;
	ub_req->awaiting_ub_write_completion = opcode == URMA_OPC_WRITE;
	ub_req->ub_state = opcode == URMA_OPC_READ ? UB_REQ_UB_STATE_WAIT_READ :
							      UB_REQ_UB_STATE_WAIT_WRITE;

	for (i = 0; i < ub_req->num_remote_sges; i++) {
		struct spdk_nvmf_ub_remote_sge *remote = &ub_req->remote_sges[i];
		urma_sge_t remote_sge = {0}, local_sge = {0};
		urma_sg_t remote_sg = {0}, local_sg = {0};
		urma_rw_wr_t ub_rw_wr = {0};
		urma_jfs_wr_t data_wr = {0};
		urma_jfs_wr_t *bad_wr = NULL;

		remote_sge.addr = remote->remote_addr;
		remote_sge.len = remote->length;
		remote_sge.tseg = remote->tseg;
		remote_sg.sge = &remote_sge;
		remote_sg.num_sge = 1;

		local_sge.addr = (uint64_t)(uintptr_t)local_buf + local_offset;
		local_sge.len = remote->length;
		local_sge.tseg = local_tseg;
		local_sg.sge = &local_sge;
		local_sg.num_sge = 1;

		if (opcode == URMA_OPC_READ) {
			ub_rw_wr.src = remote_sg;
			ub_rw_wr.dst = local_sg;
		} else {
			ub_rw_wr.src = local_sg;
			ub_rw_wr.dst = remote_sg;
		}

		data_wr.opcode = opcode;
		data_wr.flag.bs.complete_enable = 1;
		data_wr.tjetty = remote->tjetty;
		data_wr.user_ctx = (uint64_t)(uintptr_t)ub_req;
		data_wr.rw = ub_rw_wr;

		if (urma_post_jetty_send_wr(uqpair->jetty, &data_wr, &bad_wr) != URMA_SUCCESS) {
			SPDK_ERRLOG("Failed to post UB %s for qid %u SGE %u/%u\n",
				    opcode == URMA_OPC_READ ? "READ" : "WRITE", uqpair->qid,
				    i, ub_req->num_remote_sges);
			ub_req->data_transfer_failed = true;
			break;
		}

		ub_req->num_outstanding_data_wr++;
		local_offset += remote->length;
	}

	if (ub_req->num_outstanding_data_wr == 0) {
		ub_req->awaiting_ub_read_completion = false;
		ub_req->awaiting_ub_write_completion = false;
		ub_req->ub_state = UB_REQ_UB_STATE_NONE;
		return -EIO;
	}

	return 0;
}

static int
nvmf_ub_post_ub_read(struct spdk_nvmf_ub_request *ub_req, void *local_buf,
		     urma_target_seg_t *local_tseg)
{
	return nvmf_ub_post_data_transfer(ub_req, local_buf, local_tseg, URMA_OPC_READ);
}

static int
nvmf_ub_post_ub_write(struct spdk_nvmf_ub_request *ub_req, void *local_buf,
		      urma_target_seg_t *local_tseg)
{
	return nvmf_ub_post_data_transfer(ub_req, local_buf, local_tseg, URMA_OPC_WRITE);
}

static int
nvmf_ub_repost_recv(struct spdk_nvmf_ub_qpair *uqpair, uint32_t recv_slot)
{
	urma_jfr_wr_t jfr_wr = {0};
	urma_jfr_wr_t *bad_wr = NULL;
	urma_sge_t sge = {0};

	if (recv_slot >= uqpair->depth) {
		SPDK_ERRLOG("Invalid UB recv slot %u for qid %u (depth %u)\n",
			    recv_slot, uqpair->qid, uqpair->depth);
		return -EINVAL;
	}

	sge.addr = (uint64_t)(uintptr_t)uqpair->va + (size_t)recv_slot * MSG_SIZE;
	sge.len = MSG_SIZE;
	sge.tseg = uqpair->local_tseg;
	jfr_wr.src.sge = &sge;
	jfr_wr.src.num_sge = 1;
	jfr_wr.user_ctx = recv_slot;

	if (urma_post_jetty_recv_wr(uqpair->jetty, &jfr_wr, &bad_wr) != URMA_SUCCESS) {
		SPDK_ERRLOG("Failed to re-post recv WR %u for qid %u\n", recv_slot, uqpair->qid);
		return -EIO;
	}

	return 0;
}

static bool
nvmf_ub_req_is_valid(struct spdk_nvmf_ub_qpair *uqpair,
			     struct spdk_nvmf_ub_request *ub_req)
{
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	uintptr_t req_addr = (uintptr_t)ub_req;
	uintptr_t reqs_begin;
	uintptr_t reqs_end;

	if (resources == NULL) {
		return false;
	}

	reqs_begin = (uintptr_t)resources->reqs;
	reqs_end = (uintptr_t)(resources->reqs + resources->depth);
	return req_addr >= reqs_begin && req_addr < reqs_end &&
	       (req_addr - reqs_begin) % sizeof(*resources->reqs) == 0;
}

static bool
nvmf_ub_response_is_valid(struct spdk_nvmf_ub_qpair *uqpair,
			  struct spdk_nvmf_ub_response *ub_rsp)
{
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	uintptr_t rsp_addr = (uintptr_t)ub_rsp;
	uintptr_t responses_begin;
	uintptr_t responses_end;

	if (resources == NULL) {
		return false;
	}

	responses_begin = (uintptr_t)resources->responses;
	responses_end = (uintptr_t)(resources->responses + resources->depth);
	return rsp_addr >= responses_begin && rsp_addr < responses_end &&
	       (rsp_addr - responses_begin) % sizeof(*resources->responses) == 0;
}

static void
nvmf_ub_response_put(struct spdk_nvmf_ub_qpair *uqpair,
		     struct spdk_nvmf_ub_response *ub_rsp)
{
	if (!ub_rsp->in_use) {
		SPDK_ERRLOG("Attempted to release free UB response %p on qid %u\n",
			    ub_rsp, uqpair->qid);
		return;
	}

	ub_rsp->in_use = false;
	STAILQ_INSERT_TAIL(&uqpair->resources->free_response_queue, ub_rsp, link);
}

static struct spdk_nvmf_ub_response *
nvmf_ub_response_get(struct spdk_nvmf_ub_qpair *uqpair)
{
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	struct spdk_nvmf_ub_response *ub_rsp;

	ub_rsp = STAILQ_FIRST(&resources->free_response_queue);
	if (ub_rsp == NULL) {
		return NULL;
	}

	STAILQ_REMOVE_HEAD(&resources->free_response_queue, link);
	assert(!ub_rsp->in_use);
	ub_rsp->in_use = true;
	return ub_rsp;
}

static void
nvmf_ub_queue_pending_response(struct spdk_nvmf_ub_request *ub_req)
{
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(ub_req->req.qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;

	assert(ub_req->in_use);
	assert(ub_req->ub_state == UB_REQ_UB_STATE_NONE);
	assert(resources->pending_response_count < resources->depth);
	ub_req->ub_state = UB_REQ_UB_STATE_WAIT_RESPONSE;
	STAILQ_INSERT_TAIL(&resources->pending_response_queue, ub_req, link);
	resources->pending_response_count++;
	resources->pending_response_high_watermark = spdk_max(
			resources->pending_response_high_watermark,
			resources->pending_response_count);
}

static struct spdk_nvmf_ub_request *
nvmf_ub_pending_response_get(struct spdk_nvmf_ub_qpair *uqpair)
{
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	struct spdk_nvmf_ub_request *ub_req;

	ub_req = STAILQ_FIRST(&resources->pending_response_queue);
	if (ub_req == NULL) {
		return NULL;
	}

	STAILQ_REMOVE_HEAD(&resources->pending_response_queue, link);
	assert(resources->pending_response_count > 0);
	resources->pending_response_count--;
	assert(ub_req->ub_state == UB_REQ_UB_STATE_WAIT_RESPONSE);
	ub_req->ub_state = UB_REQ_UB_STATE_NONE;
	return ub_req;
}

static void
nvmf_ub_pending_response_remove(struct spdk_nvmf_ub_request *ub_req)
{
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(ub_req->req.qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;

	assert(ub_req->ub_state == UB_REQ_UB_STATE_WAIT_RESPONSE);
	STAILQ_REMOVE(&resources->pending_response_queue, ub_req,
			spdk_nvmf_ub_request, link);
	assert(resources->pending_response_count > 0);
	resources->pending_response_count--;
	ub_req->ub_state = UB_REQ_UB_STATE_NONE;
}

static void
nvmf_ub_req_put(struct spdk_nvmf_ub_request *ub_req)
{
	struct spdk_nvmf_request *req = &ub_req->req;
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(req->qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	uint32_t recv_slot;

	if (!ub_req->in_use) {
		SPDK_ERRLOG("Attempted to release free UB request %p on qid %u\n",
			    ub_req, uqpair->qid);
		return;
	}
	if (ub_req->ub_state == UB_REQ_UB_STATE_WAIT_RESPONSE) {
		nvmf_ub_pending_response_remove(ub_req);
	}

	recv_slot = ub_req->recv_slot;
	ub_req->ub_state = UB_REQ_UB_STATE_NONE;
	ub_req->awaiting_ub_read_completion = false;
	ub_req->awaiting_ub_write_completion = false;
	ub_req->num_remote_sges = 0;
	ub_req->num_outstanding_data_wr = 0;
	ub_req->data_transfer_failed = false;
	ub_req->zcopy_abort = false;
	ub_req->recv_slot = NVMF_UB_INVALID_RECV_SLOT;
	ub_req->in_use = false;
	req->cmd = NULL;
	req->iovcnt = 0;
	req->length = 0;
	req->zcopy_bdev_io = NULL;
	req->zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	STAILQ_INSERT_TAIL(&resources->free_queue, ub_req, link);

	/* Return the request and its receive credit together. */
	if (recv_slot != NVMF_UB_INVALID_RECV_SLOT &&
	    nvmf_ub_repost_recv(uqpair, recv_slot) != 0) {
		spdk_nvmf_qpair_disconnect(&uqpair->qpair);
	}
}

static void
nvmf_ub_req_abort(struct spdk_nvmf_ub_request *ub_req)
{
	/* The qpair is being disconnected, so do not expose another receive
	 * credit while releasing local request resources. */
	ub_req->recv_slot = NVMF_UB_INVALID_RECV_SLOT;
	ub_req->zcopy_abort = true;
	if (ub_req->req.zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE &&
	    ub_req->req.zcopy_bdev_io != NULL) {
		/* An outstanding UB operation still owns this buffer.  Its completion
		 * will release the zcopy I/O after the device no longer references it. */
		if (ub_req->ub_state != UB_REQ_UB_STATE_NONE) {
			return;
		}
		spdk_nvmf_request_zcopy_end(&ub_req->req, false);
		return;
	}
	if (ub_req->req.zcopy_phase == NVMF_ZCOPY_PHASE_INIT ||
	    ub_req->req.zcopy_phase == NVMF_ZCOPY_PHASE_END_PENDING) {
		return;
	}
	nvmf_ub_req_put(ub_req);
}

static void
nvmf_ub_req_free(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ub_request *ub_req;

	ub_req = SPDK_CONTAINEROF(req, struct spdk_nvmf_ub_request, req);
	if (ub_req->in_use) {
		if (req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT ||
		    req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE ||
		    req->zcopy_phase == NVMF_ZCOPY_PHASE_END_PENDING) {
			ub_req->zcopy_abort = true;
			if (req->qpair->state != SPDK_NVMF_QPAIR_ENABLED) {
				ub_req->recv_slot = NVMF_UB_INVALID_RECV_SLOT;
			}
			if (req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE &&
			    req->zcopy_bdev_io != NULL &&
			    ub_req->ub_state == UB_REQ_UB_STATE_NONE) {
				spdk_nvmf_request_zcopy_end(req, false);
			}
			return;
		}
		if (req->qpair->state == SPDK_NVMF_QPAIR_ENABLED) {
			nvmf_ub_req_put(ub_req);
		} else {
			nvmf_ub_req_abort(ub_req);
		}
	}
}

static void
nvmf_ub_start_remote_request(struct spdk_nvmf_ub_request *ub_req, void *local_buf)
{
	struct spdk_nvmf_request *req = &ub_req->req;
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(req->qpair,
		struct spdk_nvmf_ub_qpair, qpair);

	ub_req->data_from_remote = true;
	if (nvmf_ctrlr_use_zcopy(req)) {
		req->data_from_pool = false;
		spdk_nvmf_request_zcopy_start(req);
		return;
	}

	if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		spdk_nvmf_request_exec(req);
		return;
	}

	if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
		if (nvmf_ub_post_ub_read(ub_req, local_buf, uqpair->local_tseg) == 0) {
			return;
		}

		SPDK_ERRLOG("Failed to post UB READ WRs on qid %u\n", uqpair->qid);
		req->rsp->nvme_cpl.cid = req->cmd->nvme_cmd.cid;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		nvmf_ub_post_send_response(ub_req);
		return;
	}

	spdk_nvmf_request_exec(req);
}

static void
nvmf_ub_handle_cmd(struct spdk_nvmf_ub_qpair *uqpair, uint32_t recv_slot,
		   uint32_t capsule_len)
{
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	struct spdk_nvmf_ub_request *ub_req;
	struct spdk_nvmf_request *nvmf_req;
	struct spdk_nvme_sgl_descriptor *sgl;
	void *local_buf;
	uint32_t buf_idx;
	uint32_t data_len;
	void *cmd;

	if (resources == NULL || recv_slot >= uqpair->depth ||
	    capsule_len < sizeof(struct spdk_nvme_cmd) || capsule_len > MSG_SIZE) {
		SPDK_ERRLOG("Invalid UB receive on qid %u, slot=%u\n", uqpair->qid, recv_slot);
		spdk_nvmf_qpair_disconnect(&uqpair->qpair);
		return;
	}
	cmd = (uint8_t *)uqpair->va + (size_t)recv_slot * MSG_SIZE;

	/* Get a free request from the pool */
	ub_req = STAILQ_FIRST(&resources->free_queue);
	if (ub_req == NULL) {
		/* This is an invariant violation: receive slots are only reposted
		 * when their paired requests return to the free queue. */
		SPDK_ERRLOG("UB qpair %u has no free requests for recv slot %u\n",
			    uqpair->qid, recv_slot);
		spdk_nvmf_qpair_disconnect(&uqpair->qpair);
		return;
	}
	STAILQ_REMOVE_HEAD(&resources->free_queue, link);
	assert(!ub_req->in_use);

	nvmf_req = &ub_req->req;

	/* Initialize the request - all fields must be set before exec */
	nvmf_req->qpair = &uqpair->qpair;
	nvmf_req->cmd = (union nvmf_h2c_msg *)cmd;
	nvmf_req->rsp = &resources->cpls[ub_req - resources->reqs];
	memset(nvmf_req->rsp, 0, sizeof(*nvmf_req->rsp));

	/* Determine data transfer direction from the NVMe command opcode */
	nvmf_req->xfer = spdk_nvmf_req_get_xfer(nvmf_req);

	nvmf_req->length = 0;
	nvmf_req->iovcnt = 0;
	nvmf_req->data_from_pool = false;
	nvmf_req->dif_enabled = false;
	nvmf_req->zcopy_phase = NVMF_ZCOPY_PHASE_NONE;

	/* Initialize UB-specific request fields */
	ub_req->buf_idx = ub_req - resources->reqs;
	ub_req->recv_slot = recv_slot;
	ub_req->in_use = true;
	ub_req->ub_state = UB_REQ_UB_STATE_NONE;
	ub_req->data_from_remote = false;
	ub_req->remote_addr = 0;
	ub_req->remote_key = 0;
	ub_req->remote_data_tseg = NULL;
	ub_req->remote_data_tjetty = NULL;
	ub_req->num_remote_sges = 0;
	ub_req->num_outstanding_data_wr = 0;
	ub_req->data_transfer_failed = false;
	ub_req->awaiting_ub_read_completion = false;
	ub_req->awaiting_ub_write_completion = false;
	ub_req->zcopy_abort = false;

	/* Parse SGL to determine data buffer location */
	sgl = &nvmf_req->cmd->nvme_cmd.dptr.sgl1;
	data_len = sgl->generic.type == SPDK_NVME_SGL_TYPE_DATA_BLOCK ?
		   sgl->unkeyed.length : sgl->keyed.length;

	buf_idx = ub_req - resources->reqs;
	local_buf = (uint8_t *)uqpair->va + uqpair->data_offset +
		    (size_t)buf_idx * SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE;

	nvmf_req->iov[0].iov_base = local_buf;
	nvmf_req->iov[0].iov_len = data_len;
	nvmf_req->length = data_len;
	nvmf_req->iovcnt = 1;

	if (nvmf_req->xfer == SPDK_NVME_DATA_NONE) {
		/* No data transfer - command only */
		nvmf_req->iovcnt = 0;
	}

	if (nvmf_req->xfer != SPDK_NVME_DATA_NONE &&
	    data_len > SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE) {
		SPDK_ERRLOG("UB request length %u exceeds max I/O size %u on qid %u\n",
			    data_len, (uint32_t)SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE, uqpair->qid);
		nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
		nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		nvmf_ub_post_send_response(ub_req);
		return;
	}

	/* Handle different SGL types */
	switch (sgl->generic.type) {
	case SPDK_NVME_SGL_TYPE_VENDOR_SPECIFIC:
		{
			struct spdk_nvmf_ub_npu_region *region;

			if (sgl->keyed.subtype != SPDK_NVME_UB_SGL_SUBTYPE_NPU) {
				SPDK_ERRLOG("Unsupported UB vendor SGL subtype %u on qid %u\n",
					    sgl->keyed.subtype, uqpair->qid);
				nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
				nvmf_req->rsp->nvme_cpl.status.sc =
					SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
				nvmf_ub_post_send_response(ub_req);
				return;
			}

			region = nvmf_ub_find_npu_region(uqpair, sgl->keyed.key);
			if (region == NULL ||
			    !spdk_nvme_ub_range_contains(region->remote_base, region->length,
						     sgl->address, sgl->keyed.length)) {
				SPDK_ERRLOG("Invalid NPU region/address: qid=%u region=%u addr=0x%" PRIx64
					    " len=%u\n", uqpair->qid, sgl->keyed.key,
					    sgl->address, (uint32_t)sgl->keyed.length);
				nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
				nvmf_req->rsp->nvme_cpl.status.sc =
					SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				nvmf_ub_post_send_response(ub_req);
				return;
			}

			ub_req->data_from_remote = true;
			ub_req->remote_addr = sgl->address;
			ub_req->remote_key = sgl->keyed.key;
			ub_req->remote_data_tseg = region->tseg;
			ub_req->remote_data_tjetty = region->tjetty;
			ub_req->num_remote_sges = 1;
			ub_req->remote_sges[0].remote_addr = sgl->address;
			ub_req->remote_sges[0].length = sgl->keyed.length;
			ub_req->remote_sges[0].key = sgl->keyed.key;
			ub_req->remote_sges[0].tseg = region->tseg;
			ub_req->remote_sges[0].tjetty = region->tjetty;

			nvmf_ub_start_remote_request(ub_req, local_buf);
			return;
		}
		break;

	case SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK:
		if (sgl->keyed.subtype != SPDK_NVME_SGL_SUBTYPE_ADDRESS) {
			SPDK_ERRLOG("Unsupported keyed SGL subtype %u on qid %u\n",
				    sgl->keyed.subtype, uqpair->qid);
			nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
			nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
			nvmf_ub_post_send_response(ub_req);
			return;
		}

		{
			urma_target_seg_t *remote_tseg;

			/* Store remote info for UB operations in ub_req */
			ub_req->data_from_remote = true;
			ub_req->remote_addr = sgl->address;
			ub_req->remote_key = sgl->keyed.key;

			SPDK_DEBUGLOG(ub, "Keyed data block: local_buf=%p, remote_addr=0x%" PRIx64
				      ", len=%u, key=0x%x, xfer=%d\n",
				      local_buf, sgl->address, sgl->keyed.length, sgl->keyed.key,
				      nvmf_req->xfer);

			if (nvmf_req->xfer == SPDK_NVME_DATA_NONE) {
				break;
			}

			remote_tseg = nvmf_ub_get_remote_tseg(uqpair, sgl->address,
							       sgl->keyed.length, sgl->keyed.key);
			if (remote_tseg == NULL) {
				nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
				nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				nvmf_ub_post_send_response(ub_req);
				return;
			}

			ub_req->num_remote_sges = 1;
			ub_req->remote_sges[0].remote_addr = sgl->address;
			ub_req->remote_sges[0].length = sgl->keyed.length;
			ub_req->remote_sges[0].key = sgl->keyed.key;
			ub_req->remote_sges[0].tseg = remote_tseg;
			ub_req->remote_sges[0].tjetty = uqpair->target_jetty;

			nvmf_ub_start_remote_request(ub_req, local_buf);
			return;
		}
		break;

	case SPDK_NVME_SGL_TYPE_LAST_SEGMENT:
		{
			struct spdk_nvme_sgl_descriptor *descs;
			uint32_t capsule_data_len = capsule_len - sizeof(struct spdk_nvme_cmd);
			uint32_t descriptor_bytes = sgl->unkeyed.length;
			uint32_t descriptor_count;
			uint64_t total_length = 0;
			uint32_t i;

			if (sgl->unkeyed.subtype != SPDK_NVME_SGL_SUBTYPE_OFFSET ||
			    descriptor_bytes == 0 ||
			    descriptor_bytes % sizeof(struct spdk_nvme_sgl_descriptor) != 0 ||
			    sgl->address > capsule_data_len ||
			    descriptor_bytes > capsule_data_len - sgl->address) {
				SPDK_ERRLOG("Invalid UB SGL segment: qid=%u offset=%" PRIu64
					    " length=%u capsule_data=%u\n", uqpair->qid, sgl->address,
					    descriptor_bytes, capsule_data_len);
				nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
				nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
				nvmf_ub_post_send_response(ub_req);
				return;
			}

			descriptor_count = descriptor_bytes / sizeof(*descs);
			if (descriptor_count > SPDK_NVME_UB_MAX_SGL_DESCRIPTORS) {
				nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
				nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
				nvmf_ub_post_send_response(ub_req);
				return;
			}

			descs = (struct spdk_nvme_sgl_descriptor *)
				((uint8_t *)cmd + sizeof(struct spdk_nvme_cmd) + sgl->address);
			for (i = 0; i < descriptor_count; i++) {
				struct spdk_nvme_sgl_descriptor *desc = &descs[i];
				struct spdk_nvmf_ub_remote_sge *remote = &ub_req->remote_sges[i];

				if (desc->keyed.length == 0 ||
				    total_length + desc->keyed.length > SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE) {
					goto invalid_multi_sgl;
				}

				remote->remote_addr = desc->address;
				remote->length = desc->keyed.length;
				remote->key = desc->keyed.key;
				if (desc->generic.type == SPDK_NVME_SGL_TYPE_VENDOR_SPECIFIC &&
				    desc->keyed.subtype == SPDK_NVME_UB_SGL_SUBTYPE_NPU) {
					struct spdk_nvmf_ub_npu_region *region;

					region = nvmf_ub_find_npu_region(uqpair, desc->keyed.key);
					if (region == NULL ||
					    !spdk_nvme_ub_range_contains(region->remote_base, region->length,
								     desc->address, desc->keyed.length)) {
						goto invalid_multi_sgl;
					}
					remote->tseg = region->tseg;
					remote->tjetty = region->tjetty;
				} else if (desc->generic.type == SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK &&
					   desc->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS) {
					remote->tseg = nvmf_ub_get_remote_tseg(uqpair, desc->address,
										 desc->keyed.length,
										 desc->keyed.key);
					remote->tjetty = uqpair->target_jetty;
					if (remote->tseg == NULL || remote->tjetty == NULL) {
						goto invalid_multi_sgl;
					}
				} else {
					goto invalid_multi_sgl;
				}

				total_length += desc->keyed.length;
			}

			ub_req->num_remote_sges = descriptor_count;
			ub_req->remote_addr = ub_req->remote_sges[0].remote_addr;
			ub_req->remote_key = ub_req->remote_sges[0].key;
			ub_req->remote_data_tseg = ub_req->remote_sges[0].tseg;
			ub_req->remote_data_tjetty = ub_req->remote_sges[0].tjetty;
			nvmf_req->iov[0].iov_base = local_buf;
			nvmf_req->iov[0].iov_len = total_length;
			nvmf_req->iovcnt = 1;
			nvmf_req->length = total_length;

			SPDK_DEBUGLOG(ub, "Multi SGL: qid=%u descriptors=%u length=%u xfer=%d\n",
				      uqpair->qid, descriptor_count, nvmf_req->length, nvmf_req->xfer);
			nvmf_ub_start_remote_request(ub_req, local_buf);
			return;

invalid_multi_sgl:
			SPDK_ERRLOG("Invalid UB multi-SGL descriptor %u/%u on qid %u\n",
				    i, descriptor_count, uqpair->qid);
			nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
			nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
			nvmf_ub_post_send_response(ub_req);
			return;
		}

	case SPDK_NVME_SGL_TYPE_DATA_BLOCK:
		{
			uint8_t *cmd_data = (uint8_t *)nvmf_req->cmd;
			void *inline_data;

			if (sgl->unkeyed.subtype != SPDK_NVME_SGL_SUBTYPE_OFFSET) {
				SPDK_ERRLOG("Unsupported data block SGL subtype %u on qid %u\n",
					    sgl->unkeyed.subtype, uqpair->qid);
				nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
				nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
				nvmf_ub_post_send_response(ub_req);
				return;
			}

			/* Inline data - address is an offset from the receive capsule start. */

			if (sgl->address > MSG_SIZE || sgl->unkeyed.length > MSG_SIZE - sgl->address) {
				SPDK_ERRLOG("Invalid in-capsule data range: offset=%" PRIu64
					    " length=%u qid=%u\n", sgl->address,
					    sgl->unkeyed.length, uqpair->qid);
				nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
				nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_SGL_OFFSET;
				nvmf_ub_post_send_response(ub_req);
				return;
			}

			inline_data = cmd_data + sgl->address;

			nvmf_req->iov[0].iov_base = inline_data;
			nvmf_req->iov[0].iov_len = sgl->unkeyed.length;
			nvmf_req->length = sgl->unkeyed.length;
			nvmf_req->iovcnt = 1;

			SPDK_DEBUGLOG(ub, "Data block (inline): addr=%p, offset=%" PRIu64
				      ", len=%u, xfer=%d\n", inline_data, sgl->address,
				      sgl->unkeyed.length, nvmf_req->xfer);
		}
		break;


	default:
		SPDK_ERRLOG("Unhandled SGL type: type=%d, subtype=%d\n",
			    sgl->generic.type, sgl->generic.subtype);
		nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
		nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
		nvmf_ub_post_send_response(ub_req);
		return;
	}

	/* Check if SGL parsing failed (iovcnt == 0 but xfer requires data) */
	if (nvmf_req->iovcnt == 0 && nvmf_req->xfer != SPDK_NVME_DATA_NONE) {
		SPDK_ERRLOG("SGL parsing failed: iovcnt=0 but xfer=%d\n", nvmf_req->xfer);
		nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
		nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
		nvmf_ub_post_send_response(ub_req);
		return;
	}

	/* Dispatch to upper layer (controller/bdev) */
	spdk_nvmf_request_exec(nvmf_req);
}

static int
nvmf_ub_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_ub_poll_group *ugroup = nvmf_ub_get_poll_group(group);
	struct spdk_nvmf_ub_qpair *uqpair, *tmp;
	int npolled;
	int total_completions = 0;
	uint32_t i;

	/* Poll each per-connection qpair's JFC until it is empty. */
	TAILQ_FOREACH_SAFE(uqpair, &ugroup->qpairs, link, tmp) {
		bool qpair_failed = false;

		npolled = 0;
		while (!qpair_failed &&
		       (npolled = urma_poll_jfc(uqpair->send_jfc,
						(int)ugroup->max_crs,
						ugroup->crs)) > 0) {
			total_completions += npolled;

			for (i = 0; i < (uint32_t)npolled; i++) {
				urma_cr_t *cr = &ugroup->crs[i];
				struct spdk_nvmf_ub_request *completed_ub_req;
				struct spdk_nvmf_ub_response *completed_ub_rsp;

				if (cr->status != URMA_CR_SUCCESS) {
					SPDK_ERRLOG("UB completion failed: qid=%u status=%d opcode=%d s_r=%d "
						    "user_ctx=0x%" PRIx64 "\n", uqpair->qid, cr->status,
						    cr->opcode, cr->flag.bs.s_r, cr->user_ctx);
					if (cr->flag.bs.s_r == 0) {
						completed_ub_rsp = (struct spdk_nvmf_ub_response *)(uintptr_t)cr->user_ctx;
						completed_ub_req = (struct spdk_nvmf_ub_request *)(uintptr_t)cr->user_ctx;
						if (nvmf_ub_response_is_valid(uqpair, completed_ub_rsp) &&
						    completed_ub_rsp->in_use) {
							nvmf_ub_response_put(uqpair, completed_ub_rsp);
						} else if (nvmf_ub_req_is_valid(uqpair, completed_ub_req) &&
						    completed_ub_req->in_use) {
							completed_ub_req->ub_state = UB_REQ_UB_STATE_NONE;
							completed_ub_req->awaiting_ub_read_completion = false;
							completed_ub_req->awaiting_ub_write_completion = false;
							nvmf_ub_req_abort(completed_ub_req);
						}
					}
					spdk_nvmf_qpair_disconnect(&uqpair->qpair);
					qpair_failed = true;
					break;
				}

				/*
				 * Distinguish receive completions (s_r == 1) from
				 * send/UB completions (s_r == 0).
				 */
				if (cr->flag.bs.s_r == 1) {
					/* Received a command capsule */
					if (cr->user_ctx >= uqpair->depth) {
						SPDK_ERRLOG("Invalid UB recv slot 0x%" PRIx64 " on qid %u\n",
							    cr->user_ctx, uqpair->qid);
						spdk_nvmf_qpair_disconnect(&uqpair->qpair);
						qpair_failed = true;
						break;
					}
					SPDK_DEBUGLOG(ub, "Received command: qid=%u slot=%" PRIu64 " len=%u\n",
						      uqpair->qid, cr->user_ctx, cr->completion_len);
					nvmf_ub_handle_cmd(uqpair, (uint32_t)cr->user_ctx,
							   cr->completion_len);

				} else {
					completed_ub_rsp = (struct spdk_nvmf_ub_response *)(uintptr_t)cr->user_ctx;
					if (nvmf_ub_response_is_valid(uqpair, completed_ub_rsp)) {
						if (!completed_ub_rsp->in_use) {
							SPDK_ERRLOG("Invalid UB response completion: qid=%u rsp=%p opcode=%d\n",
								    uqpair->qid, completed_ub_rsp, cr->opcode);
							spdk_nvmf_qpair_disconnect(&uqpair->qpair);
							qpair_failed = true;
							break;
						}
						completed_ub_req = nvmf_ub_pending_response_get(uqpair);
						if (completed_ub_req == NULL) {
							nvmf_ub_response_put(uqpair, completed_ub_rsp);
						} else if (nvmf_ub_post_send_response_ctx(completed_ub_req,
								   completed_ub_rsp) != 0) {
							qpair_failed = true;
							break;
						}
					} else {
						completed_ub_req = (struct spdk_nvmf_ub_request *)(uintptr_t)cr->user_ctx;
						if (!nvmf_ub_req_is_valid(uqpair, completed_ub_req) ||
						    !completed_ub_req->in_use) {
							SPDK_ERRLOG("Invalid UB request completion: qid=%u req=%p opcode=%d\n",
								    uqpair->qid, completed_ub_req, cr->opcode);
							spdk_nvmf_qpair_disconnect(&uqpair->qpair);
							qpair_failed = true;
							break;
						}
						SPDK_DEBUGLOG(ub, "UB completion: qid=%u req=%p opcode=%d state=%d\n",
							      uqpair->qid, completed_ub_req, cr->opcode,
							      completed_ub_req->ub_state);

						switch (completed_ub_req->ub_state) {
						case UB_REQ_UB_STATE_WAIT_READ:
							if (completed_ub_req->num_outstanding_data_wr == 0) {
								SPDK_ERRLOG("Unexpected extra UB READ completion for req %p\n",
									    completed_ub_req);
								spdk_nvmf_qpair_disconnect(&uqpair->qpair);
								qpair_failed = true;
								break;
							}
							completed_ub_req->num_outstanding_data_wr--;
							if (completed_ub_req->num_outstanding_data_wr != 0) {
								break;
							}
							completed_ub_req->awaiting_ub_read_completion = false;
							completed_ub_req->ub_state = UB_REQ_UB_STATE_NONE;
							if (completed_ub_req->data_transfer_failed) {
								completed_ub_req->req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
								completed_ub_req->req.rsp->nvme_cpl.status.sc =
									SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
								if (completed_ub_req->req.zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE) {
									spdk_nvmf_request_zcopy_end(&completed_ub_req->req, false);
								} else {
									nvmf_ub_post_send_response(completed_ub_req);
								}
							} else if (completed_ub_req->zcopy_abort ||
							    __atomic_load_n(&uqpair->qpair.disconnect_started,
									    __ATOMIC_RELAXED)) {
								if (completed_ub_req->req.zcopy_phase ==
								    NVMF_ZCOPY_PHASE_EXECUTE) {
									spdk_nvmf_request_zcopy_end(&completed_ub_req->req, false);
								} else {
									nvmf_ub_req_abort(completed_ub_req);
								}
							} else if (completed_ub_req->req.zcopy_phase ==
								   NVMF_ZCOPY_PHASE_EXECUTE) {
								spdk_nvmf_request_zcopy_end(&completed_ub_req->req, true);
							} else {
								spdk_nvmf_request_exec(&completed_ub_req->req);
							}
							break;

						case UB_REQ_UB_STATE_WAIT_WRITE:
							if (completed_ub_req->num_outstanding_data_wr == 0) {
								SPDK_ERRLOG("Unexpected extra UB WRITE completion for req %p\n",
									    completed_ub_req);
								spdk_nvmf_qpair_disconnect(&uqpair->qpair);
								qpair_failed = true;
								break;
							}
							completed_ub_req->num_outstanding_data_wr--;
							if (completed_ub_req->num_outstanding_data_wr != 0) {
								break;
							}
							completed_ub_req->awaiting_ub_write_completion = false;
							completed_ub_req->ub_state = UB_REQ_UB_STATE_NONE;
							if (completed_ub_req->data_transfer_failed) {
								completed_ub_req->req.rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
								completed_ub_req->req.rsp->nvme_cpl.status.sc =
									SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
								if (completed_ub_req->req.zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE) {
									spdk_nvmf_request_zcopy_end(&completed_ub_req->req, false);
								} else {
									nvmf_ub_post_send_response(completed_ub_req);
								}
							} else if (completed_ub_req->zcopy_abort ||
							    __atomic_load_n(&uqpair->qpair.disconnect_started,
									    __ATOMIC_RELAXED)) {
								if (completed_ub_req->req.zcopy_phase ==
								    NVMF_ZCOPY_PHASE_EXECUTE) {
									spdk_nvmf_request_zcopy_end(&completed_ub_req->req, false);
								} else {
									nvmf_ub_req_abort(completed_ub_req);
								}
							} else if (completed_ub_req->req.zcopy_phase ==
								   NVMF_ZCOPY_PHASE_EXECUTE) {
								spdk_nvmf_request_zcopy_end(&completed_ub_req->req, false);
							} else {
								nvmf_ub_post_send_response(completed_ub_req);
							}
							break;

						default:
							SPDK_ERRLOG("Unexpected completion for req %p in state %d\n",
								    completed_ub_req, completed_ub_req->ub_state);
							nvmf_ub_req_abort(completed_ub_req);
							spdk_nvmf_qpair_disconnect(&uqpair->qpair);
							qpair_failed = true;
							break;
						}
					}
				}

				/* A nested request handler can also initiate disconnect.  Do not
				 * consume any more CRs after the qpair leaves its poll group. */
				if (__atomic_load_n(&uqpair->qpair.disconnect_started, __ATOMIC_RELAXED)) {
					qpair_failed = true;
					break;
				}
			}
		}
		if (!qpair_failed && npolled < 0) {
			SPDK_ERRLOG("UB poll: urma_poll_jfc failed for qid %u: %d\n",
				    uqpair->qid, npolled);
			spdk_nvmf_qpair_disconnect(&uqpair->qpair);
		}
	}

	return total_completions;
}

static int
nvmf_ub_post_send_response_ctx(struct spdk_nvmf_ub_request *ub_req,
			       struct spdk_nvmf_ub_response *ub_rsp)
{
	struct spdk_nvmf_request *req = &ub_req->req;
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(req->qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	void *resp;

	assert(ub_req->in_use);
	assert(ub_req->ub_state == UB_REQ_UB_STATE_NONE);
	assert(ub_rsp->in_use);

	resp = (uint8_t *)uqpair->va + uqpair->rsp_offset +
	       (size_t)ub_rsp->buf_idx * sizeof(union nvmf_c2h_msg);
	memcpy(resp, req->rsp, sizeof(union nvmf_c2h_msg));

	urma_sge_t src_sge = {
		.addr = (uint64_t)(uintptr_t)resp,
		.len = (uint32_t)sizeof(union nvmf_c2h_msg),
		.tseg = uqpair->local_tseg,
	};

	urma_sg_t src_sg = {
		.sge = &src_sge,
		.num_sge = 1
	};

	urma_send_wr_t send_wr = {
		.src = src_sg,
	};

	urma_jfs_wr_t jfs_wr = {
		.opcode = URMA_OPC_SEND,
		.flag.bs.complete_enable = 1,
		.tjetty = uqpair->target_jetty,
		.user_ctx = (uint64_t)(uintptr_t)ub_rsp,
		.send = send_wr,
		.next = NULL
	};
	urma_jfs_wr_t *bad_wr = NULL;

	/* The completion has been copied to storage owned by ub_rsp.  Return the
	 * request and receive credit before making the response visible. */
	nvmf_ub_req_put(ub_req);
	if (__atomic_load_n(&uqpair->qpair.disconnect_started, __ATOMIC_RELAXED)) {
		nvmf_ub_response_put(uqpair, ub_rsp);
		return -ENOTCONN;
	}

	if (urma_post_jetty_send_wr(uqpair->jetty, &jfs_wr, &bad_wr) != URMA_SUCCESS) {
		SPDK_ERRLOG("Failed to post UB response SEND for qid %u, req=%p\n",
			    uqpair->qid, req);
		nvmf_ub_response_put(uqpair, ub_rsp);
		spdk_nvmf_qpair_disconnect(&uqpair->qpair);
		return -EIO;
	} else {
		SPDK_DEBUGLOG(ub, "Posted UB response SEND for qid %u, req=%p\n",
			      uqpair->qid, req);
	}

	return 0;
}

/* Post the SEND response capsule or queue it until a context is available. */
static void
nvmf_ub_post_send_response(struct spdk_nvmf_ub_request *ub_req)
{
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(ub_req->req.qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	struct spdk_nvmf_ub_response *ub_rsp;

	if (__atomic_load_n(&uqpair->qpair.disconnect_started, __ATOMIC_RELAXED)) {
		nvmf_ub_req_abort(ub_req);
		return;
	}

	ub_rsp = nvmf_ub_response_get(uqpair);
	if (ub_rsp == NULL) {
		nvmf_ub_queue_pending_response(ub_req);
		return;
	}

	nvmf_ub_post_send_response_ctx(ub_req, ub_rsp);
}

static void
nvmf_ub_req_complete(struct spdk_nvmf_request *req)
{
	struct spdk_nvmf_ub_request	*ub_req = SPDK_CONTAINEROF(req,
		struct spdk_nvmf_ub_request, req);
	struct spdk_nvmf_ub_qpair	*uqpair = SPDK_CONTAINEROF(req->qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	urma_target_seg_t *local_tseg;
	void *data_buf;
	int rc;

	SPDK_DEBUGLOG(ub, "UB req_complete: qid=%u, xfer=%d, iovcnt=%u, length=%u, zcopy=%u\n",
		      uqpair->qid, req->xfer, req->iovcnt, req->length, req->zcopy_phase);

	ub_req->buf_idx = ub_req - resources->reqs;

	if (__atomic_load_n(&uqpair->qpair.disconnect_started, __ATOMIC_RELAXED)) {
		nvmf_ub_req_abort(ub_req);
		return;
	}
	if (ub_req->zcopy_abort) {
		if (req->zcopy_phase == NVMF_ZCOPY_PHASE_EXECUTE &&
		    req->zcopy_bdev_io != NULL) {
			if (ub_req->ub_state == UB_REQ_UB_STATE_NONE) {
				spdk_nvmf_request_zcopy_end(req, false);
			}
			return;
		}
		if (req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT ||
		    req->zcopy_phase == NVMF_ZCOPY_PHASE_END_PENDING) {
			return;
		}
		nvmf_ub_req_put(ub_req);
		return;
	}

	switch (req->zcopy_phase) {
	case NVMF_ZCOPY_PHASE_INIT_FAILED:
	case NVMF_ZCOPY_PHASE_COMPLETE:
		nvmf_ub_post_send_response(ub_req);
		return;
	case NVMF_ZCOPY_PHASE_EXECUTE:
		if (spdk_nvme_cpl_is_error(&req->rsp->nvme_cpl)) {
			spdk_nvmf_request_zcopy_end(req, false);
			return;
		}

		if (req->iovcnt != 1 || req->iov[0].iov_base == NULL ||
		    req->iov[0].iov_len < req->length) {
			SPDK_ERRLOG("Unsupported UB zcopy buffer for qid %u: iovcnt=%u len=%zu request=%u\n",
				    uqpair->qid, req->iovcnt,
				    req->iovcnt == 0 ? 0 : req->iov[0].iov_len, req->length);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			spdk_nvmf_request_zcopy_end(req, false);
			return;
		}

		local_tseg = nvmf_ub_get_local_tseg(uqpair, req->iov[0].iov_base,
						   req->length);
		if (local_tseg == NULL) {
			uqpair->zcopy_map_failures++;
			SPDK_ERRLOG("UB zcopy buffer %p/%u is not registered on qid %u\n",
				    req->iov[0].iov_base, req->length, uqpair->qid);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			spdk_nvmf_request_zcopy_end(req, false);
			return;
		}

		if (req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
			rc = nvmf_ub_post_ub_read(ub_req, req->iov[0].iov_base, local_tseg);
			if (rc == 0) {
				uqpair->zcopy_write_ios++;
				return;
			}
		} else if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
			rc = nvmf_ub_post_ub_write(ub_req, req->iov[0].iov_base, local_tseg);
			if (rc == 0) {
				uqpair->zcopy_read_ios++;
				return;
			}
		} else {
			rc = -EINVAL;
		}

		SPDK_ERRLOG("Failed to post UB zcopy data transfer for qid %u: %s\n",
			    uqpair->qid, spdk_strerror(-rc));
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		spdk_nvmf_request_zcopy_end(req, false);
		return;
	case NVMF_ZCOPY_PHASE_INIT:
	case NVMF_ZCOPY_PHASE_END_PENDING:
		SPDK_ERRLOG("Unexpected UB zcopy completion phase %u on qid %u\n",
			    req->zcopy_phase, uqpair->qid);
		spdk_nvmf_qpair_disconnect(&uqpair->qpair);
		return;
	case NVMF_ZCOPY_PHASE_NONE:
		break;
	default:
		SPDK_UNREACHABLE();
	}

	if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		if (spdk_nvme_cpl_is_error(&req->rsp->nvme_cpl)) {
			nvmf_ub_post_send_response(ub_req);
			return;
		}

		if (req->iovcnt != 1 || req->iov[0].iov_len < req->length ||
		    req->length > SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE) {
			SPDK_ERRLOG("Invalid C2H buffer for qid %u: iovcnt=%u len=%zu\n",
				    uqpair->qid, req->iovcnt,
				    req->iovcnt == 0 ? 0 : req->iov[0].iov_len);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			nvmf_ub_post_send_response(ub_req);
			return;
		}

		data_buf = (uint8_t *)uqpair->va + uqpair->data_offset +
			   (size_t)ub_req->buf_idx * SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE;
		if (req->iov[0].iov_base != data_buf) {
			memcpy(data_buf, req->iov[0].iov_base, req->length);
			uqpair->read_copy_ios++;
			uqpair->read_copy_bytes += req->length;
		}

		if (nvmf_ub_post_ub_write(ub_req, data_buf, uqpair->local_tseg) != 0) {
			SPDK_ERRLOG("Failed to post UB WRITE for qid %u, req=%p\n", uqpair->qid, req);
			req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			nvmf_ub_post_send_response(ub_req);
		}
		return;
	}

	nvmf_ub_post_send_response(ub_req);
}

static int
nvmf_ub_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_ub_qpair *uqpair;

	uqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_ub_qpair, qpair);
	memcpy(trid, &uqpair->listen_trid, sizeof(*trid));

	return 0;
}

static int
nvmf_ub_qpair_get_peer_trid(struct spdk_nvmf_qpair *qpair,
			     struct spdk_nvme_transport_id *trid)
{
	(void)qpair;
	(void)trid;
	return -ENOTSUP;
}

static int
nvmf_ub_qpair_get_local_trid(struct spdk_nvmf_qpair *qpair,
			      struct spdk_nvme_transport_id *trid)
{
	return nvmf_ub_qpair_get_listen_trid(qpair, trid);
}

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_ub = {
	.name = "UB",
	.type = SPDK_NVME_TRANSPORT_UB,
	.opts_init = nvmf_ub_opts_init,
	.create = nvmf_ub_create,
	.dump_opts = NULL,
	.destroy = nvmf_ub_destroy,

	.listen = nvmf_ub_listen,
	.stop_listen = nvmf_ub_stop_listen,
	.cdata_init = nvmf_ub_cdata_init,

	.listener_discover = nvmf_ub_discover,

	.poll_group_create = nvmf_ub_poll_group_create,
	.get_optimal_poll_group = NULL,
	.poll_group_destroy = nvmf_ub_poll_group_destroy,
	.poll_group_add = nvmf_ub_poll_group_add,
	.poll_group_remove = nvmf_ub_poll_group_remove,
	.poll_group_poll = nvmf_ub_poll_group_poll,

	.req_free = nvmf_ub_req_free,
	.req_complete = nvmf_ub_req_complete,
	.req_get_buffers_done = NULL,

	.qpair_fini = nvmf_ub_close_qpair,
	.qpair_get_peer_trid = nvmf_ub_qpair_get_peer_trid,
	.qpair_get_local_trid = nvmf_ub_qpair_get_local_trid,
	.qpair_get_listen_trid = nvmf_ub_qpair_get_listen_trid,
	.qpair_abort_request = NULL,

	.poll_group_dump_stat = NULL,
};

SPDK_NVMF_TRANSPORT_REGISTER(ub, &spdk_nvmf_transport_ub);
SPDK_LOG_REGISTER_COMPONENT(ub)
