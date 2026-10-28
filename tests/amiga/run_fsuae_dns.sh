#!/bin/sh
# Riptide - the resolver's EDNS0 and TCP fallback on FS-UAE, against
# tests/amiga/dns_server.py (UDP answers cut at 512 bytes or the query's
# EDNS0 size, full answers over TCP). Needs the FS-UAE built with
# tools/fs-uae/fs-uae-3.2.35-riptide.patch (uaenet.device bridged through
# libpcap), over a veth pair: Riptide's eth0 is uaenet.device unit 2 on
# 'rtd0', the host end 'rtd1' is 192.168.160.1 and runs the server.
# Checks: gethostbyname() of a name whose answer only fits over UDP with
# EDNS0 (no TCP), of one that needs TCP (A and AAAA), host -t (its own
# query: EDNS0, "Truncated, retrying in TCP mode."), "Riptide DNS" status
# and "Riptide DNS EDNS_SIZE 0" at run time (the EDNS0 name then over TCP).
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_dns.sh [BUILDDIR] [TIMEOUT]
#
# BR_IF, BR_PEER and BR_NET (the first three octets) choose other veth
# names and another subnet. Needs root (veth, packet capture), python3.
# Headless: starts Xvfb on :${XDISP:-97} when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BR_IF=${BR_IF:-rtd0}
BR_PEER=${BR_PEER:-rtd1}
BR_NET=${BR_NET:-192.168.160}
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck host; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add $BR_IF type veth peer name $BR_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del $BR_IF 2>/dev/null; }
trap cleanup EXIT
ip link set $BR_IF up
ip link set $BR_PEER up
ip addr add $BR_NET.1/24 dev $BR_PEER
if command -v ethtool >/dev/null 2>&1; then
	for d in $BR_IF $BR_PEER; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-dns.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck host; do cp "$BUILD/$f" "$dh0/C/"; done
cat > "$dh0/S/riptide.conf" <<CONF
hostname dns-test
loglevel 2
interface eth0 device=uaenet.device unit=2
ifconfig eth0 $BR_NET.2/24 up
route add default $BR_NET.1
nameserver $BR_NET.1
ipv6 off
CONF
cat > "$dh0/S/Startup-Sequence" <<SEQ
FailAt 30
Path DH0:C ADD
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/host www.dns.test >DH0:out/www
DH0:C/host big.dns.test >DH0:out/big
DH0:C/host huge.dns.test >DH0:out/huge
DH0:C/host -t A big.dns.test >DH0:out/big_t
DH0:C/host -v -t AAAA huge.dns.test $BR_NET.1 >DH0:out/huge_t
DH0:C/Riptide DNS >DH0:out/status
DH0:C/Riptide DNS EDNS_SIZE 0 >DH0:out/set
DH0:C/host big2.dns.test >DH0:out/big2
DH0:C/Riptide DNS >DH0:out/status2
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
	Xvfb :${XDISP:-97} -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:${XDISP:-97}
	export DISPLAY
	sleep 1
fi
python3 -I "$here/dns_server.py" $BR_NET.1 "$TIMEOUT" "$work/dns.log" >"$work/dns.err" 2>&1 &
dpid=$!
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
sleep 1
kill $upid $dpid 2>/dev/null
wait $upid $dpid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null

fails=0
out() { tr -d '\r' < "$dh0/out/$1" 2>/dev/null; }
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
expect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" || fail "$n: no line matches /$re/"
	done
}
count() { out "$1" | grep -Ec -- "$2"; }
qlog() { grep -Ec -- "$1" "$work/dns.log" 2>/dev/null; }

[ -f "$dh0/out/zz_done" ] || fail "the Startup-Sequence did not finish"
expect www 'www.dns.test has address 10.1.0.1'
# 40 records: over UDP with EDNS0 (1232), no TCP; gethostbyname() gives 8
[ "$(count big 'big.dns.test has address 10.2.0.')" = 8 ] || fail "big: not 8 addresses"
[ "$(qlog '^UDP big.dns.test 1 edns=1232 tc=0')" -ge 1 ] || fail "dns.log: big not over UDP with EDNS0"
[ "$(qlog '^TCP big.dns.test')" = 0 ] || fail "dns.log: big went over TCP"
# 100 A records: truncated over UDP, then TCP
expect huge 'huge.dns.test has address 10.3.0.1$'
[ "$(count huge 'has address')" = 8 ] || fail "huge: not 8 addresses"
[ "$(qlog '^UDP huge.dns.test 1 edns=1232 tc=1')" -ge 1 ] || fail "dns.log: huge not truncated over UDP"
[ "$(qlog '^TCP huge.dns.test 1 ')" -ge 1 ] || fail "dns.log: no TCP query for huge A"
# host -t: its own query with EDNS0; TCP after TC
[ "$(count big_t 'big.dns.test has address')" = 40 ] || fail "big_t: not 40 addresses"
expect huge_t 'Truncated, retrying in TCP mode' 'ANSWER: 60,' 'huge.dns.test has IPv6 address fd0d::3c$'
[ "$(count huge_t 'has IPv6 address')" = 60 ] || fail "huge_t: not 60 addresses"
expect status 'EDNS0 1232 bytes, TCP fallback on; 0 server\(s\) without EDNS; [1-9][0-9]* TCP queries, 0 failed'
# EDNS0 off at run time: the 40 records need TCP now
[ "$(count big2 'big2.dns.test has address')" = 8 ] || fail "big2: not 8 addresses"
[ "$(qlog '^UDP big2.dns.test 1 edns=- tc=1')" -ge 1 ] || fail "dns.log: big2 not plain and truncated"
[ "$(qlog '^TCP big2.dns.test 1 ')" -ge 1 ] || fail "dns.log: no TCP query for big2"
expect status2 'EDNS0 off, TCP fallback on'
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_dns: PASSED"
	exit 0
fi
echo "run_fsuae_dns: FAILED ($fails)"
exit 1
