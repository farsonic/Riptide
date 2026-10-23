#!/bin/sh
# Riptide - RiptidePrefs on FS-UAE (AROS Kickstart): open the window, walk
# the pages and edit some fields with the keyboard (xdotool), save, and
# check ENVARC:/ENV: riptide.conf and the backup.
#
#   tests/amiga/run_fsuae_prefs.sh [BUILD] [OUTDIR]
#
# BUILD is build/amiga (default) or build/amiga-gcc. Needs fs-uae, Xvfb (or
# DISPLAY), xdotool, ImageMagick, Python PIL and vbcc + NDK (for the assign
# helper: the AROS ROM has no C:Assign). Screenshots of each page stay in
# OUTDIR (default: a temporary directory).
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
here=$(cd "$(dirname "$0")" && pwd)
ptr="python3 $here/fsuae_pointer.py"
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
XDISPLAY=${XDISPLAY:-:119}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool not installed"; exit 0; }
[ -f "$BUILD/RiptidePrefs" ] || { echo "SKIP: $BUILD/RiptidePrefs not built"; exit 0; }
[ -x "$VBCC/bin/vc" ] || { echo "SKIP: vbcc not found"; exit 0; }
work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/riptide-prefs.XXXXXX")}
mkdir -p "$work"
work=$(cd "$work" && pwd)
dh0=$work/dh0
rm -rf "$dh0"
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out" "$dh0/ENV/Riptide" "$dh0/Prefs/Env-Archive/Riptide"
cp "$BUILD/RiptidePrefs" "$dh0/C/"

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

# the configuration to edit: comments and lines RiptidePrefs does not
# know must come back unchanged
cat > "$work/orig.conf" <<'EOF'
# Riptide configuration (RiptidePrefs test)

