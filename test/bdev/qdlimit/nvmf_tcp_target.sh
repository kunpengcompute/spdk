#!/usr/bin/env bash
#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (C) 2026 SPDK contributors.
#  All rights reserved.
#
# Bring up an NVMe-oF/TCP target that exports:  Malloc0 -> QD0 (qdlimit) -> subsystem
# Run this ON the target machine (Linux). Leaves spdk_tgt running in the background
# and prints "TARGET READY ..." with the connection parameters.
#
# Env knobs:
#   TARGET_IP   IPv4 to listen on        (default: first global-scope IPv4)
#   PORT        TCP port                 (default: 4420)
#   QD_DEPTH    per-core queue depth     (default: 0 = unlimited / pass-through)
#   MALLOC_MB   backing malloc size MiB  (default: 1024)
#   BLK_SIZE    block size bytes         (default: 4096)
#   TGT_CORES   spdk_tgt core mask       (default: 0x3 = 2 cores)

set -e

rootdir=$(readlink -f "$(dirname "$0")/../../..")
rpc_py="sudo $rootdir/scripts/rpc.py"

TARGET_IP=${TARGET_IP:-$(ip -4 -o addr show scope global | awk '{print $4}' | cut -d/ -f1 | head -1)}
PORT=${PORT:-4420}
QD_DEPTH=${QD_DEPTH:-0}
MALLOC_MB=${MALLOC_MB:-1024}
BLK_SIZE=${BLK_SIZE:-4096}
TGT_CORES=${TGT_CORES:-0x3}
NQN=nqn.2016-06.io.spdk:qdlimit

echo ">> reserving hugepages"
sudo sysctl -w vm.nr_hugepages=1024 >/dev/null

echo ">> starting spdk_tgt (cores=$TGT_CORES)"
# setsid so the target survives the launching shell/SSH session closing.
sudo setsid "$rootdir/build/bin/spdk_tgt" -m "$TGT_CORES" </dev/null &>/tmp/spdk_tgt.log &
tgt_pid=$!

echo ">> waiting for RPC socket"
for _ in $(seq 1 40); do
	$rpc_py framework_get_config &>/dev/null && break
	sleep 0.5
done

echo ">> building Malloc0 -> QD0 (qdlimit, depth=$QD_DEPTH) -> $NQN"
$rpc_py bdev_malloc_create -b Malloc0 "$MALLOC_MB" "$BLK_SIZE"
$rpc_py bdev_qdlimit_create -b Malloc0 -p QD0 -d "$QD_DEPTH"
$rpc_py nvmf_create_transport -t tcp -u 8192
$rpc_py nvmf_create_subsystem "$NQN" -a -s SPDKQD1
$rpc_py nvmf_subsystem_add_ns "$NQN" QD0
$rpc_py nvmf_subsystem_add_listener "$NQN" -t tcp -a "$TARGET_IP" -s "$PORT"

echo "TARGET READY nqn=$NQN ip=$TARGET_IP port=$PORT qd_depth=$QD_DEPTH tgt_pid=$tgt_pid"
