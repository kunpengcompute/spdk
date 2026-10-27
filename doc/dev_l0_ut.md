# dev_L0 上游差异与 UT 验证

验证日期：2026-10-27。Linux 原生 UT 与增量覆盖率验证已完成。产品代码保持原样，本次修改仅包含测试、测试入口、覆盖率工具和本报告。

## 版本与差异范围

- 目标：`https://gitcode.com/boostkit/spdk.git` 的 `dev_L0`，固定提交 `67de5d5f7ae6fc970faa3a9add67db5e51594e82`。
- 上游：SPDK `v23.01.1`，提交 `186986cf1044eaaf73e1c67ebcf3a7ce2f1376bb`。已通过官方 `https://github.com/spdk/spdk.git` 的 tag 与 peeled tag 核验，并非仅依据 fork 内的版本字符串。
- 原分支相对上游共修改 39 个文件，增加 9032 行、删除 60 行，包含大量文档、已有 UT、集成测试与一份重复记录实现的 patch。
- 本次增量覆盖率统计全部新增/修改的产品 C、Python 和运行脚本，共 15 个文件。新增头文件/结构字段/函数声明没有独立可执行行；文档、测试程序、构建声明和重复 patch 不作为产品代码分母。

## 相对开源 SPDK 的实际修改

### 1. L0 数据内存池与地址注册

涉及 `lib/env_dpdk/{env,l0,l0_mmap_platform,memory}.c`、`lib/rdma/common.c` 和 `lib/nvmf/transport.c`。

- 用 `SPDK_NVMF_L0_ENABLE` 切换 RDMA transport 的共享数据 buffer 池。其他 transport 仍走 DPDK 池；弱符号声明允许使用未实现 L0 接口的其他 env。
- `l0_mmap_platform.c` 打开 `/dev/hisi_l0`，以 2 MiB 对齐地址映射设备，处理映射竞争重试，并通过 `/proc/<pid>/pagemap` 获取物理地址。`SPDK_NVMF_L0_DEVICE` 是默认 fd 获取失败后的备用设备路径。
- `l0.c` 管理单个、最大 64 MiB 的 L0 区域，执行内存注册/注销、区域查找、虚拟到物理地址换算及资源清理。
- `env.c` 将原先直接返回的 `rte_mempool *` 改为带 backend 的 SPDK 包装对象；DPDK 与 L0 共用 get/put/bulk/count/lookup/迭代接口。L0 实现采用受互斥锁保护的 free list，回调收到的是 SPDK 包装对象。
- `memory.c` 在普通 DPDK 地址转换或 fd 查找失败时回退到 L0。`rdma/common.c` 按 L0 region 缓存外部 MR，在最后一个 map 引用释放时注销 MR，并复用抽取后的访问权限计算。

### 2. 每 SSD、每 poll group 的 qdlimit

涉及 `lib/nvmf/qdlimit.c`、`rdma.c`、`nvmf_rpc.c`、公共 request/poll group 字段及 Python/CLI。

- 在 RDMA 请求获取共享数据 buffer 之前按底层 bdev 准入。达到 depth 后移出共享待 buffer 队列，停放在对应 SSD 的 FIFO 队列中，避免阻塞其他 SSD。
- 完成、释放 buffer、abort、断连和 poll group 销毁时归还 slot 或移除/排空等待请求，避免重复计费。
- depth 为 0 表示无限制。限制是每 SSD、每 poll group 的深度；不能把它理解为整个 target 的统一全局深度。
- 新增 `nvmf_qdlimit_set_depth`、`nvmf_qdlimit_get_depth`、`nvmf_qdlimit_get_stats` 三个 RPC，以及对应 Python/CLI。统计聚合已实际访问该 SSD 的 poll group 数和 inflight；跨线程 inflight 统计本身是近似快照。

### 3. RDMA/bdev 延迟统计与重置

涉及 `lib/nvmf/{rdma,ctrlr_bdev,nvmf,nvmf_rpc}.c`、transport 回调和 `scripts/nvmf_rdma_latency.py`。

- 记录请求生命周期、RDMA 数据传输、bdev 处理阶段的次数、总 ticks、最小值和最大值；在数据 WR 完成路径及普通/zero-copy bdev 完成路径更新。
- 新增 transport 的 bdev 计时 hook、统计清零 hook，以及跨 channel 的 `nvmf_reset_stats` RPC/CLI。
- latency 脚本支持 RPC、文件和标准输入，聚合多个 poll group 并换算微秒。另一个新增 shell 脚本用于启动 CPU 16–39 的后台带宽负载。

