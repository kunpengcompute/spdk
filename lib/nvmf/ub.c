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

#include "spdk_internal/assert.h"
#include "spdk/log.h"

/* URMA APi includes */
#include "urma_api.h"

#include "nvmf_internal.h"
#include "transport.h"

#include "spdk_internal/trace_defs.h"

#ifndef PAGE_SIZE
#define PAGE_SIZE (0x1 << 12) /* 4KB */
#endif

#define MSG_SIZE 4096

const struct spdk_nvmf_transport_ops spdk_nvmf_transport_ub;
static const struct spdk_mem_map_ops g_nvmf_ub_mem_map_ops;

bool g_nvmf_urma_initialized = false;

#define SPDK_NVMF_UB_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_UB_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_UB_DEFAULT_MAX_QPAIRS_PER_CTRLR 8 /* 暂时约束最大8 queue*/
#define SPDK_NVMF_UB_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE (128 * 1024) /* Single-SGL 128 KiB I/O */
#define SPDK_NVMF_UB_DIF_INSERT_OR_STRIP false
#define SPDK_NVMF_UB_DEFAULT_ABORT_TIMEOUT_SEC 1
#define SPDK_NVMF_UB_DEFAULT_DATA_WR_POOL_SIZE 4095
#define SPDK_NVMF_UB_MIN_IO_BUFFER_SIZE (SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE / SPDK_NVMF_MAX_SGL_ENTRIES)
#define SPDK_NVMF_UB_DEFAULT_NUM_SHARED_BUFFERS 4095
#define SPDK_NVMF_UB_DEFAULT_BUFFER_CACHE_SIZE UINT32_MAX

#define SPDK_NVMF_UB_DEFAULT_NO_SRQ false
#define NVMF_DEFAULT_TX_SGE		SPDK_NVMF_MAX_SGL_ENTRIES

/* UB transport out-of-band message types */
#define UB_MSG_TYPE_CONNECT      1
#define UB_MSG_TYPE_CONNECT_RSP  2
#define UB_MSG_TYPE_DISCONNECT   3

/* UB transport specific constants */
#define SPDK_NVMF_UB_MAX_ACCEPT_SOCK_ONE_TIME 16
#define NVMF_UB_INVALID_RECV_SLOT UINT32_MAX

struct ub_transport_opts {
	/* no use now */
	bool	no_srq;
};

/* UB transport qpair state */
enum spdk_nvmf_ub_qpair_state {
	UB_QPAIR_STATE_INVALID = 0,
	UB_QPAIR_STATE_CONNECTING = 1,
	UB_QPAIR_STATE_RUNNING = 2,
	UB_QPAIR_STATE_DISCONNECTING = 3,
	UB_QPAIR_STATE_DISCONNECTED = 4,
};


enum spdk_nvmf_ub_request_state { /* UB request processing state */
	/* The request is not currently in use */
	UB_REQUEST_STATE_FREE = 0,

	/* Initial state when request first received */
	UB_REQUEST_STATE_NEW,

	/* The request is queued until a data buffer is available. */
	UB_REQUEST_STATE_NEED_BUFFER,

	/* The request has a data buffer available. */
	UB_REQUEST_STATE_HAVE_BUFFER,

	/* The request is waiting on UB queue depth availability
	 * to transfer data from the host to the controller.
	 */
	UB_REQUEST_STATE_DATA_TRANSFER_TO_CONTROLLER_PENDING,

	/* The request is currently transferring data from the host to the controller. */
	UB_REQUEST_STATE_TRANSFERRING_HOST_TO_CONTROLLER,

	/* The request is ready to execute at the block device */
	UB_REQUEST_STATE_READY_TO_EXECUTE,

	/* The request is currently executing at the block device */
	UB_REQUEST_STATE_EXECUTING,

	/* The request finished executing at the block device */
	UB_REQUEST_STATE_EXECUTED,

	/* The request is waiting on UB queue depth availability
	 * to transfer data from the controller to the host.
	 */
	UB_REQUEST_STATE_DATA_TRANSFER_TO_HOST_PENDING,

