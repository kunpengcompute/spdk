#!/bin/sh
# CI 构建脚本：按 PR 目标分支执行不同构建逻辑
# 目标分支由流水线通过 TARGET_BRANCH 环境变量注入；新增分支差异时在下方 case 中追加分支项
set -e

# 切到仓库根目录
cd "$(dirname "$0")/../.."

case "${TARGET_BRANCH:-}" in
  master)
    # 文档库分支：先下载 SPDK 源码并合入本仓库 patch
    sh .gitcode/scripts/prepare_spdk.sh
    cd spdk
    ;;
  *)
    # 全量仓库分支
    if [ ! -f scripts/pkgdep.sh ]; then
      echo "ERROR: 当前分支 ${TARGET_BRANCH:-<空>} 非全量 SPDK 仓库（缺少 scripts/pkgdep.sh），请在本脚本中补充该分支的构建逻辑" >&2
      exit 1
    fi
    ;;
esac

# 统一编译流程（master 在 spdk 目录内执行，全量分支在仓库根目录执行）
# 依赖：CI 镜像已预装编译依赖，openEuler 源偶发 repomd 校验失败时不阻断构建
if ! sh scripts/pkgdep.sh; then
  echo "WARN: pkgdep.sh 失败，清理缓存后重试一次" >&2
  dnf clean all >/dev/null 2>&1 || true
  sh scripts/pkgdep.sh || echo "WARN: pkgdep.sh 仍失败，继续编译（镜像已预装依赖）" >&2
fi

# GitHub 访问走镜像代理（子模块可能指向 GitHub）
git config --global url."https://gh-proxy.test.osinfra.cn/https://github.com/".insteadOf "https://github.com/"

git submodule update --init

./configure --with-rdma --enable-unit-tests --without-shared
make -j$(nproc)
