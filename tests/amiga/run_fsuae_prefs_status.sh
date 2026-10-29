#!/bin/sh
# Riptide - RiptidePrefs' Status page on FS-UAE (AROS Kickstart, MUI 3.8)
# with the real stack and traffic: Riptide's eth0 is uaenet.device unit 2
# bridged onto a veth pair (FS-UAE built with tools/fs-uae's patch); the
# host end runs tests/amiga/status_peer.py (a steady download, a slow
# upload, timed fast downloads) and the Amiga a listener. Screenshots:
# every tab on a 640x480 256-colour screen, the dark theme on 640x512 with
# 8 colours, the compact layout on the 640x256 Workbench, the GadTools
# editor's Status page, and both editors with Riptide not running. The
# DEBUG log must show the page reading the stack with traffic. Also times
# the same download with RiptidePrefs closed and with its Status page open
# (what the page costs a 68020).
#
#   FSUAE_PCAP=path/to/fs-uae MUI_DIR=/path/to/MUI \
#     tests/amiga/run_fsuae_prefs_status.sh [BUILD] [OUTDIR]
#
# ST_IF, ST_PEER, ST_NET (first three octets) choose other veth names and
# subnet; DL_SIZE the timed download (bytes); ST_VLANS the VLAN interfaces
# besides eth0 (default 2); ST_UPDATE the Status page's
# Update setting for the run (0, 1, 2 for every 1, 2, 5 s; default 0). Needs root, fs-uae, Xvfb (or
# DISPLAY), xdotool, ImageMagick, Python PIL, vbcc + NDK and vasm.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
here=$(cd "$(dirname "$0")" && pwd)
ptr="python3 $here/fsuae_pointer.py"
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
VASM=${VASM:-$VBCC/bin/vasmm68k_mot}
XDISPLAY=${XDISPLAY:-:171}
ST_IF=${ST_IF:-rts0}
ST_PEER=${ST_PEER:-rts1}
ST_NET=${ST_NET:-192.168.171}
DL_SIZE=${DL_SIZE:-4194304}
# the timed screen's depth in bitplanes (8: 256 colours, the slowest to draw on)
ST_DEPTH=${ST_DEPTH:-8}
[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet capture)"; exit 0; }
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool not installed"; exit 0; }
[ -n "${MUI_DIR:-}" ] && [ -f "$MUI_DIR/Libs/muimaster.library" ] ||
	{ echo "SKIP: set MUI_DIR to the MUI drawer of MUI 3.8 (mui38usr.lha)"; exit 0; }
for f in Riptide rtcheck nc RiptidePrefs; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
[ -x "$VBCC/bin/vc" ] && [ -x "$VASM" ] || { echo "SKIP: vbcc or vasm not found"; exit 0; }
work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/riptide-prefs-status.XXXXXX")}
mkdir -p "$work"
work=$(cd "$work" && pwd)
dh0=$work/dh0
rm -rf "$dh0"
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out" "$dh0/Libs" "$dh0/MUI" "$dh0/ENV/Riptide" "$dh0/Prefs/Env-Archive/Riptide"
for f in Riptide rtcheck nc RiptidePrefs; do cp "$BUILD/$f" "$dh0/C/"; done
cp -r "$MUI_DIR/Libs/." "$dh0/Libs/"
[ -d "$MUI_DIR/Images" ] && cp -r "$MUI_DIR/Images" "$dh0/MUI/"
head -c 3000000 /dev/urandom > "$dh0/big"

# helpers: assigns, a public screen, and waiting (the AROS ROM has no
# C:Assign and no Wait)
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
/* tpubscreen NAME WIDTH HEIGHT DEPTH MODEID COMMAND */
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
cat > "$work/twait.c" <<'EOF'
/* twait SECONDS, or twait FILE: until the file exists (at most 10 min) */
#include <stdlib.h>
#include <proto/dos.h>
int main(int argc, char **argv)
{
	int i;
	BPTR l;
	if (argc < 2)
		return 20;
	if (argv[1][0] >= '0' && argv[1][0] <= '9') {
		Delay(atol(argv[1]) * 50);
		return 0;
	}
	for (i = 0; i < 1200; i++) {
		if ((l = Lock((STRPTR)argv[1], ACCESS_READ)) != 0) {
			UnLock(l);
			return 0;
		}
		Delay(25);
	}
	return 5;
}
EOF
for h in tassign tpubscreen twait; do
	PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O2 -I"$NDK/Include_H" -o "$dh0/C/$h" "$work/$h.c" -lamiga ||
		{ echo "FAIL: cannot build $h"; exit 1; }
done
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

cat > "$dh0/S/riptide.conf" <<CONF
hostname status-test
interface eth0 device=uaenet.device unit=2
ifconfig eth0 $ST_NET.2/24 up
route add default $ST_NET.1
nameserver $ST_NET.1
CONF
# VLANs, so the Interfaces tab has more interfaces than fit (ST_VLANS of them)
v=1
while [ $v -le ${ST_VLANS:-2} ]; do
	echo "interface eth0.$((v * 10)) vlan=$((v * 10)) parent=eth0 address=10.$((v * 10)).0.2/24" >>"$dh0/S/riptide.conf"
	v=$((v + 1))
