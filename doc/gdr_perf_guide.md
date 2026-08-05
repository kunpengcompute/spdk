# SPDK NVMe-oF GDR 压测指导

本文说明如何使用 `examples/nvme/perf` 对普通 Host Memory 和 GPU
GPUDirect RDMA（GDR）两条数据路径进行对照压测。

## 1. 测试范围

普通模式的数据路径：

```text
远端 SSD -> SPDK NVMe-oF Target -> RDMA -> Initiator Host Memory
```

GDR 模式的数据路径：

```text
远端 SSD -> SPDK NVMe-oF Target -> RDMA -> Initiator GPU Memory
```

Target 端仍然处理标准 NVMe-oF/RDMA 请求，不需要修改 Target 代码。
本次修改和重新编译只需要部署到 Initiator。为了保证两组结果可比较，
Host 和 GDR 测试必须使用相同的 Target、namespace、网络、I/O size、QD、
CPU core 和运行时间。

当前 GDR 压测模式只支持：

- NVMe-oF/RDMA transport；
- 不带 metadata/DIF 的 namespace；
- CUDA device memory 数据缓冲区。

不支持 NVMe/TCP、本地 PCIe、AIO、io_uring 或 metadata/DIF 组合。

## 2. 参数说明

基础命令：

```bash
sudo ./build/examples/perf \
  -q 128 -o 131072 -w randread -t 60 -a 10 -c 0x2 \
  -r 'trtype:RDMA adrfam:IPv4 traddr:<TARGET_IP> trsvcid:4420 subnqn:<NQN>'
```

参数含义：

| 参数 | 含义 |
| --- | --- |
| `-q 128` | Queue Depth，每个 namespace/worker 保持最多 128 个未完成 I/O |
| `-o 131072` | 单个 I/O 为 131072 字节，即 128 KiB |
| `-w randread` | 随机读；还可使用 `read`、`write`、`randwrite`、`rw`、`randrw` |
| `-t 60` | 正式统计 60 秒 |
| `-a 10` | 预热 10 秒，预热阶段不计入最终结果 |
| `-c 0x2` | 使用 CPU Core 1 轮询和提交 I/O |
| `trtype:RDMA` | 使用 NVMe-oF/RDMA transport |
| `traddr` | Target RDMA listener 所在网卡的 IP 地址 |
| `trsvcid` | Target NVMe-oF listener 端口 |
| `subnqn` | Target 导出的 NVMe-oF subsystem NQN |
| `--gdr` | 使用 GPU 显存作为 I/O payload |
| `--gpu-id 0` | 使用 CUDA Device 0 |

CPU mask 示例：

```text
0x1 = Core 0
0x2 = Core 1
0x4 = Core 2
0x6 = Core 1 和 Core 2
```

GDR 测试建议始终设置 warmup。CUDA allocation、DMA-BUF 导出和首次 RDMA
MR 注册发生在初始队列填充阶段，warmup 可以避免这些一次性开销影响正式结果。

## 3. Initiator 环境检查

检查 GPU、GPU/NIC 拓扑和 RDMA 设备：

```bash
nvidia-smi
nvidia-smi topo -m
ibv_devices
ibv_devinfo
```

检查 CUDA Driver API 和 DMA-BUF MR 接口：

```bash
test -f /usr/local/cuda/include/cuda.h
ldconfig -p | grep libcuda
grep ibv_reg_dmabuf_mr /usr/include/infiniband/verbs.h
```

在 Ubuntu 上安装常用 SPDK/RDMA 构建依赖：

```bash
cd /opt/spdk/spdk
sudo ./scripts/pkgdep.sh
sudo apt-get install -y meson ninja-build pkg-config libibverbs-dev librdmacm-dev
```

CUDA Toolkit 和 NVIDIA driver 需要根据 GPU 型号和操作系统单独安装。

## 4. Initiator 编译

```bash
cd /opt/spdk/spdk

./configure \
  --with-rdma \
  --with-cuda=/usr/local/cuda

make clean
make -j$(nproc)
```

确认配置：

```bash
grep -E 'CONFIG_(RDMA|CUDA)' mk/config.mk
./build/examples/perf --help | grep -E 'gdr|gpu-id'
```

预期配置包含：

```text
CONFIG_RDMA?=y
CONFIG_CUDA?=y
```

## 5. Target 配置示例

如果 Target 已经正常导出 NVMe-oF/RDMA namespace，可以跳过本节。Target
不需要使用 `--with-cuda`，也不需要部署 GDR 版 `perf`。

下面以这些参数为例：

```text
Target RDMA IP: 192.168.100.8
Target SSD BDF: 0000:81:00.0
Port:           4420
NQN:            nqn.2016-06.io.spdk:gdrtest
```

准备 hugepage 和 SSD：

```bash
cd /opt/spdk/spdk
sudo HUGEMEM=4096 scripts/setup.sh
sudo ./build/bin/nvmf_tgt -m 0x3
```

