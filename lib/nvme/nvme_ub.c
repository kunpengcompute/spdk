/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2016 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019-2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

/*
 * NVMe over UB transport
 */

#include "spdk/stdinc.h"

#include "spdk/assert.h"
#include "spdk/dma.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/config.h"
#include "spdk/sock.h"

#include "nvme_internal.h"
#include "spdk/tree.h"
#include "spdk_internal/sgl.h"

#include "urma/urma_api.h"

#include <sys/uio.h>

/* need UB header */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define MSG_SIZE 4096
#define WRITE_ONCE(x, val) (*(volatile typeof(x) *)&(x) = (val))
#define JETTY_DEPTH  256
#define JFC_DEPTH    1024
#define SEND_CNT    4
#define SEND    0
#define MAX_IO_SIZE  8192
#define NVME_UB_MAX_COMPLETIONS_PER_POLL 128

#define NVME_UQPAIR_ERRLOG(uqpair, format, ...) NVME_QPAIR_ERRLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_WARNLOG(uqpair, format, ...) NVME_QPAIR_WARNLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_NOTICELOG(uqpair, format, ...) NVME_QPAIR_NOTICELOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_INFOLOG(uqpair, format, ...) NVME_QPAIR_INFOLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_DEBUGLOG(uqpair, format, ...) NVME_QPAIR_DEBUGLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)


enum nvme_ub_qpair_state {
    NVME_UB_JETTY_STATE_RESET = 0,
    NVME_UB_JETTY_STATE_CONNECTING = 1,
    NVME_UB_JETTY_STATE_READY = 2,
    NVME_UB_JETTY_STATE_SUSPENDED = 3,
    NVME_UB_JETTY_STATE_ERROR = 4,
    NVME_UB_JETTY_STATE_DISCONNECTING = 5,
};

/* UB qpair connection state machine - similar to TCP/RDMA */
enum nvme_ub_connect_state {
    NVME_UB_QPAIR_STATE_INVALID = 0,
    NVME_UB_QPAIR_STATE_INITIALIZING = 1,
    NVME_UB_QPAIR_STATE_FABRIC_CONNECT_SEND = 2,
    NVME_UB_QPAIR_STATE_FABRIC_CONNECT_POLL = 3,
    NVME_UB_QPAIR_STATE_AUTHENTICATING = 4,
    NVME_UB_QPAIR_STATE_RUNNING = 5,
};

/* Forward declarations */
struct nvme_ub_ctrlr;
struct nvme_ub_qpair;
struct nvme_ub_request;
struct nvme_ub_poll_group;

/* Function forward declarations */
static void nvme_ub_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr);
static void nvme_ub_remove_req(struct nvme_ub_qpair *uqpair, struct nvme_ub_request *ub_req);
static int nvme_ub_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
                   int (*iter_fn)(struct nvme_request *req, void *arg),
                   void *arg);
static void nvme_ub_sock_connect_cb(void *cb_arg, int status);

/* Info exchange structure for socket-based connection establishment */
typedef struct nvme_ub_conn_info {
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
} __attribute__((packed)) nvme_ub_conn_info_t;

struct cq_ctx {
    struct nvme_ub_qpair *uqpair;
    int id;
};

struct nvme_ub_qpair {
    struct spdk_nvme_qpair qpair;
    struct nvme_ub_ctrlr *uctrlr;
    TAILQ_ENTRY(nvme_ub_qpair) ctrlr_link;
    TAILQ_ENTRY(nvme_ub_qpair) group_link;
    TAILQ_ENTRY(nvme_ub_qpair) link_connecting;  /* For connecting_qpairs list */
    TAILQ_ENTRY(nvme_ub_qpair) link_active;      /* For active_qpairs list */

    /* URMA Jetty Queues */
    urma_jfce_t *jfce;
    urma_jfc_t *jfc;
    urma_jetty_t *jetty;
    urma_jfs_t *jfs;        /* Jetty for Send */
    urma_jfr_t *jfr;        /* Jetty for Receive */
    urma_jetty_id_t jetty_id;

    /* Queue Attributes */
    uint16_t sq_depth;
    uint16_t cq_depth;
    uint16_t qid;
    uint16_t num_entries;
    bool delay_cmd_submit;

    /* Request Tracking */
    struct nvme_ub_request **reqs;
    uint16_t num_requests;
    TAILQ_HEAD(, nvme_ub_request) free_reqs;
    TAILQ_HEAD(, nvme_ub_request) outstanding_reqs;
    uint16_t outstanding_requests;
    uint16_t current_num_sends;

    /* Remote target jetty for communication */
    urma_target_jetty_t *tjetty;

    /* Memory Registration */
    urma_target_seg_t *cmd_tseg;
    urma_target_seg_t *resp_tseg;
    urma_target_seg_t *remote_tseg;
    void *cmd_buffer;
    void *resp_buffer;
    uint64_t cmd_buffer_size;
    uint64_t resp_buffer_size;
    uint64_t payload_buffer_offset;
    struct cq_ctx *recv_ctxs;
    uint16_t recv_depth;

    /* Connection State */
    bool is_connected;
    bool error_state;

    /* Qpair State (transport layer) */
    enum nvme_ub_qpair_state state;

    /* Qpair connection state machine (NVMe-oF Fabric layer) */
    enum nvme_ub_connect_state qpair_state;

    /* SPDK socket for connection establishment */
    struct spdk_sock *sock;

    /* Synchronization */
    pthread_mutex_t lock;
};

/* URMA Request Structure */
struct nvme_ub_request {
    struct nvme_request *req;
    struct nvme_ub_qpair *uqpair;
    uint16_t id;

    /* URMA Work Request */
    urma_jfs_wr_t send_wr;
    urma_rw_wr_t rdma_wr;
    urma_sge_t sge[2];    /* Command + Data SGEs */
    uint32_t num_sge;

    /* Memory Buffers */
    void *cmd_buffer;
    void *data_buffer;
    uint32_t data_len;
    urma_target_seg_t *data_tseg;
    void *addr;

    /* Completion Callback */
    spdk_nvme_cmd_cb cb_fn;
    void *cb_arg;

    /* Request State */
    bool active;
    bool in_use;
    uint16_t cid;

    /* Completion tracking */
    uint32_t completion_flags;
    struct spdk_nvme_cpl cpl;

    TAILQ_ENTRY(nvme_ub_request) link;
};

/* URMA Poll Group Structure */
struct nvme_ub_poll_group {
    struct spdk_nvme_transport_poll_group group;
    pthread_mutex_t lock;
    TAILQ_HEAD(, nvme_ub_qpair) qpairs;
    TAILQ_HEAD(, nvme_ub_qpair) connecting_qpairs;  /* Qpairs being connected */
    TAILQ_HEAD(, nvme_ub_qpair) active_qpairs;      /* Active qpairs */
    uint32_t num_qpairs;
    struct spdk_sock_group *sock_group;
};

/* NVMe UB transport extensions for spdk_nvme_ctrlr */
struct nvme_ub_ctrlr {
    struct spdk_nvme_ctrlr ctrlr;

    urma_device_t *urma_dev;
    urma_context_t *urma_ctx;
    urma_device_attr_t dev_attr;
    char dev_name[URMA_MAX_DEV_NAME];
    int32_t eid_index;

    /* UB max SGE */
    uint16_t max_sge;

    /* URMA Jetty Completion Queue */
    urma_jfce_t *jfce;
    urma_jfc_t *jfc;
    urma_jetty_t *admin_jetty;

    /* Memory Segments */
    urma_target_seg_t *send_tseg;
    urma_target_seg_t *recv_tseg;
    void *send_buffer;
    void *recv_buffer;
    uint32_t buffer_size;

    /* Token for memory registration */
    urma_token_t token;

    /* Queue Management */
    uint32_t max_io_queues;
    uint32_t current_io_queues;
    TAILQ_HEAD(, nvme_ub_qpair) qpairs;

    /* Connection State */
    bool is_connected;
    bool admin_qpair_ready;

    /* Statistics */
    uint64_t requests_sent;
    uint64_t requests_completed;
    uint64_t send_errors;
    uint64_t recv_errors;
};

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

/* UB qpair completion flags */
#define NVME_UB_SEND_COMPLETED  1u << 0
#define NVME_UB_RECV_COMPLETED  1u << 1

/* Inline helper functions */
static inline struct nvme_ub_ctrlr *
nvme_ub_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
    assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_UB);
    return SPDK_CONTAINEROF(ctrlr, struct nvme_ub_ctrlr, ctrlr);
}

static inline struct nvme_ub_qpair *
nvme_ub_qpair(struct spdk_nvme_qpair *qpair)
{
    assert(qpair->trtype == SPDK_NVME_TRANSPORT_UB);
    return SPDK_CONTAINEROF(qpair, struct nvme_ub_qpair, qpair);
}

static inline struct nvme_ub_poll_group *
nvme_ub_poll_group(struct spdk_nvme_transport_poll_group *group)
{
    return SPDK_CONTAINEROF(group, struct nvme_ub_poll_group, group);
}

static inline void *
nvme_ub_qpair_send_buffer(struct nvme_ub_qpair *uqpair)
{
    return uqpair->qid == 0 ? uqpair->uctrlr->send_buffer : uqpair->cmd_buffer;
}

static inline uint64_t
nvme_ub_qpair_send_buffer_size(struct nvme_ub_qpair *uqpair)
{
    return uqpair->qid == 0 ? uqpair->uctrlr->buffer_size : uqpair->cmd_buffer_size;
}

static inline urma_target_seg_t *
nvme_ub_qpair_send_tseg(struct nvme_ub_qpair *uqpair)
{
    return uqpair->qid == 0 ? uqpair->uctrlr->send_tseg : uqpair->cmd_tseg;
}

static inline void *
nvme_ub_qpair_recv_buffer(struct nvme_ub_qpair *uqpair)
{
    return uqpair->qid == 0 ? uqpair->uctrlr->recv_buffer : uqpair->resp_buffer;
}

static inline urma_target_seg_t *
nvme_ub_qpair_recv_tseg(struct nvme_ub_qpair *uqpair)
{
    return uqpair->qid == 0 ? uqpair->uctrlr->recv_tseg : uqpair->resp_tseg;
}

static inline void *
nvme_ub_qpair_payload_buffer(struct nvme_ub_qpair *uqpair, uint16_t cid)
{
    return (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) + uqpair->payload_buffer_offset +
           (uint64_t)cid * MAX_IO_SIZE;
}


static inline struct nvme_ub_request *
nvme_ub_req_get(struct nvme_ub_qpair *uqpair)
{
    struct nvme_ub_request *ub_req;

    ub_req = TAILQ_FIRST(&uqpair->free_reqs);
    if (spdk_likely(ub_req)) {
        TAILQ_REMOVE(&uqpair->free_reqs, ub_req, link);
    }
    return ub_req;
}

static inline void
nvme_ub_req_put(struct nvme_ub_qpair *uqpair, struct nvme_ub_request *ub_req)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (uqpair=%p, ub_req=%p, qid=%u)\n",
            __func__, (void*)uqpair, (void*)ub_req, uqpair->qid);
    ub_req->completion_flags = 0;
    ub_req->req = NULL;
    TAILQ_INSERT_HEAD(&uqpair->free_reqs, ub_req, link);
}

static inline void
nvme_ub_req_complete(struct nvme_ub_request *ub_req, struct spdk_nvme_cpl *cpl, bool print_on_error)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (ub_req=%p, qid=%u)\n",
            __func__, (void*)ub_req, ub_req->uqpair->qid);
    struct nvme_ub_qpair *uqpair = ub_req->uqpair;
    struct spdk_nvme_qpair *qpair = &uqpair->qpair;
    struct nvme_request *req = ub_req->req;

    if (spdk_unlikely(print_on_error && spdk_nvme_cpl_is_error(cpl))) {
        spdk_nvme_qpair_print_command(qpair, &req->cmd);
        spdk_nvme_qpair_print_completion(qpair, cpl);
    }

    nvme_ub_remove_req(uqpair, ub_req);
    nvme_ub_req_put(uqpair, ub_req);
    nvme_complete_request(req->cb_fn, req->cb_arg, qpair, req, cpl);
    fprintf(stderr, "DEBUG: [EXIT] %s\n", __func__);
}

static inline void
nvme_ub_add_req(struct nvme_ub_qpair *uqpair, struct nvme_ub_request *ub_req)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (uqpair=%p, ub_req=%p, qid=%u, outstanding=%u)\n",
            __func__, (void*)uqpair, (void*)ub_req, uqpair->qid, uqpair->outstanding_requests);
    TAILQ_INSERT_TAIL(&uqpair->outstanding_reqs, ub_req, link);
    uqpair->outstanding_requests++;
}

