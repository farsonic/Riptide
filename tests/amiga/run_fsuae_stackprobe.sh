#!/bin/sh
# Riptide - run stackprobe (src/amiga/stackprobe) under one Amiga TCP/IP
# stack on FS-UAE and keep its JSON report, to compare stacks with
# tools/stackprobe_compare.py. Every stack gets the same interface:
# uaenet.device unit 0 with the static address 10.0.2.15/24 (nothing is
# on the other end; the tests run over loopback and that interface).
#
#   STACK=riptide|amitcp|amitcp_ng|aminetxduo|miamidx|miami|tolunnet|prismnet \
#     tests/amiga/run_fsuae_stackprobe.sh OUT.json [PROBE_ARGS]
#
# The machine is the same for every stack: A1200, 68020 (FS-UAE A1200
# preset), 2 MB chip + 8 MB fast RAM.
#   KICKSTART_ROM, WB_DIR  a Kickstart ROM file and an installed Workbench
#                          drawer (both copyrighted: never committed); the
#                          stack is installed on a copy of WB_DIR, which
#                          boots to the Workbench. Without them the AROS ROM
#                          replacement boots a bare DH0: (STACK=riptide only).
#   STACK_DIR              the stack's files (see install_* below);
#                          riptide: a Riptide build directory (build/amiga)
#   PROBE                  the stackprobe binary (default build/amiga/stackprobe)
#   FSUAE_EXTRA            more FS-UAE settings, e.g. "uae_cpu_model = 68000"
#   TIMEOUT                seconds (default 900)
# Headless: Xvfb on :${XDISP:-193} when DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
OUT=${1:?usage: STACK=name $0 OUT.json [PROBE_ARGS]}
PARGS=${2:-}
STACK=${STACK:-riptide}
PROBE=${PROBE:-build/amiga/stackprobe}
TIMEOUT=${TIMEOUT:-900}
[ -f "$PROBE" ] || { echo "FAIL: no $PROBE (make -f Makefile.amiga)"; exit 1; }
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-stackprobe.XXXXXX")
out=$work/out
mkdir -p "$out"
wb=
if [ -n "${KICKSTART_ROM:-}" ] && [ -n "${WB_DIR:-}" ]; then
	sys=$work/System
	cp -R "$WB_DIR" "$sys"
	wb=1
else
	[ "$STACK" = riptide ] || { echo "SKIP: STACK=$STACK needs KICKSTART_ROM and WB_DIR"; exit 0; }
	sys=$work/dh0
	mkdir -p "$sys/C" "$sys/S"
fi
cp "$PROBE" "$sys/C/stackprobe"
us=$work/user-startup   # commands that start the stack (AmigaDOS)

install_riptide() {
	d=${STACK_DIR:-build/amiga}
	cp "$d/Riptide" "$d/rtcheck" "$sys/C/"
	printf 'hostname stackprobe\ninterface eth0 device=uaenet.device unit=0\nifconfig eth0 10.0.2.15/24\n' \
		> "$sys/S/riptide.conf"
	echo 'C:rtcheck START="C:Riptide >Out:stack.log CONFIG=S:riptide.conf" NOCHECK >Out:start.log'
}

# AmiTCP 3.0b2 (Aminet comm/net/AmiTCP-bin-30b2.lha), as Install_AmiTCP
# sets it up: the AmiTCP: drawer, its libs and the loopback interface
install_amitcp() {
	cp -R "${STACK_DIR:?the unpacked AmiTCP-3.0b2 directory}" "$sys/AmiTCP"
	printf 'HOST 127.0.0.1 stackprobe\n' > "$sys/AmiTCP/db/netdb-myhost"
	sed -i 's#^WITH \([a-z.-]*\)#WITH AmiTCP:db/\1#' "$sys/AmiTCP/db/netdb"
	printf 'uae dev=uaenet.device\n' >> "$sys/AmiTCP/db/interfaces"
	echo 'Assign AmiTCP: SYS:AmiTCP'
	echo 'Assign LIBS: AmiTCP:libs ADD'
	echo 'Assign DEVS: AmiTCP:devs ADD'
	echo 'Run >Out:stack.log AmiTCP:AmiTCP'
	# as AmiTCP's HowToInstall configures the loopback interface
	echo 'SYS:Rexxc/WaitForPort AMITCP'
	echo 'AmiTCP:bin/ifconfig lo0 localhost >Out:start.log'
	echo 'AmiTCP:bin/route add localhost localhost >>Out:start.log'
	echo 'AmiTCP:bin/ifconfig uae/0 10.0.2.15 netmask 255.255.255.0 >>Out:start.log'
	echo 'AmiTCP:bin/netstat -rn >>Out:start.log'
}

