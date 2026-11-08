#!/bin/sh
# Riptide - "Riptide RESTART" on the real Amiga build: FS-UAE built with
# tools/fs-uae (uaenet.device bridged through libpcap) on the veth end
# RS_IF; the host end RS_PEER has RS_NET.1/24 and RS_NET2.1/24. Programs
# keep bsdsocket.library open throughout: six "nc -l" listeners and two
# connections to a host server. Then five restarts, each with another
# configuration (the address moves between the two subnets, one adds a
# card whose driver does not exist). Checks:
#   - QUIT is refused with the new hint while the libraries are open
#   - after every restart: the interface has the new address, ping to the
#     host works, a listener bound before the restarts accepts at the new
#     address, STATUS shows one driver open and as many signals as before
#   - a connection whose address stays survives the restart (the host's
#     data sent afterwards arrives); one whose address goes is reset after
#     the grace time (its nc ends, netstat no longer lists it)
#   - the missing driver is reported, the stack goes on without it
#   - at the end the host connects from outside to the last listener
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_restart.sh [BUILDDIR] [TIMEOUT]
#
# NETDEV=1 puts the card on the netdev backend instead of SANA-II:
# tests/amiga/fakenetdev.c (built with vbcc + NDK) as "fakenet.device"
# over uaenet.device, as tests/amiga/run_fsuae_netdev.sh does, so every
# restart detaches and attaches a netdev driver again.
#
# Needs root (veth, packet capture) and python3. Headless: starts Xvfb on
# :97 when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
RS_IF=${RS_IF:-rtrs0}
RS_PEER=${RS_PEER:-rtrs1}
RS_NET=${RS_NET:-192.168.140}
RS_NET2=${RS_NET2:-192.168.141}
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
here=$(cd "$(dirname "$0")" && pwd)
NETDEV=${NETDEV:-0}
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
if [ "$NETDEV" = 1 ]; then
	DEV="device=fakenet.device unit=0"
	[ -x "$VBCC/bin/vc" ] || { echo "SKIP: NETDEV=1 needs vbcc"; exit 0; }
else
	DEV="device=uaenet.device unit=2"
fi
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
for f in Riptide rtcheck ping nc netstat; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
ip link add $RS_IF type veth peer name $RS_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
cleanup() { ip link del $RS_IF 2>/dev/null; }
trap cleanup EXIT
ip link set $RS_IF up
ip link set $RS_PEER up
ip addr add $RS_NET.1/24 dev $RS_PEER
ip addr add $RS_NET2.1/24 dev $RS_PEER
if command -v ethtool >/dev/null 2>&1; then
	for d in $RS_IF $RS_PEER; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-restart.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck ping nc netstat; do cp "$BUILD/$f" "$dh0/C/"; done
if [ "$NETDEV" = 1 ]; then
	PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O2 -I"$NDK/Include_H" -o "$dh0/C/fakenetdev" \
		"$here/fakenetdev.c" -lamiga >"$work/vc.log" 2>&1 || { echo "FAIL: cannot build fakenetdev"; cat "$work/vc.log"; exit 1; }
