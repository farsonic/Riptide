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
cat > "$dh0/S/riptide.conf" <<'CONF'
# loopback only
hostname aros-test
CONF
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
FailAt 30
Stack 65536
DH0:C/rtcheck START="DH0:C/Riptide >DH0:riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:start.txt
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
echo "run_fsuae_bsdsocktest: FAILED (${res:-no result})"
exit 1
