#!/bin/sh
# Riptide - TCP connection features through bsdsocket.library on the real
# Amiga build: FS-UAE built with tools/fs-uae/fs-uae-3.2.35-riptide.patch
# (uaenet.device bridged through libpcap) on the veth end TF_IF; the other
# end TF_PEER sits in a network namespace with net.ipv4.tcp_fastopen=3,
# tests/amiga/tcpopt_peer.py and iptables rules. The Amiga runs nc:
#   - three requests with -F (TCP_FASTOPEN_CONNECT): the first gets a
#     cookie, the next two put their data in the SYN (the peer checks)
#   - net.inet.tcp.fastopen_cache lists the cookie
#   - connect to a port whose SYNs are dropped: "Operation timed out"
#     after net.inet.tcp.connect_timeout (4 s here)
#   - connect to a port answered with ICMP host unreachable: "No route to
#     host" after the second SYN
#   - -K 1 (SO_KEEPALIVE, TCP_KEEPIDLE and TCP_KEEPINTVL 1) on an idle
#     connection: probes
#   - netstat -s shows the counters
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_tcpopt.sh [BUILDDIR] [TIMEOUT]
#
# TF_IF, TF_PEER and TF_NET (the first three octets) choose other veth
# names and another subnet. Needs root, python3 and iptables. Headless:
# starts Xvfb on :119 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
TF_IF=${TF_IF:-rtf0}
TF_PEER=${TF_PEER:-rtf1}
TF_NET=${TF_NET:-192.168.101}
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
command -v iptables >/dev/null 2>&1 || { echo "SKIP: needs iptables"; exit 0; }
for f in Riptide rtcheck nc ping netstat sysctl; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ns=rttf$$
ip netns add $ns || { echo "SKIP: cannot create a network namespace"; exit 0; }
ip link add $TF_IF type veth peer name $TF_PEER 2>/dev/null || { ip netns del $ns; echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del $TF_IF 2>/dev/null; ip netns del $ns 2>/dev/null; }
trap cleanup EXIT
ip link set $TF_PEER netns $ns
ip link set $TF_IF up
nsx() { ip netns exec $ns sh -c "$1"; }
nsx "ip addr add $TF_NET.1/24 dev $TF_PEER && ip link set $TF_PEER up && ip link set lo up && \
     sysctl -qw net.ipv4.tcp_fastopen=3 && \
     iptables -A INPUT -p tcp --dport 9999 -j DROP && \
     iptables -A INPUT -p tcp --dport 9998 -j REJECT --reject-with icmp-host-unreachable" ||
	{ echo "SKIP: cannot set up the namespace"; exit 0; }
if command -v ethtool >/dev/null 2>&1; then
	ethtool -K $TF_IF tso off gso off gro off tx off rx off >/dev/null 2>&1
	nsx "ethtool -K $TF_PEER tso off gso off gro off tx off rx off" >/dev/null 2>&1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-tcpopt.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck nc ping netstat sysctl; do cp "$BUILD/$f" "$dh0/C/"; done
cat > "$dh0/S/riptide.conf" <<CONF
hostname tcpopt-test
interface eth0 device=uaenet.device unit=2
ifconfig eth0 $TF_NET.2/24 up
route add default $TF_NET.1
sysctl net.inet.tcp.connect_timeout=4
CONF
for i in 1 2 3; do printf 'GET %s\n' $i > "$dh0/S/req$i"; done
cat > "$dh0/S/Startup-Sequence" <<SEQ
FailAt 30
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/ping -c 2 $TF_NET.1 >DH0:out/ping
DH0:C/nc -F -w 5 $TF_NET.1 8300 <DH0:S/req1 >DH0:out/tfo1
DH0:C/nc -F -w 5 $TF_NET.1 8300 <DH0:S/req2 >DH0:out/tfo2
DH0:C/nc -F -w 5 $TF_NET.1 8300 <DH0:S/req3 >DH0:out/tfo3
DH0:C/sysctl net.inet.tcp.fastopen_cache >DH0:out/cache
DH0:C/sysctl net.inet.tcp.connect_timeout net.inet.tcp.syn_rto_initial_ms net.inet.tcp.syn_rto_max_ms net.inet.tcp.keepalive_all net.inet.tcp.fastopen >DH0:out/sysctl
DH0:C/nc -w 1 $TF_NET.1 9999 <NIL: >DH0:out/dead
DH0:C/nc -w 1 $TF_NET.1 9998 <NIL: >DH0:out/unreach
DH0:C/nc -K 1 -d -w 5 $TF_NET.1 8301 >DH0:out/ka
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
uaenet_pcap = $TF_IF
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :119 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:119
	export DISPLAY
	sleep 1
fi
ip netns exec $ns python3 -I "$here/tcpopt_peer.py" $TF_NET.1 "$work/peer.log" "$TIMEOUT" >"$work/peer.err" 2>&1 &
ppid_=$!
tpid=
if command -v tcpdump >/dev/null 2>&1; then
	tcpdump -i $TF_IF -s 200 -w "$work/tcpopt.pcap" >/dev/null 2>&1 &
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
expect ping '2 packets transmitted, 2 received'
expect tfo1 '^OK GET 1$'
expect tfo2 '^OK GET 2$'
expect tfo3 '^OK GET 3$'
grep -q '^tfo 1 syn_data=0 line=GET 1$' "$work/peer.log" 2>/dev/null || { echo "FAIL peer: the first request came in the SYN or not at all"; fails=$((fails + 1)); }
for i in 2 3; do
	grep -q "^tfo $i syn_data=1 line=GET $i\$" "$work/peer.log" 2>/dev/null ||
		{ echo "FAIL peer: request $i not in the SYN"; fails=$((fails + 1)); }
done
expect cache "fastopen_cache = $TF_NET.1 cookie [0-9a-f]+ mss 1460"
expect sysctl 'connect_timeout = 4$' 'syn_rto_initial_ms = 1000$' 'syn_rto_max_ms = 4000$' 'keepalive_all = 0$' 'fastopen = 1$'
expect dead 'connect to .* failed: Connection timed out'
expect unreach 'connect to .* failed: No route to host'
grep -q '^idle connection closed' "$work/peer.log" 2>/dev/null || { echo "FAIL peer: the idle connection did not end"; fails=$((fails + 1)); }
atleast 'connects with data in the SYN' 2
atleast 'TCP Fast Open cookie requests' 1
atleast 'keepalive probes sent' 3
atleast 'connects timed out' 1
atleast 'connects ended by ICMP unreachable' 1
echo "---- peer:"; sed 's/^/  /' "$work/peer.log" 2>/dev/null
echo "---- netstat -s -t (new counters):"
out netstat | grep -E 'Fast Open|connects|keepalive|TIME_WAIT|corked' | sed 's/^/  /'
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_tcpopt: PASSED"
	exit 0
fi
echo "run_fsuae_tcpopt: FAILED ($fails)"
exit 1