fi
# conf ADDR [extra line]: the configuration of one round
conf() {
	printf 'hostname restart-test\nloglevel 2\ninterface eth0 %s\nifconfig eth0 %s/24 up\n' "$DEV" "$1"
	[ $# -gt 1 ] && printf '%s\n' "$2"
	return 0
}
conf $RS_NET.2 > "$dh0/S/riptide.conf"
conf $RS_NET.2 "host foo 10.1.2.3" > "$dh0/S/r1.conf"
conf $RS_NET2.2 > "$dh0/S/r2.conf"
conf $RS_NET.3 "interface eth1 device=missing.device unit=0" > "$dh0/S/r3.conf"
conf $RS_NET2.3 > "$dh0/S/r4.conf"
conf $RS_NET.5 "nameserver $RS_NET.1" > "$dh0/S/r5.conf"
echo "hello listener" > "$dh0/S/msg"
# programs in the background, each from a script of its own (output
# redirected inside it, input NIL: so nc sends nothing and waits)
bg() {
	printf '%s <NIL: >DH0:out/%s\n' "$2" "$1" > "$dh0/S/bg_$1"
	[ $# -gt 2 ] && printf '%s\n' "$3" >> "$dh0/S/bg_$1"
	echo "Run >NIL: Execute DH0:S/bg_$1"
}
# round N ADDR PEER: restart, then ping, the listener on port 778N, status
round() {
	cat <<SEQ
DH0:C/Riptide RESTART $4 >DH0:out/r$1
DH0:C/ping -c 2 $3 >DH0:out/ping$1
DH0:C/nc -w 5 $2 778$1 <DH0:S/msg >DH0:out/self$1
DH0:C/Riptide STATUS >DH0:out/status$1
SEQ
}
{
	cat <<SEQ
FailAt 30
SEQ
	if [ "$NETDEV" = 1 ]; then
		echo "Run >DH0:out/fake DH0:C/fakenetdev uaenet.device 2 DH0:out/fakestats"
		echo "Wait 2"
	fi
	cat <<SEQ
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
SEQ
	for p in 1 2 3 4 5 9; do bg l$p "DH0:C/nc -l -p 778$p"; done
	bg connA "DH0:C/nc $RS_NET.1 9000"
	echo "Wait 1"
	bg connB "DH0:C/nc $RS_NET.1 9000" "Echo ended >DH0:out/connB_end"
	cat <<SEQ
Wait 2
DH0:C/ping -c 2 $RS_NET.1 >DH0:out/ping0
DH0:C/Riptide QUIT >DH0:out/quit
DH0:C/Riptide STATUS >DH0:out/status0
Copy DH0:S/r1.conf DH0:S/riptide.conf
SEQ
	round 1 $RS_NET.2 $RS_NET.1 ""
	cat <<SEQ
Wait 3
DH0:C/netstat -ant >DH0:out/ns_before
SEQ
	round 2 $RS_NET2.2 $RS_NET2.1 CONFIG=DH0:S/r2.conf
	# until B's nc ends (the grace time is Riptide's clock, which under
	# FS-UAE need not run at the pace of Wait)
	cat <<SEQ
Lab waitb
Wait 2
If NOT EXISTS DH0:out/connB_end
  Skip waitb BACK
EndIf
DH0:C/netstat -ant >DH0:out/ns_after
SEQ
	round 3 $RS_NET.3 $RS_NET.1 CONFIG=DH0:S/r3.conf
	round 4 $RS_NET2.3 $RS_NET2.1 CONFIG=DH0:S/r4.conf
	round 5 $RS_NET.5 $RS_NET.1 CONFIG=DH0:S/r5.conf
	cat <<SEQ
Echo ready >DH0:out/ext_ready
Wait 10
DH0:C/Riptide STATUS >DH0:out/status9
Echo done >DH0:out/zz_done
SEQ
} > "$dh0/S/Startup-Sequence"

# the host: connection A gets a line before and one after restart 1
# (same address); B is kept open; at the end a connection from outside
cat > "$work/peer.py" <<'PY'
import os, socket, sys, threading, time
host, out, last, end = sys.argv[1], sys.argv[2], sys.argv[3], time.time() + float(sys.argv[4])
log = open(sys.argv[5], "w", buffering=1)
def wait_for(name):
    while time.time() < end and not os.path.exists(os.path.join(out, name)):
        time.sleep(0.2)
    return os.path.exists(os.path.join(out, name))
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind((host, 9000)); srv.listen(4); srv.settimeout(1)
keep = []
def server():
    n = 0
    while time.time() < end and n < 2:
        try:
            c, a = srv.accept()
        except socket.timeout:
            continue
        n += 1
        if n == 1:
            c.sendall(b"before restart\n")
            log.write("A connected from %s\n" % (a,))
            if wait_for("r1"):
                time.sleep(1)
                c.sendall(b"after restart\n")
                log.write("A: sent after restart\n")
            c.close()
        else:
            c.sendall(b"hello B\n")
            log.write("B connected from %s\n" % (a,))
            keep.append(c)
threading.Thread(target=server, daemon=True).start()
if wait_for("ext_ready"):
    time.sleep(1)
    try:
        s = socket.create_connection((last, 7789), timeout=10)
        s.sendall(b"external hello\n"); s.shutdown(socket.SHUT_WR)
        s.recv(100); s.close()
        log.write("external: connected to %s\n" % last)
    except OSError as e:
        log.write("external: %s\n" % e)
while time.time() < end and not os.path.exists(os.path.join(out, "zz_done")):
    time.sleep(0.5)
PY

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
uaenet_pcap = $RS_IF
base_dir = $work/fsuae
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :97 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:97
	export DISPLAY
	sleep 1
fi
python3 -I "$work/peer.py" $RS_NET.1 "$dh0/out" $RS_NET.5 "$TIMEOUT" "$work/peer.log" >"$work/peer.err" 2>&1 &
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
	return 0
}
expect start 'stack ready'
expect ping0 '2 packets transmitted, 2 (packets )?received'
expect quit 'still open \([0-9]+ opens\); not quitting' "'Riptide RESTART' applies a new configuration"
expect status0 '^Drivers open 1, signals [0-9]+, restarts 0'
[ "$NETDEV" = 1 ] && expect status5 'netdev fakenet\.device unit 0'
sig0=$(out status0 | sed -n 's/^Drivers open [0-9]*, signals \([0-9]*\),.*/\1/p')
for i in 1 2 3 4 5; do
	case $i in
	1) a=$RS_NET.2 ;; 2) a=$RS_NET2.2 ;; 3) a=$RS_NET.3 ;; 4) a=$RS_NET2.3 ;; 5) a=$RS_NET.5 ;;
	esac
	expect r$i 'Riptide: restarted \(1 interface\)'
	expect ping$i '2 packets transmitted, 2 (packets )?received'
	expect status$i "^eth0 +$(echo $a | sed 's/\./\\./g')/255\.255\.255\.0 .*UP" \
		"^Drivers open 1, signals ${sig0:-x}, restarts $i\$"
	expect l$i 'hello listener'
