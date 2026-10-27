#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run hardware-free NVMf Python UTs and export real line hits as LCOV."""

import argparse
import json
from pathlib import Path
import trace
import unittest


ROOT = Path(__file__).resolve().parents[3]
SOURCES = ('scripts/nvmf_rdma_latency.py', 'python/spdk/rpc/nvmf.py', 'scripts/rpc.py')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='LCOV output filename')
    parser.add_argument('--json', type=Path, help='Optional per-file line hit JSON output')
    args = parser.parse_args()

    def run_tests():
        suite = unittest.defaultTestLoader.discover(
            str(ROOT / 'test/unit/scripts'), pattern='test_nvmf_dev_l0.py')
        return unittest.TextTestRunner(verbosity=2).run(suite)

    tracer = trace.Trace(count=True, trace=False)
    result = tracer.runfunc(run_tests)
    source_paths = {str(ROOT / source): source for source in SOURCES}
    hits = {source: {} for source in SOURCES}
    for (filename, line), count in tracer.results().counts.items():
        source = source_paths.get(str(Path(filename).resolve()))
        if source is not None:
            hits[source][line] = hits[source].get(line, 0) + count

    records = []
    report = {}
    for source in SOURCES:
        path = ROOT / source
        # Use the same executable-line discovery as Python's standard trace reporter;
        # it recursively inspects code objects and excludes comments and docstrings.
        # Python 3.11 can include a synthetic module RESUME event at line 0.
        # It has no source line and cannot be represented as a valid LCOV DA.
        executable = sorted(line for line in trace._find_executable_linenos(str(path)) if line > 0)
        counts = {line: hits[source].get(line, 0) for line in executable}
        covered = sum(count > 0 for count in counts.values())
        records.extend(('TN:nvmf_python_ut', 'SF:' + str(path)))
        records.extend(f'DA:{line},{count}' for line, count in counts.items())
        records.extend((f'LF:{len(counts)}', f'LH:{covered}', 'end_of_record'))
        report[source] = {'executable_lines': len(counts), 'covered_lines': covered,
                          'line_counts': counts}

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text('\n'.join(records) + '\n', encoding='utf-8')
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
