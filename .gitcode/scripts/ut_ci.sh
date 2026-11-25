#!/bin/sh
# CI 单元测试脚本：按 PR 目标分支执行不同测试逻辑
# 目标分支由流水线通过 TARGET_BRANCH 环境变量注入；新增分支差异时在下方 case 中追加分支项
set -e

# 切到仓库根目录
cd "$(dirname "$0")/../.."

SRC_DIR="."
case "${TARGET_BRANCH:-}" in
  master)
    # 文档库分支：先准备 SPDK 源码（幂等）
    sh .gitcode/scripts/prepare_spdk.sh
    SRC_DIR="spdk"
    ;;
  *)
    if [ ! -f scripts/pkgdep.sh ]; then
      echo "ERROR: 当前分支 ${TARGET_BRANCH:-<空>} 非全量 SPDK 仓库（缺少 scripts/pkgdep.sh），请在本脚本中补充该分支的测试逻辑" >&2
      exit 1
    fi
    ;;
esac

# 单元测试独立执行完整构建，不复用 build 作业的制品
sh .gitcode/scripts/build_ci.sh

cd "${SRC_DIR}"

# 容器内以 root 运行，SPDK 测试脚本仍会调用 sudo（镜像未装 sudo），提供直通 shim
mkdir -p /usr/local/bin
cat > /usr/local/bin/sudo <<'EOF'
#!/bin/sh
while [ $# -gt 0 ]; do
  case "$1" in
    -E|-H|-n|-S) shift ;;
    -u) shift 2 ;;
    --) shift; break ;;
    -*) shift ;;
    *) break ;;
  esac
done
exec "$@"
EOF
chmod +x /usr/local/bin/sudo

grep -x '#define SPDK_CONFIG_RDMA 1' include/spdk/config.h
SPDK_RUN_VALGRIND=0 SPDK_RUN_ASAN=0 ./test/unit/unittest.sh
