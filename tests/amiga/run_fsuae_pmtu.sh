#!/bin/sh
# Riptide - TCP path MTU black hole detection (net.inet.tcp.mtu_probing,
# RFC 4821) on the real Amiga build: FS-UAE built with
# tools/fs-uae/fs-uae-3.2.35-riptide.patch (uaenet.device bridged through
# libpcap) on the veth end PM_IF; the other end PM_PEER sits in a network
# namespace with MTU 1400, a route that still advertises MSS 1460 and
# ICMP "fragmentation needed" dropped: Riptide's full sized frames vanish
# without a trace. The Amiga (MTU 1500) runs:
#   - nc: a 1 MB upload to the peer (active open) must complete
#   - net.inet.tcp.mtu_cache lists the peer
#   - nc: a 1 MB download (HTTP style) must complete
#   - nc -l: the peer pulls 1 MB from the Amiga (passive open, cache cleared)
#   - netstat -s -t shows the black hole and MTU probe counters
# (nc writes 2 KB at a time, too little queued data for upward MTU
# probes, so these connections stay at base_mss; t_mtup_wire/_tun test
# the probing.) PM_MTU=1500 runs the same without a black hole, for the
# times to compare.
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_pmtu.sh [BUILDDIR] [TIMEOUT]
#
# PM_IF, PM_PEER and PM_NET (the first three octets) choose other veth
# names and another subnet. Needs root, python3 and iptables. Headless:
# starts Xvfb on PM_DISPLAY (default :241) when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
PM_IF=${PM_IF:-rtpm0}
PM_PEER=${PM_PEER:-rtpm1}
PM_NET=${PM_NET:-192.168.170}
PM_DISPLAY=${PM_DISPLAY:-:241}
PM_MTU=${PM_MTU:-1400}
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
command -v iptables >/dev/null 2>&1 || { echo "SKIP: needs iptables"; exit 0; }
for f in Riptide rtcheck nc ping netstat sysctl; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ns=rtpm$$
ip netns add $ns || { echo "SKIP: cannot create a network namespace"; exit 0; }
ip link add $PM_IF type veth peer name $PM_PEER 2>/dev/null || { ip netns del $ns; echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del $PM_IF 2>/dev/null; ip netns del $ns 2>/dev/null; }
trap cleanup EXIT
ip link set $PM_PEER netns $ns
ip link set $PM_IF up
nsx() { ip netns exec $ns sh -c "$1"; }
# MTU 1400: larger frames from the Amiga are dropped by the veth; the
# route's advmss keeps the advertised MSS at 1460 so Riptide sends them
nsx "ip link set $PM_PEER mtu $PM_MTU up && ip link set lo up && ip addr add $PM_NET.1/24 dev $PM_PEER && \
     ip route replace $PM_NET.0/24 dev $PM_PEER advmss 1460 && \
     iptables -A OUTPUT -p icmp --icmp-type fragmentation-needed -j DROP" ||
	{ echo "SKIP: cannot set up the namespace"; exit 0; }
if command -v ethtool >/dev/null 2>&1; then
	ethtool -K $PM_IF tso off gso off gro off tx off rx off >/dev/null 2>&1
	nsx "ethtool -K $PM_PEER tso off gso off gro off tx off rx off" >/dev/null 2>&1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-pmtu.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck nc ping netstat sysctl; do cp "$BUILD/$f" "$dh0/C/"; done
