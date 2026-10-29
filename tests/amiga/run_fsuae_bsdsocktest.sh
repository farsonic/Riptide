#!/bin/sh
# Riptide - bsdsocktest (https://github.com/tbdye/bsdsocktest), the
# bsdsocket.library conformance suite, against Riptide on FS-UAE (A1200
# with Fast RAM, AROS Kickstart replacement): its loopback tests, which
# need no network or host helper.
#
#   BSDSOCKTEST=path/to/bsdsocktest tests/amiga/run_fsuae_bsdsocktest.sh [BUILDDIR] [TIMEOUT]
#
# BSDSOCKTEST is the Amiga binary from the bsdsocktest release archive
# (bsdsocktest-0.2.3.lha: bsdsocktest/bsdsocktest). Passes when every test
# that ran passed (skipped ones are fine); prints the failures otherwise.
# Headless: starts Xvfb on :97 when DISPLAY is not set. Needs fs-uae.
#
# With vbcc (VBCC, NDK) it also runs tests/amiga/taskprobe.c, which
# snapshots the stack process every 5 s: when a run gives no result the
# script says whether the emulated system lost the stack process (state
# ready, in no exec task list; see tests/amiga/execrace.c), which is an
# FS-UAE/AROS ROM problem and not a Riptide one.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-900}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
[ -n "${BSDSOCKTEST:-}" ] && [ -f "$BSDSOCKTEST" ] || { echo "SKIP: set BSDSOCKTEST to the bsdsocktest Amiga binary"; exit 0; }
for f in Riptide rtcheck; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-bst.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/bst"
cp "$BUILD/Riptide" "$BUILD/rtcheck" "$dh0/C/"
cp "$BSDSOCKTEST" "$dh0/bst/bsdsocktest"
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
probe=
if [ -x "$VBCC/bin/vc" ] && PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O1 -I"$NDK/Include_H" \
	-o "$dh0/C/taskprobe" "$(dirname "$0")/taskprobe.c" -lamiga >/dev/null 2>&1; then
	probe='DH0:C/rtcheck START="DH0:C/taskprobe DH0:probe 5" NOCHECK >NIL:'
fi
cat > "$dh0/S/riptide.conf" <<'CONF'
# loopback only
hostname aros-test
CONF
cat > "$dh0/S/Startup-Sequence" <<SEQ
FailAt 30
Stack 65536
DH0:C/rtcheck START="DH0:C/Riptide >DH0:riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:start.txt
$probe
CD DH0:bst
DH0:bst/bsdsocktest >DH0:result.txt
Echo "bsdsocktest finished" >DH0:done.txt
SEQ
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
${KICKSTART:+kickstart_file = $KICKSTART}
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
SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ]; do
	[ -f "$dh0/done.txt" ] && { sleep 2; break; }
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
kill $upid 2>/dev/null
wait $upid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
# console colours: ESC [ ... m or the Amiga CSI (0x9b) ... m
plain() { tr -d '\r' < "$1" 2>/dev/null | LC_ALL=C sed 's/\x1b\[[0-9;]*m//g; s/\x9b[0-9;]*m//g'; }
echo "---- result.txt"
plain "$dh0/result.txt"
echo "---- (work dir $work; full log $dh0/bst/bsdsocktest.log)"
res=$(plain "$dh0/result.txt" | grep '^Results:')
if [ -n "$res" ] && ! echo "$res" | grep -q 'FAILED'; then
	echo "run_fsuae_bsdsocktest: PASSED ($res)"
	exit 0
fi
plain "$dh0/bst/bsdsocktest.log" | grep -A3 '^not ok' | head -40
# the newest probe snapshot: was the stack process lost by exec?
last=$(grep -l '^seq ' "$dh0"/probe.[0-9] 2>/dev/null | while read -r f; do
	echo "$(head -1 "$f" | cut -d' ' -f2) $f"; done | sort -n | tail -1 | cut -d' ' -f2)
if [ -z "$res" ] && [ -n "$last" ] && grep -q ' ORPHANED$' "$last"; then
	echo "---- the emulated system lost the stack process (ready, in no exec task list):"
	head -1 "$last"
	grep '^in ready list' "$last"
	echo "---- an FS-UAE/AROS ROM problem (tests/amiga/execrace.c shows it without Riptide)"
	echo "run_fsuae_bsdsocktest: FAILED (no result: emulator lost the stack process)"
	exit 1
fi
echo "run_fsuae_bsdsocktest: FAILED (${res:-no result})"
exit 1
