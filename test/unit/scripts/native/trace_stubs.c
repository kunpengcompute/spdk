/* SPDX-License-Identifier: BSD-3-Clause
 * Native macOS UT adapters only. Standard Linux SPDK builds do not use these.
 */
#include "spdk/stdinc.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/rdma.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
struct spdk_trace_histories *g_trace_histories;
void spdk_trace_add_register_fn(struct spdk_trace_register_fn *reg_fn) {}
void spdk_trace_register_description(const char *name, uint16_t tpoint_id, uint8_t owner_type,
 uint8_t object_type, uint8_t new_object, uint8_t arg1_type, const char *arg1_name) {}
void spdk_trace_register_object(uint8_t type, char id_prefix) {}
void _spdk_trace_record(uint64_t tsc, uint16_t tpoint_id, uint16_t poller_id,
 uint32_t size, uint64_t object_id, int num_args, ...) { abort(); }
