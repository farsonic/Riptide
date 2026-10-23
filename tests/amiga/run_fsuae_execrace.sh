#!/bin/sh
# Riptide - not a Riptide test: shows that the emulated system (FS-UAE with
# the AROS Kickstart ROM) can lose a task that Signal() made ready, with no
# TCP/IP stack running. Each of BOOTS fresh boots runs tests/amiga/execrace.c
# (two tasks waking each other with Signal()/Wait(), plus timer requests)
# for SECS seconds, with tests/amiga/taskprobe.c watching the "pong" task;
# a boot that does not finish in time is a stall, and the probe's snapshots
# show the lost task: state ready, in neither TaskReady nor TaskWait.
#
#   tests/amiga/run_fsuae_execrace.sh [BOOTS] [SECS] [DIR] [quiet]
#
# DIR is where execrace logs (default DH0:, RAM: keeps the host filesystem
# out of it); "quiet" leaves out the timer task (it then did not stall).
# Stalls are rare on an idle host and frequent under load (several
# emulators at once, or CPU hogs). See docs/DESIGN.md.
# Needs fs-uae, vbcc (VBCC, NDK) and Xvfb when DISPLAY is not set; reports
# how many boots stalled, and the work directories of those.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BOOTS=${1:-5}
SECS=${2:-240}
DIR=${3:-DH0:}
QUIET=${4:-}
here=$(cd "$(dirname "$0")" && pwd)
VBCC=${VBCC:-/opt/vbcc}
NDK=${NDK:-/opt/ndk3.2}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
[ -x "$VBCC/bin/vc" ] || { echo "SKIP: vbcc not found"; exit 0; }
[ -f "$here/../../build/amiga/rtcheck" ] || { echo "SKIP: build/amiga/rtcheck not built"; exit 0; }
bin=$(mktemp -d "${TMPDIR:-/tmp}/riptide-execrace-bin.XXXXXX")
for p in execrace taskprobe; do
	PATH=$VBCC/bin:$PATH VBCC=$VBCC "$VBCC/bin/vc" +aos68k -O1 -I"$NDK/Include_H" -o "$bin/$p" "$here/$p.c" -lamiga \
		>/dev/null 2>&1 || { echo "FAIL: cannot build $p"; exit 1; }
done
xpid=
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :94 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:94
	export DISPLAY
	sleep 1
fi
stalls=0
b=0
while [ $b -lt "$BOOTS" ]; do
	b=$((b + 1))
	work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-execrace.XXXXXX")
	dh0=$work/dh0
	mkdir -p "$dh0/C" "$dh0/S"
	cp "$here/../../build/amiga/rtcheck" "$bin/execrace" "$bin/taskprobe" "$dh0/C/"
	cat > "$dh0/S/Startup-Sequence" <<SEQ
FailAt 30
Stack 65536
DH0:C/rtcheck START="DH0:C/taskprobe DH0:probe 2 pong" NOCHECK >NIL:
DH0:C/execrace $DIR $SECS $QUIET
Echo done >DH0:done.txt
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
	SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
	upid=$!
	t=0
	limit=$((SECS * 2 + 180))
	while [ $t -lt $limit ] && [ ! -f "$dh0/done.txt" ] && kill -0 $upid 2>/dev/null; do
		sleep 5
		t=$((t + 5))
	done
	kill $upid 2>/dev/null
	wait $upid 2>/dev/null
	if [ -f "$dh0/done.txt" ]; then
		echo "boot $b: finished"
		rm -rf "$work"
		continue
	fi
	stalls=$((stalls + 1))
	# the newest snapshot, and every task: which one is in no list?
	last=$(grep -l '^seq ' "$dh0"/probe.[0-9] 2>/dev/null | while read -r f; do
		echo "$(head -1 "$f" | cut -d' ' -f2) $f"; done | sort -n | tail -1 | cut -d' ' -f2)
	echo "boot $b: STALLED after $(tail -1 "$dh0/execrace.log" 2>/dev/null) ($work)"
	if [ -n "$last" ]; then
		for task in pong DH0:C/execrace noise; do
			grep -E '^(ready|wait):' "$last" | grep -q "'$task'" ||
				echo "  task '$task' is in neither exec task list"
		done
	fi
done
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
rm -rf "$bin"
echo "run_fsuae_execrace: $stalls of $BOOTS boots stalled"
