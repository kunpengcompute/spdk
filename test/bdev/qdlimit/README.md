# qdlimit cross-machine perf test (NVMe-oF / TCP)

Measures how the `qdlimit` per-core queue-depth cap affects latency/throughput
over a real NVMe-oF/TCP path, using two machines:

- **target**: runs `spdk_tgt` exporting `Malloc0 -> QD0 (qdlimit) -> subsystem`
- **host/initiator**: runs `spdk_nvme_perf` (`build/examples/perf`) against the target

Both scripts are parameterized by environment variables (see headers).

## Run

On the **target** machine (pick a per-core depth; 0 = unlimited):

```bash
TARGET_IP=<target-ip> QD_DEPTH=8 test/bdev/qdlimit/nvmf_tcp_target.sh
# prints: TARGET READY nqn=... ip=... port=4420 qd_depth=8
```

On the **host/initiator** machine:

```bash
TARGET_IP=<target-ip> QDEPTH=64 IOSIZE=4096 RW=randrw MIX=70 RUNTIME=15 \
    test/bdev/qdlimit/run_perf.sh
```

To compare, restart the target with a different `QD_DEPTH` (e.g. `0` vs `8`) and
re-run perf. Drive `QDEPTH` (initiator) deeper than the target cap so the limiter
actually engages.

## Example result (2x multipass VMs, Ubuntu 22.04, TCP, malloc backend)

Initiator: `qd=64 randrw 70/30 4 KiB`, target on 2 cores.

| target `QD_DEPTH` | IOPS | avg lat (us) | max lat (us) |
|------------------:|-----:|-------------:|-------------:|
| 0 (unlimited)     | 4007 |        15993 |       183861 |
| 8                 | 5782 |        11101 |       153906 |

Capping per-core in-flight depth reduced average and tail latency for a fast
backend driven beyond its useful concurrency — the intended overload / tail
-latency control. Numbers are single noisy-VM runs; use longer `RUNTIME` and
multiple runs for rigorous measurement.
