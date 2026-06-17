/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "nvmf/qdlimit.c"

static void
test_config_set_get(void)
{
	uint32_t depth = 12345;

	/* Unconfigured bdev: ENOENT, depth defaults to 0. */
	CU_ASSERT(nvmf_qdlimit_get_depth("bdevA", &depth) == -ENOENT);
	CU_ASSERT(depth == 0);

	/* Set then read back. */
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 8) == 0);
	CU_ASSERT(nvmf_qdlimit_get_depth("bdevA", &depth) == 0);
	CU_ASSERT(depth == 8);

	/* Update in place. */
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 0) == 0);
	CU_ASSERT(nvmf_qdlimit_get_depth("bdevA", &depth) == 0);
	CU_ASSERT(depth == 0);

	/* Bad args: set_depth. */
	CU_ASSERT(nvmf_qdlimit_set_depth(NULL, 1) == -EINVAL);
	CU_ASSERT(nvmf_qdlimit_set_depth("", 1) == -EINVAL);

	/* Bad args: get_depth. */
	CU_ASSERT(nvmf_qdlimit_get_depth(NULL, &depth) == -EINVAL);
	CU_ASSERT(nvmf_qdlimit_get_depth("bdevA", NULL) == -EINVAL);

	/* Over-long bdev name (>= 256 chars) is rejected. */
	char long_name[300];
	memset(long_name, 'x', sizeof(long_name) - 1);
	long_name[sizeof(long_name) - 1] = '\0';
	CU_ASSERT(nvmf_qdlimit_set_depth(long_name, 1) == -ENAMETOOLONG);

	nvmf_qdlimit_config_cleanup();
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("qdlimit", NULL, NULL);
	CU_ADD_TEST(suite, test_config_set_get);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
