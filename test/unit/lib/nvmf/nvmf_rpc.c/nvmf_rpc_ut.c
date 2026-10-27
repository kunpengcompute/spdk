/* SPDX-License-Identifier: BSD-3-Clause */

#include "spdk/stdinc.h"
#include "spdk_cunit.h"
#include "common/lib/test_env.c"
#include "nvmf/nvmf_rpc.c"
#include "upstream_stubs.h"

/* Keep the real RPC constructors and record registrations without starting a server. */
struct ut_rpc_registration {
	const char *name;
	spdk_rpc_method_handler handler;
	uint32_t state_mask;
};

static struct ut_rpc_registration g_registrations[64];
static size_t g_num_registrations;

struct spdk_jsonrpc_request {
	int error;
	bool bool_result;
	size_t response_length;
	char response[512];
};

struct spdk_io_channel_iter {
	void *ctx;
};

static struct spdk_jsonrpc_request g_request;
static struct spdk_nvmf_tgt g_target;
static uint8_t g_channel[SPDK_IO_CHANNEL_STRUCT_SIZE + sizeof(struct spdk_nvmf_poll_group)]
__attribute__((aligned(64)));
static bool g_target_exists;
static char g_target_name[128];
static unsigned int g_channel_count;
static unsigned int g_reset_count;
static unsigned int g_put_count;
static unsigned int g_continue_count;
static int g_channel_status;
static int g_qdlimit_rc;
static uint32_t g_depth;
static char g_bdev_name[128];

void
spdk_rpc_register_method(const char *method, spdk_rpc_method_handler handler, uint32_t state_mask)
{
	assert(g_num_registrations < SPDK_COUNTOF(g_registrations));
	g_registrations[g_num_registrations++] = (struct ut_rpc_registration) {
		.name = method,
		.handler = handler,
		.state_mask = state_mask,
	};
}

DEFINE_STUB_V(spdk_rpc_register_alias_deprecated, (const char *method, const char *alias));

void
spdk_jsonrpc_send_error_response(struct spdk_jsonrpc_request *request, int error_code,
			       const char *message)
{
	request->error = error_code;
	snprintf(request->response, sizeof(request->response), "%s", message);
}

void
spdk_jsonrpc_send_bool_response(struct spdk_jsonrpc_request *request, bool value)
{
	request->bool_result = value;
}

static int
ut_json_write(void *ctx, const void *data, size_t size)
{
	struct spdk_jsonrpc_request *request = ctx;

	SPDK_CU_ASSERT_FATAL(request->response_length + size < sizeof(request->response));
	memcpy(request->response + request->response_length, data, size);
	request->response_length += size;
	request->response[request->response_length] = '\0';
	return 0;
}

struct spdk_json_write_ctx *
spdk_jsonrpc_begin_result(struct spdk_jsonrpc_request *request)
{
	return spdk_json_write_begin(ut_json_write, request, 0);
}

void
spdk_jsonrpc_end_result(struct spdk_jsonrpc_request *request, struct spdk_json_write_ctx *w)
{
	CU_ASSERT(spdk_json_write_end(w) == 0);
}

struct spdk_nvmf_tgt *
spdk_nvmf_get_tgt(const char *name)
{
	snprintf(g_target_name, sizeof(g_target_name), "%s", name ? name : "<default>");
	return g_target_exists ? &g_target : NULL;
}

void *
spdk_io_channel_iter_get_ctx(struct spdk_io_channel_iter *i)
{
	return i->ctx;
}

struct spdk_io_channel *
spdk_get_io_channel(void *device)
{
	CU_ASSERT(device == &g_target);
	return (struct spdk_io_channel *)g_channel;
}

void
spdk_put_io_channel(struct spdk_io_channel *channel)
{
	CU_ASSERT(channel == (struct spdk_io_channel *)g_channel);
	g_put_count++;
}

void
spdk_nvmf_poll_group_reset_stat(struct spdk_nvmf_poll_group *group)
{
	CU_ASSERT(group == spdk_io_channel_get_ctx((struct spdk_io_channel *)g_channel));
	g_reset_count++;
}

void
spdk_for_each_channel_continue(struct spdk_io_channel_iter *i, int status)
{
	CU_ASSERT(status == 0);
	g_continue_count++;
}

void
spdk_for_each_channel(void *device, spdk_channel_msg fn, void *ctx, spdk_channel_for_each_cpl cpl)
{
	struct spdk_io_channel_iter iter = {.ctx = ctx};
	unsigned int n;

	CU_ASSERT(device == &g_target);
	for (n = 0; n < g_channel_count; n++) {
		fn(&iter);
	}
	cpl(&iter, g_channel_status);
}

int
nvmf_qdlimit_set_depth(const char *bdev_name, uint32_t depth)
{
	snprintf(g_bdev_name, sizeof(g_bdev_name), "%s", bdev_name);
	g_depth = depth;
	return g_qdlimit_rc;
}

