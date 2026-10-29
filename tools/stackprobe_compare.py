#!/usr/bin/env python3
# Riptide - side-by-side Markdown table of stackprobe reports.
# SPDX-License-Identifier: GPL-2.0-only
#
#   tools/stackprobe_compare.py [--areas] [--label N=TEXT ...] A.json B.json ...
#
# Each JSON file is one stackprobe run (JSON=file). Prints a summary table
# (one row per stack: passed, failed, unsupported, skipped, score) and a
# feature table per area (one row per test, one column per stack). The
# score is the share of the tests that ran (not skipped, not information
# only) that passed. --areas leaves out the per-area tables.
import json
import sys

MARK = {'PASS': 'yes', 'FAIL': 'FAIL', 'UNSUPPORTED': '-', 'SKIPPED': 'skip', 'INFO': ''}
AREAS = ['libs', 'api', 'tags', 'netdb', 'tcp', 'udp', 'options', 'select', 'misc', 'ipv6', 'ext', 'perf', 'peer']


def load(path):
    with open(path, encoding='latin-1') as f:
        return json.load(f)


def column_name(r, labels, i):
    if str(i) in labels:
        return labels[str(i)]
    s = r['stack']
    return ('%s %s' % (s.get('name', '?'), s.get('version', ''))).strip()


def counts(r):
    c = {'PASS': 0, 'FAIL': 0, 'UNSUPPORTED': 0, 'SKIPPED': 0, 'INFO': 0}
    for t in r['tests']:
        c[t['result']] = c.get(t['result'], 0) + 1
    ran = c['PASS'] + c['FAIL'] + c['UNSUPPORTED']
    c['score'] = round(100.0 * c['PASS'] / ran) if ran else 0
    return c


def cell(t):
    """the table cell for one test result"""
    if t is None:
        return ''
    res, val, err = t['result'], t.get('value', ''), t.get('errno', 0)
    if res == 'INFO':
        return val.replace('|', '/')
    m = MARK.get(res, res)
    if res == 'PASS' and val and (t['id'].startswith('perf.') or t['id'].startswith('peer.') or
                                  t['id'].startswith('libs.') or t['id'].startswith('select.timeout')):
        return val.replace('|', '/')
    if res == 'FAIL' and err:
        return 'FAIL (%s)' % errname(err)
    return m


ERRNAMES = {4: 'EINTR', 9: 'EBADF', 13: 'EACCES', 22: 'EINVAL', 24: 'EMFILE', 32: 'EPIPE', 35: 'EWOULDBLOCK',
            36: 'EINPROGRESS', 38: 'ENOTSOCK', 40: 'EMSGSIZE', 42: 'ENOPROTOOPT', 43: 'EPROTONOSUPPORT',
            45: 'EOPNOTSUPP', 47: 'EAFNOSUPPORT', 48: 'EADDRINUSE', 49: 'EADDRNOTAVAIL', 51: 'ENETUNREACH',
            53: 'ECONNABORTED', 54: 'ECONNRESET', 55: 'ENOBUFS', 56: 'EISCONN', 57: 'ENOTCONN', 60: 'ETIMEDOUT',
            61: 'ECONNREFUSED', 65: 'EHOSTUNREACH', 78: 'ENOSYS'}


def errname(e):
    return ERRNAMES.get(e, 'errno %d' % e)


def table(rows, cols):
    out = ['| ' + ' | '.join(cols) + ' |', '|' + '---|' * len(cols)]
    for r in rows:
        out.append('| ' + ' | '.join(r) + ' |')
    return '\n'.join(out)


def compare(reports, labels=None, areas=True):
    labels = labels or {}
    names = [column_name(r, labels, i) for i, r in enumerate(reports)]
    out = []
    rows = []
    for name, r in zip(names, reports):
        c = counts(r)
        s = r.get('system', {})
        rows.append([name, r['stack'].get('bsdsocket', ''), str(c['PASS']), str(c['FAIL']), str(c['UNSUPPORTED']),
                     str(c['SKIPPED']), '%d %%' % c['score'],
                     ('%s, %s' % (s.get('cpu', ''), s.get('os', ''))).strip(', ')])
    out.append(table(rows, ['Stack', 'bsdsocket', 'Passed', 'Failed', 'Unsupported', 'Skipped', 'Score',
                            'Machine']))
    if not areas:
        return '\n'.join(out) + '\n'
    byid = [{t['id']: t for t in r['tests']} for r in reports]
    order = []
    for r in reports:
        for t in r['tests']:
            if t['id'] not in order:
                order.append(t['id'])
    for area in AREAS + sorted({i.split('.')[0] for i in order} - set(AREAS)):
        ids = [i for i in order if i.split('.')[0] == area]
        if not ids:
            continue
        rows = []
        for i in ids:
            desc = next((b[i]['desc'] for b in byid if i in b), i)
            rows.append(['`%s` %s' % (i.split('.', 1)[1], desc.replace('|', '/'))] + [cell(b.get(i)) for b in byid])
        out.append('\n#### %s\n' % area)
        out.append(table(rows, ['Test'] + names))
    return '\n'.join(out) + '\n'


def main(argv):
    labels, files, areas = {}, [], True
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--areas':
            areas = False
        elif a == '--label' and i + 1 < len(argv):
            k, _, v = argv[i + 1].partition('=')
            labels[k] = v
            i += 1
        else:
            files.append(a)
        i += 1
    if not files:
        print(__doc__ or 'usage: stackprobe_compare.py A.json B.json ...', file=sys.stderr)
        return 2
    sys.stdout.write(compare([load(f) for f in files], labels, areas))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
