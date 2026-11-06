#!/bin/sh
# Riptide - the netdev backend (src/amiga/netdev.c) on the real Amiga build,
# without a Raspberry Pi: tests/amiga/fakenetdev.c (built with vbcc) puts a
# netdev driver "fakenet.device" into memory that runs on uaenet.device,
# and FS-UAE built with tools/fs-uae/fs-uae-3.2.35-riptide.patch bridges
# that over the veth pair rtnd0/rtnd1 (192.168.82.0/24) to the host, which
# runs tools/speedtest_mock.py. Riptide is given "device=fakenet.device"
# and must find out by itself that it is netdev, not SANA-II. Checks:
#   - the interface comes up as netdev (log, Riptide STATUS)
#   - ping to the host and back
#   - a speedtest against the mock server (TCP both ways, the rates are
#     printed: one emulated 68020 does everything)
#   - Riptide QUIT stops and detaches the driver cleanly
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_netdev.sh [BUILDDIR] [TIMEOUT]
#
# BASELINE=1 runs the same boot with Riptide on uaenet.device itself
# (SANA-II, no fakenetdev) for the rates to compare with.
#
# Needs root (veth, packet capture), python3 and vbcc + NDK. Headless:
# starts Xvfb on :121 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
[ -x "$VBCC/bin/vc" ] || { echo "SKIP: vbcc not found"; exit 0; }
for f in Riptide rtcheck ping speedtest; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add rtnd0 type veth peer name rtnd1 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del rtnd0 2>/dev/null; }
trap cleanup EXIT
ip link set rtnd0 up
ip link set rtnd1 up
ip addr add 192.168.82.1/24 dev rtnd1
if command -v ethtool >/dev/null 2>&1; then
	for d in rtnd0 rtnd1; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-netdev.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck ping speedtest; do cp "$BUILD/$f" "$dh0/C/"; done
PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O2 -I"$NDK/Include_H" -o "$dh0/C/fakenetdev" \
	"$here/fakenetdev.c" -lamiga >"$work/vc.log" 2>&1 || { echo "FAIL: cannot build fakenetdev"; cat "$work/vc.log"; exit 1; }
cat > "$dh0/S/riptide.conf" <<'CONF'
hostname amitest
loglevel 3
interface eth0 device=@DEV@
ifconfig eth0 192.168.82.2/24 up
CONF
if [ "${BASELINE:-0}" = 1 ]; then
	sed -i 's|@DEV@|uaenet.device unit=2|' "$dh0/S/riptide.conf"
else
	sed -i 's|@DEV@|fakenet.device unit=0|' "$dh0/S/riptide.conf"
fi
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
FailAt 30
Path DH0:C ADD
@FAKE@
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/ping -c 5 192.168.82.1 >DH0:out/ping
DH0:C/ping -c 3 -i 1 192.168.82.1 >DH0:out/ping2
DH0:C/Riptide STATUS >DH0:out/status
DH0:C/speedtest --mirror 192.168.82.1:8080 --duration 6 >DH0:out/speed
DH0:C/Riptide STATUS >DH0:out/status2
DH0:C/Riptide QUIT >DH0:out/quit
Wait 2
Echo done >DH0:out/zz_done
SEQ
if [ "${BASELINE:-0}" = 1 ]; then
	sed -i 's|^@FAKE@$|Echo "rx 1 tx 1 rx_errors 0 tx_errors 0 (baseline)" >DH0:out/fakestats|' "$dh0/S/Startup-Sequence"
else
	sed -i 's|^@FAKE@$|Run >DH0:out/fake DH0:C/fakenetdev uaenet.device 2 DH0:out/fakestats\nWait 2|' "$dh0/S/Startup-Sequence"
fi
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
uaenet_pcap = rtnd0
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :121 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:121
	export DISPLAY
	sleep 1
fi
tcpdump -n -i rtnd1 -c 200 -w "$work/rtnd1.pcap" >/dev/null 2>&1 &
tpid=$!
python3 -I "$here/../../tools/speedtest_mock.py" --bind 192.168.82.1 --port 8080 >"$work/mock.log" 2>&1 &
mpid=$!
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/out/zz_done" ]; do
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
kill $upid $mpid $tpid 2>/dev/null
wait $upid $mpid $tpid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null

fails=0
out() { tr -d '\r' < "$dh0/out/$1" 2>/dev/null; }
expect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" || { echo "FAIL $n: no line matches /$re/"; fails=$((fails + 1)); }
	done
}
expect fakestats '^rx [1-9][0-9]* tx [1-9][0-9]* rx_errors 0 tx_errors 0'
if [ "${BASELINE:-0}" != 1 ]; then
	expect riptide.log 'eth0: netdev fakenet.device unit 0, ABI 1, mtu 1500'
	expect status '^eth0 +192\.168\.82\.2' 'netdev fakenet.device unit 0 \(ABI 1\), link 1000 Mb/s full duplex'
fi
expect ping '5 packets transmitted, 5 (packets )?received'
expect ping2 '3 packets transmitted, 3 (packets )?received'
expect speed '[Dd]ownload' '[Uu]pload'
expect quit 'shutting down'
for f in fake fakestats riptide.log ping ping2 status speed status2 quit; do echo "---- $f"; out $f; done
echo "---- host side (rtnd1)"; tcpdump -n -r "$work/rtnd1.pcap" 2>/dev/null | head -12
if [ $fails -eq 0 ]; then
	if [ "${BASELINE:-0}" = 1 ]; then echo "ok   baseline: SANA-II uaenet.device on FS-UAE"; else echo "ok   netdev backend on FS-UAE (fakenet.device over uaenet.device)"; fi
	rm -rf "$work"
	exit 0
fi
echo "$fails failures; files in $work"
exit 1
