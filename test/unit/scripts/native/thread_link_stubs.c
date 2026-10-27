/* SPDX-License-Identifier: BSD-3-Clause
 * Native macOS UT adapters only. Standard Linux SPDK builds do not use these.
 */
#include "spdk/stdinc.h"
#include "spdk_internal/mock.h"
#include "spdk_internal/rdma.h"
#include "spdk/thread.h"
#include "spdk/trace.h"
DEFINE_STUB(spdk_get_io_channel, struct spdk_io_channel *, (void *io_device), NULL);
DEFINE_STUB(spdk_io_channel_get_thread, struct spdk_thread *, (struct spdk_io_channel *ch), NULL);
DEFINE_STUB(spdk_thread_send_msg, int, (const struct spdk_thread *thread, spdk_msg_fn fn, void *ctx), 0);
DEFINE_STUB(spdk_poller_register_named, struct spdk_poller *, (spdk_poller_fn fn,
 void *arg, uint64_t period_microseconds, const char *name), NULL);
void spdk_poller_unregister(struct spdk_poller **poller) { *poller = NULL; }
DEFINE_STUB_V(spdk_put_io_channel, (struct spdk_io_channel *ch));
