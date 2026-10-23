#!/bin/sh
# Riptide - speed lab: TCP throughput of Amiga TCP/IP stacks on FS-UAE,
# bridged through tools/fs-uae (libpcap) to a veth pair, so no slirp and no
# NAT sit in the path. For each stack and CPU model one FS-UAE boot runs:
#   a warm-up, REPEAT downloads and REPEAT uploads of BYTES with
#   tests/amiga/netbench.c (bsdsocket.library calls only, data thrown away
#   or generated: no disk in the path) against tests/amiga/netbench_host.py
#   on the host end; for Riptide one more download and upload each under
#   the profiler (Riptide PROFILE, not counted in the medians); ping.
# Results: OUT/results.md (table), OUT/results.json (medians, all runs);
# with BASELINE=file.json the run fails when a median is more than 10 %
# (TOLERANCE) below the baseline's.
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_netbench.sh [BUILDDIR]
#
# Environment:
#   CPUS       CPU presets (default "a1200-020 a1200-030 a4000-040 a4000-060"):
#              cycle-exact CPU timing (UAE approximates 040/060 caches) at
#              14, 25, 25 and 50 MHz, no JIT, Fast RAM, no MMU emulation;
#              a4000-040-max/a4000-060-max run at host speed instead
#   STACKS     riptide, amitcp (default: riptide, plus amitcp if AMITCP_DIR)
#   AMITCP_DIR the unpacked AmiTCP-3.0b2 directory (Aminet
#              comm/net/AmiTCP-bin-30b2.lha, GPL-2.0); never committed
#   BYTES      bytes per transfer (default 2097152), REPEAT (default 3)
#   OUT        result directory (default BUILDDIR/netbench)
#   BASELINE   JSON to compare with, TOLERANCE percent (default 10)
#   TIMEOUT    seconds per FS-UAE boot (default 1200); UPTIME seconds
#              until the stack must be up (default 240)
#   NB_IF, NB_PEER, NB_NET  veth names and /24 (default rtnb0, rtnb1,
#              192.168.79), so several runs can share a host
#   KICKSTART  a Kickstart ROM image instead of FS-UAE's AROS ROM
#              (KICKSTART_EXT: its extension ROM, if any)
# Needs root (veth, packet capture), python3, ethtool (optional).
# Headless: starts Xvfb on :116 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
here=$(cd "$(dirname "$0")" && pwd)
CPUS=${CPUS:-"a1200-020 a1200-030 a4000-040 a4000-060"}
if [ -z "${STACKS:-}" ]; then
	STACKS=riptide
	[ -n "${AMITCP_DIR:-}" ] && STACKS="riptide amitcp"
fi
BYTES=${BYTES:-2097152}
REPEAT=${REPEAT:-3}
OUT=${OUT:-$BUILD/netbench-results}   # $BUILD/netbench is the program
TIMEOUT=${TIMEOUT:-1200}
TOLERANCE=${TOLERANCE:-10}
UPTIME=${UPTIME:-240}
NB_IF=${NB_IF:-rtnb0}
NB_PEER=${NB_PEER:-rtnb1}
NET=${NB_NET:-192.168.79}
PORT=9100

[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck netbench ping; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
case " $STACKS " in *" amitcp "*)
	[ -n "${AMITCP_DIR:-}" ] && [ -f "$AMITCP_DIR/AmiTCP" ] || { echo "SKIP: amitcp needs AMITCP_DIR (AmiTCP-3.0b2)"; exit 0; } ;;
esac

