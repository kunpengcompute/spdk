# README<a id="EN-US_TOPIC_0000002521402728"></a>

## Project Introduction<a id="EN-US_TOPIC_0000002551566043"></a>

### Introduction<a id="EN-US_TOPIC_0000002551566047"></a>

When NVMe SSDs are used, long software paths are prone to cause I/O bottlenecks. The Storage Performance Development Kit (SPDK) can accelerate storage applications that use NVMe SSDs as backend storage through its bedrock, a userspace, polled-mode, asynchronous NVMe driver.

This document describes the encryption/decryption, compression, and cyclic redundancy check (CRC) features of SPDK. Its encryption/decryption and compression performance can be enhanced by offloading such workloads to the Kunpeng Accelerator Engine (KAE) on the Kunpeng processor. In addition, open-source SPDK CRC can be replaced with a Huawei-developed CRC algorithm provided in the Kunpeng Storage Acceleration Library (KSAL). This library uses a modulo algorithm for large numbers and Kunpeng vectorized instructions to accelerate encoding, improving the CRC performance by more than 20%.

### Software Architecture<a id="EN-US_TOPIC_0000002520046056"></a>

The following figure shows the overall architecture of open-source SPDK.

**Figure 1** Software architecture<a id="fig17242642112411"></a><a id="software-architecture"></a>

![](docs/en/figures/introduction.png "Software architecture")

SPDK consists of the application protocol layer, storage service layer, block device layer, and driver layer from top to bottom.

- The application protocol layer includes protocols such as NVMe over Fabrics (NVMe-oF) and iSCSI target for network storage and vhost and vfio-user for virtualization.
- The storage service layer provides services such as partitioning, Open CAS Framework (OCF), and Blobstore file system (BlobFS).
- The block device layer abstracts common block devices (bdev) to support various backend storage media, such as NVMe, NVMe-oF, and Ceph RBD, as well as custom storage devices.
- The driver layer consists of user-mode drivers implemented by SPDK.

Open-source SPDK contains multiple components. This document describes the encryption, decryption, compression, and CRC features. Encryption, decryption, and compression are implemented at the storage service layer.

- The encryption/decryption module is located at the block storage device layer. It uses encryption algorithms in OpenSSL to encrypt user data, ensuring data security. It provides the AES and SM4 symmetric encryption algorithms and the RSA asymmetric encryption algorithm. Users can also configure KAE for these algorithms to improve the encryption/decryption performance or reduce the CPU usage.
- The compression module compresses user data using a compression algorithm, which saves storage and reduces data transmission over the network. The compression driver in the Data Plane Development Kit (DPDK) is used for compression. To offload the workloads to KAE, use the zlib driver. For details, see [User Guide](docs/en/user_guide.md).
- CRC is a common algorithm used to detect errors in data transmission or storage. It performs specific mathematical operations on data blocks to generate a short CRC value for data integrity check. In SPDK, CRC is mainly used in the NVMe-oF communication protocol to ensure correct data transmission and in data storage to detect silent errors in drives.

### Other Information<a id="EN-US_TOPIC_0000002520046054"></a>

**Specifications<a id="section1976310144815"></a>**

Enabling KAE (Kunpeng Accelerator Engine) offloading for decompression does not reduce performance compared to non-offloading, while the CRC algorithm achieves a 20% performance improvement over open-source SPDK.

**Availability<a id="section6297125317251"></a>**

- Version: This feature is adapted to SPDK 21.01.1.

**Constraints<a id="section94532111123"></a>**

This version is available only for the Kunpeng platform.

**Application Scenarios<a id="section1345037193015"></a>**

When SPDK crypto is used, you can use the encryption/decryption feature to offloads the workloads to KAE.

In scenarios where SPDK decompression is used, you can use the decompression feature to offload the workloads to KAE.

When SPDK CRC is used, you can use the Huawei-developed CRC algorithm to replace the open-source CRC implementation.

## Directory Structure<a id="EN-US_TOPIC_0000002551748963"></a>

```txt
├── docs                              # Project documentation
│   ├── LICENSE                       # Document license
│   └── en                            # Document directory
│       ├── figures                   # Directory of figures in documents
│       ├── public_sys-resources      # Public resource directory
│       ├── installation_guide.md     # SPDK KAE installation guide
│       ├── user_guide.md             # SPDK KAE user guide
│       └── release_notes.md          # Release notes
├── spdk-21.01.1-for-KAE.patch        # SPDK patch for enabling KAE
└── README.md                         # Introduction
```

## Version Description<a id="EN-US_TOPIC_0000002520548958"></a>

For details about the version information of the KAE-enabled SPDK feature, see [Release Notes](docs/en/release_notes.md).

## Installation Guide<a id="EN-US_TOPIC_0000002551828937"></a>

For environmental requirements for KAE-enabled SPDK, KAE installation methods, and SPDK compilation and installation, see [Installation Guide](docs/en/installation_guide.md).

## User Guide<a id="EN-US_TOPIC_0000002520989184"></a>

For details about how to use SPDK features, see [User Guide](docs/en/user_guide.md).

## Security Statement<a id="EN-US_TOPIC_0000002551446047"></a>

**Routine Check Using Antivirus Software<a id="section1558125655811"></a>**

Periodically scan hosts for viruses. This protects hosts from viruses, malicious code, spyware, and malicious programs, reducing risks such as system breakdown and information leakage. Mainstream antivirus software can be used for antivirus check.

**Vulnerability Fixing<a id="section929618383329"></a>**

To ensure the security of the production environment and reduce attacks, enable the firewall and periodically fix the following vulnerabilities:

- OS vulnerabilities
- OpenSSL vulnerabilities
- KAE vulnerabilities
- SPDK vulnerabilities

## Disclaimer<a id="EN-US_TOPIC_0000002551748967"></a>

**To Users of This Project**

- This project is intended solely for debugging and development. You are responsible for any risks and should carefully review the following information:
    - Data processing and deletion: Users are responsible for managing and deleting any data generated while using this tool. Users are advised to delete such data promptly after use to prevent information leakage.
    - Data confidentiality and transmission: Users understand and agree not to share or transmit any data generated by this tool. Neither the tool nor its developers are responsible for any information leaks, data breaches, or other negative consequences.
    - User input security: Users are responsible for the security of any commands they enter and for any risks or losses resulting from improper input. The tool and its developers are not liable for issues caused by incorrect command usage.

- Disclaimer scope: This disclaimer applies to all individuals and entities using this tool. By using the tool, you acknowledge and accept this statement and assume all risks and responsibilities arising from its use. If you do not agree, please stop using the tool immediately.
- Before using this tool, please **read and understand the preceding disclaimer**. If you have any questions, contact the developer.

**To Data Owners**

If you do not want your model or dataset to be mentioned in this project, or if you wish to update its description, please submit an issue on GitCode. We will delete or update your description according to your request. Thank you for your understanding and contribution to this project.

## License<a id="EN-US_TOPIC_0000002520548960"></a>

The documents of this project are licensed under CC-BY 4.0. For details, see [LICENSE](docs/LICENSE).

## Contribution Statement<a id="EN-US_TOPIC_0000002551828941"></a>

We welcome your contributions to the community. If you have any questions/suggestions or want to provide feedback on feature requirements and bug reports, you can submit [issues](https://gitcode.com/boostkit/community/blob/master/docs/contributor/issue-submit.md). For details, see the [contribution guideline](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md). You are also welcome to share insights in [Discussions](https://gitcode.com/boostkit/community/discussions). Thank you for your support.
