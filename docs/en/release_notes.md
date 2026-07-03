# Release Notes<a name="EN-US_TOPIC_0000002548695365"></a>

## Version Mapping<a name="EN-US_TOPIC_0000002516126574"></a>

### Product Version Information<a name="EN-US_TOPIC_0000002547526485"></a>

<a name="table826974415312"></a>
<table><tbody><tr id="row726916447319"><th class="firstcol" valign="top" width="27.79%" id="mcps1.1.3.1.1"><p id="p426912446314"><a name="p426912446314"></a><a name="p426912446314"></a>Product Name</p>
</th>
<td class="cellrowborder" valign="top" width="72.21%" headers="mcps1.1.3.1.1 "><p id="p433219386215"><a name="p433219386215"></a><a name="p433219386215"></a>Kunpeng BoostKit</p>
</td>
</tr>
<tr id="row102692044123111"><th class="firstcol" valign="top" width="27.79%" id="mcps1.1.3.2.1"><p id="p1526912441319"><a name="p1526912441319"></a><a name="p1526912441319"></a>Product Version</p>
</th>
<td class="cellrowborder" valign="top" width="72.21%" headers="mcps1.1.3.2.1 "><p id="p1864713360227"><a name="p1864713360227"></a><a name="p1864713360227"></a><span id="ph1224011917236"><a name="ph1224011917236"></a><a name="ph1224011917236"></a>24.0.RC5</span></p>
</td>
</tr>
<tr id="row14269844183118"><th class="firstcol" valign="top" width="27.79%" id="mcps1.1.3.3.1"><p id="p1027044483111"><a name="p1027044483111"></a><a name="p1027044483111"></a>Software Name</p>
</th>
<td class="cellrowborder" valign="top" width="72.21%" headers="mcps1.1.3.3.1 "><p id="p2270194443117"><a name="p2270194443117"></a><a name="p2270194443117"></a>SPDK I/O acceleration</p>
</td>
</tr>
</tbody>
</table>

### Software Version Mapping<a name="EN-US_TOPIC_0000002515966648"></a>

|Item|Version|
|--|--|
|OS|openEuler 22.03 LTS SP1<br>openEuler 22.03 LTS SP2|
|SPDK|21.01.1|
|KAE|2.0.0|
|OpenSSL|1.1.1a or later|

### Hardware Version Mapping<a name="EN-US_TOPIC_0000002515966654"></a>

|Item|Requirement|
|--|--|
|Processor|Kunpeng 920|
|iBMC|V365 or later|
|BIOS|V1.5 or later|

### Virus Scan Results<a name="EN-US_TOPIC_0000002515966650"></a>

Virus scanning is not involved because no software package is released.

### Version Description<a name="EN-US_TOPIC_0000002516126576"></a>

#### Change Description<a name="EN-US_TOPIC_0000002516126572"></a>

The encryption/decryption, compression, and CRC features are updated.

The encryption/decryption module is located at the block storage device layer. It uses encryption algorithms in OpenSSL to encrypt user data, ensuring data security. It provides the AES and SM4 symmetric encryption algorithms and the RSA asymmetric encryption algorithm.

The compression module compresses user data using a compression algorithm, which saves storage and reduces data transmission over the network.

CRC is a common algorithm used to detect errors in data transmission or storage. It performs specific mathematical operations on data blocks to generate a short CRC value for data integrity check. In SPDK, CRC is mainly used in the NVMe-oF communication protocol to ensure correct data transmission and in data storage to detect silent errors in drives.

#### Resolved Issues<a name="EN-US_TOPIC_0000002547606475"></a>

None

#### Known Issues<a name="EN-US_TOPIC_0000002547526487"></a>

None

## Related Documentation<a name="EN-US_TOPIC_0000002515966652"></a>

|Document|Description|Delivery Method|
|--|--|--|
|*KAE-enabled SPDK Release Notes*|Provides the version release information about KAE-enabled SPDK.|Open-source repository|
|*KAE-enabled SPDK Installation Guide*|Describes how to install and deploy KAE-enabled SPDK.|Open-source repository|
|*KAE-enabled SPDK User Guide*|Describes how to use the features of KAE-enabled SPDK.|Open-source repository|

### Obtaining Documentation<a name="EN-US_TOPIC_0000002547526489"></a>

Visit the [open-source repository](https://gitcode.com/boostkit/spdk) to view or download related documents.

## Change History

| Date  | Description       |
|-------|----------|
| 2024-09-30 | This is the first official release. |
