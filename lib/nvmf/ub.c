/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include <time.h>

#include "spdk/config.h"
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

bool g_nvmf_urma_initialized = false;

#define SPDK_NVMF_UB_DEFAULT_MAX_QUEUE_DEPTH 128
#define SPDK_NVMF_UB_DEFAULT_AQ_DEPTH 128
#define SPDK_NVMF_UB_DEFAULT_MAX_QPAIRS_PER_CTRLR 8 /* 暂时约束最大8 queue*/
#define SPDK_NVMF_UB_DEFAULT_IN_CAPSULE_DATA_SIZE 4096
#define SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE 8192 /* 暂时约束单SGL 8K IO */
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


enum spdk_nvmf_ub_request_state { /* 暂时和RDMA相同 */
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

	/* Listen socket for accepting connections */
	struct spdk_sock			*listen_sock;
	struct spdk_poller			*accept_poller;

	/* Pending connections sock_group for handling connect requests */
	struct spdk_sock_group		*pending_sock_group;
	struct spdk_poller			*pending_poller;

	struct spdk_nvmf_transport_poll_group *ugroup;

	TAILQ_HEAD(, spdk_nvmf_ub_device)	devices;
	/* List of ports */
	TAILQ_HEAD(, spdk_nvmf_ub_port)	ports;

	/* Cached listen transport ID - set during nvmf_ub_listen */
	struct spdk_nvme_transport_id	listen_trid;
};

/* Assuming rdma_cm uses just one protection domain per ibv_context. */
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

struct spdk_nvmf_ub_request {
	struct spdk_nvmf_request		req;

	/* Buffer index for data_bufs allocation */
	uint32_t				buf_idx;

	/* Remote address and key for RDMA operations */
	uint64_t				remote_addr;
	uint32_t				remote_key;

	/* Flag indicating if data needs to be fetched from remote */
	bool					data_from_remote;

	/* Flag indicating urma_read was issued and we're waiting for completion
	 * before calling exec(). Only used for HOST_TO_CONTROLLER transfers.
	 */
	bool					awaiting_rdma_read_completion;
	bool					awaiting_rdma_write_completion;
	STAILQ_ENTRY(spdk_nvmf_ub_request)		link;
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

	void 				*va;
	void 				*va_rsp;
	urma_target_seg_t   *local_tseg;

	TAILQ_HEAD(, spdk_nvmf_request)	reqs;

	/* Callback for qpair destruction */
	spdk_nvmf_transport_qpair_fini_cb	fini_cb_fn;
	void					*fini_cb_arg;

	TAILQ_ENTRY(spdk_nvmf_ub_qpair)		link;
};

/* UB transport resources - pre-allocated requests for a qpair */
struct spdk_nvmf_ub_resources {
	/* Array of size "max_queue_depth" containing UB requests. */
	struct spdk_nvmf_ub_request		*reqs;

	/* Array of size "max_queue_depth" containing 16 byte completions
	 * to be sent back to the user.
	 */
	union nvmf_c2h_msg			*cpls;
	urma_target_seg_t	*local_tseg_send; /* Exported target segment for read/write/atomic */

	/* Data buffers for I/O operations (e.g., Identify responses).
	 * Each buffer is max_io_size bytes to support large data transfers.
	 */
	void					*data_bufs;

	struct spdk_nvmf_ub_transport *utransport;
	struct urma_target_seg_t  *tseg;

	/* Queue to track free requests */
	STAILQ_HEAD(, spdk_nvmf_ub_request)	free_queue;

	/* Remote peer information - used for RDMA operations */
	urma_jetty_id_t		remote_jetty_id;
	uint32_t		remote_uasid;
	urma_eid_t		remote_eid;
	urma_transport_mode_t	remote_trans_mode;
};

struct spdk_nvmf_ub_port {
	struct spdk_nvme_transport_id	*trid;
	struct spdk_nvmf_transport		*transport;
	struct spdk_nvmf_ub_device		*device;
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

	/* Create URMA context with the device, eid_index 0 by default */
	utransport->urma_ctx = urma_create_context(dev, 0);
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

	nvmf_ub_create_urma(utransport);
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

	uqpair->depth = is_admin_qpair ? SPDK_NVMF_UB_DEFAULT_AQ_DEPTH : SPDK_NVMF_UB_DEFAULT_MAX_QUEUE_DEPTH; /* 128 */

	/* Create a single shared JFC using transport's JFCE - 参考 urma_server/client */
	urma_jfc_cfg_t jfc_cfg = {
		.depth = uqpair->depth,
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
		return -1;
	}

	SPDK_NOTICELOG("*** create jfr ok! ***\n");

	/* JFS configuration - 使用 URMA_TM_RM 模式 */
	urma_jfs_cfg_t jfs_cfg = {
		.depth           = uqpair->depth,
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
		urma_delete_jfc(uqpair->send_jfc);
		SPDK_ERRLOG("Failed to create jetty\n");
		return -1;
	}

	SPDK_NOTICELOG("Created jetty for qid %u, jetty_id=%u\n",
		       uqpair->qid, uqpair->jetty->jetty_id.id);

	return 0;
}

