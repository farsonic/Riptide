#!/bin/sh
# Riptide - RiptidePrefs' MUI editor on FS-UAE (AROS Kickstart) with MUI
# 3.8: the same keyboard edits as run_fsuae_prefs.sh must give the same
# riptide.conf; screenshots of every page in both themes, on a 256-colour
# 640x480 screen, a 640x512 one and the 640x256 Workbench (compact layout;
# the AROS ROM opens no screens wider than 640); the help bubble texts
# (DEBUG log), the pointer
# lighting a page, menu commands through CMD= (About, Dark Theme, Reset to
# Defaults), the saved theme, and GADTOOLS.
#
#   MUI_DIR=/path/to/MUI tests/amiga/run_fsuae_prefs_mui.sh [BUILD] [OUTDIR]
#
# MUI_DIR is the MUI drawer of MUI 3.8's user archive (Aminet
# util/libs/mui38usr.lha: Libs/, Images/); MUI is not part of Riptide. The
# AROS ROM has no diskfont, iffparse, locale, commodities, asl or
# datatypes.library, which MUI opens: stubs that fail every call stand in
# (mkstublib.py), so MUI runs without disk fonts, saved MUI settings,
# catalogs or file requesters. Needs fs-uae, Xvfb (or DISPLAY), xdotool,
# ImageMagick, Python PIL, vbcc + NDK (helpers) and vasm.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
here=$(cd "$(dirname "$0")" && pwd)
ptr="python3 $here/fsuae_pointer.py"
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
VASM=${VASM:-$VBCC/bin/vasmm68k_mot}
XDISPLAY=${XDISPLAY:-:118}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool not installed"; exit 0; }
[ -n "${MUI_DIR:-}" ] && [ -f "$MUI_DIR/Libs/muimaster.library" ] ||
	{ echo "SKIP: set MUI_DIR to the MUI drawer of MUI 3.8 (mui38usr.lha)"; exit 0; }
[ -f "$BUILD/RiptidePrefs" ] || { echo "SKIP: $BUILD/RiptidePrefs not built"; exit 0; }
[ -x "$VBCC/bin/vc" ] && [ -x "$VASM" ] || { echo "SKIP: vbcc or vasm not found"; exit 0; }
work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/riptide-prefs-mui.XXXXXX")}
mkdir -p "$work"
work=$(cd "$work" && pwd)
dh0=$work/dh0
rm -rf "$dh0"
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out" "$dh0/Libs" "$dh0/MUI" "$dh0/ENV/Riptide" "$dh0/Prefs/Env-Archive/Riptide"
cp "$BUILD/RiptidePrefs" "$dh0/C/"
cp -r "$MUI_DIR/Libs/." "$dh0/Libs/"
[ -d "$MUI_DIR/Images" ] && cp -r "$MUI_DIR/Images" "$dh0/MUI/"

# test helpers: the AROS ROM has no C:Assign and no Wait
cat > "$work/tassign.c" <<'EOF'
/* tassign NAME PATH: AssignLock() */
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
cat > "$work/tpubscreen.c" <<'EOF'
/* tpubscreen NAME WIDTH HEIGHT DEPTH MODEID COMMAND: open a public screen,
 * run COMMAND, close the screen again */
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <intuition/screens.h>
struct IntuitionBase *IntuitionBase;
int main(int argc, char **argv)
{
	struct Screen *s;
	LONG rc;
	if (argc < 7 || !(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 39)))
		return 20;
	s = OpenScreenTags(NULL, SA_PubName, (ULONG)argv[1], SA_Title, (ULONG)argv[1], SA_Width, atol(argv[2]),
	                   SA_Height, atol(argv[3]), SA_Depth, atol(argv[4]), SA_DisplayID, strtoul(argv[5], NULL, 0),
	                   SA_LikeWorkbench, TRUE, SA_SharePens, TRUE, SA_Type, PUBLICSCREEN, TAG_DONE);
	if (!s)
		return 20;
	PubScreenStatus(s, 0);
	rc = SystemTags((STRPTR)argv[6], TAG_DONE);
	while (!CloseScreen(s))
		Delay(10);
	return (int)rc;
}
EOF
for h in tassign tpubscreen; do
	PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O2 -I"$NDK/Include_H" -o "$dh0/C/$h" "$work/$h.c" -lamiga ||
		{ echo "FAIL: cannot build $h"; exit 1; }
done
# stand-ins for the libraries the AROS ROM lacks (locale's GetCatalogStr()
# returns the default string it is given)
mklib() {
	n=$1
	shift
	python3 "$here/mkstublib.py" "$n" "$@" > "$work/$n.s" &&
		"$VASM" -quiet -m68020 -Fhunkexe -nosym -o "$dh0/Libs/$n" "$work/$n.s" ||
		{ echo "FAIL: cannot build the $n stub"; exit 1; }
}
mklib locale.library 40 60 '72=move.l a1,d0'
mklib iffparse.library 40 50
mklib diskfont.library 40 20
mklib commodities.library 40 40
mklib datatypes.library 40 50
mklib asl.library 40 20