	/* The request is waiting on UB queue depth availability
	 * to send response to the host.
	 */
	UB_REQUEST_STATE_READY_TO_COMPLETE_PENDING,

	/* The request is ready to send a completion */
	UB_REQUEST_STATE_READY_TO_COMPLETE,

	/* The request is currently transferring data from the controller to the host. */
	UB_REQUEST_STATE_TRANSFERRING_CONTROLLER_TO_HOST,

	/* The request currently has an outstanding completion without an
	 * associated data transfer.
	 */
	UB_REQUEST_STATE_COMPLETING,

	/* The request completed and can be marked free. */
	UB_REQUfATE_COMPLETED,

	/* Terminator */
	UB_REQUEST_NUM_STATES,
};


struct spdk_nvmf_ub_poll_group {
	struct spdk_nvmf_transport_poll_group		group;
	/* Scratch array for urma_poll_jfc() results */
	urma_cr_t                      *crs;
	uint32_t                        max_crs;

	TAILQ_HEAD(, spdk_nvmf_ub_qpair)	qpairs;
	TAILQ_ENTRY(spdk_nvmf_ub_poll_group)		link;
};

struct spdk_nvmf_ub_transport {
	/* Must be first */
	struct spdk_nvmf_transport	transport;
	struct ub_transport_opts	ub_opts;

	/* URMA context */
	urma_context_t				*urma_ctx;
	urma_jfce_t 				*jfce;
	struct spdk_mem_map			*mem_map;

	/* Pending connections sock_group for handling connect requests */
	struct spdk_sock_group		*listen_sock_group;
	struct spdk_poller			*accept_poller;
	struct spdk_sock_group		*pending_sock_group;
	struct spdk_poller			*pending_poller;

	TAILQ_HEAD(, spdk_nvmf_ub_device)	devices;
	/* List of ports */
	TAILQ_HEAD(, spdk_nvmf_ub_port)	ports;
};

/* Device associated with the URMA transport context. */
struct spdk_nvmf_ub_device {
	urma_device_t *device;
	struct spdk_interrupt			*async_intr;
	TAILQ_ENTRY(spdk_nvmf_ub_device)	link;
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

struct spdk_nvmf_ub_request {
	struct spdk_nvmf_request		req;

	/* Buffer index in the qpair's registered data region. */
	uint32_t				buf_idx;

	/* Remote address and key for UB operations */
	uint64_t				remote_addr;
	uint32_t				remote_key;

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
	urma_target_seg_t			*tseg;
	TAILQ_ENTRY(spdk_nvmf_ub_remote_seg)	link;
};

struct spdk_nvmf_ub_qpair {
	struct spdk_nvmf_qpair			qpair;
	struct spdk_nvmf_ub_poll_group		*group;

	/* URMA resources */
	urma_jetty_t				*jetty;
	urma_target_jetty_t			*target_jetty;
	urma_jfc_t				*send_jfc;
	urma_jfc_t				*recv_jfc;
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
	urma_transport_mode_t			remote_trans_mode;

	void					*va;
	urma_target_seg_t			*local_tseg;
	urma_target_seg_t			*remote_tseg;
	TAILQ_HEAD(, spdk_nvmf_ub_remote_seg)	remote_segs;
	size_t				rsp_offset;
	size_t				data_offset;
	size_t				seg_len;

	/* Controller-to-host fallback copy statistics. */
	uint64_t			read_copy_ios;
	uint64_t			read_copy_bytes;
	uint64_t			zcopy_read_ios;
	uint64_t			zcopy_write_ios;
	uint64_t			zcopy_map_failures;

