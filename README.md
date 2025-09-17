# 项目介绍<a name="ZH-CN_TOPIC_0000002442479380"></a>

基于NVMe协议的SSD的出现后，软件路径成为IO瓶颈。SPDK是一个存储加速方案，是利用用户态、异步、轮询方式的NVMe驱动，用于加速NVMe SSD作为后端存储使用的应用软件的加速库。

本文档是对SPDK中加解密、压缩、CRC（Cyclic Redundancy Check）循环冗余校验特性进行说明，其中加解密、压缩特性是将加解密和压缩的计算卸载到鲲鹏处理器的KAE模块获取性能收益，CRC特性则是将SPDK中开源版本的CRC实现替换成自研的KSAL-CRC算法实现，存储加速算法库（简称KSAL）是华为自研的存储加速算法库，通过大数求余算法和配合鲲鹏向量化指令实现编码加速，相比开源版本的CRC算法性能提升20%以上。

# 版本说明<a name="ZH-CN_TOPIC_0000002442661242"></a>

**表 1**  版本说明

<a name="table154016110243"></a>
<table><thead align="left"><tr id="row74015192418"><th class="cellrowborder" valign="top" width="30.73%" id="mcps1.2.4.1.1"><p id="p12401515246"><a name="p12401515246"></a><a name="p12401515246"></a>鲲鹏SPDK</p>
</th>
<th class="cellrowborder" valign="top" width="27.16%" id="mcps1.2.4.1.2"><p id="p1340117112414"><a name="p1340117112414"></a><a name="p1340117112414"></a>开源SPDK</p>
</th>
<th class="cellrowborder" valign="top" width="42.11%" id="mcps1.2.4.1.3"><p id="p24012120244"><a name="p24012120244"></a><a name="p24012120244"></a>特性</p>
</th>
</tr>
</thead>
<tbody><tr id="row7401131102414"><td class="cellrowborder" valign="top" width="30.73%" headers="mcps1.2.4.1.1 "><p id="p184021011249"><a name="p184021011249"></a><a name="p184021011249"></a>spdk21.01.1-for-KAE</p>
</td>
<td class="cellrowborder" valign="top" width="27.16%" headers="mcps1.2.4.1.2 "><p id="p1240213132420"><a name="p1240213132420"></a><a name="p1240213132420"></a>spdk21.01.1</p>
</td>
<td class="cellrowborder" valign="top" width="42.11%" headers="mcps1.2.4.1.3 "><p id="p1040216111241"><a name="p1040216111241"></a><a name="p1040216111241"></a>加解密、压缩、CRC卸载到KAE</p>
</td>
</tr>
</tbody>
</table>

# 环境部署<a name="ZH-CN_TOPIC_0000002476179357"></a>



## 安装KAE<a name="ZH-CN_TOPIC_0000002442763790"></a>

安装KAE的步骤请参见鲲鹏社区《KAE使能SPDK特性指南》[安装KAE](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/basicAccelFeatures/kaeebspdk/kunpengspdk_16_0011.html)章节。

## 编译SPDK<a name="ZH-CN_TOPIC_0000002476203585"></a>

编译SPDK步骤请参见鲲鹏社区《KAE使能SPDK特性指南》[编译SPDK](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/basicAccelFeatures/kaeebspdk/kunpengspdk_16_0012.html)章节。

# 快速上手<a name="ZH-CN_TOPIC_0000002442899470"></a>




## 上手SPDK加解密特性<a name="ZH-CN_TOPIC_0000002476253209"></a>

通过使能SPDK，再做相应的加解密设置，具体步骤请参见[使用SPDK加解密特性](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/basicAccelFeatures/kaeebspdk/kunpengspdk_16_0014.html)。

## 上手SPDK压缩特性<a name="ZH-CN_TOPIC_0000002442773242"></a>

通过使能SPDK，再做相应的解压缩设置，具体步骤请参见[使用SPDK压缩特性](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/basicAccelFeatures/kaeebspdk/kunpengspdk_16_0015.html)。

## 上手SPDK CRC特性<a name="ZH-CN_TOPIC_0000002476213025"></a>

通过使能SPDK，再做相应的CRC设置，具体步骤请参见[使用SPDK CRC特性](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/basicAccelFeatures/kaeebspdk/kunpengspdk_16_0016.html)。

# 贡献指南<a name="ZH-CN_TOPIC_0000002442739574"></a>

如果使用过程中有任何问题，或者需要反馈特性需求和bug报告，可以提交issues联系我们，具体贡献方法可参考[这里](https://gitcode.com/boostkit/community/blob/master/docs/contributor/contributing.md)。

# 免责声明<a name="ZH-CN_TOPIC_0000002442916578"></a>

此代码仓计划参与SPDK软件开源，仅作SPDK功能扩展/SPDK性能提升，编码风格遵照原生开源软件，继承原生开源软件安全设计，不破坏原生开源软件设计及编码风格和方式，软件的任何漏洞与安全问题，均由相应的上游社区根据其漏洞和安全响应机制解决。请密切关注上游社区发布的通知和版本更新。鲲鹏计算社区对软件的漏洞及安全问题不承担任何责任。

# 许可证书<a name="ZH-CN_TOPIC_0000002476196473"></a>

BSD LICENSE

# 参考文档<a name="ZH-CN_TOPIC_0000002476236681"></a>

鲲鹏社区分布式存储：[KAE使能SPDK特性指南](https://www.hikunpeng.com/document/detail/zh/kunpengsdss/basicAccelFeatures/kaeebspdk/kunpengspdk_16_0005.html)。

