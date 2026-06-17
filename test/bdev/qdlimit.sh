#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 SPDK contributors.
#  All rights reserved.

testdir=$(readlink -f $(dirname $0))
rootdir=$(readlink -f $testdir/../..)
source $rootdir/test/common/autotest_common.sh

rpc_py="$rootdir/scripts/rpc.py"
bdevperf=$rootdir/build/examples/bdevperf

# Start bdevperf in RPC mode (no JSON config; we create bdevs via RPC).
"$bdevperf" -z -q 32 -o 4096 -t 5 -w randrw -M 50 &
bdevperf_pid=$!
trap 'killprocess $bdevperf_pid; exit 1' SIGINT SIGTERM EXIT
waitforlisten $bdevperf_pid

# Build: Malloc0 -> QD0 (per-core depth 8).
$rpc_py bdev_malloc_create -b Malloc0 64 512
$rpc_py bdev_qdlimit_create -b Malloc0 -p QD0 -d 8

# Verify the vbdev exists with expected properties.
$rpc_py bdev_get_bdevs -b QD0 | grep -q '"product_name": "qdlimit"'
$rpc_py bdev_get_bdevs -b QD0 | grep -q '"queue_depth": 8'

# Drive IO through QD0 and verify it completes without error.
PYTHONPATH=$PYTHONPATH:$rootdir/python "$rootdir/examples/bdev/bdevperf/bdevperf.py" \
	perform_tests

# Runtime depth change must succeed.
$rpc_py bdev_qdlimit_set_depth QD0 16
$rpc_py bdev_get_bdevs -b QD0 | grep -q '"queue_depth": 16'

# Teardown.
$rpc_py bdev_qdlimit_delete QD0
$rpc_py bdev_malloc_delete Malloc0

trap - SIGINT SIGTERM EXIT
killprocess $bdevperf_pid