# AmiTCP_NG: its self-starting LIBS:bsdsocket.library and usergroup.library,
# and the AmiTCP drawer of its installer (db files)
install_amitcp_ng() {
	d=${STACK_DIR:?the AmiTCP_NG directory with build/ and install/}
	cp "$d/build/bsdsocket.library" "$d/build/usergroup.library" "$sys/Libs/"
	mkdir -p "$sys/AmiTCP"
	cp -R "$d/install/db" "$sys/AmiTCP/db"
	[ -d "$d/install/Devs" ] && cp -R "$d/install/Devs/." "$sys/Devs/"
	cp "$d/build/AddNetInterface" "$sys/C/"
	mkdir -p "$sys/Devs/NetInterfaces"
	printf 'device=uaenet.device\nunit=0\naddress=10.0.2.15\nnetmask=255.255.255.0\n' > "$sys/Devs/NetInterfaces/eth0"
	echo 'Assign AmiTCP: SYS:AmiTCP'
	echo 'C:AddNetInterface eth0 >Out:start.log'
}

# AmiNetXDuo release archive, its "full" stack installed into the system as
# the Installer's system layout does, with a static DEVS:NetInterfaces/eth0
install_aminetxduo() {
	d=${STACK_DIR:?the unpacked AmiNetXDuo drawer}
	cp "$d/Libs/bsdsocket.library" "$d/Libs/usergroup.library" "$d/Libs/tls.library" "$sys/Libs/"
	for f in "$d"/C/*; do
		[ -f "$f" ] && cp "$f" "$sys/C/"
	done
	cp -R "$d/Devs/Internet" "$sys/Devs/"
	mkdir -p "$sys/Devs/NetInterfaces"
	printf 'DEVICE=uaenet.device\nUNIT=0\nCONFIGURE=STATIC\nADDRESS=10.0.2.15\nNETMASK=255.255.255.0\n' \
		> "$sys/Devs/NetInterfaces/eth0"
	echo 'C:AddNetInterface DEVS:NetInterfaces/eth0 >Out:start.log'
	echo 'C:ShowNetStatus >>Out:start.log'
}

# a stack whose STACK_DIR holds files to copy over System: (Libs/, Devs/,
# C/, S/ ...) and an optional "start" file with the commands that start it.
# MiamiDx and Miami are run this way: their drawer as Miami:, settings saved
# from their GUI (docs/COMPARISON.md section 12), and a start file that
# starts RexxMast and the program and sends ONLINE to the MIAMI.1 port
install_tree() {
	d=${STACK_DIR:?the install tree of the stack}
	for x in "$d"/*; do
		[ "$(basename "$x")" = start ] && continue
		cp -R "$x" "$sys/"
	done
	[ -f "$d/start" ] && cat "$d/start"
}

case $STACK in
riptide) install_riptide > "$us" ;;
amitcp) install_amitcp > "$us" ;;
amitcp_ng) install_amitcp_ng > "$us" ;;
aminetxduo) install_aminetxduo > "$us" ;;
*) install_tree > "$us" ;;
esac

{
	cat "$us"
	echo 'Wait 3'
	echo "C:stackprobe $PARGS JSON=Out:probe.json >Out:probe.txt"
	echo 'Echo done >Out:zz_done'
} > "$work/probe-cmds"
if [ -n "$wb" ]; then
	# run from S:User-Startup, before the Workbench loads
	cp "$work/probe-cmds" "$sys/S/User-Startup"
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
${FSUAE_EXTRA:-}
CFG
else
	{ echo 'FailAt 30'; echo 'Stack 65536'; cat "$work/probe-cmds"; } > "$sys/S/Startup-Sequence"
	cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = A1200
fast_memory = 8192
hard_drive_0 = $sys
hard_drive_0_label = DH0
hard_drive_1 = $out
hard_drive_1_label = Out
sound_card = none
floppy_drive_count = 0
automatic_input_grab = 0
video_sync = off
uae_sana2 = true
CFG
fi
xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb ":${XDISP:-193}" -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=":${XDISP:-193}"
	export DISPLAY
	sleep 1
fi
SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ]; do
	[ -s "$out/zz_done" ] && { sleep 2; break; }
	kill -0 $upid 2>/dev/null || break
	sleep 2
	t=$((t + 2))
done
[ -s "$out/zz_done" ] || python3 "$(dirname "$0")/fsuae_pointer.py" shot "$work/stopped.png" >/dev/null 2>&1
kill $upid 2>/dev/null
wait $upid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
for f in "$out"/*.txt "$out"/*.log; do
	[ -f "$f" ] && tr -d '\r' < "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done
cat "$out/probe.txt" 2>/dev/null
if [ -s "$out/probe.json" ]; then
	cp "$out/probe.json" "$OUT"
	echo "run_fsuae_stackprobe: $STACK done, $OUT (files in $work)"
	exit 0
fi
echo "run_fsuae_stackprobe: $STACK FAILED: no report (files in $work)"
exit 1
