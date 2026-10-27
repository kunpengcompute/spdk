#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Execute the L0 background script safely against fake command boundaries.

Use --output FILE to export Bash xtrace observations as LCOV. The six-line
source has four executable command lines: 1, 3, 4 and 5. The blank line and
the closing `done` token have no independently executable command.
"""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'scripts/l0_precise_flow_control/start_background_job.sh'
EXECUTABLE_LINES = (1, 3, 4, 5)
LINE_HITS = Counter()

# The fake tools log independently by CPU to avoid races between background jobs.
# Every exec target resolves into the temporary directory created by the test.
FAKE_TOOL = r'''
import json
import os
from pathlib import Path
import sys

name = Path(sys.argv[0]).name
args = sys.argv[1:]
logs = Path(os.environ['SPDK_UT_LOGS'])
record = {'args': args, 'cwd': os.getcwd()}
if name == 'seq':
    assert args == ['16', '39'], args
    (logs / 'seq.json').write_text(json.dumps(record))
    print('\n'.join(str(cpu) for cpu in range(16, 40)))
elif name == 'nohup':
    assert args[0] == 'numactl', args
    cpu = args[2]
    (logs / ('nohup-' + cpu + '.json')).write_text(json.dumps(record))
    os.execv(str(Path(os.environ['SPDK_UT_BIN']) / 'numactl'), args)
elif name == 'numactl':
    assert args[:1] == ['-C'], args
    cpu = args[1]
    assert args[2:5] == ['-m', '0', './bw_mem'], args
    os.environ['SPDK_UT_CPU'] = cpu
    (logs / ('numactl-' + cpu + '.json')).write_text(json.dumps(record))
    os.execv('./bw_mem', args[4:])
elif name == 'bw_mem':
    assert args == ['-P', '1', '-W', '1', '-N', '100000', '16M', 'rd'], args
    cpu = os.environ['SPDK_UT_CPU']
    (logs / ('bw_mem-' + cpu + '.json')).write_text(json.dumps(record))
else:
    raise AssertionError(name)
'''


class BackgroundJobTest(unittest.TestCase):
    def test_cpu_affinity_arguments_and_benchmark_directory(self):
        bash = shutil.which('bash')
        self.assertIsNotNone(bash, 'bash is needed to execute the original script')
        with tempfile.TemporaryDirectory(prefix='spdk-l0-background-') as directory:
            temporary = Path(directory).resolve()
            tools, benchmark, logs = [temporary / item for item in ('bin', 'benchmark', 'logs')]
            for path in (tools, benchmark, logs):
                path.mkdir()
            for name in ('seq', 'nohup', 'numactl', 'bw_mem'):
                target = (benchmark if name == 'bw_mem' else tools) / name
                target.write_text('#!' + sys.executable + '\n' + FAKE_TOOL, encoding='utf-8')
                target.chmod(0o755)
            environment = os.environ.copy()
            environment.update({
                # All three external commands used by the script are fake executables.
                'PATH': str(tools),
                'SPDK_UT_BIN': str(tools),
                'SPDK_UT_LOGS': str(logs),
                # macOS Bash 3 truncates long PS4 expansions. Only this original
                # Bash source runs; every fake child tool is a Python executable.
                'PS4': '+SPDK_UT_TRACE:${LINENO}: ',
                'PYTHONNOUSERSITE': '1',
            })
            environment.pop('BASH_ENV', None)
            environment.pop('ENV', None)
            completed = subprocess.run(
                [bash, '-x', str(SOURCE), str(benchmark)], cwd=temporary,
                env=environment, text=True, capture_output=True, timeout=30, check=True)
            # subprocess waits for EOF on pipes inherited by background children, so
            # every fake benchmark has completed before their records are inspected.
            self.assertEqual(completed.stdout, '')
            records = {path.stem: json.loads(path.read_text()) for path in logs.glob('*.json')}
            expected_keys = {'seq'}
            for cpu in range(16, 40):
                expected_keys.update(f'{tool}-{cpu}' for tool in ('nohup', 'numactl', 'bw_mem'))
            self.assertEqual(set(records), expected_keys)
            self.assertEqual(records['seq']['args'], ['16', '39'])
            benchmark_args = ['-P', '1', '-W', '1', '-N', '100000', '16M', 'rd']
            for cpu in range(16, 40):
                affinity_args = ['-C', str(cpu), '-m', '0', './bw_mem'] + benchmark_args
                self.assertEqual(records[f'nohup-{cpu}']['args'], ['numactl'] + affinity_args)
                self.assertEqual(records[f'numactl-{cpu}']['args'], affinity_args)
                self.assertEqual(records[f'bw_mem-{cpu}']['args'], benchmark_args)
            for record in records.values():
                self.assertEqual(record['cwd'], str(benchmark))

            observed = Counter()
            # Bash 3 reports the closing `done` line for the nested command
            # substitution. Count top-level command locations; the enclosing
            # for command is traced correctly and seq is verified above.
            pattern = re.compile(r'^\+SPDK_UT_TRACE:(\d+): ', re.MULTILINE)
            for number in pattern.findall(completed.stderr):
                observed[int(number)] += 1
            self.assertEqual(set(observed), set(EXECUTABLE_LINES))
            self.assertEqual(observed[1], 1)
            self.assertEqual(observed[3], 1)
            self.assertEqual(observed[5], 24)
            LINE_HITS.update(observed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, help='Export observed source-line counts as LCOV')
    parser.add_argument('--json', type=Path, help='Optional JSON source-line counts')
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(BackgroundJobTest))
    counts = {line: LINE_HITS[line] for line in EXECUTABLE_LINES}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text('\n'.join([
            'TN:l0_background_job_ut', 'SF:' + str(SOURCE),
            *(f'DA:{line},{count}' for line, count in counts.items()),
            'LF:' + str(len(counts)),
            'LH:' + str(sum(count > 0 for count in counts.values())),
            'end_of_record', '',
        ]), encoding='utf-8')
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({'source': str(SOURCE), 'line_counts': counts}, indent=2)
                             + '\n', encoding='utf-8')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