在 Target 的另一个终端配置 NVMe-oF：

```bash
cd /opt/spdk/spdk

sudo ./scripts/rpc.py nvmf_create_transport \
  -t RDMA -u 8192 -i 131072 -c 8192

sudo ./scripts/rpc.py bdev_nvme_attach_controller \
  -b Nvme0 -t PCIe -a 0000:81:00.0

sudo ./scripts/rpc.py nvmf_create_subsystem \
  nqn.2016-06.io.spdk:gdrtest \
  -a -s SPDKGDR000000001

sudo ./scripts/rpc.py nvmf_subsystem_add_ns \
  nqn.2016-06.io.spdk:gdrtest Nvme0n1

sudo ./scripts/rpc.py nvmf_subsystem_add_listener \
  nqn.2016-06.io.spdk:gdrtest \
  -t rdma -a 192.168.100.8 -s 4420

sudo ./scripts/rpc.py nvmf_get_subsystems
```

请根据实际环境替换 SSD BDF、RDMA IP、NQN 和 CPU mask。

## 6. Host Memory 基线

在 Initiator 上执行：

```bash
cd /opt/spdk/spdk

sudo ./build/examples/perf \
  -q 128 \
  -o 131072 \
  -w randread \
  -t 60 \
  -a 10 \
  -c 0x2 \
  -L \
  -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:gdrtest'
```

记录输出中的 IOPS、MiB/s、平均延迟、最小/最大延迟及延迟百分位。

## 7. GPU GDR 随机读

保持所有基础参数不变，只增加 `--gdr --gpu-id 0`：

```bash
sudo ./build/examples/perf \
  -q 128 \
  -o 131072 \
  -w randread \
  -t 60 \
  -a 10 \
  -c 0x2 \
  -L \
  --gdr \
  --gpu-id 0 \
  -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:gdrtest'
```

启动成功时应看到：

```text
GDR mode enabled on CUDA device 0
Initialization complete. Launching workers.
```

随机读的数据方向为：

```text
SSD -> Target -> RDMA WRITE -> Initiator GPU Memory
```

## 8. 写压测

警告：`write`、`randwrite`、`rw` 和 `randrw` 会覆盖远端 namespace 中的数据。

Host Memory 随机写：

```bash
sudo ./build/examples/perf \
  -q 128 -o 131072 -w randwrite -t 60 -a 10 -c 0x2 -L \
  -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:gdrtest'
```

GPU GDR 随机写：

```bash
sudo ./build/examples/perf \
  -q 128 -o 131072 -w randwrite -t 60 -a 10 -c 0x2 -L \
  --gdr --gpu-id 0 \
  -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.100.8 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:gdrtest'
```

随机写的数据方向为：

```text
Initiator GPU Memory -> RDMA READ -> Target -> SSD
```

## 9. 推荐测试矩阵

至少测试下面的组合，并对每组分别运行 Host 和 GDR：

| 项目 | 建议值 |
| --- | --- |
| I/O size | 4 KiB、16 KiB、64 KiB、128 KiB、1 MiB |
| Queue Depth | 1、8、32、128 |
| Workload | `randread`、`randwrite` |
| 正式时间 | 60 秒或更长 |
| Warmup | 10 秒或更长 |

为了减少误差：

- 每组至少运行三次；
- Host/GDR 使用完全相同的参数；
- 固定 CPU、GPU、NIC、Target namespace 和网络路径；
- CPU core 尽量选择与 GPU/RDMA NIC 位于同一 NUMA node 的空闲 core；
- 同时记录 Initiator 和 Target 的 CPU 占用、NIC 流量以及 GPU/NIC 拓扑。

## 10. 常见错误

### `--gdr requires a build configured with --with-rdma --with-cuda`

重新配置并完整编译 Initiator：

```bash
./configure --with-rdma --with-cuda=/usr/local/cuda
make clean
make -j$(nproc)
```

### `cuMemGetHandleForAddressRange() failed`

检查 CUDA driver、CUDA Toolkit 版本、GPU DMA-BUF 支持和 NVIDIA kernel
module。确认运行 `perf` 的进程能够正常访问指定 GPU。

### `ibv_reg_dmabuf_mr() failed`

检查：

- rdma-core 和 NIC driver 是否支持 DMA-BUF MR；
- GPU 与 RDMA NIC 是否允许 peer DMA；
- `ibv_reg_dmabuf_mr` 是否存在于 verbs headers/library；
- GPU、NIC 和 CPU NUMA 拓扑是否合理。

### `GDR requires an NVMe-oF/RDMA controller memory domain`

确认 `-r` 使用 `trtype:RDMA`，Target listener 也使用 RDMA，而不是 TCP 或
本地 PCIe transport。

### GDR 与 Host 结果几乎相同

这不一定代表 GDR 没有生效。端到端结果可能被远端 SSD、Target CPU 或网络
带宽限制。可以提高 Target 后端性能或使用更大的 I/O/QD，进一步判断瓶颈。