## UT 与实测结果

复用 SPDK 的 CUnit、`DEFINE_STUB`/`MOCK_SET`、直接包含生产 `.c` 的测试模式和 `mk/spdk.unittest.mk`；保留真实 RPC 注册构造器并验证新增注册，JSON 解析/序列化使用真实 SPDK 实现。

新增 env 和 RPC UT，扩展已有 qdlimit、RDMA、ctrlr_bdev、transport、nvmf 和 RDMA common UT，并统一接入 `test/unit/unittest.sh`。另外补充 Python 标准 unittest 和隔离外部命令的 shell UT。

Linux 最终入口实测：**12 个 CUnit 二进制，其中弱符号测试分别在缺失后端和动态加载后端时运行，共 73 个测试、1890 条断言全部通过；17 项 Python 和 1 项 shell 测试全部通过，总计 91 项。**

固定上游差异与 GCC/gcov/LCOV 执行行求交，结果为 **1095 / 1095，100% 新增可执行行覆盖率**，15 个产品源码文件均有覆盖率记录，未覆盖行数为 0。已将远端原始数据取回本机，再次通过同一门禁校验。

Linux ELF 补充 UT 保持可选 L0 函数为真正的未定义弱符号，并通过 `LD_PRELOAD` 加载纯测试后端，覆盖运行时是否提供接口的判断。fixture 不分配 L0 内存、不访问设备。Python 覆盖率导出已兼容 3.11 的模块合成第 0 行；该行不存在于源码，门禁仍拒绝非法 LCOV 行号。

| 产品源码 | 命中 / 新增可执行行 |
| --- | ---: |
| `lib/env_dpdk/env.c` | 179 / 179 |
| `lib/env_dpdk/l0.c` | 125 / 125 |
| `lib/env_dpdk/l0_mmap_platform.c` | 56 / 56 |
| `lib/env_dpdk/memory.c` | 12 / 12 |
| `lib/nvmf/ctrlr_bdev.c` | 28 / 28 |
| `lib/nvmf/nvmf.c` | 9 / 9 |
| `lib/nvmf/nvmf_rpc.c` | 97 / 97 |
| `lib/nvmf/qdlimit.c` | 188 / 188 |
| `lib/nvmf/rdma.c` | 166 / 166 |
| `lib/nvmf/transport.c` | 11 / 11 |
| `lib/rdma/common.c` | 46 / 46 |
| `python/spdk/rpc/nvmf.py` | 15 / 15 |
| `scripts/nvmf_rdma_latency.py` | 134 / 134 |
| `scripts/rpc.py` | 25 / 25 |
| `scripts/l0_precise_flow_control/start_background_job.sh` | 4 / 4 |

覆盖率门禁 `test/unit/scripts/check_dev_l0_coverage.py` 会对缺失文件数据、无新增产品代码、源码与指定提交不一致、任意新增执行行未命中返回失败。已验证缺少文件数据时会失败。它不把无数据文件按 0/0 计为通过。

这里的 100% 是增量行覆盖率，并非整个文件、分支或所有构建配置的覆盖率。此前 macOS 的 89 项测试及 LLVM 1456/1456 结果作为补充记录保留；两种编译器的执行行归属不同，Linux 验收以本表 GCC 数据为准。

## 执行环境与验证边界

本次完整实测在 openEuler 24.03 LTS-SP3、Linux aarch64、GCC/gcov 12.3.1、LCOV 1.16、Python 3.11.6 下执行。CUnit 为 3.5.5-cunity，源码提交 `67cee3f06e4bed117738ae4acf0b98f711a621ad`；LCOV 源码提交 `dfeb7505ef372f806ddb280b52a90f41cd8169cd`。RDMA、UUID、OpenSSL 等开发依赖来自 openEuler 官方仓库，逐包校验校验和后解包到专用用户前缀。系统软件包数据库与共享私有配置均未修改。

本次使用单测专用配置：以完整上游 `CONFIG` 为模板，通过 `scripts/detect_cc.sh` 生成真实工具链配置，使用原生 Makefile 编译 log、util、json、jsonrpc、rpc、trace、thread、ut_mock 及 UT。空外部 env 提供单测的环境边界；完整 env_dpdk 与 NVMe-oF 产品 `.c` 仍由 UT 直接包含并编译，包含 Linux 条件代码。验收范围为原生单元测试，不包含完整 `spdk_tgt`/DPDK 驱动构建或设备集成测试。

