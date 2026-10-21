#!/bin/sh
# Riptide - install Riptide with the Commodore/Amiga Technologies Installer
# on FS-UAE (AROS Kickstart), then boot the installed system.
#
#   INSTALLER43=.../Installer [LHA68K=.../lha_68020] tests/amiga/run_fsuae_installer.sh [BUILD] [OUTDIR]
#
# INSTALLER43 is Installer 43.3 from Aminet (util/misc/Installer-43_3.lha,
# freely distributable); LHA68K optionally Amiga LhA 2.15 (util/arc/lha_68k.lha).
# Needs "make -f Makefile.amiga package", fs-uae, Xvfb, xdotool, ImageMagick,
# Python PIL and vbcc + NDK (for a 30-line assign helper: the AROS ROM has
# no C:Assign).
#
# Boot 1: (with LHA68K) unpacks build/riptide-<v>.lha with Amiga LhA into
#         DH0:T/, otherwise copies build/package/. Runs Installer at Novice
#         level; the GUI is driven with xdotool (tests/amiga/fsuae_pointer.py):
#         welcome, about, emulator notes, summary, "Installation complete".
#         Checks the copied programs and docs, ENVARC:/ENV: riptide.conf and
#         hosts, the S:User-Startup block and that Riptide was started.
# Boot 2: a normal boot whose Startup-Sequence executes S:User-Startup with
#         ENV: = ENVARC: (as after the usual copy); checks that Riptide came up
#         from the generated configuration (host name, uaenet.device eth0)
#         and that "rtcheck -v" passes.
# Screenshots of each step stay in OUTDIR (default: a temporary directory).
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build}
TOOLS="ip ifconfig route arp netstat ping traceroute host sysctl hostname nc ftp speedtest"
here=$(cd "$(dirname "$0")" && pwd)
ptr="python3 $here/fsuae_pointer.py"
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool not installed"; exit 0; }
[ -n "${INSTALLER43:-}" ] && [ -f "$INSTALLER43" ] || { echo "SKIP: set INSTALLER43 to Installer 43.3 (Aminet util/misc/Installer-43_3.lha)"; exit 0; }
[ -f "$BUILD/package/Riptide/Install_Riptide" ] || { echo "SKIP: $BUILD/package not built (make -f Makefile.amiga package)"; exit 0; }
[ -x "$VBCC/bin/vc" ] || { echo "SKIP: vbcc not found"; exit 0; }
work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/riptide-installer.XXXXXX")}
mkdir -p "$work"
work=$(cd "$work" && pwd)
dh0=$work/dh0
rm -rf "$dh0"
mkdir -p "$dh0/C" "$dh0/S" "$dh0/T" "$dh0/out" "$dh0/ENV" "$dh0/Prefs/Env-Archive" "$dh0/Libs" "$dh0/Devs"
cp "$INSTALLER43" "$dh0/C/Installer"

cat > "$work/tassign.c" <<'EOF'
/* tassign NAME PATH: AssignLock() (the AROS ROM has no C:Assign) */
#include <proto/dos.h>
int main(int argc, char **argv)
{
	BPTR l;
	if (argc < 3 || !(l = Lock((STRPTR)argv[2], ACCESS_READ)))
		return 20;
	if (!AssignLock((STRPTR)argv[1], l)) {
		UnLock(l);
		return 20;
	}
	return 0;
}
EOF
PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O2 -I"$NDK/Include_H" -o "$dh0/C/tassign" "$work/tassign.c" -lamiga ||
	{ echo "FAIL: cannot build the assign helper"; exit 1; }

lha=$(ls "$BUILD"/riptide-*.lha 2>/dev/null | head -1)
assigns='DH0:C/tassign ENVARC DH0:Prefs/Env-Archive
DH0:C/tassign LIBS DH0:Libs
DH0:C/tassign DEVS DH0:Devs
DH0:C/tassign T DH0:T'
{
	echo "FailAt 30"
	echo "$assigns"
	echo "DH0:C/tassign ENV DH0:ENV"
	if [ -n "${LHA68K:-}" ] && [ -f "$LHA68K" ] && [ -n "$lha" ]; then
		cp "$LHA68K" "$dh0/C/lha"
		cp "$lha" "$dh0/T/riptide.lha"
		echo "DH0:C/lha -q x DH0:T/riptide.lha DH0:T/ >DH0:out/lha"
		unpack=lha
	else
		cp -R "$BUILD/package/Riptide" "$BUILD/package/Riptide.info" "$dh0/T/"
		unpack=copy
	fi
	echo "DH0:C/Installer SCRIPT DH0:T/Riptide/Install_Riptide APPNAME Riptide MINUSER NOVICE DEFUSER NOVICE"
	echo "DH0:C/Riptide STATUS >DH0:out/status1"
	echo "Echo done >DH0:out/zz_boot1"
} > "$dh0/S/Startup-Sequence"
cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = A1200
hard_drive_0 = $dh0
hard_drive_0_label = DH0
sound_card = none
floppy_drive_count = 0
automatic_input_grab = 0
video_sync = off
uae_sana2 = true
network_card = a2065
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :98 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:98
	export DISPLAY
	sleep 1
