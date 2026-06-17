/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Intel Corporation. All rights reserved.
 */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "nvmf/qdlimit.c"

/* Opaque fake bdevs: we only use their addresses as identity keys and stub get_name. */
static char g_fake_bdev_a;
static char g_fake_bdev_b;

DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev),
	    ((void *)bdev == &g_fake_bdev_a) ? "bdevA" : "bdevB");

DEFINE_STUB(_nvmf_subsystem_get_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, uint32_t nsid), NULL);

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

static void
test_pg_init_fini(void)
{
	struct spdk_nvmf_transport_poll_group group = {};

	nvmf_qdlimit_pg_init(&group);
	CU_ASSERT(group.qdlimit_ctx != NULL);
	CU_ASSERT(TAILQ_EMPTY(&((struct qdlimit_pg_ctx *)group.qdlimit_ctx)->ssds));

	nvmf_qdlimit_pg_fini(&group);
	CU_ASSERT(group.qdlimit_ctx == NULL);

	/* Idempotent: fini on a NULL-ctx group (OOM/double-fini) must not crash. */
	nvmf_qdlimit_pg_fini(&group);
	CU_ASSERT(group.qdlimit_ctx == NULL);
}

static void
test_admit_under_and_over_limit(void)
{
	struct spdk_nvmf_transport_poll_group group = {};
	struct spdk_nvmf_request r1 = {}, r2 = {}, r3 = {};

	STAILQ_INIT(&group.pending_buf_queue);
	nvmf_qdlimit_pg_init(&group);
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 2) == 0);

	/* Simulate the transport: each request is the queue head when gated. */
	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r1, buf_link);
	CU_ASSERT(qdlimit_admit_bdev(&group, &r1, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_ADMIT);
	CU_ASSERT(r1.qdlimit_charged == true);

	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r2, buf_link);
	CU_ASSERT(qdlimit_admit_bdev(&group, &r2, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_ADMIT);

	/* Third request exceeds depth=2: throttled and removed from pending_buf_queue. */
	STAILQ_INSERT_TAIL(&group.pending_buf_queue, &r3, buf_link);
	CU_ASSERT(qdlimit_admit_bdev(&group, &r3, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_THROTTLED);
	CU_ASSERT(r3.qdlimit_charged == false);
	/* r3 left pending_buf_queue; r1 and r2 remain. */
	CU_ASSERT(STAILQ_FIRST(&group.pending_buf_queue) == &r1);

	nvmf_qdlimit_pg_fini_drain(&group); /* test helper, see step 3 */
	nvmf_qdlimit_config_cleanup();
}

static void
test_admit_bypass(void)
{
	struct spdk_nvmf_transport_poll_group group = {};
	struct spdk_nvmf_request rn = {}, ru = {};

	STAILQ_INIT(&group.pending_buf_queue);
	nvmf_qdlimit_pg_init(&group);

	/* Unconfigured bdev (bdevB) => unlimited bypass, never charged. */
	CU_ASSERT(qdlimit_admit_bdev(&group, &ru, (void *)&g_fake_bdev_b, "bdevB") == NVMF_QDLIMIT_ADMIT);
	CU_ASSERT(ru.qdlimit_charged == false);

	/* depth == 0 explicit => unlimited bypass. */
	CU_ASSERT(nvmf_qdlimit_set_depth("bdevA", 0) == 0);
	CU_ASSERT(qdlimit_admit_bdev(&group, &rn, (void *)&g_fake_bdev_a, "bdevA") == NVMF_QDLIMIT_ADMIT);
	CU_ASSERT(rn.qdlimit_charged == false);

	nvmf_qdlimit_pg_fini(&group);
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
	CU_ADD_TEST(suite, test_pg_init_fini);
	CU_ADD_TEST(suite, test_admit_under_and_over_limit);
	CU_ADD_TEST(suite, test_admit_bypass);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return num_failures;
}
