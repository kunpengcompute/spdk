/* SPDX-License-Identifier: BSD-3-Clause */

#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "nvmf/transport.c"

SPDK_LOG_REGISTER_COMPONENT(nvmf)

/* A non-DPDK env may implement neither optional L0 symbol. Keep both truly
 * undefined, so this exercises ELF weak-symbol resolution rather than a stub
 * returning false with a non-NULL function address. */
static void
test_l0_symbols_absent(void)
{
	if (getenv("SPDK_UT_WEAK_L0_AVAILABLE") != NULL) {
		CU_ASSERT(spdk_l0_data_pool_enabled != NULL);
		CU_ASSERT(spdk_l0_mempool_create != NULL);
		CU_ASSERT(nvmf_transport_l0_enabled());
	} else {
		CU_ASSERT(spdk_l0_data_pool_enabled == NULL);
		CU_ASSERT(spdk_l0_mempool_create == NULL);
		CU_ASSERT(!nvmf_transport_l0_enabled());
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();
	suite = CU_add_suite("nvmf_transport_weak", NULL, NULL);
	CU_ADD_TEST(suite, test_l0_symbols_absent);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return failures;
}
