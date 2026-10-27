/* SPDX-License-Identifier: BSD-3-Clause
 * Native macOS UT adapters only. Standard Linux SPDK builds do not use these.
 */
#include "spdk/stdinc.h"
#include "spdk_internal/mock.h"

DEFINE_RETURN_MOCK(calloc, void *);
void *
__wrap_calloc(size_t count, size_t size)
{
	return ut_calloc_mocked ? ut_calloc : calloc(count, size);
}

/* Darwin ld does not implement GNU --wrap. Restrict interception to this UT TU. */
#define calloc __wrap_calloc
#include DEV_L0_TEST_SOURCE
#undef calloc
