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
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/trace.h"
#include "spdk/queue.h"
#include "spdk/nvme.h"
#include "spdk/nvme_ub.h"
#include "spdk/nvmf_spec.h"
#include "spdk/string.h"
#include "spdk/endian.h"
#include "spdk/likely.h"
#include "spdk/config.h"
#include "spdk/sock.h"

#include "nvme_internal.h"
#include "spdk/tree.h"
#include "spdk_internal/sgl.h"
#include "spdk_internal/nvme_ub.h"

#include "urma/urma_api.h"

SPDK_STATIC_ASSERT(URMA_EID_SIZE == SPDK_NVME_UB_EID_SIZE,
                   "URMA EID size does not match UB OOB protocol");

/* need UB header */
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

#define MSG_SIZE 4096
#define MAX_IO_SIZE  (128 * 1024)
#define NVME_UB_MAX_COMPLETIONS_PER_POLL 128
#define NVME_UB_COMPLETION_BATCH_SIZE 32
#define NVME_UB_MAX_SEND_SIGNAL_INTERVAL 16
#define URMA_DEVICE_NAME_ENV "URMA_DEVICE_NAME"
#define URMA_DEFAULT_DEVICE_NAME "bonding_dev_0"
#define URMA_EID_INDEX_ENV "URMA_EID_INDEX"
#define URMA_BONDING_DEVICE_PREFIX "bonding_dev_"

static bool
nvme_ub_device_uses_multipath(const char *dev_name)
{
    return dev_name != NULL &&
           strncmp(dev_name, URMA_BONDING_DEVICE_PREFIX,
                   sizeof(URMA_BONDING_DEVICE_PREFIX) - 1) == 0;
}

static int
nvme_ub_get_eid_index(urma_device_t *dev)
{
    urma_eid_info_t *eid_list;
    const char *value;
    uint32_t eid_cnt, i;
    long eid_index;
    int selected;

    eid_list = urma_get_eid_list(dev, &eid_cnt);
    if (eid_list == NULL) {
        SPDK_ERRLOG("Failed to get EID list for URMA device %s\n", dev->name);
        return -ENODEV;
    }

    if (eid_cnt == 0) {
        SPDK_ERRLOG("URMA device %s has no EIDs\n", dev->name);
        urma_free_eid_list(eid_list);
        return -ENODEV;
    }

    value = getenv(URMA_EID_INDEX_ENV);
    if (value == NULL || value[0] == '\0') {
        selected = (int)eid_list[0].eid_index;
        urma_free_eid_list(eid_list);
        return selected;
    }

    eid_index = spdk_strtol(value, 10);
    if (eid_index < 0 || eid_index > INT_MAX) {
        SPDK_ERRLOG("Invalid %s value '%s'; expected an integer between 0 and %d\n",
                    URMA_EID_INDEX_ENV, value, INT_MAX);
        urma_free_eid_list(eid_list);
        return -EINVAL;
    }

    for (i = 0; i < eid_cnt; i++) {
        if (eid_list[i].eid_index == (uint32_t)eid_index) {
            selected = (int)eid_index;
            urma_free_eid_list(eid_list);
            return selected;
        }
    }

    SPDK_ERRLOG("%s=%ld is not present on URMA device %s\n",
                URMA_EID_INDEX_ENV, eid_index, dev->name);
    urma_free_eid_list(eid_list);
    return -ENODEV;
}

static pthread_once_t g_nvme_ub_urma_once = PTHREAD_ONCE_INIT;
static int g_nvme_ub_urma_init_rc = -EIO;
static bool g_nvme_ub_urma_initialized;

static void
nvme_ub_urma_init_once(void)
{
    urma_init_attr_t init_attr = {
        .uasid = 0,
    };

    g_nvme_ub_urma_init_rc = urma_init(&init_attr);
    if (g_nvme_ub_urma_init_rc == URMA_SUCCESS) {
        g_nvme_ub_urma_initialized = true;
    } else if (g_nvme_ub_urma_init_rc == EEXIST) {
        /*
         * Some UB/NPU runtimes initialize liburma before the NVMe transport
         * is constructed.  URMA is process-wide, so reuse that instance.  Do
         * not mark it as owned by this transport: its original owner remains
         * responsible for calling urma_uninit().
         */
        SPDK_NOTICELOG("URMA library was already initialized; reusing the existing instance\n");
        g_nvme_ub_urma_init_rc = URMA_SUCCESS;
    }
}

static int
nvme_ub_urma_init(void)
{
    int rc;

    rc = pthread_once(&g_nvme_ub_urma_once, nvme_ub_urma_init_once);
    if (rc != 0) {
        SPDK_ERRLOG("pthread_once failed while initializing URMA: %s\n", strerror(rc));
        return -rc;
    }

    return g_nvme_ub_urma_init_rc;
}

__attribute__((destructor)) static void
nvme_ub_urma_fini(void)
{
    if (g_nvme_ub_urma_initialized) {
        urma_uninit();
        g_nvme_ub_urma_initialized = false;
    }
}

#define NVME_UQPAIR_ERRLOG(uqpair, format, ...) NVME_QPAIR_ERRLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_WARNLOG(uqpair, format, ...) NVME_QPAIR_WARNLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_NOTICELOG(uqpair, format, ...) NVME_QPAIR_NOTICELOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_INFOLOG(uqpair, format, ...) NVME_QPAIR_INFOLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)
#define NVME_UQPAIR_DEBUGLOG(uqpair, format, ...) NVME_QPAIR_DEBUGLOG((uqpair) ? &(uqpair)->qpair : NULL, format, ##__VA_ARGS__)


enum nvme_ub_qpair_state {
    NVME_UB_JETTY_STATE_RESET = 0,
    NVME_UB_JETTY_STATE_CONNECTING = 1,
    NVME_UB_JETTY_STATE_READY = 2,
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

struct nvme_ub_npu_endpoint {
    uint32_t endpoint_id;
    uint32_t token;
    uint32_t rjetty_context_size;
    void *rjetty_context;
    TAILQ_ENTRY(nvme_ub_npu_endpoint) link;
};

struct nvme_ub_npu_region {
    uint32_t region_id;
    uint32_t endpoint_id;
    uint32_t token;
    uint64_t user_base;
    uint64_t remote_base;
    uint64_t length;
    uint32_t segment_context_size;
    void *segment_context;
    TAILQ_ENTRY(nvme_ub_npu_region) link;
};

struct cq_ctx {
    struct nvme_ub_qpair *uqpair;
    int id;
};

struct spdk_nvmf_ub_cmd {
    struct spdk_nvme_cmd cmd;
    struct spdk_nvme_sgl_descriptor sgl[SPDK_NVME_UB_MAX_SGL_DESCRIPTORS];
};
SPDK_STATIC_ASSERT(offsetof(struct spdk_nvmf_ub_cmd, sgl) == sizeof(struct spdk_nvme_cmd),
                   "UB SGL descriptors must immediately follow the NVMe command");
SPDK_STATIC_ASSERT(sizeof(struct spdk_nvmf_ub_cmd) <= MSG_SIZE,
                   "UB command capsule exceeds the receive message size");

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
    urma_jfr_t *jfr;        /* Jetty for Receive */
    urma_jetty_id_t jetty_id;

    /* Queue Attributes */
    uint16_t qid;
    uint16_t num_entries;

    /* Request Tracking */
    struct nvme_ub_request **reqs;
    uint16_t num_requests;
    TAILQ_HEAD(, nvme_ub_request) free_reqs;
    TAILQ_HEAD(, nvme_ub_request) outstanding_reqs;
    uint16_t outstanding_requests;

    /* Periodically request command SEND completions to reclaim JFS entries
     * without generating one completion for every I/O. */
    uint16_t sends_since_signal;
    uint16_t send_signal_interval;
    uint64_t command_send_wrs;
    uint64_t command_send_cqes;

