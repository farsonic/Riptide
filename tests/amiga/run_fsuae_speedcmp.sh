#!/bin/sh
# Riptide - the speed comparison: TCP and UDP throughput of one Amiga
# TCP/IP stack on FS-UAE, measured the same way for every stack so the
# numbers can be put side by side (tools/speedcmp_table.py). The machine is
# the same for all: A1200, 68020 (FS-UAE's A1200 preset, cycle exact),
# 2 MB chip + 8 MB fast RAM, a real Kickstart and an installed Workbench
# (both copyrighted, never committed), uaenet.device unit 2 bridged through
# tools/fs-uae (libpcap) to a veth pair on the host: no slirp, no NAT. The
# programs are the speed lab's (bsdsocket.library calls only):
#   tests/amiga/netbench.c   REPEAT downloads and uploads of BYTES against
#                            tests/amiga/netbench_host.py, with the CPU
#                            time left idle (-I); a loopback transfer (-L)
#   tests/amiga/udpblast.c   UDPCOUNT datagrams of 1400 bytes to
#                            tests/amiga/udpsink.py, blocking sends
# and the host pings the Amiga while it is idle. One boot per stack; the
# JSON report holds every run and the medians.
#
#   STACK=riptide|amitcp|amitcp_ng|aminetxduo|miamidx|miami32|prismnet|tolunnet \
#   FSUAE_PCAP=path/to/fs-uae KICKSTART_ROM=... WB_DIR=... [STACK_DIR=...] \
#     tests/amiga/run_fsuae_speedcmp.sh OUT.json
#
#   STACK_DIR   the stack's files: riptide a Riptide build directory
#               (default build/amiga-gcc); amitcp the AmiTCP-3.0b2 drawer;
#               amitcp_ng its tree with build/ and install/; aminetxduo its
#               unpacked archive; prismnet a directory with its
#               bsdsocket.library, usergroup.library and PrismNet; tolunnet
#               a directory with its tolunnet program; miamidx the MiamiDx
#               drawer as Install_MiamiDx lays it out (MUI modules in
#               Libs/, a saved MiamiDx.default); miami32 a drawer with
#               Miami (3.2b), MiamiInit and a MiamiInit.config. The Miamis
#               need MUI_DIR (MUI 3.8's MUI drawer) and go online over ARexx.
#   NETBENCH, UDPBLAST  the programs (default build/amiga/netbench, the vbcc
#               build, whose -I measures the idle CPU; build/amiga/udpblast)
#   BYTES (2097152), REPEAT (3), UDPCOUNT (3000), LOOPBYTES (BYTES)
#   SC_IF, SC_PEER, SC_NET  veth names and /24 (rtsc0, rtsc1, 192.168.83)
#   TIMEOUT     seconds per boot (900); UPTIME seconds until the stack must
#               answer pings (180)
#   UNIT        the uaenet.device unit (2: the first bridged one);
#               A_HOST, H_HOST the Amiga's and the host's numbers in the
#               /24 (2 and 1). The Miamis' saved settings cannot name
#               another unit, so they run as UNIT=0 SC_NET=10.0.2
#               A_HOST=15 H_HOST=2 under an FS-UAE whose pcap interfaces
#               start at unit 0 (FSUAE_UAENET_PCAP_FIRST=1, tools/fs-uae).
#   FSUAE_EXTRA more FS-UAE settings
# Needs root (veth, packet sockets), python3. Headless: Xvfb on
# :${XDISP:-241} when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
OUT=${1:?usage: STACK=name FSUAE_PCAP=... KICKSTART_ROM=... WB_DIR=... $0 OUT.json}
STACK=${STACK:-riptide}
here=$(cd "$(dirname "$0")" && pwd)
NETBENCH=${NETBENCH:-build/amiga/netbench}
UDPBLAST=${UDPBLAST:-build/amiga/udpblast}
BYTES=${BYTES:-2097152}
REPEAT=${REPEAT:-3}
UDPCOUNT=${UDPCOUNT:-3000}
LOOPBYTES=${LOOPBYTES:-$BYTES}
SC_IF=${SC_IF:-rtsc0}
SC_PEER=${SC_PEER:-rtsc1}
NET=${SC_NET:-192.168.83}
TIMEOUT=${TIMEOUT:-900}
UPTIME=${UPTIME:-180}
UNIT=${UNIT:-2}
A_HOST=${A_HOST:-2}      # host numbers in the /24: the Amiga and the host
H_HOST=${H_HOST:-1}
PORT=9200
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ -n "${KICKSTART_ROM:-}" ] && [ -f "$KICKSTART_ROM" ] && [ -n "${WB_DIR:-}" ] && [ -d "$WB_DIR" ] ||
	{ echo "SKIP: KICKSTART_ROM and WB_DIR (an installed Workbench) are needed"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet sockets)"; exit 0; }
