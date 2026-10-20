#!/bin/sh
# Riptide - full-system test: boot FS-UAE (A1200, built-in AROS Kickstart
# replacement) from a directory hard drive whose Startup-Sequence runs
# rtcheck, which starts Riptide, runs the library checks against the
# running stack (real multitasking, timer.device, signals), and stops it
# with "Riptide QUIT" (refused while libraries are open, accepted after).
#
#   tests/amiga/run_fsuae.sh [BUILDDIR] [TIMEOUT_SECONDS]
#
# Headless: starts Xvfb on :99 when DISPLAY is not set. Needs fs-uae.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-240}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
for f in Riptide rtcheck; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-fsuae.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S"
cp "$BUILD/Riptide" "$BUILD/rtcheck" "$dh0/C/"
cat > "$dh0/S/riptide.conf" <<'CONF'
# full-system test: loopback only
hostname aros-test
loglevel 2
host testhost 127.0.0.5
CONF
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
DH0:C/rtcheck START="DH0:C/Riptide >DH0:riptide.log CONFIG=DH0:S/riptide.conf" STOP="DH0:C/Riptide QUIT" >DH0:result.txt
SEQ
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
	Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:99
	export DISPLAY
	sleep 1
fi
SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ]; do
	if grep -q "rtcheck: \(PASSED\|FAILED\)\|cannot open bsdsocket" "$dh0/result.txt" 2>/dev/null; then
		sleep 2
		break
	fi
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
kill $upid 2>/dev/null
wait $upid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
echo "---- result.txt"
cat "$dh0/result.txt" 2>/dev/null | tr -d '\r'
echo "---- riptide.log"
cat "$dh0/riptide.log" 2>/dev/null | tr -d '\r'
echo "---- (work dir $work)"
if grep -q "rtcheck: PASSED" "$dh0/result.txt" 2>/dev/null; then
	echo "run_fsuae: PASSED"
	exit 0
fi
echo "run_fsuae: FAILED"
exit 1
