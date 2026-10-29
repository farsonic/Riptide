#!/bin/sh
# Riptide - RiptidePrefs started from its Workbench icon on a real Kickstart
# 3.1.4 with Workbench 3.1.4 and MUI 3.8, Riptide running; after each round
# Workbench must still work.
#
#   MUI_DIR=/path/to/MUI tests/amiga/run_fsuae_ks314_wb.sh [PKG] [OUTDIR]
#
# A report said: "I opened and then saved RiptidePrefs and now I can't
# select any icons". Rounds, each started by double-clicking an icon on the
# Workbench window (a real Workbench start):
#   1 MUI editor: host name changed, Save, "Apply" to the running Riptide
#   2 the same, answered "Later"
#   3 a change, then Esc (Cancel)
#   4 opened twice: the second copy must hand over to the first and quit
#   5 the Status page left open for a minute (updating every second), Save
#   6 the GadTools editor (an icon with the GADTOOLS tool type): Save, Apply
# After each round: tests/amiga/wbprobe.c's snapshot must show no
# RiptidePrefs left and the Workbench screen's layers free, and Workbench
# must select an icon, drag one, open a drawer window and close it, and show
# its menus. Screenshots of every step stay in OUTDIR.
#
# PKG is a built package drawer (default build/package/Riptide, made by
# "make -f Makefile.amiga TOOLCHAIN=gcc package"). Needs KS314_ROM and
# KS314_SYS (tests/amiga/ks314.sh), MUI_DIR, fs-uae, Xvfb (or DISPLAY),
# xdotool, ImageMagick, Python PIL and vbcc + NDK (for wbprobe).
# SPDX-License-Identifier: GPL-2.0-only
set -u
here=$(cd "$(dirname "$0")" && pwd)
KS314_REPO=$(cd "$here/../.." && pwd)
. "$here/ks314.sh"
ks314_init
PKG=${1:-$KS314_REPO/build/package/Riptide}
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool not installed"; exit 0; }
[ -n "${MUI_DIR:-}" ] && [ -f "$MUI_DIR/Libs/muimaster.library" ] ||
	{ echo "SKIP: set MUI_DIR to the MUI drawer of MUI 3.8 (mui38usr.lha)"; exit 0; }
[ -f "$PKG/RiptidePrefs" ] || { echo "SKIP: no package in $PKG (make -f Makefile.amiga TOOLCHAIN=gcc package)"; exit 0; }
[ -x "$VBCC/bin/vc" ] || { echo "SKIP: vbcc not found"; exit 0; }
ptr="python3 $here/fsuae_pointer.py"
work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/riptide-ks314-wb.XXXXXX")}
mkdir -p "$work"
work=$(cd "$work" && pwd)
sys=$work/sys
out=$sys/out

fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }

ks314_system "$sys" "$PKG" || { echo "FAIL: cannot prepare the system"; exit 1; }
PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O2 -I"$NDK/Include_H" -o "$sys/C/wbprobe" \
	"$here/wbprobe.c" -lamiga >"$work/vc.log" 2>&1 || { echo "FAIL: cannot build wbprobe"; exit 1; }
# the MUI editor's icon and a second one for the GadTools editor, both left
# out on the Workbench window (positions in the window, 1:1 across, 2:1 down
# on the 640x256 screen as captured)
cp "$sys/Prefs/RiptidePrefs" "$sys/Prefs/RiptidePrefsGT"
ks314_leftout "$sys" Prefs/RiptidePrefs 300 20 DEBUG=SYS:out/rp.log
ks314_leftout "$sys" Prefs/RiptidePrefsGT 300 80 GADTOOLS DEBUG=SYS:out/gt.log
mkdir -p "$out"
ks314_hook "$sys" <<'EOF'
Run >NIL: C:wbprobe SYS:out/probe 2
Wait 6
C:Riptide STATUS >SYS:out/status
Echo up >SYS:out/booted
EOF
ks314_config "$work/test.fs-uae" "$sys"

xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :${XDISP:-201} -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:${XDISP:-201}
	export DISPLAY
	sleep 1
