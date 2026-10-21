#!/bin/sh
# Riptide - "Riptide BENCH" on FS-UAE (AROS Kickstart): throughput of the C
# and 680x0 assembly checksum/copy routines, timed with the EClock.
# Emulator timing is not real hardware timing: compare columns, not
# absolute numbers.
#
#   tests/amiga/run_fsuae_bench.sh [BUILDDIR] [TIMEOUT_SECONDS]
# Environment: FSUAE_MODEL (default A1200), FSUAE_EXTRA (more fs-uae
# config lines, e.g. "cpu = 68060").
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-600}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
[ -f "$BUILD/Riptide" ] || { echo "SKIP: $BUILD/Riptide not built"; exit 0; }
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-bench.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
cp "$BUILD/Riptide" "$dh0/C/"
{
	echo "FailAt 30"
	echo 'DH0:C/Riptide BENCH >DH0:out/bench'
	echo 'echo done >DH0:out/zz_done'
} > "$dh0/S/Startup-Sequence"
cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = ${FSUAE_MODEL:-A1200}
${FSUAE_EXTRA:-}
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
	Xvfb :98 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:98
	export DISPLAY
	sleep 1
fi
SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ]; do
	[ -s "$dh0/out/zz_done" ] && { sleep 1; break; }
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
kill $upid 2>/dev/null
wait $upid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
tr -d '\r' < "$dh0/out/bench" 2>/dev/null
if grep -q '^  8192 ' "$dh0/out/bench" 2>/dev/null; then
	echo "run_fsuae_bench: PASSED (work dir $work)"
	exit 0
fi
echo "run_fsuae_bench: FAILED (work dir $work; $work/fs-uae.log)"
exit 1