static int
nvmf_ub_import_remote_jetty(struct spdk_nvmf_ub_qpair *uqpair)
{
	struct spdk_nvmf_ub_transport *utransport;
	urma_rjetty_t rjetty = {0};
	urma_token_t token = { .token = 0xABCD };

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);

	rjetty.jetty_id.id = uqpair->remote_jetty_id.id;
	rjetty.jetty_id.uasid = uqpair->remote_uasid;
	memcpy(rjetty.jetty_id.eid.raw, uqpair->remote_eid.raw, URMA_EID_SIZE);
	rjetty.trans_mode = URMA_TM_RM;
	rjetty.type = URMA_JETTY;
	rjetty.tp_type = URMA_CTP;

	uqpair->target_jetty = urma_import_jetty(utransport->urma_ctx, &rjetty, &token);
	if (uqpair->target_jetty == NULL) {
		SPDK_ERRLOG("urma_import_jetty failed for qid %u, remote_jetty_id=%u\n",
			    uqpair->qid, uqpair->remote_jetty_id.id);
		return -EIO;
	}

	SPDK_NOTICELOG("Imported remote jetty for qid %u, remote_jetty_id=%u, tpn=%u\n",
		       uqpair->qid, uqpair->remote_jetty_id.id, uqpair->target_jetty->tp.tpn);

	/* CTP mode does not require bind_jetty - library handles TP internally */

	return 0;
}

