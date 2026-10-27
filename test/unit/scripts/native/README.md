# Supplemental macOS execution

The unit tests live in the existing SPDK CUnit directories and use the standard
SPDK Makefiles on Linux. These adapters additionally let a macOS host execute the
same eleven CUnit suites and produce LLVM line coverage. The entry point also
runs the Python and shell tests and requires 100% coverage of added executable
product lines against the fixed SPDK upstream revision:

```sh
CUNIT_PREFIX=/path/to/cunit-install \
RDMA_CORE_SOURCE=/path/to/rdma-core \
DEV_L0_OUTPUT_DIR=/path/to/coverage \
  test/unit/scripts/run_dev_l0_native.sh
```

Use official CUnit and rdma-core sources. `RDMA_INCLUDE` can select an already
prepared header directory instead of `RDMA_CORE_SOURCE`. No Linux RDMA library,
DPDK installation, device, or hugepages are needed for this supplemental run.
The script builds every suite from its original CUnit `main`, including the
original `nvmf.c` suite with the actual SPDK thread implementation.

`compat.h` substitutes pthread mutexes for the unavailable macOS spinlock type.
`calloc_adapter.c` supplies SPDK's allocator-failure mocking without GNU ld
`--wrap`. The other adapter sources provide the external RDMA, thread, and trace
symbols normally supplied by Linux libraries; the test's existing mocks and
completion-queue callbacks still control the tested behavior. They are not
linked into production or used by the standard Linux Makefiles. These runs do
not verify real RDMA devices, Linux memory mapping, or hardware latency.

Each suite emits its log, executable, raw and merged profile, and LCOV `.info`
file under `DEV_L0_OUTPUT_DIR`. Failures stop the script.