fi
upid=
boot() {
	SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae-$1.log" 2>&1 &
	upid=$!
}
halt() {
	[ -n "$upid" ] && kill "$upid" 2>/dev/null && wait "$upid" 2>/dev/null
	upid=
}
waitfile() {
	t=0
	while [ $t -lt "$2" ] && [ ! -s "$1" ]; do sleep 1; t=$((t + 1)); done
	[ -s "$1" ]
}
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
prev=
step() {
	# step NAME X Y EX EY: wait for a settled new screen with a button edge at
	# EX,EY, keep a screenshot, click X,Y
	$ptr wait 3 300 "$4" "$5" $prev || fail "boot 1: no '$1' screen"
	$ptr shot "$work/$1.png"
	prev=$work/$1.png
	$ptr click "$2" "$3" >/dev/null
}

# ---- boot 1: install --------------------------------------------------------
boot 1
step 01-welcome 400 484 296 470    # "Proceed With Install" (Novice preselected)
step 02-about 400 511 296 498      # "Proceed"
step 03-notes 400 511 296 498      # emulator notes: "Proceed"
step 04-summary 510 511 405 498    # (exit ...) summary: "Proceed"
step 05-complete 510 511 405 498   # "Installation complete": "Proceed"
waitfile "$dh0/out/zz_boot1" 60 || fail "boot 1: the Installer did not finish (see $work/*.png)"
$ptr shot "$work/06-after.png"
halt
for f in "$dh0"/out/*; do [ -f "$f" ] && tr -d '\r' < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done

out() { cat "$dh0/out/$1" 2>/dev/null; }
expect() {
	n=$1; shift
	for re in "$@"; do
		out "$n" | grep -Eq -- "$re" || { fail "$n: no line matches /$re/"; out "$n" | sed 's/^/  | /'; }
	done
}
for f in Riptide rtcheck $TOOLS ping6 traceroute6 rtdiag; do
	cmp -s "$dh0/C/$f" "$BUILD/package/Riptide/C/$f" || fail "C:$f not installed"
done
[ ! -e "$dh0/C/ftpd-test" ] || fail "ftpd-test installed at Novice level"
for f in AMIGA.md COMPARISON.md README.md LICENSE; do
	cmp -s "$dh0/Riptide/Docs/$f" "$BUILD/package/Riptide/Docs/$f" || fail "DH0:Riptide/Docs/$f not installed"
done
[ -f "$dh0/Riptide.info" ] || fail "no icon for DH0:Riptide"
conf=$dh0/Prefs/Env-Archive/Riptide/riptide.conf
grep -q '^interface eth0 device=uaenet.device unit=0 mtu=1500$' "$conf" 2>/dev/null || fail "riptide.conf: no uaenet.device interface"
grep -q '^dhcp eth0$' "$conf" 2>/dev/null || fail "riptide.conf: no dhcp eth0"
grep -q '^#ifconfig  eth0 10.0.2.15/24 up$' "$conf" 2>/dev/null || fail "riptide.conf: no commented static block"
cmp -s "$conf" "$dh0/ENV/Riptide/riptide.conf" || fail "ENV:Riptide/riptide.conf differs from ENVARC:"
grep -q '^127.0.0.1 localhost$' "$dh0/Prefs/Env-Archive/Riptide/hosts" 2>/dev/null || fail "no hosts file"
us=$(ls "$dh0"/S/[Uu]ser-[Ss]tartup 2>/dev/null | head -1)
[ -n "$us" ] && tr -d '\r' < "$us" | grep -q '^;BEGIN Riptide$' && tr -d '\r' < "$us" | grep -q '^Run >NIL: C:Riptide$' &&
	tr -d '\r' < "$us" | grep -q '^;END Riptide$' || fail "S:User-Startup has no Riptide block"
expect status1 '^Riptide [0-9.]+, host amiga' 'SANA-II uaenet.device unit 0'

# ---- boot 2: start from S:User-Startup --------------------------------------
{
	echo "FailAt 30"
	echo "$assigns"
	echo "DH0:C/tassign ENV DH0:Prefs/Env-Archive"
	echo "If EXISTS S:User-Startup"
	echo "  Execute S:User-Startup"
	echo "EndIf"
	# wait until the stack is up (rtcheck polls for bsdsocket.library)
	echo 'C:rtcheck START="C:Riptide STATUS >NIL:" NOCHECK >DH0:out/wait'
	echo "C:Riptide STATUS >DH0:out/status2"
	echo "C:rtcheck -v >DH0:out/rtcheck"
	echo "C:hostname >DH0:out/hostname"
	echo "C:ping -c 2 127.0.0.1 >DH0:out/ping"
	echo "Echo done >DH0:out/zz_boot2"
} > "$dh0/S/Startup-Sequence"
boot 2
waitfile "$dh0/out/zz_boot2" 240 || fail "boot 2 did not finish"
sleep 1
halt
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
for f in "$dh0"/out/*; do [ -f "$f" ] && tr -d '\r' < "$f" > "$f.tmp" && mv "$f.tmp" "$f"; done
expect wait 'stack ready: bsdsocket.library'
expect status2 '^Riptide [0-9.]+, host amiga' '^eth0 ' 'SANA-II uaenet.device unit 0' 'DHCP'
expect hostname '^amiga$'
expect rtcheck 'rtcheck: PASSED'
expect ping '2 packets transmitted, 2 received'

if [ $fails -eq 0 ]; then
	echo "ok   Installer 43.3 on FS-UAE: Novice install ($unpack), Riptide from S:User-Startup, $(grep -o 'PASSED.*' "$dh0/out/rtcheck")"
	echo "     screenshots: $work/*.png"
	exit 0
fi
echo "$fails failures; files in $work"
exit 1
