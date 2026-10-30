#!/bin/sh
# Riptide - RiptidePrefs on FS-UAE (AROS Kickstart) with MUI 3.8: one
# editor at a time, and starting from Workbench.
#
# 1. One at a time: with an editor open (MUI, then GadTools) and a window
#    over it, RiptidePrefs started again (as the MUI and the GadTools
#    editor) ends at once with rc 0, opens no window, and the open editor
#    comes to the front; Save still works. "RiptidePrefs USE" (no window)
#    works while an editor is open. A copy started as from Workbench while
#    one is open ends too, and its Workbench message is replied.
# 2. Workbench start: a launcher (twb, built below) starts
#    RiptidePrefs the way Workbench does (a process without a CLI and a
#    WBStartup message) and waits for the reply. With a stand-in for
#    Riptide's RIPTIDE port (tfakeport: answers every command and logs it):
#    Save without changes, Save with changes answering Apply, Save with
#    changes answering Later, and Cancel. Each time the launcher must get
#    its reply and the process must be gone; afterwards a command still
#    runs and a window still opens.
#
#   MUI_DIR=/path/to/MUI tests/amiga/run_fsuae_prefs_single.sh [BUILD] [OUTDIR]
#
# Needs fs-uae, Xvfb (or DISPLAY), xdotool, ImageMagick, Python PIL, vbcc +
# NDK (helpers) and vasm; MUI_DIR as for run_fsuae_prefs_mui.sh.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
here=$(cd "$(dirname "$0")" && pwd)
ptr="python3 $here/fsuae_pointer.py"
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
VASM=${VASM:-$VBCC/bin/vasmm68k_mot}
XDISPLAY=${XDISPLAY:-:117}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
command -v xdotool >/dev/null 2>&1 || { echo "SKIP: xdotool not installed"; exit 0; }
[ -n "${MUI_DIR:-}" ] && [ -f "$MUI_DIR/Libs/muimaster.library" ] ||
	{ echo "SKIP: set MUI_DIR to the MUI drawer of MUI 3.8 (mui38usr.lha)"; exit 0; }
[ -f "$BUILD/RiptidePrefs" ] || { echo "SKIP: $BUILD/RiptidePrefs not built"; exit 0; }
[ -x "$VBCC/bin/vc" ] && [ -x "$VASM" ] || { echo "SKIP: vbcc or vasm not found"; exit 0; }
work=${2:-$(mktemp -d "${TMPDIR:-/tmp}/riptide-prefs-single.XXXXXX")}
mkdir -p "$work"
work=$(cd "$work" && pwd)
dh0=$work/dh0
rm -rf "$dh0"
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out" "$dh0/go" "$dh0/Libs" "$dh0/MUI" "$dh0/ENV/Riptide" \
	"$dh0/Prefs/Env-Archive/Riptide"
cp "$BUILD/RiptidePrefs" "$dh0/C/"
# copies with icons for the tool type starts (the ROM has no Copy)
mkdir -p "$dh0/tt1" "$dh0/tt2"
cp "$BUILD/RiptidePrefs" "$dh0/tt1/"
cp "$BUILD/RiptidePrefs" "$dh0/tt2/"
${PYTHON:-python3} tools/mkicon.py tool "$dh0/tt1/RiptidePrefs.info" --image prefs \
	--tooltype PAGE=Names --tooltype DEBUG=DH0:out/tt1 >/dev/null
${PYTHON:-python3} tools/mkicon.py tool "$dh0/tt2/RiptidePrefs.info" --image prefs \
	--tooltype GADTOOLS --tooltype PAGE=Names --tooltype DEBUG=DH0:out/tt2 >/dev/null
cp -r "$MUI_DIR/Libs/." "$dh0/Libs/"
[ -d "$MUI_DIR/Images" ] && cp -r "$MUI_DIR/Images" "$dh0/MUI/"