    /* Payload registration statistics. Updated by the qpair owner thread. */
    uint64_t direct_payload_ios;
    uint64_t direct_payload_bytes;
    uint64_t staged_payload_ios;
    uint64_t staged_payload_bytes;

    /* Remote target jetty for communication */
    urma_target_jetty_t *tjetty;

    /* Memory Registration */
    urma_target_seg_t *cmd_tseg;
    urma_target_seg_t *resp_tseg;
    void *cmd_buffer;
    void *resp_buffer;
    uint64_t cmd_buffer_size;
    uint64_t resp_buffer_size;
    uint64_t payload_buffer_offset;
    struct cq_ctx *recv_ctxs;
    uint16_t recv_depth;

    /* Connection State */
    bool is_connected;

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
    urma_sge_t sge[2];    /* Command + Data SGEs */
    uint32_t num_sge;
    uint32_t command_capsule_len;
    struct spdk_nvme_sgl_descriptor sgl[SPDK_NVME_UB_MAX_SGL_DESCRIPTORS];

    /* Memory Buffers */
    void *cmd_buffer;
    void *data_buffer;
    uint32_t data_len;
    urma_target_seg_t *data_tseg;
    void *addr;
    bool payload_staged;

    TAILQ_ENTRY(nvme_ub_request) link;
};

/* URMA Poll Group Structure */
struct nvme_ub_poll_group {
    struct spdk_nvme_transport_poll_group group;
    pthread_mutex_t lock;
    TAILQ_HEAD(, nvme_ub_qpair) qpairs;
    TAILQ_HEAD(, nvme_ub_qpair) connecting_qpairs;  /* Qpairs being connected */
    TAILQ_HEAD(, nvme_ub_qpair) active_qpairs;      /* Active qpairs */
    struct spdk_sock_group *sock_group;
};

/* NVMe UB transport extensions for spdk_nvme_ctrlr */
struct nvme_ub_ctrlr {
    struct spdk_nvme_ctrlr ctrlr;

    urma_context_t *urma_ctx;
    urma_device_attr_t dev_attr;
    bool multi_path;

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
    struct spdk_mem_map *mem_map;

    /* Token for memory registration */
    urma_token_t token;

    /* Queue Management */
    uint32_t current_io_queues;
    TAILQ_HEAD(, nvme_ub_qpair) qpairs;

    /* NPU resources are immutable while an I/O qpair exists. */
    pthread_mutex_t npu_lock;
    bool npu_registry_initialized;
    uint32_t next_region_id;
    uint64_t npu_registry_generation;
    TAILQ_HEAD(, nvme_ub_npu_endpoint) npu_endpoints;
    TAILQ_HEAD(, nvme_ub_npu_region) npu_regions;
};

/* We have to use the typedef in the function declaration to appease astyle. */
typedef struct spdk_nvme_ctrlr spdk_nvme_ctrlr_t;

/* Inline helper functions */
static inline struct nvme_ub_ctrlr *
nvme_ub_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
    assert(ctrlr->trid.trtype == SPDK_NVME_TRANSPORT_UB);
    return SPDK_CONTAINEROF(ctrlr, struct nvme_ub_ctrlr, ctrlr);
}

static struct nvme_ub_npu_endpoint *
nvme_ub_find_npu_endpoint(struct nvme_ub_ctrlr *uctrlr, uint32_t endpoint_id)
{
    struct nvme_ub_npu_endpoint *endpoint;

    TAILQ_FOREACH(endpoint, &uctrlr->npu_endpoints, link) {
        if (endpoint->endpoint_id == endpoint_id) {
            return endpoint;
        }
    }

    return NULL;
}

static struct nvme_ub_npu_region *
nvme_ub_find_npu_region_by_id(struct nvme_ub_ctrlr *uctrlr, uint32_t region_id)
{
    struct nvme_ub_npu_region *region;

    TAILQ_FOREACH(region, &uctrlr->npu_regions, link) {
        if (region->region_id == region_id) {
            return region;
        }
    }

    return NULL;
}

static bool
nvme_ub_ranges_overlap(uint64_t base1, uint64_t len1, uint64_t base2, uint64_t len2)
{
    return base1 < base2 + len2 && base2 < base1 + len1;
}

int
spdk_nvme_ub_register_npu_endpoint(struct spdk_nvme_ctrlr *ctrlr,
                                   const struct spdk_nvme_ub_npu_endpoint_info *info)
{
    struct nvme_ub_npu_endpoint *endpoint;
    struct nvme_ub_ctrlr *uctrlr;
    uint32_t endpoint_count = 0;
    int rc = 0;

    if (ctrlr == NULL || ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_UB) {
        return -ENOTSUP;
    }
    if (info == NULL || info->size < sizeof(*info) || info->endpoint_id == 0 ||
        info->rjetty_context == NULL || info->rjetty_context_size < sizeof(urma_rjetty_t) ||
        info->rjetty_context_size > SPDK_NVME_UB_OOB_MAX_CONTEXT_SIZE) {
        return -EINVAL;
    }

    uctrlr = nvme_ub_ctrlr(ctrlr);
    pthread_mutex_lock(&uctrlr->npu_lock);
    if (uctrlr->current_io_queues != 0) {
        rc = -EBUSY;
        goto out;
    }
    if (nvme_ub_find_npu_endpoint(uctrlr, info->endpoint_id) != NULL) {
        rc = -EEXIST;
        goto out;
    }
    TAILQ_FOREACH(endpoint, &uctrlr->npu_endpoints, link) {
        endpoint_count++;
    }
    if (endpoint_count >= SPDK_NVME_UB_OOB_MAX_ENDPOINTS) {
        rc = -ENOSPC;
        goto out;
    }

    endpoint = calloc(1, sizeof(*endpoint));
    if (endpoint == NULL) {
        rc = -ENOMEM;
        goto out;
    }
    endpoint->rjetty_context = malloc(info->rjetty_context_size);
    if (endpoint->rjetty_context == NULL) {
        free(endpoint);
        rc = -ENOMEM;
        goto out;
    }

    endpoint->endpoint_id = info->endpoint_id;
    endpoint->token = info->token;
    endpoint->rjetty_context_size = info->rjetty_context_size;
    memcpy(endpoint->rjetty_context, info->rjetty_context, info->rjetty_context_size);
    TAILQ_INSERT_TAIL(&uctrlr->npu_endpoints, endpoint, link);
    uctrlr->npu_registry_generation++;

out:
    pthread_mutex_unlock(&uctrlr->npu_lock);
    return rc;
}

int
spdk_nvme_ub_unregister_npu_endpoint(struct spdk_nvme_ctrlr *ctrlr, uint32_t endpoint_id)
{
    struct nvme_ub_npu_endpoint *endpoint;
    struct nvme_ub_npu_region *region;
    struct nvme_ub_ctrlr *uctrlr;
    int rc = 0;

    if (ctrlr == NULL || ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_UB) {
        return -ENOTSUP;
    }

    uctrlr = nvme_ub_ctrlr(ctrlr);
    pthread_mutex_lock(&uctrlr->npu_lock);
    if (uctrlr->current_io_queues != 0) {
        rc = -EBUSY;
        goto out;
    }
    endpoint = nvme_ub_find_npu_endpoint(uctrlr, endpoint_id);
    if (endpoint == NULL) {
        rc = -ENOENT;
        goto out;
    }
    TAILQ_FOREACH(region, &uctrlr->npu_regions, link) {
        if (region->endpoint_id == endpoint_id) {
            rc = -EBUSY;
            goto out;
        }
    }

    TAILQ_REMOVE(&uctrlr->npu_endpoints, endpoint, link);
    free(endpoint->rjetty_context);
    free(endpoint);
    uctrlr->npu_registry_generation++;

out:
    pthread_mutex_unlock(&uctrlr->npu_lock);
    return rc;
}