static inline void
nvme_ub_remove_req(struct nvme_ub_qpair *uqpair, struct nvme_ub_request *ub_req)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (uqpair=%p, ub_req=%p, qid=%u, outstanding=%u)\n",
            __func__, (void*)uqpair, (void*)ub_req, uqpair->qid, uqpair->outstanding_requests);
    TAILQ_REMOVE(&uqpair->outstanding_reqs, ub_req, link);
    assert(uqpair->outstanding_requests > 0);
    uqpair->outstanding_requests--;
}

/* Helper function to register buffer segments */
static int
nvme_ub_register_segs(urma_context_t *urma_ctx, urma_token_t *token, uint32_t buffer_size,
                      void **buf1, void **buf2, urma_target_seg_t **tseg1, urma_target_seg_t **tseg2)
{
    urma_reg_seg_flag_t seg_flag = {
        .bs.token_policy = URMA_TOKEN_NONE,
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
        .bs.token_id_valid = 0,
        .bs.reserved = 0
    };
    urma_seg_cfg_t seg_cfg = {
        .len = buffer_size,
        .token_value = *token,
        .flag = seg_flag,
        .token_id = NULL,
        .user_ctx = 0,
        .iova = 0
    };
    int rc;

    /* Allocate first buffer - must be page aligned for UB transport */
    rc = posix_memalign(buf1, PAGE_SIZE, PAGE_SIZE * 512);
    if (rc != 0) {
        return rc;
    }
    memset(*buf1, 0, buffer_size);

    /* Register first segment */
    seg_cfg.va = (uint64_t)*buf1;
    *tseg1 = urma_register_seg(urma_ctx, &seg_cfg);
    if (*tseg1 == NULL) {
        free(*buf1);
        *buf1 = NULL;
        return -EFAULT;
    }

    /* Allocate second buffer */
    rc = posix_memalign(buf2, PAGE_SIZE, PAGE_SIZE * 512);
    if (rc != 0) {
        urma_unregister_seg(*tseg1);
        free(*buf1);
        *tseg1 = NULL;
        *buf1 = NULL;
        return rc;
    }
    memset(*buf2, 0, buffer_size);

    /* Register second segment */
    seg_cfg.va = (uint64_t)*buf2;
    *tseg2 = urma_register_seg(urma_ctx, &seg_cfg);
    if (*tseg2 == NULL) {
        free(*buf2);
        urma_unregister_seg(*tseg1);
        free(*buf1);
        *tseg1 = NULL;
        *buf1 = NULL;
        *buf2 = NULL;
        return -EFAULT;
    }

    return 0;
}

/* Helper function to unregister and free buffer segments */
static void
nvme_ub_unregister_segs(urma_target_seg_t *tseg1, urma_target_seg_t *tseg2,
                        void *buf1, void *buf2)
{
    if (tseg1) {
        urma_unregister_seg(tseg1);
    }
    if (tseg2) {
        urma_unregister_seg(tseg2);
    }
    if (buf1) {
        free(buf1);
    }
    if (buf2) {
        free(buf2);
    }
}

/* Helper function to allocate requests for a qpair */
static int
nvme_ub_alloc_qpair_reqs(struct nvme_ub_qpair *uqpair, uint32_t queue_size)
{
    uint32_t i;

    uqpair->reqs = spdk_zmalloc(queue_size * sizeof(struct nvme_ub_request *),
                    0, NULL, SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
    if (!uqpair->reqs) {
        return -ENOMEM;
    }

    for (i = 0; i < queue_size; i++) {
        uqpair->reqs[i] = spdk_zmalloc(sizeof(struct nvme_ub_request), 0, NULL,
                        SPDK_ENV_NUMA_ID_ANY, SPDK_MALLOC_DMA);
        if (!uqpair->reqs[i]) {
            for (uint32_t j = 0; j < i; j++) {
                spdk_free(uqpair->reqs[j]);
            }
            spdk_free(uqpair->reqs);
            uqpair->reqs = NULL;
            return -ENOMEM;
        }
        uqpair->reqs[i]->id = i;
        uqpair->reqs[i]->uqpair = uqpair;
        TAILQ_INSERT_TAIL(&uqpair->free_reqs, uqpair->reqs[i], link);
    }
    uqpair->num_requests = queue_size;

    return 0;
}

/* Helper function to free requests for a qpair */
static void
nvme_ub_free_qpair_reqs(struct nvme_ub_qpair *uqpair)
{
    uint32_t i;

    if (uqpair->reqs) {
        for (i = 0; i < uqpair->num_requests; i++) {
            if (uqpair->reqs[i]) {
                spdk_free(uqpair->reqs[i]);
            }
        }
        spdk_free(uqpair->reqs);
        uqpair->reqs = NULL;
        uqpair->num_requests = 0;
    }
}

static int
nvme_ub_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
    struct nvme_ub_ctrlr *uctrlr = nvme_ub_ctrlr(ctrlr);
    struct nvme_ub_qpair *admin_uqpair = ctrlr->adminq ? nvme_ub_qpair(ctrlr->adminq) : NULL;
    struct nvme_ub_qpair *uqpair, *tmp;

    TAILQ_FOREACH_SAFE(uqpair, &uctrlr->qpairs, ctrlr_link, tmp) {
        TAILQ_REMOVE(&uctrlr->qpairs, uqpair, ctrlr_link);
        if (uqpair == admin_uqpair) {
            continue;
        }

        if (uqpair->sock) {
            spdk_sock_close(&uqpair->sock);
        }
        if (uqpair->tjetty) {
            urma_unimport_jetty(uqpair->tjetty);
        }
        if (uqpair->remote_tseg) {
            urma_unimport_seg(uqpair->remote_tseg);
        }
        if (uqpair->jetty) {
            urma_delete_jetty(uqpair->jetty);
        }
        if (uqpair->jfr) {
            urma_delete_jfr(uqpair->jfr);
        }
        if (uqpair->cmd_tseg) {
            urma_unregister_seg(uqpair->cmd_tseg);
        }
        if (uqpair->resp_tseg) {
            urma_unregister_seg(uqpair->resp_tseg);
        }
        free(uqpair->cmd_buffer);
        free(uqpair->resp_buffer);
        if (uqpair->jfc) {
            urma_delete_jfc(uqpair->jfc);
        }
        if (uqpair->jfce) {
            urma_delete_jfce(uqpair->jfce);
        }
        free(uqpair->recv_ctxs);
        nvme_ub_free_qpair_reqs(uqpair);
        nvme_qpair_deinit(&uqpair->qpair);
        spdk_free(uqpair);
    }

    if (admin_uqpair) {
        if (admin_uqpair->sock) {
            spdk_sock_close(&admin_uqpair->sock);
        }
        if (admin_uqpair->tjetty) {
            urma_unimport_jetty(admin_uqpair->tjetty);
        }
        if (admin_uqpair->remote_tseg) {
            urma_unimport_seg(admin_uqpair->remote_tseg);
        }
        if (admin_uqpair->jetty) {
            urma_delete_jetty(admin_uqpair->jetty);
            uctrlr->admin_jetty = NULL;
        }
        if (admin_uqpair->jfr) {
            urma_delete_jfr(admin_uqpair->jfr);
        }
        free(admin_uqpair->recv_ctxs);
        nvme_ub_free_qpair_reqs(admin_uqpair);
        nvme_qpair_deinit(&admin_uqpair->qpair);
        spdk_free(admin_uqpair);
        ctrlr->adminq = NULL;
    }

    if (uctrlr->admin_jetty) {
        urma_delete_jetty(uctrlr->admin_jetty);
    }
    if (uctrlr->send_tseg) {
        urma_unregister_seg(uctrlr->send_tseg);
    }
    if (uctrlr->recv_tseg) {
        urma_unregister_seg(uctrlr->recv_tseg);
    }
    free(uctrlr->send_buffer);
    free(uctrlr->recv_buffer);
    if (uctrlr->jfc) {
        urma_delete_jfc(uctrlr->jfc);
    }
    if (uctrlr->jfce) {
        urma_delete_jfce(uctrlr->jfce);
    }
    if (uctrlr->urma_ctx) {
        urma_delete_context(uctrlr->urma_ctx);
    }
    urma_uninit();

    nvme_ctrlr_destruct_finish(ctrlr);
    spdk_free(uctrlr);
    return 0;
}

static int
nvme_ub_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (ctrlr=%p)\n", __func__, (void*)ctrlr);
    /* No special enable needed for UB transport */
    return 0;
}

static uint32_t
nvme_ub_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
    struct nvme_ub_ctrlr *uctrlr = nvme_ub_ctrlr(ctrlr);
    uint64_t max_xfer_size;

    max_xfer_size = spdk_min(uctrlr->dev_attr.dev_cap.max_msg_size, (uint64_t)MAX_IO_SIZE);
    return (uint32_t)max_xfer_size;
}

static uint16_t
nvme_ub_ctrlr_get_max_sges(struct spdk_nvme_ctrlr *ctrlr)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (ctrlr=%p)\n", __func__, (void*)ctrlr);
    struct nvme_ub_ctrlr *uctrlr = nvme_ub_ctrlr(ctrlr);

    return uctrlr->max_sge;
}

