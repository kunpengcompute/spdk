/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"

#include "common/lib/test_env.c"
#include "common/lib/test_sock.c"
#include "common/lib/test_iobuf.c"

#include "spdk/sock.h"
#include "nvmf/ub.c"
#include "nvmf/transport.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

DEFINE_STUB_V(spdk_nvmf_tgt_new_qpair, (struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_qpair *qpair));

static void
test_nvmf_ub_opts_init(void)
{
	struct spdk_nvmf_transport_opts opts = {};

	nvmf_ub_opts_init(&opts);

	CU_ASSERT(opts.max_queue_depth == SPDK_NVMF_UB_DEFAULT_MAX_QUEUE_DEPTH);
	CU_ASSERT(opts.max_qpairs_per_ctrlr == SPDK_NVMF_UB_DEFAULT_MAX_QPAIRS_PER_CTRLR);
	CU_ASSERT(opts.in_capsule_data_size == SPDK_NVMF_UB_DEFAULT_IN_CAPSULE_DATA_SIZE);
	CU_ASSERT(opts.max_io_size == SPDK_NVMF_UB_DEFAULT_MAX_IO_SIZE);
	CU_ASSERT(opts.max_aq_depth == SPDK_NVMF_UB_DEFAULT_AQ_DEPTH);
	CU_ASSERT(opts.dif_insert_or_strip == SPDK_NVMF_UB_DIF_INSERT_OR_STRIP);
	CU_ASSERT(opts.abort_timeout_sec == SPDK_NVMF_UB_DEFAULT_ABORT_TIMEOUT_SEC);
	CU_ASSERT(opts.transport_specific == NULL);
	CU_ASSERT(opts.data_wr_pool_size == SPDK_NVMF_UB_DEFAULT_DATA_WR_POOL_SIZE);
}

static void
init_ub_resource_queues(struct spdk_nvmf_ub_qpair *uqpair,
			struct spdk_nvmf_ub_resources *resources,
			struct spdk_nvmf_ub_request *reqs,
			struct spdk_nvmf_ub_response *responses,
			uint32_t depth)
{
	uint32_t i;

	memset(uqpair, 0, sizeof(*uqpair));
	memset(resources, 0, sizeof(*resources));
	memset(reqs, 0, depth * sizeof(*reqs));
	memset(responses, 0, depth * sizeof(*responses));
	uqpair->resources = resources;
	resources->reqs = reqs;
	resources->responses = responses;
	resources->depth = depth;
	STAILQ_INIT(&resources->free_queue);
	STAILQ_INIT(&resources->free_response_queue);
	STAILQ_INIT(&resources->pending_response_queue);

	for (i = 0; i < depth; i++) {
		reqs[i].req.qpair = &uqpair->qpair;
		reqs[i].recv_slot = NVMF_UB_INVALID_RECV_SLOT;
		responses[i].buf_idx = i;
		STAILQ_INSERT_TAIL(&resources->free_response_queue, &responses[i], link);
	}
}

static void
test_nvmf_ub_response_pool(void)
{
	struct spdk_nvmf_ub_qpair uqpair;
	struct spdk_nvmf_ub_resources resources;
	struct spdk_nvmf_ub_request reqs[2];
	struct spdk_nvmf_ub_response responses[2];
	struct spdk_nvmf_ub_response *rsp0, *rsp1;

	init_ub_resource_queues(&uqpair, &resources, reqs, responses, SPDK_COUNTOF(reqs));

	rsp0 = nvmf_ub_response_get(&uqpair);
	rsp1 = nvmf_ub_response_get(&uqpair);
	SPDK_CU_ASSERT_FATAL(rsp0 == &responses[0]);
	SPDK_CU_ASSERT_FATAL(rsp1 == &responses[1]);
	CU_ASSERT(rsp0->in_use);
	CU_ASSERT(rsp1->in_use);
	CU_ASSERT(nvmf_ub_response_get(&uqpair) == NULL);

	nvmf_ub_response_put(&uqpair, rsp0);
	CU_ASSERT(!rsp0->in_use);
	CU_ASSERT(nvmf_ub_response_get(&uqpair) == rsp0);
}

static void
test_nvmf_ub_pending_response_queue(void)
{
	struct spdk_nvmf_ub_qpair uqpair;
	struct spdk_nvmf_ub_resources resources;
	struct spdk_nvmf_ub_request reqs[2];
	struct spdk_nvmf_ub_response responses[2];
	struct spdk_nvmf_ub_request *req;

	init_ub_resource_queues(&uqpair, &resources, reqs, responses, SPDK_COUNTOF(reqs));
	reqs[0].in_use = true;
	reqs[1].in_use = true;

	nvmf_ub_queue_pending_response(&reqs[0]);
	nvmf_ub_queue_pending_response(&reqs[1]);
	CU_ASSERT(resources.pending_response_count == 2);
	CU_ASSERT(resources.pending_response_high_watermark == 2);

	req = nvmf_ub_pending_response_get(&uqpair);
	SPDK_CU_ASSERT_FATAL(req == &reqs[0]);
	CU_ASSERT(req->rdma_state == UB_REQ_RDMA_STATE_NONE);
	CU_ASSERT(resources.pending_response_count == 1);

	/* Releasing a queued request during disconnect must unlink it first. */
	reqs[1].recv_slot = NVMF_UB_INVALID_RECV_SLOT;
	nvmf_ub_req_abort(&reqs[1]);
	CU_ASSERT(resources.pending_response_count == 0);
	CU_ASSERT(STAILQ_EMPTY(&resources.pending_response_queue));
	CU_ASSERT(!reqs[1].in_use);
	CU_ASSERT(STAILQ_FIRST(&resources.free_queue) == &reqs[1]);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvmf_ub", NULL, NULL);

	CU_ADD_TEST(suite, test_nvmf_ub_opts_init);
	CU_ADD_TEST(suite, test_nvmf_ub_response_pool);
	CU_ADD_TEST(suite, test_nvmf_ub_pending_response_queue);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
