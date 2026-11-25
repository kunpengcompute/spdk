FROM swr.cn-north-4.myhuaweicloud.com/cloud_boostkit/openeuler22.03_lts_sp3:latest

# 系统构建依赖（对应 scripts/pkgdep/rhel.sh 的 yum 安装清单）
RUN dnf install -y \
        gcc gcc-c++ make \
        CUnit-devel libcmocka-devel \
        libaio-devel openssl-devel libuuid-devel libiscsi-devel \
        ncurses-devel json-c-devel numactl-devel nasm systemtap-sdt-devel \
        clang clang-devel \
        python3 python3-devel python3-pip \
        autoconf automake libtool help2man \
        libibverbs-devel librdmacm-devel rdma-core-devel \
        libasan libubsan libubsan-static \
        git \
    && dnf clean all \
    && rm -rf /var/cache/dnf

RUN pip3 install -i https://mirrors.aliyun.com/pypi/simple/ \
        ninja meson pyelftools ijson python-magic \
        grpcio grpcio-tools pyyaml scikit-build

WORKDIR /workspace

# 容器常驻
CMD ["sleep", "infinity"]
