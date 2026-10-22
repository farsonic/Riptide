#!/bin/sh
# Riptide - NTP and mDNS on the real Amiga build: FS-UAE built with
# tools/fs-uae/fs-uae-3.2.35-riptide.patch (uaenet.device bridged through
# libpcap) over a veth pair. Riptide's eth0 is uaenet.device unit 2 on
# 'rtmd0' (192.168.81.2, host name "amitest", timezone +10:00); the host end
# 'rtmd1' is 192.168.81.1 running tests/amiga/netsvc_peer.py (an SNTP
# server with the host clock, an mDNS responder for linuxpeer.local and an
# mDNS querier for amitest.local). Checks:
#   - the first sync steps the Amiga clock (timer.device TR_SETSYSTIME,
#     battclock.resource) and the next one finds it within 2 s (the emulated
#     clock drifts against the host: video_sync is off)
#   - "Riptide NTP STATUS" / "Riptide STATUS" show NTP and mDNS
#   - the Amiga resolves linuxpeer.local (gethostbyname -> mDNS) and its own
#     name, and answers QU, QM, legacy unicast and reverse PTR queries
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_netsvc.sh [BUILDDIR] [TIMEOUT]
#
# Needs root (veth, packet capture) and python3. Headless: starts Xvfb on
# :118 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck ping host; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add rtmd0 type veth peer name rtmd1 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del rtmd0 2>/dev/null; }
trap cleanup EXIT
ip link set rtmd0 up
ip link set rtmd1 up
ip addr add 192.168.81.1/24 dev rtmd1
if command -v ethtool >/dev/null 2>&1; then
	for d in rtmd0 rtmd1; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-netsvc.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck ping host; do cp "$BUILD/$f" "$dh0/C/"; done
cat > "$dh0/S/riptide.conf" <<'CONF'
hostname amitest
loglevel 2
interface eth0 device=uaenet.device unit=2
ifconfig eth0 192.168.81.2/24 up
timezone +10:00
ntp 192.168.81.1
ntp battclock on
CONF
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
FailAt 30
Path DH0:C ADD
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/ping -c 6 192.168.81.1 >DH0:out/ping
DH0:C/Riptide NTP STATUS >DH0:out/ntp1
DH0:C/Riptide NTP SYNC
DH0:C/ping -c 3 192.168.81.1 >NIL:
DH0:C/Riptide NTP STATUS >DH0:out/ntp2
DH0:C/Riptide TIMEZONE >DH0:out/tz
DH0:C/host linuxpeer.local >DH0:out/host_peer
DH0:C/host amitest.local >DH0:out/host_self
DH0:C/ping -c 12 192.168.81.1 >NIL:
DH0:C/Riptide MDNS STATUS >DH0:out/mdns
DH0:C/Riptide STATUS >DH0:out/status
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
uaenet_pcap = rtmd0
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :118 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:118
	export DISPLAY
	sleep 1
fi
python3 -I "$here/netsvc_peer.py" rtmd1 192.168.81.1 amitest 192.168.81.2 5 "$TIMEOUT" >"$work/peer.log" 2>&1 &
ppid_=$!
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
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
expect_peer() {
	for re in "$@"; do
		grep -Eq -- "$re" "$work/peer.log" || { echo "FAIL peer: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
expect ping '6 packets transmitted, [1-6] received'
expect ntp1 '^NTP: 192\.168\.81\.1, stratum 2, offset [-+][0-9]+\.[0-9]{3} s \(clock set\)'
expect ntp2 '^NTP: 192\.168\.81\.1, stratum 2, offset [-+][01]\.[0-9]{3} s'
expect tz '^timezone \+10:00$'
expect host_peer 'linuxpeer\.local has address 192\.168\.81\.1'
expect host_self 'amitest\.local has address 192\.168\.81\.2'
expect mdns '^mDNS: amitest\.local, responding on eth0, [1-9][0-9]* answers, 0 conflicts'
expect status '^NTP: 192\.168\.81\.1' '^mDNS: amitest\.local'
expect_peer '^ntp request from 192\.168\.81\.2' '^ok probe amitest\.local' '^ok announce amitest\.local ttl 120 cache-flush True' \
	'^ok qu \(unicast\) amitest\.local 192\.168\.81\.2' '^ok qm \(multicast\) amitest\.local 192\.168\.81\.2 ttl 120' \
	'^ok legacy amitest\.local 192\.168\.81\.2 ttl 10 cache-flush False' '^ok ptr 2\.81\.168\.192\.in-addr\.arpa amitest\.local' \
	'^ok answered linuxpeer\.local' '^done all answered'
for f in ntp1 ntp2 mdns; do echo "---- $f: $(out $f)"; done
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_netsvc: PASSED"
	exit 0
fi
echo "run_fsuae_netsvc: FAILED ($fails)"
exit 1