static struct spdk_nvme_qpair *
nvme_ub_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
                 const struct spdk_nvme_io_qpair_opts *opts)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (ctrlr=%p, qid=%u, queue_size=%u)\n",
            __func__, (void*)ctrlr, qid, opts->io_queue_size);
    struct nvme_ub_ctrlr *uctrlr = nvme_ub_ctrlr(ctrlr);
    struct nvme_ub_qpair *uqpair;
    struct spdk_nvme_qpair *qpair;
    urma_jfc_cfg_t jfc_cfg;
    urma_jfs_cfg_t jfs_cfg;
    urma_jfr_cfg_t jfr_cfg;
    urma_jetty_cfg_t jetty_cfg;
    urma_seg_cfg_t seg_cfg;
    urma_reg_seg_flag_t seg_flag;
    uint32_t jfc_depth;
    int rc;

    if (qid == 0) {
        NVME_CTRLR_ERRLOG(ctrlr, "IO qpair qid cannot be 0, use admin qpair\n");
        return NULL;
    }

    if (opts->io_queue_size < SPDK_NVME_QUEUE_MIN_ENTRIES) {
        NVME_CTRLR_ERRLOG(ctrlr, "Failed to create qpair with size %u. Minimum queue size is %d.\n",
                  opts->io_queue_size, SPDK_NVME_QUEUE_MIN_ENTRIES);
        return NULL;
    }

    uqpair = spdk_zmalloc(sizeof(struct nvme_ub_qpair), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
                  SPDK_MALLOC_DMA);
    if (!uqpair) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate uqpair\n");
        return NULL;
    }

    uqpair->sock = NULL;
    uqpair->uctrlr = uctrlr;
    uqpair->num_entries = opts->io_queue_size - 1;
    uqpair->delay_cmd_submit = opts->delay_cmd_submit;
    uqpair->qid = qid;
    uqpair->sq_depth = opts->io_queue_size;
    uqpair->cq_depth = opts->io_queue_size;
    /* A large application I/O can be split into multiple NVMe commands.  Size
     * the response receive queue for the full SQ, not the application's queue
     * depth, so every command that can be outstanding has a response credit. */
    uqpair->recv_depth = uqpair->num_entries;
    uqpair->state = NVME_UB_JETTY_STATE_RESET;
    uqpair->qpair_state = NVME_UB_QPAIR_STATE_INVALID;

    qpair = &uqpair->qpair;
    rc = nvme_qpair_init(qpair, qid, ctrlr, opts->qprio, opts->io_queue_requests, opts->async_mode);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(ctrlr, "nvme_qpair_init failed\n");
        spdk_free(uqpair);
        return NULL;
    }

    /* Initialize request tracking */
    TAILQ_INIT(&uqpair->free_reqs);
    TAILQ_INIT(&uqpair->outstanding_reqs);

    /* Allocate requests using helper function */
    /* Transport request contexts are SQ credits.  io_queue_requests is the
     * size of SPDK's software request pool and can be much larger when an I/O
     * is split into child requests.  Limiting the UB pool to num_entries makes
     * nvme_ub_req_get() return -EAGAIN at the negotiated SQ depth so excess
     * children remain on qpair->queued_req. */
    rc = nvme_ub_alloc_qpair_reqs(uqpair, uqpair->num_entries);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate requests\n");
        nvme_qpair_deinit(qpair);
        spdk_free(uqpair);
        return NULL;
    }

    uqpair->recv_ctxs = calloc(uqpair->recv_depth, sizeof(*uqpair->recv_ctxs));
    if (uqpair->recv_ctxs == NULL) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate receive contexts\n");
        goto fail;
    }

    /* A completion queue must not be shared across application poll groups.  Keep the
     * admin queue and every I/O qpair on independent JFCs so a poller cannot consume
     * a completion owned by another qpair/thread. */
    uqpair->jfce = urma_create_jfce(uctrlr->urma_ctx);
    if (uqpair->jfce == NULL) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to create IO JFCE, errno=%d\n", errno);
        goto fail;
    }

    jfc_depth = opts->io_queue_size <= UINT32_MAX / 2 ? opts->io_queue_size * 2 : UINT32_MAX;
    jfc_depth = spdk_min(jfc_depth, uctrlr->dev_attr.dev_cap.max_jfc_depth);
    if (jfc_depth == 0) {
        NVME_CTRLR_ERRLOG(ctrlr, "invalid IO JFC depth\n");
        goto fail;
    }

    memset(&jfc_cfg, 0, sizeof(jfc_cfg));
    jfc_cfg.depth = jfc_depth;
    jfc_cfg.jfce = uqpair->jfce;
    jfc_cfg.user_ctx = (uint64_t)(uintptr_t)uqpair;
    uqpair->jfc = urma_create_jfc(uctrlr->urma_ctx, &jfc_cfg);
    if (uqpair->jfc == NULL) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to create IO JFC, errno=%d\n", errno);
        goto fail;
    }

    /* Create Jetty (combining JFS and JFR) - using shared JFR approach, URMA_TM_RM mode */
    memset(&jfs_cfg, 0, sizeof(jfs_cfg));
    jfs_cfg.depth = opts->io_queue_size;
    jfs_cfg.flag.bs.order_type = 0;
    jfs_cfg.flag.bs.multi_path = 0;
    jfs_cfg.trans_mode = URMA_TM_RM;
    jfs_cfg.priority = URMA_MAX_PRIORITY;
    jfs_cfg.max_sge = uctrlr->max_sge;
    jfs_cfg.max_inline_data = 0;
    jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
    jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
    jfs_cfg.jfc = uqpair->jfc;
    jfs_cfg.user_ctx = (uint64_t)(uintptr_t)uqpair;

    /* Create JFR for IO qpair - 使用 URMA_TM_RM 模式 */
    memset(&jfr_cfg, 0, sizeof(jfr_cfg));
    jfr_cfg.depth = opts->io_queue_size;
    jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
    jfr_cfg.flag.bs.order_type = 0;
    jfr_cfg.trans_mode = URMA_TM_RM;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
    jfr_cfg.jfc = uqpair->jfc;
    jfr_cfg.token_value = uctrlr->token;
    jfr_cfg.id = 0;
    jfr_cfg.max_sge = 1;

    urma_jfr_t *io_jfr = urma_create_jfr(uctrlr->urma_ctx, &jfr_cfg);
    if (io_jfr == NULL) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to create IO JFR, errno=%d\n", errno);
        goto fail;
    }
    uqpair->jfr = io_jfr;

    memset(&jetty_cfg, 0, sizeof(jetty_cfg));
    jetty_cfg.flag.bs.share_jfr = 1; /* UB dev must use shared JFR */
    jetty_cfg.jfs_cfg = jfs_cfg;
    jetty_cfg.shared.jfr = io_jfr;
    jetty_cfg.shared.jfc = uqpair->jfc;

    uqpair->jetty = urma_create_jetty(uctrlr->urma_ctx, &jetty_cfg);
    if (uqpair->jetty == NULL) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to create jetty, errno=%d\n", errno);
        urma_delete_jfr(io_jfr);
        uqpair->jfr = NULL;
        goto fail;
    }

    uqpair->jetty_id = uqpair->jetty->jetty_id;

    /* Keep command/payload staging and response capsules private to the qpair. */
    uqpair->payload_buffer_offset = SPDK_ALIGN_CEIL(
            (uint64_t)uqpair->num_entries * sizeof(struct spdk_nvme_cmd), PAGE_SIZE);
    uqpair->cmd_buffer_size = uqpair->payload_buffer_offset +
                              (uint64_t)uqpair->num_entries * MAX_IO_SIZE;
    uqpair->resp_buffer_size = (uint64_t)uqpair->recv_depth * MSG_SIZE;

    rc = posix_memalign(&uqpair->cmd_buffer, PAGE_SIZE, uqpair->cmd_buffer_size);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate IO qpair cmd buffer, rc=%d\n", rc);
        goto fail;
    }
    memset(uqpair->cmd_buffer, 0, uqpair->cmd_buffer_size);

    rc = posix_memalign(&uqpair->resp_buffer, PAGE_SIZE, uqpair->resp_buffer_size);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to allocate IO qpair resp buffer, rc=%d\n", rc);
        goto fail;
    }
    memset(uqpair->resp_buffer, 0, uqpair->resp_buffer_size);

    /* Register IO qpair sq (command) buffer segment */
    seg_flag = (urma_reg_seg_flag_t) {
        .bs.token_policy = URMA_TOKEN_NONE,
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
        .bs.token_id_valid = 0,
        .bs.reserved = 0
    };
    seg_cfg = (urma_seg_cfg_t) {
        .va = (uint64_t)uqpair->cmd_buffer,
        .len = uqpair->cmd_buffer_size,
        .token_id = NULL,
        .token_value = uctrlr->token,
        .flag = seg_flag,
        .user_ctx = 0,
        .iova = 0
    };
    uqpair->cmd_tseg = urma_register_seg(uctrlr->urma_ctx, &seg_cfg);
    if (uqpair->cmd_tseg == NULL) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to register sq seg\n");
        goto fail;
    }

    /* Register IO qpair cq (response) buffer segment */
    seg_cfg.va = (uint64_t)uqpair->resp_buffer;
    seg_cfg.len = uqpair->resp_buffer_size;
    uqpair->resp_tseg = urma_register_seg(uctrlr->urma_ctx, &seg_cfg);
    if (uqpair->resp_tseg == NULL) {
        NVME_CTRLR_ERRLOG(ctrlr, "failed to register cq seg\n");
        goto fail;
    }
    fprintf(stderr, "DEBUG: IO qpair %u segments registered: sq_tseg=%p, cq_tseg=%p\n",
            qid, (void*)uqpair->cmd_tseg, (void*)uqpair->resp_tseg);

    /* Add to controller's qpair list */
    TAILQ_INSERT_TAIL(&uctrlr->qpairs, uqpair, ctrlr_link);
    uctrlr->current_io_queues++;

    NVME_CTRLR_DEBUGLOG(ctrlr, "created IO qpair %u\n", qid);
    return qpair;

fail:
    if (uqpair->jetty) {
        urma_delete_jetty(uqpair->jetty);
    }
    if (uqpair->jfr) {
        urma_delete_jfr(uqpair->jfr);
    }
    if (uqpair->resp_tseg) {
        urma_unregister_seg(uqpair->resp_tseg);
    }
    if (uqpair->cmd_tseg) {
        urma_unregister_seg(uqpair->cmd_tseg);
    }
    if (uqpair->cmd_buffer) {
        free(uqpair->cmd_buffer);
    }
    if (uqpair->resp_buffer) {
        free(uqpair->resp_buffer);
    }
    if (uqpair->jfc) {
        urma_delete_jfc(uqpair->jfc);
    }
    if (uqpair->jfce) {
        urma_delete_jfce(uqpair->jfce);
    }
    free(uqpair->recv_ctxs);
    nvme_ub_free_qpair_reqs(uqpair);
    nvme_qpair_deinit(qpair);
    spdk_free(uqpair);
    return NULL;
}

static int
nvme_ub_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (ctrlr=%p, qpair=%p, qid=%u)\n",
            __func__, (void*)ctrlr, (void*)qpair, qpair->id);
    struct nvme_ub_ctrlr *uctrlr = nvme_ub_ctrlr(ctrlr);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);

    assert(qpair->id != 0); /* Use admin qpair deletion for qid 0 */

    /* Abort all outstanding requests */
    nvme_ub_qpair_abort_reqs(qpair, qpair->abort_dnr);

    /* Remove from controller's qpair list */
    TAILQ_REMOVE(&uctrlr->qpairs, uqpair, ctrlr_link);
    uctrlr->current_io_queues--;

    /* Close socket if still open */
    if (uqpair->sock) {
        spdk_sock_close(&uqpair->sock);
    }

    /* Free URMA resources */
    if (uqpair->tjetty) {
        urma_unimport_jetty(uqpair->tjetty);
    }
    if (uqpair->remote_tseg) {
        urma_unimport_seg(uqpair->remote_tseg);
    }
    if (uqpair->jetty) {
        urma_delete_jetty(uqpair->jetty);
    }
    if (uqpair->jfr) {
        urma_delete_jfr(uqpair->jfr);
    }
    if (uqpair->cmd_tseg) {
        urma_unregister_seg(uqpair->cmd_tseg);
    }
    if (uqpair->resp_tseg) {
        urma_unregister_seg(uqpair->resp_tseg);
    }
    if (uqpair->cmd_buffer) {
        free(uqpair->cmd_buffer);
    }
    if (uqpair->resp_buffer) {
        free(uqpair->resp_buffer);
    }
    if (uqpair->jfc) {
        urma_delete_jfc(uqpair->jfc);
    }
    if (uqpair->jfce) {
        urma_delete_jfce(uqpair->jfce);
    }
    free(uqpair->recv_ctxs);

    /* Free request structures */
    nvme_ub_free_qpair_reqs(uqpair);

    nvme_qpair_deinit(qpair);
    spdk_free(uqpair);

    return 0;
}

#define MAX_POLL_JFC_CNT 10
#define SLEEP_TIME (100 * 1000)

/* UB transport max keyed SGL length */
#define NVME_UB_MAX_KEYED_SGL_LENGTH 0xFFFFFF

/* Helper function to configure dptr for null request (payload_size == 0)
 * 参考 nvme_rdma_build_null_request
 */
static inline void
nvme_ub_configure_null_request(struct nvme_ub_request *ub_req, struct nvme_request *req)
{
    req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;

    /* Configure SGL descriptor with zero length */
    req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
    req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
    req->cmd.dptr.sgl1.keyed.length = 0;
    req->cmd.dptr.sgl1.keyed.key = 0;
    req->cmd.dptr.sgl1.address = NULL;

    /* Record that we have a null request (no data payload) */
    ub_req->num_sge = 0;
}

/* Helper function to configure dptr for contiguous payload
 * 参考 nvme_rdma_configure_contig_request
 */
static inline int
nvme_ub_configure_contig_request(struct nvme_ub_qpair *uqpair,
                                  struct nvme_ub_request *ub_req,
                                  struct nvme_request *req)
{
    urma_target_seg_t *send_tseg = nvme_ub_qpair_send_tseg(uqpair);
    void *payload_buffer;
    uint32_t rkey;

    assert(req->payload_size != 0);
    assert(req->payload_size <= NVME_UB_MAX_KEYED_SGL_LENGTH);

    if (spdk_unlikely(req->payload_size > MAX_IO_SIZE || send_tseg == NULL)) {
        NVME_UQPAIR_ERRLOG(uqpair, "invalid payload size %u or send segment\n", req->payload_size);
        return -EINVAL;
    }

    payload_buffer = nvme_ub_qpair_payload_buffer(uqpair, req->cmd.cid);
    if (spdk_unlikely((uint8_t *)payload_buffer + req->payload_size >
                      (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
                      nvme_ub_qpair_send_buffer_size(uqpair))) {
        NVME_UQPAIR_ERRLOG(uqpair, "payload buffer exceeds registered segment\n");
        return -EINVAL;
    }

    rkey = send_tseg->seg.token_id;

    /* Configure SGL descriptor in the NVMe command dptr */
    req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
    req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
    req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
    req->cmd.dptr.sgl1.keyed.length = (uint32_t)req->payload_size;
    req->cmd.dptr.sgl1.keyed.key = rkey;

    memcpy(payload_buffer, (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,
           req->payload_size);
    req->cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)payload_buffer;

    fprintf(stderr, "DEBUG: %s contig: addr=0x%lx, length=%u, key=0x%x\n",
            __func__, req->cmd.dptr.sgl1.address, req->cmd.dptr.sgl1.keyed.length, rkey);

    /* Record num_sge for this request */
    ub_req->num_sge = 1;

    return 0;
}

/* Helper function to configure dptr for SGL payload
 * 参考 nvme_rdma_build_sgl_request
 */
static inline int
nvme_ub_configure_sgl_request(struct nvme_ub_qpair *uqpair,
                               struct nvme_ub_request *ub_req,
                               struct nvme_request *req)
{
    urma_target_seg_t *send_tseg = nvme_ub_qpair_send_tseg(uqpair);
    uint8_t *payload_buffer;
    uint32_t remaining_size;
    uint32_t sge_length;
    uint32_t copied = 0;
    uint32_t rkey;
    int num_sge = 0;
    int rc;

    assert(req->payload_size != 0);
    assert(req->payload.reset_sgl_fn != NULL);
    assert(req->payload.next_sge_fn != NULL);

    if (spdk_unlikely(req->payload_size > MAX_IO_SIZE || send_tseg == NULL)) {
        NVME_UQPAIR_ERRLOG(uqpair, "invalid payload size %u or send segment\n", req->payload_size);
        return -EINVAL;
    }

