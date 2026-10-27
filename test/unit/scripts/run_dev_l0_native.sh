#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Supplemental macOS execution of SPDK CUnit UTs; Linux make remains authoritative.
set -euo pipefail

rootdir=$(cd "$(dirname "$0")/../../.." && pwd)
native="$rootdir/test/unit/scripts/native"
output=${DEV_L0_OUTPUT_DIR:-"$rootdir/build/dev_l0_native"}
: "${CUNIT_PREFIX:?Set CUNIT_PREFIX to an installed CUnit prefix}"
cc=${CC:-clang}
llvm_cov=${LLVM_COV:-$(xcrun --find llvm-cov)}
llvm_profdata=${LLVM_PROFDATA:-$(xcrun --find llvm-profdata)}
mkdir -p "$output"
output=$(cd "$output" && pwd)

if [[ $(uname -s) != Darwin ]]; then
	echo 'Use the standard SPDK configure/make UT workflow on Linux.' >&2
	exit 2
fi

if [[ -z ${RDMA_INCLUDE:-} ]]; then
	: "${RDMA_CORE_SOURCE:?Set RDMA_CORE_SOURCE to official rdma-core sources, or RDMA_INCLUDE to prepared headers}"
	RDMA_INCLUDE="$output/native-include"
	mkdir -p "$RDMA_INCLUDE"/{infiniband,rdma,linux,spdk}
	for file in verbs.h verbs_api.h sa.h; do
		cp "$RDMA_CORE_SOURCE/libibverbs/$file" "$RDMA_INCLUDE/infiniband/$file"
	done
	cp "$RDMA_CORE_SOURCE/kernel-headers/rdma/ib_user_ioctl_verbs.h" "$RDMA_INCLUDE/infiniband/"
	cp "$RDMA_CORE_SOURCE/kernel-headers/rdma/ib_user_verbs.h" "$RDMA_INCLUDE/rdma/"
	for file in rdma_cma.h rdma_verbs.h; do
		cp "$RDMA_CORE_SOURCE/librdmacm/$file" "$RDMA_INCLUDE/rdma/$file"
	done
	cp "$native/linux/"*.h "$RDMA_INCLUDE/linux/"
	: > "$RDMA_INCLUDE/spdk/config.h"
fi

cd "$rootdir"
common_flags=(-std=gnu11 -O0 -g -DDEBUG -DSPDK_UNIT_TEST=1
	-Wno-macro-redefined -Wno-format -Wno-pointer-sign
	-include "$native/compat.h" -I"$RDMA_INCLUDE"
	-Iinclude -Ilib -Itest -I"$CUNIT_PREFIX/include"
	-fprofile-instr-generate -fcoverage-mapping)
link_flags=(-L"$CUNIT_PREFIX/lib" -lcunit -Wl,-dead_strip)
log_sources=(lib/log/log.c lib/log/log_flags.c)
util_sources=(lib/util/string.c lib/util/strerror_tls.c lib/util/iov.c lib/util/math.c)
json_sources=(lib/json/json_write.c lib/json/json_util.c)
rdma_sources=(lib/util/dif.c lib/util/crc16.c lib/util/crc32.c)

run_suite() {
	local suite=$1
	shift
	"$cc" "${common_flags[@]}" "$@" "${link_flags[@]}" -o "$output/${suite}_ut"
	LLVM_PROFILE_FILE="$output/$suite.profraw" "$output/${suite}_ut" > "$output/$suite.log" 2>&1
	"$llvm_profdata" merge -sparse "$output/$suite.profraw" -o "$output/$suite.profdata"
	"$llvm_cov" export -format=lcov "$output/${suite}_ut" \
		-instr-profile="$output/$suite.profdata" > "$output/$suite.info"
	cat "$output/$suite.log"
}

run_suite qdlimit -DDEV_L0_TEST_SOURCE='"'"$rootdir/test/unit/lib/nvmf/qdlimit.c/qdlimit_ut.c"'"' \
	"$native/calloc_adapter.c" "${log_sources[@]}" lib/util/string.c
run_suite ctrlr_bdev test/unit/lib/nvmf/ctrlr_bdev.c/ctrlr_bdev_ut.c \
	"${log_sources[@]}" "${util_sources[@]}"
run_suite rdma test/unit/lib/nvmf/rdma.c/rdma_ut.c \
	"$native/rdma_link_stubs.c" "$native/thread_link_stubs.c" "$native/trace_stubs.c" \
	"${log_sources[@]}" "${util_sources[@]}" "${json_sources[@]}" "${rdma_sources[@]}"
run_suite transport test/unit/lib/nvmf/transport.c/transport_ut.c \
	"$native/thread_link_stubs.c" "$native/trace_stubs.c" \
	"${log_sources[@]}" "${util_sources[@]}" "${json_sources[@]}" "${rdma_sources[@]}"
run_suite nvmf test/unit/lib/nvmf/nvmf.c/nvmf_ut.c lib/thread/thread.c \
	"$native/trace_stubs.c" "${log_sources[@]}" lib/log/log_deprecated.c \
	"${util_sources[@]}" "${json_sources[@]}" lib/util/uuid.c lib/util/bit_array.c \
	lib/util/cpuset.c lib/util/fd_group.c

# The following helpers run the same source-based UTs as the Linux Makefiles.
ROOT=$rootdir
NATIVE_OUT_DIR=$output
NATIVE_COMPAT_DIR=$RDMA_INCLUDE
CC=$cc
export ROOT NATIVE_OUT_DIR NATIVE_COMPAT_DIR CUNIT_PREFIX CC
CFLAGS=("${common_flags[@]}")

emit_profile() {
	local name=$1 binary=$2
	LLVM_PROFILE_FILE="$output/$name.profraw" "$binary" > "$output/$name.log" 2>&1
	"$llvm_profdata" merge -sparse "$output/$name.profraw" -o "$output/$name.profdata"
	"$llvm_cov" export -format=lcov "$binary" \
		-instr-profile="$output/$name.profdata" > "$output/$name.info"
	cat "$output/$name.log"
}

source "$native/env.sh"
source "$native/rpc.sh"

"$cc" "${common_flags[@]}" \
	-DDEV_L0_TEST_SOURCE='"'"$rootdir/test/unit/lib/rdma/common.c/common_ut.c"'"' \
	"$native/calloc_adapter.c" "${log_sources[@]}" \
	lib/util/string.c lib/util/strerror_tls.c "${link_flags[@]}" -o "$output/common_ut"
emit_profile common "$output/common_ut"

python3 test/unit/scripts/run_nvmf_dev_l0_coverage.py \
	--output "$output/python.info" --json "$output/python.json"
python3 test/unit/scripts/test_l0_background_job.py \
	--output "$output/shell.info" --json "$output/shell.json"

python3 test/unit/scripts/check_dev_l0_coverage.py --json "$output/summary.json" \
	"$output/qdlimit.info" "$output/rdma.info" "$output/ctrlr_bdev.info" \
	"$output/nvmf.info" "$output/transport.info" "$output/l0.info" \
	"$output/l0_mmap_platform.info" "$output/env.info" "$output/memory.info" \
	"$output/rpc.info" "$output/common.info" "$output/python.info" "$output/shell.info"
