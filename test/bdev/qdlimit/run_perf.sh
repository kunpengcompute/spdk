#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 SPDK contributors.
#  All rights reserved.
#
# Drive spdk_nvme_perf against a qdlimit-backed NVMe-oF/TCP target.
# Run this ON the initiator/host machine (Linux). Prints perf's latency report.
#
# Required env:
#   TARGET_IP   IPv4 of the target
# Optional env:
#   PORT        TCP port           (default: 4420)
#   NQN         subsystem NQN      (default: nqn.2016-06.io.spdk:qdlimit)
#   QDEPTH      perf queue depth   (default: 64)   # drive deeper than target cap
#   IOSIZE      IO size bytes      (default: 4096)
#   RUNTIME     seconds            (default: 15)
#   RW          workload           (default: randrw)
#   MIX         read percent       (default: 70)
#   CORES       perf core mask     (default: 0x1)

set -e

rootdir=$(readlink -f "$(dirname "$0")/../../..")

TARGET_IP=${TARGET_IP:?set TARGET_IP to the target machine IPv4}
PORT=${PORT:-4420}
NQN=${NQN:-nqn.2016-06.io.spdk:qdlimit}
QDEPTH=${QDEPTH:-64}
IOSIZE=${IOSIZE:-4096}
RUNTIME=${RUNTIME:-15}
RW=${RW:-randrw}
MIX=${MIX:-70}
CORES=${CORES:-0x1}

echo ">> reserving hugepages"
sudo sysctl -w vm.nr_hugepages=1024 >/dev/null

echo ">> perf qd=$QDEPTH io=$IOSIZE rw=$RW mix=$MIX t=${RUNTIME}s -> $TARGET_IP:$PORT"
sudo "$rootdir/build/examples/perf" \
	-q "$QDEPTH" -o "$IOSIZE" -w "$RW" -M "$MIX" -t "$RUNTIME" -c "$CORES" \
	-r "trtype:TCP adrfam:IPv4 traddr:$TARGET_IP trsvcid:$PORT subnqn:$NQN"
