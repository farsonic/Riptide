#!/bin/sh
# Riptide - full-system test of the ftp client on FS-UAE (AROS Kickstart):
# Riptide runs with loopback only, tests/amiga/ftpd_test.c (ftpd-test)
# serves DH0:ftproot on port 2121, and ftp runs scripts against it over
# 127.0.0.1: login, listings, 1 MB uploads and downloads, ascii mode
# conversion, restart/reget, mget/mput, directory and rename commands,
# error replies, EPSV->PASV fallback with a bogus PASV address (the NAT
# case), active mode (PORT), aborted transfers (simulated Ctrl-C) and the
# ftp:// URL forms. DH0: is a host directory, so the transferred files are
# compared here, byte for byte. A packet capture (Riptide CAPTURE) of one
# URL download is saved to DH0: and checked here (and with tcpdump).
# Riptide PROFILE and DEBUG are checked on
# the same transfers: a 1 MB download without diagnostics, with the
# profiler, and with profiler plus debug log (throughput of each printed).
#
#   tests/amiga/run_fsuae_ftp.sh [BUILDDIR] [TIMEOUT_SECONDS]
# Environment: FSUAE_MODEL (default A1200), FSUAE_EXTRA (more fs-uae
# config lines, e.g. "cpu = 68040"), for throughput runs on other CPUs.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-900}
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
for f in Riptide rtcheck ftp ftpd-test ping; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-ftp.XXXXXX")
dh0=$work/dh0
root=$dh0/ftproot
loc=$dh0/local
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out" "$root/sub" "$loc"
for f in Riptide rtcheck ftp ftpd-test ping; do cp "$BUILD/$f" "$dh0/C/"; done

# test data
head -c 1048576 /dev/urandom > "$root/big.bin"
head -c 1048576 /dev/urandom > "$loc/up.bin"
head -c 400000 "$root/big.bin" > "$loc/big.part"      # for reget
head -c 300000 "$root/big.bin" > "$loc/big.part2"     # for restart 300000
printf 'hello from the server\nsecond line\n' > "$root/hello.txt"
head -c 3000 /dev/urandom > "$root/sub/a.dat"
head -c 5000 /dev/urandom > "$root/sub/b.dat"
printf 'not matched by *.dat\n' > "$root/sub/c.txt"
printf 'line one\r\nline two\r\nbare\rcr\r\nend\r\n' > "$root/crlf.txt"
i=0; : > "$loc/text.txt"
while [ $i -lt 400 ]; do echo "text line $i of the ascii mode test, padded to cross buffer ends" >> "$loc/text.txt"; i=$((i + 1)); done
printf 'mput one\n' > "$loc/m1.txt"
printf 'mput two\n' > "$loc/m2.txt"

cat > "$dh0/S/riptide.conf" <<'CONF'
hostname aros-test
loglevel 2
CONF

cat > "$dh0/S/ftpscript" <<'SCRIPT'
# main session (ftp -v -n 127.0.0.1 2121 <DH0:S/ftpscript)
user test secret
lcd DH0:local
system
pwd
ls
nlist
size big.bin
modtime big.bin
get big.bin big.copy
get big.bin NIL:
hash 131072
get big.bin NIL:
hash off
put up.bin
get up.bin up.back
size up.bin
reget big.bin big.part
restart 300000
get big.bin big.part2
ascii
put text.txt text.asc
get text.asc text.back
quote TYPE I
put text.txt wire.txt
get crlf.txt crlf.lf
binary
type
cd sub
pwd
mget *.dat
cdup
mkdir newdir
cd newdir
mput m*.txt
ls
cdup
rename newdir/m1.txt newdir/renamed.txt
delete newdir/m2.txt
get nosuchfile
delete nosuchfile
cd nosuchdir
size nosuchfile
rmdir sub
mkdir tmpdir
rmdir tmpdir
quote FEAT
quote HELP
site help
nosuchcmd
site noepsv
site pasvip 10.99.99.99
debug
ls sub
debug
status
passive
debug
nlist sub
get hello.txt act.txt
debug
passive
testbreak 200000
get big.bin broken.bin
pwd
testbreak 100000
put up.bin broken.up
size up.bin
get hello.txt after.txt
close
ls
quit
SCRIPT

