/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 SPDK contributors.
 *   All rights reserved.
 */

#include "spdk_cunit.h"
#include "spdk/stdinc.h"

/* Forward-declare so the header's STAILQ_HEAD(, qdlimit_bdev_io) compiles. */
struct qdlimit_bdev_io;

#include "bdev/qdlimit/vbdev_qdlimit.h"

static void
test_acquire_release_unlimited(void)
{
	struct qdlimit_io_channel qd_ch = {};

	qd_ch.max_depth = 0; /* unlimited */
	qd_ch.outstanding = 0;

	/* Always admits when unlimited, counter still tracks in-flight. */
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);
	CU_ASSERT(qd_ch.outstanding == 1);
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);
	CU_ASSERT(qd_ch.outstanding == 2);

	qdlimit_release(&qd_ch);
	CU_ASSERT(qd_ch.outstanding == 1);
	qdlimit_release(&qd_ch);
	CU_ASSERT(qd_ch.outstanding == 0);
}

static void
test_acquire_caps_at_max_depth(void)
{
	struct qdlimit_io_channel qd_ch = {};

	qd_ch.max_depth = 2;
	qd_ch.outstanding = 0;

	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);  /* 1 */
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);  /* 2 */
	CU_ASSERT(qd_ch.outstanding == 2);

	/* At cap -> rejected, counter unchanged. */
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == false);
	CU_ASSERT(qd_ch.outstanding == 2);

	/* Free one slot -> next acquire succeeds. */
	qdlimit_release(&qd_ch);
	CU_ASSERT(qd_ch.outstanding == 1);
	CU_ASSERT(qdlimit_try_acquire(&qd_ch) == true);
	CU_ASSERT(qd_ch.outstanding == 2);
}

static void
test_io_is_limited(void)
{
	/* Data-path ops are limited. */
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_READ) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_WRITE) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_UNMAP) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_WRITE_ZEROES) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_FLUSH) == true);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_COPY) == true);

	/* Management / buffer ops bypass the limit. */
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_RESET) == false);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_ABORT) == false);
	CU_ASSERT(qdlimit_io_is_limited(SPDK_BDEV_IO_TYPE_ZCOPY) == false);
}

int
main(int argc, char **argv)
{
	CU_pSuite suite = NULL;
	unsigned int num_failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();

	suite = CU_add_suite("qdlimit", NULL, NULL);

	CU_ADD_TEST(suite, test_acquire_release_unlimited);
	CU_ADD_TEST(suite, test_acquire_caps_at_max_depth);
	CU_ADD_TEST(suite, test_io_is_limited);

	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	num_failures = CU_get_number_of_failures();
	CU_cleanup_registry();

	return num_failures;
}