int
spdk_nvme_ub_register_npu_region(struct spdk_nvme_ctrlr *ctrlr,
                                 const struct spdk_nvme_ub_npu_region_info *info,
                                 uint32_t *region_id)
{
    const urma_seg_t *segment;
    struct nvme_ub_npu_region *region, *other;
    struct nvme_ub_ctrlr *uctrlr;
    uint32_t candidate;
    uint32_t region_count = 0;
    int rc = 0;

    if (ctrlr == NULL || ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_UB) {
        return -ENOTSUP;
    }
    if (info == NULL || info->size < sizeof(*info) || region_id == NULL ||
        info->endpoint_id == 0 || info->length == 0 ||
        info->user_base > UINT64_MAX - info->length || info->segment_context == NULL ||
        info->segment_context_size < sizeof(urma_seg_t) ||
        info->segment_context_size > SPDK_NVME_UB_OOB_MAX_CONTEXT_SIZE) {
        return -EINVAL;
    }

    segment = info->segment_context;
    if (segment->len < info->length || segment->ubva.va > UINT64_MAX - info->length) {
        return -ERANGE;
    }

    uctrlr = nvme_ub_ctrlr(ctrlr);
    pthread_mutex_lock(&uctrlr->npu_lock);
    if (uctrlr->current_io_queues != 0) {
        rc = -EBUSY;
        goto out;
    }
    if (nvme_ub_find_npu_endpoint(uctrlr, info->endpoint_id) == NULL) {
        rc = -ENOENT;
        goto out;
    }
    TAILQ_FOREACH(other, &uctrlr->npu_regions, link) {
        region_count++;
        if (nvme_ub_ranges_overlap(info->user_base, info->length,
                                   other->user_base, other->length)) {
            rc = -EADDRINUSE;
            goto out;
        }
    }
    if (region_count >= SPDK_NVME_UB_OOB_MAX_REGIONS) {
        rc = -ENOSPC;
        goto out;
    }

    region = calloc(1, sizeof(*region));
    if (region == NULL) {
        rc = -ENOMEM;
        goto out;
    }
    region->segment_context = malloc(info->segment_context_size);
    if (region->segment_context == NULL) {
        free(region);
        rc = -ENOMEM;
        goto out;
    }

    candidate = uctrlr->next_region_id;
    do {
        candidate++;
        if (candidate == 0) {
            candidate++;
        }
    } while (nvme_ub_find_npu_region_by_id(uctrlr, candidate) != NULL);
    uctrlr->next_region_id = candidate;

    region->region_id = candidate;
    region->endpoint_id = info->endpoint_id;
    region->token = info->token;
    region->user_base = info->user_base;
    region->remote_base = segment->ubva.va;
    region->length = info->length;
    region->segment_context_size = info->segment_context_size;
    memcpy(region->segment_context, info->segment_context, info->segment_context_size);
    TAILQ_INSERT_TAIL(&uctrlr->npu_regions, region, link);
    uctrlr->npu_registry_generation++;
    *region_id = candidate;

out:
    pthread_mutex_unlock(&uctrlr->npu_lock);
    return rc;
}

int
spdk_nvme_ub_unregister_npu_region(struct spdk_nvme_ctrlr *ctrlr, uint32_t region_id)
{
    struct nvme_ub_npu_region *region;
    struct nvme_ub_ctrlr *uctrlr;
    int rc = 0;

    if (ctrlr == NULL || ctrlr->trid.trtype != SPDK_NVME_TRANSPORT_UB) {
        return -ENOTSUP;
    }

    uctrlr = nvme_ub_ctrlr(ctrlr);
    pthread_mutex_lock(&uctrlr->npu_lock);
    if (uctrlr->current_io_queues != 0) {
        rc = -EBUSY;
        goto out;
    }
    region = nvme_ub_find_npu_region_by_id(uctrlr, region_id);
    if (region == NULL) {
        rc = -ENOENT;
        goto out;
    }

    TAILQ_REMOVE(&uctrlr->npu_regions, region, link);
    free(region->segment_context);
    free(region);
    uctrlr->npu_registry_generation++;

out:
    pthread_mutex_unlock(&uctrlr->npu_lock);
    return rc;
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
    ub_req->req = NULL;
    ub_req->data_buffer = NULL;
    ub_req->data_len = 0;
    ub_req->data_tseg = NULL;
    ub_req->addr = NULL;
    ub_req->payload_staged = false;
    ub_req->num_sge = 0;
    ub_req->command_capsule_len = sizeof(struct spdk_nvme_cmd);
    TAILQ_INSERT_HEAD(&uqpair->free_reqs, ub_req, link);
}

static inline void
nvme_ub_req_complete(struct nvme_ub_request *ub_req, struct spdk_nvme_cpl *cpl, bool print_on_error)
{
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
}

static inline void
nvme_ub_add_req(struct nvme_ub_qpair *uqpair, struct nvme_ub_request *ub_req)
{
    TAILQ_INSERT_TAIL(&uqpair->outstanding_reqs, ub_req, link);
    uqpair->outstanding_requests++;
}