# IPv6 (ftp -v -n ::1 2122): EPSV, then active mode with EPRT
cat > "$dh0/S/ftp6" <<'SCRIPT'
user test secret
lcd DH0:local
debug
get hello.txt hello6.txt
get big.bin big6.bin
passive
put up.bin up6.bin
get hello.txt act6.txt
bye
SCRIPT
# an IPv4 client of the dual-stack server on [::]:2122 (PASV/PORT still work)
cat > "$dh0/S/ftp46" <<'SCRIPT'
user test secret
lcd DH0:local
debug
site noepsv
get hello.txt hello46.txt
passive
get hello.txt act46.txt
bye
SCRIPT

# throughput with and without the diagnostics
cat > "$dh0/S/ftpspeed" <<'SCRIPT'
user test secret
get big.bin NIL:
get big.bin NIL:
bye
SCRIPT

cat > "$dh0/S/badlogin" <<'SCRIPT'
user baduser wrong
ls
user test
secret
pwd
bye
SCRIPT

{
	echo "FailAt 30"
	echo 'DH0:C/rtcheck START="DH0:C/Riptide >DH0:riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start'
	echo 'DH0:C/rtcheck START="DH0:C/ftpd-test -v -p 2121 DH0:ftproot >DH0:out/ftpd.log" NOCHECK >NIL:'
	echo 'DH0:C/rtcheck START="DH0:C/ftpd-test -6 -v -p 2122 DH0:ftproot >DH0:out/ftpd6.log" NOCHECK >NIL:'
	# give the server a moment to listen
	echo 'DH0:C/ping -c 2 -i 0.5 -q 127.0.0.1 >NIL:'
	echo 'DH0:C/ftp -v -n 127.0.0.1 2121 <DH0:S/ftpscript >DH0:out/ftp1'
	echo 'DH0:C/ftp -v -n 127.0.0.1 2121 <DH0:S/badlogin >DH0:out/badlogin'
	echo 'DH0:C/Riptide CAPTURE start iface=nope >DH0:out/cap_bad'
	echo 'DH0:C/Riptide CAPTURE start proto=tcp host=127.0.0.1 snaplen=96 file=DH0:cap.pcap >DH0:out/cap_start'
	echo 'DH0:C/ftp -o DH0:local/url.dat ftp://user:pw@127.0.0.1:2121/sub/a.dat >DH0:out/url_get'
	echo 'DH0:C/Riptide CAPTURE status >DH0:out/cap_status'
	echo 'DH0:C/Riptide CAPTURE stop >DH0:out/cap_stop'
	echo 'DH0:C/Riptide CAPTURE save DH0:cap2.pcap >DH0:out/cap_save'
	echo 'DH0:C/Riptide CAPTURE clear >DH0:out/cap_clear'
	echo 'DH0:C/ftp -u ftp://127.0.0.1:2121/sub/ DH0:local/m1.txt >DH0:out/url_put'
	echo 'DH0:C/ftp -v ftp://127.0.0.1:2121/sub/ >DH0:out/url_list'
	echo 'DH0:C/ftp ftp://127.0.0.1:2121/nosuch.bin >DH0:out/url_missing'
	echo 'DH0:C/ftp -v -n ::1 2122 <DH0:S/ftp6 >DH0:out/ftp6'
	echo 'DH0:C/ftp -v -n 127.0.0.1 2122 <DH0:S/ftp46 >DH0:out/ftp46'
	echo 'DH0:C/ftp -o DH0:local/url6.dat ftp://[::1]:2122/sub/a.dat >DH0:out/url6'
	# diagnostics: profile and debug log around the same download
	echo 'DH0:C/Riptide DEBUG STATUS >DH0:out/debug_off'
	echo 'DH0:C/ftp -v -n 127.0.0.1 2121 <DH0:S/ftpspeed >DH0:out/speed_off'
	echo 'DH0:C/Riptide PROFILE START >DH0:out/prof_start'
	echo 'DH0:C/ftp -v -n 127.0.0.1 2121 <DH0:S/ftpspeed >DH0:out/speed_prof'
	echo 'DH0:C/Riptide PROFILE SHOW >DH0:out/prof_show'
	echo 'DH0:C/Riptide PROFILE RESET >NIL:'
	echo 'DH0:C/Riptide DEBUG tcp,socket,timer SIZE=32 >DH0:out/debug_on'
	echo 'DH0:C/ftp -v -n 127.0.0.1 2121 <DH0:S/ftpspeed >DH0:out/speed_dbg'
	echo 'DH0:C/Riptide PROFILE STOP >DH0:out/prof_stop'
	# a short session whose lines all fit the ring (2 MB Chip RAM only:
	# a bigger ring leaves no room to load the Riptide command again)
	echo 'DH0:C/Riptide DEBUG CLEAR >NIL:'
	echo 'DH0:C/ftp -o NIL: ftp://127.0.0.1:2121/hello.txt >NIL:'
	echo 'DH0:C/Riptide PROFILE SHOW >DH0:out/prof_show2'
	echo 'DH0:C/Riptide DEBUG SHOW 5 >DH0:out/debug_show'
	echo 'DH0:C/Riptide DEBUG DUMP DH0:out/debug.dump >DH0:out/debug_dump'
	echo 'DH0:C/Riptide DEBUG NONE >DH0:out/debug_none'
	echo 'DH0:C/Riptide DEBUG BOGUS >DH0:out/debug_bad'
	echo 'DH0:C/Riptide PROFILE BOGUS >DH0:out/prof_bad'
	echo 'DH0:C/ftp -h >DH0:out/zz_done'
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
	Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:99
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
# after NAME A B: a line matching B comes after the first line matching A
after() {
	if ! out "$1" | awk -v a="$2" -v b="$3" '!s && $0 ~ a { s = 1; next } s && $0 ~ b { f = 1 } END { exit !f }'; then
		echo "FAIL $1: no /$3/ after /$2/"
		fails=$((fails + 1))
	fi
}
same() {   # same FILE1 FILE2 description
	if ! cmp -s "$1" "$2"; then
		echo "FAIL $3: $(basename "$1") differs from $(basename "$2") ($(wc -c < "$1" 2>/dev/null || echo missing) vs $(wc -c < "$2") bytes)"
		fails=$((fails + 1))
	fi
}

expect start 'stack ready'
expect ftp1 '^Connected to 127\.0\.0\.1 \(127\.0\.0\.1\)\.' '^220-Riptide ftpd-test' '^220 Ready\.' \
	'^230 User test logged in\.' '^Local directory now DH0:local' '^215 UNIX Type: L8' \
	'^257 "/" is the current directory' '^-rw-r--r-- 1 ftp ftp +1048576 ... [ 0-9][0-9] [0-9]{2}:[0-9]{2} big\.bin' \
	'^drwxr-xr-x .* sub$' '^hello\.txt$' '^213 1048576$' '^213 [0-9]{14}$' \
	'^1048576 bytes received in [0-9]+\.[0-9]{2} secs \([0-9]+\.[0-9] [KM]B/s\)' \
	'^1048576 bytes sent in [0-9]+\.[0-9]{2} secs' '^########$' \
	'^350 Restarting at 400000\.' '^648576 bytes received' '^350 Restarting at 300000\.' '^748576 bytes received' \
	'^200 Type set to A\.' '^Using binary mode to transfer files\.' '^257 "/sub" is the current directory' \
	'^257 "/newdir" directory created' '^-rw-r--r-- .* m1\.txt$' '^-rw-r--r-- .* m2\.txt$' \
	'^350 File exists' '^250 Rename successful' '^250 DELE command successful' \
	'^550 nosuchfile: No such file or directory' '^550 nosuchdir: No such file or directory' \
	'^550 nosuchfile: not a plain file' '^550 sub: Cannot remove directory' '^250 RMD command successful' \
	'^211-Features:' '^ REST STREAM$' '^211 End$' '^214-The following commands are recognized:' '^214 Direct comments' \
	'^214-SITE commands:' '^\?Invalid command$' '^200 EPSV disabled\.' \
	'^---> EPSV$' '^502 EPSV not implemented\.' '^EPSV refused, trying PASV\.' '^---> PASV$' \
	'^Ignoring PASV address 10\.99\.99\.99, using 127\.0\.0\.1\.' '^-rw-r--r-- .* a\.dat$' \
	'^Passive mode: off\.' '^---> PORT 127,0,0,1,[0-9]+,[0-9]+$' '^200 PORT command successful\.' '^sub/a\.dat$' \
	'^Passive mode: on\.' '^Passive mode: on; EPSV: on \(refused by server, using PASV\)\.' \
	'^receive aborted\. waiting for remote to finish abort\.' '^426 Connection closed; transfer aborted\.' \
	'^226 ABOR command successful\.' '^send aborted\. waiting for remote to finish abort\.' \
	'^225 ABOR command successful' '^221 Goodbye\.' '^Not connected\.'
reject ftp1 '^---> PASS secret'
after ftp1 '^receive aborted' '^257 "/" is the current directory'
after ftp1 '^send aborted' '^213 1048576$'
after ftp1 '^ftp> get hello.txt after.txt' '^226 Transfer complete'
expect badlogin '^530 Login incorrect\.' '^Login failed\.' '^530 Please login with USER and PASS\.' \
	'^230 User test logged in\.' '^257 "/" is the current directory'
expect url_list '^-rw-r--r-- .* a\.dat$' '^-rw-r--r-- .* c\.txt$'
expect url_missing '^550 nosuch\.bin: No such file or directory'
expect url_get '^3000 bytes received in [0-9]+\.[0-9]{2} secs'
reject url_get '^(220|230|150|226)'
expect url_put '^9 bytes sent in [0-9]+\.[0-9]{2} secs'
expect zz_done 'usage: ftp'
expect ftp6 '^Connected to ::1 \(::1\)\.' '^230 User test logged in\.' '^---> EPSV$' \
	'^229 Entering Extended Passive Mode \(\|\|\|[0-9]+\|\)' '^1048576 bytes received in' \
	'^Passive mode: off\.' '^---> EPRT \|2\|::1\|[0-9]+\|$' '^200 EPRT command successful\.' '^221 Goodbye\.'
reject ftp6 '^---> (PASV|PORT)'
expect ftp46 '^Connected to 127\.0\.0\.1 \(127\.0\.0\.1\)\.' '^---> PASV$' '^227 Entering Passive Mode \(127,0,0,1,' \
	'^---> PORT 127,0,0,1,[0-9]+,[0-9]+$' '^200 PORT command successful\.'
expect url6 '^3000 bytes received in [0-9]+\.[0-9]{2} secs'
expect ftpd6.log 'connection from ::1' 'connection from ::ffff:127\.0\.0\.1'

# diagnostics
expect debug_off '^debug: off; ring 0 KB'
expect prof_start '^profile: started'
expect prof_show '^profile: running, [0-9]+\.[0-9] ms wall clock, [0-9]+ KB received' \
	'^section +calls +total ms +%wall +us/call +max us +us/KB' \
	'^tcp input +[1-9][0-9]* ' '^tcp output +[1-9][0-9]* ' '^ip input +[1-9][0-9]* ' \
	'^copy to app +[1-9][0-9]* ' '^copy from app +[1-9][0-9]* ' '^stack loop +[1-9][0-9]* ' \
	'^idle +[1-9][0-9]* ' '^timers +[1-9][0-9]* ' '^not in Riptide '
expect debug_on '^debug: tcp,timer,socket; ring 32 KB'
expect prof_stop '^profile: stopped'
expect prof_show2 '^profile: stopped' '^tcp input +[1-9][0-9]* '
expect debug_show ' (tcp|socket|timer): '
expect debug_dump '^debug: [0-9]+ bytes written to DH0:out/debug\.dump'
expect debug_none '^debug: off; ring 32 KB'
expect debug_bad "unknown category 'BOGUS'"
expect prof_bad '^usage: profile start'
for f in speed_off speed_prof speed_dbg; do
	expect $f '^1048576 bytes received in'
done
if [ -s "$dh0/out/debug.dump" ]; then
	grep -Eq '^ *[0-9]+\.[0-9]{6} tcp: [0-9]+>[0-9]+ state [A-Z_]+ -> ESTABLISHED' "$dh0/out/debug.dump" ||
		{ echo "FAIL debug.dump: no TCP state change"; fails=$((fails + 1)); }
	grep -Eq '^ *[0-9]+\.[0-9]{6} socket: socket\(2, 1, [06]\) = [0-9]+' "$dh0/out/debug.dump" ||
		{ echo "FAIL debug.dump: no socket() line"; fails=$((fails + 1)); }
	grep -Eq ' tcp: [0-9]+>[0-9]+ ack (now|delayed)' "$dh0/out/debug.dump" ||
		{ echo "FAIL debug.dump: no ACK decisions"; fails=$((fails + 1)); }
else
	echo "FAIL debug.dump missing"; fails=$((fails + 1))
fi

# files, byte for byte
same "$loc/big.copy" "$root/big.bin" "get"
same "$root/up.bin" "$loc/up.bin" "put"
same "$loc/up.back" "$loc/up.bin" "get back"
same "$loc/big.part" "$root/big.bin" "reget"
same "$loc/big.part2" "$root/big.bin" "restart"
same "$root/text.asc" "$loc/text.txt" "ascii put (both sides convert)"
same "$loc/text.back" "$loc/text.txt" "ascii get (both sides convert)"
sed 's/$/\r/' "$loc/text.txt" > "$work/text.crlf"
same "$root/wire.txt" "$work/text.crlf" "ascii put: LF -> CRLF on the wire"
printf 'line one\nline two\nbare\rcr\nend\n' > "$work/crlf.expect"
same "$loc/crlf.lf" "$work/crlf.expect" "ascii get: CRLF -> LF"
same "$loc/a.dat" "$root/sub/a.dat" "mget a.dat"
same "$loc/b.dat" "$root/sub/b.dat" "mget b.dat"
[ -e "$loc/c.txt" ] && { echo "FAIL mget: c.txt matched *.dat"; fails=$((fails + 1)); }
same "$root/newdir/renamed.txt" "$loc/m1.txt" "mput + rename"
[ -e "$root/newdir/m2.txt" ] && { echo "FAIL delete: newdir/m2.txt still there"; fails=$((fails + 1)); }
[ -e "$root/tmpdir" ] && { echo "FAIL rmdir: tmpdir still there"; fails=$((fails + 1)); }
same "$loc/act.txt" "$root/hello.txt" "active mode get"
same "$loc/after.txt" "$root/hello.txt" "get after the aborts"
same "$loc/url.dat" "$root/sub/a.dat" "URL get"
same "$root/sub/m1.txt" "$loc/m1.txt" "URL put"
same "$loc/hello6.txt" "$root/hello.txt" "IPv6 EPSV get"
same "$loc/big6.bin" "$root/big.bin" "IPv6 EPSV get (1 MB)"
same "$root/up6.bin" "$loc/up.bin" "IPv6 EPRT put"
same "$loc/act6.txt" "$root/hello.txt" "IPv6 EPRT get"
same "$loc/hello46.txt" "$root/hello.txt" "IPv4 client, dual-stack server, PASV"
same "$loc/act46.txt" "$root/hello.txt" "IPv4 client, dual-stack server, PORT"
same "$loc/url6.dat" "$root/sub/a.dat" "URL get from [::1]"
# packet capture of the URL download
expect cap_bad '^capture: no interface .nope.'
expect cap_start '^capture: running, 0 frames' 'snaplen 96, stop when full, proto tcp host 127\.0\.0\.1, file DH0:cap\.pcap'
expect cap_status '^capture: running, [1-9][0-9]* frames .* 0 dropped'
expect cap_stop '^capture: [1-9][0-9]* frames saved to DH0:cap\.pcap'
expect cap_save '^capture: [1-9][0-9]* frames saved to DH0:cap2\.pcap'
expect cap_clear '^capture: off$'
nframes=$(sed -n 's/^capture: \([0-9]*\) frames saved to DH0:cap\.pcap.*/\1/p' "$dh0/out/cap_stop")
same "$dh0/cap2.pcap" "$dh0/cap.pcap" "capture save = file saved by stop"
if ! python3 -I - "$dh0/cap.pcap" "${nframes:-0}" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
want = int(sys.argv[2])
assert d[:4] == b'\xa1\xb2\xc3\xd4', 'big-endian pcap magic'
major, minor, _, _, snap, link = struct.unpack('>HHiIII', d[4:24])
assert (major, minor, snap, link) == (2, 4, 16 + 96, 113), (major, minor, snap, link)
off, n, syn, retr, data = 24, 0, 0, 0, 0
while off < len(d):
    sec, usec, incl, orig = struct.unpack('>IIII', d[off:off + 16])
    rec = d[off + 16:off + 16 + incl]
    assert 16 <= incl <= orig and incl <= snap and usec < 1000000 and len(rec) == incl
    pkt, hat, hal, proto = struct.unpack('>HHH8xH', rec[:16])
    assert (pkt, hat, hal, proto) == (0, 772, 0, 0x800), (pkt, hat, hal, proto)
    ip = rec[16:]
    assert ip[9] == 6 and ip[12:16] == b'\x7f\0\0\x01'
    ihl = (ip[0] & 15) * 4
    sport, dport = struct.unpack('>HH', ip[ihl:ihl + 4])
    flags = ip[ihl + 13]
    if dport == 2121 and flags & 2:
        syn += 1
    if b'RETR sub/a.dat' in ip or b'RETR a.dat' in ip:
        retr += 1
    tlen = struct.unpack('>H', ip[2:4])[0] - ihl - (ip[ihl + 12] >> 4) * 4
    if sport != 2121 and dport != 2121:
        data += tlen
    off += 16 + incl
    n += 1
assert n == want, (n, want)
assert syn == 1 and retr == 1, (syn, retr)
assert data >= 3000, data
print('  capture: %d frames, %d data-connection bytes' % (n, data))
PY
then
	echo "FAIL capture: DH0:cap.pcap"; fails=$((fails + 1))
fi
if command -v tcpdump >/dev/null 2>&1; then
	tl=$(tcpdump -nn -r "$dh0/cap.pcap" 2>/dev/null | grep -c '127\.0\.0\.1\.2121 >')
	[ "$tl" -gt 0 ] || { echo "FAIL capture: tcpdump cannot read DH0:cap.pcap"; fails=$((fails + 1)); }
fi
bs=$(wc -c < "$loc/broken.bin" 2>/dev/null || echo 0)
if [ "$bs" -lt 200000 ] || [ "$bs" -ge 1048576 ]; then
	echo "FAIL abort: broken.bin has $bs bytes"; fails=$((fails + 1))
fi

echo "---- throughput (loopback, client and server on the same emulated ${FSUAE_MODEL:-A1200} ${FSUAE_EXTRA:-})"
grep -E 'bytes (received|sent) in' "$dh0/out/ftp1" | grep -E '^(1048576|648576|748576) ' | head -8 | sed 's/^/  /'
echo "---- the same over IPv6 (::1)"
grep -E 'bytes (received|sent) in' "$dh0/out/ftp6" | grep -E '^1048576 ' | sed 's/^/  /'
for f in speed_off speed_prof speed_dbg; do
	case $f in speed_off) d="diagnostics off";; speed_prof) d="profiling";; *) d="profiling + debug tcp,socket,timer";; esac
	echo "---- 1 MB get, $d"
	grep -E '^1048576 bytes received in' "$dh0/out/$f" | sed 's/^/  /'
done
echo "---- profile of the profiled run"
sed 's/^/  /' "$dh0/out/prof_show" 2>/dev/null
echo "---- work dir $work (outputs in $dh0/out)"
if [ ! -s "$dh0/out/zz_done" ]; then
	echo "run_fsuae_ftp: did not finish (timeout ${TIMEOUT}s)"
	tail -5 "$dh0/out/ftp1" 2>/dev/null
	fails=$((fails + 1))
fi
if [ $fails -eq 0 ]; then
	echo "run_fsuae_ftp: PASSED"
	exit 0
fi
echo "run_fsuae_ftp: FAILED ($fails)"
exit 1