# ---- helpers (the AROS ROM has no Assign, Wait or Workbench) ----
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
cat > "$work/tgo.c" <<'EOF'
/* tgo FILE: wait until FILE exists (the host makes it), at most 10 minutes */
#include <proto/dos.h>
int main(int argc, char **argv)
{
	int t;
	for (t = 0; argc > 1 && t < 2400; t++) {
		BPTR l = Lock((STRPTR)argv[1], ACCESS_READ);
		if (l) {
			UnLock(l);
			return 0;
		}
		Delay(12);
	}
	return 20;
}
EOF
cat > "$work/tcover.c" <<'EOF'
/* tcover SECONDS: a window over most of the Workbench screen for a while */
#include <stdlib.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <intuition/intuition.h>
struct IntuitionBase *IntuitionBase;
int main(int argc, char **argv)
{
	struct Window *w;
	if (argc < 2 || !(IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37)))
		return 20;
	w = OpenWindowTags(NULL, WA_Left, 0, WA_Top, 12, WA_Width, 640, WA_Height, 240, WA_Title,
	                   (ULONG)"Cover", WA_DragBar, TRUE, WA_Activate, TRUE, WA_SmartRefresh, TRUE, TAG_DONE);
	if (!w)
		return 20;
	Delay(50 * atol(argv[1]));
	CloseWindow(w);
	CloseLibrary((struct Library *)IntuitionBase);
	return 0;
}
EOF
cat > "$work/tfakeport.c" <<'EOF'
/* tfakeport SECONDS LOG: a stand-in for Riptide's RIPTIDE port: every
 * message is answered with success, and its command line goes to LOG */
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/ports.h>
struct RiptideMsg {
	struct Message msg;
	ULONG cmd;
	LONG result;
	char text[1024];
	STRPTR reply;
	ULONG replysize, replylen, flags;
};
int main(int argc, char **argv)
{
	static char name[] = "RIPTIDE";
	struct MsgPort *p;
	struct RiptideMsg *m;
	BPTR f;
	long t, end;
	if (argc < 3 || !(p = CreateMsgPort()))
		return 20;
	p->mp_Node.ln_Name = name;
	AddPort(p);
	end = atol(argv[1]) * 5;
	for (t = 0; t < end; t++) {
		Delay(10);
		while ((m = (struct RiptideMsg *)GetMsg(p)) != NULL) {
			if ((f = Open((STRPTR)argv[2], MODE_READWRITE)) != 0) {
				Seek(f, 0, OFFSET_END);
				m->text[sizeof(m->text) - 1] = 0;
				Write(f, m->text, (LONG)strlen(m->text));
				Write(f, "\n", 1);
				Close(f);
			}
			m->result = 0;
			m->text[0] = 0;
			if (m->msg.mn_Length >= sizeof(*m) && m->reply && m->replysize)
				m->reply[0] = 0;
			if (m->msg.mn_Length >= sizeof(*m)) {
				m->replylen = 0;
				m->flags = 0;
			}
			ReplyMsg(&m->msg);
		}
	}
	Forbid();
	RemPort(p);
	Permit();
	DeleteMsgPort(p);
	return 0;
}
EOF
cat > "$work/twb.c" <<'EOF'
/* twb PROGRAM SECONDS OUT: start PROGRAM as Workbench does (a process
 * without a CLI, then a WBStartup message to its process port) and wait
 * up to SECONDS for the message to come back; the result goes to OUT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <workbench/startup.h>
static void say(const char *out, const char *s)
{
	BPTR f = Open((STRPTR)out, MODE_NEWFILE);
	if (f) {
		Write(f, (APTR)s, (LONG)strlen(s));
		Close(f);
	}
}
int main(int argc, char **argv)
{
	static struct WBStartup wbs;
	static struct WBArg arg;
	struct MsgPort *rp;
	struct Process *pr;
	BPTR seg, dir;
	long t, n;
	if (argc < 4 || !(rp = CreateMsgPort()))
		return 20;
	static char path[128];
	seg = LoadSeg((STRPTR)argv[1]);
	/* the program's drawer and name, as Workbench gives them */
	strncpy(path, argv[1], sizeof(path) - 1);
	*PathPart((STRPTR)path) = 0;
	dir = Lock((STRPTR)path, ACCESS_READ);
	if (!seg || !dir) {
		say(argv[3], "cannot load\n");
		return 20;
	}
	pr = CreateNewProcTags(NP_Seglist, seg, NP_FreeSeglist, FALSE, NP_Name, (ULONG)"RiptidePrefs",
	                       NP_StackSize, 16384, NP_Cli, FALSE, NP_CurrentDir, 0, NP_Input, 0, NP_Output, 0,
	                       NP_CloseInput, FALSE, NP_CloseOutput, FALSE, TAG_DONE);
	if (!pr) {
		say(argv[3], "cannot start\n");
		return 20;
	}
	arg.wa_Lock = dir;
	arg.wa_Name = (BYTE *)FilePart((STRPTR)argv[1]);
	wbs.sm_Message.mn_Node.ln_Type = NT_MESSAGE;
	wbs.sm_Message.mn_ReplyPort = rp;
	wbs.sm_Message.mn_Length = sizeof(wbs);
	wbs.sm_Process = &pr->pr_MsgPort;
	wbs.sm_Segment = seg;
	wbs.sm_NumArgs = 1;
	wbs.sm_ArgList = &arg;
	PutMsg(&pr->pr_MsgPort, &wbs.sm_Message);
	n = atol(argv[2]) * 5;
	for (t = 0; t < n; t++) {
		if (GetMsg(rp)) {
			char buf[64];
			sprintf(buf, "wbreply ok %ld.%ld s\n", t / 5, (t % 5) * 2);
			say(argv[3], buf);
			Delay(25);              /* the process ends after its reply */
			UnLoadSeg(seg);
			UnLock(dir);
			DeleteMsgPort(rp);
			return 0;
		}
		Delay(10);
	}
	say(argv[3], "wbreply timeout\n");
	return 5;
}
EOF
cat > "$work/tps.c" <<'EOF'
/* tps NAME OUT: does a task called NAME exist? ("yes"/"no" to OUT) */
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main(int argc, char **argv)
{
	struct Task *t;
	BPTR f;
	if (argc < 3)
		return 20;
	Forbid();
	t = FindTask((STRPTR)argv[1]);
	Permit();
	if ((f = Open((STRPTR)argv[2], MODE_NEWFILE)) != 0) {
		Write(f, t ? "yes\n" : "no\n", t ? 4 : 3);
		Close(f);
	}
	return 0;
}
EOF
for h in tassign tgo tcover tfakeport twb tps; do
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

