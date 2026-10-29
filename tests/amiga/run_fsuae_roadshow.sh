#!/bin/sh
# Riptide - full-system test of the Roadshow extensions of
# bsdsocket.library (version 4): boots FS-UAE, starts Riptide (loopback
# only) and runs tests/amiga/rs_test, which calls every Roadshow vector
# (packet filter, interfaces, routes, DNS, statistics, monitoring hooks,
# configuration data, netdb, resolver, address allocation, tags).
#
#   tests/amiga/run_fsuae_roadshow.sh [BUILDDIR] [TIMEOUT_SECONDS]
#
# BUILDDIR holds Riptide and rtcheck (build/amiga or build/amiga-gcc);
# rs_test comes from build/amiga (vbcc, built from the SDK). The
# environment variable KICKSTART_ROM selects a Kickstart ROM file instead
# of FS-UAE's built-in AROS one. Headless: starts Xvfb on :212 when
# DISPLAY is not set. Needs fs-uae.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-300}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
for f in "$BUILD/Riptide" "$BUILD/rtcheck" build/amiga/rs_test; do
	[ -f "$f" ] || { echo "SKIP: $f not built"; exit 0; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-roadshow.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S"
cp "$BUILD/Riptide" "$BUILD/rtcheck" build/amiga/rs_test "$dh0/C/"
cat > "$dh0/S/riptide.conf" <<'CONF'
# Roadshow API test: loopback only
hostname rs-test
loglevel 2
host testhost 127.0.0.5
CONF
cat > "$dh0/S/Startup-Sequence" <<'SEQ'
FailAt 30
Stack 65536
DH0:C/rtcheck START="DH0:C/Riptide >DH0:riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >NIL:
DH0:C/rs_test >DH0:result.txt
Echo "rs_test returned $RC" >>DH0:result.txt
DH0:C/Riptide QUIT >>DH0:result.txt
Echo done >DH0:done.txt
SEQ
kick=
[ -n "${KICKSTART_ROM:-}" ] && kick="kickstart_file = $KICKSTART_ROM"
cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = A1200
fast_memory = 8192
$kick
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
	Xvfb :212 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:212
	export DISPLAY
	sleep 1
fi
SDL_AUDIODRIVER=dummy fs-uae "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
upid=$!
t=0
while [ $t -lt "$TIMEOUT" ] && [ ! -f "$dh0/done.txt" ] && kill -0 $upid 2>/dev/null; do
	sleep 2
	t=$((t + 2))
done
sleep 2
kill $upid 2>/dev/null
wait $upid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
echo "---- result.txt"
tr -d '\r' < "$dh0/result.txt" 2>/dev/null
echo "---- riptide.log"
tr -d '\r' < "$dh0/riptide.log" 2>/dev/null | tail -40
echo "---- (work dir $work)"
if tr -d '\r' < "$dh0/result.txt" 2>/dev/null | grep -q "^rs_test: [0-9]* passed, 0 failed"; then
	echo "run_fsuae_roadshow: PASSED"
	exit 0
fi
echo "run_fsuae_roadshow: FAILED"
exit 1
