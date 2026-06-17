# qdlimit (NVMe-oF/RDMA pre-buffer admission control) — Design Spec

Date: 2026-06-17
Branch: `qdlimit`
Supersedes: `2026-06-16-qdlimit-vbdev-design.md` (bdev-layer passthrough vbdev)

## 1. Motivation

The original `qdlimit` is a bdev-layer passthrough vbdev: it caps per-core in-flight
depth to a backing SSD, but it only sees an IO **after** the NVMe-oF target has already
allocated a data buffer for it from the transport's shared pool.

The real overload problem we need to solve is **shared `data_buf_pool` exhaustion**. The
RDMA transport allocates data buffers from a single pool shared across all connections and
namespaces (`transport->data_buf_pool`, `lib/nvmf/transport.c`). Under overload, a slow or
busy SSD accumulates many in-flight requests that each hold a buffer; this drains the shared
pool and inflicts head-of-line blocking and tail latency on **every other** namespace sharing
that pool — a classic noisy-neighbor problem. Limiting at the bdev layer is too late: the
buffer is already taken by the time the IO reaches the vbdev.

**Goal:** move admission control to *before* buffer allocation, so a single SSD cannot
monopolize the shared buffer pool. Preserve the per-SSD semantics and per-core approximate
counting of the original design.

## 2. Scope and Key Decisions

| Decision | Choice |
|----------|--------|
| What we protect | Shared RDMA `data_buf_pool` (memory pressure / noisy-neighbor isolation) |
| Limit granularity | **per-SSD** (per backing bdev), aggregated across all namespaces mapping to it |
| Counting model | **per-core** (per poll group), lock-free, approximate (global ≈ `depth × cores`) — same model as original |
| Architecture | **Approach B:** limiting lives entirely in nvmf; the bdev-layer vbdev is retired |
| Transport | **RDMA only** (`lib/nvmf/rdma.c`); TCP out of scope |
| Gate point | `RDMA_REQUEST_STATE_NEW → NEED_BUFFER`, before `nvmf_rdma_request_parse_sgl`/`get_buffers` |
| Over-limit behavior | Park request on a **per-SSD wait queue** (removed from `pending_buf_queue`) so it does not block requests for other SSDs |
| Slot release timing | Hooked into `_nvmf_rdma_request_free` (buffer returned to pool, i.e. **post-ACK** for reads) |
| Decoupling level | **L2:** all logic in a standalone `lib/nvmf/qdlimit.{c,h}` component; rdma.c gets only 2–3 thin, marked hook points |
| Config identity | bdev name; `depth = 0` means unlimited |

Out of scope: TCP/FC transports, global (cross-core) exact counting, dynamic/adaptive
thresholds, persistence of config across restarts.

## 3. Background: the RDMA buffer path (verified against source)

NVMe-oF/RDMA request state machine (`nvmf_rdma_request_process`, `lib/nvmf/rdma.c`):

```
NEW
 └─ xfer != NONE → NEED_BUFFER, insert into group->pending_buf_queue   (rdma.c:2058)
NEED_BUFFER                                                            (rdma.c:2061)
 ├─ only the HEAD of pending_buf_queue is processed (strict FIFO)      (rdma.c:2067)
 ├─ nvmf_rdma_request_parse_sgl() → spdk_nvmf_request_get_buffers()    (rdma.c:2073, 1581/1688)
 └─ if no buffer: stay queued, retried every poll                      (rdma.c:2080)
...
READY_TO_COMPLETE → request_transfer_out() posts RDMA WRITE+SEND
 └─ TRANSFERRING_CONTROLLER_TO_HOST  (waits for data WR completion = remote ACK)
 └─ [WR completion/ACK] → COMPLETED                                    (rdma.c:2300)
       └─ _nvmf_rdma_request_free() → spdk_nvmf_request_free_buffers() (rdma.c:1887)
```

Two existing facts this design leans on:

1. **Buffer back-pressure already exists.** Requests waiting for a buffer sit on the
   group-wide `pending_buf_queue` and are re-driven every poll by
   `nvmf_rdma_qpair_process_pending` (rdma.c:2904). qdlimit reuses this retry loop — no new
   wakeup machinery needed.

2. **Buffers are already freed post-ACK.** For reads, the buffer is returned to the pool in
   the `COMPLETED` state, which is only reached after the data-transfer WR completes (the
   remote NIC has ACKed the data). Hooking slot release into `_nvmf_rdma_request_free` makes
   the qdlimit slot release coincide exactly with buffer return — post-ACK, by construction.

**Head-of-line hazard:** `pending_buf_queue` is strict FIFO and processes only its head. With
per-SSD limits this would let a throttled (over-limit) SSD's head request block requests for
*other* SSDs queued behind it — defeating isolation. The design avoids this by removing a
throttled request from `pending_buf_queue` and parking it on a per-SSD wait queue.

