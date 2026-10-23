#!/bin/sh
# Riptide - 802.1Q VLAN interfaces on the real Amiga build: FS-UAE built
# with tools/fs-uae (uaenet.device bridged through libpcap) on the veth end
# VL_IF; the other end VL_PEER is VL_NET.1/24 untagged, and VLAN 10 on it
# is answered by tests/amiga/vlan_peer.py (ARP, ping, UDP echo on port 7 for
# VL_NET2.1; the container kernel may lack 8021q). Riptide: eth0
# (uaenet.device unit 2) VL_NET.2/24, eth0.10 (VLAN 10, PCP 4) VL_NET2.2/24.
# Checks: ping untagged and on VLAN 10 (also with a 1496 byte packet: a
# 1518 byte tagged frame through uaenet.device and the pcap bridge), UDP
# on VLAN 10, the PCP of the frames, ip -d link / ifconfig / netstat -i /
# Riptide STATUS, and VLANs added and removed at run time with ip and
# ifconfig.
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_vlan.sh [BUILDDIR] [TIMEOUT]
#
# VL_IF, VL_PEER, VL_NET and VL_NET2 (first three octets) choose other veth
# names and subnets. Needs root (veth, packet capture) and python3.
# Headless: starts Xvfb on :95 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
VL_IF=${VL_IF:-rtv0}
VL_PEER=${VL_PEER:-rtv1}
VL_NET=${VL_NET:-192.168.130}
VL_NET2=${VL_NET2:-192.168.131}
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck ping ip ifconfig netstat nc; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add $VL_IF type veth peer name $VL_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del $VL_IF 2>/dev/null; }
trap cleanup EXIT
ip link set $VL_IF up
ip link set $VL_PEER up
ip addr add $VL_NET.1/24 dev $VL_PEER
if command -v ethtool >/dev/null 2>&1; then
	for d in $VL_IF $VL_PEER; do
		ethtool -K $d tso off gso off gro off tx off rx off rxvlan off txvlan off >/dev/null 2>&1
	done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-vlan.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck ping ip ifconfig netstat nc; do cp "$BUILD/$f" "$dh0/C/"; done
cat > "$dh0/S/riptide.conf" <<CONF
hostname vlan-test
interface eth0 device=uaenet.device unit=2
ifconfig eth0 $VL_NET.2/24 up
interface eth0.10 vlan=10 parent=eth0 pcp=4
ifconfig eth0.10 $VL_NET2.2/24 up
CONF
echo "hello over VLAN 10" > "$dh0/S/msg"
cat > "$dh0/S/Startup-Sequence" <<SEQ
FailAt 30
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/ping -c 3 $VL_NET.1 >DH0:out/ping0
DH0:C/ping -c 3 $VL_NET2.1 >DH0:out/ping10
DH0:C/ping -c 2 -s 1468 $VL_NET2.1 >DH0:out/pingbig
DH0:C/nc -u -w 2 $VL_NET2.1 7 <DH0:S/msg >DH0:out/udp10
DH0:C/ip -d link >DH0:out/iplink
DH0:C/ifconfig eth0.10 >DH0:out/ifc
DH0:C/netstat -i >DH0:out/nsi
DH0:C/Riptide STATUS >DH0:out/status
DH0:C/ip link add link eth0 name eth0.20 type vlan id 20 pcp priority >DH0:out/add20
DH0:C/ip -d link show dev eth0.20 >DH0:out/show20
DH0:C/ip link del eth0.20 >DH0:out/del20
DH0:C/ifconfig eth0.30 create vlan 30 vlandev eth0 vlanpcp 2 >DH0:out/c30
DH0:C/ifconfig eth0.30 >DH0:out/ifc30
DH0:C/ifconfig eth0.30 destroy >DH0:out/d30
DH0:C/ip link >DH0:out/after
DH0:C/ip link add link eth0 name bad type vlan id 10 >DH0:out/dup
DH0:C/ping -c 2 $VL_NET2.1 >DH0:out/ping10b
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
uaenet_pcap = $VL_IF
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :95 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:95
	export DISPLAY
	sleep 1
fi
python3 -I "$here/vlan_peer.py" $VL_PEER 10 $VL_NET2.1 "$work/peer.log" "$TIMEOUT" >"$work/peer.err" 2>&1 &
ppid=$!
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
sleep 1
kill $upid $ppid 2>/dev/null
wait $upid $ppid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null

fails=0
out() { tr -d '\r' < "$dh0/out/$1" 2>/dev/null; }
expect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" || { echo "FAIL $n: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
reject() {
	out "$1" | grep -Eq -- "$2" && { echo "FAIL $1: unexpected /$2/"; fails=$((fails + 1)); }
}
expect start 'stack ready'
expect ping0 '3 packets transmitted, 3 received'
expect ping10 '3 packets transmitted, 3 received'
expect pingbig '2 packets transmitted, 2 received'
expect udp10 'hello over VLAN 10'
expect iplink '^[0-9]+: eth0\.10@eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1496' \
	'vlan protocol 802\.1Q id 10 parent eth0 pcp 4'
expect ifc "inet $VL_NET2.2  netmask 255.255.255.0" 'vlan: 10 vlanpcp: 4 parent interface: eth0'
expect nsi '^eth0\.10 +1496 '
expect status 'eth0\.10' 'VLAN 10 on eth0'
expect show20 'eth0\.20@eth0' 'vlan protocol 802\.1Q id 20 parent eth0 pcp priority'
expect ifc30 'vlan: 30 vlanpcp: 2 parent interface: eth0'
reject after 'eth0\.20|eth0\.30'
expect dup 'has a VLAN 10 already'
expect ping10b '2 packets transmitted, 2 received'
# the far end saw the requests with Riptide's priority 4 on the tag
grep -q '^arp pcp 4' "$work/peer.log" 2>/dev/null || { echo "FAIL peer.log: no ARP with PCP 4"; fails=$((fails + 1)); }
grep -q '^ping 1496 pcp 4' "$work/peer.log" 2>/dev/null ||
	{ echo "FAIL peer.log: no 1496 byte ping (1518 byte frame)"; fails=$((fails + 1)); }
grep -q '^udp ' "$work/peer.log" 2>/dev/null || { echo "FAIL peer.log: no UDP"; fails=$((fails + 1)); }
echo "---- peer:"
sed 's/^/  /' "$work/peer.log" 2>/dev/null | sort | uniq -c | head -8
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_vlan: PASSED"
	exit 0
fi
echo "run_fsuae_vlan: FAILED ($fails)"
exit 1
