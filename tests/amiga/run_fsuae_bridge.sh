#!/bin/sh
# Riptide - on an FS-UAE built with tools/fs-uae/fs-uae-3.2.35-riptide.patch
# (uaenet.device bridged through libpcap), over a veth pair: Riptide's eth0
# is uaenet.device unit 2 on 'rtbr0', the host end 'rtbr1' is
# 192.168.77.1 with an HTTP server and tests/amiga/v6responder.py (an IPv6
# router for fd00:77::/64). Checks IPv4 ping, two 1 MB downloads with the
# full TCP window and no retransmissions or out-of-order segments, IPv6
# address autoconfiguration, the RA default route, and ping6. A second
# address on another subnet (ifconfig eth0:1): a download from a server on
# that subnet comes from it, ip addr / ifconfig / Riptide STATUS list it,
# "ip addr del" removes it; ip -s link shows the QoS bands (qos=on); nc by name
# with an unanswered IPv6 address and "happy_eyeballs prefer ipv6"
# connects over IPv4 (Happy Eyeballs).
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_bridge.sh [BUILDDIR] [TIMEOUT]
#
# BR_IF, BR_PEER and BR_NET (the first three octets) choose other veth
# names and another subnet, so several runs can share a host.
#
# Needs root (veth, packet capture), python3, tcpdump optional.
# Headless: starts Xvfb on :96 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BR_IF=${BR_IF:-rtbr0}         # veth end FS-UAE captures on
BR_PEER=${BR_PEER:-rtbr1}     # host end
BR_NET=${BR_NET:-192.168.77}  # /24: host .1, Riptide .2
BR_NET2=172.31.${BR_NET##*.}  # second subnet: host .1, Riptide .2
BUILD=${1:-build/amiga}
TIMEOUT=${2:-900}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck nc ping ping6 ip netstat rtdiag ifconfig; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add $BR_IF type veth peer name $BR_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del $BR_IF 2>/dev/null; }
trap cleanup EXIT
ip link set $BR_IF up
ip link set $BR_PEER up
ip addr add $BR_NET.1/24 dev $BR_PEER
ip addr add $BR_NET2.1/24 dev $BR_PEER
# no offloads: the capture must see real frames with real checksums
if command -v ethtool >/dev/null 2>&1; then
	for d in $BR_IF $BR_PEER; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-bridge.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out" "$work/www"
for f in Riptide rtcheck nc ping ping6 ip netstat rtdiag ifconfig; do cp "$BUILD/$f" "$dh0/C/"; done
head -c 1048576 /dev/urandom > "$work/www/big.bin"
cat > "$dh0/S/riptide.conf" <<CONF
hostname bridge-test
loglevel 2
interface eth0 device=uaenet.device unit=2 qos=on
ifconfig eth0 $BR_NET.2/24 up
route add default $BR_NET.1
host hetest fd00:77::99
host hetest $BR_NET.1
happy_eyeballs prefer ipv6
CONF
printf 'GET /big.bin HTTP/1.0\r\nHost: %s.1\r\n\r\n' "$BR_NET" > "$dh0/S/get"
printf 'GET /small.txt HTTP/1.0\r\nHost: %s.1\r\n\r\n' "$BR_NET2" > "$dh0/S/get2"
echo "second subnet" > "$work/www/small.txt"
cat > "$dh0/S/Startup-Sequence" <<SEQ
FailAt 30
Path DH0:C ADD
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/ping -c 3 $BR_NET.1 >DH0:out/ping
DH0:C/rtdiag -o DH0:out/diag -s 64 nc $BR_NET.1 8080 <DH0:S/get >DH0:out/body
DH0:C/nc $BR_NET.1 8080 <DH0:S/get >DH0:out/body2
DH0:C/ip -6 addr show dev eth0 >DH0:out/ip6
DH0:C/ping6 -c 3 fd00:77::1 >DH0:out/ping6
DH0:C/ip -6 route >DH0:out/route6
DH0:C/netstat -s >DH0:out/netstat
DH0:C/ifconfig eth0:1 $BR_NET2.2/24 >DH0:out/alias_add
DH0:C/nc $BR_NET2.1 8081 <DH0:S/get2 >DH0:out/body3
DH0:C/ip addr show dev eth0 >DH0:out/addr
DH0:C/ifconfig eth0 >DH0:out/ifc
DH0:C/Riptide STATUS >DH0:out/status
DH0:C/ip -s link show dev eth0 >DH0:out/link
DH0:C/ip addr del $BR_NET2.2/24 dev eth0 >DH0:out/alias_del
DH0:C/ip addr show dev eth0 >DH0:out/addr2
DH0:C/nc -v hetest 8080 <DH0:S/get >DH0:out/body4
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
uaenet_pcap = $BR_IF
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :96 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:96
	export DISPLAY
	sleep 1