static int
nvmf_ub_register_seg(struct spdk_nvmf_ub_qpair *uqpair)
{
	int i;
	struct spdk_nvmf_ub_transport *utransport;
	void *tmp_va;
	int rc;

	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);
	if (utransport == NULL || utransport->urma_ctx == NULL) {
		SPDK_ERRLOG("Invalid utransport or urma_ctx\n");
		return -1;
	}

	/* Use posix_memalign instead of memalign for better portability */
	rc = posix_memalign(&tmp_va, PAGE_SIZE, PAGE_SIZE * 512);
	if (rc != 0) {
		SPDK_NOTICELOG("Failed to alloc buffer, rc=%d\n", rc);
		return -1;
	}
	uqpair->va = tmp_va;
	memset(uqpair->va, 7, PAGE_SIZE * 512);
	uint64_t *va64 = (uint64_t *)uqpair->va;

    urma_reg_seg_flag_t flag = {
        .bs.token_policy = URMA_TOKEN_NONE,
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
        .bs.token_id_valid = 0,
        .bs.reserved = 0
    };

    urma_seg_cfg_t seg_cfg = {
        .va          = (uint64_t)uqpair->va,
        .len         = PAGE_SIZE * 512,
		.token_id	 = NULL,
        .token_value = { 0xABCD },
        .flag        = flag,
		.user_ctx 	 = 0x5678,
		.iova		 = 0
    };

    uqpair->local_tseg = urma_register_seg(utransport->urma_ctx, &seg_cfg);
    if (uqpair->local_tseg == NULL) {
        SPDK_NOTICELOG("Failed to register segment\n");
        free(uqpair->va);
        return -1;
    }
	fprintf(stderr, "DEBUG: %s 678 va=%llx, local_tseg=%llx\n", __func__, uqpair->va, uqpair->local_tseg);

	urma_sge_t src_sge = {0};
	urma_sg_t src_sg = {0};
	urma_jfr_wr_t *bad_wr = NULL;

	for (i = 0; i < 128; i++) {
		urma_jfr_wr_t wr = {0};
        src_sge.addr = (uint64_t)uqpair->va + i * MSG_SIZE;
        src_sge.len = MSG_SIZE;
        src_sge.tseg = uqpair->local_tseg;
        src_sg.sge = &src_sge;
        src_sg.num_sge = 1;
        wr.src = src_sg;
        wr.user_ctx = i;
        wr.next = NULL;
        if (urma_post_jetty_recv_wr(uqpair->jetty, &wr, &bad_wr) != URMA_SUCCESS) {
            SPDK_NOTICELOG("Failed to recv %i in server jfr thread\n", i);
            free(uqpair->va);
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
	uint32_t i;
	uint32_t data_buf_size;

	resources = calloc(1, sizeof(*resources));
	if (!resources) {
		SPDK_ERRLOG("Unable to allocate ub_resources for qpair %u\n", uqpair->qid);
		return NULL;
	}

	resources->reqs = spdk_zmalloc(max_queue_depth * sizeof(*resources->reqs),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);
	resources->cpls = spdk_zmalloc(max_queue_depth * sizeof(*resources->cpls),
				       0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	/* Allocate data buffers for I/O operations (e.g., Identify responses).
	 * Size per buffer is max_io_size to support large data transfers.
	 */
	data_buf_size = SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE; /* 8192 bytes */
	resources->data_bufs = spdk_zmalloc(max_queue_depth * data_buf_size,
					    0x1000, NULL, SPDK_ENV_LCORE_ID_ANY, SPDK_MALLOC_DMA);

	if (!resources->reqs || !resources->cpls || !resources->data_bufs) {
		SPDK_ERRLOG("Unable to allocate sufficient memory for UB qpair %u\n", uqpair->qid);
		spdk_free(resources->reqs);
		spdk_free(resources->cpls);
		spdk_free(resources->data_bufs);
		free(resources);
		return NULL;
	}

	STAILQ_INIT(&resources->free_queue);

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

		STAILQ_INSERT_TAIL(&resources->free_queue, ub_req, link);
	}

	SPDK_DEBUGLOG(ub, "Created UB resources for qpair %u with %u requests, data_buf_size=%u\n",
		      uqpair->qid, max_queue_depth, data_buf_size);

	return resources;
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

	/* Create UB resources (pre-allocated requests) */
	uqpair->resources = nvmf_ub_resources_create(uqpair, uqpair->depth);
	if (uqpair->resources == NULL) {
		SPDK_ERRLOG("Failed to create resources for qid %u\n", uqpair->qid);
		rc = -1;
		goto error;
	}

	uqpair->resources->utransport = utransport;

	/* Copy remote peer information to resources for RDMA operations */
	uqpair->resources->remote_jetty_id = uqpair->remote_jetty_id;
	uqpair->resources->remote_uasid = uqpair->remote_uasid;
	uqpair->resources->remote_eid = uqpair->remote_eid;
	uqpair->resources->remote_trans_mode = uqpair->remote_trans_mode;

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

	if (resources->tseg) {
		urma_unimport_seg(resources->tseg);
		resources->tseg = NULL;
	}

	if (resources->local_tseg_send) {
		urma_unregister_seg(resources->local_tseg_send);
		resources->local_tseg_send = NULL;
	}

	spdk_free(resources->data_bufs);
	spdk_free(resources->cpls);
	spdk_free(resources->reqs);
	free(resources);
}

static int
nvmf_ub_qpair_destroy(struct spdk_nvmf_ub_qpair *uqpair)
{
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

	if (uqpair->resources) {
		nvmf_ub_resources_destroy(uqpair->resources);
		uqpair->resources = NULL;
	}

	free(uqpair);

	return 0;
}

static void
nvmf_ub_pending_sock_cb(void *ctx, struct spdk_sock_group *group, struct spdk_sock *sock);

static void
nvmf_ub_handle_accept(struct spdk_nvmf_transport *transport, struct spdk_sock *sock)
{
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

	/* Remove from sock_group before handling connect */
	//spdk_sock_group_remove_sock(group, sock);

	rc = nvmf_ub_handle_connect(pending->uqpair, req, sock);
	if (rc < 0) {
		SPDK_ERRLOG("Failed to handle connect: %d\n", rc);
		goto cleanup;
	}

	// fprintf(stderr, "DEBUG: %s 1013 transport=%llx\n", __func__, (void*)pending->uqpair->qpair.transport);
	SPDK_NOTICELOG("UB qpair connected: qid=%u\n", pending->uqpair->qid);

	urma_seg_t remote_seg = {0};
	remote_seg.ubva.eid = req->eid;
	remote_seg.ubva.uasid = req->uasid;
	remote_seg.ubva.va = req->seg_va;
	remote_seg.len = req->seg_len;
	remote_seg.attr.bs.token_policy = URMA_TOKEN_NONE;
	remote_seg.attr.bs.cacheable = URMA_NON_CACHEABLE;
	remote_seg.token_id = req->seg_token_id;

	urma_token_t token = {0};
	token.token = 0xABCD;

	urma_import_seg_flag_t flag = {0};
	flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;

	urma_target_seg_t *tseg;
	tseg = urma_import_seg(pending->uqpair->jetty->urma_ctx, &remote_seg, &token, 0, flag);
	if (tseg == NULL) {
		fprintf(stderr, "Failed to import remote segment\n");
		return ;
	}

	return;

cleanup:
	spdk_sock_group_remove_sock(group, sock);
	// free(pending->uqpair);
	free(pending);
	// fprintf(stderr, "DEBUG: %s 1022 transport=%llx\n", __func__, (void*)pending->uqpair->qpair.transport);
	spdk_sock_close(&sock);
}

static int
nvmf_ub_accept(void *ctx)
{
	struct spdk_nvmf_transport *transport = ctx;
	struct spdk_nvmf_ub_transport *utransport;
	struct spdk_sock *sock;
	int i;

	utransport = nvmf_ub_get_transport(transport);

	if (utransport->listen_sock == NULL) {
		return SPDK_POLLER_IDLE;
	}

	for (i = 0; i < SPDK_NVMF_UB_MAX_ACCEPT_SOCK_ONE_TIME; i++) {
		sock = spdk_sock_accept(utransport->listen_sock);
		if (sock == NULL) {
			break;
		}
		SPDK_NOTICELOG("*** accept one ***\n");
		nvmf_ub_handle_accept(transport, sock);
	}

	return SPDK_POLLER_IDLE;
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

	SPDK_NOTICELOG("*** nvmf_ub_listen ***\n");

	if (!strlen(trid->trsvcid)) {
		SPDK_ERRLOG("Service id is required\n");
		return -1;
	}

	/* dont support multi listen sock */
	/* port seems to be useless */

	utransport = nvmf_ub_get_transport(transport);

	trsvcid_int = nvmf_ub_trsvcid_to_int(trid->trsvcid);
	if (trsvcid_int < 0) {
		SPDK_ERRLOG("Invalid trsvcid '%s'\n", trid->trsvcid);
		return -1;
	}

	if (utransport->listen_sock == NULL) {
		utransport->pending_sock_group = spdk_sock_group_create(NULL);
		if (utransport->pending_sock_group == NULL) {
			SPDK_ERRLOG("spdk_sock_group_create for pending failed\n");
			return -1;
		}

		utransport->listen_sock = spdk_sock_listen(trid->traddr, trsvcid_int, NULL);
		if (utransport->listen_sock == NULL) {
			SPDK_ERRLOG("spdk_sock_listen(%s, %d) failed\n", trid->traddr, trsvcid_int);
			spdk_sock_group_close(&utransport->pending_sock_group);
			utransport->pending_sock_group = NULL;
			return -1;
		}

		utransport->accept_poller = SPDK_POLLER_REGISTER(nvmf_ub_accept, transport, 0);
		utransport->pending_poller = SPDK_POLLER_REGISTER(nvmf_ub_pending_poll, transport, 0);
	}

	port = calloc(1, sizeof(*port));
	if (!port) {
		SPDK_ERRLOG("Port allocation failed\n");
		return -1;
	}

	port->trid = malloc(sizeof(*port->trid));
	if (port->trid == NULL) {
		SPDK_ERRLOG("Failed to allocate trid copy\n");
		free(port);
		return -1;
	}
	*port->trid = *trid;
	port->transport = transport;

	TAILQ_INSERT_TAIL(&utransport->ports, port, link);

	/* Cache the listen trid - used by qpair_get_listen_trid */
	utransport->listen_trid = *trid;

	SPDK_NOTICELOG("*** NVMe/UB Target Listening on %s port %s ***\n",
		       trid->traddr, trid->trsvcid);

	return 0;
}

static void
nvmf_ub_stop_listen(struct spdk_nvmf_transport *transport,
		      const struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_ub_transport *utransport;
	struct spdk_nvmf_ub_port *port;

	SPDK_NOTICELOG("*** nvmf_ub_stop_listen ***\n");

	utransport = nvmf_ub_get_transport(transport);

	port = nvmf_ub_find_port(utransport, trid);
	if (port) {
		TAILQ_REMOVE(&utransport->ports, port, link);
		free(port->trid);
		free(port);
	}

	if (TAILQ_EMPTY(&utransport->ports)) {
		if (utransport->pending_poller) {
			spdk_poller_unregister(&utransport->pending_poller);
			utransport->pending_poller = NULL;
		}
		if (utransport->accept_poller) {
			spdk_poller_unregister(&utransport->accept_poller);
			utransport->accept_poller = NULL;
		}
		if (utransport->listen_sock) {
			spdk_sock_close(&utransport->listen_sock);
			utransport->listen_sock = NULL;
		}
		if (utransport->pending_sock_group) {
			spdk_sock_group_close(&utransport->pending_sock_group);
			utransport->pending_sock_group = NULL;
		}
	}
}


int test = 0;

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

	struct spdk_nvmf_ub_transport *utransport;
	utransport = nvmf_ub_get_transport(transport);
	utransport->ugroup = ugroup;
	if (!ugroup->crs) {
		SPDK_ERRLOG("UB poll_group_create: crs allocation failed\n");
		return NULL;
	}

	TAILQ_INIT(&ugroup->qpairs);

	return &ugroup->group;
}

