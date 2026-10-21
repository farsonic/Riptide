#!/bin/sh
# Riptide - full-system test of the speedtest tool on FS-UAE (AROS
# Kickstart, emulated A1200/68020): Riptide runs with loopback only, three
# tests/amiga/httpd_test.c servers (httpd-test) play speedtest.net and a
# speedtest server on 127.0.0.1 (8080 plain, 8081 chunked + redirects,
# 8082 redirecting to https://), and speedtest runs against them:
# --server-url and --mirror discovery, --simple/--json/--list output,
# exact byte counts (checked against the server's counters, read with nc),
# the HTTPS-only error, a refused server and Ctrl-C (raised by a test hook
# while the download runs). The download and upload rates of the basic run
# are printed: both ends share one emulated 68020, so they are CPU bound.
#
#   tests/amiga/run_fsuae_speedtest.sh [BUILDDIR] [TIMEOUT_SECONDS]
# Environment: FSUAE_MODEL (default A1200), FSUAE_EXTRA (more fs-uae
# config lines, e.g. "cpu = 68040").
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-900}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
for f in Riptide rtcheck speedtest httpd-test ping nc; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-speedtest.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck speedtest httpd-test ping nc; do cp "$BUILD/$f" "$dh0/C/"; done

cat > "$dh0/S/riptide.conf" <<'CONF'
hostname aros-test
loglevel 2
CONF
printf 'GET /__stats HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n' > "$dh0/S/statsreq"

U=http://127.0.0.1:8080/speedtest/upload.php
SMALL="--download-sizes 350,500 --upload-sizes 32,64 --count 2 --duration 60"
{
	echo "FailAt 30"
	echo 'DH0:C/rtcheck START="DH0:C/Riptide >DH0:riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start'
	echo 'DH0:C/rtcheck START="DH0:C/httpd-test -p 8080 -d 10 >DH0:out/httpd1.log" NOCHECK >NIL:'
	echo 'DH0:C/rtcheck START="DH0:C/httpd-test -p 8081 -d 10 -c -r >DH0:out/httpd2.log" NOCHECK >NIL:'
	echo 'DH0:C/rtcheck START="DH0:C/httpd-test -p 8082 -s >DH0:out/httpd3.log" NOCHECK >NIL:'
	echo 'DH0:C/ping -c 2 -i 0.5 -q 127.0.0.1 >NIL:'
	echo 'DH0:C/speedtest --version >DH0:out/version'
	echo 'DH0:C/speedtest -h >DH0:out/help'
	echo "DH0:C/speedtest --server-url $U --duration 8 >DH0:out/basic"
	echo "DH0:C/speedtest --server-url $U --simple --duration 4 >DH0:out/simple"
	echo "DH0:C/speedtest --server-url $U --json $SMALL >DH0:out/json"
	echo 'DH0:C/speedtest --mirror http://127.0.0.1:8080 --list >DH0:out/list'
	echo 'DH0:C/speedtest --mirror http://127.0.0.1:8080 --duration 4 --upload-sizes 32,64 >DH0:out/mirror'
	echo "DH0:C/speedtest --mirror http://127.0.0.1:8081 --json $SMALL >DH0:out/chunked"
	echo 'DH0:C/speedtest --mirror http://127.0.0.1:8082 >DH0:out/https'
	echo 'IF ERROR'
	echo '  Echo >>DH0:out/https "rc>=10"'
	echo 'ENDIF'
	echo 'DH0:C/speedtest --server-url http://127.0.0.1:1/speedtest/upload.php >DH0:out/refused'
	echo "DH0:C/speedtest --server-url $U --duration 60 --test-break 4000 >DH0:out/brk"
	echo 'IF WARN'
	echo '  Echo >>DH0:out/brk "rc>=5"'
	echo 'ENDIF'
	echo 'IF ERROR'
	echo '  Echo >>DH0:out/brk "rc>=10"'
	echo 'ENDIF'
	echo 'DH0:C/nc 127.0.0.1 8080 <DH0:S/statsreq >DH0:out/stats1'
	echo 'DH0:C/nc 127.0.0.1 8081 <DH0:S/statsreq >DH0:out/stats2'
	echo 'Echo >DH0:out/zz_done "done"'
} > "$dh0/S/Startup-Sequence"

cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = ${FSUAE_MODEL:-A1200}
fast_memory = ${FSUAE_FAST:-8192}
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
start=$(date +%s)
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
echo "---- emulator ran $(( $(date +%s) - start )) s"
for f in "$dh0"/out/*; do
	tr -d '\r' < "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done

fails=0
out() { cat "$dh0/out/$1" 2>/dev/null; }
expect() {
	n=$1; shift
	for re in "$@"; do
		if ! out "$n" | grep -Eq -- "$re"; then
			echo "FAIL $n: no line matches /$re/"
			fails=$((fails + 1))
		fi
	done
}
reject() {
	if out "$1" | grep -Eq -- "$2"; then
		echo "FAIL $1: unexpected /$2/"
		fails=$((fails + 1))
	fi
}
# a number from JSON output or the server counters
num() { out "$1" | grep -Eo "\"?$2\"?[:=] ?[0-9]+" | head -1 | grep -Eo '[0-9]+$'; }
eq() {   # eq WHAT GOT WANT
	if [ "${2:-x}" != "$3" ]; then
		echo "FAIL $1: ${2:-nothing}, expected $3"
		fails=$((fails + 1))
	fi
}

DL=$((2 * (350 * 350 * 2 / 10 + 500 * 500 * 2 / 10)))
UL=$((2 * (32 + 64) * 1024))
expect start 'stack ready'
expect httpd1.log '^httpd-test: listening on port 8080'
expect version '^speedtest-riptide [0-9.]+$' 'speedtest-cli by Matt Martz \(Apache-2.0\)'
expect help '^usage: speedtest ' '--server-url URL'
expect basic '^Testing against http://127\.0\.0\.1:8080/speedtest/upload\.php\.\.\.$' \
	'^Hosted by 127\.0\.0\.1:8080 \(custom server\): [0-9]+\.[0-9]{3} ms$' \
	'^Testing download speed\.+$' '^Download: [0-9]+\.[0-9]{2} Mbit/s$' \
	'^Testing upload speed\.+$' '^Upload: [0-9]+\.[0-9]{2} Mbit/s$'
reject basic '^speedtest: '
expect simple '^Ping: [0-9]+\.[0-9]{3} ms$' '^Download: [0-9]+\.[0-9]{2} Mbit/s$' '^Upload: [0-9]+\.[0-9]{2} Mbit/s$'
expect json '^\{"download": [0-9]+\.[0-9]{3}, "upload": [0-9]+\.[0-9]{3}, "ping": [0-9]+\.[0-9]{3}, "server": \{"url": "http://127\.0\.0\.1:8080/speedtest/upload\.php", .*"timestamp": "[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:]{8}\.000000Z", "bytes_sent": [0-9]+, "bytes_received": [0-9]+, "share": null, "client": \{.*\}\}$'
eq "json bytes_received" "$(num json bytes_received)" $DL
eq "json bytes_sent" "$(num json bytes_sent)" $UL
expect list '^ 1003\) Refused \(Nowhere, Amiga\) \[5\.55 km\]$' '^ 1001\) Riptide httpd-test \(Loopback, Amiga\) \[55\.59 km\]$'
reject list '1004\)'
expect mirror '^Retrieving speedtest\.net configuration\.\.\.$' \
	'^Testing from Amiga Loopback & Co \(127\.0\.0\.1\)\.\.\.$' '^Retrieving speedtest\.net server list\.\.\.$' \
	'^Selecting best server based on ping\.\.\.$' \
	'^Hosted by Riptide httpd-test \(Loopback\) \[55\.59 km\]: [0-9]+\.[0-9]{3} ms$' \
	'^Download: [0-9]+\.[0-9]{2} Mbit/s$' '^Upload: [0-9]+\.[0-9]{2} Mbit/s$'
expect chunked '"sponsor": "Riptide httpd-test", "id": "1001"' '"isp": "Amiga Loopback & Co"'
eq "chunked bytes_received" "$(num chunked bytes_received)" $DL
eq "chunked bytes_sent" "$(num chunked bytes_sent)" $UL
eq "server 8081 download_bytes" "$(num stats2 download_bytes)" $DL
eq "server 8081 upload_bytes" "$(num stats2 upload_bytes)" $UL
eq "server 8081 redirects" "$(num stats2 redirects)" 2
eq "server 8081 badprefix" "$(num stats2 badprefix)" 0
eq "server 8080 badprefix" "$(num stats1 badprefix)" 0
expect https '^speedtest: configuration: http://127\.0\.0\.1:8082/speedtest-config\.php redirects to https://www\.speedtest\.net/speedtest-config\.php$' \
	'now requires HTTPS' '--server-url http://HOST:PORT/speedtest/upload\.php' '^rc>=10$'
expect refused 'Connection refused' '^speedtest: Unable to connect to the server to test latency\.$'
expect brk '^Testing download speed' '^Cancelling\.\.\.$' '^rc>=5$'
reject brk '^rc>=10$'
reject brk '^Download:'

echo "---- loopback throughput (speedtest and httpd-test on the same emulated ${FSUAE_MODEL:-A1200} ${FSUAE_EXTRA:-})"
grep -E '^(Hosted|Download|Upload):?' "$dh0/out/basic" | sed 's/^/  /'
echo "  server 8080: $(out stats1 | grep -E 'connections=')"
echo "---- work dir $work (outputs in $dh0/out)"
if [ ! -s "$dh0/out/zz_done" ]; then
	echo "run_fsuae_speedtest: did not finish (timeout ${TIMEOUT}s)"
	ls "$dh0/out"
	fails=$((fails + 1))
fi
if [ $fails -eq 0 ]; then
	echo "run_fsuae_speedtest: PASSED"
	exit 0
fi
echo "run_fsuae_speedtest: FAILED ($fails)"
exit 1