fi
(cd "$work/www" && exec python3 -m http.server 8080 --bind $BR_NET.1) >"$work/http.log" 2>&1 &
hpid=$!
(cd "$work/www" && exec python3 -m http.server 8081 --bind $BR_NET2.1) >"$work/http2.log" 2>&1 &
hpid="$hpid $!"
python3 -I "$here/v6responder.py" $BR_PEER "$TIMEOUT" >"$work/v6.log" 2>&1 &
vpid=$!
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
sleep 1
kill $upid $hpid $vpid 2>/dev/null
wait $upid $hpid $vpid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null

fails=0
out() { tr -d '\r' < "$dh0/out/$1" 2>/dev/null; }
expect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" || { echo "FAIL $n: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
grep -q "uaenet pcap: uaenet.device unit 2 = $BR_IF" "$work/fsuae/Cache/Logs/fs-uae.log.txt" 2>/dev/null ||
	{ echo "FAIL fs-uae.log: no 'uaenet.device unit 2 = $BR_IF'"; fails=$((fails + 1)); }
expect ping '3 packets transmitted, 3 received'
expect ip6 'inet6 fe80::[0-9a-f:]+/64 scope link' 'inet6 fd00:77::[0-9a-f:]+/64 scope global dynamic'
expect route6 '^fd00:77::/64 dev eth0 proto ra' '^default via fe80::[0-9a-f:]+ dev eth0 proto ra'
expect ping6 '3 packets transmitted, 3 received'
expect netstat ' 0 segments retransmitted' ' 0 packets queued out of order'
# the second address
expect body3 'second subnet'
grep -q "^$BR_NET2.2 .*GET /small.txt" "$work/http2.log" ||
	{ echo "FAIL http2.log: the request did not come from $BR_NET2.2"; fails=$((fails + 1)); }
expect addr "inet $BR_NET.2/24 brd $BR_NET.255 scope global eth0" \
	"inet $BR_NET2.2/24 brd $BR_NET2.255 scope global secondary eth0"
expect ifc "inet $BR_NET2.2  netmask 255.255.255.0  broadcast $BR_NET2.255  \\(alias\\)"
expect status "also $BR_NET2.2/255.255.255.0" 'QoS on'
expect link 'QoS: direct [0-9]+, interactive [0-9]+'
out addr2 | grep -q "$BR_NET2.2" && { echo "FAIL addr2: $BR_NET2.2 still there"; fails=$((fails + 1)); }
# Happy Eyeballs: IPv6 first, unanswered; IPv4 wins
expect body4 "Connection to hetest \\($BR_NET.1\\) 8080 port succeeded"
if ! tail -c 1048576 "$dh0/out/body4" 2>/dev/null | cmp -s - "$work/www/big.bin"; then
	echo "FAIL body4: the download by name differs"
	fails=$((fails + 1))
fi
# body has rtdiag's messages around the data; body2 is the plain download
n=$(wc -c < "$dh0/out/body2" 2>/dev/null || echo 0)
if ! tail -c 1048576 "$dh0/out/body2" 2>/dev/null | cmp -s - "$work/www/big.bin"; then
	echo "FAIL body2: the 1 MB download differs ($n bytes)"
	fails=$((fails + 1))
fi
[ "$(wc -c < "$dh0/out/body" 2>/dev/null || echo 0)" -gt 1048576 ] ||
	{ echo "FAIL body: short download (rtdiag run)"; fails=$((fails + 1)); }
secs=$(out diag/report.txt | sed -n 's/^\([0-9.]*\) seconds.*/\1/p')
echo "---- 1 MB download over the bridge: ${secs:-?} s (A1200, 68020)"
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_bridge: PASSED"
	exit 0
fi
echo "run_fsuae_bridge: FAILED ($fails)"
exit 1