`test/unit/scripts/native/` 仅用于 macOS 补充验证：用 pthread mutex 提供 spinlock 类型兼容、局部 calloc adapter 替代 Darwin ld 不支持的 GNU `--wrap`、为无法链接的 Linux 外部依赖提供 stub。标准 Linux Makefile 不使用这些 native adapter。测试始终执行仓库中的完整生产源码，没有复制/摘取实现。

DPDK、verbs、设备和系统调用在依赖边界被 mock；非法 backend/role 的不可达陷阱用 longjmp 观测，未验证真实进程 abort 信号。此次结果不验证真实设备 mmap、DMA 一致性或 RDMA 硬件交互，也不要求旧 VFIO 逻辑全覆盖。

实际验证目录为当前账号主目录下的 `kvc-spdk-dev-l0-67de5d5f7-ut`，这是用户在确认 `/home/wrh` 不存在且账号无权创建后明确选择的目录。所有远端检查、同步、编译、测试和取日志均由 `taskset` 约束到用户当前预约的 CPU **240–287**；编译并行度为 4，实际 affinity 已逐次核验。

### 当前环境的真实 L0 支持状态

- 默认设备 `/dev/hisi_l0` 不存在。
- 文档中的另一个设备 `/dev/hisi_soc_cache_mgmt` 存在，确为字符设备，但权限为 `root:root 0600`，当前账号无读写权限。
- 当前进程有效 capability 为 0；未发现名含 L0/cache_mgmt 的已加载模块。设备存在不能据此断言驱动必须以模块形式加载，也不能仅凭模块列表判断硬件不支持。
- **当前账号不能执行真实 L0 映射/I/O 验证。** 这不等同于物理服务器不支持 L0。真实验证仍需管理员提供设备访问和相应物理页映射权限；此次已经完成的是不依赖设备的 L0 UT。

## 复现

在 Linux 专用目录中保持固定提交一致、应用补丁，准备含 CUnit/RDMA/UUID/OpenSSL 开发文件的用户依赖前缀后：

```bash
DEV_L0_DEP_PREFIX=/path/to/dependency-prefix \
DEV_L0_JOBS=4 \
taskset -c 240-287 bash test/unit/scripts/build_dev_l0_linux.sh
```

示例绑定本次获准的 CPU 240–287；其他预约需替换为其授权范围。`build_dev_l0_linux.sh` 从模板生成单测配置并构建框架依赖，再调用 `test/unit/run_dev_l0.sh`。已有构建配置的专用 checkout 需设置 `DEV_L0_RECONFIGURE=1` 才会重建配置。依赖已经构建时，可以直接调用后者。入口运行 12 个二进制及动态后端模式，合并零计数 baseline 与运行 LCOV，再执行 Python/shell 测试和 100% 增量门禁。输出默认在 `build/coverage/dev_l0/`。运行会清零当前 checkout 的 unit test `.gcda`，应使用专用验证目录。

本次远端目录内保留了 `deps/env.sh`，加载后即可获得该用户依赖前缀和 LCOV 路径。交付产物的 `linux/` 中保留归一化路径后的完整 LCOV、零计数 baseline、逐套日志、原始 `.gcno/.gcda`、构建配置及依赖校验和清单。

本次 Mac 实测的可复现入口（CUnit 安装 prefix 与 rdma-core 源码目录通过环境变量提供）：

```bash
CUNIT_PREFIX=/path/to/cunit-install \
RDMA_CORE_SOURCE=/path/to/rdma-core \
DEV_L0_OUTPUT_DIR=/path/to/coverage-output \
bash test/unit/scripts/run_dev_l0_native.sh
```

该入口会从提供的官方头文件生成独立兼容 include 目录，完整编译/运行 11 套 CUnit，再执行 Python/shell UT 和增量门禁，不依赖之前的临时生成文件。`summary.json` 含基线/目标提交、逐文件 SHA-256、命中数和未覆盖行；`*.info`、`*.profraw`、`*.profdata` 和逐套日志可复查。

只校验已有数据时：

```bash
python3 test/unit/scripts/check_dev_l0_coverage.py --json summary.json coverage.info
```

若 LCOV 来自另一 checkout，增加 `--source-root /absolute/source/path/recorded/in/lcov`。该参数只映射路径，不改变提交、源码一致性或覆盖率要求。
