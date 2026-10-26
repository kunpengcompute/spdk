# 设计文档：每盘并发控制 vbdev（队列深度限制器 qdlimit）

- 日期：2026-06-16
- 分支：v23.01.x
- 状态：已评审，待实现

## 1. 背景与目标

### 1.1 诉求
在 SPDK NVMf target（服务端）把 IO 下发给 SSD 之前，增加一个队列管理模块，
**控制对每个 SSD 的并发（在途 IO 深度）**，用于压制单盘过载、降低尾延迟（P99）。

约束：
- 模块要**解耦干净**——尽量不改 SPDK 主干代码。
- **方便迁移**——换 SPDK 版本时改动最小。
- 先做**静态控制**（固定阈值，不做自适应）。

### 1.2 关键决策（已与需求方确认）
1. **目标 = 压尾延迟 / 防单盘过载**（不是速率限制、不是多租户公平）。
2. **限流精度 = per-core 近似（无锁）**。即对每个 `(SSD, reactor 核)` 限制在途深度，
   全盘实际上限 ≈ `活跃核数 × 单核阈值`。这与 SPDK share-nothing 的 per-core 模型天然契合，
   不引入跨核原子/共享状态。
3. **实现形态 = 独立 passthrough vbdev 模块**，叠加在底层 bdev（典型为 nvme bdev = 一块 SSD）之上。

## 2. SPDK 现状（设计依据）

### 2.1 SQE 处理流程（接收 → 下发 SSD）
```
网络(RDMA/TCP)
  ① Transport  lib/nvmf/rdma.c·tcp.c   解析 capsule → struct spdk_nvmf_request(req->cmd 即 SQE)
  ② Controller lib/nvmf/ctrlr.c        spdk_nvmf_request_exec → 区分 Admin/IO
  ③ NVMf-bdev  lib/nvmf/ctrlr_bdev.c   翻译 SQE 字段 → spdk_bdev_readv_blocks() 等
  ④ 通用 bdev  lib/bdev/bdev.c         (现有 QoS 仅速率限制 IOPS/bps, 非并发限制)
  ⑤ NVMe bdev  module/bdev/nvme/bdev_nvme.c
               _bdev_nvme_submit_request → 选 nvme_qpair → spdk_nvme_ns_cmd_*(ns, qpair, ...)
               ★ 这一步才把请求构造成 SQE 放进 SSD 提交队列
  物理 SSD
```

### 2.2 请求的"变形链"：SQE → bdev_io → SQE（决定排队对象）
- 原始 64B SQE（`struct spdk_nvme_cmd`）只存在于 **①②层的 `req->cmd`**。
- 第 ③ 层 `ctrlr_bdev.c` 把 SQE 翻译成 **块级语义**后，请求即以 `struct spdk_bdev_io` 形态向下流动。
- 在第 ④ 层（含本模块所处的 vbdev 层）请求**不再携带 SQE**，而是：
  - `bdev_io->type` = `SPDK_BDEV_IO_TYPE_READ / WRITE / ...`
  - `bdev_io->u.bdev.{iovs, iovcnt, offset_blocks(=SLBA), num_blocks(=NLB), md_buf}`
- SQE 在第 ⑤ 层 `bdev_nvme` 中由 `spdk_nvme_ns_cmd_*` **依据 bdev_io 字段重新构造**。
- 仅透传命令（`SPDK_BDEV_IO_TYPE_NVME_IO/ADMIN`）的 bdev_io 里才内嵌 `struct spdk_nvme_cmd`
  （`bdev_io->u.nvme_passthru.cmd`）；普通读写不含 SQE。

**结论**：本模块拦截/排队的对象是 `struct spdk_bdev_io *`（块级语义），挡在 SQE 生成之前，
正好满足"在 SQE 下发给 SSD 前控制并发"的目标。入队为零拷贝（仅链入链表）。

### 2.3 SQE 如何分发到核（决定为何 per-core 即可）
- 分发单位是**连接（qpair）**，不是单个 SQE。
- 连接建立时 `spdk_nvmf_tgt_new_qpair()` 通过 `get_optimal_poll_group`（RDMA 亲和）
  或 round-robin（`tgt->next_poll_group`）把连接绑定到某个 poll group 的线程（= 一个 reactor 核），
  此后该连接所有 SQE 都在该核处理。
