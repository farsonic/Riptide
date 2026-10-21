#!/bin/sh
# Riptide - on an FS-UAE built with tools/fs-uae/fs-uae-3.2.35-riptide.patch
# (uaenet.device bridged through libpcap), over a veth pair: Riptide's eth0
# is uaenet.device unit 2 on 'rtbr0', the host end 'rtbr1' is
# 192.168.77.1 with an HTTP server and tests/amiga/v6responder.py (an IPv6
# router for fd00:77::/64). Checks IPv4 ping, two 1 MB downloads with the
# full TCP window and no retransmissions or out-of-order segments, IPv6
# address autoconfiguration, the RA default route, and ping6.
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_bridge.sh [BUILDDIR] [TIMEOUT]
#
# Needs root (veth, packet capture), python3, tcpdump optional.
# Headless: starts Xvfb on :96 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-900}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck nc ping ping6 ip netstat rtdiag; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add rtbr0 type veth peer name rtbr1 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del rtbr0 2>/dev/null; }
trap cleanup EXIT
ip link set rtbr0 up
ip link set rtbr1 up
ip addr add 192.168.77.1/24 dev rtbr1
# no offloads: the capture must see real frames with real checksums
if command -v ethtool >/dev/null 2>&1; then
	for d in rtbr0 rtbr1; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-bridge.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out" "$work/www"
for f in Riptide rtcheck nc ping ping6 ip netstat rtdiag; do cp "$BUILD/$f" "$dh0/C/"; done
head -c 1048576 /dev/urandom > "$work/www/big.bin"
cat > "$dh0/S/riptide.conf" <<'CONF'
hostname bridge-test
loglevel 2
interface eth0 device=uaenet.device unit=2
ifconfig eth0 192.168.77.2/24 up
route add default 192.168.77.1
CONF
printf 'GET /big.bin HTTP/1.0\r\nHost: 192.168.77.1\r\n\r\n' > "$dh0/S/get"
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
FailAt 30
Path DH0:C ADD
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/ping -c 3 192.168.77.1 >DH0:out/ping
DH0:C/rtdiag -o DH0:out/diag -s 64 nc 192.168.77.1 8080 <DH0:S/get >DH0:out/body
DH0:C/nc 192.168.77.1 8080 <DH0:S/get >DH0:out/body2
DH0:C/ip -6 addr show dev eth0 >DH0:out/ip6
DH0:C/ping6 -c 3 fd00:77::1 >DH0:out/ping6
DH0:C/ip -6 route >DH0:out/route6
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
uaenet_pcap = rtbr0
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
(cd "$work/www" && exec python3 -m http.server 8080 --bind 192.168.77.1) >"$work/http.log" 2>&1 &
hpid=$!
python3 -I "$here/v6responder.py" rtbr1 "$TIMEOUT" >"$work/v6.log" 2>&1 &
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
grep -q "uaenet pcap: uaenet.device unit 2 = rtbr0" "$work/fsuae/Cache/Logs/fs-uae.log.txt" 2>/dev/null ||
	{ echo "FAIL fs-uae.log: no 'uaenet.device unit 2 = rtbr0'"; fails=$((fails + 1)); }
expect ping '3 packets transmitted, 3 received'
expect ip6 'inet6 fe80::[0-9a-f:]+/64 scope link' 'inet6 fd00:77::[0-9a-f:]+/64 scope global dynamic'
expect route6 '^fd00:77::/64 dev eth0 proto ra' '^default via fe80::[0-9a-f:]+ dev eth0 proto ra'
expect ping6 '3 packets transmitted, 3 received'
expect netstat ' 0 segments retransmitted' ' 0 packets queued out of order'
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
