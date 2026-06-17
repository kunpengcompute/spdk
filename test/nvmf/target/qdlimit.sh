#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 Intel Corporation. All rights reserved.

testdir=$(readlink -f "$(dirname "$0")")
rootdir=$(readlink -f "$testdir/../../..")
source "$rootdir/test/common/autotest_common.sh"
source "$rootdir/test/nvmf/common.sh"

rpc_py="$rootdir/scripts/rpc.py"

# Hard requirements for this test's measurement/parsing.
if ! hash jq fio nvme 2>/dev/null; then
	echo "qdlimit test requires jq, fio and nvme-cli"
	exit 1
fi

MALLOC_BS=4096
IOSIZE=4096
QD=4
SSDA_NQN="nqn.2016-06.io.spdk:cnodeA"
SSDB_NQN="nqn.2016-06.io.spdk:cnodeB"
SSDA_SERIAL="SPDKQDLIMITSSDA01"
SSDB_SERIAL="SPDKQDLIMITSSDB01"

ssda_dev=""
ssdb_dev=""

cleanup() {
	# Best-effort teardown; safe to call even if steps above failed.
	pkill -f "fio --name=qd_" 2>/dev/null || :
	if [ -n "$ssda_dev$ssdb_dev" ]; then
		$NVME_DISCONNECT -n "$SSDA_NQN" 2>/dev/null || :
		$NVME_DISCONNECT -n "$SSDB_NQN" 2>/dev/null || :
		waitforserial_disconnect "$SSDA_SERIAL" 2>/dev/null || :
		waitforserial_disconnect "$SSDB_SERIAL" 2>/dev/null || :
	fi
	nvmftestfini || :
	rm -f /tmp/qdlimit_ssdb_*.json
}

# NVME_CONNECT/NVME_DISCONNECT default to the plain commands but may be overridden by the harness.
NVME_DISCONNECT=${NVME_DISCONNECT:-"nvme disconnect"}

# Map a connected controller serial number to its /dev/nvmeXnY namespace device. Uses
# id-ctrl (stable across nvme-cli versions) rather than the volatile `nvme list -o json` schema.
get_dev_for_serial() {
	local serial=$1 dev
	for dev in $(get_nvme_devs); do
		if nvme id-ctrl "$dev" 2>/dev/null | grep -qE "^sn[[:space:]]*:[[:space:]]*${serial}([[:space:]]|$)"; then
			echo "$dev"
			return 0
		fi
	done
	return 1
}

# Sample qdlimit's reported total in-flight for a bdev for $2 seconds, return the max observed.
sample_max_inflight() {
	local bdev=$1 dur=$2 maxv=0 v stats end
	end=$((SECONDS + dur))
	while [ "$SECONDS" -lt "$end" ]; do
		stats=$($rpc_py nvmf_qdlimit_get_stats "$bdev")
		v=$(echo "$stats" | jq '.total_inflight')
		[ "$v" -gt "$maxv" ] && maxv=$v
		sleep 0.2
	done
	echo "$maxv"
}

nvmftestinit
nvmfappstart -m 0x3
trap cleanup EXIT

$rpc_py nvmf_create_transport $NVMF_TRANSPORT_OPTS -u 8192

# Two malloc bdevs in two subsystems, both on the same transport (shared data_buf_pool).
$rpc_py bdev_malloc_create -b SSD_A 64 $MALLOC_BS
$rpc_py bdev_malloc_create -b SSD_B 64 $MALLOC_BS

$rpc_py nvmf_create_subsystem "$SSDA_NQN" -a -s "$SSDA_SERIAL"
$rpc_py nvmf_subsystem_add_ns "$SSDA_NQN" SSD_A
$rpc_py nvmf_subsystem_add_listener "$SSDA_NQN" -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

$rpc_py nvmf_create_subsystem "$SSDB_NQN" -a -s "$SSDB_SERIAL"
$rpc_py nvmf_subsystem_add_ns "$SSDB_NQN" SSD_B
$rpc_py nvmf_subsystem_add_listener "$SSDB_NQN" -t $TEST_TRANSPORT -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT

# Cap SSD_A at a low per-core depth; leave SSD_B unlimited.
$rpc_py nvmf_qdlimit_set_depth SSD_A $QD
$rpc_py nvmf_qdlimit_set_depth SSD_B 0
[ "$($rpc_py nvmf_qdlimit_get_depth SSD_A | jq .depth)" -eq "$QD" ]

$NVME_CONNECT -t $TEST_TRANSPORT -n "$SSDA_NQN" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
$NVME_CONNECT -t $TEST_TRANSPORT -n "$SSDB_NQN" -a $NVMF_FIRST_TARGET_IP -s $NVMF_PORT
waitforserial "$SSDA_SERIAL"
waitforserial "$SSDB_SERIAL"

