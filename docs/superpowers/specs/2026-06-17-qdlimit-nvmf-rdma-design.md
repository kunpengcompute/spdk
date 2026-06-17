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

**Buffer-occupancy ceiling test (key acceptance criterion).** Prove that admission control
caps per-SSD buffer-pool usage regardless of client load:
- Configure one SSD with depth `qd` and a fixed IO size `iosize`. Sweep client concurrency
  (fio `iodepth` × jobs / number of connections) from below `qd` to well above it.
- Measure that SSD's buffer occupancy at each step. Occupancy is derived from
  `nvmf_qdlimit_get_stats` (sum of per-core `inflight` for the SSD) × `iosize`, cross-checked
  against the transport pool consumption (`spdk_mempool_count(data_buf_pool)` delta).
- **Expected shape:** occupancy rises **linearly** with offered concurrency while offered
  < `qd` (per core), then **plateaus** once offered ≥ `qd` — it does not keep growing.
  Ceiling = `qd × iosize` per core, i.e. `qd × iosize × cores` globally. Excess client
  concurrency is absorbed by the per-SSD wait queue, not by additional buffers.
- Assert: measured occupancy never exceeds the ceiling (within one in-flight quantum) and
  is flat across all over-subscribed steps; with the limit removed (`depth = 0`), occupancy
  instead keeps climbing with concurrency (control case).

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

---
---

# qdlimit(NVMe-oF/RDMA buffer 申请前准入控制)— 设计文档(中文版)

日期:2026-06-17
分支:`qdlimit`
取代:`2026-06-16-qdlimit-vbdev-design.md`(bdev 层 passthrough vbdev)

> 本节为上文英文版的中文对照,内容完全一致;若有歧义以英文版为准。

## 1. 动机

原始 `qdlimit` 是一个 bdev 层的 passthrough vbdev:它给后端 SSD 设置 per-core 在途深度上限,
但它只能在 NVMe-oF target **已经**从 transport 共享池为该 IO 分配好数据 buffer **之后**才看到这个 IO。

我们真正要解决的过载问题是**共享 `data_buf_pool` 被耗尽**。RDMA transport 从一个被所有连接和
namespace 共用的池子(`transport->data_buf_pool`,`lib/nvmf/transport.c`)分配数据 buffer。过载时,
一个慢盘/忙盘会堆积大量在途请求,每个都占着一块 buffer;这会把共享池掏空,并对**所有其它**共用该
池的 namespace 造成队头阻塞和尾延迟——典型的 noisy-neighbor 问题。在 bdev 层限流太晚了:IO 到达
vbdev 时 buffer 早已被占用。

**目标:** 把准入控制前移到 *buffer 分配之前*,使单个 SSD 无法独占共享 buffer 池。保留原设计的
per-SSD 语义与 per-core 近似计数。

## 2. 范围与关键决策

| 决策 | 选择 |
|------|------|
| 保护对象 | 共享的 RDMA `data_buf_pool`(内存压力 / noisy-neighbor 隔离) |
| 限流粒度 | **per-SSD**(按后端 bdev),映射到同一 bdev 的所有 namespace 聚合计算 |
| 计数模型 | **per-core**(每个 poll group),无锁、近似(全局 ≈ `depth × 核数`)——与原设计相同 |
| 架构 | **方案 B:** 限流逻辑完全放在 nvmf;bdev 层 vbdev 退役 |
| 传输层 | **仅 RDMA**(`lib/nvmf/rdma.c`);TCP 不在范围内 |
| 闸门位置 | `RDMA_REQUEST_STATE_NEW → NEED_BUFFER`,位于 `nvmf_rdma_request_parse_sgl`/`get_buffers` 之前 |
| 超限行为 | 把请求停到**per-SSD 等待队列**(从 `pending_buf_queue` 摘出),以免堵塞发往其它 SSD 的请求 |
| 名额释放时机 | 挂入 `_nvmf_rdma_request_free`(buffer 还池,即读请求的 **post-ACK** 时刻) |
| 解耦层级 | **L2:** 全部逻辑在独立的 `lib/nvmf/qdlimit.{c,h}` 组件;rdma.c 只留 2–3 个有标记的薄钩子 |
| 配置标识 | bdev 名;`depth = 0` 表示不限 |

不在范围内:TCP/FC 传输、全局(跨核)精确计数、动态/自适应阈值、配置跨重启持久化。

## 3. 背景:RDMA buffer 路径(已对照源码核实)