done
expect r3 'missing\.device' 'restarted \(1 interface\), with the errors above'
reject status3 '^eth1 '
expect status5 "DNS: $RS_NET\.1"
reject status4 'DNS: [0-9]'
# A: the address stayed, the line sent after restart 1 arrived
expect connA 'before restart' 'after restart'
# B: listed before restart 2, reset in the grace time after it
expect ns_before "$RS_NET\.1:9000 +(ESTABLISHED|FIN_WAIT2)"
reject ns_after "$RS_NET\.1:9000 +(ESTABLISHED|FIN_WAIT2)"
[ -f "$dh0/out/connB_end" ] || { echo "FAIL connB: nc did not end after its address went"; fails=$((fails + 1)); }
expect riptide.log 'restart: 1 connection reset, their address is gone' 
expect connB 'hello B'
expect l9 'external hello'
expect status9 '^Drivers open 1, signals'
grep -q '^external: connected' "$work/peer.log" 2>/dev/null || { echo "FAIL peer.log: no external connection"; fails=$((fails + 1)); }
for f in quit r1 r3 status1 status2 status3 status5 ns_before ns_after riptide.log; do echo "---- $f"; out $f; done
echo "---- peer:"; cat "$work/peer.log" 2>/dev/null
echo "---- (work dir $work)"
if [ $fails -eq 0 ]; then
	[ "$NETDEV" = 1 ] && echo "run_fsuae_restart: PASSED (netdev backend)" || echo "run_fsuae_restart: PASSED"
	exit 0
fi
echo "run_fsuae_restart: FAILED ($fails)"
exit 1
