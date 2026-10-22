#!/bin/sh
# Riptide - IPv4 multicast on an FS-UAE built with
# tools/fs-uae/fs-uae-3.2.35-riptide.patch (uaenet.device bridged through
# libpcap), over a veth pair: Riptide's eth0 is uaenet.device unit 2 on
# 'rtmc0', the host end 'rtmc1' is 192.168.82.1 running
# tests/amiga/mcast_peer.py. The Amiga joins 239.82.0.1 with
# "nc -u -l -m" (IGMPv3 report seen on the wire, a general query from
# Linux answered, Linux's datagrams received, the leave reported), shows
# "ip maddr" and "netstat -g", and sends to 239.82.0.2, which Linux joined.
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_mcast.sh [BUILDDIR] [TIMEOUT]
#
# Needs root (veth, packet capture) and python3.
# Headless: starts Xvfb on $XDISPLAY (default :97) when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck nc ip netstat; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add rtmc0 type veth peer name rtmc1 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del rtmc0 2>/dev/null; }
trap cleanup EXIT
ip link set rtmc0 up
ip link set rtmc1 up
ip addr add 192.168.82.1/24 dev rtmc1
if command -v ethtool >/dev/null 2>&1; then
	for d in rtmc0 rtmc1; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-mcast.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck nc ip netstat; do cp "$BUILD/$f" "$dh0/C/"; done
printf 'hello from amiga\n' > "$dh0/S/msg"
cat > "$dh0/S/riptide.conf" <<'CONF'
hostname mcast-test
loglevel 2
interface eth0 device=uaenet.device unit=2
ifconfig eth0 192.168.82.2/24 up
CONF
# No default route: multicast must work without one. rtcheck START runs
# the first listener detached (the AROS boot shell has no Run); the
# datagram to 239.82.0.2 tells the peer to start sending, which ends both
# listeners (no timeouts: the emulated clock runs fast).
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
FailAt 30
Path DH0:C ADD
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/rtcheck START="DH0:C/nc -u -l -v -c 1 -m 239.82.0.1 5401 >DH0:out/mrecv" NOCHECK >NIL:
Wait 2
DH0:C/ip maddr show dev eth0 >DH0:out/maddr
DH0:C/netstat -g >DH0:out/groups
DH0:C/nc -u -w 1 239.82.0.2 5402 <DH0:S/msg >NIL:
DH0:C/nc -u -l -c 1 -m 239.82.0.1 5401 >DH0:out/mrecv2
Wait 2
DH0:C/ip -d maddr show dev eth0 >DH0:out/maddr2
DH0:C/netstat -s >DH0:out/netstat
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
uaenet_pcap = rtmc0
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb "${XDISPLAY:-:97}" -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=${XDISPLAY:-:97}
	export DISPLAY
	sleep 1
fi
python3 -I "$here/mcast_peer.py" rtmc1 192.168.82.1 192.168.82.2 "$TIMEOUT" >"$work/peer.log" 2>&1 &
ppid_=$!
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
sleep 2
kill $upid $ppid_ 2>/dev/null
wait $upid $ppid_ 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null

fails=0
out() { tr -d '\r' < "$dh0/out/$1" 2>/dev/null; }
expect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" || { echo "FAIL $n: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
peer() {
	for re in "$@"; do
		grep -Eq -- "$re" "$work/peer.log" || { echo "FAIL peer: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
grep -q "uaenet pcap: uaenet.device unit 2 = rtmc0" "$work/fsuae/Cache/Logs/fs-uae.log.txt" 2>/dev/null ||
	{ echo "FAIL fs-uae.log: no 'uaenet.device unit 2 = rtmc0'"; fails=$((fails + 1)); }
peer 'IGMPV3 dst=224\.0\.0\.22 ttl=1 tos=0xc0 ra=1 records=.*4:239\.82\.0\.1' \
     '^QUERY_ANSWERED' '^LEFT' \
     "GOT_FROM_AMIGA src=192\.168\.82\.2 ttl=1 data=b'hello from amiga'"
expect mrecv 'Received [0-9]+ bytes from 192\.168\.82\.1' '^multicast to amiga [0-9]+'
expect mrecv2 '^multicast to amiga [0-9]+'
expect maddr '^[0-9]+:	eth0' '	link  01:00:5e:52:00:01' '	inet  239\.82\.0\.1' '	inet  224\.0\.0\.1'
expect groups '^eth0 +1 +239\.82\.0\.1' '^eth0 +1 +224\.0\.0\.1'
expect maddr2 '	inet  224\.0\.0\.1'
out maddr2 | grep -q '239\.82\.0\.1' && { echo "FAIL maddr2: 239.82.0.1 still joined"; fails=$((fails + 1)); }
expect netstat '^Igmp:' '[1-9][0-9]* V3 membership queries received' '[1-9][0-9]* V3 membership reports sent'
echo "---- peer log:"
sed 's/^/  | /' "$work/peer.log"
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_mcast: PASSED"
	exit 0
fi
echo "run_fsuae_mcast: FAILED ($fails)"
exit 1