NVMe-oF/RDMA 请求状态机(`nvmf_rdma_request_process`,`lib/nvmf/rdma.c`):

```
NEW
 └─ xfer != NONE → NEED_BUFFER,插入 group->pending_buf_queue          (rdma.c:2058)
NEED_BUFFER                                                            (rdma.c:2061)
 ├─ 只处理 pending_buf_queue 的队头(严格 FIFO)                       (rdma.c:2067)
 ├─ nvmf_rdma_request_parse_sgl() → spdk_nvmf_request_get_buffers()   (rdma.c:2073, 1581/1688)
 └─ 若无 buffer:留在队列,每个 poll 重试                              (rdma.c:2080)
...
READY_TO_COMPLETE → request_transfer_out() 投递 RDMA WRITE+SEND
 └─ TRANSFERRING_CONTROLLER_TO_HOST(等待数据 WR 完成 = 远端 ACK)
 └─ [WR 完成/ACK] → COMPLETED                                          (rdma.c:2300)
       └─ _nvmf_rdma_request_free() → spdk_nvmf_request_free_buffers() (rdma.c:1887)
```

本设计依赖的两个既有事实:

1. **buffer 回压机制已存在。** 等待 buffer 的请求挂在 group 级的 `pending_buf_queue` 上,由
   `nvmf_rdma_qpair_process_pending`(rdma.c:2904)每个 poll 重新驱动。qdlimit 复用这套重试循环——
   无需新增唤醒机制。

2. **buffer 本就在 ACK 之后释放。** 对读请求,buffer 是在 `COMPLETED` 状态还池的,而该状态只有在
   数据传输 WR 完成(远端 NIC 已对数据回 ACK)后才会到达。把名额释放挂到 `_nvmf_rdma_request_free`,
   就使 qdlimit 名额释放与 buffer 还池在结构上严格重合——天然 post-ACK。

**队头阻塞隐患:** `pending_buf_queue` 是严格 FIFO 且只处理队头。在 per-SSD 限流下,这会让一个超限
SSD 的队头请求堵住排在它后面、发往*其它* SSD 的请求——破坏隔离。本设计通过把超限请求从
`pending_buf_queue` 摘出、停到 per-SSD 等待队列来规避此问题。

## 4. 架构(L2)

所有 qdlimit 逻辑放在一个新的自包含组件里:

- `lib/nvmf/qdlimit.c` / `lib/nvmf/qdlimit.h`

RDMA transport 通过一套小而稳定的钩子 API 调用它。transport 永远看不到 qdlimit 内部
(配置表、计数器、等待队列)。

### 4.1 钩子 API(`qdlimit.h`)

```c
/* 准入闸门返回的决策。*/
enum nvmf_qdlimit_status {
    NVMF_QDLIMIT_ADMIT,     /* 未超限(或不限):继续去 get_buffers */
    NVMF_QDLIMIT_THROTTLED, /* 超限:请求已停到 per-SSD 等待队列 */
};

/* per-poll-group 上下文生命周期(每个 RDMA poll group 一份)。*/
void  nvmf_qdlimit_pg_init(struct spdk_nvmf_transport_poll_group *group);
void  nvmf_qdlimit_pg_fini(struct spdk_nvmf_transport_poll_group *group);

/* 准入闸门,在 NEW->NEED_BUFFER、get_buffers 之前调用。
 * 返回 THROTTLED 时,模块已把 req 从 pending_buf_queue 摘出并停放。*/
enum nvmf_qdlimit_status
nvmf_qdlimit_admit(struct spdk_nvmf_transport_poll_group *group,
                   struct spdk_nvmf_request *req);

/* 名额释放,从 _nvmf_rdma_request_free 调用。
 * 减 per-core 计数,并为该 SSD 重新激活一个停放中的等待者。*/
void  nvmf_qdlimit_release(struct spdk_nvmf_transport_poll_group *group,
                          struct spdk_nvmf_request *req);

/* 面向 RPC 的配置(全局、按 bdev)。depth == 0 => 不限。*/
int   nvmf_qdlimit_set_depth(const char *bdev_name, uint32_t depth);
int   nvmf_qdlimit_get_depth(const char *bdev_name, uint32_t *depth);
/* list RPC 的遍历接口此处从略 */
```

### 4.2 数据结构

**全局配置(模块持有,RPC 管理):** 一张以 bdev 名为键的表 →
`{ uint32_t depth; struct spdk_bdev *bdev; }`。极少写(RPC),热路径读。`depth == 0` => 不限。
per-SSD 语义:映射到同一 bdev 的所有 namespace 共用一个上限。