static struct spdk_nvmf_transport_poll_group *
nvmf_ub_get_optimal_poll_group(struct spdk_nvmf_qpair *qpair)
{
	SPDK_NOTICELOG("*** nvmf_ub_get_optimal_poll_group ***\n");
	return NULL;
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

	if (ugroup->group.transport == NULL) {
		/* Transport can be NULL when nvmf_ub_poll_group_create()
		 * calls this function directly in a failure path. */
		free(ugroup);
		return;
	}

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
	struct spdk_nvmf_ub_transport 	*utransport;

	ugroup = SPDK_CONTAINEROF(group, struct spdk_nvmf_ub_poll_group, group);
	uqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_ub_qpair, qpair);

	resources = uqpair->resources;
	utransport = resources->utransport;

	test = 1;

	urma_reg_seg_flag_t flag = {
        .bs.token_policy = URMA_TOKEN_NONE,
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
        .bs.token_id_valid = 0,
        .bs.reserved = 0
    };

    urma_seg_cfg_t seg_cfg = {
        .va = (uint64_t)resources->cpls,
        .len = sizeof(*resources->cpls) * uqpair->depth,
        .token_id = NULL,
        .token_value = { 0xABCD },
        .flag = flag,
        .user_ctx = (uintptr_t)NULL,
        .iova = 0
    };
	fprintf(stderr, "DEBUG: %s 1291 resources->cpls=%llx\n", __func__, resources->cpls);

    
	/*
    resources->local_tseg_send = urma_register_seg(utransport->urma_ctx, &seg_cfg);
	fprintf(stderr, "DEBUG: %s 1293 resources->local_tseg_send=%llx\n", __func__, resources->local_tseg_send);
    if (local_tseg == NULL) {
        SPDK_NOTICELOG("Failed to register segment\n");
        free(va);
        return -1;
    }
	*/
	

	TAILQ_INIT(&uqpair->reqs);
	TAILQ_INSERT_TAIL(&ugroup->qpairs, uqpair, link);

	return 0;
}

static int
nvmf_ub_poll_group_remove(struct spdk_nvmf_transport_poll_group *group,
			    struct spdk_nvmf_qpair *qpair)
{
	SPDK_NOTICELOG("*** nvmf_ub_poll_group_remove ***\n");
	return 0;
}

int polled_sum;
int polled_sum_recv;
int polled_sum_send;
static int poll_call_count = 0;
static time_t last_print_time = 0;


