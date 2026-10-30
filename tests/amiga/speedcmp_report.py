#!/usr/bin/env python3
# Riptide - the JSON report of one tests/amiga/run_fsuae_speedcmp.sh run,
# from the files the Amiga wrote (OUT) and the host's logs (WORK):
#
#   speedcmp_report.py OUT WORK DEST.json STACK BYTES REPEAT UDPCOUNT [ONLINE_S]
#
# Prints one summary line. Python 3 standard library only.
# SPDX-License-Identifier: GPL-2.0-only
import json
import os
import re
import statistics
import sys


def read(p):
    try:
        return open(p, 'rb').read().decode('latin-1').replace('\r', '')
    except OSError:
        return ''


def med(v):
    return statistics.median(v) if v else None


def report(out, work, stack, nbytes, repeat, udpcount, online):
    nb = read(os.path.join(out, 'nb.txt'))
    runs = {'download': [], 'upload': []}
    idle = {'download': [], 'upload': []}
    last = None
    for line in nb.split('\n'):
        m = re.match(r'netbench: (\S+) (download|upload) (\d+) bytes ([\d.]+) s ([\d.]+) KB/s', line)
        if m and int(m.group(3)) == nbytes:
            runs[m.group(2)].append(float(m.group(5)))
            last = m.group(2)
            continue
        m = re.match(r'netbench: \S+ cpu idle ([\d.]+) %', line)
        if m and last:
            idle[last].append(float(m.group(1)))
            last = None
    udptxt = read(os.path.join(out, 'udp.txt'))
    udp = re.search(r'udpblast: udp (\d+) sent (\d+) bytes ([\d.]+) s (\d+) dgram/s ([\d.]+) KB/s', udptxt)
    udp_err = re.search(r'errors (\d+), waits (\d+)', udptxt)
    sink = re.findall(r'burst \d+: (\d+) datagrams, (\d+) payload bytes', read(os.path.join(work, 'sink.log')))
    loop = re.search(r'netbench: loop loopback (\d+) bytes ([\d.]+) s ([\d.]+) KB/s', read(os.path.join(out, 'loop.txt')))
    ping = read(os.path.join(work, 'ping.txt'))
    pm = re.search(r'= ([\d.]+)/([\d.]+)/([\d.]+)', ping)
    pl = re.search(r'(\d+)% packet loss', ping)
    ready = re.search(r'stack ready: (.*)', read(os.path.join(out, 'ready')))
    r = {
        'stack': stack,
        'id': ready.group(1).strip() if ready else None,
        'bytes': nbytes, 'repeat': repeat,
        'online_s': online,
        'download_kbs': med(runs['download']), 'download_runs': runs['download'],
        'upload_kbs': med(runs['upload']), 'upload_runs': runs['upload'],
        'idle_download_pct': med(idle['download']), 'idle_upload_pct': med(idle['upload']),
        'udp_dgram_s': int(udp.group(4)) if udp else None,
        'udp_kbs': float(udp.group(5)) if udp else None,
        'udp_sent': int(udp.group(1)) if udp else None,
        'udp_errors': int(udp_err.group(1)) if udp_err else None,
        'udp_received': max(int(s[0]) for s in sink) if sink else None,
        'udp_count': udpcount,
        'loopback_kbs': float(loop.group(3)) if loop else None,
        'ping_ms': float(pm.group(2)) if pm else None,
        'ping_min_ms': float(pm.group(1)) if pm else None,
        'ping_loss_pct': int(pl.group(1)) if pl else None,
        'notes': [],
    }
    if not os.path.exists(os.path.join(out, 'up')):
        r['notes'].append('bsdsocket.library never opened')
    elif online is None:
        r['notes'].append('never answered a ping from the host')
    if len(runs['download']) < repeat or len(runs['upload']) < repeat:
        r['notes'].append('%d of %d downloads and %d of %d uploads completed' %
                          (len(runs['download']), repeat, len(runs['upload']), repeat))
    if not os.path.exists(os.path.join(out, 'zz_done')):
        r['notes'].append('did not finish')
    return r


def summary(r):
    f = lambda v: '%.1f' % v if v is not None else 'n/a'
    return 'speedcmp: %s: download %s KB/s, upload %s KB/s, UDP %s dgram/s (%s of %d received), loopback %s KB/s, ping %s ms%s' % (
        r['stack'], f(r['download_kbs']), f(r['upload_kbs']), r['udp_dgram_s'] or 'n/a',
        r['udp_received'] if r['udp_received'] is not None else 'n/a', r['udp_count'], f(r['loopback_kbs']), f(r['ping_ms']),
        ('; ' + '; '.join(r['notes'])) if r['notes'] else '')


def main(argv):
    if len(argv) < 8:
        sys.stderr.write(__doc__ or 'usage: speedcmp_report.py OUT WORK DEST.json STACK BYTES REPEAT UDPCOUNT [ONLINE_S]\n')
        return 2
    out, work, dest, stack = argv[1:5]
    nbytes, repeat, udpcount = int(argv[5]), int(argv[6]), int(argv[7])
    online = int(argv[8]) if len(argv) > 8 and argv[8] else None
    r = report(out, work, stack, nbytes, repeat, udpcount, online)
    open(dest, 'w').write(json.dumps(r, indent=1) + '\n')
    print(summary(r))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