    payload_buffer = nvme_ub_qpair_payload_buffer(uqpair, req->cmd.cid);
    if (spdk_unlikely(payload_buffer + req->payload_size >
                      (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
                      nvme_ub_qpair_send_buffer_size(uqpair))) {
        NVME_UQPAIR_ERRLOG(uqpair, "payload buffer exceeds registered segment\n");
        return -EINVAL;
    }

    rkey = send_tseg->seg.token_id;

    req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

    remaining_size = req->payload_size;

    /* UB exposes one contiguous registered staging area to the target.  Gather
     * the application's SGL into it and describe the whole payload with one
     * keyed SGL descriptor. */
    do {
        void *addr;

        rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &addr, &sge_length);
        if (spdk_unlikely(rc)) {
            NVME_UQPAIR_ERRLOG(uqpair, "next_sge_fn failed\n");
            return -1;
        }

        if (spdk_unlikely(sge_length == 0)) {
            NVME_UQPAIR_ERRLOG(uqpair, "next_sge_fn returned an empty SGE\n");
            return -EINVAL;
        }

        sge_length = spdk_min(remaining_size, sge_length);

        if (num_sge == 0) {
            ub_req->addr = addr;
        }

        memcpy(payload_buffer + copied, addr, sge_length);
        copied += sge_length;
        remaining_size -= sge_length;
        num_sge++;
    } while (remaining_size > 0);

    req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
    req->cmd.dptr.sgl1.keyed.key = rkey;
    req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
    req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
    req->cmd.dptr.sgl1.keyed.length = req->payload_size;
    req->cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)payload_buffer;

    ub_req->num_sge = num_sge;

    fprintf(stderr, "DEBUG: %s SGL: num_sge=%d, addr=0x%lx, length=%u\n",
            __func__, num_sge, req->cmd.dptr.sgl1.address, req->cmd.dptr.sgl1.keyed.length);

    return 0;
}

/* Initialize URMA request based on payload type and size
 * 参考 nvme_rdma_req_init
 */
static inline int
nvme_ub_req_init(struct nvme_ub_qpair *uqpair, struct nvme_ub_request *ub_req)
{
    struct nvme_request *req = ub_req->req;
    enum nvme_payload_type payload_type;
    int rc = -1;

    payload_type = nvme_payload_type(&req->payload);

    if (spdk_unlikely(req->payload_size == 0)) {
        /* Null request - no data transfer */
        nvme_ub_configure_null_request(ub_req, req);
        rc = 0;
    } else if (payload_type == NVME_PAYLOAD_TYPE_CONTIG) {
        /* Contiguous payload */
        rc = nvme_ub_configure_contig_request(uqpair, ub_req, req);
    } else if (payload_type == NVME_PAYLOAD_TYPE_SGL) {
        /* SGL payload */
        rc = nvme_ub_configure_sgl_request(uqpair, ub_req, req);
    }

    return rc;
}

/* Post a recv WR to prepare for receiving response - 参考 urma_sample.c client_send */
static inline int
nvme_ub_post_recv_wr(struct nvme_ub_qpair *uqpair, int i)
{
    struct cq_ctx *ctx;
    urma_sge_t src_sge;
    urma_jfr_wr_t wr;
    urma_jfr_wr_t *bad_wr = NULL;
    int rc;

    if (spdk_unlikely(i < 0 || (uint32_t)i >= uqpair->recv_depth ||
                      uqpair->recv_ctxs == NULL)) {
        return -EINVAL;
    }

    ctx = &uqpair->recv_ctxs[i];
    ctx->uqpair = uqpair;
    ctx->id = i;

    src_sge = (urma_sge_t) {
        .addr = (uint64_t)nvme_ub_qpair_recv_buffer(uqpair) + (uint64_t)i * MSG_SIZE,
        .len = MSG_SIZE,
        .tseg = nvme_ub_qpair_recv_tseg(uqpair),
    };
    wr = (urma_jfr_wr_t) {
        .src = {
            .sge = &src_sge,
            .num_sge = 1,
        },
        .user_ctx = (uint64_t)(uintptr_t)ctx,
        .next = NULL,
    };

    rc = urma_post_jetty_recv_wr(uqpair->jetty, &wr, &bad_wr);
    if (spdk_unlikely(rc != URMA_SUCCESS)) {
        NVME_UQPAIR_ERRLOG(uqpair, "urma_post_jetty_recv_wr failed, rc=%d\n", rc);
        return -1;
    }
    return 0;
}

static int
nvme_ub_copy_payload_from_staging(struct nvme_ub_request *ub_req)
{
    struct nvme_request *req = ub_req->req;
    uint8_t *src;
    uint32_t remaining, copied = 0;
    enum nvme_payload_type payload_type;

    if (req->payload_size == 0) {
        return 0;
    }

    src = nvme_ub_qpair_payload_buffer(ub_req->uqpair, ub_req->id);
    payload_type = nvme_payload_type(&req->payload);
    if (payload_type == NVME_PAYLOAD_TYPE_CONTIG) {
        memcpy((uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,
               src, req->payload_size);
        return 0;
    }

    if (payload_type != NVME_PAYLOAD_TYPE_SGL || req->payload.reset_sgl_fn == NULL ||
        req->payload.next_sge_fn == NULL) {
        return -EINVAL;
    }

    req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);
    remaining = req->payload_size;
    while (remaining > 0) {
        void *addr;
        uint32_t length;
        int rc;

        rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &addr, &length);
        if (spdk_unlikely(rc != 0 || length == 0)) {
            return -EINVAL;
        }

        length = spdk_min(length, remaining);
        memcpy(addr, src + copied, length);
        copied += length;
        remaining -= length;
    }

    return 0;
}

/* Process one completion from a qpair-owned JFC.  SEND completions only release
 * transport credits; an NVMe request completes exclusively when its response
 * capsule is received. */
static int
nvme_ub_process_cr(struct nvme_ub_qpair *poll_uqpair, const urma_cr_t *cr)
{
    struct nvme_ub_qpair *uqpair;
    struct nvme_ub_request *ub_req;
    int rc;

    if (spdk_unlikely(cr->status != URMA_CR_SUCCESS)) {
        NVME_UQPAIR_ERRLOG(poll_uqpair, "CR error, status=%d, opcode=%d, s_r=%d\n",
                   cr->status, cr->opcode, cr->flag.bs.s_r);
        return -EIO;
    }

    if (cr->flag.bs.s_r == 1) {
        struct cq_ctx *ctx = (struct cq_ctx *)(uintptr_t)cr->user_ctx;
        struct spdk_nvme_cpl cpl;

        if (spdk_unlikely(ctx == NULL || ctx->uqpair == NULL)) {
            return -EPROTO;
        }

        uqpair = ctx->uqpair;
        if (spdk_unlikely(uqpair != poll_uqpair || ctx->id < 0 ||
                          (uint32_t)ctx->id >= uqpair->recv_depth ||
                          cr->completion_len < sizeof(cpl))) {
            NVME_UQPAIR_ERRLOG(poll_uqpair,
                       "invalid RECV completion owner=%p slot=%d len=%u\n",
                       (void *)uqpair, ctx->id, cr->completion_len);
            return -EPROTO;
        }

        memcpy(&cpl, (uint8_t *)nvme_ub_qpair_recv_buffer(uqpair) +
               (uint64_t)ctx->id * MSG_SIZE, sizeof(cpl));
        if (spdk_unlikely(cpl.cid >= uqpair->num_requests)) {
            NVME_UQPAIR_ERRLOG(uqpair, "invalid completion CID %u\n", (unsigned)cpl.cid);
            return -EPROTO;
        }

        ub_req = uqpair->reqs[cpl.cid];
        if (spdk_unlikely(ub_req == NULL || ub_req->req == NULL)) {
            NVME_UQPAIR_ERRLOG(uqpair, "no outstanding request for CID %u\n",
                       (unsigned)cpl.cid);
            return -EPROTO;
        }

        rc = nvme_ub_copy_payload_from_staging(ub_req);
        if (spdk_unlikely(rc != 0)) {
            NVME_UQPAIR_ERRLOG(uqpair, "failed to copy payload for CID %u\n",
                       (unsigned)cpl.cid);
            return rc;
        }

        rc = nvme_ub_post_recv_wr(uqpair, ctx->id);
        if (spdk_unlikely(rc != 0)) {
            return rc;
        }

        nvme_ub_req_complete(ub_req, &cpl, true);
        uqpair->uctrlr->requests_completed++;
        return 1;
    }

    ub_req = (struct nvme_ub_request *)(uintptr_t)cr->user_ctx;
    if (spdk_unlikely(ub_req == NULL || ub_req->uqpair == NULL ||
                      ub_req->uqpair != poll_uqpair)) {
        NVME_UQPAIR_ERRLOG(poll_uqpair, "invalid SEND completion context %p\n", (void *)ub_req);
        return -EPROTO;
    }

    uqpair = ub_req->uqpair;
    if (spdk_unlikely(cr->opcode != URMA_CR_OPC_SEND)) {
        NVME_UQPAIR_WARNLOG(uqpair, "unexpected initiator completion opcode %d\n", cr->opcode);
        return 0;
    }

    if (uqpair->current_num_sends > 0) {
        uqpair->current_num_sends--;
    }

    return 0;
}

