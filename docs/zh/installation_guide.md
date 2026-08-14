# 安装指南<a id="ZH-CN_TOPIC_0000002521561170"></a>

## 环境要求<a id="ZH-CN_TOPIC_0000002520046774"></a>

在安装SPDK之前，需要先安装KAE2.0，安装前请确保使用的环境满足KAE支持的软硬件环境，并且正确安装相应的License，License安装成功后，操作系统才能识别到加速器设备。

**硬件要求<a id="zh-cn_topic_0000001217080138_section10273165810425"></a>**

| 项目    | 规格             |
|-------|----------------|
| CPU型号 | 鲲鹏920处理器       |
| 其它    | iBMC版本：V365及以上 |
| 其它    | BIOS版本：V105及以上 |

**操作系统要求**

| 项目   | 版本                                         |
|------|--------------------------------------------|
| 操作系统 | openEuler 22.03 SP1<br>openEuler 22.03 SP2 |

**软件要求**

| 项目      | 版本        | 获取路径                                                            |
|---------|-----------|-----------------------------------------------------------------|
| KAE     | 2.0.0     | [获取链接](https://gitcode.com/boostkit/KAE/tree/kae2/)             |
| SPDK    | 21.01.1   | [获取链接](https://gitcode.com/boostkit/spdk)                       |
| OpenSSL | 1.1.1a及以上 | [获取链接](https://openssl-library.org/source/old/1.1.1/index.html) |

**License获取<a id="section11769132984820"></a>**

鲲鹏服务器主板K系列硬件加速引擎已默认开启，无需申请License。

License申请和安装操作请参见《[华为服务器 iBMC 许可证 使用指导](https://support.huawei.com/enterprise/zh/management-software/ibmc-pid-8060757?category=operation-maintenance)》。

## 安装KAE<a id="ZH-CN_TOPIC_0000002551566777"></a>

鲲鹏加速引擎KAE是基于鲲鹏920处理器提供的硬件加速解决方案，包含了KAE加解密和KAEzip，KAE加解密模块是基于OpenSSL的，因此在安装和使用KAE加解密模块前请正确安装OpenSSL。

具体的OpenSSL安装步骤请参见[安装OpenSSL](https://www.hikunpeng.com/document/detail/zh/kunpengaccel/kae/kae/docs/zh/installation_guide.md#%E5%AE%89%E8%A3%85openssltongsuo)。

**操作步骤<a id="section1834411526119"></a>**

1. 使用远程登录工具，以root账号进入Linux操作系统命令行界面。
2. 在`/home`目录下拉取KAE2.0代码。

    ```sh
    git clone https://gitcode.com/boostkit/KAE.git -b kae2
    ```

    > ![](public_sys-resources/icon-note.gif) **说明：**
    > 
    > kae2分支适用于openEuler 22.03 LTS SP2，若操作系统为openEuler 22.03 LTS SP1，请使用KAE 2.0.0版本进行安装使用。

3. 进入克隆下来的KAE目录，一键安装所有模块。

    ```sh
    sh build.sh all
    ```

    代码脚本提供一键式安装命令。进入KAE源码包目录，使用`sh build.sh all`命令安装KAE中所有组件内容，由于加解密和解压缩模块都需要安装，所以此处一键安装所有模块，详细安装流程请参见《[源码安装（KAE2.0）](https://www.hikunpeng.com/document/detail/zh/kunpengaccel/kae/kae/docs/zh/installation_guide.md#%E6%96%B9%E5%BC%8F%E4%B8%80%EF%BC%9A%E6%BA%90%E7%A0%81%E5%AE%89%E8%A3%85)》。

**验证KAE是否安装成功<a id="section10160654163615"></a>**

进入KAE目录，执行env-check.sh脚本。

```sh
sh env-check.sh
```

>![](public_sys-resources/icon-note.gif) **说明：** 
> 
>执行build.sh脚本后，即使过程中没有报错，KAE仍然有可能未安装成功，要验证KAE是否安装成功，可以运行KAE目录下的env-check.sh脚本文件，该脚本会检测KAE环境是否正常，如果检测到环境不正常会提示用户出现的问题，反之则KAE安装成功。

## 编译SPDK<a id="ZH-CN_TOPIC_0000002520046780"></a>

正确安装完KAE软件之后，开始编译SPDK。SPDK软件不需要安装，只需要进行编译操作就可以生成可执行文件。

**操作步骤<a id="section1834411526119"></a>**

1. 使用远程登录工具，以root账号进入Linux操作系统命令行界面。
2. 在`/home`目录下拉取SPDK代码。

    ```sh
    git clone https://github.com/spdk/spdk.git
    ```

3. 进入克隆下来的SPDK目录，切换到指定的commitID，下载并合入patch。

    ```sh
    cd spdk
    git checkout 1f0dd58a43b5bc8118b123eca1b07781b052293d
    wget https://gitcode.com/boostkit/spdk/blob/master/spdk-21.01.1-for-KAE.patch
    git apply spdk-21.01.1-for-KAE.patch
    ```
   
4. 加载dpdk、isal等模块。

    ```sh
    git submodule update --init
    ```

5. 安装SPDK所需要的依赖。

    ```sh
    ./scripts/pkgdep.sh
    ```

    >![](public_sys-resources/icon-note.gif) **说明：** 
    >
    >pkgdep.sh中没有适配openEuler系统，需要手动添加命令适配openEuler系统，打开pkgdep.sh，找到如下命令行：
    >
    >```shell
    >if [[ ${ID,,} == *"suse"* ]]; then
    >        ID="sles"
    >fi
    >```
    >
    >随后在命令行下方添加如下命令：
    >
    >```shell
    >if [[ ${ID,,} == *"openeuler"* ]]; then
    >        ID="rhel"
    >fi
    >```

6. 配置编译选项。

    ```sh
    ./configure --with-crypto --with-reduce --with-ksal --with-crypto_openssl
    ```

    >![](public_sys-resources/icon-note.gif) **说明：** 
    >
    > 1. `./configure`是执行配置编译选项功能，如`--with-ksal`为加载ksal模块， `--with-crypto`、`--with-crypto_openssl`为加载加解密模块， `--with-reduce`为加载解压缩模块。详细配置可通过执行`./configure  -h`查看。
    > 2. `--with-reduce`是加载解压缩模块，如果要加载该模块，需要配置环境变量`export CFLAGS="-DZLIB\_MEM\_SIMU\_PMEM"`。
    > 3. `--with-crypto`、`--with-crypto_openssl`是加载加解密模块，如果需要加载该模块，需要合入两个补丁。单击[获取链接](https://gitcode.com/boostkit/spdk/releases/spdk21.01.1-for-KAE)获取补丁`0001-fix-openssl-engine-double-free-bug.patch`、`spdk_v21.01.1_dpdk_compress_kae.patch`放置在`spdk/dpdk`目录下，执行如下命令即可。
    >
    >    ```sh
    >    patch -p1 < 0001-fix-openssl-engine-double-free-bug.patch
    >     patch -p1 < spdk_v21.01.1_dpdk_compress_kae.patch
    >    ```
    >
    > 4. `--with-ksal`是加载ksal算法模块，KSAL是华为自研的高性能crc算法，对外闭源，采取动态库的形式集成，因此需要先[安装KSAL算法包](https://www.hikunpeng.com/document/detail/zh/kunpengaccel/storage/ksal/kunpengksal_16_0007.html)，加载KSAL动态库才能正常配置与编译，执行编译操作，单击[下载rpm包](https://www.hikunpeng.com/boostkit/download?version=24.0.RC5)。

7. <a id="000001"></a>执行编译操作。

    ```sh
    make -j
    ```

## 修订记录
| 发布日期  | 修改说明       |
|-------|----------|
| 2024-09-30 | 第一次正式发布。 |