cat > "$work/orig.conf" <<'EOF'
# Riptide configuration (RiptidePrefs test)

interface eth0 device=uaenet.device unit=0 mtu=1500 tcpwindow=1440
dhcp eth0
hostname amy    # the old name
# keep this comment
compat aliases on
sysctl net.inet.tcp.delack_ms=40
EOF
reset_conf() {
	cp "$work/orig.conf" "$dh0/Prefs/Env-Archive/Riptide/riptide.conf"
	cp "$work/orig.conf" "$dh0/ENV/Riptide/riptide.conf"
	rm -f "$dh0/Prefs/Env-Archive/Riptide/riptide.conf.bak"
}
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
CFG

xpid=
upid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb "$XDISPLAY" -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=$XDISPLAY
	export DISPLAY
	sleep 1
fi
cleanup_uae() {
	[ -n "$upid" ] && kill "$upid" 2>/dev/null && wait "$upid" 2>/dev/null
	upid=
}
cleanup() {
	cleanup_uae
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
# boot with the given Startup-Sequence lines (after the assigns)
boot() {
	{
		echo "FailAt 30"
		echo "DH0:C/tassign ENVARC DH0:Prefs/Env-Archive"
		echo "DH0:C/tassign ENV DH0:ENV"
		echo "DH0:C/tassign MUI DH0:MUI"
		cat
	} > "$dh0/S/Startup-Sequence"
	SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >>"$work/fs-uae.log" 2>&1 &
	upid=$!
	sleep 10
}
# wait for out/NAME, the mark a finished RiptidePrefs leaves
waitfor() {
	t=0
	while [ $t -lt 40 ] && [ ! -s "$dh0/out/$1" ]; do sleep 1; t=$((t + 1)); done
	[ -s "$dh0/out/$1" ] || { fail "RiptidePrefs did not finish ($1)"; return 1; }
	grep -q '^rc 0' "$dh0/out/$1" || fail "RiptidePrefs ($1): $(cat "$dh0/out/$1")"
	# the next RiptidePrefs takes a moment to open: an empty screen is
	# "stable" too, so give it time before the next settle
	sleep 4
}
# STAGES="1 2 3": the boots to run (default all)
want() { case " ${STAGES:-1 2 3} " in *" $1 "*) return 0;; esac; return 1; }
# the pages' names, as the page list shows them
pages="interface address extra vlans names time tcp connecting ports keepalive background memory he qos qosrules other"

# ---- 1: 640x480, 256 colours, light: the edits of run_fsuae_prefs.sh ----
if want 1; then
reset_conf
boot <<'EOF'
DH0:C/tpubscreen RPTEST 640 480 8 0x8004 "DH0:C/RiptidePrefs PUBSCREEN=RPTEST DEBUG=DH0:out/log1"
Echo "rc $RC" >DH0:out/zz_1
EOF
settle a01-interface
# IPv4 off (IPv6 stays on)
key 4
settle a01b-interface-ipv4-off
# Address: switch to static, Save with no address: a requester, back to DHCP
key p
settle a02-address
key m
settle a03-address-static
key s
settle a03b-invalid
key Return
settle a03c-dismissed
key Return m
# Extra addresses: a bad one (requester), then 10.0.1.2/24
key p
settle a03d-extra
key l
typ '10.0.1.300'
key Return d
settle a03e-extra-invalid
key Return
settle a03f-extra-dismissed
key Return l
key Right Right Right Right Right Right Right Right Right Right
key BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace BackSpace
typ '10.0.1.2/24'
key Return d
settle a03g-extra-added
# VLANs: one with DHCP and priority 4
key p
settle a03h-vlans
key l
typ '10 dhcp pcp=4'
key Return d
settle a03i-vlans-added
# Names: replace the host name, add a domain and a name server
key p
settle a04-names
key h Right Right Right BackSpace BackSpace BackSpace
typ zeus
key Return
key d
typ example.org
key Return
key 1
typ 192.168.1.1
key Return
settle a05-names-edited
# Time: NTP on with a server, mDNS on
key p
settle a06-time
key n
settle a07-time-ntp
key v
typ pool.ntp.org
key Return
key m
settle a08-time-edited
key p
settle a09-tcp
# Connecting: Fast Open on
key p
settle a09a-connecting
key f
settle a09b-connecting-edited
# Ports: connect timeout 15 s
key p
settle a09c-ports
key m
typ 15
key Return
settle a09c-ports-edited
# Keepalive: keepalive for all after 120 s
key p
settle a09d-keepalive
key k
typ 120
key Return
settle a09e-keepalive-edited
# Background: LEDBAT by default, background programs
key p
settle a09f-background
key t t t
settle a09g-background-ledbat
key r
typ 'AmiFTP,wget'
key Return
settle a09g-background-edited
key p
settle a09h-memory
key p
settle a09i-happyeyeballs
# QoS: bands on
key p
settle a09j-qos
key q
settle a09k-qos-edited
# QoS rules: add one
key p
settle a09l-qosrules
key l
typ 'port=22,23 priority=interactive'
key Return d
settle a09m-qosrules-added
key p
settle a10-other
# Other lines: add one
key l
typ 'compat bsdsocket off'
key Return d
settle a11-other-added
key s
waitfor zz_1
$ptr shot "$work/a12-after.png"
cleanup_uae