# FS-UAE settings of a CPU preset. FS-UAE runs 68030+ at host speed unless
# told otherwise; uae_cpu_multiplier counts 3.55 MHz steps. uae_mmu_model = 0:
# with the MMU emulated (68030/68060 presets), Riptide on uaenet.device
# hangs the machine at the first transfer; cause not yet known.
cpu_cfg() {
	case $1 in
	a1200-020) printf 'amiga_model = A1200\nfast_memory = 8192\n' ;;
	a1200-030) printf 'amiga_model = A1200\ncpu = 68030\nuae_mmu_model = 0\nuae_cpu_speed = real\nuae_cpu_cycle_exact = true\nuae_cpu_compatible = true\nuae_cpu_multiplier = 7\nfast_memory = 8192\n' ;;
	a4000-040) printf 'amiga_model = A4000/040\nuae_cpu_speed = real\nuae_cpu_cycle_exact = true\nuae_cpu_compatible = true\nuae_cpu_multiplier = 7\nfast_memory = 0\nzorro_iii_memory = 16384\n' ;;
	a4000-060) printf 'amiga_model = A4000/040\ncpu = 68060\nuae_mmu_model = 0\nuae_cpu_speed = real\nuae_cpu_cycle_exact = true\nuae_cpu_compatible = true\nuae_cpu_multiplier = 14\nfast_memory = 0\nzorro_iii_memory = 16384\n' ;;
	a4000-040-max) printf 'amiga_model = A4000/040\nfast_memory = 0\nzorro_iii_memory = 16384\n' ;;
	a4000-060-max) printf 'amiga_model = A4000/040\ncpu = 68060\nuae_mmu_model = 0\nfast_memory = 0\nzorro_iii_memory = 16384\n' ;;
	*) return 1 ;;
	esac
}

