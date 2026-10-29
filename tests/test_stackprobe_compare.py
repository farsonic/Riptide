#!/usr/bin/env python3
# Riptide - host test of tools/stackprobe_compare.py: counts, score, cells
# and the per-area tables from two made-up stackprobe reports.
# SPDX-License-Identifier: GPL-2.0-only
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import stackprobe_compare as sc  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('FAIL ' + what)


def test(id, res, value='', err=0):
    return {'id': id, 'desc': 'desc of ' + id, 'result': res, 'value': value, 'errno': err, 'ms': -1}


a = {'stack': {'name': 'StackA', 'version': '1.0', 'bsdsocket': '4.1'},
     'system': {'cpu': '68020', 'os': 'Kickstart 46.143'},
     'tests': [test('api.socket', 'PASS'), test('tcp.refused', 'FAIL', '', 54), test('ipv6.socket', 'UNSUPPORTED'),
               test('peer.tcp_echo', 'SKIPPED', 'no PEER='), test('tcp.backlog', 'INFO', '6 of 8'),
               test('perf.tcp_loopback', 'PASS', '512 KB/s')]}
b = {'stack': {'name': 'StackB', 'version': '3.0b2', 'bsdsocket': '3.28'},
     'system': {'cpu': '68020', 'os': 'Kickstart 46.143'},
     'tests': [test('api.socket', 'PASS'), test('tcp.refused', 'PASS'), test('ext.rs_bpf', 'UNSUPPORTED')]}

c = sc.counts(a)
check(c['PASS'] == 2 and c['FAIL'] == 1 and c['UNSUPPORTED'] == 1 and c['SKIPPED'] == 1, 'counts')
check(c['score'] == 50, 'score is passed / (passed + failed + unsupported): %d' % c['score'])
check(sc.counts(b)['score'] == 67, 'score rounds')
check(sc.cell(a['tests'][1]) == 'FAIL (ECONNRESET)', 'a FAIL cell names the errno')
check(sc.cell(a['tests'][2]) == '-', 'an unsupported cell')
check(sc.cell(a['tests'][4]) == '6 of 8', 'an INFO cell shows its value')
check(sc.cell(a['tests'][5]) == '512 KB/s', 'a perf cell shows its value')
check(sc.cell(None) == '', 'a missing test is empty')

md = sc.compare([a, b], {'1': 'B label'})
check('| StackA 1.0 | 4.1 | 2 | 1 | 1 | 1 | 50 % | 68020, Kickstart 46.143 |' in md, 'summary row')
check('| B label |' in md, 'a label replaces the column name')
check('#### tcp' in md and '#### ext' in md and '#### libs' not in md, 'only areas with tests')
lines = [ln for ln in md.splitlines() if ln.startswith('| `refused`')]
check(len(lines) == 1 and lines[0].endswith('| FAIL (ECONNRESET) | yes |'), 'the refused row: %s' % lines)
lines = [ln for ln in md.splitlines() if ln.startswith('| `rs_bpf`')]
check(len(lines) == 1 and lines[0].endswith('|  | - |'), 'a test only one stack ran')
check(md.index('#### api') < md.index('#### tcp') < md.index('#### ipv6') < md.index('#### perf'), 'area order')
check('#### tcp' not in sc.compare([a, b], areas=False), '--areas: summary only')

# the command line
with tempfile.TemporaryDirectory() as d:
    pa, pb = os.path.join(d, 'a.json'), os.path.join(d, 'b.json')
    json.dump(a, open(pa, 'w'))
    json.dump(b, open(pb, 'w'))
    r = subprocess.run([sys.executable, os.path.join(HERE, '..', 'tools', 'stackprobe_compare.py'), pa, pb],
                       capture_output=True, text=True)
    check(r.returncode == 0 and '| StackB 3.0b2 |' in r.stdout, 'the command line')

print('%s test_stackprobe_compare: %d failures' % ('ok  ' if not fails else 'FAIL', fails))
sys.exit(1 if fails else 0)