interface eth0 device=uaenet.device unit=0 mtu=1500 tcpwindow=1440
dhcp eth0
hostname amy    # the old name
# keep this comment
compat aliases on
sysctl net.inet.tcp.delack_ms=40
EOF
cp "$work/orig.conf" "$dh0/Prefs/Env-Archive/Riptide/riptide.conf"
cp "$work/orig.conf" "$dh0/ENV/Riptide/riptide.conf"
{
	echo "FailAt 30"
	echo "DH0:C/tassign ENVARC DH0:Prefs/Env-Archive"
	echo "DH0:C/tassign ENV DH0:ENV"
	echo "DH0:C/RiptidePrefs"
	echo "Echo \"rc \$RC\" >DH0:out/zz_done"
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
CFG

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb "$XDISPLAY" -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=$XDISPLAY
	export DISPLAY
	sleep 1
fi
SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
cleanup() {
	[ -n "$upid" ] && kill "$upid" 2>/dev/null && wait "$upid" 2>/dev/null
	upid=
	[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
	xpid=
}
trap cleanup EXIT
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }
key() {
	for k in "$@"; do
		xdotool key "$k"
		sleep ${KEYDELAY:-0.7}
	done
}
typ() {
	xdotool type --delay 200 "$1"
	sleep ${KEYDELAY:-0.7}
}
settle() {
	$ptr stable 2 60 >/dev/null || fail "screen did not settle before $1"
	$ptr shot "$work/$1.png"
}

# wait for the window (the screen settles once RiptidePrefs is up)
sleep 8
settle 01-interface
# IPv4 off (IPv6 stays on)
key 4
settle 01b-interface-ipv4-off
# Address page: switch to static to show the fields, then back to DHCP
key p
settle 02-address
key m
settle 03-address-static
# Save with an empty static address: a requester, the window stays
key s
settle 03b-invalid
$ptr click 511 407 >/dev/null    # "OK"
settle 03c-dismissed
# the requester leaves the address field active: leave it, back to DHCP
key Return m
# Extra addresses: a bad one (requester), then 10.0.1.2/24
key p
settle 03d-extra
key l
typ '10.0.1.300'
key Return d
settle 03e-extra-invalid
$ptr click 511 407 >/dev/null    # "OK"
settle 03f-extra-dismissed
key Return l
key BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace
typ '10.0.1.2/24'
key Return d
settle 03g-extra-added
# Names: replace the host name, add a domain and a name server
key p
settle 04-names
key h Delete Delete Delete
typ zeus
key Return
key d
typ example.org
key Return
key 1
typ 192.168.1.1
key Return
settle 05-names-edited
# Time: NTP on with a server, mDNS on
key p
settle 06-time
key n
settle 07-time-ntp
key v
typ pool.ntp.org
key Return
key m
settle 08-time-edited
key p
settle 09-tcp
# Advanced: Connecting: Fast Open on, connect timeout 15 s
key p
settle 09a-adv-connecting
key f t
typ 15
key Return
settle 09b-adv-connecting-edited
key p
settle 09c-adv-ports
# Advanced: Keepalive: keepalive for all after 120 s
key p
settle 09d-adv-keepalive
key k
typ 120
key Return
settle 09e-adv-keepalive-edited
# Advanced: Background: LEDBAT by default, background programs
key p
settle 09f-adv-background
key t t t
settle 09g-adv-background-ledbat
key r
typ 'AmiFTP,wget'
key Return
settle 09g-adv-background-edited
key p
settle 09h-adv-memory
key p
settle 09i-adv-happyeyeballs
# Advanced: QoS: bands on
key p
settle 09j-adv-qos
key q
settle 09k-adv-qos-edited
# Advanced: QoS rules: add one
key p
settle 09l-adv-qosrules
key l
typ 'port=22,23 priority=interactive'
key Return d
settle 09m-adv-qosrules-added
key p
settle 10-other
# Other lines: add one
key l
typ 'compat bsdsocket off'
key Return d
settle 11-other-added
key s

t=0
while [ $t -lt 30 ] && [ ! -s "$dh0/out/zz_done" ]; do sleep 1; t=$((t + 1)); done
[ -s "$dh0/out/zz_done" ] || fail "RiptidePrefs did not exit after Save"
$ptr shot "$work/12-after.png"
cleanup

conf=$dh0/Prefs/Env-Archive/Riptide/riptide.conf
cat > "$work/expect.conf" <<'EOF'
# Riptide configuration (RiptidePrefs test)

interface eth0 device=uaenet.device unit=0 mtu=1500 tcpwindow=1440
dhcp eth0
ifconfig eth0 alias 10.0.1.2/24
nameserver 192.168.1.1
hostname zeus    # the old name
domain example.org
# keep this comment
compat aliases on
sysctl net.inet.tcp.delack_ms=40
compat bsdsocket off
ntp pool.ntp.org
mdns on
ipv4 off
qos on
qos rule port=22,23 priority=interactive
sysctl net.inet.tcp.fastopen=1
sysctl net.inet.tcp.connect_timeout=15
sysctl net.inet.tcp.keepalive_all=120
sysctl net.inet.tcp.cc=ledbat
sysctl net.inet.tcp.bg_programs=AmiFTP,wget
EOF
cmp -s "$work/orig.conf" "$conf.bak" || fail "ENVARC:Riptide/riptide.conf.bak is not the old file"
cmp -s "$conf" "$dh0/ENV/Riptide/riptide.conf" || fail "ENV: and ENVARC: riptide.conf differ"
if ! cmp -s "$work/expect.conf" "$conf"; then
	fail "riptide.conf is not as expected:"
	diff -u "$work/expect.conf" "$conf" | sed 's/^/  | /'
fi
if [ $fails -eq 0 ]; then
	echo "ok   RiptidePrefs on FS-UAE ($BUILD): pages (Advanced too), keyboard edits, list pages, Save (ENVARC:, ENV:, .bak)"
	echo "     screenshots: $work/*.png"
	exit 0
fi
echo "$fails failures; files in $work"
exit 1