## 4. Architecture (L2)

All qdlimit logic lives in a new self-contained component:

- `lib/nvmf/qdlimit.c` / `lib/nvmf/qdlimit.h`

The RDMA transport calls into it through a small, stable hook API. The transport never sees
qdlimit internals (config table, counters, wait queues).

### 4.1 Hook API (`qdlimit.h`)

```c
/* Decision returned by the admission gate. */
enum nvmf_qdlimit_status {
    NVMF_QDLIMIT_ADMIT,     /* under limit (or unlimited): proceed to get_buffers */
    NVMF_QDLIMIT_THROTTLED, /* over limit: request parked on per-SSD wait queue */
};

/* Per-poll-group context lifecycle (one per RDMA poll group). */
void  nvmf_qdlimit_pg_init(struct spdk_nvmf_transport_poll_group *group);
void  nvmf_qdlimit_pg_fini(struct spdk_nvmf_transport_poll_group *group);

/* Admission gate, called at NEW->NEED_BUFFER before get_buffers.
 * On THROTTLED, the module removes req from pending_buf_queue and parks it. */
enum nvmf_qdlimit_status
nvmf_qdlimit_admit(struct spdk_nvmf_transport_poll_group *group,
                   struct spdk_nvmf_request *req);

/* Slot release, called from _nvmf_rdma_request_free.
 * Decrements the per-core counter and re-arms one parked waiter for that SSD. */
void  nvmf_qdlimit_release(struct spdk_nvmf_transport_poll_group *group,
                          struct spdk_nvmf_request *req);

/* RPC-facing config (global, per-bdev). depth == 0 => unlimited. */
int   nvmf_qdlimit_set_depth(const char *bdev_name, uint32_t depth);
int   nvmf_qdlimit_get_depth(const char *bdev_name, uint32_t *depth);
/* iteration for list RPC omitted here for brevity */
```

### 4.2 Data structures

**Global config (module-owned, RPC-managed):** a table keyed by bdev name →
`{ uint32_t depth; struct spdk_bdev *bdev; }`. Written rarely (RPC), read on the hot path.
`depth == 0` => unlimited. Per-SSD semantics: all namespaces backed by the same bdev share
one limit.

