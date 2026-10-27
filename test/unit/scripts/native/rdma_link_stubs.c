/* SPDX-License-Identifier: BSD-3-Clause
 * Native macOS UT adapters only. Standard Linux SPDK builds do not use these.
 */
#include "spdk/stdinc.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/rdma.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
DEFINE_STUB(rdma_create_id, int, (struct rdma_event_channel *channel,
				  struct rdma_cm_id **id, void *context,
				  enum rdma_port_space ps), 0);
DEFINE_STUB(rdma_bind_addr, int, (struct rdma_cm_id *id, struct sockaddr *addr), 0);
DEFINE_STUB(rdma_listen, int, (struct rdma_cm_id *id, int backlog), 0);
DEFINE_STUB(rdma_destroy_id, int, (struct rdma_cm_id *id), 0);
DEFINE_STUB(rdma_reject, int, (struct rdma_cm_id *id,
			       const void *private_data, uint8_t private_data_len), 0);
DEFINE_STUB_V(rdma_destroy_qp, (struct rdma_cm_id *id));
DEFINE_STUB_V(rdma_destroy_event_channel, (struct rdma_event_channel *channel));
DEFINE_STUB(ibv_dealloc_pd, int, (struct ibv_pd *pd), 0);
DEFINE_STUB(rdma_create_event_channel, struct rdma_event_channel *, (void), NULL);
DEFINE_STUB(rdma_get_devices, struct ibv_context **, (int *num_devices), NULL);
DEFINE_STUB(ibv_query_device, int, (struct ibv_context *context,
				    struct ibv_device_attr *device_attr), 0);
DEFINE_STUB(ibv_alloc_pd, struct ibv_pd *, (struct ibv_context *context), NULL);
DEFINE_STUB_V(rdma_free_devices, (struct ibv_context **list));
DEFINE_STUB(ibv_get_async_event, int, (struct ibv_context *context, struct ibv_async_event *event),
	    0);
DEFINE_STUB(ibv_event_type_str, const char *, (enum ibv_event_type event_type), NULL);
DEFINE_STUB_V(ibv_ack_async_event, (struct ibv_async_event *event));
DEFINE_STUB(rdma_get_cm_event, int, (struct rdma_event_channel *channel,
				     struct rdma_cm_event **event), 0);
DEFINE_STUB(rdma_ack_cm_event, int, (struct rdma_cm_event *event), 0);
DEFINE_STUB(ibv_wc_status_str, const char *, (enum ibv_wc_status status), NULL);
DEFINE_STUB(rdma_get_dst_port, __be16, (struct rdma_cm_id *id), 0);
DEFINE_STUB(rdma_get_src_port, __be16, (struct rdma_cm_id *id), 0);