for f in "$NETBENCH" "$UDPBLAST"; do
	[ -f "$f" ] || { echo "SKIP: $f not built (make -f Makefile.amiga)"; exit 0; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-speedcmp.XXXXXX")
out=$work/out
sys=$work/System
mkdir -p "$out"
cp -R "$WB_DIR" "$sys"
mkdir -p "$sys/C" "$sys/S" "$sys/Libs" "$sys/Devs"
cp "$NETBENCH" "$sys/C/netbench"
cp "$UDPBLAST" "$sys/C/udpblast"
A=$NET.$A_HOST      # the Amiga
H=$NET.$H_HOST      # the host
us=$work/start-cmds

# ---- one function per stack: files into $sys, start commands on stdout ----
install_riptide() {
	d=${STACK_DIR:-build/amiga-gcc}
	cp "$d/Riptide" "$d/rtcheck" "$sys/C/"
	mkdir -p "$sys/Prefs/Env-Archive/Riptide"
	cat > "$sys/Prefs/Env-Archive/Riptide/riptide.conf" <<-CONF
	hostname speedcmp
	interface eth0 device=uaenet.device unit=$UNIT
	ifconfig eth0 $A/24 up
	route add default $H
	mdns off
	ntp off
	CONF
	echo 'C:rtcheck START="C:Riptide >Out:stack.log" NOCHECK >Out:start.log'
}

# AmiTCP 3.0b2 as Install_AmiTCP sets it up (AmiTCP: drawer, its libs and
# devs added to the assigns), the interface as its HowToInstall describes
install_amitcp() {
	cp -R "${STACK_DIR:?the unpacked AmiTCP-3.0b2 directory}" "$sys/AmiTCP"
	printf 'HOST %s speedcmp\n' "$A" > "$sys/AmiTCP/db/netdb-myhost"
	sed -i 's#^WITH \([a-z.-]*\)#WITH AmiTCP:db/\1#' "$sys/AmiTCP/db/netdb"
	# the interface "uae0" is SANA-II unit $UNIT (db/interfaces: UNIT=)
	printf 'uae dev=uaenet.device unit=%s\n' "$UNIT" >> "$sys/AmiTCP/db/interfaces"
	echo 'Assign AmiTCP: SYS:AmiTCP'
	echo 'Assign LIBS: AmiTCP:libs ADD'
	echo 'Assign DEVS: AmiTCP:devs ADD'
	echo 'Run >Out:stack.log AmiTCP:AmiTCP'
	echo 'SYS:Rexxc/WaitForPort AMITCP'
	echo 'AmiTCP:bin/ifconfig lo0 localhost >Out:start.log'
	echo 'AmiTCP:bin/route add localhost localhost >>Out:start.log'
	echo "AmiTCP:bin/ifconfig uae0 $A netmask 255.255.255.0 >>Out:start.log"
	echo "AmiTCP:bin/route add default $H >>Out:start.log"
	echo 'AmiTCP:bin/ifconfig uae0 >>Out:start.log'
	echo 'AmiTCP:bin/netstat -in >>Out:start.log'
	echo 'AmiTCP:bin/netstat -rn >>Out:start.log'
}

# AmiTCP_NG: its self-starting LIBS:bsdsocket.library, the AmiTCP: db
# drawer of its installer, a Roadshow-style DEVS:NetInterfaces/eth0
install_amitcp_ng() {
	d=${STACK_DIR:?the AmiTCP_NG directory with build/ and install/}
	cp "$d/build/bsdsocket.library" "$d/build/usergroup.library" "$sys/Libs/"
	mkdir -p "$sys/AmiTCP"
	cp -R "$d/install/db" "$sys/AmiTCP/db"
	[ -d "$d/install/Devs" ] && cp -R "$d/install/Devs/." "$sys/Devs/"
	cp "$d/build/AddNetInterface" "$sys/C/"
	mkdir -p "$sys/Devs/NetInterfaces"
	printf 'device=uaenet.device\nunit=%s\naddress=%s\nnetmask=255.255.255.0\ngateway=%s\n' \
		"$UNIT" "$A" "$H" > "$sys/Devs/NetInterfaces/eth0"
	echo 'Assign AmiTCP: SYS:AmiTCP'
	echo 'C:AddNetInterface eth0 >Out:start.log'
}

# AmiNetXDuo's "full" stack installed into the system as its Installer's
# system layout does, with a static DEVS:NetInterfaces/eth0
install_aminetxduo() {
	d=${STACK_DIR:?the unpacked AmiNetXDuo drawer}
	cp "$d/Libs/bsdsocket.library" "$d/Libs/usergroup.library" "$d/Libs/tls.library" "$sys/Libs/"
	for f in "$d"/C/*; do
		[ -f "$f" ] && cp "$f" "$sys/C/"
	done
	cp -R "$d/Devs/Internet" "$sys/Devs/"
	mkdir -p "$sys/Devs/NetInterfaces"
	printf 'DEVICE=uaenet.device\nUNIT=%s\nCONFIGURE=STATIC\nADDRESS=%s\nNETMASK=255.255.255.0\nGATEWAY=%s\n' \
		"$UNIT" "$A" "$H" > "$sys/Devs/NetInterfaces/eth0"
	echo 'C:AddNetInterface DEVS:NetInterfaces/eth0 >Out:start.log'
	echo 'C:ShowNetStatus >>Out:start.log'
}

# PrismNet: the library starts C:PrismNet; settings in ENVARC:PrismNet
install_prismnet() {
	d=${STACK_DIR:?a directory with the PrismNet program and its libraries}
	cp "$d/bsdsocket.library" "$d/usergroup.library" "$sys/Libs/"
	cp "$d/PrismNet" "$sys/C/"
	p=$sys/Prefs/Env-Archive/PrismNet
	mkdir -p "$p/interfaces"
	printf 'AUTOSTART=YES\nCOMMAND=C:PrismNet\n' > "$p/startup"
	printf 'speedcmp\n' > "$p/hostname"
	printf '127.0.0.1 localhost\n%s speedcmp\n' "$A" > "$p/hosts"
	: > "$p/dns"
	printf 'DEFAULT %s\n' "$H" > "$p/routes"
	printf 'DEVICE=uaenet.device UNIT=%s\nCONFIG=STATIC ADDRESS=%s NETMASK=255.255.255.0 GATEWAY=%s\n' \
		"$UNIT" "$A" "$H" > "$p/interfaces/eth0"
	echo 'Copy SYS:Prefs/Env-Archive/PrismNet ENV:PrismNet ALL QUIET'
	echo 'C:PrismNet START >Out:start.log'
	echo 'Wait 3'
	echo 'C:PrismNet STATUS >>Out:start.log'
}

# tolunnet: C:tolunnet reads DEVS:tolunnet.config
install_tolunnet() {
	d=${STACK_DIR:?a directory with the tolunnet program}
	cp "$d/tolunnet" "$sys/C/"
	printf 'DEVICE=uaenet.device\nUNIT=%s\nDHCP=NO\nIP=%s\nNETMASK=255.255.255.0\nGATEWAY=%s\nHOSTNAME=speedcmp\nLOG=Out:tolunnet-daemon.log\n' \
		"$UNIT" "$A" "$H" > "$sys/Devs/tolunnet.config"
	echo 'Stack 32768'
	echo 'Run <NIL: >Out:stack.log C:tolunnet'
	echo 'Wait 5'
}

# MiamiInit's settings file is IFF with the unit, address, gateway and
# netmask at fixed places (DEVI chunk: unit; PROT chunk: address, gateway,
# netmask); this rewrites those for our network
patch_miamiinit() {
	python3 -I - "$1" "$UNIT" "$A" "$H" "$2" "$3" <<-'PY'
	import socket, sys
	p, unit, a, h, olda, oldh = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6]
	d = bytearray(open(p, 'rb').read())
	i = d.find(b'DEVI')
	assert i > 0, 'not a MiamiInit config'
	d[i + 12:i + 16] = unit.to_bytes(4, 'big')      # DEVI: type, unit, device name
	for old, new in ((olda, a), (oldh, h)):
	    o, n = socket.inet_aton(old), socket.inet_aton(new)
	    assert d.count(o) == 1, 'address %s not found once' % old
	    d[:] = d.replace(o, n)
	open(p, 'wb').write(d)
	PY
}

# Miami 3.2b: its program, MiamiInit's config rewritten for this network
# and imported at start; online over ARexx. MiamiDx: the same, through
# its own import (it understands MiamiInit files too).
install_miami() {
	d=${STACK_DIR:?the Miami drawer}
	[ -n "${MUI_DIR:-}" ] && [ -f "$MUI_DIR/Libs/muimaster.library" ] || { echo "FAIL: MUI_DIR needed" >&2; exit 1; }
	cp -R "$d" "$sys/Miami"
	cp -R "$MUI_DIR" "$sys/MUI"
	# the saved settings were made for 10.0.2.15 with the router 10.0.2.2
	patch_miamiinit "$sys/Miami/MiamiInit.config" 10.0.2.15 10.0.2.2
	prog=${MIAMI_PROG:-Miami}
	if [ -f "$sys/Miami/$prog.default" ]; then
		# saved settings too, in case the import is not honoured: the
		# IFAC chunk's addresses, and the unit (MIAMI_UNIT_OFFSET into it)
		python3 -I - "$sys/Miami/$prog.default" "$UNIT" "$A" "$H" "${MIAMI_UNIT_OFFSET:-26}" <<-'PY'
		import socket, sys
		p, unit, a, h, uo = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], int(sys.argv[5])
		d = bytearray(open(p, 'rb').read())
		i = d.find(b'IFAC')
		if i > 0:
		    d[i + 8 + uo:i + 8 + uo + 4] = unit.to_bytes(4, 'big')
		for old, new in (('10.0.2.15', a), ('10.0.2.2', h)):
		    o, n = socket.inet_aton(old), socket.inet_aton(new)
		    if d.count(o) == 1:
		        d[:] = d.replace(o, n)
		open(p, 'wb').write(d)
		PY
	fi
	echo 'Assign MUI: SYS:MUI'
	echo 'Assign LIBS: MUI:Libs ADD'
	echo 'Assign Miami: SYS:Miami'
	if [ "$prog" = MiamiDx ]; then
		mkdir -p "$sys/Prefs/Env-Archive/MIAMI"
		printf 'MUI' > "$sys/Prefs/Env-Archive/MIAMI/GUI"
		echo 'SetEnv MIAMI/GUI MUI'
	fi
	echo 'SYS:System/RexxMast >NIL:'
	echo "Run >Out:stack.log Miami:$prog IMPORTMIAMIINIT=Miami:MiamiInit.config SAVESETTINGS NOGUI"
	echo 'SYS:Rexxc/WaitForPort MIAMI.1'
	echo 'Wait 3'
	echo "SYS:Rexxc/RX \"address 'MIAMI.1' 'ONLINE'\" >Out:start.log"
	echo 'Wait 5'
	echo "SYS:Rexxc/RX \"address 'MIAMI.1' 'ISONLINE'; say 'ISONLINE rc' rc\" >>Out:start.log"
}

case $STACK in
riptide) install_riptide > "$us" ;;
amitcp) install_amitcp > "$us" ;;
amitcp_ng) install_amitcp_ng > "$us" ;;
aminetxduo) install_aminetxduo > "$us" ;;
prismnet) install_prismnet > "$us" ;;
tolunnet) install_tolunnet > "$us" ;;
miami32) install_miami > "$us" ;;
miamidx) MIAMI_PROG=MiamiDx install_miami > "$us" ;;
*) echo "FAIL: unknown STACK=$STACK"; exit 1 ;;
esac

nb="C:netbench -n $BYTES"
{
	echo 'FailAt 30'
	echo 'Stack 65536'
	cat "$us"
	echo 'C:netbench -W 120 >Out:ready'
	echo 'Echo up >Out:up'
	# the link may take a while: up to 12 tries at the warm-up transfer
	i=1
	while [ $i -le 12 ]; do
		echo "C:netbench -n 65536 -l warmup -d $H $PORT >>Out:warmup"
		echo 'If NOT FAIL'
		echo '  Skip ready'
		echo 'EndIf'
		echo 'C:netbench -s 5'
		i=$((i + 1))
	done
	echo 'Lab ready'
	echo 'Echo ready >Out:online'
	i=1
	while [ $i -le "$REPEAT" ]; do
		echo "$nb -I -l run$i -d $H $PORT >>Out:nb.txt"
		echo "$nb -I -l run$i -u $H $PORT >>Out:nb.txt"
		i=$((i + 1))
	done
	echo "C:udpblast -n $UDPCOUNT -s 1400 -l udp $H $((PORT + 1)) >>Out:udp.txt"
	echo 'C:netbench -s 3'
	echo "C:netbench -L -n $LOOPBYTES -l loop >>Out:loop.txt"
	echo 'Echo done >Out:zz_done'
	echo 'C:netbench -s 90'
} > "$sys/S/User-Startup"
cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = A1200
kickstart_file = $KICKSTART_ROM
fast_memory = 8192
hard_drive_0 = $sys
hard_drive_0_label = System
hard_drive_1 = $out
hard_drive_1_label = Out
sound_card = none
floppy_drive_count = 0
automatic_input_grab = 0
video_sync = off
uae_sana2 = true
uaenet_pcap = $SC_IF
base_dir = $work/fsuae
${FSUAE_EXTRA:-}
CFG

ip link del "$SC_IF" 2>/dev/null
ip link add "$SC_IF" type veth peer name "$SC_PEER" 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
xpid=
hpid=
spid=
upid=
cleanup() {
	[ -n "$upid" ] && kill "$upid" 2>/dev/null
	[ -n "$hpid" ] && kill "$hpid" 2>/dev/null
	[ -n "$spid" ] && kill "$spid" 2>/dev/null
	[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
	ip link del "$SC_IF" 2>/dev/null
}
trap cleanup EXIT
ip link set "$SC_IF" up
ip link set "$SC_PEER" up
ip addr add "$H/24" dev "$SC_PEER"
if command -v ethtool >/dev/null 2>&1; then
	for d in "$SC_IF" "$SC_PEER"; do ethtool -K "$d" tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb ":${XDISP:-241}" -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=":${XDISP:-241}"
	export DISPLAY
	sleep 1
fi
python3 -I "$here/netbench_host.py" "$H" $PORT >"$work/host.log" 2>&1 &
hpid=$!
python3 -I "$here/udpsink.py" "$SC_PEER" "$H" $((PORT + 1)) - 2 >"$work/sink.log" 2>&1 &
spid=$!
sleep 1
t_start=$(date +%s)
SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
online=
t=0
while [ $t -lt "$TIMEOUT" ]; do
	[ -s "$out/zz_done" ] && break
	kill -0 $upid 2>/dev/null || break
	if [ -z "$online" ] && [ -f "$out/up" ] && ping -c 1 -W 1 "$A" >/dev/null 2>&1; then
		online=$(( $(date +%s) - t_start ))
		echo "speedcmp: $STACK answers pings after $online s"
	fi
	if [ $t -ge "$UPTIME" ] && [ -z "$online" ]; then
		echo "WARN: $STACK did not answer pings within $UPTIME s"
		UPTIME=$TIMEOUT
	fi
	sleep 2
	t=$((t + 2))
done
if [ -s "$out/zz_done" ]; then
	# the Amiga is idle now: round trips
	ping -c 10 -i 0.2 "$A" > "$work/ping.txt" 2>&1
else
	python3 "$here/fsuae_pointer.py" shot "$work/stopped.png" >/dev/null 2>&1
	echo "WARN: $STACK did not finish in $TIMEOUT s (screen: $work/stopped.png)"
fi
sleep 3        # the sink's last burst
kill $upid 2>/dev/null
wait $upid 2>/dev/null
upid=
for f in "$out"/*; do
	[ -f "$f" ] && tr -d '\r' < "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done
cat "$out/nb.txt" "$out/udp.txt" "$out/loop.txt" 2>/dev/null
python3 -I "$here/speedcmp_report.py" "$out" "$work" "$OUT" "$STACK" "$BYTES" "$REPEAT" "$UDPCOUNT" "${online:-}"
echo "run_fsuae_speedcmp: $STACK done, $OUT (files in $work)"