static int
nvme_ub_qpair_submit_request(struct spdk_nvme_qpair *qpair, volatile struct nvme_request *req)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (qpair=%p, qid=%u, req=%p, payload_size=%u, opc=0x%02x)\n",
            __func__, (void*)qpair, qpair->id, (void*)req, req->payload_size, req->cmd.opc);
    fprintf(stderr, "------------------------\n");
    fprintf(stderr, "the request cmd context is:\n");
    uint64_t *va64 = req;
    for (int i = 0; i<8;i++) {
        fprintf(stderr, "0x%llx\n", va64[i]);
    }
    fprintf(stderr, "------------------------\n");

    if (req->payload.contig_or_cb_arg) {
        fprintf(stderr, "DEBUG: [ENTER] %s payload_address=%llx\n",
            __func__, (void *)(req->payload.contig_or_cb_arg + req->payload_offset));
    }
    if (req->parent) {
        fprintf(stderr, "DEBUG: [ENTER] %s req->parent=%llx\n",
            __func__, req->parent);
    }
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct nvme_ub_ctrlr *uctrlr = uqpair->uctrlr;
    struct nvme_ub_request *ub_req;
    urma_jfs_wr_t *bad_wr = NULL;
    int rc;

    assert(uqpair != NULL);
    assert(req != NULL);

    if (uqpair->state != NVME_UB_JETTY_STATE_READY) {
        fprintf(stderr, "DEBUG: %s qpair not ready, state=%d\n", __func__, uqpair->state);
        return -EAGAIN;
    }

    if (uqpair->tjetty == NULL) {
        NVME_UQPAIR_ERRLOG(uqpair, "tjetty is NULL, not connected\n");
        return -EAGAIN;
    }

    /* Get a free request structure - 参考 nvme_rdma_qpair_submit_request */
    ub_req = nvme_ub_req_get(uqpair);
    if (spdk_unlikely(!ub_req)) {
        fprintf(stderr, "DEBUG: %s no free requests\n", __func__);
        return -EAGAIN;
    }

    // memset(ub_req, 0, sizeof(*ub_req));
    ub_req->uqpair = uqpair;
    ub_req->req = (struct nvme_request *)req;
    req->cmd.cid = ub_req->id;
    fprintf(stderr, "***********DEBUG: %s CID=%d*****************\n", __func__, req->cmd.cid);

    /* Initialize the request - 配置 dptr 和 SGE，参考 nvme_rdma_req_init */
    rc = nvme_ub_req_init(uqpair, ub_req);
    if (spdk_unlikely(rc)) {
        NVME_UQPAIR_ERRLOG(uqpair, "nvme_ub_req_init failed, rc=%d\n", rc);
        nvme_ub_req_put(uqpair, ub_req);
        return -1;
    }

    /* Add to outstanding requests list */
    nvme_ub_add_req(uqpair, ub_req);

    /* Add qpair to active_qpairs list if not already there - 参考 nvme_rdma */
    if (TAILQ_ENTRY_NOT_ENQUEUED(uqpair, link_active) && qpair->poll_group) {
        struct nvme_ub_poll_group *group = nvme_ub_poll_group(qpair->poll_group);
        TAILQ_INSERT_TAIL(&group->active_qpairs, uqpair, link_active);
    }

    /* Determine the URMA opcode based on NVMe command opcode - 参考 urma_sample.c */
    uint8_t nvme_opc = req->cmd.opc;
    urma_opcode_t urma_opc = URMA_OPC_SEND;

    fprintf(stderr, "DEBUG: %s nvme_opc=0x%02x -> urma_opc=%d\n", __func__, nvme_opc, urma_opc);

    /* Build the URMA work request based on opcode - 参考 urma_sample.c */
    ub_req->send_wr.opcode = urma_opc;
    ub_req->send_wr.flag.bs.complete_enable = 1;
    ub_req->send_wr.flag.bs.inline_flag = 0;
    ub_req->send_wr.tjetty = uqpair->tjetty;
    ub_req->send_wr.user_ctx = (uint64_t)(uintptr_t)ub_req;
    ub_req->send_wr.next = NULL;

    /* Each I/O qpair owns its registered command/payload staging buffer. */
    void *send_buf;
    urma_target_seg_t *send_seg;
    send_buf = (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
               sizeof(struct spdk_nvme_cmd) * req->cmd.cid;
    send_seg = nvme_ub_qpair_send_tseg(uqpair);

    if (spdk_unlikely(send_seg == NULL ||
                      (uint8_t *)send_buf + sizeof(struct spdk_nvme_cmd) >
                      (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
                      nvme_ub_qpair_send_buffer_size(uqpair))) {
        NVME_UQPAIR_ERRLOG(uqpair, "command buffer exceeds registered segment\n");
        nvme_ub_remove_req(uqpair, ub_req);
        nvme_ub_req_put(uqpair, ub_req);
        return -EINVAL;
    }

    /* Copy cmd to send_buffer */
    memcpy(send_buf, &req->cmd, sizeof(struct spdk_nvme_cmd));
    /* The payload, when present, is referenced by the keyed SGL in the command. */
    ub_req->sge[0].addr = (uint64_t)send_buf;
    ub_req->sge[0].len = sizeof(struct spdk_nvme_cmd);
    ub_req->sge[0].tseg = send_seg;
    urma_sg_t src_sg;
    src_sg.sge = &ub_req->sge[0];
    src_sg.num_sge = 1;
    urma_send_wr_t send_wr = {
        .src = src_sg,
        .tseg = send_seg
    };
    ub_req->send_wr.send = send_wr;

    fprintf(stderr, "DEBUG: %s SEND op: addr=0x%lx len=%u, userctx=%d\n", __func__, 
            ub_req->sge[0].addr, ub_req->sge[0].len, ub_req->send_wr.user_ctx);

    /* Post the work request - 参考 _nvme_rdma_qpair_submit_request */
    rc = urma_post_jetty_send_wr(uqpair->jetty, &ub_req->send_wr, &bad_wr);
    if (spdk_unlikely(rc != URMA_SUCCESS)) {
        NVME_UQPAIR_ERRLOG(uqpair, "urma_post_jetty_send_wr failed, rc=%d\n", rc);
        nvme_ub_remove_req(uqpair, ub_req);
        nvme_ub_req_put(uqpair, ub_req);
        return -EIO;
    }

    uqpair->current_num_sends++;
    fprintf(stderr, "DEBUG: %s posted successfully, urma_opc=%d, current_num_sends=%u\n",
            __func__, urma_opc, uqpair->current_num_sends);
    uctrlr->requests_sent++;

    return 0;
}

/* UB qpair failure handler - similar to nvme_rdma_fail_qpair */
static void
nvme_ub_fail_qpair(struct spdk_nvme_qpair *qpair, int failure_reason)
{
    if (qpair->transport_failure_reason == SPDK_NVME_QPAIR_FAILURE_NONE) {
        qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
    }
    nvme_ctrlr_disconnect_qpair(qpair);
}

/* UB qpair disconnect - similar to nvme_rdma_ctrlr_disconnect_qpair */
static void
nvme_ub_ctrlr_disconnect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);

    fprintf(stderr, "DEBUG: [ENTER] %s (ctrlr=%p, qpair=%p, qid=%u)\n",
            __func__, (void*)ctrlr, (void*)qpair, qpair->id);

    /* Set disconnecting state */
    uqpair->state = NVME_UB_JETTY_STATE_DISCONNECTING;

    /* Close SPDK socket if still open */
    if (uqpair->sock) {
        spdk_sock_close(&uqpair->sock);
        uqpair->sock = NULL;
    }

    /* Abort all outstanding requests */
    nvme_ub_qpair_abort_reqs(qpair, qpair->abort_dnr);

    /* Clean up URMA resources */
    if (uqpair->tjetty) {
        urma_unimport_jetty(uqpair->tjetty);
        uqpair->tjetty = NULL;
    }

    if (uqpair->remote_tseg) {
        urma_unimport_seg(uqpair->remote_tseg);
        uqpair->remote_tseg = NULL;
    }

    if (uqpair->jetty) {
        urma_delete_jetty(uqpair->jetty);
        uqpair->jetty = NULL;
        if (uqpair->qid == 0) {
            uqpair->uctrlr->admin_jetty = NULL;
        }
    }

    if (uqpair->jfs) {
        urma_delete_jfs(uqpair->jfs);
        uqpair->jfs = NULL;
    }

    if (uqpair->jfr) {
        urma_delete_jfr(uqpair->jfr);
        uqpair->jfr = NULL;
    }

    /* Mark as not connected */
    uqpair->is_connected = false;

    /* Notify upper layer that disconnect is complete */
    nvme_transport_ctrlr_disconnect_qpair_done(qpair);

    fprintf(stderr, "DEBUG: [EXIT] %s\n", __func__);
}

/* UB qpair connection poll - similar to nvme_tcp_ctrlr_connect_qpair_poll */
static int
nvme_ub_ctrlr_connect_qpair_poll(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    int rc;

    /* Prevent recursive calls */
    if (qpair->in_connect_poll) {
        return -EAGAIN;
    }

    qpair->in_connect_poll = true;

    switch (uqpair->qpair_state) {
    case NVME_UB_QPAIR_STATE_INVALID:
        rc = -EAGAIN;
        break;

    case NVME_UB_QPAIR_STATE_INITIALIZING:
        /* Check if URMA transport is ready */
        if (uqpair->state == NVME_UB_JETTY_STATE_READY) {
            /* Transport ready, now send NVMe-oF Fabric CONNECT command */
            uqpair->qpair_state = NVME_UB_QPAIR_STATE_FABRIC_CONNECT_SEND;
            rc = -EAGAIN;
        } else {
            rc = -EAGAIN;
        }
        break;

    case NVME_UB_QPAIR_STATE_FABRIC_CONNECT_SEND:
        rc = nvme_fabric_qpair_connect_async(qpair, uqpair->num_entries + 1);
        if (rc < 0) {
            NVME_UQPAIR_ERRLOG(uqpair, "Failed to send NVMe-oF Fabric CONNECT command\n");
            break;
        }
        uqpair->qpair_state = NVME_UB_QPAIR_STATE_FABRIC_CONNECT_POLL;
        rc = -EAGAIN;
        break;

    case NVME_UB_QPAIR_STATE_FABRIC_CONNECT_POLL:
        rc = nvme_fabric_qpair_connect_poll(qpair);
        if (rc == 0) {
            if (nvme_fabric_qpair_auth_required(qpair)) {
                rc = nvme_fabric_qpair_authenticate_async(qpair);
                if (rc == 0) {
                    uqpair->qpair_state = NVME_UB_QPAIR_STATE_AUTHENTICATING;
                    rc = -EAGAIN;
                }
            } else {
                uqpair->qpair_state = NVME_UB_QPAIR_STATE_RUNNING;
                nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
            }
        } else if (rc != -EAGAIN) {
            NVME_UQPAIR_ERRLOG(uqpair, "Failed to poll NVMe-oF Fabric CONNECT command\n");
        }
        break;

    case NVME_UB_QPAIR_STATE_AUTHENTICATING:
        rc = nvme_fabric_qpair_authenticate_poll(qpair);
        if (rc == 0) {
            uqpair->qpair_state = NVME_UB_QPAIR_STATE_RUNNING;
            nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTED);
        }
        break;

    case NVME_UB_QPAIR_STATE_RUNNING:
        rc = 0;
        break;

    default:
        NVME_UQPAIR_ERRLOG(uqpair, "Unexpected qpair_state: %d\n", uqpair->qpair_state);
        rc = -EINVAL;
        break;
    }

    qpair->in_connect_poll = false;
    return rc;
}

static int
nvme_ub_qpair_reset(struct spdk_nvme_qpair *qpair)
{
    return 0;
}

static void
print_discovery_log(struct spdk_nvmf_discovery_log_page *log_page)
{
	uint64_t numrec;
	char str[512];
	uint32_t i;
	fprintf(stderr, "==============================================================================================================\n");

	fprintf(stderr, "Discovery Log Page\n");
	fprintf(stderr, "==================\n");

	numrec = from_le64(&log_page->numrec);

	fprintf(stderr, "Generation Counter: %" PRIu64 "\n", from_le64(&log_page->genctr));
	fprintf(stderr, "Number of Records:  %" PRIu64 "\n", numrec);
	fprintf(stderr, "Record Format:      %" PRIu16 "\n", from_le16(&log_page->recfmt));
	fprintf(stderr, "\n");

	for (i = 0; i < numrec; i++) {
		struct spdk_nvmf_discovery_log_page_entry *entry = &log_page->entries[i];

		fprintf(stderr, "Discovery Log Entry %u\n", i);
		fprintf(stderr, "----------------------\n");
		fprintf(stderr, "Transport Type:                        %u (%s)\n",
		       entry->trtype, spdk_nvme_transport_id_trtype_str(entry->trtype));
		fprintf(stderr, "Address Family:                        %u (%s)\n",
		       entry->adrfam, spdk_nvme_transport_id_adrfam_str(entry->adrfam));
		fprintf(stderr, "Subsystem Type:                        %u (%s)\n",
		       entry->subtype,
		       entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY ? "Referral to a discovery service" :
		       entry->subtype == SPDK_NVMF_SUBTYPE_NVME ? "NVM Subsystem" :
		       entry->subtype == SPDK_NVMF_SUBTYPE_DISCOVERY_CURRENT ? "Current Discovery Subsystem" :
		       "Unknown");
		fprintf(stderr, "Port ID:                               %" PRIu16 " (0x%04" PRIx16 ")\n",
		       from_le16(&entry->portid), from_le16(&entry->portid));
		fprintf(stderr, "Controller ID:                         %" PRIu16 " (0x%04" PRIx16 ")\n",
		       from_le16(&entry->cntlid), from_le16(&entry->cntlid));
		snprintf(str, sizeof(entry->trsvcid) + 1, "%s", entry->trsvcid);
		fprintf(stderr, "Transport Service Identifier:          %s\n", str);
		snprintf(str, sizeof(entry->subnqn) + 1, "%s", entry->subnqn);
		fprintf(stderr, "NVM Subsystem Qualified Name:          %s\n", str);
		snprintf(str, sizeof(entry->traddr) + 1, "%s", entry->traddr);
		fprintf(stderr, "Transport Address:                     %s\n", str);
	}

    fprintf(stderr, "==============================================================================================================\n");
}

static void
nvme_ub_dummy_disconnected_qpair_cb(struct spdk_nvme_qpair *qpair, void *poll_group_ctx)
{
    (void)qpair;
    (void)poll_group_ctx;
}

static int
nvme_ub_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct spdk_nvme_transport_poll_group *tgroup = qpair->poll_group;
    int32_t total_completions = 0;
    int rc = 0;
    uint32_t i;

    /* If poll_group is set, delegate to poll_group_process_completions.
     * This is the normal path when using poll groups. */
    if (tgroup != NULL) {
        return spdk_nvme_poll_group_process_completions(tgroup->group, max_completions,
                nvme_ub_dummy_disconnected_qpair_cb);
    }

    /* No poll group - direct completion processing (typically during connection phase) */
    if (max_completions == 0) {
        max_completions = spdk_max((uint32_t)uqpair->num_entries, 1u);
    } else {
        max_completions = spdk_min(max_completions,
                                   spdk_max((uint32_t)uqpair->num_entries, 1u));
    }

    switch (nvme_qpair_get_state(qpair)) {
    case NVME_QPAIR_CONNECTING:
        /* Use the UB connection poll state machine to handle fabric connect */
        rc = nvme_ub_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
        if (rc == 0) {
            /* Connection completed successfully */
        } else if (rc != -EAGAIN) {
            NVME_UQPAIR_ERRLOG(uqpair, "Connect poll failed, rc=%d\n", rc);
            goto failed;
        }
        break;

    case NVME_QPAIR_DISCONNECTING:
        return -ENXIO;

    default:
        break;
    }

    /* Poll JFC for completions */
    for (i = 0; i < max_completions; i++) {
        urma_cr_t cr = {0};
        int cnt;

        cnt = urma_poll_jfc(uqpair->jfc, 1, &cr);
        if (cnt <= 0) {
            break;
        }

        rc = nvme_ub_process_cr(uqpair, &cr);
        if (spdk_unlikely(rc < 0)) {
            goto failed;
        }
        total_completions += rc;
    }

    return total_completions;

failed:
    nvme_ub_fail_qpair(qpair, 0);
    return -ENXIO;
}