ip link del $NB_IF 2>/dev/null
ip link add $NB_IF type veth peer name $NB_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
xpid=
hpid=
cleanup() {
	[ -n "$hpid" ] && kill "$hpid" 2>/dev/null
	[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
	ip link del $NB_IF 2>/dev/null
}
trap cleanup EXIT
ip link set $NB_IF up
ip link set $NB_PEER up
ip addr add $NET.1/24 dev $NB_PEER
# no offloads: the capture must see real frames with real checksums
if command -v ethtool >/dev/null 2>&1; then
	for d in $NB_IF $NB_PEER; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :116 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:116
	export DISPLAY
	sleep 1
fi
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
python3 -I "$here/netbench_host.py" $NET.1 $PORT >"$OUT/host.log" 2>&1 &
hpid=$!
sleep 1

# one boot: run_one STACK CPU
run_one() {
	stack=$1 cpu=$2
	work=$OUT/$stack-$cpu
	rm -rf "$work"
	dh0=$work/dh0
	mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
	cp "$BUILD/netbench" "$BUILD/rtcheck" "$BUILD/ping" "$dh0/C/"
	nb="DH0:C/netbench -n $BYTES"
	{
		echo "FailAt 30"
		echo "Stack 65536"
		if [ "$stack" = riptide ]; then
			cp "$BUILD/Riptide" "$dh0/C/"
			cat > "$dh0/S/riptide.conf" <<-CONF
			hostname netbench
			interface eth0 device=uaenet.device unit=2
			ifconfig eth0 $NET.2/24 up
			route add default $NET.1
			mdns off
			ntp off
			CONF
			echo 'DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/stack.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start'
		else
			# AmiTCP 3.0b2 as installed by its Install_AmiTCP, minus the
			# installer: static address, uaenet.device unit 2 as uae/2
			cp -R "$AMITCP_DIR" "$dh0/AmiTCP"
			mkdir -p "$dh0/T" "$dh0/ENV"
			printf 'uae dev=uaenet.device\n' >> "$dh0/AmiTCP/db/interfaces"
			printf '%s.2 netbench\n%s.1 host\n' $NET $NET >> "$dh0/AmiTCP/db/hosts"
			printf 'HOST %s.2 netbench\n' $NET > "$dh0/AmiTCP/db/netdb-myhost"
			# netdb's WITH files are relative to the current directory
			sed -i 's#^WITH \([a-z.-]*\)#WITH AmiTCP:db/\1#' "$dh0/AmiTCP/db/netdb"
			# no Assign command on the AROS ROM: netbench -A/-a; and AmiTCP
			# will not start without rexxsyslib.library: netbench -R
			echo 'DH0:C/netbench -A AmiTCP=DH0:AmiTCP'
			echo 'DH0:C/netbench -a LIBS=DH0:AmiTCP/libs'
			echo 'DH0:C/netbench -a DEVS=DH0:AmiTCP/devs'
			echo 'DH0:C/netbench -A T=DH0:T'
			echo 'DH0:C/netbench -A ENV=DH0:ENV'
			echo 'DH0:C/netbench -R'
			echo 'Run >DH0:out/stack.log AmiTCP:AmiTCP'
			echo 'DH0:C/netbench -s 3'
			echo 'DH0:C/netbench -W 60 >DH0:out/start'
			echo 'AmiTCP:bin/ifconfig lo/0 127.0.0.1 >>DH0:out/start'
			echo "AmiTCP:bin/ifconfig uae/2 $NET.2 netmask 255.255.255.0 >>DH0:out/start"
			echo "AmiTCP:bin/route add default $NET.1 >>DH0:out/start"
		fi
		echo 'Echo up >DH0:out/up'
		echo "$nb -n 65536 -l warmup -d $NET.1 $PORT >DH0:out/warmup"
		i=1
		while [ $i -le "$REPEAT" ]; do
			echo "$nb -l run$i -d $NET.1 $PORT >>DH0:out/nb.txt"
			echo "$nb -l run$i -u $NET.1 $PORT >>DH0:out/nb.txt"
			i=$((i + 1))
		done
		if [ "$stack" = riptide ]; then
			echo 'DH0:C/Riptide PROFILE RESET >NIL:'
			echo 'DH0:C/Riptide PROFILE START >NIL:'
			echo "$nb -l profiled -d $NET.1 $PORT >>DH0:out/nb_prof.txt"
			echo 'DH0:C/Riptide PROFILE SHOW >DH0:out/prof_download'
			echo 'DH0:C/Riptide PROFILE RESET >NIL:'
			echo "$nb -l profiled -u $NET.1 $PORT >>DH0:out/nb_prof.txt"
			echo 'DH0:C/Riptide PROFILE SHOW >DH0:out/prof_upload'
			echo 'DH0:C/Riptide PROFILE STOP >NIL:'
			echo "DH0:C/ping -c 5 -i 0.5 $NET.1 >DH0:out/ping"
		else
			echo "AmiTCP:bin/ping -c 5 $NET.1 >DH0:out/ping"
		fi
		echo 'Echo done >DH0:out/zz_done'
	} > "$dh0/S/Startup-Sequence"
	{
		echo "[fs-uae]"
		cpu_cfg "$cpu"
		cat <<-CFG
		hard_drive_0 = $dh0
		hard_drive_0_label = DH0
		sound_card = none
		floppy_drive_count = 0
		automatic_input_grab = 0
		video_sync = off
		uae_sana2 = 1
		uaenet_pcap = $NB_IF
		base_dir = $work/fsuae
		CFG
		[ -n "${KICKSTART:-}" ] && echo "kickstart_file = $KICKSTART"
		[ -n "${KICKSTART_EXT:-}" ] && echo "kickstart_ext_file = $KICKSTART_EXT"
	} > "$work/test.fs-uae"
	SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
	upid=$!
	t=0
	while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
		kill -0 $upid 2>/dev/null || break
		# the stack is up (or the machine crashed) within UPTIME s
		if [ $t -ge "$UPTIME" ] && [ ! -f "$dh0/out/up" ]; then
			echo "WARN $stack $cpu: the stack did not come up in $UPTIME s"
			touch "$dh0/out/zz_done"
			break
		fi
		sleep 1
		t=$((t + 1))
	done
	sleep 1
	kill $upid 2>/dev/null
	wait $upid 2>/dev/null
	[ -f "$dh0/out/zz_done" ] || echo "WARN $stack $cpu: did not finish in $TIMEOUT s"
	echo "---- $stack $cpu: $t s"
	[ -f "$dh0/out/nb.txt" ] && tr -d '\r' < "$dh0/out/nb.txt" | sed 's/^/  /'
}

for stack in $STACKS; do
	for cpu in $CPUS; do
		cpu_cfg "$cpu" >/dev/null || { echo "unknown CPU preset '$cpu'"; exit 1; }
		run_one "$stack" "$cpu"
	done
done

python3 -I - "$OUT" "$BYTES" "$REPEAT" "${BASELINE:-}" "$TOLERANCE" "$STACKS" "$CPUS" <<'PY'
import json, os, re, statistics, sys
out, nbytes, repeat, baseline, tol = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], float(sys.argv[5])
stacks, cpus = sys.argv[6].split(), sys.argv[7].split()
LINE = re.compile(r'netbench: (\S+) (download|upload) (\d+) bytes ([\d.]+) s ([\d.]+) KB/s')

