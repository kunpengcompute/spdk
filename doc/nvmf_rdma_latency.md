# NVMf RDMA Latency Statistics

This branch adds RDMA target-side latency statistics to `nvmf_get_stats`.
The statistics are cumulative until the target process exits or
`nvmf_reset_stats` is called.

## Commands

Display per-poll-group and global averages:

```sh
python scripts/rpc.py nvmf_get_stats | python scripts/nvmf_rdma_latency.py
```

Query the RPC server directly from the summary script:

```sh
python scripts/nvmf_rdma_latency.py -s /var/tmp/spdk.sock
```

Reset cumulative statistics at runtime:

```sh
python scripts/rpc.py nvmf_reset_stats
```

If the target uses a non-default target name, pass it to both commands:

```sh
python scripts/rpc.py nvmf_reset_stats -t nvmf_tgt
python scripts/nvmf_rdma_latency.py -s /var/tmp/spdk.sock -t nvmf_tgt
```

## Output Meaning

The summary script prints these latency classes for both read and write:

`lifecycle`

Full target-side request lifetime. It starts when the target polls the incoming
RDMA command capsule and ends after the request and its data buffers are freed.

`data_rdma`

Data-transfer-only RDMA latency between host and target. It excludes the command
capsule receive path and the NVMe completion response. For reads, this measures
the target RDMA WRITE data path to the host buffer. For writes, this measures the
target RDMA READ path that pulls host data into the target buffer.

`bdev`

Backend bdev I/O latency. For an NVMe SSD bdev, this is the SSD read/write
latency as seen by the NVMf target, from bdev submission to bdev completion
callback.

## Notes

The global average is weighted by I/O count across poll groups. It is not a
simple average of poll-group averages.

If `nvmf_reset_stats` is called while I/O is in flight, completions that happen
after the reset are counted in the new window.

Small in-capsule writes do not have a separate data RDMA transfer, so they do
not contribute to `write.data_rdma`.
