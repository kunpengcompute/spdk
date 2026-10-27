#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Run after a GCC --enable-debug --enable-coverage --with-rdma SPDK build.
set -euo pipefail

rootdir=$(cd "$(dirname "$0")/../.." && pwd)
cd "$rootdir"
out=${1:-"$rootdir/build/coverage/dev_l0"}
mkdir -p "$out"
out=$(cd "$out" && pwd)

for tool in make lcov python3; do
	command -v "$tool" >/dev/null
done
for feature in COVERAGE RDMA; do
	if ! grep -q "#define SPDK_CONFIG_${feature} 1" include/spdk/config.h; then
		echo "Configure with --enable-debug --enable-coverage --with-rdma first" >&2
		exit 1
	fi
done
if ! grep -q 'CC_TYPE=gcc' mk/cc.mk; then
	echo "Use GCC and matching gcov for this LCOV runner" >&2
	exit 1
fi

tests=(
	lib/env_dpdk/l0.c/l0_ut
	lib/env_dpdk/l0_mmap_platform.c/l0_mmap_platform_ut
	lib/env_dpdk/env.c/env_ut
	lib/env_dpdk/memory.c/memory_ut
	lib/nvmf/qdlimit.c/qdlimit_ut
	lib/nvmf/rdma.c/rdma_ut
	lib/nvmf/ctrlr_bdev.c/ctrlr_bdev_ut
	lib/nvmf/nvmf.c/nvmf_ut
	lib/nvmf/transport.c/transport_ut
	lib/nvmf/transport_weak.c/transport_weak_ut
	lib/nvmf/nvmf_rpc.c/nvmf_rpc_ut
	lib/rdma/common.c/common_ut
)
for test in "${tests[@]}"; do
	make -C "test/unit/$(dirname "$test")"
done

# Keep a zero-count baseline: an unexecuted function must remain in the denominator.
lcov --zerocounters --directory test/unit
lcov --capture --initial --directory test/unit --rc geninfo_auto_base=1 \
	--output-file "$out/baseline.info"
for test in "${tests[@]}"; do
	"test/unit/$test" >"$out/$(basename "$test").log" 2>&1
	echo "PASS $test"
done
weak_dir="$rootdir/test/unit/lib/nvmf/transport_weak.c"
SPDK_UT_WEAK_L0_AVAILABLE=1 \
	LD_PRELOAD="$weak_dir/weak_l0_fixture.so${LD_PRELOAD:+:$LD_PRELOAD}" \
	"$weak_dir/transport_weak_ut" >"$out/transport_weak_preload_ut.log" 2>&1
echo 'PASS nvmf transport with runtime L0 backend'
lcov --capture --directory test/unit --rc geninfo_auto_base=1 \
	--output-file "$out/runtime.info"
lcov --add-tracefile "$out/baseline.info" --add-tracefile "$out/runtime.info" \
	--output-file "$out/c.info"
python3 test/unit/scripts/run_nvmf_dev_l0_coverage.py \
	--output "$out/python.info" --json "$out/python.json"
python3 test/unit/scripts/test_l0_background_job.py --output "$out/shell.info"
python3 test/unit/scripts/check_dev_l0_coverage.py --json "$out/summary.json" \
	"$out/c.info" "$out/python.info" "$out/shell.info"