fi
SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
cleanup() {
	kill "$upid" 2>/dev/null && wait "$upid" 2>/dev/null
	[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
}
trap cleanup EXIT

# screen places (1024x768 capture of the 640x256 Workbench screen)
MUI_ICON="520 240"
GT_ICON="520 356"
SYS_ICON="235 270"
SAVE="277 569"            # MUI editor buttons (compact layout)
MSG_YES="520 421"         # "Apply" / "Later" in the MUI editor's question
MSG_NO="592 421"
MSG_OK="612 413"          # OK of its message

waitlog() {
	# waitlog FILE TEXT SECS
	t=0
	while [ $t -lt "$3" ]; do
		[ -f "$1" ] && tr -d '\r' <"$1" | grep -qF -- "$2" && return 0
		sleep 1
		t=$((t + 1))
	done
	return 1
}
shot() { $ptr shot "$work/$1.png"; }
key() { xdotool key "$1"; sleep "${2:-1}"; }
# region DIFF A B X0 Y0 X1 Y1: 0 when the two screenshots differ there
differ() {
	python3 - "$work/$1.png" "$work/$2.png" "$3" "$4" "$5" "$6" <<'EOF'
import sys
from PIL import Image, ImageChops
a, b = (Image.open(p).convert("RGB") for p in sys.argv[1:3])
box = tuple(int(v) for v in sys.argv[3:7])
d = ImageChops.difference(a.crop(box), b.crop(box)).getbbox()
sys.exit(0 if d else 1)
EOF
}

waitlog "$out/booted" up 240 || { fail "Workbench did not come up"; echo "$fails failures; files in $work"; exit 1; }
$ptr stable 3 60 >/dev/null
shot 00-workbench

# wb_ok ROUND: Workbench still works and RiptidePrefs is gone
wb_ok() {
	r=$1
	sleep 5         # two probe snapshots
	cp "$out/probe" "$work/$r-probe.txt" 2>/dev/null
	grep -q 'riptideprefs port no, tasks 0' "$work/$r-probe.txt" ||
		fail "$r: RiptidePrefs still there: $(grep riptideprefs "$work/$r-probe.txt")"
	grep -q '^layers free' "$work/$r-probe.txt" || fail "$r: Workbench screen layers: $(grep '^layers' "$work/$r-probe.txt")"
	$ptr move 700 520 >/dev/null
	sleep 1
	shot "$r-wb0"
	# select the System icon: it must be drawn selected
	$ptr click $SYS_ICON >/dev/null
	sleep 1.5
	shot "$r-wb1"
	differ "$r-wb0" "$r-wb1" 210 245 260 295 || fail "$r: the System icon did not highlight"
	# drag the MUI editor's icon away and back
	$ptr drag $MUI_ICON 700 240 >/dev/null
	sleep 1.5
	shot "$r-wb2"
	differ "$r-wb1" "$r-wb2" 650 210 760 290 || fail "$r: the RiptidePrefs icon did not move"
	$ptr drag 700 240 $MUI_ICON >/dev/null
	sleep 1.5
	# open the System drawer, then close its window
	$ptr click $SYS_ICON 2 >/dev/null
	sleep 4
	shot "$r-wb3"
	differ "$r-wb1" "$r-wb3" 300 185 620 400 || fail "$r: the System drawer did not open"
	$ptr click 248 191 >/dev/null   # its close gadget
	sleep 2
	shot "$r-wb4"
	differ "$r-wb3" "$r-wb4" 300 185 620 400 || fail "$r: the System drawer did not close"
	# the menus: hold the right button over the screen title
	$ptr move 400 137 >/dev/null
	xdotool mousedown 3
	sleep 1.2
	shot "$r-wb5"
	xdotool mouseup 3
	sleep 1
	differ "$r-wb4" "$r-wb5" 192 128 830 148 || fail "$r: no Workbench menus"
}

# open_editor "X Y" LOG ROUND [TEXT]: double-click an icon, wait for the
# editor's window (TEXT in its DEBUG log: "window open" for MUI)
open_editor() {
	rm -f "$out/$2"
	$ptr click $1 2 >/dev/null
	waitlog "$out/$2" "${4:-window open}" 40 || { fail "$3: the editor did not open"; return 1; }
	$ptr stable 2 30 >/dev/null
	return 0
}

# names_host NAME: MUI editor, Names page, host name NAME (from the Status page)
mui_host() {
	n=0
	while [ $n -lt 5 ]; do key p; n=$((n + 1)); done
	sleep 1
	key h
	n=0
	while [ $n -lt 8 ]; do xdotool key BackSpace; n=$((n + 1)); done
	xdotool type --delay 150 "$1"
	sleep 1
}

conf=$sys/Prefs/Env-Archive/Riptide/riptide.conf

# ---- 1: MUI, Save, Apply ----------------------------------------------------
r=r1-mui-apply
if open_editor "$MUI_ICON" rp.log $r; then
	shot $r-a-open
	mui_host zeus
	shot $r-b-edited
	$ptr click $SAVE >/dev/null
	waitlog "$out/rp.log" "Apply the changes" 20 || fail "$r: no Apply question"
	sleep 1
	shot $r-c-question
	$ptr click $MSG_YES >/dev/null
	waitlog "$out/rp.log" "Applied" 30 || fail "$r: nothing applied"
	sleep 1
	shot $r-d-applied
	$ptr click $MSG_OK >/dev/null
	waitlog "$out/rp.log" "exit 0" 20 || fail "$r: the editor did not exit"
	cp "$out/rp.log" "$work/$r.log"
	grep -q '^hostname zeus$' "$conf" || fail "$r: riptide.conf has no hostname zeus"
fi
wb_ok $r

# ---- 2: MUI, Save, Later -----------------------------------------------------
r=r2-mui-later
if open_editor "$MUI_ICON" rp.log $r; then
	mui_host mars
	$ptr click $SAVE >/dev/null
	waitlog "$out/rp.log" "Apply the changes" 20 || fail "$r: no Apply question"
	sleep 1
	shot $r-c-question
	$ptr click $MSG_NO >/dev/null
	waitlog "$out/rp.log" "exit 0" 20 || fail "$r: the editor did not exit"
	cp "$out/rp.log" "$work/$r.log"
	grep -q '^hostname mars$' "$conf" || fail "$r: riptide.conf has no hostname mars"
fi
wb_ok $r

# ---- 3: MUI, a change, Esc -----------------------------------------------------
r=r3-mui-cancel
if open_editor "$MUI_ICON" rp.log $r; then
	mui_host venus
	key Return
	key Escape 2
	waitlog "$out/rp.log" "exit 0" 20 || fail "$r: the editor did not exit"
	cp "$out/rp.log" "$work/$r.log"
	grep -q '^hostname mars$' "$conf" || fail "$r: Cancel changed riptide.conf"
fi
wb_ok $r

# ---- 4: opened twice ------------------------------------------------------------
r=r4-twice
if open_editor "$MUI_ICON" rp.log $r; then
	# cover it with the System drawer, then start it again
	$ptr click $SYS_ICON 2 >/dev/null
	sleep 4
	shot $r-a-covered
	$ptr click $MUI_ICON 2 >/dev/null
	sleep 8
	shot $r-b-again
	cp "$out/probe" "$work/$r-during.txt"
	grep -q 'tasks 1$' "$work/$r-during.txt" || fail "$r: $(grep riptideprefs "$work/$r-during.txt") while one should run"
	differ "$r-a-covered" "$r-b-again" 300 185 620 400 || fail "$r: the open editor did not come to the front"
	key Escape 3
	$ptr click 248 191 >/dev/null   # close the System drawer
	sleep 2
fi
wb_ok $r

# ---- 5: Status page open for a minute, then Save -----------------------------
r=r5-status
if open_editor "$MUI_ICON" rp.log $r; then
	sleep 60
	shot $r-a-status
	grep -c '^status: running' "$out/rp.log" >"$work/$r-reads.txt"
	$ptr click $SAVE >/dev/null
	waitlog "$out/rp.log" "exit 0" 20 || fail "$r: the editor did not exit"
	cp "$out/rp.log" "$work/$r.log"
fi
wb_ok $r

# ---- 6: GadTools editor, Save, Apply ---------------------------------------------
r=r6-gadtools
if open_editor "$GT_ICON" gt.log $r "GadTools editor"; then
	shot $r-a-open
	n=0
	while [ $n -lt 5 ]; do key p; n=$((n + 1)); done
	key h
	n=0
	while [ $n -lt 8 ]; do xdotool key BackSpace; n=$((n + 1)); done
	xdotool type --delay 150 jupiter
	key Return
	shot $r-b-edited
	key s 3
	shot $r-c-question
	if waitlog "$out/gt.log" "Apply the changes" 10; then
		# the question is an EasyRequest: Return answers its first button
		key Return 3
		shot $r-d-answered
		waitlog "$out/gt.log" "Applied" 20 && key Return 2
	fi
	waitlog "$out/gt.log" "exit 0" 20 || fail "$r: the editor did not exit"
	cp "$out/gt.log" "$work/$r.log"
	grep -q '^hostname jupiter$' "$conf" || fail "$r: riptide.conf has no hostname jupiter"
fi
wb_ok $r

if [ $fails -eq 0 ]; then
	echo "ok   RiptidePrefs from Workbench icons on Kickstart 3.1.4 + MUI 3.8: Save/Apply, Later, Cancel, twice, Status page, GadTools; Workbench works after each"
	echo "     screenshots: $work/*.png"
	exit 0
fi
echo "$fails failures; files in $work"
exit 1