ssda_dev=$(get_dev_for_serial "$SSDA_SERIAL") || { echo "FAIL: could not resolve SSD_A device"; exit 1; }
ssdb_dev=$(get_dev_for_serial "$SSDB_SERIAL") || { echo "FAIL: could not resolve SSD_B device"; exit 1; }
echo "Resolved SSD_A=$ssda_dev SSD_B=$ssdb_dev"

# --- Buffer-occupancy ceiling sweep against SSD_A ---
# Drive increasing offered concurrency at SSD_A. qdlimit's reported total_inflight (the actual
# count of buffer-holding admitted requests) must never exceed depth*num_poll_groups, and must
# saturate at that ceiling once offered concurrency >= the ceiling. Occupancy = inflight*IOSIZE.
saturated=0
for od in 1 2 4 8 16 32 64; do
	fio --name=qd_ceiling --filename="$ssda_dev" --rw=randread --bs=${IOSIZE} \
	    --iodepth=${od} --numjobs=1 --runtime=6 --time_based --ioengine=libaio \
	    --direct=1 --group_reporting &> /dev/null &
	fio_pid=$!
	sleep 1 # let fio ramp
	max_inflight=$(sample_max_inflight SSD_A 3)
	npg=$($rpc_py nvmf_qdlimit_get_stats SSD_A | jq '.num_poll_groups')
	wait $fio_pid

	# num_poll_groups counts only cores actually driving SSD_A, so this ceiling is exact.
	ceiling=$((QD * npg))
	[ "$npg" -ge 1 ] || { echo "FAIL: SSD_A never observed on any core"; exit 1; }
	echo "offered=$((od)) max_inflight=${max_inflight} ceiling=${ceiling} (depth=${QD} x npg=${npg})"

	# Hard cap: in-flight must never exceed the ceiling, regardless of offered load.
	if [ "$max_inflight" -gt "$ceiling" ]; then
		echo "FAIL: SSD_A in-flight ${max_inflight} exceeded ceiling ${ceiling} at offered=${od}"
		exit 1
	fi
	# Once offered concurrency is at/above the ceiling, the cap must actually bind (saturate).
	if [ "$od" -ge "$ceiling" ] && [ "$max_inflight" -eq "$ceiling" ]; then
		saturated=1
	fi
done
[ "$saturated" -eq 1 ] || { echo "FAIL: SSD_A in-flight never saturated at the configured ceiling"; exit 1; }
echo "Buffer-occupancy ceiling: PASS (in-flight plateaus at depth*num_poll_groups -> occupancy capped at depth*npg*IOSIZE)"

# --- Isolation: throttling SSD_A must not regress SSD_B latency ---
# Baseline: SSD_B alone (no contention).
fio --name=qd_b_base --filename="$ssdb_dev" --rw=randread --bs=${IOSIZE} \
    --iodepth=32 --numjobs=1 --runtime=8 --time_based --ioengine=libaio \
    --direct=1 --percentile_list=99.0 --output-format=json > /tmp/qdlimit_ssdb_base.json 2> /dev/null
base_p99=$(jq '.jobs[0].read.clat_ns.percentile["99.000000"]' /tmp/qdlimit_ssdb_base.json)

# Contended: SSD_B while SSD_A is hammered far beyond its cap.
fio --name=qd_b_cont --filename="$ssdb_dev" --rw=randread --bs=${IOSIZE} \
    --iodepth=32 --numjobs=1 --runtime=10 --time_based --ioengine=libaio \
    --direct=1 --percentile_list=99.0 --output-format=json > /tmp/qdlimit_ssdb_cont.json 2> /dev/null &
b_pid=$!
fio --name=qd_a_load --filename="$ssda_dev" --rw=randread --bs=${IOSIZE} \
    --iodepth=256 --numjobs=4 --runtime=10 --time_based --ioengine=libaio \
    --direct=1 &> /dev/null &
a_pid=$!
wait $b_pid
wait $a_pid
cont_p99=$(jq '.jobs[0].read.clat_ns.percentile["99.000000"]' /tmp/qdlimit_ssdb_cont.json)

echo "SSD_B p99(ns): baseline=${base_p99} under_SSD_A_overload=${cont_p99}"
# Isolation holds if SSD_B's p99 under contention stays within 3x its uncontended baseline.
# (A noisy-neighbour failure shows up as a much larger blow-up than 3x.)
if [ -z "$base_p99" ] || [ "$base_p99" = "null" ] || [ -z "$cont_p99" ] || [ "$cont_p99" = "null" ]; then
	echo "FAIL: could not measure SSD_B p99"
	exit 1
fi
if [ "$cont_p99" -gt $((base_p99 * 3)) ]; then
	echo "FAIL: SSD_B p99 regressed >3x under SSD_A overload (isolation broken)"
	exit 1
fi
echo "Noisy-neighbour isolation: PASS"

echo "qdlimit RDMA integration: PASS"
