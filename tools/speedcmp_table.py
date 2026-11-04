#!/usr/bin/env python3
# Riptide - Markdown table of tests/amiga/run_fsuae_speedcmp.sh reports.
# SPDX-License-Identifier: GPL-2.0-only
#
#   tools/speedcmp_table.py [--label N=TEXT ...] A.json B.json ...
#
# Each JSON file is one stack's run (medians of its transfers). Prints one
# row per stack, best values in bold: TCP download and upload (KB/s), CPU
# left idle during them, UDP datagrams per second sent and how many of them
# the host received, loopback TCP (KB/s), the host's ping round trip, and
# the notes of runs that did not finish. --label N=TEXT names column N
# (from 0) instead of the stack's own id string.
import json
import sys

COLS = [('download_kbs', 'Download KB/s', max), ('upload_kbs', 'Upload KB/s', max),
        ('udp_dgram_s', 'UDP dgram/s', max), ('loopback_kbs', 'Loopback KB/s', max),
        ('ping_ms', 'Ping ms', min)]


def label(r):
    """the stack's name for the table: its library id string, shortened"""
    s = r.get('id') or r.get('stack') or '?'
    s = s.split('(')[0].strip()
    if len(s) > 40:
        s = s[:40].rstrip() + '...'
    return s


def fmt(v, key):
    if v is None:
        return 'n/a'
    if key == 'udp_dgram_s':
        return '%d' % v
    return '%.1f' % v


def best(reports, key, pick):
    vals = [r.get(key) for r in reports if r.get(key) is not None]
    return pick(vals) if vals else None


def table(reports, labels=None):
    """the rows of the Markdown table (a list of strings)"""
    labels = labels or {}
    head = ['Stack'] + [c[1] for c in COLS] + ['CPU idle down/up', 'UDP received', 'Notes']
    out = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    bests = {key: best(reports, key, pick) for key, _, pick in COLS}
    for i, r in enumerate(reports):
        cells = [labels.get(str(i), label(r))]
        for key, _, _ in COLS:
            v = r.get(key)
            s = fmt(v, key)
            if v is not None and v == bests[key] and len(reports) > 1:
                s = '**' + s + '**'
            cells.append(s)
        idle = (r.get('idle_download_pct'), r.get('idle_upload_pct'))
        cells.append('%s / %s' % tuple('%.0f %%' % v if v is not None else 'n/a' for v in idle))
        if r.get('udp_received') is not None and r.get('udp_count'):
            cells.append('%d of %d' % (r['udp_received'], r['udp_count']))
        else:
            cells.append('n/a')
        cells.append('; '.join(r.get('notes') or []) or '')
        out.append('| ' + ' | '.join(cells) + ' |')
    return out


def main(argv):
    labels, files = {}, []
    i = 1
    while i < len(argv):
        if argv[i] == '--label' and i + 1 < len(argv) and '=' in argv[i + 1]:
            n, text = argv[i + 1].split('=', 1)
            labels[n] = text
            i += 2
        else:
            files.append(argv[i])
            i += 1
    if not files:
        sys.stderr.write('usage: speedcmp_table.py [--label N=TEXT ...] A.json B.json ...\n')
        return 2
    reports = [json.load(open(f)) for f in files]
    print('\n'.join(table(reports, labels)))
    b = reports[0].get('bytes')
    n = reports[0].get('repeat')
    if b and n:
        print('')
        print('Medians of %d transfers of %d bytes each way; KB/s = 1024 bytes per second of '
              'emulated time. UDP: %d datagrams of 1400 bytes, blocking sends. Ping: 10 echoes '
              'from the host while the Amiga is idle.' % (n, b, reports[0].get('udp_count') or 0))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