- 每个对 SSD X 有 IO 的核，会给 SSD X 建**一个** `nvme_qpair`。
- 故单盘在途总深度 = 各核 qpair 在途之和；**没有任何单核能看到全局深度** → per-core 限流是自然且无锁的粒度。

## 3. 方案选型

| 方案 | 描述 | 评价 |
|---|---|---|
| ① 改 bdev_nvme | 在 `_bdev_nvme_submit_request` 拦截，给 `nvme_qpair` 挂计数器 | 精度最高，但侵入最复杂文件、迁移成本高 ✗ |
| ② 扩展 bdev QoS | 在 `lib/bdev/bdev.c` QoS 框架加"深度限制" | 需改核心共享文件、QoS 是速率语义且跑独立线程 ✗ |
| ③ 独立 passthru vbdev | 新增模块叠加在底层 bdev 上 | 零改主干、迁移最易、per-core 无锁 ✓ **采用** |

**采用方案③**：最契合"解耦干净 + 易迁移 + per-core 无锁"，遵循 SPDK 既有 vbdev 范式
（`vbdev_passthru` / `vbdev_delay`）。

## 4. 详细设计

### 4.1 文件布局（新增独立目录，零改主干）
```
module/bdev/qdlimit/
├── Makefile
├── vbdev_qdlimit.c       # 核心：模块注册、io_channel、IO 流程
├── vbdev_qdlimit.h       # 对外结构与接口
└── vbdev_qdlimit_rpc.c   # RPC: create / delete / set_depth
```
（模块名 `qdlimit` = queue-depth limiter。）通过 `SPDK_BDEV_MODULE_REGISTER` 注册。

### 4.2 数据结构
```c
/* vbdev 节点：每实例一个，创建后基本只读 */
struct vbdev_qdlimit {
    struct spdk_bdev        qd_bdev;      /* 对上暴露的 bdev */
    struct spdk_bdev       *base_bdev;    /* 底层 SSD bdev */
    struct spdk_bdev_desc  *base_desc;
    struct spdk_thread     *thread;       /* 创建/析构所在线程 */
    uint32_t                queue_depth;  /* 每核阈值, 0 = 不限(纯透传) */
    TAILQ_ENTRY(vbdev_qdlimit) link;
};

/* per-core 状态：每 io_channel 一个, 全部仅本核访问 → 无锁 */
struct qdlimit_io_channel {
    struct spdk_io_channel *base_ch;
    uint32_t                outstanding; /* 本核在途计数 */
    uint32_t                max_depth;   /* 创建时从节点拷入 */
    STAILQ_HEAD(, qdlimit_bdev_io) queued_io;  /* 超阈值等待队列(FIFO) */
};

/* per-IO 上下文：放在 bdev_io->driver_ctx 内, 无额外内存分配 */
struct qdlimit_bdev_io {
    struct spdk_io_channel        *ch;
    struct spdk_bdev_io_wait_entry bdev_io_wait;   /* 处理底层 ENOMEM */
    STAILQ_ENTRY(qdlimit_bdev_io)  link;
};
```
`outstanding` / `queued_io` 均在 channel 上、只被本核读写 → 完全无锁，匹配 per-core 近似精度。

