# SPDK NVMf RDMA L0 使能说明

本文说明如何在带 L0 patch 的 SPDK 中启用 L0，并让 `spdk_tgt` 的 NVMf RDMA target data buffer pool 使用 L0 内存。

当前 patch 的作用范围是 NVMf RDMA target 侧的共享 data buffer pool。普通 RDMA transport 未启用 L0 时仍走原来的 DPDK mempool；TCP、PCIe 等 transport 不会因为该开关自动使用 L0。

## 前置条件

以下命令通常需要 root 权限执行。

### 1. 配置 BIOS Cache Mode

使用 L0 前需要先进入 BIOS 配置 Cache Mode，否则无法按一个 CPU 的 L3 cache 整体申请 L0 资源。

BIOS 路径：

```text
Advanced -> Performance Config -> Cache Mode
```

将 Cache Mode 设置为：

```text
in:share out:share
```

保存 BIOS 配置后重启系统，再继续执行后续 L0 内核、驱动和 io stash 配置。

### 2. 启用 L0

安装指定内核：

```bash
rpm -ivh kernel-5.10.0_l0_mwp+-80.aarch64.rpm
```

安装完成后建议重启并确认已经进入该 L0 内核：

```bash
reboot
uname -r
```

加载 L0 驱动：

```bash
modprobe hisi_l0
```

默认 L0 设备路径是 `/dev/hisi_l0`。可以用下面命令检查设备是否存在：

```bash
ls -l /dev/hisi_l0
```

patch 中 `lib/env_dpdk/l0_mmap_platform.c` 定义了默认设备：

```c
#define L0_DEV "/dev/hisi_l0"
```

因此如果系统使用默认路径，SPDK 编译前不需要额外设置 L0 设备环境变量。

如果设备路径不是 `/dev/hisi_l0`，可以在启动 `spdk_tgt` 前设置：

```bash
export SPDK_NVMF_L0_DEVICE=/path/to/l0_device
```

### 3. 启用 io stash

拉取代码仓：

```bash
git clone https://gitcode.com/openeuler/cache_tuner.git
```

进入 `cache_stash` 模块目录并编译。如果 clone 后仓库目录名为 `cache_tuner`，路径通常是 `cache_tuner/cache_stash`：

```bash
cd cache_tuner/cache_stash
make -j
```

加载模块：

```bash
insmod cache_stash.ko
```

打开 LLC stash：

```bash
echo 1 > /sys/kernel/cache_stash/llc_enable
```

检查是否启用成功。打印 `1` 表示使能成功：

```bash
cat /sys/kernel/cache_stash/llc_enable
```

## 编译 SPDK

L0 patch 已经把 L0 相关源码加入 SPDK env dpdk 编译路径，例如 `lib/env_dpdk/Makefile` 中包含：

```make
C_SRCS = env.c memory.c pci.c init.c threads.c l0.c l0_mmap_platform.c
```

所以编译前不需要设置 `SPDK_NVMF_L0_ENABLE`。这个变量是 `spdk_tgt` 启动时读取的运行时开关。

如果要跑 NVMf RDMA target，SPDK 仍然需要按 RDMA 方式编译，例如：

```bash
./configure --with-rdma
make -j
```

## 推荐启用策略

建议优先只开启 io stash，不设置 `SPDK_NVMF_L0_ENABLE`。此时 NVMf RDMA target 仍使用默认 DPDK mempool，但可以先验证 io stash 对带宽吸收和缓存命中的改善。

如果只开启 io stash 后，内存带宽吸收仍然不够，再在启动 `spdk_tgt` 前开启 L0：

```bash
export SPDK_NVMF_L0_ENABLE=1
```

也就是说，L0 是第二阶段增强开关，不建议在没有带宽瓶颈判断前默认打开。

## 启动 spdk_tgt 时按需启用 L0

需要启用 L0 时，在启动 `spdk_tgt` 前设置：

```bash
export SPDK_NVMF_L0_ENABLE=1
```

支持的真值包括 `1`、`y`、`yes`、`true`、`on`。未设置或设置为其他值时，不启用 L0。

示例：

```bash
export SPDK_NVMF_L0_ENABLE=1
./build/bin/spdk_tgt
```

如果 L0 设备不是默认的 `/dev/hisi_l0`：

```bash
export SPDK_NVMF_L0_ENABLE=1
export SPDK_NVMF_L0_DEVICE=/path/to/l0_device
./build/bin/spdk_tgt
```

## 创建 NVMf RDMA transport

`spdk_tgt` 启动后，按原有方式创建 RDMA transport、subsystem、namespace 和 listener。例如：

```bash
scripts/rpc.py nvmf_create_transport -t RDMA
```

只要 `SPDK_NVMF_L0_ENABLE=1` 已在 `spdk_tgt` 启动前设置，创建 RDMA transport 时，target 的 `data_buf_pool` 会优先使用 L0-backed mempool。

