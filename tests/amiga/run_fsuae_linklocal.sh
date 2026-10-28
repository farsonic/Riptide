#!/bin/sh
# Riptide - IPv4 link-local addresses (RFC 3927) on the real Amiga build:
# FS-UAE built with tools/fs-uae/fs-uae-3.2.35-riptide.patch (uaenet.device
# bridged through libpcap) over a veth pair with no DHCP server. Riptide's
# eth0 is uaenet.device unit 2 on 'rtl0' with "dhcp eth0" (link-local on by
# default, after 10 s without a lease) and host name "amiga"; the host end
# 'rtl1' is 192.168.180.1/24 and 169.254.1.1/16 running
# tests/amiga/linklocal_peer.py. Checks:
#   - the Amiga probes for a 169.254 address and announces it (ARP)
#   - mDNS announces amiga.local with it, and Linux resolves amiga.local
#     to it (legacy unicast query) and pings it
#   - the Amiga pings 169.254.1.1; "ip dhcp", "ip route" (169.254.0.0/16, no
#     default route), "ip addr" and "Riptide STATUS" show the address
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_linklocal.sh [BUILDDIR] [TIMEOUT]
#
# Needs root (veth, packet capture) and python3. Headless: starts Xvfb on
# :119 (LL_DISPLAY) when DISPLAY is not set. LL_IF, LL_PEER, LL_NET choose
# other veth names and another /24.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-400}
LL_IF=${LL_IF:-rtl0}
LL_PEER=${LL_PEER:-rtl1}
LL_NET=${LL_NET:-192.168.180}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck ping ip; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add $LL_IF type veth peer name $LL_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del $LL_IF 2>/dev/null; }
trap cleanup EXIT
ip link set $LL_IF up
ip link set $LL_PEER up
ip addr add $LL_NET.1/24 dev $LL_PEER
ip addr add 169.254.1.1/16 dev $LL_PEER
if command -v ethtool >/dev/null 2>&1; then
	for d in $LL_IF $LL_PEER; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-ll.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck ping ip; do cp "$BUILD/$f" "$dh0/C/"; done
cat > "$dh0/S/riptide.conf" <<'CONF'
hostname amiga
loglevel 2
interface eth0 device=uaenet.device unit=2
dhcp eth0
CONF
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
FailAt 30
Path DH0:C ADD
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
Lab again
Wait 2
DH0:C/ping -c 1 169.254.1.1 >NIL:
If WARN
  Skip again BACK
EndIf
DH0:C/ping -c 3 169.254.1.1 >DH0:out/ping
DH0:C/ip dhcp >DH0:out/ipdhcp
DH0:C/ip route >DH0:out/iproute
DH0:C/ip addr show dev eth0 >DH0:out/ipaddr
DH0:C/Riptide STATUS >DH0:out/status
Wait 20
DH0:C/ip dhcp >DH0:out/ipdhcp2
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
uaenet_pcap = $LL_IF
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	d=${LL_DISPLAY:-119}
	Xvfb :$d -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:$d
	export DISPLAY
	sleep 1
fi
python3 -I "$here/linklocal_peer.py" $LL_PEER 169.254.1.1 amiga "$TIMEOUT" >"$work/peer.log" 2>&1 &
ppid_=$!
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && { [ ! -f "$dh0/out/zz_done" ] || ! grep -q '^done' "$work/peer.log"; }; do
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
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
unexpect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" && { echo "FAIL $n: a line matches /$re/"; fails=$((fails + 1)); }
	done
}
expect_peer() {
	for re in "$@"; do
		grep -Eq -- "$re" "$work/peer.log" || { echo "FAIL peer: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
ll=$(sed -n 's/^ok arp announce \(169\.254\.[0-9.]*\)$/\1/p' "$work/peer.log" | tail -n 1)
lre=$(echo "${ll:-169.254.x}" | sed 's/\./\\./g')
expect ping '3 packets transmitted, [1-3] (packets )?received'
expect ipdhcp '^eth0: (selecting|init)' "link-local $lre/16"
expect iproute "^169\.254\.0\.0/16 dev eth0 proto kernel scope link src $lre"
unexpect iproute '^default'
expect ipaddr "inet $lre/16"
expect status "^eth0 +$lre/255\.255\.0\.0" "DHCP (selecting|init), link-local $lre"
expect ipdhcp2 "link-local $lre/16"
expect_peer '^ok probe 169\.254\.' '^ok arp announce 169\.254\.' "^ok mdns announce amiga\.local $lre" \
	"^ok resolved amiga\.local $lre" "^ok ping $lre" '^done address'
for f in ipdhcp iproute status; do echo "---- $f:"; out $f | head -n 12; done
echo "---- peer:"; cat "$work/peer.log"
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_linklocal: PASSED"
	exit 0
fi
echo "run_fsuae_linklocal: FAILED ($fails)"
exit 1
