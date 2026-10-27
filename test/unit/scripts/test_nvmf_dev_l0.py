#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Hardware-free tests for the dev_L0 NVMf Python RPC and latency additions."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import runpy
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'python'))

from spdk.rpc import nvmf  # noqa: E402


def load_latency():
    spec = importlib.util.spec_from_file_location(
        'nvmf_rdma_latency_ut', ROOT / 'scripts/nvmf_rdma_latency.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class NvmfRpcTest(unittest.TestCase):
    def test_reset_default_target(self):
        client = mock.Mock()
        self.assertIs(nvmf.nvmf_reset_stats(client), client.call.return_value)
        client.call.assert_called_once_with('nvmf_reset_stats', {})

    def test_reset_named_target(self):
        client = mock.Mock()
        nvmf.nvmf_reset_stats(client, 'target0')
        client.call.assert_called_once_with('nvmf_reset_stats', {'tgt_name': 'target0'})

    def test_qdlimit_set_depth_zero_and_nonzero(self):
        for depth in (0, 32):
            with self.subTest(depth=depth):
                client = mock.Mock()
                self.assertIs(nvmf.nvmf_qdlimit_set_depth(client, 'Nvme0n1', depth),
                              client.call.return_value)
                client.call.assert_called_once_with(
                    'nvmf_qdlimit_set_depth', {'bdev_name': 'Nvme0n1', 'depth': depth})

    def test_qdlimit_getters(self):
        for method in ('nvmf_qdlimit_get_depth', 'nvmf_qdlimit_get_stats'):
            with self.subTest(method=method):
                client = mock.Mock()
                self.assertIs(getattr(nvmf, method)(client, 'Nvme0n1'), client.call.return_value)
                client.call.assert_called_once_with(method, {'bdev_name': 'Nvme0n1'})

    def test_rpc_error_propagates(self):
        client = mock.Mock()
        client.call.side_effect = RuntimeError('target unavailable')
        with self.assertRaisesRegex(RuntimeError, 'target unavailable'):
            nvmf.nvmf_reset_stats(client)


class NvmfCliTest(unittest.TestCase):
    def run_cli(self, *arguments):
        output = io.StringIO()
        with mock.patch.object(sys, 'argv', ['rpc.py', '--dry-run', *arguments]), \
                mock.patch.object(sys, 'stdin', io.StringIO()), contextlib.redirect_stdout(output):
            runpy.run_path(str(ROOT / 'scripts/rpc.py'), run_name='__main__')
        text = output.getvalue()
        self.assertTrue(text.startswith('Request:\n'), text)
        return json.loads(text[len('Request:\n'):])

    def test_reset_commands(self):
        self.assertEqual(self.run_cli('nvmf_reset_stats'),
                         {'method': 'nvmf_reset_stats', 'params': {}})
        self.assertEqual(self.run_cli('nvmf_reset_stats', '-t', 'target0'),
                         {'method': 'nvmf_reset_stats', 'params': {'tgt_name': 'target0'}})

    def test_qdlimit_commands(self):
        for depth in (0, 19):
            self.assertEqual(self.run_cli('nvmf_qdlimit_set_depth', 'Nvme0n1', str(depth)),
                             {'method': 'nvmf_qdlimit_set_depth',
                              'params': {'bdev_name': 'Nvme0n1', 'depth': depth}})
        for method in ('nvmf_qdlimit_get_depth', 'nvmf_qdlimit_get_stats'):
            self.assertEqual(self.run_cli(method, 'Nvme0n1'),
                             {'method': method, 'params': {'bdev_name': 'Nvme0n1'}})

    def test_invalid_depth_is_rejected(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            self.run_cli('nvmf_qdlimit_set_depth', 'Nvme0n1', 'invalid')
        self.assertEqual(error.exception.code, 2)


class RdmaLatencyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.latency = load_latency()

    def device(self, count, total, minimum, maximum):
        result = {}
        for op in ('read', 'write'):
            for _, count_pattern, total_pattern, min_pattern, max_pattern in self.latency.LATENCY_CATEGORIES:
                for pattern, value in ((count_pattern, count), (total_pattern, total),
                                       (min_pattern, minimum), (max_pattern, maximum)):
                    result[pattern.format(op=op)] = value
        return result

    def data(self):
        return {'tick_rate': 1000000, 'poll_groups': [
            {'name': 'empty'},
            {'name': 'tcp', 'transports': [{'trtype': 'TCP'}]},
            {'name': 'rdma-empty', 'transports': [{'trtype': 'RDMA'}]},
            {'name': 'core0', 'transports': [
                {'trtype': 'TCP', 'devices': [{}]},
                {'trtype': 'RDMA', 'devices': [self.device(2, 20, 4, 16),
                                             self.device(0, 0, 0, 0)]}]},
            {'name': 'core1', 'transports': [
                {'trtype': 'RDMA', 'devices': [self.device(3, 30, 2, 18)]}]},
        ]}

    def test_conversion_and_empty_stats(self):
        self.assertEqual(self.latency._ticks_to_us(100, 2000000), 50.0)
        stats = self.latency._new_stats()
        self.assertEqual(len(stats), 24)
        self.assertEqual(stats['read_io_count'], 0)
        self.assertEqual(stats['min_write_bdev_latency_ticks'], sys.maxsize)

    def test_merge_counts_totals_and_extrema(self):
        stats = self.latency._new_stats()
        self.latency._merge_device_stats(stats, {})
        self.latency._merge_device_stats(stats, self.device(0, 99, 0, 99))
        self.latency._merge_device_stats(stats, self.device(2, 20, 4, 16))
        self.latency._merge_device_stats(stats, self.device(3, 30, 2, 18))
        self.assertEqual(stats, self.device(5, 50, 2, 18))

    def test_nonempty_and_zero_latency_output(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.latency._format_stats('core0', self.device(2, 20, 4, 16), 1000000)
            self.latency._format_stats('empty', self.latency._new_stats(), 1000000)
        text = output.getvalue()
        self.assertEqual(text.count('avg_us=10.00 min_us=4.00 max_us=16.00'), 6)
        self.assertEqual(text.count(': count=0'), 6)
        self.assertIn('read.data_rdma', text)
        self.assertIn('write.bdev', text)

    def test_json_bom_and_invalid_input(self):
        self.assertEqual(self.latency._load_json_text('\ufeff{"tick_rate": 1}'), {'tick_rate': 1})
        with self.assertRaises(json.JSONDecodeError):
            self.latency._load_json_text('invalid')

    def test_load_file_and_piped_stdin(self):
        args = mock.Mock(input=None)
        with mock.patch.object(sys, 'stdin', io.StringIO(json.dumps(self.data()))):
            self.assertEqual(self.latency._load_stats(args), self.data())
        with tempfile.TemporaryDirectory() as directory:
            args.input = str(Path(directory) / 'stats.json')
            Path(args.input).write_text('\ufeff' + json.dumps(self.data()), encoding='utf-8')
            self.assertEqual(self.latency._load_stats(args), self.data())

    def test_load_rpc_with_interactive_stdin(self):
        args = mock.Mock(input=None, server_addr='/tmp/ut.sock', port=4420,
                         timeout=1.5, verbose='INFO', conn_retries=3, tgt_name='target0')
        stdin = mock.Mock()
        stdin.isatty.return_value = True
        with mock.patch.object(sys, 'stdin', stdin), \
                mock.patch.object(self.latency, 'JSONRPCClient') as client_type, \
                mock.patch.object(self.latency.rpc_nvmf, 'nvmf_get_stats', return_value=self.data()) as get_stats:
            client = client_type.return_value.__enter__.return_value
            self.assertEqual(self.latency._load_stats(args), self.data())
            client_type.assert_called_once_with('/tmp/ut.sock', 4420, 1.5,
                                                log_level='INFO', conn_retries=3)
            get_stats.assert_called_once_with(client, tgt_name='target0')
            client_type.return_value.__exit__.assert_called_once()

    def test_validate_mixed_transports_and_missing_fields(self):
        self.latency._validate_stats(self.data())
        for data in ({}, {'tick_rate': 1}, {'poll_groups': []}):
            with self.subTest(data=data), self.assertRaisesRegex(ValueError, 'not nvmf_get_stats'):
                self.latency._validate_stats(data)
        for device in ({}, {'read_io_count': 0}):
            data = {'tick_rate': 1, 'poll_groups': [{'transports': [
                {'trtype': 'RDMA', 'devices': [device]}]}]}
            with self.subTest(device=device), self.assertRaisesRegex(ValueError, 'latency fields are missing'):
                self.latency._validate_stats(data)

    def test_main_aggregates_multiple_poll_groups(self):
        output = io.StringIO()
        with mock.patch.object(sys, 'argv', ['nvmf_rdma_latency.py']), \
                mock.patch.object(sys, 'stdin', io.StringIO(json.dumps(self.data()))), \
                contextlib.redirect_stdout(output):
            self.latency.main()
        text = output.getvalue()
        self.assertIn('poll_group=core0', text)
        self.assertIn('poll_group=core1', text)
        self.assertNotIn('poll_group=empty', text)
        self.assertNotIn('poll_group=tcp', text)
        self.assertEqual(text.count('count=5 avg_us=10.00 min_us=2.00 max_us=18.00'), 6)

    def test_main_script_entry_with_empty_data(self):
        output = io.StringIO()
        with mock.patch.object(sys, 'argv', ['nvmf_rdma_latency.py']), \
                mock.patch.object(sys, 'stdin', io.StringIO('{"tick_rate": 1000000, "poll_groups": []}')), \
                contextlib.redirect_stdout(output):
            runpy.run_path(str(ROOT / 'scripts/nvmf_rdma_latency.py'), run_name='__main__')
        self.assertTrue(output.getvalue().startswith('global:\n'))
        self.assertEqual(output.getvalue().count(': count=0'), 6)


if __name__ == '__main__':
    unittest.main()