	TAILQ_HEAD(, spdk_nvmf_request)	reqs;

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
	struct spdk_nvmf_ub_device		*device;
	struct spdk_sock			*listen_sock;
	TAILQ_ENTRY(spdk_nvmf_ub_port)	link;
};

struct ub_connect_req_rsp {
    urma_eid_t eid;           /* Endpoint ID */
    uint32_t uasid;           /* URMA context ID */
    uint64_t seg_va;          /* Segment virtual address */
    uint64_t seg_len;         /* Segment length */
    uint32_t seg_flag;        /* Segment flags */
    uint32_t seg_token_id;    /* Segment token ID */
    urma_jetty_id_t jetty_id; /* Local jetty ID */
    uint16_t qid;             /* Queue pair ID */
    uint8_t trans_mode;
    uint8_t msg_type;
} __attribute__((packed));

static void
nvmf_ub_dump_req_rsp(struct ub_connect_req_rsp *req)
{
    SPDK_NOTICELOG("=== UB Connect Req/Rsp ===\n");
    SPDK_NOTICELOG("msg_type: 0x%02x\n", req->msg_type);
    SPDK_NOTICELOG("qid: %u\n", req->qid);
    SPDK_NOTICELOG("jetty_id: uasid=%u, id=%u\n",
                   req->jetty_id.uasid, req->jetty_id.id);
    SPDK_NOTICELOG("uasid: %u\n", req->uasid);
    SPDK_NOTICELOG("eid: %s\n", req->eid.raw);
    SPDK_NOTICELOG("seg_va: 0x%016lx\n", req->seg_va);
    SPDK_NOTICELOG("seg_len: %lu\n", req->seg_len);
    SPDK_NOTICELOG("seg_flag: 0x%08x\n", req->seg_flag);
    SPDK_NOTICELOG("seg_token_id: %u\n", req->seg_token_id);
    SPDK_NOTICELOG("trans_mode: %u\n", req->trans_mode);
    SPDK_NOTICELOG("================================\n");
}


/* Pending connection waiting for connect request */
struct spdk_nvmf_ub_pending_conn {
	struct spdk_sock			*sock;
	struct spdk_nvmf_ub_qpair		*uqpair;
	struct ub_connect_req_rsp			req;
	size_t					req_offset;
	TAILQ_ENTRY(spdk_nvmf_ub_pending_conn)	link;
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

static int get_eid_index(urma_device_t *dev)
{
    urma_eid_info_t *eid_list;
    uint32_t eid_cnt;
    int eid_index = -1;

    eid_list = urma_get_eid_list(dev, &eid_cnt);
    if (eid_list == NULL) {
        return -1;
    }
    for (uint32_t i = 0; eid_list != NULL && i < eid_cnt; i++) {
        printf("device_name :%s (eid%d: "EID_FMT").\n", dev->name, eid_list[i].eid_index, EID_ARGS(eid_list[i].eid));
    }
    if (eid_cnt > 0) {
        eid_index = eid_list[0].eid_index;
    }
    urma_free_eid_list(eid_list);
    return eid_index;
}

static int
nvmf_ub_create_urma(struct spdk_nvmf_ub_transport *utransport)
{
	urma_init_attr_t init_attr = {};
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
	if (rc != URMA_SUCCESS) {
		SPDK_ERRLOG("Failed to initialize URMA library: %d\n", rc);
		return -1;
	}

	g_nvmf_urma_initialized = true;
	SPDK_NOTICELOG("URMA library initialized successfully\n");

	char *dev_name = "udmac0d1e2";
	urma_device_t *dev = urma_get_device_by_name(dev_name);
	if (dev == NULL) {
		SPDK_ERRLOG("urma get device by name failed!\n");
		return -1;
	}

	urma_device_attr_t dev_attr;
	if (urma_query_device(dev, &dev_attr) != URMA_SUCCESS) {
		SPDK_ERRLOG("Failed to query device %s.\n", dev_name);
		return -1;
	}

	SPDK_NOTICELOG("Got URMA device by eid successfully\n");

    int eid_index = get_eid_index(dev);
    if (eid_index < 0) {
		SPDK_ERRLOG("Failed to get eid_index\n");
		return -1;
    }
	SPDK_NOTICELOG("eid_index %d\n", eid_index);

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
	SPDK_NOTICELOG("Created JFCE successfully\n");

	/* Add device to the device list for management */
	struct spdk_nvmf_ub_device *ub_dev = calloc(1, sizeof(*ub_dev));
	if (ub_dev == NULL) {
		SPDK_ERRLOG("Failed to allocate ub_device\n");
		return -1;
	}
	ub_dev->device = dev;
	ub_dev->async_intr = NULL;
	TAILQ_INSERT_TAIL(&utransport->devices, ub_dev, link);
	SPDK_NOTICELOG("Added device to transport device list\n");

	return 0;
}

static struct spdk_nvmf_transport *
nvmf_ub_create(struct spdk_nvmf_transport_opts *opts)
{
	struct spdk_iobuf_opts opts_iobuf = {};
	struct spdk_nvmf_ub_transport *utransport;
	uint32_t			sge_count;

	utransport = calloc(1, sizeof(*utransport));
	if (!utransport) {
		return NULL;
	}

	TAILQ_INIT(&utransport->devices);
	TAILQ_INIT(&utransport->ports);

	utransport->transport.ops = &spdk_nvmf_transport_ub;

	utransport->ub_opts.no_srq = SPDK_NVMF_UB_DEFAULT_NO_SRQ;

	SPDK_NOTICELOG("*** UB Transport Init ***\n");

	SPDK_INFOLOG(ub, "*** UB Transport Init ***\n"
		     "  Transport opts:  max_ioq_depth=%d, max_io_size=%d,\n"
		     "  max_io_qpairs_per_ctrlr=%d, io_unit_size=%d,\n"
		     "  in_capsule_data_size=%d, max_aq_depth=%d,\n"
		     "  num_shared_buffers=%d, no_srq=%d, abort_timeout_sec=%d\n",
		     opts->max_queue_depth,
		     opts->max_io_size,
		     opts->max_qpairs_per_ctrlr - 1,
		     opts->io_unit_size,
		     opts->in_capsule_data_size,
		     opts->max_aq_depth,
		     opts->num_shared_buffers,
		     utransport->ub_opts.no_srq,
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
	SPDK_NOTICELOG("*** UB Transport Init over ***\n");
	return &utransport->transport;
}

static void
nvmf_ub_dump_opts(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w)
{
	SPDK_NOTICELOG("*** nvmf_ub_dump_opts ***\n");
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

	SPDK_NOTICELOG("*** create jfr ok! ***\n");

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

	SPDK_NOTICELOG("Created jetty for qid %u, jetty_id=%u\n",
		       uqpair->qid, uqpair->jetty->jetty_id.id);

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
	void *tmp_va;
	uint32_t i;
	int rc;

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

	/* Use posix_memalign instead of memalign for better portability */
	rc = posix_memalign(&tmp_va, PAGE_SIZE, uqpair->seg_len);
	if (rc != 0) {
		SPDK_NOTICELOG("Failed to alloc buffer, rc=%d\n", rc);
		return -1;
	}
	uqpair->va = tmp_va;
	memset(uqpair->va, 0, uqpair->seg_len);

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
		free(uqpair->va);
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
			free(uqpair->va);
			uqpair->va = NULL;
			return -1;
		}
	}

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

