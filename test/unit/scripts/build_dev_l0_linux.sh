#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Build only the SPDK libraries and UTs needed for dev_L0, using real Linux GCC.
# No DPDK runtime or physical L0/RDMA device is initialized by these unit tests.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
: "${DEV_L0_DEP_PREFIX:?Set to the user dependency prefix containing CUnit and RDMA development files}"
if [[ $(uname -s) != Linux ]]; then
	echo 'This bootstrap requires Linux.' >&2
	exit 2
fi
if [[ -e mk/config.mk && ${DEV_L0_RECONFIGURE:-0} != 1 ]]; then
	echo 'Use a fresh checkout or set DEV_L0_RECONFIGURE=1 to replace its build configuration.' >&2
	exit 2
fi
mkdir -p build/dev_l0_unit_env
cat >build/dev_l0_unit_env/env.mk <<'EOF'
ENV_CFLAGS =
ENV_CXXFLAGS =
ENV_LIBS =
ENV_LINKER_ARGS =
EOF

# Use the complete upstream CONFIG template; only unit-test build options change.
python3 - "$root" "$DEV_L0_DEP_PREFIX" <<'PY'
from pathlib import Path
import re,sys
root,prefix=map(Path,sys.argv[1:])
options=dict(DEBUG='y',COVERAGE='y',RDMA='y',ISAL='n',ISAL_CRYPTO='n',
    CRYPTO='n',IPSEC_MB='n',VHOST='n',VIRTIO='n',VFIO_USER='n',XNVME='n',
    URING='n',AVAHI='n',USDT='n',IDXD='n',IDXD_KERNEL='n',
    TESTS='n',EXAMPLES='n',APPS='n',UNIT_TESTS='y',ARCH='armv8-a',
    ENV=str(root/'build/dev_l0_unit_env'),PREFIX=str(prefix))
text=(root/'CONFIG').read_text()
for name,value in options.items():
    text=re.sub(r'^CONFIG_'+name+r'=.*$',lambda m:'CONFIG_'+name+'='+value,text,flags=re.M)
(root/'mk/config.mk').write_text(text)
PY
scripts/detect_cc.sh --cc="${CC:-gcc}" --cxx="${CXX:-g++}" --lto=n >mk/cc.mk
printf 'CFLAGS?=-I%s/include -I%s/usr/include\n' "$DEV_L0_DEP_PREFIX" "$DEV_L0_DEP_PREFIX" >mk/cc.flags.mk
printf 'LDFLAGS?=-L%s/lib -L%s/lib64 -L%s/usr/lib64\n' "$DEV_L0_DEP_PREFIX" "$DEV_L0_DEP_PREFIX" "$DEV_L0_DEP_PREFIX" >>mk/cc.flags.mk
make build_dir include/spdk/config.h
for library in log util json jsonrpc rpc trace thread ut_mock; do
	make -j"${DEV_L0_JOBS:-4}" -C "lib/$library"
done
bash test/unit/run_dev_l0.sh "${DEV_L0_COVERAGE_DIR:-$root/build/coverage/dev_l0}"
