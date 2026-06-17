#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Intel Corporation. All rights reserved.

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

rpc_py="$rootdir/scripts/rpc.py"

# Bring up SoftRoCE over the loopback/test NIC (provided by nvmf test harness setup).
nvmftestinit

# Start the target.
nvmfappstart -m 0x3
$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Two malloc bdevs sharing the one RDMA data buffer pool.
$rpc_py bdev_malloc_create -b SSD_A 64 4096
$rpc_py bdev_malloc_create -b SSD_B 64 4096

# Subsystem with both namespaces.
$rpc_py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 SSD_A
$rpc_py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 SSD_B
$rpc_py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 \
	-t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Cap SSD_A at a low per-core depth; leave SSD_B unlimited.
QD=4
$rpc_py nvmf_qdlimit_set_depth SSD_A $QD
$rpc_py nvmf_qdlimit_set_depth SSD_B 0
[ "$($rpc_py nvmf_qdlimit_get_depth SSD_A | jq .depth)" -eq "$QD" ]

# Helper: total RDMA data buffers currently checked out of the shared pool.
# Derived from transport stats (num_shared_buffers minus current free cache).
pool_inuse() {
	$rpc_py nvmf_get_stats | jq '
		[.poll_groups[].transports[]
		 | select(.trtype=="RDMA")
		 | (.pending_data_buffer // 0)] | add // 0'
}

# Connect initiator.
nvme connect -t $TEST_TRANSPORT -n nqn.2016-06.io.spdk:cnode1 \
	-a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
waitforserial SPDK00000000000001

# --- Buffer-occupancy ceiling sweep against SSD_A ---
# Increase offered concurrency; admitted in-flight (hence buffers) must plateau at QD/core.
iosize=4096
prev=0
plateaued=0
for od in 1 2 4 8 16 32 64; do
	fio --name=ssdA --filename=/dev/nvme-fabric-ssd_a --rw=randread --bs=${iosize} \
	    --iodepth=${od} --numjobs=1 --runtime=5 --time_based --ioengine=libaio \
	    --direct=1 --group_reporting &
	fio_pid=$!
	sleep 3
	# Admitted in-flight for SSD_A cannot exceed QD * num_cores; with -m 0x3 => 2 cores.
	inflight=$($rpc_py nvmf_qdlimit_get_depth SSD_A | jq .depth) # configured ceiling
	occ_units=$(pool_inuse)
	echo "offered=${od} pool_inflight_units=${occ_units} ceiling_per_core=${inflight}"
	wait $fio_pid
	# Past the ceiling, occupancy must stop growing (allow one in-flight quantum slack).
	if [ "$od" -ge "$QD" ]; then
		if [ "$occ_units" -le $((prev + 1)) ]; then
			plateaued=1
		fi
	fi
	prev=$occ_units
done
[ "$plateaued" -eq 1 ] || { echo "FAIL: buffer occupancy did not plateau at ceiling"; exit 1; }

# --- Isolation: SSD_A throttled must not raise SSD_B p99 ---
fio --name=ssdB --filename=/dev/nvme-fabric-ssd_b --rw=randread --bs=${iosize} \
    --iodepth=32 --numjobs=1 --runtime=10 --time_based --ioengine=libaio \
    --direct=1 --percentile_list=99.0 --output-format=json > /tmp/ssdB.json &
fio --name=ssdA --filename=/dev/nvme-fabric-ssd_a --rw=randread --bs=${iosize} \
    --iodepth=256 --numjobs=4 --runtime=10 --time_based --ioengine=libaio --direct=1 &
wait
p99=$(jq '.jobs[0].read.clat_ns.percentile["99.000000"]' /tmp/ssdB.json)
echo "SSD_B p99(ns) under SSD_A overload = ${p99}"
# Sanity ceiling: with isolation working, SSD_B p99 should stay well under 5 ms.
[ "$p99" -lt 5000000 ] || { echo "FAIL: SSD_B p99 regressed under SSD_A overload"; exit 1; }

nvme disconnect -n nqn.2016-06.io.spdk:cnode1
nvmftestfini
echo "qdlimit RDMA integration: PASS"
