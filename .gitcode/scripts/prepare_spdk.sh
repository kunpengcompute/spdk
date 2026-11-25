#!/bin/sh
# master（文档库）分支专用：准备 SPDK 21.01.1 源码并合入 patch
# 幂等：源码已就绪则直接返回
set -e

cd "$(dirname "$0")/../.."

# 源码已就绪（含 configure 说明是完整检出）则跳过
if [ -f spdk/configure ]; then
  exit 0
fi

# 清掉可能残留的不完整目录，避免克隆失败
rm -rf spdk

# GitHub 访问走镜像代理（同时覆盖子模块拉取）
git config --global url."https://gh-proxy.test.osinfra.cn/https://github.com/".insteadOf "https://github.com/"

git clone https://github.com/spdk/spdk.git spdk
cd spdk
git checkout 1f0dd58a43b5bc8118b123eca1b07781b052293d
cp ../spdk-21.01.1-for-KAE.patch .
# 仓库中的 patch 为 UTF-16LE(+BOM) 编码且含乱码，git 会当作二进制导致 apply 失败，先转为 UTF-8/LF
# 其中 README.md 段的行数与 @@ 声明不符（损坏），文档改动与编译无关，剔除后再打
python3 - <<'PY'
import re

raw = open('spdk-21.01.1-for-KAE.patch', 'rb').read()
if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
    text = raw.decode('utf-16')
else:
    text = raw.decode('utf-8')
text = text.replace('\r\n', '\n')

parts = re.split(r'(?m)^(?=diff --git )', text)
kept = [p for p in parts if not p.startswith('diff --git a/README.md b/README.md')]
print('补丁分段数: %d, 剔除损坏段: %d' % (len(parts), len(parts) - len(kept)))

with open('spdk-21.01.1-for-KAE.patch', 'w', newline='\n', encoding='utf-8') as f:
    f.write(''.join(kept))
PY
git apply spdk-21.01.1-for-KAE.patch
cd ..