conf=$dh0/Prefs/Env-Archive/Riptide/riptide.conf
cat > "$work/expect.conf" <<'EOF'
# Riptide configuration (RiptidePrefs test)

interface eth0 device=uaenet.device unit=0 mtu=1500 tcpwindow=1440
dhcp eth0
ifconfig eth0 alias 10.0.1.2/24
interface eth0.10 vlan=10 parent=eth0 dhcp pcp=4
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
grep -q '^window open' "$dh0/out/log1" 2>/dev/null || fail "the MUI editor did not open (log: $(cat "$dh0/out/log1" 2>/dev/null))"
fi

# ---- 2: 640x512, 8 colours, dark: every page, then Cancel ----
if want 2; then
reset_conf
boot <<'EOF'
DH0:C/tpubscreen RPTEST 640 512 3 0x8004 "DH0:C/RiptidePrefs PUBSCREEN=RPTEST THEME=DARK"
Echo "rc $RC" >DH0:out/zz_2
EOF
n=1
for p in $pages; do
	settle "$(printf 'b%02d-dark-%s' $n "$p")"
	key p
	n=$((n + 1))
done
key Escape
waitfor zz_2
cleanup_uae
cmp -s "$work/orig.conf" "$dh0/ENV/Riptide/riptide.conf" || fail "Cancel changed ENV:Riptide/riptide.conf"
fi

# ---- 3: the 640x256 Workbench (compact): bubble texts, menu commands ----
if want 3; then
reset_conf
boot <<'EOF'
DH0:C/RiptidePrefs DEBUG=DH0:out/log3
Echo "rc $RC" >DH0:out/zz_3a
DH0:C/RiptidePrefs CMD=105
Echo "rc $RC" >DH0:out/zz_3b
DH0:C/RiptidePrefs CMD=300
Echo "rc $RC" >DH0:out/zz_3c
DH0:C/RiptidePrefs PAGE=Names
Echo "rc $RC" >DH0:out/zz_3d
DH0:C/RiptidePrefs CMD=200
Echo "rc $RC" >DH0:out/zz_3e
DH0:C/RiptidePrefs GADTOOLS
Echo "rc $RC" >DH0:out/zz_3f
EOF
settle c01-workbench-compact
# the pointer over a page lights it
$ptr move 300 246 >/dev/null
sleep 2
$ptr shot "$work/c02-hover.png"
key Escape
waitfor zz_3a
settle c03-about
key Return
settle c04-about-closed
key Escape
waitfor zz_3b
settle c05-menu-dark
key Escape
waitfor zz_3c
settle c06-saved-dark-names
key Escape
waitfor zz_3d
settle c07-defaults
key u
waitfor zz_3e
settle c08-gadtools
key Escape c
waitfor zz_3f
cleanup_uae
grep -q '^dark=1' "$dh0/ENV/Riptide/RiptidePrefs.prefs" 2>/dev/null || fail "Dark Theme was not saved"
grep -q '^hostname' "$dh0/ENV/Riptide/riptide.conf" && fail "Reset to Defaults + Use kept the host name"
grep -q '^compat aliases on' "$dh0/ENV/Riptide/riptide.conf" || fail "Reset to Defaults lost an unknown line"
grep -q 'screen 640x256' "$dh0/out/log3" 2>/dev/null || fail "no screen line in the log"
# help bubbles: MUI 3.8 shows none here (without iffparse.library it cannot
# read its settings, and its default leaves bubbles off), so check the
# texts the editor gives MUI instead
grep -q 'bubble field: .*Interface name' "$dh0/out/log3" 2>/dev/null || fail "no bubble text on the first setting"
grep -q 'bubble page: .*Interface' "$dh0/out/log3" 2>/dev/null || fail "no bubble text from the page list"
fi

cleanup
if [ $fails -eq 0 ]; then
	echo "ok   RiptidePrefs MUI editor on FS-UAE ($BUILD): keyboard edits as the GadTools test, both themes, 3 screens, bubble texts, menus, GADTOOLS"
	echo "     screenshots: $work/*.png"
	exit 0
fi
echo "$fails failures; files in $work"
exit 1