done
cp "$dh0/S/riptide.conf" "$dh0/Prefs/Env-Archive/Riptide/riptide.conf"
# the Status page's Update setting (0 every second, 1 every 2 s, 2 every 5 s)
if [ -n "${ST_UPDATE:-}" ]; then
	printf 'status_update=%s\n' "$ST_UPDATE" >"$dh0/ENV/Riptide/RiptidePrefs.prefs"
	cp "$dh0/ENV/Riptide/RiptidePrefs.prefs" "$dh0/Prefs/Env-Archive/Riptide/"
fi
cp "$dh0/S/riptide.conf" "$dh0/ENV/Riptide/riptide.conf"
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
zoom = full
uae_sana2 = 1
uaenet_pcap = $ST_IF
CFG

ip link add $ST_IF type veth peer name $ST_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
ip link set $ST_IF up
ip link set $ST_PEER up
ip addr add $ST_NET.1/24 dev $ST_PEER
if command -v ethtool >/dev/null 2>&1; then
	for d in $ST_IF $ST_PEER; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi
xpid=
upid=
ppid=
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
	[ -n "$ppid" ] && kill "$ppid" 2>/dev/null
	[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
	xpid=
	ip link del $ST_IF 2>/dev/null
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
# the page changes every second: no "stable" screen, a pause instead
shot() {
	sleep "${2:-3}"
	$ptr shot "$work/$1.png"
}
waitfile() {
	t=0
	while [ $t -lt "${2:-300}" ] && [ ! -s "$dh0/out/$1" ]; do sleep 1; t=$((t + 1)); done
	[ -s "$dh0/out/$1" ] || { fail "no out/$1"; return 1; }
}
boot() {
	{
		echo "FailAt 30"
		echo "DH0:C/tassign ENVARC DH0:Prefs/Env-Archive"
		echo "DH0:C/tassign ENV DH0:ENV"
		echo "DH0:C/tassign MUI DH0:MUI"
		cat
	} > "$dh0/S/Startup-Sequence"
	SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >>"$work/fs-uae.log" 2>&1 &
	upid=$!
}
want() { case " ${STAGES:-1 2} " in *" $1 "*) return 0;; esac; return 1; }

# ---- 1: Riptide running, traffic ----
if want 1; then
python3 -I "$here/status_peer.py" $ST_NET.1 "$DL_SIZE" 900 "$work/peer.log" >"$work/peer.err" 2>&1 &
ppid=$!
cat > "$dh0/S/prefs0" <<EOF
DH0:C/tpubscreen RPTEST 640 480 $ST_DEPTH 0x8004 "DH0:C/RiptidePrefs PUBSCREEN=RPTEST PAGE=Interface DEBUG=DH0:out/log0"
Echo "rc \$RC" >DH0:out/zz_0
EOF
cat > "$dh0/S/prefs1" <<EOF
DH0:C/tpubscreen RPTEST 640 480 $ST_DEPTH 0x8004 "DH0:C/RiptidePrefs PUBSCREEN=RPTEST DEBUG=DH0:out/log1"
Echo "rc \$RC" >DH0:out/zz_1
EOF
# the connections the Connections tab shows, each in its own script (one
# redirection each: Run's own and nc's would clash): a listener, a steady
# download and a slow upload
cat > "$dh0/S/bg_l" <<EOF
DH0:C/nc -d -l 2121 >NIL:
Echo "rc \$RC" >DH0:out/nc_l
EOF
cat > "$dh0/S/bg_steady" <<EOF
DH0:C/nc -d $ST_NET.1 7779 >NIL:
Echo "rc \$RC" >DH0:out/nc_steady
EOF
cat > "$dh0/S/bg_up" <<EOF
DH0:C/nc $ST_NET.1 7780 >NIL: <DH0:big
Echo "rc \$RC" >DH0:out/nc_up
EOF
# the timed downloads: 0 with nothing open, A with RiptidePrefs open on
# another page, B with its Status page (A and B on the same screen)
boot <<EOF
DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start
DH0:C/twait 3
DH0:C/nc -d $ST_NET.1 7777 >NIL:
Echo 0 >DH0:out/dl_0
Run >NIL: Execute DH0:S/prefs0
DH0:C/twait 15
DH0:C/nc -d $ST_NET.1 7777 >NIL:
Echo a >DH0:out/dl_a
DH0:C/twait DH0:out/zz_0
Run >NIL: Execute DH0:S/prefs1
DH0:C/twait 15
DH0:C/nc -d $ST_NET.1 7777 >NIL:
Echo b >DH0:out/dl_b
Run >NIL: Execute DH0:S/bg_l
Run >NIL: Execute DH0:S/bg_steady
Run >NIL: Execute DH0:S/bg_up
Echo ready >DH0:out/ready
DH0:C/twait DH0:out/zz_1
DH0:C/tpubscreen RPTEST 640 512 3 0x8004 "DH0:C/RiptidePrefs PUBSCREEN=RPTEST THEME=DARK"
Echo "rc \$RC" >DH0:out/zz_2
DH0:C/RiptidePrefs DEBUG=DH0:out/log3
Echo "rc \$RC" >DH0:out/zz_3
DH0:C/RiptidePrefs GADTOOLS PAGE=Status
Echo "rc \$RC" >DH0:out/zz_4
EOF
# A done: close the first editor so the second (the Status page) opens
waitfile dl_a 600 && shot s00-other-page 1 && key Escape
waitfile ready 600 && {
	shot s01-interfaces 14
	key 2
	shot s02-connections 4
	key 3
	shot s03-names
	key 4
	shot s04-counters
	key 1
	shot s05-interfaces-later 6
	key Escape
}
waitfile zz_1 60
shot s06-dark 14
key 2
shot s07-dark-connections 4
key Escape
waitfile zz_2 60
shot s08-compact 14
# the interfaces that did not fit: a click on the list
$ptr click 600 400 >/dev/null    # the interface list (screenshot pixels)
shot s08b-compact-more-interfaces 4
key 2
shot s09-compact-connections 4
key Escape
waitfile zz_3 60
shot s10-gadtools 8
key Escape
waitfile zz_4 60
cleanup_uae
kill "$ppid" 2>/dev/null
ppid=
grep -q '^window open' "$dh0/out/log1" 2>/dev/null || fail "the MUI editor did not open"
# every window inside its screen
for lg in log0 log1 log3; do
	awk '/^screen [0-9]+x[0-9]+/ { split($2, s, "x") } /^window [0-9]+x[0-9]+ at/ { split($2, w, "x"); split($4, p, ",");
		if (p[1] + w[1] > s[1] || p[2] + w[2] > s[2]) bad = 1 } END { exit bad }' "$dh0/out/$lg" 2>/dev/null ||
		fail "$lg: the window does not fit its screen ($(grep -E '^(screen|window [0-9])' "$dh0/out/$lg" | tr '\n' ' '))"