cat > "$work/orig.conf" <<'EOF'
# Riptide configuration (RiptidePrefs single test)

interface eth0 device=uaenet.device unit=0
dhcp eth0
hostname amy
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
# the next editor started from Workbench takes a while to open its
# window (longer on a busy host), and the screen before it is "stable"
# too: wait for the light face of its Save button (MUI and GadTools both
# have one there; the Shell window behind is grey)
wbnext() {
	sleep ${WBDELAY:-3}
	$ptr wait 2 120 230 548 >/dev/null || fail "no editor window before $1"
	$ptr shot "$work/$1.png"
}
# the same for the GadTools editor (grey buttons): a settled screen other
# than the one left when the editor before it closed
wbnext_gt() {
	$ptr shot "$work/gone.png"
	sleep ${WBDELAY:-3}
	$ptr stable 2 120 "$work/gone.png" >/dev/null || fail "no editor window before $1"
	$ptr shot "$work/$1.png"
}
# wait for out/NAME (written by the Amiga)
waitfor() {
	t=0
	while [ $t -lt ${2:-60} ] && [ ! -s "$dh0/out/$1" ]; do sleep 1; t=$((t + 1)); done
	[ -s "$dh0/out/$1" ] || { fail "no out/$1"; return 1; }
	return 0
}
# let the Amiga go on (its tgo DH0:go/NAME)
go() {
	: > "$dh0/go/$1"
	sleep 2
}
rc0() {
	waitfor "$1" "${2:-60}" && { grep -q '^rc 0' "$dh0/out/$1" || fail "$1: $(cat "$dh0/out/$1")"; }
}
want() { case " ${STAGES:-1 2} " in *" $1 "*) return 0;; esac; return 1; }