static void
nvmf_ub_handle_cmd(struct spdk_nvmf_ub_qpair *uqpair, void *cmd)
{
	struct spdk_nvmf_ub_resources *resources = uqpair->resources;
	struct spdk_nvmf_ub_request *ub_req;
	struct spdk_nvmf_request *nvmf_req;
	struct spdk_nvme_sgl_descriptor *sgl;

	if (resources == NULL) {
		SPDK_ERRLOG("UB qpair %u has no resources\n", uqpair->qid);
		return;
	}

	/* Get a free request from the pool */
	ub_req = STAILQ_FIRST(&resources->free_queue);
	if (ub_req == NULL) {
		SPDK_ERRLOG("UB qpair %u has no free requests\n", uqpair->qid);
		return;
	}
	STAILQ_REMOVE_HEAD(&resources->free_queue, link);

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
	fprintf(stderr, "ub_req->buf_idx %x %llx %llx\n", ub_req->buf_idx, ub_req, resources->reqs);
	ub_req->data_from_remote = false; 
	ub_req->remote_addr = 0;
	ub_req->remote_key = 0;
	ub_req->awaiting_rdma_read_completion = false;

	/* Parse SGL to determine data buffer location */
	sgl = &nvmf_req->cmd->nvme_cmd.dptr.sgl1;

	uint32_t buf_idx = ub_req - resources->reqs;
	void *local_buf = (uint8_t *)uqpair->va + 256 * PAGE_SIZE+ buf_idx * SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE;

	nvmf_req->iov[0].iov_base = local_buf;
	nvmf_req->iov[0].iov_len = sgl->keyed.length;
	nvmf_req->length = sgl->keyed.length;
	nvmf_req->iovcnt = 1;
	fprintf(stderr, "buf_idex=%d\n", buf_idx);
	fprintf(stderr, "[DEBUG]:%s iov_base=0x%llx, iov_len=%d\n", __func__ ,nvmf_req->iov[0].iov_base, nvmf_req->iov[0].iov_len);
	uint64_t *va64 = nvmf_req->iov[0].iov_base;
	fprintf(stderr, "-------------------------------\n");
	fprintf(stderr, "the iov_base start context is:\n");
	for (int j = 0; j < 8; j++) {
		fprintf(stderr, "0x%llx  **\n", va64[j]);
	}
	fprintf(stderr, "-------------------------------\n");

	if (nvmf_req->xfer == SPDK_NVME_DATA_NONE) {
		/* No data transfer - command only */
		SPDK_NOTICELOG("SPDK_NVME_DATA_NONE\n");
		nvmf_req->iovcnt = 0;
	}

	if (nvmf_req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		SPDK_NOTICELOG("SPDK_NVME_DATA_CONTROLLER_TO_HOST\n");
		spdk_nvmf_request_exec(nvmf_req);
		return;
	}

	/* Handle different SGL types */
	switch (sgl->keyed.type) {
	case SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK:
		if (sgl->keyed.subtype == SPDK_NVME_SGL_SUBTYPE_ADDRESS) {
			/* Store remote info for UB operations in ub_req */
			ub_req->data_from_remote = true;
			ub_req->remote_addr = sgl->address;
			ub_req->remote_key = sgl->keyed.key;

			fprintf(stderr, "Keyed data block: local_buf=%p, remote_addr=0x%lx, len=%u, key=0x%x, xfer=%d\n",
				      local_buf, sgl->address, sgl->keyed.length, sgl->keyed.key, nvmf_req->xfer);

			/* If this is a Host-to-Controller transfer, we need to RDMA READ the data
			 * from client's remote memory before executing the command. */
			if (nvmf_req->xfer == SPDK_NVME_DATA_HOST_TO_CONTROLLER) {
				/* Post RDMA READ to fetch data from client */

				/* Unimport previous seg if exists to avoid resource leak */
				if (resources->tseg) {
					urma_unimport_seg(resources->tseg);
					resources->tseg = NULL;
				}

				urma_seg_t remote_seg = {0};
				remote_seg.ubva.eid = resources->remote_eid;
				remote_seg.ubva.uasid = resources->remote_uasid;
				remote_seg.ubva.va = nvmf_req->cmd->nvme_cmd.dptr.sgl1.address;
				remote_seg.len = nvmf_req->cmd->nvme_cmd.dptr.sgl1.keyed.length;
				remote_seg.attr.bs.token_policy = URMA_TOKEN_NONE;
				remote_seg.attr.bs.cacheable = URMA_NON_CACHEABLE;
				remote_seg.token_id = nvmf_req->cmd->nvme_cmd.dptr.sgl1.keyed.key;

				urma_token_t token = {0};
				token.token = 0xABCD;

				urma_import_seg_flag_t flag = {0};
				flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;

				resources->tseg = urma_import_seg(uqpair->jetty->urma_ctx, &remote_seg, &token, 0, flag);
				if (resources->tseg == NULL) {
					fprintf(stderr, "Failed to import remote segment\n");
					return ;
				}

				urma_sg_t dst_sg = {
					.sge = &(urma_sge_t){
						.addr = sgl->address,
						.len = sgl->keyed.length,
						.tseg = resources->tseg
					},
					.num_sge = 1
				};
				urma_sg_t src_sg = {
					.sge = &(urma_sge_t){
						// .addr = (uint64_t)uqpair->va + PAGE_SIZE * 256,
						.addr = local_buf,
						.len = sgl->keyed.length,
						.tseg = uqpair->local_tseg  /* Remote memory, no local tseg */
					},
					.num_sge = 1
				};

				urma_rw_wr_t rdma_wr = {
					.src = dst_sg,
					.dst = src_sg
				};

				ub_req->awaiting_rdma_read_completion = 1;
				urma_jfs_wr_t read_wr = {
					.opcode = URMA_OPC_READ,
					.flag.bs.complete_enable = 1,
					.tjetty = uqpair->target_jetty,
					.user_ctx = (uint64_t)(uintptr_t)ub_req,
					.rw = rdma_wr,
					.next = NULL
				};
				urma_jfs_wr_t *bad_wr = NULL;

				SPDK_NOTICELOG("Posting RDMA READ: remote_addr=0x%llx, local_buf=%llx, len=%u, key=0x%x\n",
					       sgl->address, src_sg.sge->addr, sgl->keyed.length, sgl->keyed.key);

				if (urma_post_jetty_send_wr(uqpair->jetty, &read_wr, &bad_wr) != URMA_SUCCESS) {
					SPDK_ERRLOG("Failed to post RDMA READ WR\n");
					nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
					STAILQ_INSERT_HEAD(&resources->free_queue, ub_req, link);
					return;
				}

				/* Don't execute request yet - wait for RDMA READ to complete */
				return;
			}
		}
		break;

	case SPDK_NVME_SGL_TYPE_DATA_BLOCK:
		if (sgl->unkeyed.subtype == SPDK_NVME_SGL_SUBTYPE_OFFSET) {
			/* Inline data - data is in the same buffer, right after the NVMe command.
			 * The address field contains an offset from the end of the command. */
			uint8_t *cmd_data = (uint8_t *)nvmf_req->cmd;
			void *inline_data = cmd_data + sizeof(struct spdk_nvme_cmd) + sgl->address;

			nvmf_req->iov[0].iov_base = inline_data;
			nvmf_req->iov[0].iov_len = sgl->unkeyed.length;
			nvmf_req->length = sgl->unkeyed.length;
			nvmf_req->iovcnt = 1;

			fprintf(stderr, "Data block (inline): addr=%p, offset=%lu, len=%u, xfer=%d\n",
				      inline_data, (unsigned long)sgl->address, sgl->unkeyed.length, nvmf_req->xfer);
		}
		break;


	default:
		SPDK_ERRLOG("Unhandled SGL type: type=%d, subtype=%d\n",
			    sgl->generic.type, sgl->generic.subtype);
		nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
		break;
	}

	/* Check if SGL parsing failed (iovcnt == 0 but xfer requires data) */
	if (nvmf_req->iovcnt == 0 && nvmf_req->xfer != SPDK_NVME_DATA_NONE) {
		SPDK_ERRLOG("SGL parsing failed: iovcnt=0 but xfer=%d\n", nvmf_req->xfer);
		nvmf_req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_SGL_DESCRIPTOR_TYPE_INVALID;
	}

done:
	/* Dispatch to upper layer (controller/bdev) */
	spdk_nvmf_request_exec(nvmf_req);
}