static int
nvme_ub_connect_established(struct nvme_ub_qpair *uqpair)
{
    struct nvme_ub_ctrlr *uctrlr = uqpair->uctrlr;
    struct spdk_nvme_qpair *qpair = &uqpair->qpair;
    urma_target_seg_t *recv_tseg = nvme_ub_qpair_recv_tseg(uqpair);
    urma_rjetty_t remote_jetty = {};
    nvme_ub_conn_info_t local_info, remote_info;
    struct iovec iov[2];
    int rc;

    if (uqpair->sock == NULL || recv_tseg == NULL || uqpair->jfc == NULL ||
        uqpair->recv_ctxs == NULL || uqpair->recv_depth == 0) {
        NVME_UQPAIR_ERRLOG(uqpair, "Incomplete qpair resources\n");
        return -1;
    }

    fprintf(stderr, "1480\n");
    /* Pack local connection info */
    memset(&local_info, 0, sizeof(local_info));
    local_info.eid = uctrlr->urma_ctx->eid;
    local_info.uasid = uctrlr->urma_ctx->uasid;
    local_info.jetty_id = uqpair->jetty_id;
    local_info.qid = uqpair->qid;
    local_info.msg_type = 1;
    local_info.trans_mode = 2;

    local_info.seg_va = recv_tseg->seg.ubva.va;
    local_info.seg_len = recv_tseg->seg.len;
    local_info.seg_flag = recv_tseg->seg.attr.value;
    local_info.seg_token_id = recv_tseg->seg.token_id;



    /* Exchange connection info with remote using SPDK sock */
    fprintf(stderr, "DEBUG: %s local_info: eid=0x%lx, uasid=0x%x, seg_va=0x%lx, seg_len=%lu, "
            "seg_flag=0x%x, seg_token_id=0x%x, jetty_id=0x%lx, qid=%u, trans_mode=%u, msg_type=%u\n",
            __func__, local_info.eid, local_info.uasid, local_info.seg_va, local_info.seg_len,
            local_info.seg_flag, local_info.seg_token_id, local_info.jetty_id.id, local_info.qid,
            local_info.trans_mode, local_info.msg_type);
    fprintf(stderr, "DEBUG: %s seg_flag=0x%x, seg_token_id=0x%x, jetty_id=0x%lx, qid=%u, trans_mode=%u, msg_type=%u\n",
             __func__, local_info.seg_flag, local_info.seg_token_id, local_info.jetty_id.id, local_info.qid,
            local_info.trans_mode, local_info.msg_type);

    

    iov[0].iov_base = &local_info;
    iov[0].iov_len = sizeof(local_info);
    rc = spdk_sock_writev(uqpair->sock, iov, 1);
    fprintf(stderr, "DEBUG: %s sent local_info, size=%d, rc=%d\n", __func__, (int)sizeof(local_info), rc);
    if (rc != sizeof(local_info)) {
        NVME_UQPAIR_ERRLOG(uqpair, "Failed to send local info\n");
        return -1;
    }

    spdk_delay_us(100000);
    iov[0].iov_base = &remote_info;
    iov[0].iov_len = sizeof(remote_info);
    rc = spdk_sock_readv(uqpair->sock, iov, 1);
    fprintf(stderr, "DEBUG: %s recv remote_info, size=%d, rc=%d\n", __func__, (int)sizeof(remote_info), rc);
    if (rc != sizeof(remote_info)) {
        NVME_UQPAIR_ERRLOG(uqpair, "Failed to recv remote info\n");
        return -1;
    }

    fprintf(stderr, "DEBUG: %s remote_info: eid="EID_FMT", uasid=0x%x, seg_va=0x%lx, seg_len=%lu, ",
            __func__, EID_ARGS(remote_info.eid), remote_info.uasid, remote_info.seg_va, remote_info.seg_len);


    // fprintf(stderr, "DEBUG: %s remote_info: seg_flag=0x%x, seg_token_id=0x%x, jetty_id=0x%lx, qid=%u, trans_mode=%u, msg_type=%u\n",
    //         remote_info.seg_flag, remote_info.seg_token_id, remote_info.jetty_id.id, remote_info.qid,
    //         remote_info.trans_mode, remote_info.msg_type);

    /* Build remote jetty info - 参考 urma_client.c 使用 CTP 模式 */
    if (remote_info.qid == 0xFFFF) {
        NVME_UQPAIR_ERRLOG(uqpair, "remote qid is wrong!\n");
        return -1;
    }
    remote_jetty.jetty_id = remote_info.jetty_id;
    remote_jetty.trans_mode = URMA_TM_RM;
    remote_jetty.type = URMA_JETTY;
    remote_jetty.tp_type = URMA_CTP;

    /* Close socket after info exchange */
    spdk_sock_close(&uqpair->sock);

    /* Import remote jetty - CTP 模式不需要手动 bind_jetty */
    uqpair->tjetty = urma_import_jetty(uctrlr->urma_ctx, &remote_jetty, &uctrlr->token);
    if (uqpair->tjetty == NULL) {
        NVME_UQPAIR_ERRLOG(uqpair, "Failed to import remote jetty\n");
        return -1;
    }

    fprintf(stderr, "DEBUG: %s URMA link up, tpn=%u\n", __func__, uqpair->tjetty->tp.tpn);

    /* Import remote segment - 参考 urma_sample.c 的 prepare_client 函数 */
    urma_seg_t remote_seg = {
        .ubva.eid = remote_info.eid,
        .ubva.uasid = remote_info.uasid,
        .ubva.va = remote_info.seg_va,
        .len = remote_info.seg_len,
        .attr.value = remote_info.seg_flag,
        .token_id = remote_info.seg_token_id
    };

    urma_import_seg_flag_t seg_flag = {
        .bs.cacheable = URMA_NON_CACHEABLE,
        .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
        .bs.mapping = URMA_SEG_NOMAP,
        .bs.reserved = 0
    };

    uqpair->remote_tseg = urma_import_seg(uctrlr->urma_ctx, &remote_seg, &uctrlr->token, 0,
                                         seg_flag);
    if (uqpair->remote_tseg == NULL) {
        NVME_UQPAIR_ERRLOG(uqpair, "Failed to import remote segment\n");
        urma_unimport_jetty(uqpair->tjetty);
        uqpair->tjetty = NULL;
        return -1;
    }
    fprintf(stderr, "DEBUG: %s Remote segment imported successfully\n", __func__);

    uqpair->is_connected = true;
    uqpair->state = NVME_UB_JETTY_STATE_READY;

    /* Keep qpair in CONNECTING state - discover will be triggered via poll_group callback
     * after transport connection is ready. This prevents nvme protocol layer from
     * entering discover before connect completes. */
    nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);

    fprintf(stderr, "Connection established successfully\n");

    for (int i = 0; i < uqpair->recv_depth; i++) {
        rc = nvme_ub_post_recv_wr(uqpair, i);
        if (spdk_unlikely(rc != 0)) {
            NVME_UQPAIR_ERRLOG(uqpair, "nvme_ub_post_recv_wr failed\n");
            return -EIO;
        }
    }
    fprintf(stderr, "Post recv wr successfully\n");

    return 0;
}

static void
nvme_ub_sock_connect_cb(void *cb_arg, int status)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (cb_arg=%p, status=%d, qid=%u)\n",
            __func__, cb_arg, status, ((struct nvme_ub_qpair*)cb_arg)->qid);
    struct nvme_ub_qpair *uqpair = cb_arg;
    struct spdk_nvme_qpair *qpair = &uqpair->qpair;
    int rc;

    if (status < 0) {
        NVME_UQPAIR_ERRLOG(uqpair, "Socket connection error %d (%s)\n", status, spdk_strerror(abs(status)));
        nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
        return;
    }

    /* Complete the URMA connection establishment */
    rc = nvme_ub_connect_established(uqpair);
    if (rc != 0) {
        NVME_UQPAIR_ERRLOG(uqpair, "Failed to establish URMA connection\n");
        if (uqpair->sock) {
            spdk_sock_close(&uqpair->sock);
        }
        nvme_qpair_set_state(qpair, NVME_QPAIR_DISCONNECTED);
        return;
    }

    NVME_UQPAIR_DEBUGLOG(uqpair, "Socket connection completed and URMA established\n");
}

static int
nvme_ub_ctrlr_connect_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (ctrlr=%p, qpair=%p, qid=%u)\n",
            __func__, (void*)ctrlr, (void*)qpair, qpair->id);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct spdk_sock_opts opts = {};
    int rc;

    if (uqpair->is_connected) {
        return 0;
    }

    uqpair->state = NVME_UB_JETTY_STATE_CONNECTING;
    uqpair->qpair_state = NVME_UB_QPAIR_STATE_INITIALIZING;

    /* Use SPDK sock async connect - this will use epoll on Linux */
    spdk_sock_get_default_opts(&opts);
    opts.opts_size = sizeof(opts);

    uqpair->sock = spdk_sock_connect_ext(ctrlr->trid.traddr,
                      atoi(ctrlr->trid.trsvcid), NULL, &opts);
    if (uqpair->sock == NULL) {
        NVME_UQPAIR_ERRLOG(uqpair, "spdk_sock_connect_async failed\n");
        return -1;
    }

    rc = nvme_ub_connect_established(uqpair);
    if (rc != 0) {
        NVME_UQPAIR_ERRLOG(uqpair, "Failed to establish URMA connection\n");
        return rc;
    }

    fprintf(stderr, "Async socket connection initiated\n");
    return 0;
}

static void
nvme_ub_qpair_abort_reqs(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (qpair=%p, qid=%u, dnr=%u)\n",
            __func__, (void*)qpair, qpair->id, dnr);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct nvme_ub_request *ub_req, *tmp;
    struct spdk_nvme_cpl cpl;

    cpl.sqid = qpair->id;
    cpl.status.sc = SPDK_NVME_SC_ABORTED_SQ_DELETION;
    cpl.status.sct = SPDK_NVME_SCT_GENERIC;
    cpl.status.dnr = dnr;

    TAILQ_FOREACH_SAFE(ub_req, &uqpair->outstanding_reqs, link, tmp) {
        ub_req->cpl = cpl;
        nvme_ub_req_complete(ub_req, &cpl, false);
    }
}

static int
nvme_ub_qpair_iterate_requests(struct spdk_nvme_qpair *qpair,
                   int (*iter_fn)(struct nvme_request *req, void *arg),
                   void *arg)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (qpair=%p, qid=%u, iter_fn=%p, arg=%p)\n",
            __func__, (void*)qpair, qpair->id, (void*)iter_fn, arg);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct nvme_ub_request *ub_req;

    TAILQ_FOREACH(ub_req, &uqpair->outstanding_reqs, link) {
        if (iter_fn(ub_req->req, arg) != 0) {
            return -1;
        }
    }
    return 0;
}

static void
nvme_ub_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (qpair=%p, qid=%u)\n",
            __func__, (void*)qpair, qpair->id);
    /* AER handling for admin qpair - simplified for now */
}

static int
nvme_ub_qpair_authenticate(struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (qpair=%p, qid=%u)\n",
            __func__, (void*)qpair, qpair->id);
    /* Authentication not implemented yet - assume no auth required */
    return 0;
}

/* Poll Group Implementation */
static struct spdk_nvme_transport_poll_group *
nvme_ub_poll_group_create(void)
{
    fprintf(stderr, "DEBUG: [ENTER] %s\n", __func__);
    struct nvme_ub_poll_group *group;

    group = calloc(1, sizeof(*group));
    if (group == NULL) {
        SPDK_ERRLOG("Unable to allocate poll group.\n");
        return NULL;
    }

    pthread_mutex_init(&group->lock, NULL);
    TAILQ_INIT(&group->qpairs);
    TAILQ_INIT(&group->connecting_qpairs);
    TAILQ_INIT(&group->active_qpairs);
    group->num_qpairs = 0;

    /* Create SPDK sock group for epoll integration */
    group->sock_group = spdk_sock_group_create(NULL);
    if (group->sock_group == NULL) {
        SPDK_ERRLOG("Unable to create sock group.\n");
        pthread_mutex_destroy(&group->lock);
        free(group);
        return NULL;
    }

    return &group->group;
}

static int
nvme_ub_poll_group_destroy(struct spdk_nvme_transport_poll_group *tgroup)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (tgroup=%p)\n", __func__, (void*)tgroup);
    struct nvme_ub_poll_group *group = nvme_ub_poll_group(tgroup);
    int rc;

    if (group->sock_group) {
        rc = spdk_sock_group_close(&group->sock_group);
        if (rc != 0) {
            SPDK_ERRLOG("Unable to close UB socket group, rc=%d\n", rc);
            return rc;
        }
    }
    pthread_mutex_destroy(&group->lock);
    free(group);
    return 0;
}

/* Poll group connect qpair - 参考 nvme_rdma_poll_group_connect_qpair */
static int
nvme_ub_poll_group_connect_qpair(struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (qpair=%p, qid=%u)\n", __func__, (void*)qpair, qpair->id);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct nvme_ub_poll_group *group = nvme_ub_poll_group(qpair->poll_group);

    /* For UB, connection is established synchronously in nvme_ub_ctrlr_connect_qpair.
     * But we need to add the qpair to the connecting list to be consistent with
     * the NVMe upper layer state machine. The actual resubmit will happen in
     * poll_group_process_completions when the state is CONNECTED.
     */
    if (!TAILQ_ENTRY_ENQUEUED(uqpair, link_connecting)) {
        TAILQ_INSERT_TAIL(&group->connecting_qpairs, uqpair, link_connecting);
    }

    return 0;
}