**Per-poll-group context (`void *qdlimit_ctx` stored on the poll group):** for each SSD seen
on this core, an entry holding:
- `uint32_t inflight;` — count of buffer-holding admitted requests on this core (lock-free;
  only ever touched by this core's poll thread)
- `STAILQ_HEAD wait_q;` — per-SSD FIFO of requests throttled on this core

A new field is added to the poll group struct:
```c
void *qdlimit_ctx;   /* opaque per-group qdlimit state; NULL when unused */
```

**Per-request marker:** a single bit recording whether this request currently holds a
qdlimit slot, so `release` decrements exactly once and only for charged requests. Stored on
`struct spdk_nvmf_rdma_request` (e.g. `bool qdlimit_charged;`) to keep the module free of
per-request allocation.

### 4.3 Resolving nsid → bdev at the gate

Reuses existing helpers; no new lookup primitive:
- `subsystem = req->qpair->ctrlr->subsys`
- `ns = _nvmf_subsystem_get_ns(subsystem, req->cmd->nvme_cmd.nsid)` (`nvmf_internal.h:448`)
- `bdev = ns->bdev`

If ns or bdev cannot be resolved (admin/fabrics commands, invalid nsid), or the resolved
bdev has no configured limit / `depth == 0`, the gate returns `ADMIT` immediately (bypass).
Only data-bearing IO (`xfer != SPDK_NVME_DATA_NONE`) is ever gated.

## 5. Control flow

### 5.1 Admission (`NEW → NEED_BUFFER`, in rdma.c via `nvmf_qdlimit_admit`)

```
if req->xfer == DATA_NONE:               return ADMIT          # no buffer needed
resolve bdev from nsid; if none:         return ADMIT          # bypass
depth = config[bdev]; if depth == 0:     return ADMIT          # unlimited
ctx  = per-core entry for bdev
if ctx.inflight >= depth:
    STAILQ_REMOVE(group->pending_buf_queue, req)               # unblock other SSDs
    STAILQ_INSERT_TAIL(ctx.wait_q, req)
    req.qdlimit_charged = false
    return THROTTLED
else:
    ctx.inflight++
    req.qdlimit_charged = true
    return ADMIT                                               # proceed to get_buffers
```

rdma.c at `RDMA_REQUEST_STATE_NEED_BUFFER`, before `parse_sgl`:
```c
if (nvmf_qdlimit_admit(group, &rdma_req->req) == NVMF_QDLIMIT_THROTTLED) {
    break;   /* parked on per-SSD wait queue; will be re-armed on release */
}
```

### 5.2 Release (`_nvmf_rdma_request_free`, via `nvmf_qdlimit_release`)

```
if not req.qdlimit_charged:               return            # never charged / already released
resolve bdev; ctx = per-core entry
ctx.inflight--
req.qdlimit_charged = false
if ctx.wait_q not empty:
    next = STAILQ_REMOVE_HEAD(ctx.wait_q)
    STAILQ_INSERT_HEAD(group->pending_buf_queue, next)        # re-arm: head, admissible next poll
```

The re-armed waiter is reconsidered by `nvmf_rdma_qpair_process_pending` on the next poll;
since `inflight` is now below `depth`, it passes the gate and proceeds to `get_buffers`. No
explicit transport callback is required — the existing per-poll retry loop drives it.

### 5.3 Liveness and ordering notes

- **Isolation:** throttled requests live only on their SSD's `wait_q`, never on the shared
  `pending_buf_queue`, so an over-limit SSD never blocks another SSD's progress.
- **FIFO per SSD:** `wait_q` is FIFO; releases re-arm the oldest waiter first.
- **Liveness inheritance:** re-armed requests rejoin `pending_buf_queue` and are driven by the
  same per-poll loop that already recovers from buffer starvation, so qdlimit inherits its
  liveness guarantees.
- **Teardown:** on qpair/poll-group teardown, parked requests on `wait_q` must be drained the
  same way `pending_buf_queue` entries are (state reset / completed with error). `pg_fini`
  and the qpair destroy path account for any still-parked requests.

## 6. RPC / management surface

Replaces the retired vbdev RPCs (`bdev_qdlimit_create/delete/set_depth`).

- `nvmf_qdlimit_set_depth`  — params: `bdev_name` (string), `depth` (uint32, 0 = unlimited)
- `nvmf_qdlimit_get_depth`  — params: `bdev_name`; returns configured depth
- `nvmf_qdlimit_get_stats`  — returns, per configured SSD, the depth and per-core in-flight
  counts (and optionally parked counts) for observability

Plus Python bindings (`scripts/rpc/`) and `scripts/rpc.py` CLI subcommands mirroring the
existing nvmf RPC conventions.

## 7. Retiring the vbdev

- Remove `module/bdev/qdlimit/` (`vbdev_qdlimit.c`, `.h`, `_rpc.c`, `Makefile`) and its
  build wiring.
- Remove its Python bindings / CLI subcommands.
- Remove `test/unit/lib/bdev/.../qdlimit` unit tests and the malloc-based integration test
  that target the vbdev.
- The cross-machine NVMe-oF/RDMA perf harness is repurposed to validate the new mechanism
  (see §8).

## 8. Testing

**Unit tests** (`test/unit/lib/nvmf/qdlimit.c`, module tested in isolation via the hook API):
- admit increments per-core counter; release decrements exactly once
- `qdlimit_charged` guards double-release; uncharged request release is a no-op
- over-limit request is parked on the SSD `wait_q` and removed from `pending_buf_queue`
- release re-arms the FIFO head of `wait_q` back onto `pending_buf_queue`
- `xfer == DATA_NONE` bypasses the gate
- `depth == 0` (unlimited) and unconfigured bdev bypass the gate
- two SSDs: throttling SSD-A leaves SSD-B's counter/flow untouched (isolation)

**Integration test** (RDMA; SoftRoCE/`rxe` provides a local RDMA device for CI):
- two namespaces backed by two malloc/null bdevs sharing one transport buffer pool
- set a low depth on the "slow" SSD-A; drive both with fio
- assert: SSD-B p99 latency is unaffected while SSD-A is throttled; the shared pool is not
  exhausted (no global stalls); SSD-A throughput tracks its configured depth

**Cross-machine perf** (repurposed harness): two hosts over RDMA/RoCE, demonstrate tail-latency
isolation under buffer-pool pressure with vs without a configured limit.

## 9. Intrusion summary (what changes in trunk)

- `lib/nvmf/rdma.c`: (1) one `nvmf_qdlimit_admit` call at NEED_BUFFER, (2) one
  `nvmf_qdlimit_release` call in `_nvmf_rdma_request_free`, (3) `pg_init`/`pg_fini` calls in
  poll-group create/destroy, (4) one `bool qdlimit_charged` field on the rdma request struct.
- poll group struct: one `void *qdlimit_ctx` field.
- New: `lib/nvmf/qdlimit.{c,h}`, RPC, Python bindings, CLI, unit + integration tests.
- Removed: the entire `module/bdev/qdlimit/` vbdev and its tests.

All hot-path edits are small, clearly marked, and side-effect-free when no limit is
configured (`qdlimit_ctx == NULL` / `depth == 0` fast bypass), minimizing rebase exposure
against upstream rdma.c.