int
nvmf_qdlimit_get_depth(const char *bdev_name, uint32_t *depth)
{
	snprintf(g_bdev_name, sizeof(g_bdev_name), "%s", bdev_name);
	*depth = g_depth;
	return g_qdlimit_rc;
}

int
nvmf_qdlimit_get_stats(const char *bdev_name, uint32_t *depth, uint32_t *total_inflight,
		      uint32_t *num_poll_groups)
{
	snprintf(g_bdev_name, sizeof(g_bdev_name), "%s", bdev_name);
	*depth = g_depth;
	*total_inflight = 7;
	*num_poll_groups = 2;
	return g_qdlimit_rc;
}

static void
ut_reset(void)
{
	memset(&g_request, 0, sizeof(g_request));
	g_target_exists = true;
	g_target_name[0] = '\0';
	g_channel_count = 2;
	g_channel_status = 0;
	g_reset_count = g_put_count = g_continue_count = 0;
	g_qdlimit_rc = 0;
	g_depth = 32;
	g_bdev_name[0] = '\0';
}

static void
ut_call(spdk_rpc_method_handler handler, const char *json)
{
	struct spdk_json_val values[32];
	char buffer[256];

	ut_reset();
	if (!json) {
		handler(&g_request, NULL);
		return;
	}
	snprintf(buffer, sizeof(buffer), "%s", json);
	SPDK_CU_ASSERT_FATAL(spdk_json_parse(buffer, strlen(buffer), values, SPDK_COUNTOF(values),
					   NULL, SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE) > 0);
	handler(&g_request, values);
}

static void
test_rpc_registrations(void)
{
	struct ut_rpc_registration expected[] = {
		{"nvmf_reset_stats", rpc_nvmf_reset_stats, SPDK_RPC_RUNTIME},
		{"nvmf_qdlimit_set_depth", rpc_nvmf_qdlimit_set_depth, SPDK_RPC_RUNTIME},
		{"nvmf_qdlimit_get_depth", rpc_nvmf_qdlimit_get_depth, SPDK_RPC_RUNTIME},
		{"nvmf_qdlimit_get_stats", rpc_nvmf_qdlimit_get_stats, SPDK_RPC_RUNTIME},
	};
	size_t i, j;

	for (i = 0; i < SPDK_COUNTOF(expected); i++) {
		for (j = 0; j < g_num_registrations; j++) {
			if (strcmp(expected[i].name, g_registrations[j].name) == 0) {
				CU_ASSERT(g_registrations[j].handler == expected[i].handler);
				CU_ASSERT(g_registrations[j].state_mask == expected[i].state_mask);
				break;
			}
		}
		CU_ASSERT(j < g_num_registrations);
	}
}

static void
test_reset_stats_success(void)
{
	ut_call(rpc_nvmf_reset_stats, NULL);
	CU_ASSERT(g_request.error == 0);
	CU_ASSERT(g_request.bool_result);
	CU_ASSERT_STRING_EQUAL(g_target_name, "<default>");
	CU_ASSERT(g_reset_count == 2);
	CU_ASSERT(g_put_count == 2);
	CU_ASSERT(g_continue_count == 2);

	ut_call(rpc_nvmf_reset_stats, "{\"tgt_name\":\"target0\"}");
	CU_ASSERT(g_request.bool_result);
	CU_ASSERT_STRING_EQUAL(g_target_name, "target0");
	CU_ASSERT(g_reset_count == 2);
}

static void
test_reset_stats_failures(void)
{
	struct rpc_nvmf_reset_stats_ctx *ctx;
	struct spdk_io_channel_iter iter;

	ut_reset();
	MOCK_SET(calloc, NULL);
	rpc_nvmf_reset_stats(&g_request, NULL);
	MOCK_CLEAR(calloc);
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INTERNAL_ERROR);
	CU_ASSERT_STRING_EQUAL(g_request.response, "Memory allocation error");

	ut_call(rpc_nvmf_reset_stats, "{\"tgt_name\":4}");
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);

	ut_reset();
	g_target_exists = false;
	rpc_nvmf_reset_stats(&g_request, NULL);
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INTERNAL_ERROR);
	CU_ASSERT_STRING_EQUAL(g_request.response, "Unable to find a target.");

	ut_reset();
	g_channel_status = -EIO;
	rpc_nvmf_reset_stats(&g_request, NULL);
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INTERNAL_ERROR);
	CU_ASSERT_STRING_EQUAL(g_request.response, spdk_strerror(EIO));
	CU_ASSERT(!g_request.bool_result);

	/* The completion also succeeds when the target has no poll groups. */
	ut_reset();
	g_channel_count = 0;
	rpc_nvmf_reset_stats(&g_request, NULL);
	CU_ASSERT(g_request.bool_result);
	CU_ASSERT(g_reset_count == 0);

	/* Exercise ownership of an allocated target name on an error completion. */
	ut_reset();
	ctx = calloc(1, sizeof(*ctx));
	SPDK_CU_ASSERT_FATAL(ctx != NULL);
	ctx->tgt_name = strdup("target0");
	ctx->request = &g_request;
	iter.ctx = ctx;
	rpc_nvmf_reset_stats_done(&iter, -ENOMEM);
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INTERNAL_ERROR);
}

