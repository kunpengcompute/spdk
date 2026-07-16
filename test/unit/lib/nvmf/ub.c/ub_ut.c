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

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvmf_ub", NULL, NULL);

	CU_ADD_TEST(suite, test_nvmf_ub_opts_init);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}