**per-poll-group 上下文(`void *qdlimit_ctx`,挂在 poll group 上):** 对本核见过的每个 SSD,有一个条目:
- `uint32_t inflight;` — 本核上持有 buffer 的已准入请求数(无锁;只被本核 poll 线程访问)
- `STAILQ_HEAD wait_q;` — 本核上被限流请求的 per-SSD FIFO

在 poll group 结构体里新增一个字段:
```c
void *qdlimit_ctx;   /* 不透明的 per-group qdlimit 状态;未启用时为 NULL */
```

**per-request 标记:** 一个 bit,记录该请求当前是否持有 qdlimit 名额,使 `release` 只对已计数的请求
精确减一次。存在 `struct spdk_nvmf_rdma_request` 上(如 `bool qdlimit_charged;`),让模块免于 per-request 分配。

### 4.3 闸门处的 nsid → bdev 解析

复用现有 helper,不引入新原语:
- `subsystem = req->qpair->ctrlr->subsys`
- `ns = _nvmf_subsystem_get_ns(subsystem, req->cmd->nvme_cmd.nsid)`(`nvmf_internal.h:448`)
- `bdev = ns->bdev`

若 ns/bdev 无法解析(admin/fabrics 命令、非法 nsid),或解析出的 bdev 没有配置上限 / `depth == 0`,
闸门立即返回 `ADMIT`(旁路)。只有带数据的 IO(`xfer != SPDK_NVME_DATA_NONE`)才会被限流。

## 5. 控制流

### 5.1 准入(`NEW → NEED_BUFFER`,rdma.c 中经由 `nvmf_qdlimit_admit`)

```
if req->xfer == DATA_NONE:               return ADMIT          # 不需要 buffer
resolve bdev from nsid; if none:         return ADMIT          # 旁路
depth = config[bdev]; if depth == 0:     return ADMIT          # 不限
ctx  = bdev 对应的 per-core 条目
if ctx.inflight >= depth:
    STAILQ_REMOVE(group->pending_buf_queue, req)               # 放开其它 SSD
    STAILQ_INSERT_TAIL(ctx.wait_q, req)
    req.qdlimit_charged = false
    return THROTTLED
else:
    ctx.inflight++
    req.qdlimit_charged = true
    return ADMIT                                               # 继续去 get_buffers
```

rdma.c 在 `RDMA_REQUEST_STATE_NEED_BUFFER`、`parse_sgl` 之前:
```c
if (nvmf_qdlimit_admit(group, &rdma_req->req) == NVMF_QDLIMIT_THROTTLED) {
    break;   /* 已停到 per-SSD 等待队列;释放时会被重新激活 */
}
```

### 5.2 释放(`_nvmf_rdma_request_free`,经由 `nvmf_qdlimit_release`)

```
if not req.qdlimit_charged:               return            # 从未计数 / 已释放
resolve bdev; ctx = per-core 条目
ctx.inflight--
req.qdlimit_charged = false
if ctx.wait_q not empty:
    next = STAILQ_REMOVE_HEAD(ctx.wait_q)
    STAILQ_INSERT_HEAD(group->pending_buf_queue, next)        # 重新激活:塞回队头,下个 poll 即可放行
```

被重新激活的等待者会在下个 poll 由 `nvmf_rdma_qpair_process_pending` 重新评估;此时 `inflight` 已低于
`depth`,于是通过闸门继续去 `get_buffers`。无需显式的 transport 回调——由既有的 per-poll 重试循环驱动。

### 5.3 活性与顺序说明

- **隔离:** 被限流请求只待在其 SSD 的 `wait_q` 上,绝不进共享 `pending_buf_queue`,因此超限 SSD 永不
  阻塞其它 SSD 的推进。
- **per-SSD FIFO:** `wait_q` 是 FIFO;释放时优先重新激活最早的等待者。
- **活性继承:** 重新激活的请求回到 `pending_buf_queue`,由同一个已能从 buffer 饥饿中恢复的 per-poll
  循环驱动,因此 qdlimit 继承其活性保证。
- **拆除:** qpair/poll-group 拆除时,`wait_q` 上停放的请求必须像 `pending_buf_queue` 条目一样被排空
  (状态重置 / 以错误完成)。`pg_fini` 与 qpair 销毁路径负责处理仍停放的请求。

## 6. RPC / 管理接口

取代已退役的 vbdev RPC(`bdev_qdlimit_create/delete/set_depth`)。