### 4.3 IO 流程
```
submit_request(ch, io):
  ├─ RESET / ABORT  ──► 直接透传底层, 不计数、不排队 (管理命令必须放行)
  └─ 数据 IO (READ/WRITE/UNMAP/WRITE_ZEROES/...):
        if (max_depth == 0 || outstanding < max_depth):
              _submit_to_base(io)
        else:
              STAILQ_INSERT_TAIL(queued_io, io)        // 攒住, 等放行

_submit_to_base(io):
  outstanding++
  按 type 转发到底层 spdk_bdev_*_blocks(base_desc, base_ch, ..., cb=_complete)
  (读使用 get_buf_cb, 与 vbdev_delay 一致)
  若底层返回 -ENOMEM:
        outstanding--
        spdk_bdev_queue_io_wait()                      // 与深度队列分离, 稍后重试

_complete(base_io, success, cb_arg):
  设置 orig_io 状态; 释放 base_io
  outstanding--
  if (!STAILQ_EMPTY(queued_io)):                       // 一进一出, 严格 FIFO
        next = STAILQ_REMOVE_HEAD(queued_io)
        _submit_to_base(next)
  spdk_bdev_io_complete(orig_io)
```
- 完成回调由 SPDK 保证在**提交它的同一核**执行 → 计数增减全在本核，无锁安全。
- 放行为"完成一个放一个"，天然限幅、不会递归爆栈。
- 入队对象为 `struct spdk_bdev_io *`（见 2.2），零拷贝。

### 4.4 配置 / RPC（以静态控制为主）
```
bdev_qdlimit_create    {base_bdev_name, name, queue_depth}
bdev_qdlimit_delete    {name}
bdev_qdlimit_set_depth {name, queue_depth}    # 可选: 运行时调参, 经 spdk_for_each_channel 广播
```
- `queue_depth` 语义 = **每核**阈值；全盘上限 ≈ `活跃核数 × queue_depth`（文档/RPC help 明确标注）。
- `queue_depth = 0` = 不限流（纯透传，便于一键禁用）。
- 实现 `write_config_json`，使配置随 `save_config / load_config` 持久化。
- 仿 passthru/delay 的 **association list + examine_config**：底层 bdev 后出现时也能自动建好，
  与 create 的先后顺序无关。

### 4.5 错误处理与边界
- **底层热移除**：注册 `spdk_bdev_event_cb`，收到 `SPDK_BDEV_EVENT_REMOVE` → 注销本 vbdev（仿 passthru）。
- **底层 ENOMEM**：走 `spdk_bdev_queue_io_wait`，与深度队列分离，互不干扰。
- **RESET**：透传底层、不计数；限流队列中"尚未下发"的 IO 不受影响（它们未触达硬件）。
- **ABORT**：v1 转发底层；仍在限流队列、未下发的 IO 不可被 abort——语义无害（未到 SSD），文档注明。
- **channel 销毁 / destruct**：正常摘除前队列应已 drain；销毁时断言 `queued_io` 为空。
- **资源占用**：排队的 bdev_io 占用每 channel bdev_io 内存池槽位，但上游 nvmf 在途有限，
  队列长度被动收敛、不会无界增长——文档标注。
- **性能**：per-IO 上下文复用 `driver_ctx`，**无额外内存分配**；仅多一次 bdev 转发跳，
  `outstanding` 为普通自增，开销可忽略。

## 5. 测试

### 5.1 单元测试（`test/unit/lib/bdev/qdlimit/`，bdev stub）
1. 在途计数封顶于 `queue_depth`，不超出。
2. 超阈值 IO 入队，完成后按 FIFO 顺序放行。
3. `queue_depth = 0` 时纯透传、不限流。
4. `set_depth` 修改后对各核生效。
5. RESET/ABORT 绕过计数直接透传。

### 5.2 集成测试
- bdevperf / fio 叠加 qdlimit 跑在 malloc / nvme bdev 上，
  用 `bdev_get_iostat` 校验在途深度不超过设定阈值。

### 5.3 RPC 测试
- `test/rpc/` 增加 create / delete / set_depth 用例，并验证 `save/load_config` 往返一致。

## 6. 明确不做（YAGNI）
- 不做跨核全盘精确限流（方案 B）——如未来需要，可在 `vbdev_qdlimit` 节点上加跨核原子计数作为扩展点。
- 不做自适应/动态阈值（按延迟反馈自动调节）。
- 不改造现有 bdev QoS。
- 不在 nvmf / bdev / bdev_nvme 主干插桩。

## 7. 迁移性说明
本模块为完全独立的新增目录，仅依赖 `spdk/bdev.h`、`spdk/bdev_module.h` 等稳定公共接口
（与 passthru/delay 同级）。跨 SPDK 版本迁移时，预期仅需跟随公共 bdev 接口的少量签名变化，
不触碰主干逻辑。
