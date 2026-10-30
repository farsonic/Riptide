#!/usr/bin/env python3
# Riptide - host test of tools/speedcmp_table.py: labels, best-value
# marking, missing values and notes from two made-up speedcmp reports.
# SPDX-License-Identifier: GPL-2.0-only
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
import speedcmp_table as st  # noqa: E402

fails = 0


def check(cond, what):
    global fails
    if not cond:
        fails += 1
        print('FAIL ' + what)


a = {'stack': 'riptide', 'id': 'Riptide 0.26 bsdsocket.library 4.1 (30.10.2026)', 'bytes': 2097152, 'repeat': 3,
     'download_kbs': 818.2, 'upload_kbs': 496.4, 'idle_download_pct': 0.0, 'idle_upload_pct': 0.0,
     'udp_dgram_s': 443, 'udp_received': 3000, 'udp_count': 3000, 'loopback_kbs': 1008.4,
     'ping_ms': 15.4, 'notes': []}
b = {'stack': 'amitcp', 'id': 'AmiTCP/IP 3.0b2 (bsdsocket.library 3.28)', 'bytes': 2097152, 'repeat': 3,
     'download_kbs': 300.0, 'upload_kbs': 600.0, 'idle_download_pct': 12.5, 'idle_upload_pct': None,
     'udp_dgram_s': None, 'udp_received': None, 'udp_count': 3000, 'loopback_kbs': None,
     'ping_ms': 9.9, 'notes': ['2 of 3 downloads and 3 of 3 uploads completed']}

check(st.label(a) == 'Riptide 0.26 bsdsocket.library 4.1', 'label drops the date: %r' % st.label(a))
check(st.label({'stack': 'x'}) == 'x', 'label falls back to the stack name')
rows = st.table([a, b])
check(len(rows) == 4, 'header, rule and two rows: %d' % len(rows))
check(rows[2].startswith('| Riptide 0.26 bsdsocket.library 4.1 |'), 'first row label')
check('**818.2**' in rows[2] and '**600.0**' in rows[3], 'best download and upload in bold')
check('**9.9**' in rows[3] and '**15.4**' not in rows[2], 'lowest ping in bold')
check('**443**' in rows[2], 'UDP rate as an integer, best in bold')
check('n/a' in rows[3] and '3000 of 3000' in rows[2], 'missing values and UDP received')
check('0 % / 0 %' in rows[2] and '12 % / n/a' in rows[3], 'idle cells')
check(rows[3].endswith('| 2 of 3 downloads and 3 of 3 uploads completed |'), 'notes cell')
rows = st.table([a, b], {'1': 'AmiTCP 3.0b2'})
check(rows[3].startswith('| AmiTCP 3.0b2 |'), '--label renames a column')
rows = st.table([a])
check('**' not in rows[2], 'no bold with one stack')

if fails:
    print('test_speedcmp_table: %d failures' % fails)
    sys.exit(1)
print('test_speedcmp_table: ok')