/* Poll group disconnect qpair - 参考 nvme_rdma_poll_group_disconnect_qpair */
static int
nvme_ub_poll_group_disconnect_qpair(struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (qpair=%p, qid=%u)\n", __func__, (void*)qpair, qpair->id);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct nvme_ub_poll_group *group = nvme_ub_poll_group(qpair->poll_group);

    if (TAILQ_ENTRY_ENQUEUED(uqpair, link_connecting)) {
        TAILQ_REMOVE_CLEAR(&group->connecting_qpairs, uqpair, link_connecting);
    }

    if (TAILQ_ENTRY_ENQUEUED(uqpair, link_active)) {
        TAILQ_REMOVE_CLEAR(&group->active_qpairs, uqpair, link_active);
    }

    return 0;
}

/* Poll group add qpair - 参考 nvme_rdma_poll_group_add */
static int
nvme_ub_poll_group_add(struct spdk_nvme_transport_poll_group *tgroup,
                       struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (tgroup=%p, qpair=%p, qid=%u)\n",
            __func__, (void*)tgroup, (void*)qpair, qpair->id);
    struct nvme_ub_poll_group *group = nvme_ub_poll_group(tgroup);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);

    pthread_mutex_lock(&group->lock);

    if (!TAILQ_ENTRY_ENQUEUED(uqpair, group_link)) {
        TAILQ_INSERT_TAIL(&group->qpairs, uqpair, group_link);
        group->num_qpairs++;
    }

    pthread_mutex_unlock(&group->lock);

    return 0;
}

/* Poll group remove qpair - 参考 nvme_rdma_poll_group_remove */
static int
nvme_ub_poll_group_remove(struct spdk_nvme_transport_poll_group *tgroup,
                          struct spdk_nvme_qpair *qpair)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (tgroup=%p, qpair=%p, qid=%u)\n",
            __func__, (void*)tgroup, (void*)qpair, qpair->id);
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct nvme_ub_poll_group *group = nvme_ub_poll_group(tgroup);

    pthread_mutex_lock(&group->lock);

    if (TAILQ_ENTRY_ENQUEUED(uqpair, link_connecting)) {
        TAILQ_REMOVE_CLEAR(&group->connecting_qpairs, uqpair, link_connecting);
    }

    if (TAILQ_ENTRY_ENQUEUED(uqpair, link_active)) {
        TAILQ_REMOVE_CLEAR(&group->active_qpairs, uqpair, link_active);
    }

    if (TAILQ_ENTRY_ENQUEUED(uqpair, group_link)) {
        TAILQ_REMOVE_CLEAR(&group->qpairs, uqpair, group_link);
        group->num_qpairs--;
    }

    pthread_mutex_unlock(&group->lock);

    return 0;
}

static int64_t
nvme_ub_poll_group_process_completions(struct spdk_nvme_transport_poll_group *tgroup,
        uint32_t completions_per_qpair, spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
    struct nvme_ub_poll_group *group = nvme_ub_poll_group(tgroup);
    struct nvme_ub_qpair *uqpair, *tmp_uqpair;
    int64_t total_completions = 0;
    int rc;

    if (completions_per_qpair == 0) {
        completions_per_qpair = NVME_UB_MAX_COMPLETIONS_PER_POLL;
    }

    pthread_mutex_lock(&group->lock);

    if (group->sock_group) {
        rc = spdk_sock_group_poll(group->sock_group);
        if (rc < 0) {
            SPDK_WARNLOG("UB socket group poll failed, rc=%d\n", rc);
        }
    }

    TAILQ_FOREACH_SAFE(uqpair, &group->connecting_qpairs, link_connecting, tmp_uqpair) {
        struct spdk_nvme_qpair *qpair = &uqpair->qpair;

        rc = nvme_ub_ctrlr_connect_qpair_poll(qpair->ctrlr, qpair);
        if (rc != -EAGAIN) {
            TAILQ_REMOVE_CLEAR(&group->connecting_qpairs, uqpair, link_connecting);
            if (rc == 0) {
                nvme_qpair_resubmit_requests(qpair, uqpair->num_entries);
            } else {
                NVME_UQPAIR_ERRLOG(uqpair, "Failed to connect, rc=%d\n", rc);
                nvme_ub_fail_qpair(qpair, 0);
            }
        }
    }

    TAILQ_FOREACH_SAFE(uqpair, &group->active_qpairs, link_active, tmp_uqpair) {
        uint32_t qpair_completions = 0;
        uint32_t i;

        for (i = 0; i < completions_per_qpair; i++) {
            urma_cr_t cr = {0};
            int cnt;

            cnt = urma_poll_jfc(uqpair->jfc, 1, &cr);
            if (cnt <= 0) {
                break;
            }

            rc = nvme_ub_process_cr(uqpair, &cr);
            if (spdk_unlikely(rc < 0)) {
                NVME_UQPAIR_ERRLOG(uqpair, "Failed to process completion, rc=%d\n", rc);
                nvme_ub_fail_qpair(&uqpair->qpair, 0);
                break;
            }
            qpair_completions += rc;
            total_completions += rc;
        }

        /* Unlike the single-qpair completion path, the generic poll-group
         * layer only aggregates completion counts.  The transport must
         * resubmit this qpair's software-queued requests itself. */
        if (qpair_completions > 0 && uqpair->is_connected) {
            nvme_qpair_resubmit_requests(&uqpair->qpair, qpair_completions);
        }

        if (!uqpair->is_connected && disconnected_qpair_cb != NULL) {
            disconnected_qpair_cb(&uqpair->qpair, tgroup->group->ctx);
        }
    }

    pthread_mutex_unlock(&group->lock);
    return total_completions;
}

static void
nvme_ub_poll_group_check_disconnected_qpairs(struct spdk_nvme_transport_poll_group *tgroup,
        spdk_nvme_disconnected_qpair_cb disconnected_qpair_cb)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (tgroup=%p)\n", __func__, (void*)tgroup);
    struct nvme_ub_poll_group *group = nvme_ub_poll_group(tgroup);
    struct nvme_ub_qpair *uqpair, *tmp;

    pthread_mutex_lock(&group->lock);

    TAILQ_FOREACH_SAFE(uqpair, &group->qpairs, group_link, tmp) {
        if (!uqpair->is_connected && uqpair->state != NVME_UB_JETTY_STATE_CONNECTING) {
            if (disconnected_qpair_cb) {
                disconnected_qpair_cb(&uqpair->qpair, NULL);
            }
        }
    }

    pthread_mutex_unlock(&group->lock);
}

static int
nvme_ub_poll_group_get_stats(struct spdk_nvme_transport_poll_group *tgroup,
                 struct spdk_nvme_transport_poll_group_stat **stats)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (tgroup=%p)\n", __func__, (void*)tgroup);
    /* Statistics not implemented yet */
    *stats = NULL;
    return -ENOTSUP;
}

static void
nvme_ub_poll_group_free_stats(struct spdk_nvme_transport_poll_group *tgroup,
                   struct spdk_nvme_transport_poll_group_stat *stats)
{
    fprintf(stderr, "DEBUG: [ENTER] %s (tgroup=%p, stats=%p)\n", __func__, (void*)tgroup, (void*)stats);
    /* No stats to free */
}