static int
nvmf_ub_poll_group_poll(struct spdk_nvmf_transport_poll_group *group)
{
	struct spdk_nvmf_ub_poll_group *ugroup     = nvmf_ub_get_poll_group(group);
	struct spdk_nvmf_ub_qpair      *uqpair;
	int                        npolled;
	uint32_t                   i;
	int j;

	poll_call_count++;
	time_t now = time(NULL);
	if (now - last_print_time >= 5) {
		SPDK_WARNLOG("=== UB poll: called %d times in 5s, polled_sum_recv=%d ===\n",
			     poll_call_count, polled_sum_recv);
		poll_call_count = 0;
		last_print_time = now;
	}

	if (test == 1) {
		TAILQ_FOREACH(uqpair, &ugroup->qpairs, link) {
			SPDK_WARNLOG("UB poll: uqpair %d \n", uqpair->remote_jetty_id.id);
		}
		test = 0;
	}


	/* Poll each per-connection qpair's JFC - loop until empty */
	TAILQ_FOREACH(uqpair, &ugroup->qpairs, link) {  // 1、确认是不是同一个jfc 2、发包有没有发
		/* Loop until no more completions */
		while ((npolled = urma_poll_jfc(uqpair->send_jfc,
						(int)ugroup->max_crs,
						ugroup->crs)) > 0) {

			for (i = 0; i < (uint32_t)npolled; i++) {
				urma_cr_t *cr = &ugroup->crs[i];

				SPDK_WARNLOG("--------UB poll: urma_poll_jfc npolled %d max_crs %d qid %d\n", 
								npolled, ugroup->max_crs, uqpair->qid);
				SPDK_WARNLOG("*********UB poll: CR status=%d opcode=%d s_r=%d user_ctx %d**********\n",
						     cr->status, cr->opcode, cr->flag.bs.s_r, cr->user_ctx);
				if (cr->status != URMA_CR_SUCCESS) {
					SPDK_WARNLOG("--------UB poll: CR status=%d opcode=%d s_r=%d\n",
						     cr->status, cr->opcode, cr->flag.bs.s_r);
					continue;
				}

				polled_sum += npolled;

				/*
				 * Distinguish receive completions (s_r == 1) from
				 * send/RDMA completions (s_r == 0).
				 */
				if (cr->flag.bs.s_r == 1) {
					/* Received a command capsule */
					polled_sum_recv += 1;
					SPDK_WARNLOG("UB poll: polled_sum_recv %d len %d\n", polled_sum_recv, cr->completion_len);
					SPDK_WARNLOG("UB poll cr:user_ctx  %lx\n", cr->user_ctx);
					SPDK_WARNLOG("UB poll cr:opcode  %x\n", cr->opcode);
					SPDK_WARNLOG("UB poll cr:status  %x\n", cr->status);
					SPDK_WARNLOG("UB poll cr:user_data  %lx\n", cr->user_data);
					SPDK_WARNLOG("UB poll cr:local id  %x\n", cr->local_id);
					struct spdk_nvmf_fabric_connect_cmd *cmd = uqpair->va + cr->user_ctx * MSG_SIZE;
					SPDK_WARNLOG("spdk_nvmf_fabric_connect_cmd cmd opcode %d\n", cmd->opcode);
					uint64_t *va64 = (uint64_t *)cmd;
					for (j = 0; j < 8; j++) {
						SPDK_WARNLOG("0x%lx\n", va64[j]);
					}


					/* Re-post the receive buffer */
					{
						urma_jfr_wr_t  jfr_wr = {0};
						urma_jfr_wr_t *bad_wr  = NULL;
						urma_sge_t     sge      = {0};
						sge.addr = (uint64_t)(uintptr_t)uqpair->va + cr->user_ctx * MSG_SIZE;
						sge.len  = (uint32_t)MSG_SIZE;
						sge.tseg = uqpair->local_tseg;
						jfr_wr.src.sge     = &sge;
						jfr_wr.src.num_sge = 1;
						jfr_wr.user_ctx    = cr->user_ctx;
						if (urma_post_jetty_recv_wr(uqpair->jetty, &jfr_wr, &bad_wr) != URMA_SUCCESS) {
							SPDK_WARNLOG("UB poll: Failed to re-post recv WR, slot=%lu\n", cr->user_ctx);
						} else {
							SPDK_WARNLOG("UB poll: Re-posted recv WR, slot=%lu\n", cr->user_ctx);
						}
					}

					nvmf_ub_handle_cmd(uqpair, cmd);

				} else {
					/* Send/RDMA completion */
					// 判断当前的req 是dma的还是rsp的
					// 如果是dma的 那么要把计数加上
					struct spdk_nvmf_ub_request *completed_ub_req = (struct spdk_nvmf_ub_request *)(uintptr_t)cr->user_ctx;
					SPDK_WARNLOG("---------UB poll: RDMA/SEND completion, req=%p, opcode=%d\n",
						     completed_ub_req, cr->opcode);

					if (completed_ub_req->awaiting_rdma_read_completion && cr->opcode == URMA_CR_OPC_SEND) {
						/* RDMA READ completed - now we can execute the request */
						SPDK_NOTICELOG("RDMA READ completed, executing request\n");
						spdk_nvmf_request_exec(&completed_ub_req->req);
						completed_ub_req->awaiting_rdma_read_completion = 0;
					} 
					if (completed_ub_req->awaiting_rdma_write_completion && cr->opcode == URMA_CR_OPC_SEND) {
						/* RDMA READ completed - now we can execute the request */
						SPDK_NOTICELOG("RDMA WRITE completed, executing request\n");
						completed_ub_req->awaiting_rdma_write_completion = 0;
					}else {
						SPDK_NOTICELOG("SEND rsp completed!\n");
					}
				}
			}
		}
		if (npolled < 0) {
			SPDK_WARNLOG("UB poll: urma_poll_jfc error %d\n", npolled);
		}
	}

	return 1;
}

