# Installation Guide<a id="EN-US_TOPIC_0000002521561170"></a>

## Environment Requirements<a id="EN-US_TOPIC_0000002520046774"></a>

Install KAE 2.0 before installing SPDK. Ensure that the environment satisfies the software and hardware requirements of KAE and a license is correctly installed. The OS can identify the accelerator devices only after the license is installed.

**Hardware<a id="en-us_topic_0000001217080138_section10273165810425"></a>**

| Project   | Specifications            |
|-------|----------------|
| CPU model| Kunpeng 920      |
| Others   | iBMC: V365 or later|
| Others   | BIOS: V105 or later|

**OS**

| Project  | Version                                        |
|------|--------------------------------------------|
| OS| openEuler 22.03 SP1<br>openEuler 22.03 SP2 |

**Software**

| Project     | Version       | How to Obtain                                                           |
|---------|-----------|-----------------------------------------------------------------|
| KAE     | 2.0.0     | [Link](https://gitcode.com/boostkit/KAE/tree/kae2/)            |
| SPDK    | 21.01.1   | [Link](https://gitcode.com/boostkit/spdk)                      |
| OpenSSL | 1.1.1a or later| [Link](https://openssl-library.org/source/old/1.1.1/index.html)|

**Obtaining the License <a id="section11769132984820"></a>**

The hardware acceleration engine of Kunpeng K series server motherboards is enabled by default. You do not need to apply for a license.

For details about how to apply for and install a license, see [Huawei Server iBMC License User Guide](https://support.huawei.com/enterprise/en/management-software/ibmc-pid-8060757?category=operation-maintenance).

## Installing KAE<a id="EN-US_TOPIC_0000002551566777"></a>

Kunpeng Accelerator Engine (KAE) is a hardware acceleration solution provided on the Kunpeng 920 processor. It consists of KAE encryption/decryption and KAEzip. The KAE encryption/decryption module is based on OpenSSL. Therefore, install OpenSSL before installing and using this module.

For details about how to install OpenSSL, see [Installing OpenSSL](https://www.hikunpeng.com/document/detail/en/kunpengaccel/kae/usermanual/kunpengaccel_06_0009.html).

**Procedure<a id="section1834411526119"></a>**

1. Use a remote login tool to log in to the Linux CLI as the **root** user.
2. Obtain the KAE 2.0 code from the `/home` directory.

    ```sh
    git clone https://gitcode.com/boostkit/KAE.git -b kae2
    ```

    > ![](public_sys-resources/icon-note.gif) **Note:**
    > 
    > The kae2 branch applies to openEuler 22.03 LTS SP2. If openEuler 22.03 LTS SP1 is used, use KAE 2.0.0.

3. Go to the pulled KAE directory and install all modules.

    ```sh
    sh build.sh all
    ```

    This script provides commands for one-click installation. Go to the KAE source package directory and run the `sh build.sh all ` command to install all KAE components. Both the encryption/decryption and decompression modules need to be installed. For details about the installation process, see [Source Code Installation (KAE 2.0)](https://www.hikunpeng.com/document/detail/en/kunpengaccel/kae/usermanual/kunpengaccel_06_0012.html).

**Verifying KAE Installation<a id="section10160654163615"></a>**

Go to the KAE directory and run the **env-check.sh** script.

```sh
sh env-check.sh
```

>![](public_sys-resources/icon-note.gif) **Note:**
> 
>After the **build.sh** script is executed, KAE may still fail to be installed even if no error is reported during the process. To verify whether KAE is successfully installed, run the **env-check.sh** script in the KAE directory. This script checks whether the KAE environment is normal. If the environment is abnormal, the script displays the problem. If the environment is normal, KAE is successfully installed.

## Compiling SPDK<a id="EN-US_TOPIC_0000002520046780"></a>

After KAE is installed, compile SPDK. You do not need to install SPDK. Just compile it to generate an executable file.

**Procedure<a id="section1834411526119"></a>**

1. Use a remote login tool to log in to the Linux CLI as the **root** user.
2. Obtain the SPDK code from the `/home` directory.

    ```sh
    git clone https://github.com/spdk/spdk.git
    ```

3. Go to the cloned SPDK directory, switch to the specified commit ID, and download and integrate the patch.

    ```sh
    cd spdk
    git checkout 1f0dd58a43b5bc8118b123eca1b07781b052293d
    wget https://gitcode.com/boostkit/spdk/blob/master/spdk-21.01.1-for-KAE.patch
    git apply spdk-21.01.1-for-KAE.patch
    ```
   
4. Load the DPDK and isal modules.

    ```sh
    git submodule update --init
    ```

5. Install the SPDK dependency.

    ```sh
    ./scripts/pkgdep.sh
    ```

    >![](public_sys-resources/icon-note.gif) **Note:**
    >
    >The **pkgdep.sh** script does not adapt to the openEuler system. You need to manually add commands to adapt to the openEuler system. Open the **pkgdep.sh** script and find the following commands:
    >
    >```shell
    >if [[ ${ID,,} == *"suse"* ]]; then
    >        ID="sles"
    >fi
    >```
    >
    >Add the following commands at the bottom of the CLI:
    >
    >```shell
    >if [[ ${ID,,} == *"openeuler"* ]]; then
    >        ID="rhel"
    >fi
    >```

6. Configure compilation options.

    ```sh
    ./configure --with-crypto --with-reduce --with-ksal --with-crypto_openssl
    ```

    >![](public_sys-resources/icon-note.gif) **Note:**
    >
    > 1. The `./configure` command is used for configuration and compilation. For example, `--with-ksal` is used to load the KSAL module, `--with-crypto` and `--with-crypto_openssl` are used to load the encryption/decryption module, and `--with-reduce` is used to load the decompression module. You can run the `./configure -h` command to view the detailed configuration.
    > 2. `--with-reduce` is used to load the decompression module. To load this module, you need to configure the environment variable `export CFLAGS="-DZLIB\_MEM\_SIMU\_PMEM"`.
    > 3. `--with-crypto` and `--with-crypto_openssl` are used to load the encryption/decryption module. To load this module, you need to integrate two patches. [Obtain patches](https://gitcode.com/boostkit/spdk/releases/spdk21.01.1-for-KAE) `0001-fix-openssl-engine-double-free-bug.patch` and `spdk_v21.01.1_dpdk_compress_kae.patch`. Place them in the `spdk/dpdk` directory and run the following commands:
    >
    >    ```sh
    >    patch -p1 < 0001-fix-openssl-engine-double-free-bug.patch
    >     patch -p1 < spdk_v21.01.1_dpdk_compress_kae.patch
    >    ```
    >
    > 4. The `--with-ksal` command is used to load the KSAL algorithm module. KSAL is a Huawei-developed high-performance CRC algorithm and is closed source externally. It is integrated as a dynamic library. Therefore, you need to [install the KSAL algorithm package](https://www.hikunpeng.com/document/detail/en/kunpengsdss/basicAccelFeatures/ksal/kunpengksal_16_0007.html) and load the KSAL dynamic library before configuration and compilation. To perform the compilation, you need to [download the RPM package](https://kunpeng-repo.obs.cn-north-4.myhuaweicloud.com/Kunpeng%20BoostKit/Kunpeng%20BoostKit%2024.0.RC5/BoostKit-KSAL_1.8.0.zip).

7. <a id="000001"></a>Perform the compilation.

    ```sh
    make -j
    ```
   