- `nvmf_qdlimit_set_depth`  — 参数:`bdev_name`(字符串)、`depth`(uint32,0 = 不限)
- `nvmf_qdlimit_get_depth`  — 参数:`bdev_name`;返回已配置的 depth
- `nvmf_qdlimit_get_stats`  — 返回每个已配置 SSD 的 depth 与各核在途数(可选含停放数),用于可观测性

外加 Python 绑定(`scripts/rpc/`)和 `scripts/rpc.py` CLI 子命令,沿用既有 nvmf RPC 约定。

## 7. vbdev 退役

- 删除 `module/bdev/qdlimit/`(`vbdev_qdlimit.c`、`.h`、`_rpc.c`、`Makefile`)及其构建接线。
- 删除其 Python 绑定 / CLI 子命令。
- 删除针对该 vbdev 的 `test/unit/lib/bdev/.../qdlimit` 单测与基于 malloc 的集成测试。
- 跨机 NVMe-oF/RDMA 性能测试改造为验证新机制(见 §8)。

## 8. 测试

**单元测试**(`test/unit/lib/nvmf/qdlimit.c`,通过钩子 API 隔离测试模块):
- admit 使 per-core 计数 +1;release 精确 −1 一次
- `qdlimit_charged` 防止重复释放;对未计数请求的释放是 no-op
- 超限请求被停到该 SSD 的 `wait_q` 并从 `pending_buf_queue` 摘出
- release 把 `wait_q` 的 FIFO 队头重新激活回 `pending_buf_queue`
- `xfer == DATA_NONE` 旁路闸门
- `depth == 0`(不限)与未配置的 bdev 旁路闸门
- 两个 SSD:限流 SSD-A 不影响 SSD-B 的计数/流量(隔离)

**集成测试**(RDMA;用 SoftRoCE/`rxe` 在 CI 里提供本地 RDMA 设备):
- 两个 namespace 由两个 malloc/null bdev 支撑,共用一个 transport buffer 池
- 给"慢盘"SSD-A 设一个低 depth;用 fio 同时压两个盘
- 断言:SSD-A 被限流期间 SSD-B 的 p99 不受影响;共享池不被耗尽(无全局停顿);SSD-A 吞吐与其配置 depth 一致

**Buffer 占用封顶测试(关键验收标准)。** 证明准入控制无论客户端负载多大,都能把单盘的 buffer 池占用封顶:
- 给某盘配置 depth `qd` 与固定 IO 大小 `iosize`。把客户端并发(fio `iodepth` × jobs / 连接数)从低于 `qd`
  扫到远高于 `qd`。
- 每一步测量该盘的 buffer 占用。占用由 `nvmf_qdlimit_get_stats`(该 SSD 各核 `inflight` 求和)× `iosize`
  得出,并用 transport 池消耗量(`spdk_mempool_count(data_buf_pool)` 的差值)交叉核对。
- **期望形态:** 当 offered < `qd`(单核)时占用随并发**线性增长**,一旦 offered ≥ `qd` 便**封平**——不再增长。
  天花板 = 单核 `qd × iosize`,即全局 `qd × iosize × 核数`。多出的客户端并发被 per-SSD 等待队列吸收,而非占用更多 buffer。
- 断言:实测占用始终不超过天花板(误差在一个在途量子内),且在所有过载步骤保持平直;移除限制(`depth = 0`)后,
  占用则随并发持续攀升(对照组)。

**跨机性能**(改造后的测试台):两台主机经 RDMA/RoCE,演示在 buffer 池压力下、有/无配置上限两种情况的尾延迟隔离效果。

## 9. 侵入面汇总(trunk 中的改动)

- `lib/nvmf/rdma.c`:(1) NEED_BUFFER 处一处 `nvmf_qdlimit_admit` 调用;(2) `_nvmf_rdma_request_free` 中一处
  `nvmf_qdlimit_release` 调用;(3) poll-group 创建/销毁处的 `pg_init`/`pg_fini` 调用;(4) rdma 请求结构体上一个
  `bool qdlimit_charged` 字段。
- poll group 结构体:一个 `void *qdlimit_ctx` 字段。
- 新增:`lib/nvmf/qdlimit.{c,h}`、RPC、Python 绑定、CLI、单元 + 集成测试。
- 删除:整个 `module/bdev/qdlimit/` vbdev 及其测试。

所有热路径改动都很小、有清晰标记,且在未配置上限时无副作用(`qdlimit_ctx == NULL` / `depth == 0` 快速旁路),
把相对上游 rdma.c 的 rebase 风险降到最低。