static spdk_nvme_ctrlr_t *
nvme_ub_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
              const struct spdk_nvme_ctrlr_opts *opts,
              void *devhandle)
{
    struct nvme_ub_ctrlr *uctrlr;

    fprintf(stderr, "DEBUG: nvme_ub_ctrlr_construct called, trid trtype=%d, traddr=%s, trsvcid=%s\n",
            trid->trtype, trid->traddr, trid->trsvcid);
    urma_device_t **urma_devs;
    urma_device_t *urma_dev = NULL;
    urma_eid_info_t *eid_list = NULL;
    urma_device_attr_t dev_attr;
    uint32_t eid_cnt = 0;
    int eid_index = -1;
    int i, rc;
    int num_devices = 0;
    char dev_name[URMA_MAX_DEV_NAME] = {0};
    struct nvme_ub_qpair *admin_uqpair;
    urma_jfc_cfg_t jfc_cfg;
    urma_jfr_cfg_t jfr_cfg;
    urma_jfs_cfg_t jfs_cfg;
    urma_jetty_cfg_t jetty_cfg;
    urma_reg_seg_flag_t seg_flag;
    urma_seg_cfg_t seg_cfg;
    uint32_t admin_queue_size;
    uint32_t admin_jfc_depth;

    /* Initialize urma lib */
    urma_init_attr_t init_attr = {
        .uasid = 0,
    };

    if (urma_init(&init_attr) != URMA_SUCCESS) {
        SPDK_ERRLOG("Failed to urma init\n");
        return NULL;
    }

    uctrlr = spdk_zmalloc(sizeof(struct nvme_ub_ctrlr), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
                  SPDK_MALLOC_DMA);
    if (uctrlr == NULL) {
        SPDK_ERRLOG("could not allocate ctrlr\n");
        urma_uninit();
        return NULL;
    }
    fprintf(stderr, "DEBUG: uctrlr allocated at %p\n", (void*)uctrlr);

    /* Initialize qpairs list */
    TAILQ_INIT(&uctrlr->qpairs);

    uctrlr->ctrlr.opts = *opts;
    uctrlr->ctrlr.trid = *trid;

    /* Get device list and find a suitable device */
    urma_devs = urma_get_device_list(&num_devices);
    if (urma_devs == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "ub_get_devices() failed: %s (%d)\n", spdk_strerror(errno),
                  errno);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "urma_get_device_list returned %d devices\n", num_devices);

    uctrlr->max_sge = 65535; /* Start with max value */
    i = 0;

    /* Find first available device and query its attributes */
    for (i;i < num_devices;i++){
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Device %d: name=%s\n",i, urma_devs[i]->name);
    }
    urma_free_device_list(urma_devs);

    char *tmp_dev_name = "udmac0d1e2";
    urma_dev = urma_get_device_by_name(tmp_dev_name);
    if (urma_dev == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "urma get device by name failed!\n");
        return NULL;
    }

    rc = urma_query_device(urma_dev, &dev_attr);
    if (rc < 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to query UB device attributes.\n");
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "name=%s, max_jfc_depth=%u, max_jfs_sge=%u\n",
                urma_dev->name, dev_attr.dev_cap.max_jfc_depth, dev_attr.dev_cap.max_jfs_sge);
    uctrlr->max_sge = spdk_min(uctrlr->max_sge, (uint16_t)dev_attr.dev_cap.max_jfs_sge);
    uctrlr->dev_attr = dev_attr;
    strncpy(uctrlr->dev_name, urma_dev->name, URMA_MAX_DEV_NAME - 1);

    if (urma_dev == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "No URMA device found.\n");
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "Using device: %s\n", uctrlr->dev_name);

    /* Get EID index */
    eid_list = urma_get_eid_list(urma_dev, &eid_cnt);
    if (eid_list == NULL || eid_cnt == 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to get EID list.\n");
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    fprintf(stderr, "DEBUG: eid_cnt=%d\n",eid_cnt);
    eid_index = eid_list[0].eid_index;
    uctrlr->eid_index = eid_index;
    fprintf(stderr, "DEBUG: eid_index=%d, eid="EID_FMT"\n", eid_index, EID_ARGS(eid_list[0].eid));
    urma_free_eid_list(eid_list);

    /* Create URMA context */
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "Creating URMA context with dev=%p, eid_index=%d\n",
              (void*)urma_dev, eid_index);
    uctrlr->urma_ctx = urma_create_context(urma_dev, (uint32_t)eid_index);
    if (uctrlr->urma_ctx == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to create URMA context, errno=%d.\n", errno);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "URMA context created successfully, eid="EID_FMT"\n",
              EID_ARGS(uctrlr->urma_ctx->eid));

    /* Initialize token for memory registration */
    uctrlr->token.token = 0xABCD; /* Use a default token value */

    /* Create JFCE (Jetty Completion Event Channel) */
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "Creating JFCE...\n");
    uctrlr->jfce = urma_create_jfce(uctrlr->urma_ctx);
    if (uctrlr->jfce == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to create JFCE, errno=%d.\n", errno);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "JFCE created successfully.\n");

    /* Create JFC for admin qpair */
    admin_queue_size = opts->admin_queue_size;
    fprintf(stderr, "DEBUG: admin_queue_size = %u, max_jfc_depth = %u\n",
            admin_queue_size, uctrlr->dev_attr.dev_cap.max_jfc_depth);
    /* Validate against device capabilities */
    if (admin_queue_size > uctrlr->dev_attr.dev_cap.max_jfc_depth) {
        admin_queue_size = uctrlr->dev_attr.dev_cap.max_jfc_depth;
    }
    if (admin_queue_size == 0) {
        admin_queue_size = 32; /* default */
    }
    fprintf(stderr, "DEBUG: Using admin_queue_size = %u, jfce = %p\n", admin_queue_size, (void*)uctrlr->jfce);
    admin_jfc_depth = admin_queue_size <= UINT32_MAX / 2 ? admin_queue_size * 2 : UINT32_MAX;
    admin_jfc_depth = spdk_min(admin_jfc_depth, uctrlr->dev_attr.dev_cap.max_jfc_depth);
    memset(&jfc_cfg, 0, sizeof(jfc_cfg));
    jfc_cfg.depth = admin_jfc_depth;
    jfc_cfg.flag.value = 0;
    jfc_cfg.jfce = uctrlr->jfce;
    jfc_cfg.user_ctx = 0;

    fprintf(stderr, "DEBUG: Creating JFC with depth=%u, jfce=%p\n",
              jfc_cfg.depth, (void*)jfc_cfg.jfce);
    uctrlr->jfc = urma_create_jfc(uctrlr->urma_ctx, &jfc_cfg);
    if (uctrlr->jfc == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to create admin JFC, errno=%d.\n", errno);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "JFC created successfully.\n");

    /* Create admin JFR */
    memset(&jfr_cfg, 0, sizeof(jfr_cfg));
    jfr_cfg.depth = admin_queue_size;
    jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
    jfr_cfg.flag.bs.order_type = 0;
    jfr_cfg.trans_mode = URMA_TM_RM;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER * 10;
    jfr_cfg.jfc = uctrlr->jfc;
    jfr_cfg.token_value = uctrlr->token;
    jfr_cfg.id = 0; /* Admin qpair has qid 0 */
    jfr_cfg.max_sge = 1;

    fprintf(stderr, "DEBUG: Creating admin JFR with depth=%u, jfc=%p\n",
            jfr_cfg.depth, (void*)jfr_cfg.jfc);
    urma_jfr_t *admin_jfr = urma_create_jfr(uctrlr->urma_ctx, &jfr_cfg);
    if (admin_jfr == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to create admin JFR, errno=%d.\n", errno);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    fprintf(stderr, "DEBUG: Admin JFR created successfully, jfr=%p\n", (void*)admin_jfr);

    /* Create admin JFS */
    memset(&jfs_cfg, 0, sizeof(jfs_cfg));
    jfs_cfg.depth = admin_queue_size;
    jfs_cfg.flag.bs.order_type = 0;
    jfs_cfg.flag.bs.multi_path = 0;
    jfs_cfg.trans_mode = URMA_TM_RM;
    jfs_cfg.priority = URMA_MAX_PRIORITY;
    jfs_cfg.max_sge = uctrlr->max_sge;
    // jfs_cfg.max_sge = 1;
    jfs_cfg.max_inline_data = 0;
    jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
    jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
    jfs_cfg.jfc = uctrlr->jfc;
    jfs_cfg.user_ctx = 0;

    /* Create admin jetty (combined JFS + JFR) */
    memset(&jetty_cfg, 0, sizeof(jetty_cfg));
    jetty_cfg.flag.bs.share_jfr = 1; /* UB dev must use shared JFR */
    jetty_cfg.jfs_cfg = jfs_cfg;
    jetty_cfg.shared.jfr = admin_jfr;
    jetty_cfg.shared.jfc = uctrlr->jfc;

    fprintf(stderr, "DEBUG: Creating admin jetty with share_jfr=1, jfs depth=%u, max_sge=%u, shared.jfr=%p\n",
            jfs_cfg.depth, jfs_cfg.max_sge, (void*)admin_jfr);
    uctrlr->admin_jetty = urma_create_jetty(uctrlr->urma_ctx, &jetty_cfg);
    if (uctrlr->admin_jetty == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to create admin jetty, errno=%d.\n", errno);
        urma_delete_jfr(admin_jfr);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    fprintf(stderr, "DEBUG: Creating admin jetty =%d\n", uctrlr->admin_jetty);

    /* Allocate and register admin send/recv buffers as URMA segments */
    uctrlr->buffer_size = PAGE_SIZE * 512;
    rc = nvme_ub_register_segs(uctrlr->urma_ctx, &uctrlr->token, uctrlr->buffer_size,
                               &uctrlr->send_buffer, &uctrlr->recv_buffer,
                               &uctrlr->send_tseg, &uctrlr->recv_tseg);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "failed to register admin segments, rc=%d\n", rc);
        urma_delete_jetty(uctrlr->admin_jetty);
        urma_delete_jfr(admin_jfr);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    fprintf(stderr, "DEBUG: Admin segments registered: send_tseg=%p, recv_tseg=%p\n",
            (void*)uctrlr->send_tseg, (void*)uctrlr->recv_tseg);

    /* Construct the NVMe controller base */
    fprintf(stderr, "DEBUG: Calling nvme_ctrlr_construct, ctrlr=%p\n", (void*)&uctrlr->ctrlr);
    rc = nvme_ctrlr_construct(&uctrlr->ctrlr);
    if (rc != 0) {
        fprintf(stderr, "DEBUG: nvme_ctrlr_construct failed, rc=%d\n", rc);
        urma_delete_jetty(uctrlr->admin_jetty);
        urma_delete_jfr(admin_jfr);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    fprintf(stderr, "DEBUG: nvme_ctrlr_construct succeeded\n");

    /* Create admin qpair - use a separate function that sets up URMA resources */
    admin_uqpair = spdk_zmalloc(sizeof(struct nvme_ub_qpair), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
                    SPDK_MALLOC_DMA);
    if (!admin_uqpair) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "failed to allocate admin uqpair\n");
        nvme_ctrlr_destruct(&uctrlr->ctrlr);
        urma_delete_jetty(uctrlr->admin_jetty);
        urma_delete_jfr(admin_jfr);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }
    fprintf(stderr, "DEBUG: admin_uqpair allocated at %p\n", (void*)admin_uqpair);

    admin_uqpair->uctrlr = uctrlr;
    admin_uqpair->sock = NULL;
    admin_uqpair->jfce = uctrlr->jfce;
    admin_uqpair->jfc = uctrlr->jfc;
    admin_uqpair->jfr = admin_jfr; /* Store admin JFR for cleanup */
    admin_uqpair->num_entries = admin_queue_size - 1;
    admin_uqpair->delay_cmd_submit = false;
    admin_uqpair->qid = 0;
    admin_uqpair->sq_depth = admin_queue_size;
    admin_uqpair->cq_depth = admin_queue_size;
    admin_uqpair->recv_depth = admin_uqpair->num_entries;
    admin_uqpair->payload_buffer_offset = 256 * PAGE_SIZE;
    admin_uqpair->state = NVME_UB_JETTY_STATE_RESET;
    admin_uqpair->qpair_state = NVME_UB_QPAIR_STATE_INVALID;
    admin_uqpair->jetty = uctrlr->admin_jetty;
    admin_uqpair->jetty_id = uctrlr->admin_jetty->jetty_id;

    TAILQ_INIT(&admin_uqpair->free_reqs);
    TAILQ_INIT(&admin_uqpair->outstanding_reqs);

    admin_uqpair->recv_ctxs = calloc(admin_uqpair->recv_depth,
                                     sizeof(*admin_uqpair->recv_ctxs));
    if (admin_uqpair->recv_ctxs == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "failed to allocate admin receive contexts\n");
        spdk_free(admin_uqpair);
        nvme_ctrlr_destruct(&uctrlr->ctrlr);
        urma_delete_jetty(uctrlr->admin_jetty);
        urma_delete_jfr(admin_jfr);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }

    rc = nvme_ub_alloc_qpair_reqs(admin_uqpair, admin_uqpair->num_entries);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "failed to allocate admin requests\n");
        free(admin_uqpair->recv_ctxs);
        spdk_free(admin_uqpair);
        nvme_ctrlr_destruct(&uctrlr->ctrlr);
        urma_delete_jetty(uctrlr->admin_jetty);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }

    /* Initialize admin qpair via nvme layer */
    rc = nvme_qpair_init(&admin_uqpair->qpair, 0, &uctrlr->ctrlr, 0, admin_queue_size, true);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "nvme_qpair_init for admin failed\n");
        nvme_ub_free_qpair_reqs(admin_uqpair);
        free(admin_uqpair->recv_ctxs);
        urma_delete_jfr(admin_uqpair->jfr);
        spdk_free(admin_uqpair);
        nvme_ctrlr_destruct(&uctrlr->ctrlr);
        urma_delete_jetty(uctrlr->admin_jetty);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }

    uctrlr->ctrlr.adminq = &admin_uqpair->qpair;
    TAILQ_INSERT_TAIL(&uctrlr->qpairs, admin_uqpair, ctrlr_link);

    if (nvme_ctrlr_add_process(&uctrlr->ctrlr, 0) != 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "nvme_ctrlr_add_process() failed\n");
        nvme_qpair_deinit(&admin_uqpair->qpair);
        nvme_ub_free_qpair_reqs(admin_uqpair);
        free(admin_uqpair->recv_ctxs);
        urma_delete_jfr(admin_uqpair->jfr);
        spdk_free(admin_uqpair);
        nvme_ctrlr_destruct(&uctrlr->ctrlr);
        urma_delete_jetty(uctrlr->admin_jetty);
        urma_delete_jfc(uctrlr->jfc);
        urma_delete_jfce(uctrlr->jfce);
        urma_delete_context(uctrlr->urma_ctx);
        spdk_free(uctrlr);
        urma_uninit();
        return NULL;
    }

    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "successfully initialized the nvmf ctrlr\n");
    return &uctrlr->ctrlr;
}

const struct spdk_nvme_transport_ops ub_ops = {
    .name = "UB",
    .type = SPDK_NVME_TRANSPORT_UB,
    .ctrlr_construct = nvme_ub_ctrlr_construct,
    .ctrlr_scan = nvme_fabric_ctrlr_scan,
    .ctrlr_destruct = nvme_ub_ctrlr_destruct,
    .ctrlr_enable = nvme_ub_ctrlr_enable,

    .ctrlr_set_reg_4 = nvme_fabric_ctrlr_set_reg_4,
    .ctrlr_set_reg_8 = nvme_fabric_ctrlr_set_reg_8,
    .ctrlr_get_reg_4 = nvme_fabric_ctrlr_get_reg_4,
    .ctrlr_get_reg_8 = nvme_fabric_ctrlr_get_reg_8,
    .ctrlr_set_reg_4_async = nvme_fabric_ctrlr_set_reg_4_async,
    .ctrlr_set_reg_8_async = nvme_fabric_ctrlr_set_reg_8_async,
    .ctrlr_get_reg_4_async = nvme_fabric_ctrlr_get_reg_4_async,
    .ctrlr_get_reg_8_async = nvme_fabric_ctrlr_get_reg_8_async,

    .ctrlr_get_max_xfer_size = nvme_ub_ctrlr_get_max_xfer_size,
    .ctrlr_get_max_sges = nvme_ub_ctrlr_get_max_sges,

    .ctrlr_create_io_qpair = nvme_ub_ctrlr_create_io_qpair,
    .ctrlr_delete_io_qpair = nvme_ub_ctrlr_delete_io_qpair,
    .ctrlr_connect_qpair = nvme_ub_ctrlr_connect_qpair,
    .ctrlr_disconnect_qpair = nvme_ub_ctrlr_disconnect_qpair,

    .ctrlr_get_memory_domains = NULL,
    .ctrlr_process_transport_events = NULL,

    .qpair_abort_reqs = nvme_ub_qpair_abort_reqs,
    .qpair_reset = nvme_ub_qpair_reset,
    .qpair_submit_request = nvme_ub_qpair_submit_request,
    .qpair_process_completions = nvme_ub_qpair_process_completions,
    .qpair_iterate_requests = nvme_ub_qpair_iterate_requests,
    .qpair_authenticate = nvme_ub_qpair_authenticate,
    .admin_qpair_abort_aers = nvme_ub_admin_qpair_abort_aers,

    .poll_group_create = nvme_ub_poll_group_create,
    .poll_group_connect_qpair = nvme_ub_poll_group_connect_qpair,
    .poll_group_disconnect_qpair = nvme_ub_poll_group_disconnect_qpair,
    .poll_group_add = nvme_ub_poll_group_add,
    .poll_group_remove = nvme_ub_poll_group_remove,
    .poll_group_process_completions = nvme_ub_poll_group_process_completions,
    .poll_group_check_disconnected_qpairs = nvme_ub_poll_group_check_disconnected_qpairs,
    .poll_group_destroy = nvme_ub_poll_group_destroy,
    .poll_group_get_stats = nvme_ub_poll_group_get_stats,
    .poll_group_free_stats = nvme_ub_poll_group_free_stats,
};

SPDK_NVME_TRANSPORT_REGISTER(ub, &ub_ops);
