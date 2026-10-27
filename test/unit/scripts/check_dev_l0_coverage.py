#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fail closed on missing or uncovered dev_L0 product source coverage.

The denominator is added C/Python/shell lines that the coverage compiler/interpreter
marks executable, relative to the fixed upstream revision. Headers in this
change contain declarations only; tests, docs and performance harnesses are not
product code. Supply an initial zero-count LCOV capture as well as runtime data
on GCC builds, so functions that were never linked/run cannot disappear.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]
L0_COMMIT = '2ea99cf2d41b46680246134fd47a11ee05b37d33'
L0_SOURCES = frozenset((
    'lib/env_dpdk/env.c',
    'lib/env_dpdk/l0.c',
    'lib/env_dpdk/l0_mmap_platform.c',
    'lib/env_dpdk/memory.c',
    'lib/nvmf/transport.c',
    'lib/rdma/common.c',
))


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args], text=True)


def product_source(path):
    return ((path.startswith('lib/') and path.endswith('.c'))
            or (path.startswith('python/') and path.endswith('.py'))
            or path in ('scripts/rpc.py', 'scripts/nvmf_rdma_latency.py',
                        'scripts/l0_precise_flow_control/start_background_job.sh'))


def added_lines(base, head):
    result = {}
    current = None
    diff = git('diff', '--no-ext-diff', '--no-renames', '--unified=0',
               '--no-color', base, head, '--')
    for line in diff.splitlines():
        if line.startswith('+++ b/'):
            current = line[6:]
        elif line.startswith('@@') and current and product_source(current):
            match = re.search(r'\+(\d+)(?:,(\d+))? @@', line)
            if not match:
                raise ValueError('unrecognized diff hunk')
            start, count = int(match[1]), int(match[2] or 1)
            if count:
                result.setdefault(current, set()).update(range(start, start + count))
    return result


def load_lcov(paths, source_root):
    result = {}
    for path in paths:
        current = None
        for line in Path(path).read_text().splitlines():
            if line.startswith('SF:'):
                source = Path(line[3:])
                if source.is_absolute():
                    try:
                        source = source.relative_to(source_root)
                    except ValueError:
                        current = None
                        continue
                current = source.as_posix()
                result.setdefault(current, {})
            elif line.startswith('DA:') and current:
                fields = line[3:].split(',')
                number, hits = int(fields[0]), int(fields[1])
                if number <= 0 or hits < 0:
                    raise ValueError('invalid LCOV line data')
                counts = result[current]
                counts[number] = counts.get(number, 0) + hits
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scope', choices=('all', 'l0'), default='all',
                        help='l0: all six L0 C sources from commit ' + L0_COMMIT)
    parser.add_argument('--base')
    parser.add_argument('--head')
    parser.add_argument('--source-root', type=Path, default=ROOT,
                        help='source checkout prefix recorded in LCOV')
    parser.add_argument('--json', type=Path)
    parser.add_argument('lcov', nargs='+', type=Path)
    args = parser.parse_args()
    args.base = args.base or (L0_COMMIT + '^' if args.scope == 'l0' else 'v23.01.1')
    args.head = args.head or (L0_COMMIT if args.scope == 'l0'
                             else '67de5d5f7ae6fc970faa3a9add67db5e51594e82')
    additions = added_lines(args.base, args.head)
    if args.scope == 'l0':
        missing = L0_SOURCES.difference(additions)
        if missing:
            parser.error('L0 scope must include all six sources; no additions for: '
                         + ', '.join(sorted(missing)))
        additions = {path: lines for path, lines in additions.items() if path in L0_SOURCES}
    if not additions:
        parser.error('no added product source lines; refusing vacuous 100%%')
    dirty = git('diff', args.head, '--', *sorted(additions))
    if dirty:
        parser.error('product sources differ from --head; regenerate matching coverage')
    counts = load_lcov(args.lcov, args.source_root.resolve())
    rows = []
    for path, changed in sorted(additions.items()):
        data = counts.get(path, {})
        executable = sorted(changed.intersection(data))
        missed = [line for line in executable if data[line] == 0]
        missing = not executable
        rows.append({'path': path, 'added_source_lines': len(changed),
                     'executable_lines': len(executable),
                     'covered_lines': len(executable) - len(missed),
                     'uncovered_lines': missed, 'missing_coverage': missing,
                     'sha256': hashlib.sha256((ROOT / path).read_bytes()).hexdigest()})
        detail = 'MISSING COVERAGE' if missing else '{} / {}'.format(
            len(executable) - len(missed), len(executable))
        print('{}: {}{}'.format(path, detail, ' missed=' + str(missed) if missed else ''))
    failed = any(row['missing_coverage'] or row['uncovered_lines'] for row in rows)
    total = sum(row['executable_lines'] for row in rows)
    covered = sum(row['covered_lines'] for row in rows)
    report = {'scope': args.scope,
              'base': git('rev-parse', args.base + '^{commit}').strip(),
              'head': git('rev-parse', args.head + '^{commit}').strip(),
              'status': 'FAIL' if failed else 'PASS', 'files': rows,
              'covered_lines': covered, 'executable_lines': total,
              'coverage_percent': covered * 100.0 / total if total else None,
              'lcov_inputs': [str(path) for path in args.lcov]}
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2) + '\n')
    print('{}: {}/{} observed added executable lines; {} file(s) missing data'.format(
        report['status'], covered, total,
        sum(row['missing_coverage'] for row in rows)))
    return int(failed)


if __name__ == '__main__':
    sys.exit(main())