head -c 1048576 /dev/urandom > "$dh0/S/up.bin"
head -c 1048576 /dev/urandom > "$work/down.bin"
printf 'GET /down.bin HTTP/1.0\r\n\r\n' > "$dh0/S/get"
cat > "$dh0/S/riptide.conf" <<CONF
hostname pmtu-test
interface eth0 device=uaenet.device unit=2
ifconfig eth0 $PM_NET.2/24 up
route add default $PM_NET.1
sysctl net.inet.tcp.mtu_probing=1
CONF
cat > "$dh0/S/Startup-Sequence" <<SEQ
FailAt 30
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/ping -c 2 $PM_NET.1 >DH0:out/ping
DH0:C/sysctl net.inet.tcp.mtu_probing net.inet.tcp.base_mss net.inet.tcp.probe_threshold net.inet.tcp.probe_interval >DH0:out/sysctl
DH0:C/nc $PM_NET.1 9000 <DH0:S/up.bin >DH0:out/up
DH0:C/sysctl net.inet.tcp.mtu_cache >DH0:out/cache
DH0:C/nc $PM_NET.1 8080 <DH0:S/get >DH0:out/body
DH0:C/sysctl -w net.inet.tcp.mtu_cache=clear >DH0:out/clear
DH0:C/nc -l -p 9100 <DH0:S/up.bin >DH0:out/pull
DH0:C/netstat -s -t >DH0:out/netstat
Echo done >DH0:out/zz_done
SEQ
cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = A1200
fast_memory = 8192
hard_drive_0 = $dh0
hard_drive_0_label = DH0
sound_card = none
floppy_drive_count = 0
automatic_input_grab = 0
video_sync = off
uae_sana2 = 1
uaenet_pcap = $PM_IF
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb $PM_DISPLAY -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=$PM_DISPLAY
	export DISPLAY
	sleep 1
fi
ip netns exec $ns python3 -I "$here/pmtu_peer.py" $PM_NET.1 $PM_NET.2 "$work" "$TIMEOUT" >"$work/peer.err" 2>&1 &
ppid_=$!
tpid=
if command -v tcpdump >/dev/null 2>&1; then
	tcpdump -i $PM_IF -s 96 -w "$work/pmtu.pcap" >/dev/null 2>&1 &
	tpid=$!
fi
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
sleep 1
kill $upid $ppid_ $tpid 2>/dev/null
wait $upid $ppid_ $tpid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null

fails=0
out() { tr -d '\r' < "$dh0/out/$1" 2>/dev/null; }
expect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" || { echo "FAIL $n: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
num() { out netstat | sed -n "s/^ *\([0-9]*\) $1.*/\1/p" | head -1; }
atleast() {
	v=$(num "$1")
	[ -n "$v" ] && [ "$v" -ge "$2" ] || { echo "FAIL netstat: '$1' is ${v:-missing}, expected >= $2"; fails=$((fails + 1)); }
}
same() {
	cmp -s "$1" "$2" || { echo "FAIL $3: the 1 MB differs ($(wc -c < "$1" 2>/dev/null || echo 0) bytes)"; fails=$((fails + 1)); }
}
expect ping '2 packets transmitted, 2 received'
expect sysctl 'mtu_probing = 1$' 'base_mss = 1024$' 'probe_threshold = 8$' 'probe_interval = 600$'
[ "$PM_MTU" -lt 1500 ] && expect cache "$PM_NET.1 mtu [0-9]+ age"
same "$work/up.bin" "$dh0/S/up.bin" upload
tail -c 1048576 "$dh0/out/body" > "$work/body.bin" 2>/dev/null
same "$work/body.bin" "$work/down.bin" download
same "$work/pull.bin" "$dh0/S/up.bin" "pull (passive open)"
[ "$PM_MTU" -lt 1500 ] && atleast 'MSS reductions on a suspected path MTU black hole' 2
expect netstat 'MTU probes acknowledged' 'MTU probes lost'
echo "---- peer (MTU $PM_MTU, ICMP dropped):"; sed 's/^/  /' "$work/peer.log" 2>/dev/null
echo "---- net.inet.tcp.mtu_cache after the upload:"; out cache | sed 's/^/  /'
echo "---- netstat -s -t:"
out netstat | grep -E 'MTU|black hole|retransmitted|timeouts' | sed 's/^/  /'
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_pmtu: PASSED"
	exit 0
fi
echo "run_fsuae_pmtu: FAILED ($fails)"
exit 1