def read(p):
    try:
        return open(p, 'rb').read().decode('latin-1').replace('\r', '')
    except OSError:
        return ''

res = {'bytes': nbytes, 'repeat': repeat, 'results': {}}
md = ['| Stack | CPU | Download KB/s | Upload KB/s | Ping ms |', '|---|---|---|---|---|']
for st in stacks:
    for cpu in cpus:
        d = os.path.join(out, '%s-%s' % (st, cpu), 'dh0', 'out')
        runs = {'download': [], 'upload': []}
        for m in LINE.finditer(read(os.path.join(d, 'nb.txt'))):
            if int(m.group(3)) == nbytes:
                runs[m.group(2)].append(float(m.group(5)))
        ping = re.search(r'= [\d.]+/([\d.]+)/', read(os.path.join(d, 'ping')))
        r = {'download_kbs': statistics.median(runs['download']) if runs['download'] else None,
             'upload_kbs': statistics.median(runs['upload']) if runs['upload'] else None,
             'download_runs': runs['download'], 'upload_runs': runs['upload'],
             'ping_ms': float(ping.group(1)) if ping else None}
        for k in ('download', 'upload'):
            prof = read(os.path.join(d, 'prof_' + k))
            if prof:
                r['profile_' + k] = prof
        res['results'].setdefault(st, {})[cpu] = r
        f = lambda v: '%.1f' % v if v is not None else 'n/a'
        md.append('| %s | %s | %s | %s | %s |' % (st, cpu, f(r['download_kbs']), f(r['upload_kbs']), f(r['ping_ms'])))
md.append('')
md.append('Medians of %d transfers of %d bytes each way; KB/s = 1024 bytes per second of emulated time.' % (repeat, nbytes))
for st in stacks:
    for cpu in cpus:
        r = res['results'][st][cpu]
        for k in ('download', 'upload'):
            p = r.get('profile_' + k)
            if p:
                md += ['', '### Riptide profile, %s, %s' % (cpu, k), '', '```'] + p.rstrip().split('\n') + ['```']
open(os.path.join(out, 'results.json'), 'w').write(json.dumps(res, indent=1) + '\n')
open(os.path.join(out, 'results.md'), 'w').write('\n'.join(md) + '\n')
print('\n'.join(md[:2 + len(stacks) * len(cpus) + 2]))
fails = 0
missing = [(s, c) for s in stacks for c in cpus
           if res['results'][s][c]['download_kbs'] is None or res['results'][s][c]['upload_kbs'] is None]
for s, c in missing:
    print('FAIL %s %s: no complete result' % (s, c))
    fails += 1
if baseline:
    base = json.load(open(baseline))['results']
    for s in stacks:
        for c in cpus:
            for k in ('download_kbs', 'upload_kbs'):
                b, v = base.get(s, {}).get(c, {}).get(k), res['results'][s][c][k]
                if b and v is not None and v < b * (1 - tol / 100):
                    print('FAIL %s %s %s: %.1f KB/s, baseline %.1f (-%.0f %%)' % (s, c, k, v, b, 100 - 100 * v / b))
                    fails += 1
                elif b and v is not None:
                    print('ok   %s %s %s: %.1f KB/s, baseline %.1f (%+.0f %%)' % (s, c, k, v, b, 100 * v / b - 100))
print('results: %s/results.md, %s/results.json' % (out, out))
print('run_fsuae_netbench: %s' % ('FAILED (%d)' % fails if fails else 'PASSED'))
sys.exit(1 if fails else 0)
PY