static inline void
nvme_ub_remove_req(struct nvme_ub_qpair *uqpair, struct nvme_ub_request *ub_req)
{
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

static int
nvme_ub_mem_map_notify(void *cb_ctx, struct spdk_mem_map *map,
                       enum spdk_mem_map_notify_action action, void *vaddr, size_t size)
{
    struct nvme_ub_ctrlr *uctrlr = cb_ctx;
    urma_target_seg_t *tseg;
    int rc;

    switch (action) {
    case SPDK_MEM_MAP_NOTIFY_REGISTER: {
        urma_reg_seg_flag_t flag = {
            .bs.token_policy = URMA_TOKEN_NONE,
            .bs.cacheable = URMA_NON_CACHEABLE,
            .bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC,
            .bs.token_id_valid = 0,
            .bs.reserved = 0,
        };
        urma_seg_cfg_t seg_cfg = {
            .va = (uint64_t)(uintptr_t)vaddr,
            .len = size,
            .token_id = NULL,
            .token_value = uctrlr->token,
            .flag = flag,
            .user_ctx = 0,
            .iova = 0,
        };

        tseg = urma_register_seg(uctrlr->urma_ctx, &seg_cfg);
        if (tseg == NULL) {
            SPDK_WARNLOG("Unable to register application memory %p/%zu with URMA\n",
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
            SPDK_WARNLOG("Unable to unregister application memory %p/%zu from URMA\n",
                         vaddr, size);
        }
        return spdk_mem_map_clear_translation(map, (uint64_t)(uintptr_t)vaddr, size);
    default:
        SPDK_UNREACHABLE();
    }

    return -EINVAL;
}

static int
nvme_ub_mem_map_are_contiguous(uint64_t addr_1, uint64_t addr_2)
{
    return addr_1 == addr_2;
}

static const struct spdk_mem_map_ops g_nvme_ub_mem_map_ops = {
    .notify_cb = nvme_ub_mem_map_notify,
    .are_contiguous = nvme_ub_mem_map_are_contiguous,
};

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

static void
nvme_ub_free_npu_registry(struct nvme_ub_ctrlr *uctrlr)
{
    struct nvme_ub_npu_endpoint *endpoint, *endpoint_tmp;
    struct nvme_ub_npu_region *region, *region_tmp;

    if (!uctrlr->npu_registry_initialized) {
        return;
    }

    TAILQ_FOREACH_SAFE(region, &uctrlr->npu_regions, link, region_tmp) {
        TAILQ_REMOVE(&uctrlr->npu_regions, region, link);
        free(region->segment_context);
        free(region);
    }
    TAILQ_FOREACH_SAFE(endpoint, &uctrlr->npu_endpoints, link, endpoint_tmp) {
        TAILQ_REMOVE(&uctrlr->npu_endpoints, endpoint, link);
        free(endpoint->rjetty_context);
        free(endpoint);
    }
    pthread_mutex_destroy(&uctrlr->npu_lock);
    uctrlr->npu_registry_initialized = false;
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
    if (uctrlr->mem_map) {
        spdk_mem_map_free(&uctrlr->mem_map);
    }
    if (uctrlr->urma_ctx) {
        urma_delete_context(uctrlr->urma_ctx);
    }
    nvme_ub_free_npu_registry(uctrlr);
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
    return SPDK_NVME_UB_MAX_SGL_DESCRIPTORS;
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
    uqpair->qid = qid;
    uqpair->send_signal_interval = spdk_min((uint32_t)NVME_UB_MAX_SEND_SIGNAL_INTERVAL,
                                            spdk_max(1u, (uint32_t)uqpair->num_entries / 4));
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
    jfs_cfg.flag.bs.multi_path = uctrlr->multi_path;
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
            (uint64_t)uqpair->num_entries * sizeof(struct spdk_nvmf_ub_cmd), PAGE_SIZE);
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

    /* Add to controller's qpair list */
    TAILQ_INSERT_TAIL(&uctrlr->qpairs, uqpair, ctrlr_link);
    uctrlr->current_io_queues++;

    NVME_UQPAIR_INFOLOG(uqpair,
                       "Created IO qpair: queue_size=%u, JFC depth=%u, recv_depth=%u, jetty_id=%u\n",
                       opts->io_queue_size, jfc_depth, uqpair->recv_depth,
                       uqpair->jetty_id.id);
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

    NVME_UQPAIR_DEBUGLOG(uqpair,
                          "payload stats: direct_ios=%" PRIu64 " direct_bytes=%" PRIu64
                          " staged_ios=%" PRIu64 " staged_bytes=%" PRIu64
                          " send_wrs=%" PRIu64 " send_cqes=%" PRIu64
                          " signal_interval=%u\n",
                          uqpair->direct_payload_ios, uqpair->direct_payload_bytes,
                          uqpair->staged_payload_ios, uqpair->staged_payload_bytes,
                          uqpair->command_send_wrs, uqpair->command_send_cqes,
                          uqpair->send_signal_interval);

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
    ub_req->payload_staged = false;
}

static inline urma_target_seg_t *
nvme_ub_get_registered_tseg(struct nvme_ub_qpair *uqpair, void *addr, size_t length)
{
    struct spdk_mem_map *map = uqpair->uctrlr->mem_map;
    urma_target_seg_t *tseg;
    uint64_t translated_length = length;

    if (map == NULL || addr == NULL || length == 0) {
        return NULL;
    }

    tseg = (urma_target_seg_t *)(uintptr_t)
           spdk_mem_map_translate(map, (uint64_t)(uintptr_t)addr, &translated_length);
    if (tseg == NULL || translated_length < length) {
        return NULL;
    }

    return tseg;
}

static int
nvme_ub_translate_npu_address(struct nvme_ub_ctrlr *uctrlr, uint64_t user_addr,
                              uint32_t length, uint32_t *region_id, uint64_t *remote_addr)
{
    struct nvme_ub_npu_region *region;
    int rc = -ENOENT;

    pthread_mutex_lock(&uctrlr->npu_lock);
    TAILQ_FOREACH(region, &uctrlr->npu_regions, link) {
        if (user_addr < region->user_base || user_addr - region->user_base >= region->length) {
            continue;
        }
        if (!spdk_nvme_ub_range_contains(region->user_base, region->length,
                                         user_addr, length)) {
            rc = -ERANGE;
            break;
        }

        *region_id = region->region_id;
        *remote_addr = region->remote_base + (user_addr - region->user_base);
        rc = 0;
        break;
    }
    pthread_mutex_unlock(&uctrlr->npu_lock);

    return rc;
}

static void
nvme_ub_configure_npu_sgl(struct nvme_ub_request *ub_req, struct nvme_request *req,
                          void *payload_buffer, uint32_t region_id, uint64_t remote_addr)
{
    req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
    req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_VENDOR_SPECIFIC;
    req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_UB_SGL_SUBTYPE_NPU;
    req->cmd.dptr.sgl1.keyed.length = (uint32_t)req->payload_size;
    req->cmd.dptr.sgl1.keyed.key = region_id;
    req->cmd.dptr.sgl1.address = remote_addr;

    ub_req->data_buffer = payload_buffer;
    ub_req->data_len = req->payload_size;
    ub_req->data_tseg = NULL;
    ub_req->payload_staged = false;
}

static inline void
nvme_ub_configure_keyed_sgl(struct nvme_ub_request *ub_req, struct nvme_request *req,
                            void *payload_buffer, urma_target_seg_t *payload_tseg,
                            bool payload_staged)
{
    req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
    req->cmd.dptr.sgl1.keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
    req->cmd.dptr.sgl1.keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
    req->cmd.dptr.sgl1.keyed.length = (uint32_t)req->payload_size;
    req->cmd.dptr.sgl1.keyed.key = payload_tseg->seg.token_id;
    req->cmd.dptr.sgl1.address = (uint64_t)(uintptr_t)payload_buffer;

    ub_req->data_buffer = payload_buffer;
    ub_req->data_len = req->payload_size;
    ub_req->data_tseg = payload_tseg;
    ub_req->payload_staged = payload_staged;
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
    urma_target_seg_t *payload_tseg;
    void *payload_buffer;
    uint64_t remote_addr;
    uint32_t region_id;
    int rc;

    assert(req->payload_size != 0);
    assert(req->payload_size <= NVME_UB_MAX_KEYED_SGL_LENGTH);

    if (spdk_unlikely(req->payload_size > MAX_IO_SIZE || send_tseg == NULL)) {
        NVME_UQPAIR_ERRLOG(uqpair, "invalid payload size %u or send segment\n", req->payload_size);
        return -EINVAL;
    }

    payload_buffer = (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset;
    rc = nvme_ub_translate_npu_address(uqpair->uctrlr,
                                      (uint64_t)(uintptr_t)payload_buffer,
                                      req->payload_size, &region_id, &remote_addr);
    if (rc == 0) {
        nvme_ub_configure_npu_sgl(ub_req, req, payload_buffer, region_id, remote_addr);
        ub_req->num_sge = 1;
        return 0;
    }
    if (rc == -ERANGE) {
        NVME_UQPAIR_ERRLOG(uqpair, "NPU payload crosses a registered region boundary\n");
        return rc;
    }

    payload_tseg = nvme_ub_get_registered_tseg(uqpair, payload_buffer, req->payload_size);
    if (payload_tseg != NULL) {
        nvme_ub_configure_keyed_sgl(ub_req, req, payload_buffer, payload_tseg, false);
    } else {
        payload_buffer = nvme_ub_qpair_payload_buffer(uqpair, req->cmd.cid);
        if (spdk_unlikely((uint8_t *)payload_buffer + req->payload_size >
                          (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
                          nvme_ub_qpair_send_buffer_size(uqpair))) {
            NVME_UQPAIR_ERRLOG(uqpair, "payload buffer exceeds registered segment\n");
            return -EINVAL;
        }

        if (spdk_nvme_opc_get_data_transfer(req->cmd.opc) !=
            SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
            memcpy(payload_buffer, (uint8_t *)req->payload.contig_or_cb_arg + req->payload_offset,
                   req->payload_size);
        }
        nvme_ub_configure_keyed_sgl(ub_req, req, payload_buffer, send_tseg, true);
    }

    /* Record num_sge for this request */
    ub_req->num_sge = 1;

    return 0;
}

static int
nvme_ub_stage_sgl_request(struct nvme_ub_qpair *uqpair,
                          struct nvme_ub_request *ub_req,
                          struct nvme_request *req)
{
    urma_target_seg_t *send_tseg = nvme_ub_qpair_send_tseg(uqpair);
    uint8_t *payload_buffer;
    uint32_t remaining_size;
    uint32_t sge_length;
    uint32_t copied = 0;
    int num_sge = 0;
    int rc;

    payload_buffer = nvme_ub_qpair_payload_buffer(uqpair, req->cmd.cid);
    if (spdk_unlikely(payload_buffer + req->payload_size >
                      (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
                      nvme_ub_qpair_send_buffer_size(uqpair))) {
        NVME_UQPAIR_ERRLOG(uqpair, "payload buffer exceeds registered segment\n");
        return -EINVAL;
    }

    req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);

    remaining_size = req->payload_size;

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

        if (spdk_nvme_opc_get_data_transfer(req->cmd.opc) !=
            SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
            memcpy(payload_buffer + copied, addr, sge_length);
        }
        copied += sge_length;
        remaining_size -= sge_length;
        num_sge++;
    } while (remaining_size > 0);

    nvme_ub_configure_keyed_sgl(ub_req, req, payload_buffer, send_tseg, true);
    ub_req->num_sge = 1;
    return 0;
}

/* Build a command capsule containing one keyed/vendor descriptor per payload
 * SGE, matching the NVMe/RDMA multi-SGL representation. */
static inline int
nvme_ub_configure_sgl_request(struct nvme_ub_qpair *uqpair,
                              struct nvme_ub_request *ub_req,
                              struct nvme_request *req)
{
    uint32_t remaining_size = req->payload_size;
    uint32_t num_sge = 0;
    bool needs_staging = false;
    bool has_npu_sge = false;
    int rc;

    assert(req->payload_size != 0);
    assert(req->payload.reset_sgl_fn != NULL);
    assert(req->payload.next_sge_fn != NULL);

    if (spdk_unlikely(req->payload_size > MAX_IO_SIZE ||
                      nvme_ub_qpair_send_tseg(uqpair) == NULL)) {
        NVME_UQPAIR_ERRLOG(uqpair, "invalid payload size %u or send segment\n",
                           req->payload_size);
        return -EINVAL;
    }

    req->payload.reset_sgl_fn(req->payload.contig_or_cb_arg, req->payload_offset);
    while (remaining_size > 0) {
        struct spdk_nvme_sgl_descriptor *desc;
        urma_target_seg_t *payload_tseg;
        uint64_t remote_addr;
        uint32_t region_id;
        uint32_t sge_length;
        void *addr;

        if (spdk_unlikely(num_sge == SPDK_NVME_UB_MAX_SGL_DESCRIPTORS)) {
            NVME_UQPAIR_ERRLOG(uqpair, "payload requires more than %u SGL descriptors\n",
                               SPDK_NVME_UB_MAX_SGL_DESCRIPTORS);
            return -E2BIG;
        }

        rc = req->payload.next_sge_fn(req->payload.contig_or_cb_arg, &addr, &sge_length);
        if (spdk_unlikely(rc != 0 || sge_length == 0)) {
            NVME_UQPAIR_ERRLOG(uqpair, "failed to get payload SGE %u\n", num_sge);
            return rc != 0 ? rc : -EINVAL;
        }

        sge_length = spdk_min(remaining_size, sge_length);
        if (spdk_unlikely(sge_length > NVME_UB_MAX_KEYED_SGL_LENGTH)) {
            return -E2BIG;
        }

        desc = &ub_req->sgl[num_sge];
        memset(desc, 0, sizeof(*desc));
        rc = nvme_ub_translate_npu_address(uqpair->uctrlr,
                                           (uint64_t)(uintptr_t)addr,
                                           sge_length, &region_id, &remote_addr);
        if (rc == 0) {
            desc->keyed.type = SPDK_NVME_SGL_TYPE_VENDOR_SPECIFIC;
            desc->keyed.subtype = SPDK_NVME_UB_SGL_SUBTYPE_NPU;
            desc->keyed.length = sge_length;
            desc->keyed.key = region_id;
            desc->address = remote_addr;
            has_npu_sge = true;
        } else if (rc == -ERANGE) {
            NVME_UQPAIR_ERRLOG(uqpair, "NPU SGE %u crosses a registered region boundary\n",
                               num_sge);
            return rc;
        } else {
            payload_tseg = nvme_ub_get_registered_tseg(uqpair, addr, sge_length);
            if (payload_tseg == NULL) {
                needs_staging = true;
            } else {
                desc->keyed.type = SPDK_NVME_SGL_TYPE_KEYED_DATA_BLOCK;
                desc->keyed.subtype = SPDK_NVME_SGL_SUBTYPE_ADDRESS;
                desc->keyed.length = sge_length;
                desc->keyed.key = payload_tseg->seg.token_id;
                desc->address = (uint64_t)(uintptr_t)addr;
            }
        }

        if (num_sge == 0) {
            ub_req->addr = addr;
        }
        remaining_size -= sge_length;
        num_sge++;
    }

    if (needs_staging) {
        if (has_npu_sge) {
            NVME_UQPAIR_ERRLOG(uqpair,
                               "cannot stage a payload containing NPU HBM and unregistered CPU SGEs\n");
            return -ENOTSUP;
        }
        return nvme_ub_stage_sgl_request(uqpair, ub_req, req);
    }

    req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_CONTIG;
    ub_req->num_sge = num_sge;
    ub_req->data_len = req->payload_size;
    ub_req->payload_staged = false;

    if (num_sge == 1) {
        memcpy(&req->cmd.dptr.sgl1, &ub_req->sgl[0], sizeof(req->cmd.dptr.sgl1));
        ub_req->command_capsule_len = sizeof(struct spdk_nvme_cmd);
    } else {
        memset(&req->cmd.dptr.sgl1, 0, sizeof(req->cmd.dptr.sgl1));
        req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
        req->cmd.dptr.sgl1.unkeyed.subtype = SPDK_NVME_SGL_SUBTYPE_OFFSET;
        req->cmd.dptr.sgl1.unkeyed.length = num_sge * sizeof(ub_req->sgl[0]);
        req->cmd.dptr.sgl1.address = 0;
        ub_req->command_capsule_len = sizeof(struct spdk_nvme_cmd) +
                                      req->cmd.dptr.sgl1.unkeyed.length;
    }

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

    ub_req->num_sge = 0;
    ub_req->command_capsule_len = sizeof(struct spdk_nvme_cmd);
    ub_req->payload_staged = false;
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

    if (req->payload_size == 0 || !ub_req->payload_staged ||
        spdk_nvme_opc_get_data_transfer(req->cmd.opc) != SPDK_NVME_DATA_CONTROLLER_TO_HOST) {
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
        return 1;
    }

    uqpair = (struct nvme_ub_qpair *)(uintptr_t)cr->user_ctx;
    if (spdk_unlikely(uqpair == NULL || uqpair != poll_uqpair)) {
        NVME_UQPAIR_ERRLOG(poll_uqpair, "invalid SEND completion context %p\n",
                           (void *)(uintptr_t)cr->user_ctx);
        return -EPROTO;
    }

    if (spdk_unlikely(cr->opcode != URMA_CR_OPC_SEND)) {
        NVME_UQPAIR_WARNLOG(uqpair, "unexpected initiator completion opcode %d\n", cr->opcode);
        return 0;
    }

    uqpair->command_send_cqes++;

    return 0;
}

static int
nvme_ub_qpair_submit_request(struct spdk_nvme_qpair *qpair, volatile struct nvme_request *req)
{
    struct nvme_ub_qpair *uqpair = nvme_ub_qpair(qpair);
    struct nvme_ub_request *ub_req;
    urma_jfs_wr_t *bad_wr = NULL;
    bool send_signaled;
    int rc;

    assert(uqpair != NULL);
    assert(req != NULL);

    if (uqpair->state != NVME_UB_JETTY_STATE_READY) {
        return -EAGAIN;
    }

    if (uqpair->tjetty == NULL) {
        NVME_UQPAIR_ERRLOG(uqpair, "tjetty is NULL, not connected\n");
        return -EAGAIN;
    }

    /* Get a free request structure - 参考 nvme_rdma_qpair_submit_request */
    ub_req = nvme_ub_req_get(uqpair);
    if (spdk_unlikely(!ub_req)) {
        return -EAGAIN;
    }

    ub_req->uqpair = uqpair;
    ub_req->req = (struct nvme_request *)req;
    req->cmd.cid = ub_req->id;

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

    urma_opcode_t urma_opc = URMA_OPC_SEND;

    /* Build the URMA work request based on opcode - 参考 urma_sample.c */
    send_signaled = uqpair->qid == 0 ||
                    uqpair->sends_since_signal + 1 >= uqpair->send_signal_interval;
    ub_req->send_wr.opcode = urma_opc;
    ub_req->send_wr.flag.bs.complete_enable = send_signaled;
    ub_req->send_wr.flag.bs.inline_flag = 0;
    ub_req->send_wr.tjetty = uqpair->tjetty;
    ub_req->send_wr.user_ctx = (uint64_t)(uintptr_t)uqpair;
    ub_req->send_wr.next = NULL;

    /* Each I/O qpair owns its registered command/payload staging buffer. */
    void *send_buf;
    urma_target_seg_t *send_seg;
    send_buf = (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
               sizeof(struct spdk_nvmf_ub_cmd) * req->cmd.cid;
    send_seg = nvme_ub_qpair_send_tseg(uqpair);

    if (spdk_unlikely(send_seg == NULL ||
                      ub_req->command_capsule_len > sizeof(struct spdk_nvmf_ub_cmd) ||
                      (uint8_t *)send_buf + ub_req->command_capsule_len >
                      (uint8_t *)nvme_ub_qpair_send_buffer(uqpair) +
                      nvme_ub_qpair_send_buffer_size(uqpair))) {
        NVME_UQPAIR_ERRLOG(uqpair, "command buffer exceeds registered segment\n");
        nvme_ub_remove_req(uqpair, ub_req);
        nvme_ub_req_put(uqpair, ub_req);
        return -EINVAL;
    }

    /* Copy cmd to send_buffer */
    memcpy(send_buf, &req->cmd, sizeof(struct spdk_nvme_cmd));
    if (ub_req->num_sge > 1) {
        memcpy((uint8_t *)send_buf + sizeof(struct spdk_nvme_cmd), ub_req->sgl,
               ub_req->num_sge * sizeof(ub_req->sgl[0]));
    }
    /* The payload, when present, is referenced by the keyed SGL in the command. */
    ub_req->sge[0].addr = (uint64_t)send_buf;
    ub_req->sge[0].len = ub_req->command_capsule_len;
    ub_req->sge[0].tseg = send_seg;
    urma_sg_t src_sg;
    src_sg.sge = &ub_req->sge[0];
    src_sg.num_sge = 1;
    urma_send_wr_t send_wr = {
        .src = src_sg,
        .tseg = send_seg
    };
    ub_req->send_wr.send = send_wr;

    /* Post the work request - 参考 _nvme_rdma_qpair_submit_request */
    rc = urma_post_jetty_send_wr(uqpair->jetty, &ub_req->send_wr, &bad_wr);
    if (spdk_unlikely(rc != URMA_SUCCESS)) {
        NVME_UQPAIR_ERRLOG(uqpair, "urma_post_jetty_send_wr failed, rc=%d\n", rc);
        nvme_ub_remove_req(uqpair, ub_req);
        nvme_ub_req_put(uqpair, ub_req);
        return -EIO;
    }

    if (req->payload_size != 0) {
        if (ub_req->payload_staged) {
            uqpair->staged_payload_ios++;
            uqpair->staged_payload_bytes += req->payload_size;
        } else {
            uqpair->direct_payload_ios++;
            uqpair->direct_payload_bytes += req->payload_size;
        }
    }

    uqpair->command_send_wrs++;
    if (send_signaled) {
        uqpair->sends_since_signal = 0;
    } else {
        uqpair->sends_since_signal++;
    }

    return 0;
}

/* UB qpair failure handler - similar to nvme_rdma_fail_qpair */
static void
nvme_ub_fail_qpair(struct spdk_nvme_qpair *qpair)
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

    if (uqpair->jetty) {
        urma_delete_jetty(uqpair->jetty);
        uqpair->jetty = NULL;
        if (uqpair->qid == 0) {
            uqpair->uctrlr->admin_jetty = NULL;
        }
    }

    if (uqpair->jfr) {
        urma_delete_jfr(uqpair->jfr);
        uqpair->jfr = NULL;
    }

    /* Mark as not connected */
    uqpair->is_connected = false;

    NVME_UQPAIR_INFOLOG(uqpair, "UB qpair disconnected\n");

    /* Notify upper layer that disconnect is complete */
    nvme_transport_ctrlr_disconnect_qpair_done(qpair);
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
        }
        rc = -EAGAIN;
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
    urma_cr_t crs[NVME_UB_COMPLETION_BATCH_SIZE];
    int32_t total_completions = 0;
    uint32_t num_polled = 0;
    int rc = 0;

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
        if (rc != 0 && rc != -EAGAIN) {
            NVME_UQPAIR_ERRLOG(uqpair, "Connect poll failed, rc=%d\n", rc);
            goto failed;
        }
        break;

    case NVME_QPAIR_DISCONNECTING:
        return -ENXIO;

    default:
        break;
    }

    /* Poll the JFC in batches to amortize the SDK call overhead. */
    while (num_polled < max_completions) {
        uint32_t batch_size = spdk_min(max_completions - num_polled,
                                       (uint32_t)NVME_UB_COMPLETION_BATCH_SIZE);
        uint32_t i;
        int cnt;

        cnt = urma_poll_jfc(uqpair->jfc, batch_size, crs);
        if (cnt <= 0) {
            break;
        }

        num_polled += cnt;
        for (i = 0; i < (uint32_t)cnt; i++) {
            rc = nvme_ub_process_cr(uqpair, &crs[i]);
            if (spdk_unlikely(rc < 0)) {
                goto failed;
            }
            total_completions += rc;
        }
    }

    return total_completions;

failed:
    nvme_ub_fail_qpair(qpair);
    return -ENXIO;
}


static int
nvme_ub_sock_write_all(struct spdk_sock *sock, const void *buf, size_t length)
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
nvme_ub_sock_read_all(struct spdk_sock *sock, void *buf, size_t length)
{
    uint64_t deadline = spdk_get_ticks() + 5 * spdk_get_ticks_hz();
    size_t offset = 0;

    while (offset < length) {
        ssize_t rc = spdk_sock_recv(sock, (uint8_t *)buf + offset, length - offset);

        if (rc > 0) {
            offset += rc;
            continue;
        }
        if (rc == 0) {
            return -ECONNRESET;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
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
nvme_ub_build_connect_request(struct nvme_ub_qpair *uqpair, urma_target_seg_t *send_tseg,
                              void **request_buf, size_t *request_len, uint64_t *generation)
{
    struct nvme_ub_ctrlr *uctrlr = uqpair->uctrlr;
    struct nvme_ub_npu_endpoint *endpoint;
    struct nvme_ub_npu_region *region;
    struct spdk_nvme_ub_oob_header *header;
    struct spdk_nvme_ub_oob_cpu_info *cpu;
    uint32_t endpoint_count = 0, region_count = 0;
    uint64_t total = sizeof(*header) + sizeof(*cpu);
    uint8_t *cursor;

    pthread_mutex_lock(&uctrlr->npu_lock);
    if (uqpair->qid != 0) {
        TAILQ_FOREACH(endpoint, &uctrlr->npu_endpoints, link) {
            endpoint_count++;
            total += sizeof(struct spdk_nvme_ub_oob_endpoint) + endpoint->rjetty_context_size;
        }
        TAILQ_FOREACH(region, &uctrlr->npu_regions, link) {
            region_count++;
            total += sizeof(struct spdk_nvme_ub_oob_region) + region->segment_context_size;
        }
    }

    if (endpoint_count > SPDK_NVME_UB_OOB_MAX_ENDPOINTS ||
        region_count > SPDK_NVME_UB_OOB_MAX_REGIONS ||
        total > SPDK_NVME_UB_OOB_MAX_SIZE) {
        pthread_mutex_unlock(&uctrlr->npu_lock);
        return -E2BIG;
    }

    header = calloc(1, (size_t)total);
    if (header == NULL) {
        pthread_mutex_unlock(&uctrlr->npu_lock);
        return -ENOMEM;
    }

    header->magic = SPDK_NVME_UB_OOB_MAGIC;
    header->version = SPDK_NVME_UB_OOB_VERSION;
    header->msg_type = SPDK_NVME_UB_OOB_CONNECT;
    header->length = (uint32_t)total;
    header->qid = uqpair->qid;
    header->endpoint_count = endpoint_count;
    header->region_count = region_count;
    header->registry_generation = uctrlr->npu_registry_generation;

    cpu = (struct spdk_nvme_ub_oob_cpu_info *)(header + 1);
    memcpy(cpu->seg_eid, send_tseg->seg.ubva.eid.raw, SPDK_NVME_UB_EID_SIZE);
    cpu->seg_uasid = send_tseg->seg.ubva.uasid;
    cpu->seg_va = send_tseg->seg.ubva.va;
    cpu->seg_len = send_tseg->seg.len;
    cpu->seg_flag = send_tseg->seg.attr.value;
    cpu->seg_token_id = send_tseg->seg.token_id;
    memcpy(cpu->jetty_eid, uqpair->jetty_id.eid.raw, SPDK_NVME_UB_EID_SIZE);
    cpu->jetty_uasid = uqpair->jetty_id.uasid;
    cpu->jetty_id = uqpair->jetty_id.id;
    cpu->trans_mode = URMA_TM_RM;

    cursor = (uint8_t *)(cpu + 1);
    if (uqpair->qid != 0) {
        TAILQ_FOREACH(endpoint, &uctrlr->npu_endpoints, link) {
            struct spdk_nvme_ub_oob_endpoint *record = (void *)cursor;

            record->record_size = sizeof(*record) + endpoint->rjetty_context_size;
            record->endpoint_id = endpoint->endpoint_id;
            record->token = endpoint->token;
            record->rjetty_context_size = endpoint->rjetty_context_size;
            memcpy(record + 1, endpoint->rjetty_context, endpoint->rjetty_context_size);
            cursor += record->record_size;
        }
        TAILQ_FOREACH(region, &uctrlr->npu_regions, link) {
            struct spdk_nvme_ub_oob_region *record = (void *)cursor;

            record->record_size = sizeof(*record) + region->segment_context_size;
            record->region_id = region->region_id;
            record->endpoint_id = region->endpoint_id;
            record->token = region->token;
            record->user_base = region->user_base;
            record->remote_base = region->remote_base;
            record->length = region->length;
            record->segment_context_size = region->segment_context_size;
            memcpy(record + 1, region->segment_context, region->segment_context_size);
            cursor += record->record_size;
        }
    }
    pthread_mutex_unlock(&uctrlr->npu_lock);

    *request_buf = header;
    *request_len = (size_t)total;
    *generation = header->registry_generation;
    return 0;
}

static int
nvme_ub_connect_established(struct nvme_ub_qpair *uqpair)
{
    struct nvme_ub_ctrlr *uctrlr = uqpair->uctrlr;
    struct spdk_nvme_qpair *qpair = &uqpair->qpair;
    urma_target_seg_t *send_tseg = nvme_ub_qpair_send_tseg(uqpair);
    urma_target_seg_t *recv_tseg = nvme_ub_qpair_recv_tseg(uqpair);
    struct spdk_nvme_ub_oob_header response = {};
    struct spdk_nvme_ub_oob_cpu_info remote_info = {};
    urma_rjetty_t remote_jetty = {};
    void *request = NULL;
    size_t request_len = 0;
    uint64_t registry_generation;
    int rc;

    if (uqpair->sock == NULL || send_tseg == NULL || recv_tseg == NULL || uqpair->jfc == NULL ||
        uqpair->recv_ctxs == NULL || uqpair->recv_depth == 0) {
        NVME_UQPAIR_ERRLOG(uqpair, "Incomplete qpair resources\n");
        return -EINVAL;
    }

    rc = nvme_ub_build_connect_request(uqpair, send_tseg, &request, &request_len,
                                       &registry_generation);
    if (rc != 0) {
        return rc;
    }
    rc = nvme_ub_sock_write_all(uqpair->sock, request, request_len);
    free(request);
    if (rc != 0) {
        NVME_UQPAIR_ERRLOG(uqpair, "Failed to send OOB v1 connect request: %s\n",
                          spdk_strerror(-rc));
        return rc;
    }

    rc = nvme_ub_sock_read_all(uqpair->sock, &response, sizeof(response));
    if (rc == 0) {
        if (response.magic != SPDK_NVME_UB_OOB_MAGIC ||
            response.version != SPDK_NVME_UB_OOB_VERSION ||
            response.msg_type != SPDK_NVME_UB_OOB_CONNECT_RSP ||
            response.length != sizeof(response) + sizeof(remote_info) ||
            response.qid != uqpair->qid ||
            response.endpoint_count != 0 || response.region_count != 0 ||
            response.registry_generation != registry_generation) {
            rc = -EPROTO;
        } else if (response.status != 0) {
            rc = response.status;
        }
    }
    if (rc == 0) {
        rc = nvme_ub_sock_read_all(uqpair->sock, &remote_info, sizeof(remote_info));
    }
    if (rc != 0) {
        NVME_UQPAIR_ERRLOG(uqpair, "Invalid OOB v1 connect response: %s\n",
                          spdk_strerror(-rc));
        return rc;
    }

    memcpy(remote_jetty.jetty_id.eid.raw, remote_info.jetty_eid,
           SPDK_NVME_UB_EID_SIZE);
    remote_jetty.jetty_id.uasid = remote_info.jetty_uasid;
    remote_jetty.jetty_id.id = remote_info.jetty_id;
    remote_jetty.trans_mode = URMA_TM_RM;
    remote_jetty.type = URMA_JETTY;
    remote_jetty.tp_type = URMA_CTP;

    spdk_sock_close(&uqpair->sock);
    uqpair->tjetty = urma_import_jetty(uctrlr->urma_ctx, &remote_jetty, &uctrlr->token);
    if (uqpair->tjetty == NULL) {
        NVME_UQPAIR_ERRLOG(uqpair,
                          "Failed to import remote jetty (multi_path=%d, errno=%d: %s)\n",
                          uctrlr->multi_path, errno, strerror(errno));
        return -EIO;
    }

    uqpair->is_connected = true;
    uqpair->state = NVME_UB_JETTY_STATE_READY;
    nvme_qpair_set_state(qpair, NVME_QPAIR_CONNECTING);

    for (uint32_t i = 0; i < uqpair->recv_depth; i++) {
        rc = nvme_ub_post_recv_wr(uqpair, i);
        if (spdk_unlikely(rc != 0)) {
            NVME_UQPAIR_ERRLOG(uqpair, "nvme_ub_post_recv_wr failed\n");
            return -EIO;
        }
    }

    NVME_UQPAIR_NOTICELOG(uqpair,
                          "OOB v1 connection established: remote_jetty_id=%u, recv_depth=%u, "
                          "generation=%" PRIu64 "\n",
                          remote_info.jetty_id, uqpair->recv_depth, registry_generation);
    return 0;
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

    NVME_UQPAIR_INFOLOG(uqpair, "Connecting UB qpair to %s:%s\n",
                       ctrlr->trid.traddr, ctrlr->trid.trsvcid);

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
                nvme_ub_fail_qpair(qpair);
            }
        }
    }

    TAILQ_FOREACH_SAFE(uqpair, &group->active_qpairs, link_active, tmp_uqpair) {
        urma_cr_t crs[NVME_UB_COMPLETION_BATCH_SIZE];
        uint32_t qpair_completions = 0;
        uint32_t num_polled = 0;
        bool qpair_failed = false;

        while (num_polled < completions_per_qpair) {
            uint32_t batch_size = spdk_min(completions_per_qpair - num_polled,
                                           (uint32_t)NVME_UB_COMPLETION_BATCH_SIZE);
            uint32_t i;
            int cnt;

            cnt = urma_poll_jfc(uqpair->jfc, batch_size, crs);
            if (cnt <= 0) {
                break;
            }

            num_polled += cnt;
            for (i = 0; i < (uint32_t)cnt; i++) {
                rc = nvme_ub_process_cr(uqpair, &crs[i]);
                if (spdk_unlikely(rc < 0)) {
                    NVME_UQPAIR_ERRLOG(uqpair, "Failed to process completion, rc=%d\n", rc);
                    nvme_ub_fail_qpair(&uqpair->qpair);
                    qpair_failed = true;
                    break;
                }
                qpair_completions += rc;
                total_completions += rc;
            }

            if (qpair_failed) {
                break;
            }
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

static spdk_nvme_ctrlr_t *
nvme_ub_ctrlr_construct(const struct spdk_nvme_transport_id *trid,
              const struct spdk_nvme_ctrlr_opts *opts,
              void *devhandle)
{
    struct nvme_ub_ctrlr *uctrlr;

    fprintf(stderr, "DEBUG: nvme_ub_ctrlr_construct called, trid trtype=%d, traddr=%s, trsvcid=%s\n",
            trid->trtype, trid->traddr, trid->trsvcid);
    urma_device_t *urma_dev = NULL;
    urma_device_attr_t dev_attr;
    int eid_index = -1;
    int rc;
    struct nvme_ub_qpair *admin_uqpair;
    urma_jfc_cfg_t jfc_cfg;
    urma_jfr_cfg_t jfr_cfg;
    urma_jfs_cfg_t jfs_cfg;
    urma_jetty_cfg_t jetty_cfg;
    urma_reg_seg_flag_t seg_flag;
    urma_seg_cfg_t seg_cfg;
    uint32_t admin_queue_size;
    uint32_t admin_jfc_depth;

    /* Initialize the process-wide URMA library instance. */
    rc = nvme_ub_urma_init();
    if (rc != URMA_SUCCESS) {
        SPDK_ERRLOG("Failed to initialize URMA library: %d (%s)\n",
                    rc, strerror(rc));
        return NULL;
    }

    uctrlr = spdk_zmalloc(sizeof(struct nvme_ub_ctrlr), 0, NULL, SPDK_ENV_NUMA_ID_ANY,
                  SPDK_MALLOC_DMA);
    if (uctrlr == NULL) {
        SPDK_ERRLOG("could not allocate ctrlr\n");
        return NULL;
    }

    /* Initialize qpairs list */
    TAILQ_INIT(&uctrlr->qpairs);

    uctrlr->ctrlr.opts = *opts;
    uctrlr->ctrlr.trid = *trid;

    NVME_CTRLR_INFOLOG(&uctrlr->ctrlr, "Initializing NVMe/UB controller for %s:%s\n",
                       trid->traddr, trid->trsvcid);

    uctrlr->max_sge = SPDK_NVME_UB_MAX_SGL_DESCRIPTORS;

    const char *dev_name = getenv(URMA_DEVICE_NAME_ENV);
    if (dev_name == NULL || dev_name[0] == '\0') {
        dev_name = URMA_DEFAULT_DEVICE_NAME;
    }

    urma_dev = urma_get_device_by_name(dev_name);
    if (urma_dev == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to get URMA device %s.\n", dev_name);
        return NULL;
    }

    rc = urma_query_device(urma_dev, &dev_attr);
    if (rc < 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to query UB device attributes.\n");
        spdk_free(uctrlr);
        return NULL;
    }
    uctrlr->max_sge = spdk_min(uctrlr->max_sge, (uint16_t)dev_attr.dev_cap.max_jfs_sge);
    uctrlr->dev_attr = dev_attr;
    uctrlr->multi_path = nvme_ub_device_uses_multipath(urma_dev->name);
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "Using device: %s, multi_path=%d\n",
                        urma_dev->name, uctrlr->multi_path);

    eid_index = nvme_ub_get_eid_index(urma_dev);
    if (eid_index < 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to determine EID index.\n");
        spdk_free(uctrlr);
        return NULL;
    }
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "Using EID index %d\n", eid_index);

    /* Create URMA context */
    NVME_CTRLR_DEBUGLOG(&uctrlr->ctrlr, "Creating URMA context with dev=%p, eid_index=%d\n",
              (void*)urma_dev, eid_index);
    uctrlr->urma_ctx = urma_create_context(urma_dev, (uint32_t)eid_index);
    if (uctrlr->urma_ctx == NULL) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "Failed to create URMA context, errno=%d.\n", errno);
        spdk_free(uctrlr);
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
        return NULL;
    }
    fprintf(stderr, "DEBUG: Admin JFR created successfully, jfr=%p\n", (void*)admin_jfr);

    /* Create admin JFS */
    memset(&jfs_cfg, 0, sizeof(jfs_cfg));
    jfs_cfg.depth = admin_queue_size;
    jfs_cfg.flag.bs.order_type = 0;
    jfs_cfg.flag.bs.multi_path = uctrlr->multi_path;
    jfs_cfg.trans_mode = URMA_TM_RM;
    jfs_cfg.priority = URMA_MAX_PRIORITY;
    jfs_cfg.max_sge = uctrlr->max_sge;
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
        return NULL;
    }
    fprintf(stderr, "DEBUG: admin_uqpair allocated at %p\n", (void*)admin_uqpair);

    admin_uqpair->uctrlr = uctrlr;
    admin_uqpair->sock = NULL;
    admin_uqpair->jfce = uctrlr->jfce;
    admin_uqpair->jfc = uctrlr->jfc;
    admin_uqpair->jfr = admin_jfr; /* Store admin JFR for cleanup */
    admin_uqpair->num_entries = admin_queue_size - 1;
    admin_uqpair->qid = 0;
    admin_uqpair->send_signal_interval = 1;
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
        return NULL;
    }

    /* Register SPDK-managed application memory once per controller.  If a
     * provider cannot register the complete memory map, retain the staging
     * path rather than failing controller construction. */
    uctrlr->mem_map = spdk_mem_map_alloc(0, &g_nvme_ub_mem_map_ops, uctrlr);
    if (uctrlr->mem_map == NULL) {
        NVME_CTRLR_WARNLOG(&uctrlr->ctrlr,
                           "Unable to create UB application memory map; using staging buffers\n");
    }

    TAILQ_INIT(&uctrlr->npu_endpoints);
    TAILQ_INIT(&uctrlr->npu_regions);
    rc = pthread_mutex_init(&uctrlr->npu_lock, NULL);
    if (rc != 0) {
        NVME_CTRLR_ERRLOG(&uctrlr->ctrlr, "failed to initialize NPU registry lock: %s\n",
                          strerror(rc));
        nvme_ub_ctrlr_destruct(&uctrlr->ctrlr);
        return NULL;
    }
    uctrlr->npu_registry_initialized = true;

    NVME_CTRLR_NOTICELOG(&uctrlr->ctrlr,
                         "NVMe/UB controller initialized: device=%s, EID index=%d, admin_queue_size=%u\n",
                         urma_dev->name, eid_index, admin_queue_size);
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
    .poll_group_free_stats = NULL,
};

SPDK_NVME_TRANSPORT_REGISTER(ub, &ub_ops);