	if (uqpair->remote_tseg != NULL && uqpair->remote_tseg->seg.token_id == token_id) {
		return uqpair->remote_tseg;
	}

	TAILQ_FOREACH(entry, &uqpair->remote_segs, link) {
		if (entry->token_id == token_id) {
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

static int
nvmf_ub_handle_connect(struct spdk_nvmf_ub_qpair *uqpair, struct ub_connect_req_rsp *req,
		       struct spdk_sock *sock)
{
	struct spdk_nvmf_ub_transport *utransport;
	struct ub_connect_req_rsp rsp = {0};
	urma_rjetty_t rjetty = {0};
	urma_token_t token = { .token = 0xABCD };
	int rc;

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);

	uqpair->qid = req->qid; /* nvme queue */
	uqpair->remote_jetty_id = req->jetty_id;
	uqpair->remote_uasid = req->uasid;
	memcpy(uqpair->remote_eid.raw, req->eid.raw, URMA_EID_SIZE);
	uqpair->remote_trans_mode = req->trans_mode;

	SPDK_NOTICELOG("Handling connect req\n");
	nvmf_ub_dump_req_rsp(req);

	rc = nvmf_ub_create_jetty(uqpair, (uqpair->qid == 0));
	if (rc < 0) {
		SPDK_ERRLOG("Failed to create jetty for qid %u: %d\n", uqpair->qid, rc);
		goto error;
	}

	rc = nvmf_ub_register_seg(uqpair);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to register segment for qid %u: %d\n", uqpair->qid, rc);
		goto error;
	}

	/* Build remote jetty info */
	rjetty.jetty_id.id = uqpair->remote_jetty_id.id;
	rjetty.jetty_id.uasid = uqpair->remote_uasid;
	memcpy(rjetty.jetty_id.eid.raw, uqpair->remote_eid.raw, URMA_EID_SIZE);
	rjetty.trans_mode = URMA_TM_RM;
	rjetty.type = URMA_JETTY;
	rjetty.tp_type = URMA_CTP;

	/* Import remote jetty using CTP - no bind needed */
	uqpair->target_jetty = urma_import_jetty(utransport->urma_ctx, &rjetty, &token);
	if (uqpair->target_jetty == NULL) {
		SPDK_ERRLOG("urma_import_jetty failed for qid %u\n", uqpair->qid);
		rc = -EIO;
		goto error;
	}

	SPDK_NOTICELOG("Imported remote jetty for qid %u, tpn=%u\n",
		       uqpair->qid, uqpair->target_jetty->tp.tpn);

	/* The initiator advertises its command/payload segment during OOB
	 * connection setup.  Import it once and reuse it for normal staging I/O. */
	uqpair->remote_tseg = nvmf_ub_import_remote_seg(uqpair, req->seg_va, req->seg_len,
						       req->seg_flag, req->seg_token_id);
	if (uqpair->remote_tseg == NULL) {
		SPDK_WARNLOG("Unable to pre-import initiator payload segment for qid %u; "
			     "segments will be imported lazily\n", uqpair->qid);
	}

	/* Create UB resources (pre-allocated requests) */
	uqpair->resources = nvmf_ub_resources_create(uqpair, uqpair->depth);
	if (uqpair->resources == NULL) {
		SPDK_ERRLOG("Failed to create resources for qid %u\n", uqpair->qid);
		rc = -1;
		goto error;
	}

	fprintf(stderr, "start send local info\n");
	/* Build and send connect response */
	rsp.eid = uqpair->jetty->urma_ctx->eid;
    rsp.uasid = uqpair->jetty->jetty_id.uasid;
    rsp.jetty_id = uqpair->jetty->jetty_id;

	rsp.qid = uqpair->qid;
    rsp.msg_type = UB_MSG_TYPE_CONNECT_RSP;
    rsp.trans_mode = URMA_TM_RM;

	fprintf(stderr, "start send local info\n");
    rsp.seg_va = uqpair->local_tseg->seg.ubva.va;
    rsp.seg_len = uqpair->local_tseg->seg.len;
    rsp.seg_flag = uqpair->local_tseg->seg.attr.value;
    rsp.seg_token_id = uqpair->local_tseg->seg.token_id;
	memcpy(rsp.eid.raw, uqpair->jetty->jetty_id.eid.raw, URMA_EID_SIZE);
	fprintf(stderr, "start send local info\n");

	struct iovec rsp_iov = { .iov_base = &rsp, .iov_len = sizeof(rsp) };
	if (spdk_sock_writev(sock, &rsp_iov, 1) != sizeof(rsp)) {
		SPDK_ERRLOG("Failed to send connect response for qid %u\n", uqpair->qid);
		return -1;
	}

	// fprintf(stderr, "DEBUG: %s 821 transport=%llx\n", __func__, &utransport->transport);
	spdk_nvmf_tgt_new_qpair(utransport->transport.tgt, &uqpair->qpair);

	uqpair->state = UB_QPAIR_STATE_RUNNING;

	return 0;

error:
	rsp.msg_type = UB_MSG_TYPE_CONNECT_RSP;
	rsp.qid = -1;
	rsp.jetty_id.id = uqpair->jetty ? uqpair->jetty->jetty_id.id : 0;
	rsp.uasid = 0;
	rsp.trans_mode = URMA_TM_RM;

	struct iovec rsp_iov_err = { .iov_base = &rsp, .iov_len = sizeof(rsp) };
	if (spdk_sock_writev(sock, &rsp_iov_err, 1) != sizeof(rsp)) {
		SPDK_ERRLOG("Failed to send error response for qid %u\n", req->qid);
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
	SPDK_NOTICELOG("UB qpair %u payload stats: read_copy_ios=%" PRIu64
		       " read_copy_bytes=%" PRIu64 " zcopy_read_ios=%" PRIu64
		       " zcopy_write_ios=%" PRIu64 " zcopy_map_failures=%" PRIu64 "\n",
		       uqpair->qid, uqpair->read_copy_ios, uqpair->read_copy_bytes,
		       uqpair->zcopy_read_ios, uqpair->zcopy_write_ios,
		       uqpair->zcopy_map_failures);

	nvmf_ub_unimport_remote_segs(uqpair);

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

	free(uqpair->va);
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

	pending->sock = sock;
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
	pending->req_offset = 0;

	SPDK_NOTICELOG("New connection accepted, waiting for connect request\n");

	rc = spdk_sock_group_add_sock(utransport->pending_sock_group, sock,
				      nvmf_ub_pending_sock_cb, pending);
	if (rc < 0) {
		SPDK_ERRLOG("spdk_sock_group_add_sock failed: %d\n", rc);
		free(pending->uqpair);
		free(pending);
		spdk_sock_close(&sock);
		return;
	}
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

	return rc != 0 ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
nvmf_ub_pending_sock_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock)
{
	struct spdk_nvmf_ub_pending_conn *pending = ctx;
	struct ub_connect_req_rsp *req;
	ssize_t ret;
	int rc;
	SPDK_NOTICELOG("spdk_sock_recv imm \n");

	req = &pending->req;

	/* Continue receiving connect request */
	ret = spdk_sock_recv(sock, (char *)req + pending->req_offset,
			     sizeof(*req) - pending->req_offset);

	SPDK_NOTICELOG("spdk_sock_recv %ld  offset %ld \n", ret, pending->req_offset);

	if (ret < 0) {
		SPDK_ERRLOG("Failed to recv connect request: %zd\n", ret);
		return;
	}

	if (ret == 0) {
		SPDK_ERRLOG("socket closed by peer: %zd\n", ret);
		// fprintf(stderr, "DEBUG: %s 980 transport=%llx\n", __func__, (void*)pending->uqpair->qpair.transport);
		goto cleanup;
	}

	pending->req_offset += ret;

	/* Check if we received full request */
	if (pending->req_offset < sizeof(*req)) {
		SPDK_ERRLOG("Failed to recv connect request: < \n");
		return;
	}

		/* Check if we received full request */
	if (pending->req_offset > sizeof(*req)) {
		SPDK_ERRLOG("Failed to recv connect request: > \n");
		goto cleanup;
	}

	/* Full request received, process it */
	if (req->msg_type != UB_MSG_TYPE_CONNECT) {
		SPDK_ERRLOG("Invalid message type: %u\n", req->msg_type);
		goto cleanup;
	}

	rc = nvmf_ub_handle_connect(pending->uqpair, req, sock);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to handle connect: %d\n", rc);
		goto cleanup;
	}

	// fprintf(stderr, "DEBUG: %s 1013 transport=%llx\n", __func__, (void*)pending->uqpair->qpair.transport);
	SPDK_NOTICELOG("UB qpair connected: qid=%u\n", pending->uqpair->qid);

	/* The socket is only used for the out-of-band connection exchange.
	 * The qpair now belongs to the NVMf core, but the pending context and
	 * socket must be released before the pending sock group is destroyed. */
	rc = spdk_sock_group_remove_sock(group, sock);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to remove connected UB socket from pending group: %d\n", rc);
	}
	spdk_sock_close(&sock);
	free(pending);
	return;

cleanup:
	rc = spdk_sock_group_remove_sock(group, sock);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to remove UB socket from pending group: %d\n", rc);
	}
	nvmf_ub_qpair_destroy(pending->uqpair);
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
		SPDK_NOTICELOG("*** accept one ***\n");
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
	struct spdk_nvmf_ub_transport *utransport;
	struct spdk_nvmf_ub_port *port;
	int trsvcid_int;
	bool connection_infra_created = false;
	int rc;