如果创建 transport 时显式调整 `num_shared_buffers`、`io_unit_size` 或 `max_io_size`，需要注意当前 patch 的单个 L0 region 上限为 64MiB。实际使用时建议给对齐和元数据留一些余量，`io_unit_size` 参数 `-u` 与 `num_shared_buffers` 参数 `-n` 的乘积不要超过 60MiB：

```text
-u * -n <= 60 * 1024 * 1024
```

例如 `-u 131072 -n 480` 的乘积为 60MiB。

注意：环境变量必须在创建 transport 之前设置。对已经创建好的 transport，再设置环境变量不会 retroactively 替换已有 buffer pool。

### L0 模式参考 RPC 配置

开启 L0 后，`spdk_tgt` 的 RPC 配置可以参考如下命令。第一行 `nvmf_create_transport` 中需要特别注意 `-u` 和 `-n` 的乘积不要超过 60MiB。

```bash
./scripts/rpc.py nvmf_create_transport -t RDMA -q 128 -m 127 -c 4096 -i 131072 -u 131072 -a 128 -n 480 -b 32

./scripts/rpc.py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:5d:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme1 -t PCIe -a 0000:5e:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme2 -t PCIe -a 0000:5f:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme3 -t PCIe -a 0000:60:00.0

./scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 8
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme0n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme1n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme2n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme3n1

./scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t RDMA -a <ip1> -s <port1>
./scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t RDMA -a <ip2> -s <port2>
```

## patch 生效路径

### 运行时开关

`lib/env_dpdk/l0.c` 中：

```c
bool
spdk_l0_data_pool_enabled(void)
{
    return l0_env_flag_enabled("SPDK_NVMF_L0_ENABLE");
}
```

这个函数读取 `SPDK_NVMF_L0_ENABLE`，决定 L0 data pool 是否启用。

### RDMA transport 使用 L0 mempool

`lib/nvmf/transport.c` 中，创建 transport data buffer pool 时有如下判断：

```c
if (strcasecmp(transport_name, "RDMA") == 0 && nvmf_transport_l0_enabled()) {
    transport->data_buf_pool = spdk_l0_mempool_create(...);
} else {
    transport->data_buf_pool = spdk_mempool_create(...);
}
```

因此只有 RDMA transport 且 `SPDK_NVMF_L0_ENABLE` 为真时，才会走 L0-backed mempool。

### L0 内存映射和注册

`spdk_l0_mempool_create()` 会创建 L0 region，并将该 region 切成 mempool object：

```c
region = spdk_l0_region_create(name, aligned_total_len);
```

`spdk_l0_region_create()` 会：

1. 打开 L0 设备，默认 `/dev/hisi_l0`。
2. 通过 `mmap_alloc()` 映射 L0 内存，地址按 2MB 对齐。
3. 通过 `vtop()` 获取物理地址。
4. 调用 `spdk_mem_register(region->vaddr, region->len)` 注册到 SPDK memory map。

### RDMA MR 注册

RDMA 侧在 `lib/rdma/common.c` 中支持 L0 region：

```c
if (!spdk_l0_find_region(address, &region, &offset)) {
    SPDK_ERRLOG("No translation for ptr %p, size %zu\n", address, length);
    return -EINVAL;
}

mr = rdma_get_external_mr(map, region);
```

当普通 SPDK mem_map 查不到 translation 时，如果地址属于 L0 region，就为该 L0 region 创建并缓存 RDMA MR。这样 NVMf RDMA target 对 L0 data buffer 进行 RDMA 访问时可以拿到正确的 lkey/rkey。

## 常见检查点

1. 确认内核版本：

```bash
uname -r
```

2. 确认 L0 设备存在：

```bash
ls -l /dev/hisi_l0
```

3. 确认 io stash 已启用：

```bash
cat /sys/kernel/cache_stash/llc_enable
```

期望输出：

```text
1
```

4. 确认 `spdk_tgt` 启动前已经设置 L0 开关：

```bash
echo $SPDK_NVMF_L0_ENABLE
```

期望输出：

```text
1
```

5. 启动日志中应能看到类似 L0 region 分配日志：

```text
Allocated L0 region <name> vaddr=<addr> phys=<phys> len=<len>
```

如果 RDMA transport 创建失败，并出现 `Unable to allocate L0-backed buffer pool for transport RDMA`，优先检查 `/dev/hisi_l0`、`SPDK_NVMF_L0_DEVICE`、L0 region 大小限制以及 io stash/L0 驱动是否已经就绪。

## 当前 patch 限制

1. 当前实现只为 NVMf RDMA transport 的 shared data buffer pool 切换到 L0。
2. `spdk_l0_region_create()` 当前限制单个 L0 region 最大为 64MiB。
3. 当前 patch 只支持一个 L0 region；重复创建 L0 region 会返回 busy。
4. `SPDK_NVMF_L0_ENABLE` 是运行时开关，必须在 `spdk_tgt` 启动并创建 RDMA transport 前设置。
5. 后端 bdev、NVMe 盘、普通 SPDK hugepage 初始化仍按原 SPDK 流程配置；L0 仅替换该 patch 覆盖到的 NVMf RDMA data buffer pool。