# ---- 1: one editor at a time ----
if want 1; then
reset_conf
cat > "$dh0/S/first" <<'EOF'
DH0:C/RiptidePrefs <args>
Echo "rc $RC" >DH0:out/<mark>
EOF
sed 's/<args>/DEBUG=DH0:out\/m1/; s/<mark>/zz_m1/' "$dh0/S/first" > "$dh0/S/first_mui"
sed 's/<args>/GADTOOLS DEBUG=DH0:out\/g1/; s/<mark>/zz_g1/' "$dh0/S/first" > "$dh0/S/first_gt"
boot <<'EOF'
Run >NIL: Execute DH0:S/first_mui
DH0:C/tgo DH0:go/a
Run >NIL: DH0:C/tcover 20
DH0:C/tgo DH0:go/b
DH0:C/RiptidePrefs DEBUG=DH0:out/m2
Echo "rc $RC" >DH0:out/zz_m2
DH0:C/tgo DH0:go/c
DH0:C/RiptidePrefs GADTOOLS DEBUG=DH0:out/m3
Echo "rc $RC" >DH0:out/zz_m3
DH0:C/twb DH0:C/RiptidePrefs 30 DH0:out/wb_m4
DH0:C/RiptidePrefs USE
Echo "rc $RC" >DH0:out/zz_use
DH0:C/tgo DH0:go/d
Run >NIL: Execute DH0:S/first_gt
DH0:C/tgo DH0:go/e
Run >NIL: DH0:C/tcover 20
DH0:C/tgo DH0:go/f
DH0:C/RiptidePrefs DEBUG=DH0:out/g2
Echo "rc $RC" >DH0:out/zz_g2
DH0:C/tgo DH0:go/g
DH0:C/RiptidePrefs GADTOOLS DEBUG=DH0:out/g3
Echo "rc $RC" >DH0:out/zz_g3
DH0:C/tgo DH0:go/h
DH0:C/tps RiptidePrefs DH0:out/ps_end
Echo done >DH0:out/zz_done1
EOF
# the MUI editor first
settle s01-mui-open
go a
sleep 3
settle s02-covered
go b
rc0 zz_m2 30
settle s03-mui-to-front
go c
rc0 zz_m3 30
waitfor wb_m4 40 && { grep -q '^wbreply ok' "$dh0/out/wb_m4" || fail "Workbench start beside an editor: $(cat "$dh0/out/wb_m4")"; }
rc0 zz_use 30
settle s04-mui-after
key s
rc0 zz_m1 30
go d
# the GadTools editor first
settle s05-gt-open
go e
sleep 3
settle s06-gt-covered
go f
rc0 zz_g2 30
settle s07-gt-to-front
go g
rc0 zz_g3 30
key s
rc0 zz_g1 30
go h
waitfor zz_done1 30
cleanup_uae
for l in m2 m3 g2 g3; do
	grep -q 'another RiptidePrefs is open' "$dh0/out/$l" 2>/dev/null || fail "$l: no 'another RiptidePrefs is open' in its log"
	grep -q 'window open\|GadTools editor' "$dh0/out/$l" 2>/dev/null && fail "$l: a second editor opened"
done
[ "$(grep -c 'to the front' "$dh0/out/m1" 2>/dev/null)" -ge 2 ] || fail "the MUI editor did not come to the front twice (log m1)"
[ "$(grep -c 'to the front' "$dh0/out/g1" 2>/dev/null)" -ge 2 ] || fail "the GadTools editor did not come to the front twice (log g1)"
grep -q '^no' "$dh0/out/ps_end" 2>/dev/null || fail "a RiptidePrefs process is left over"
fi