done
grep -Eq '^status: running, ([4-9]|[1-9][0-9]) interfaces' "$dh0/out/log1" || fail "the Status page did not see the VLANs"
grep -Eq '^status: running, [1-9][0-9]* interfaces, [1-9][0-9]* connections, in [1-9]' "$dh0/out/log1" ||
	fail "the Status page saw no traffic (log1)"
grep -Eq '^status: running, .* connections, in [0-9]+ out [1-9]' "$dh0/out/log1" ||
	fail "the Status page saw nothing sent (log1)"
grep -Eq 'cpu [1-9]' "$dh0/out/log1" || fail "no CPU share in log1"
# the listener, the steady download and the upload at once
grep -Eq '^status: running, [0-9]+ interfaces, ([3-9]|[1-9][0-9]+) connections' "$dh0/out/log1" ||
	fail "the Status page never saw the three background connections (log1)"
grep -q '^status: running' "$dh0/out/log3" 2>/dev/null || fail "the compact Status page did not read the stack"
n=$(grep -c "^dl [0-9.]* $DL_SIZE\$" "$work/peer.log" 2>/dev/null)
[ "${n:-0}" -ge 3 ] || fail "the timed downloads did not complete ($(cat "$work/peer.log" 2>/dev/null))"
for z in zz_0 zz_1 zz_2 zz_3 zz_4; do grep -q '^rc 0' "$dh0/out/$z" 2>/dev/null || fail "$z: $(cat "$dh0/out/$z" 2>/dev/null)"; done
fi

# ---- 2: Riptide not running ----
if want 2; then
boot <<'EOF'
DH0:C/tpubscreen RPTEST 640 480 8 0x8004 "DH0:C/RiptidePrefs PUBSCREEN=RPTEST DEBUG=DH0:out/log5"
Echo "rc $RC" >DH0:out/zz_5
DH0:C/RiptidePrefs GADTOOLS PAGE=Status
Echo "rc $RC" >DH0:out/zz_6
EOF
sleep 15
shot s11-not-running 6
key 2
shot s12-not-running-connections 3
key Escape
waitfile zz_5 60
shot s13-gadtools-not-running 8
key Escape
waitfile zz_6 60
cleanup_uae
grep -q '^status: stopped' "$dh0/out/log5" 2>/dev/null || fail "no 'stopped' read without Riptide"
fi

cleanup
if [ -f "$work/peer.log" ]; then
	# the timed downloads: nothing open; RiptidePrefs on another page; its Status page
	awk -v s="$DL_SIZE" 'BEGIN { split("no RiptidePrefs,another page,Status page", w, ",") }
		/^dl / && $3 == s { n++; printf "     download %d (%s): %.1f KB/s (%s s)\n", n, w[n], s / 1024 / $2, $2 }' \
		"$work/peer.log"
fi
if [ $fails -eq 0 ]; then
	echo "ok   RiptidePrefs Status page on FS-UAE ($BUILD): live tables with traffic, every tab, dark, compact, GadTools, not running"
	echo "     screenshots: $work/*.png"
	exit 0
fi
echo "$fails failures; files in $work"
exit 1
