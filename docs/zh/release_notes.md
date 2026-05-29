# 版本说明书<a name="ZH-CN_TOPIC_0000002548695365"></a>

## 版本配套说明<a name="ZH-CN_TOPIC_0000002516126574"></a>

### 产品版本信息<a name="ZH-CN_TOPIC_0000002547526485"></a>

<a name="table826974415312"></a>
<table><tbody><tr id="row726916447319"><th class="firstcol" valign="top" width="27.79%" id="mcps1.1.3.1.1"><p id="p426912446314"><a name="p426912446314"></a><a name="p426912446314"></a>产品名称</p>
</th>
<td class="cellrowborder" valign="top" width="72.21%" headers="mcps1.1.3.1.1 "><p id="p433219386215"><a name="p433219386215"></a><a name="p433219386215"></a>Kunpeng BoostKit</p>
</td>
</tr>
<tr id="row102692044123111"><th class="firstcol" valign="top" width="27.79%" id="mcps1.1.3.2.1"><p id="p1526912441319"><a name="p1526912441319"></a><a name="p1526912441319"></a>产品版本</p>
</th>
<td class="cellrowborder" valign="top" width="72.21%" headers="mcps1.1.3.2.1 "><p id="p1864713360227"><a name="p1864713360227"></a><a name="p1864713360227"></a><span id="ph1224011917236"><a name="ph1224011917236"></a><a name="ph1224011917236"></a>24.0.RC5</span></p>
</td>
</tr>
<tr id="row14269844183118"><th class="firstcol" valign="top" width="27.79%" id="mcps1.1.3.3.1"><p id="p1027044483111"><a name="p1027044483111"></a><a name="p1027044483111"></a>软件名称</p>
</th>
<td class="cellrowborder" valign="top" width="72.21%" headers="mcps1.1.3.3.1 "><p id="p2270194443117"><a name="p2270194443117"></a><a name="p2270194443117"></a>SPDK IO加速</p>
</td>
</tr>
</tbody>
</table>

### 软件版本配套说明<a name="ZH-CN_TOPIC_0000002515966648"></a>

|软件类型|版本|
|--|--|
|OS|openEuler 22.03 LTS SP1<br>openEuler 22.03 LTS SP2|
|SPDK|21.01.1|
|KAE|2.0.0|
|OpenSSL|1.1.1a及以上|

### 硬件版本配套说明<a name="ZH-CN_TOPIC_0000002515966654"></a>

|项目|要求|
|--|--|
|处理器|鲲鹏920处理器|
|iBMC版本|V365及以上|
|BIOS版本|V1.5及以上|

### 病毒扫描结果<a name="ZH-CN_TOPIC_0000002515966650"></a>

不涉及软件包发布，不涉及病毒扫描。

### 版本说明<a name="ZH-CN_TOPIC_0000002516126576"></a>

#### 更新说明<a name="ZH-CN_TOPIC_0000002516126572"></a>

更新加解密、压缩和CRC特性。

加解密模块位于块存储设备层，利用OpenSSL中的加密算法对用户的数据进行加密，保护用户的数据安全。提供的算法包括：对称加密（AES，SM4），非对称加密（RSA）。

压缩是使用压缩算法对用户的数据进行压缩，能够节省存储空间，如果需要经过网络传输的话也能减少网络传输的数据量。

CRC特性，是一种用于检测数据传输或存储中错误的常见算法。它通过对数据块执行特定的数学运算来生成一个短的校验值（CRC值），用于对比数据完整性。在SPDK中主要用于：数据协议（用于NVMe-oF通信协议，以确保信息的正确传递），数据存储（用于检测磁盘中的静默错误）等。

#### 已解决的问题<a name="ZH-CN_TOPIC_0000002547606475"></a>

无

#### 遗留问题<a name="ZH-CN_TOPIC_0000002547526487"></a>

无

## 版本配套文档<a name="ZH-CN_TOPIC_0000002515966652"></a>

|文档名称|内容简介|交付形式|
|--|--|--|
|《KAE使能SPDK 版本说明书》|本文档提供KAE使能SPDK的版本发布信息。|开源仓|
|《KAE使能SPDK 安装指南》|本文档提供KAE使能SPDK的安装、部署指导。|开源仓|
|《KAE使能SPDK 用户指南》|本文档提供KAE使能SPDK的特性使用指南。|开源仓|

### 获取文档的方法<a name="ZH-CN_TOPIC_0000002547526489"></a>

您可以通过访问[开源仓](https://gitcode.com/boostkit/spdk)浏览和获取相关文档。

## 修订记录
| 发布日期  | 修改说明       |
|-------|----------|
| 2024-09-30 | 第一次正式发布。 |