# ---- 2: started as from Workbench ----
if want 2; then
reset_conf
boot <<'EOF'
Run >NIL: DH0:C/tfakeport 900 DH0:out/fake.log
DH0:C/twb DH0:C/RiptidePrefs 120 DH0:out/wb1
DH0:C/tps RiptidePrefs DH0:out/ps1
DH0:C/twb DH0:C/RiptidePrefs 120 DH0:out/wb2
DH0:C/tps RiptidePrefs DH0:out/ps2
DH0:C/twb DH0:C/RiptidePrefs 120 DH0:out/wb3
DH0:C/tps RiptidePrefs DH0:out/ps3
DH0:C/twb DH0:C/RiptidePrefs 120 DH0:out/wb4
DH0:C/tps RiptidePrefs DH0:out/ps4
DH0:C/twb DH0:tt1/RiptidePrefs 120 DH0:out/wb5
DH0:C/tps RiptidePrefs DH0:out/ps5
DH0:C/twb DH0:tt2/RiptidePrefs 120 DH0:out/wb6
DH0:C/tps RiptidePrefs DH0:out/ps6
DH0:C/tcover 3
Echo "rc $RC" >DH0:out/zz_cover
DH0:C/RiptidePrefs PAGE=Names
Echo "rc $RC" >DH0:out/zz_after
EOF
# a: Save without changes
wbnext w01-wb-open
key s
waitfor wb1 60
# b: a change, Save, Apply
wbnext w02-wb-open
key p p p p p p p p
key h shift+Right BackSpace BackSpace BackSpace
typ zeus
key Return
key s
settle w03-apply-asked
key Return
settle w04-applied
key Return
waitfor wb2 60
# c: a change, Save, Later
wbnext w05-wb-open
key p p p p p p p p
key h shift+Right BackSpace BackSpace BackSpace BackSpace
typ mars
key Return
# Save clicked (not its key): it must not stay drawn pressed under the
# question
$ptr click 278 554 >/dev/null
settle w06-apply-asked
key Escape
waitfor wb3 60
# d: a change, Cancel
wbnext w07-wb-open
key p p p p p p p p
key h shift+Right BackSpace BackSpace BackSpace BackSpace
typ venus
key Return
key Escape
waitfor wb4 60
# e, f: the icon's tool types: PAGE=Names and DEBUG=, then GADTOOLS too
wbnext w07b-tooltypes-mui
key Escape
waitfor wb5 60
wbnext_gt w07c-tooltypes-gadtools
key Escape
waitfor wb6 60
# afterwards: a window opens, the editor opens again
rc0 zz_cover 30
wbnext w08-after
key Escape
rc0 zz_after 60
cleanup_uae
grep -q 'window open, page 4' "$dh0/out/tt1" 2>/dev/null || fail "tool types: not the MUI editor on Names (tt1: $(head -c 300 "$dh0/out/tt1" 2>/dev/null))"
grep -q 'GadTools editor' "$dh0/out/tt2" 2>/dev/null || fail "tool types: GADTOOLS did not choose the GadTools editor"
for n in 1 2 3 4 5 6; do
	grep -q '^wbreply ok' "$dh0/out/wb$n" 2>/dev/null || fail "Workbench start $n: $(cat "$dh0/out/wb$n" 2>/dev/null)"
	grep -q '^no' "$dh0/out/ps$n" 2>/dev/null || fail "Workbench start $n: the process is still there"
done
grep -q 'hostname zeus' "$dh0/out/fake.log" 2>/dev/null || fail "Apply sent no hostname command (fake.log: $(cat "$dh0/out/fake.log" 2>/dev/null))"
grep -q 'hostname mars' "$dh0/out/fake.log" 2>/dev/null && fail "Later applied the change"
grep -q '^hostname mars' "$dh0/Prefs/Env-Archive/Riptide/riptide.conf" || fail "Save with Later did not save"
grep -q 'venus' "$dh0/ENV/Riptide/riptide.conf" && fail "Cancel saved"
fi

cleanup
if [ $fails -eq 0 ]; then
	echo "ok   RiptidePrefs one at a time and from Workbench on FS-UAE ($BUILD)"
	echo "     screenshots: $work/*.png"
	exit 0
fi
echo "$fails failures; files in $work"
exit 1