static void
test_qdlimit_set_depth(void)
{
	ut_call(rpc_nvmf_qdlimit_set_depth, "{\"bdev_name\":\"disk0\",\"depth\":0}");
	CU_ASSERT(g_request.bool_result);
	CU_ASSERT_STRING_EQUAL(g_bdev_name, "disk0");
	CU_ASSERT(g_depth == 0);
	ut_call(rpc_nvmf_qdlimit_set_depth, "{\"bdev_name\":\"disk0\",\"depth\":19}");
	CU_ASSERT(g_request.bool_result);
	CU_ASSERT(g_depth == 19);
	ut_call(rpc_nvmf_qdlimit_set_depth, "{\"bdev_name\":\"disk0\",\"depth\":-1}");
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);
}

static void
test_qdlimit_set_depth_errors(void)
{
	struct rpc_qdlimit_set_depth req = {.bdev_name = "disk0", .depth = 1};
	struct spdk_json_val values[8];
	char json[] = "{\"bdev_name\":\"disk0\",\"depth\":1}";
	int errors[] = {-ENOMEM, -ENODEV};
	size_t i;

	SPDK_CU_ASSERT_FATAL(spdk_json_parse(json, strlen(json), values, SPDK_COUNTOF(values), NULL,
					   SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE) > 0);
	for (i = 0; i < SPDK_COUNTOF(errors); i++) {
		ut_reset();
		g_qdlimit_rc = errors[i];
		rpc_nvmf_qdlimit_set_depth(&g_request, values);
		CU_ASSERT(g_request.error == (errors[i] == -ENOMEM ? SPDK_JSONRPC_ERROR_INTERNAL_ERROR :
					     SPDK_JSONRPC_ERROR_INVALID_PARAMS));
		CU_ASSERT_STRING_EQUAL(g_request.response, spdk_strerror(-errors[i]));
		CU_ASSERT_STRING_EQUAL(g_bdev_name, req.bdev_name);
	}
}

static void
test_qdlimit_getters(void)
{
	ut_call(rpc_nvmf_qdlimit_get_depth, "{\"bdev_name\":\"disk0\"}");
	CU_ASSERT(g_request.error == 0);
	CU_ASSERT_STRING_EQUAL(g_bdev_name, "disk0");
	CU_ASSERT_STRING_EQUAL(g_request.response, "{\"bdev_name\":\"disk0\",\"depth\":32}");

	ut_call(rpc_nvmf_qdlimit_get_stats, "{\"bdev_name\":\"disk0\"}");
	CU_ASSERT(g_request.error == 0);
	CU_ASSERT_STRING_EQUAL(g_request.response,
			       "{\"bdev_name\":\"disk0\",\"depth\":32,\"total_inflight\":7,\"num_poll_groups\":2}");

	ut_call(rpc_nvmf_qdlimit_get_depth, "{}");
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);
	ut_call(rpc_nvmf_qdlimit_get_stats, "{\"bdev_name\":4}");
	CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);
}

static void
test_qdlimit_getters_unconfigured(void)
{
	spdk_rpc_method_handler handlers[] = {rpc_nvmf_qdlimit_get_depth, rpc_nvmf_qdlimit_get_stats};
	struct spdk_json_val values[8];
	char json[] = "{\"bdev_name\":\"disk0\"}";
	size_t i;

	SPDK_CU_ASSERT_FATAL(spdk_json_parse(json, strlen(json), values, SPDK_COUNTOF(values), NULL,
					   SPDK_JSON_PARSE_FLAG_DECODE_IN_PLACE) > 0);
	for (i = 0; i < SPDK_COUNTOF(handlers); i++) {
		ut_reset();
		g_qdlimit_rc = -ENOENT;
		handlers[i](&g_request, values);
		CU_ASSERT(g_request.error == SPDK_JSONRPC_ERROR_INVALID_PARAMS);
		CU_ASSERT_STRING_EQUAL(g_request.response, "bdev not configured for qdlimit");
		CU_ASSERT(g_request.response_length == 0);
	}
}

int
main(int argc, char **argv)
{
	CU_pSuite suite;
	unsigned int failures;

	CU_set_error_action(CUEA_ABORT);
	CU_initialize_registry();
	suite = CU_add_suite("nvmf_rpc", NULL, NULL);
	CU_ADD_TEST(suite, test_rpc_registrations);
	CU_ADD_TEST(suite, test_reset_stats_success);
	CU_ADD_TEST(suite, test_reset_stats_failures);
	CU_ADD_TEST(suite, test_qdlimit_set_depth);
	CU_ADD_TEST(suite, test_qdlimit_set_depth_errors);
	CU_ADD_TEST(suite, test_qdlimit_getters);
	CU_ADD_TEST(suite, test_qdlimit_getters_unconfigured);
	CU_basic_set_mode(CU_BRM_VERBOSE);
	CU_basic_run_tests();
	failures = CU_get_number_of_failures();
	CU_cleanup_registry();
	return failures;
}
