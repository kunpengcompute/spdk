#!/usr/bin/env python3
#  SPDX-License-Identifier: BSD-3-Clause

import argparse
import json
import os
import sys


sys.path.append(os.path.join(os.path.dirname(__file__), "..", "python"))

import spdk.rpc.nvmf as rpc_nvmf  # noqa: E402
from spdk.rpc.client import JSONRPCClient  # noqa: E402


def _ticks_to_us(ticks, tick_rate):
    return ticks * 1000000.0 / tick_rate


LATENCY_CATEGORIES = [
    ("lifecycle", "{op}_io_count", "{op}_latency_ticks",
     "min_{op}_latency_ticks", "max_{op}_latency_ticks"),
    ("data_rdma", "{op}_data_rdma_io_count", "{op}_data_rdma_latency_ticks",
     "min_{op}_data_rdma_latency_ticks", "max_{op}_data_rdma_latency_ticks"),
    ("bdev", "{op}_bdev_io_count", "{op}_bdev_latency_ticks",
     "min_{op}_bdev_latency_ticks", "max_{op}_bdev_latency_ticks"),
]


def _field(pattern, op):
    return pattern.format(op=op)


def _format_latency(op, category, stats, tick_rate):
    name, count_pattern, total_pattern, min_pattern, max_pattern = category
    label = f"{op}.{name}"
    count_key = _field(count_pattern, op)
    total_key = _field(total_pattern, op)
    min_key = _field(min_pattern, op)
    max_key = _field(max_pattern, op)

    if stats[count_key]:
        print("  %-16s: count=%d avg_us=%.2f min_us=%.2f max_us=%.2f" % (
            label,
            stats[count_key],
            _ticks_to_us(stats[total_key], tick_rate) / stats[count_key],
            _ticks_to_us(stats[min_key], tick_rate),
            _ticks_to_us(stats[max_key], tick_rate),
        ))
    else:
        print("  %-16s: count=0" % label)


def _format_stats(name, stats, tick_rate):
    for op in ("read", "write"):
        for category in LATENCY_CATEGORIES:
            _format_latency(op, category, stats, tick_rate)


def _merge_device_stats(dst, dev):
    for op in ("read", "write"):
        for _, count_pattern, total_pattern, min_pattern, max_pattern in LATENCY_CATEGORIES:
            count_key = _field(count_pattern, op)
            total_key = _field(total_pattern, op)
            min_key = _field(min_pattern, op)
            max_key = _field(max_pattern, op)

            io_count = dev.get(count_key, 0)
            if io_count:
                dst[count_key] += io_count
                dst[total_key] += dev[total_key]
                dst[min_key] = min(dst[min_key], dev[min_key])
                dst[max_key] = max(dst[max_key], dev[max_key])


def _new_stats():
    stats = {}
    for op in ("read", "write"):
        for _, count_pattern, total_pattern, min_pattern, max_pattern in LATENCY_CATEGORIES:
            stats[_field(count_pattern, op)] = 0
            stats[_field(total_pattern, op)] = 0
            stats[_field(min_pattern, op)] = sys.maxsize
            stats[_field(max_pattern, op)] = 0
    return stats


def _load_stats_from_rpc(args):
    with JSONRPCClient(
        args.server_addr,
        args.port,
        args.timeout,
        log_level=args.verbose,
        conn_retries=args.conn_retries,
    ) as client:
        return rpc_nvmf.nvmf_get_stats(client, tgt_name=args.tgt_name)


def _load_json_text(text):
    return json.loads(text.lstrip("\ufeff"))


def _load_stats(args):
    if args.input:
        with open(args.input, "r", encoding="utf-8-sig") as fh:
            return _load_json_text(fh.read())

    if not sys.stdin.isatty():
        return _load_json_text(sys.stdin.read())

    return _load_stats_from_rpc(args)


def _validate_stats(data):
    if "tick_rate" not in data or "poll_groups" not in data:
        raise ValueError("input is not nvmf_get_stats JSON")

    for poll_group in data["poll_groups"]:
        for transport in poll_group.get("transports", []):
            if transport.get("trtype") != "RDMA":
                continue
            for device in transport.get("devices", []):
                if "read_io_count" not in device or "write_bdev_io_count" not in device:
                    raise ValueError("RDMA latency fields are missing in nvmf_get_stats output")


def main():
    parser = argparse.ArgumentParser(
        description="Summarize RDMA read/write lifecycle latency from nvmf_get_stats output."
    )
    parser.add_argument(
        "-i", "--input",
        help="Read nvmf_get_stats JSON from file. If omitted, read stdin when piped; otherwise query RPC."
    )
    parser.add_argument(
        "-s", "--server-addr",
        default="/var/tmp/spdk.sock",
        help="RPC domain socket path or IP address. Default: /var/tmp/spdk.sock"
    )
    parser.add_argument(
        "-p", "--port",
        default=5260,
        type=int,
        help="RPC port number when server-addr is an IP address. Default: 5260"
    )
    parser.add_argument(
        "--timeout",
        default=60.0,
        type=float,
        help="Timeout in seconds waiting for RPC response. Default: 60.0"
    )
    parser.add_argument(
        "-r", "--conn-retries",
        default=0,
        type=int,
        help="Retry connecting to the RPC server N times with 0.2s interval. Default: 0"
    )
    parser.add_argument(
        "-v", "--verbose",
        choices=["DEBUG", "INFO", "ERROR"],
        default="ERROR",
        help="RPC client log level. Default: ERROR"
    )
    parser.add_argument(
        "-t", "--tgt-name",
        help="The name of the parent NVMe-oF target when querying RPC directly"
    )
    args = parser.parse_args()

    data = _load_stats(args)
    _validate_stats(data)

    global_stats = _new_stats()
    tick_rate = data["tick_rate"]

    for poll_group in data["poll_groups"]:
        poll_group_stats = _new_stats()
        for transport in poll_group.get("transports", []):
            if transport.get("trtype") != "RDMA":
                continue
            for device in transport.get("devices", []):
                _merge_device_stats(poll_group_stats, device)

        if poll_group_stats["read_io_count"] or poll_group_stats["write_io_count"]:
            print(f"poll_group={poll_group['name']}")
            _format_stats(poll_group["name"], poll_group_stats, tick_rate)
            _merge_device_stats(global_stats, poll_group_stats)

    print("global:")
    _format_stats("global", global_stats, tick_rate)


if __name__ == "__main__":
    main()