	SPDK_NOTICELOG("*** nvmf_ub_listen ***\n");

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

	SPDK_NOTICELOG("*** NVMe/UB Target Listening on %s port %s ***\n",
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
	struct spdk_nvmf_ub_transport *utransport;
	struct spdk_nvmf_ub_port *port;
	int rc;

	SPDK_NOTICELOG("*** nvmf_ub_stop_listen ***\n");

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
	if (!ugroup) {
		return;
	}

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
	TAILQ_INIT(&uqpair->reqs);
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
nvmf_ub_post_ub_read(struct spdk_nvmf_ub_request *ub_req, void *local_buf,
		     urma_target_seg_t *local_tseg)
{
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(ub_req->req.qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	urma_target_seg_t *remote_tseg;
	urma_sge_t remote_sge, local_sge;
	urma_sg_t remote_sg, local_sg;
	urma_rw_wr_t ub_rw_wr;
	urma_jfs_wr_t read_wr = {0};
	urma_jfs_wr_t *bad_wr = NULL;

	remote_tseg = nvmf_ub_get_remote_tseg(uqpair, ub_req->remote_addr,
			ub_req->req.length, ub_req->remote_key);
	if (remote_tseg == NULL) {
		return -EFAULT;
	}

	remote_sge.addr = ub_req->remote_addr;
	remote_sge.len = ub_req->req.length;
	remote_sge.tseg = remote_tseg;
	remote_sg.sge = &remote_sge;
	remote_sg.num_sge = 1;

	local_sge.addr = (uint64_t)(uintptr_t)local_buf;
	local_sge.len = ub_req->req.length;
	local_sge.tseg = local_tseg;
	local_sg.sge = &local_sge;
	local_sg.num_sge = 1;

	ub_rw_wr.src = remote_sg;
	ub_rw_wr.dst = local_sg;
	read_wr.opcode = URMA_OPC_READ;
	read_wr.flag.bs.complete_enable = 1;
	read_wr.tjetty = uqpair->target_jetty;
	read_wr.user_ctx = (uint64_t)(uintptr_t)ub_req;
	read_wr.rw = ub_rw_wr;

	ub_req->awaiting_ub_read_completion = true;
	ub_req->ub_state = UB_REQ_UB_STATE_WAIT_READ;
	if (urma_post_jetty_send_wr(uqpair->jetty, &read_wr, &bad_wr) != URMA_SUCCESS) {
		ub_req->awaiting_ub_read_completion = false;
		ub_req->ub_state = UB_REQ_UB_STATE_NONE;
		return -EIO;
	}

	return 0;
}

static int
nvmf_ub_post_ub_write(struct spdk_nvmf_ub_request *ub_req, void *local_buf,
		      urma_target_seg_t *local_tseg)
{
	struct spdk_nvmf_ub_qpair *uqpair = SPDK_CONTAINEROF(ub_req->req.qpair,
		struct spdk_nvmf_ub_qpair, qpair);
	urma_target_seg_t *remote_tseg;
	urma_sge_t remote_sge, local_sge;
	urma_sg_t remote_sg, local_sg;
	urma_rw_wr_t ub_rw_wr;
	urma_jfs_wr_t write_wr = {0};
	urma_jfs_wr_t *bad_wr = NULL;

	remote_tseg = nvmf_ub_get_remote_tseg(uqpair, ub_req->remote_addr,
			ub_req->req.length, ub_req->remote_key);
	if (remote_tseg == NULL) {
		return -EFAULT;
	}

	remote_sge.addr = ub_req->remote_addr;
	remote_sge.len = ub_req->req.length;
	remote_sge.tseg = remote_tseg;
	remote_sg.sge = &remote_sge;
	remote_sg.num_sge = 1;

	local_sge.addr = (uint64_t)(uintptr_t)local_buf;
	local_sge.len = ub_req->req.length;
	local_sge.tseg = local_tseg;
	local_sg.sge = &local_sge;
	local_sg.num_sge = 1;

	ub_rw_wr.src = local_sg;
	ub_rw_wr.dst = remote_sg;
	write_wr.opcode = URMA_OPC_WRITE;
	write_wr.flag.bs.complete_enable = 1;
	write_wr.tjetty = uqpair->target_jetty;
	write_wr.user_ctx = (uint64_t)(uintptr_t)ub_req;
	write_wr.rw = ub_rw_wr;

	ub_req->awaiting_ub_write_completion = true;
	ub_req->ub_state = UB_REQ_UB_STATE_WAIT_WRITE;
	if (urma_post_jetty_send_wr(uqpair->jetty, &write_wr, &bad_wr) != URMA_SUCCESS) {
		ub_req->awaiting_ub_write_completion = false;
		ub_req->ub_state = UB_REQ_UB_STATE_NONE;
		return -EIO;
	}

	return 0;
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
nvmf_ub_handle_cmd(struct spdk_nvmf_ub_qpair *uqpair, uint32_t recv_slot)
{
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	struct spdk_nvmf_ub_request *ub_req;
	struct spdk_nvmf_request *nvmf_req;
	struct spdk_nvme_sgl_descriptor *sgl;
	void *local_buf;
	uint32_t buf_idx;
	uint32_t data_len;
	void *cmd;

	if (resources == NULL || recv_slot >= uqpair->depth) {
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
			/* Store remote info for UB operations in ub_req */
			ub_req->data_from_remote = true;
			ub_req->remote_addr = sgl->address;
			ub_req->remote_key = sgl->keyed.key;

			SPDK_DEBUGLOG(ub, "Keyed data block: local_buf=%p, remote_addr=0x%" PRIx64
				      ", len=%u, key=0x%x, xfer=%d\n",
				      local_buf, sgl->address, sgl->keyed.length, sgl->keyed.key,
				      nvmf_req->xfer);

			if ((nvmf_req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER ||
			     nvmf_req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) &&
			    nvmf_ctrlr_use_zcopy(nvmf_req)) {
				nvmf_req->data_from_pool = false;
				spdk_nvmf_request_zcopy_start(nvmf_req);
				return;
			}

			if (nvmf_req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
				spdk_nvmf_request_exec(nvmf_req);
				return;
			}

			/* If this is a Host-to-Controller transfer, we need to UB READ the data
			 * from client's remote memory before executing the command. */
			if (nvmf_req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				if (nvmf_ub_post_ub_read(ub_req, local_buf,
							  uqpair->local_tseg) != 0) {
					SPDK_ERRLOG("Failed to post UB READ WR\n");
					nvmf_req->rsp->nvme_cpl.cid = nvmf_req->cmd->nvme_cmd.cid;
					nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					nvmf_ub_post_send_response(ub_req);
					return;
				}
				return;
			}
		}
		break;

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
					nvmf_ub_handle_cmd(uqpair, (uint32_t)cr->user_ctx);

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
							completed_ub_req->awaiting_ub_read_completion = false;
							completed_ub_req->ub_state = UB_REQ_UB_STATE_NONE;
							if (completed_ub_req->zcopy_abort ||
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
							completed_ub_req->awaiting_ub_write_completion = false;
							completed_ub_req->ub_state = UB_REQ_UB_STATE_NONE;
							if (completed_ub_req->zcopy_abort ||
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
	.dump_opts = nvmf_ub_dump_opts,
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