int magic_num = 777;

static void
nvmf_ub_req_complete(struct spdk_nvmf_request *req)
{
	SPDK_WARNLOG("UB nvmf_ub_req_complete\n");
	struct spdk_nvmf_ub_transport	*utransport = SPDK_CONTAINEROF(req->qpair->transport,
			struct spdk_nvmf_ub_transport, transport);

	struct spdk_nvmf_ub_request	*ub_req = SPDK_CONTAINEROF(req,
		struct spdk_nvmf_ub_request, req);

	struct spdk_nvmf_ub_qpair     *uqpair = SPDK_CONTAINEROF(ub_req->req.qpair,
		struct spdk_nvmf_ub_qpair, qpair);

	SPDK_DEBUGLOG(ub, "UB req_complete: qid=%u, xfer=%d, iovcnt=%u, length=%u\n",
		      uqpair->qid, req->xfer, req->iovcnt, req->length);

	// 暂时在complete里处理data和rsp
	struct spdk_nvmf_ub_resources *resources= uqpair->resources;
	ub_req->buf_idx = ub_req - resources->reqs;

	if (req->xfer == SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
		urma_seg_t remote_seg = {0};
		remote_seg.ubva.eid = resources->remote_eid;
		remote_seg.ubva.uasid = resources->remote_uasid;
		remote_seg.ubva.va = req->cmd->nvme_cmd.dptr.sgl1.address;
		remote_seg.len = req->cmd->nvme_cmd.dptr.sgl1.keyed.length;
		remote_seg.attr.bs.token_policy = URMA_TOKEN_NONE;
		remote_seg.attr.bs.cacheable = URMA_NON_CACHEABLE;
		remote_seg.token_id = req->cmd->nvme_cmd.dptr.sgl1.keyed.key;

		urma_token_t token = {0};
		token.token = 0xABCD;

		urma_import_seg_flag_t flag = {0};
		flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;

		resources->tseg = urma_import_seg(uqpair->jetty->urma_ctx, &remote_seg, &token, 0, flag);
		if (resources->tseg == NULL) {
			fprintf(stderr, "Failed to import remote segment\n");
			return ;
		}

		void *resp = (void *)((uint8_t *)uqpair->va + PAGE_SIZE * 128 + ub_req->buf_idx * SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE);
		fprintf(stderr, "iov_base=0x%llx, iov_len=%d\n", req->iov[0].iov_base, req->iov[0].iov_len);
		uint64_t *va64 = req->iov[0].iov_base;
		for (int j = 0; j < 8; j++) {
			fprintf(stderr, "0x%llx   **\n", va64[j]);
		}
		memcpy(resp, req->iov[0].iov_base, req->iov[0].iov_len);
		magic_num++;
    	urma_sg_t dst_sg = {
			.sge = &(urma_sge_t){
				.addr = req->cmd->nvme_cmd.dptr.sgl1.address,
				.len = req->cmd->nvme_cmd.dptr.sgl1.keyed.length,
				.tseg = resources->tseg
			},
			.num_sge = 1
		};
		urma_sg_t src_sg = {
			.sge = &(urma_sge_t){
			.addr = (uint64_t)resp,
			.len = req->cmd->nvme_cmd.dptr.sgl1.keyed.length,
			.tseg = uqpair->local_tseg  /* Remote memory, no local tseg */
			},
			.num_sge = 1
		};
		// fprintf(stderr, "va start=%llx\n", uqpair->va);
		// req->iov[0].iov_base = (uint64_t)va + PAGE_SIZE * 128;
		
		urma_rw_wr_t rdma_wr = {
			.src = src_sg,
			.dst = dst_sg
		};

		urma_jfs_wr_t write_wr = {
			.opcode = URMA_OPC_WRITE,
			.flag.bs.complete_enable = 1,
			.tjetty = uqpair->target_jetty,
			.user_ctx = (uint64_t)(uintptr_t)ub_req,
			.rw = rdma_wr,
			.next = NULL
		};
		urma_jfs_wr_t *bad_wr = NULL;
		ub_req->awaiting_rdma_write_completion = 1;

		if (urma_post_jetty_send_wr(uqpair->jetty, &write_wr, &bad_wr) != URMA_SUCCESS) {
			SPDK_WARNLOG("UB poll: Failed to re-post send write WR, slot=0x%llx\n", req);
		} else {
			SPDK_WARNLOG("UB poll: post send write WR, slot=0x%llx\n", req);
		}

		struct spdk_nvmf_ub_transport *utransport;
		utransport = nvmf_ub_get_transport(uqpair->qpair.transport);
		while (ub_req->awaiting_rdma_write_completion) {
			// fprintf(stderr, "awaiting_rdma_write_completion!\n");
			// usleep(10000);
			nvmf_ub_poll_group_poll(utransport->ugroup);
		}

	}

    // post send
	{
		uint64_t offset = 16;
		*(uint64_t *)uqpair->va = magic_num;

		void *resp = uqpair->va + PAGE_SIZE * 128 + ub_req->buf_idx * SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE;
		memcpy(resp, req->rsp, sizeof(union nvmf_c2h_msg));
		magic_num++;
    	urma_sge_t src_sge = {
        	.addr = (uint64_t)resp,
        	.len = 16,
        	.tseg = uqpair->local_tseg,
    	};

		fprintf(stderr, "DEBUG: %s 1696 req->rsp=%llx\n", __func__, req->rsp);
		fprintf(stderr, "DEBUG: %s 1697 va=%llx, local_tseg=%llx\n", __func__, uqpair->va, uqpair->local_tseg);
		if (req->iov[0].iov_base) {
			uint64_t *va64 = req->iov[0].iov_base;
			fprintf(stderr, "-------------------------------\n");
			fprintf(stderr, "the read context is:\n");
			for (int j = 0; j < 8; j++) {
				fprintf(stderr, "0x%llx  **\n", va64[j]);
			}
			fprintf(stderr, "-------------------------------\n");
		}
		

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
        	.user_ctx = (uint64_t)(uintptr_t)ub_req,
        	.send = send_wr,
        	.next = NULL
    	};
    	urma_jfs_wr_t *bad_wr = NULL;

		if (urma_post_jetty_send_wr(uqpair->jetty, &jfs_wr, &bad_wr) != URMA_SUCCESS) {
			SPDK_WARNLOG("UB poll: Failed to re-post recv WR, slot=0x%llx\n", req);
		} else {
			SPDK_WARNLOG("UB poll: post send WR, slot=0x%llx\n", req);
		}
	}

	/* Return the request back to the free queue */
	STAILQ_INSERT_TAIL(&resources->free_queue, ub_req, link);
}

static int
nvmf_ub_qpair_get_listen_trid(struct spdk_nvmf_qpair *qpair,
			       struct spdk_nvme_transport_id *trid)
{
	struct spdk_nvmf_ub_qpair *uqpair;
	struct spdk_nvmf_ub_transport *utransport;

	uqpair = SPDK_CONTAINEROF(qpair, struct spdk_nvmf_ub_qpair, qpair);
	utransport = nvmf_ub_get_transport(uqpair->qpair.transport);

	memcpy(trid, &utransport->listen_trid, sizeof(struct spdk_nvme_transport_id));

	return 0;
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

	.req_free = NULL,
	.req_complete = nvmf_ub_req_complete,
	.req_get_buffers_done = NULL,

	.qpair_fini = NULL,
	.qpair_get_peer_trid = NULL,
	.qpair_get_local_trid = NULL,
	.qpair_get_listen_trid = nvmf_ub_qpair_get_listen_trid,
	.qpair_abort_request = NULL,

	.poll_group_dump_stat = NULL,
};

SPDK_NVMF_TRANSPORT_REGISTER(ub, &spdk_nvmf_transport_ub);
SPDK_LOG_REGISTER_COMPONENT(ub)