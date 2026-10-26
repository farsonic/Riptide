#!/bin/sh
# Riptide - UDP send rate and pacing on FS-UAE, bridged through tools/fs-uae
# (libpcap) to a veth pair, like the speed lab (run_fsuae_netbench.sh).
# For each CPU preset one boot runs tests/amiga/udpblast.c in several modes
# against tests/amiga/udpsink.py on the host end, which counts what
# arrives on the wire (and how much went without a checksum):
#
#   blk      blocking sendto(), 1400-byte datagrams, unicast
#   nb       non-blocking, WaitSelect() for writability when the stack
#            says EWOULDBLOCK/ENOBUFS (pacing)
#   nock     blocking, SO_NO_CHECK (no UDP checksum)
#   mmsg16   RiptideSendMMsg(), 16 datagrams per call
#   mcast    blocking, to the group 239.192.0.42
#   blk64 / mmsg64   64-byte datagrams: the per-call cost
#
# and a second boot with QoS on (1 frame in the driver, 8 per band) so the
# transmit queue fills: qblk, qnb, qmmsg16 as above must wait for room.
#
# Results: OUT/CPU.txt and OUT/CPU-qos.txt (udpblast and udpsink lines), a table on stdout.
# With CHECK=1 the run fails unless every mode sent all datagrams without
# an error, the host saw them all (99%), nock went without checksums,
# mmsg16 worked and qnb had to wait.
#
#   FSUAE_PCAP=path/to/fs-uae tests/amiga/run_fsuae_udpblast.sh [BUILDDIR]
#
# Environment: CPUS (default "a1200-030 a4000-040"; presets as the speed
# lab's), COUNT datagrams per mode (default 3000), OUT (default
# BUILDDIR/udpblast-results), UB_IF/UB_PEER/UB_NET (default rtub0/rtub1,
# 192.168.150), TIMEOUT seconds per boot (default 900).
# Needs root (veth, packet sockets), python3. Headless: Xvfb :117 when
# DISPLAY is not set.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
here=$(cd "$(dirname "$0")" && pwd)
CPUS=${CPUS:-"a1200-030 a4000-040"}
COUNT=${COUNT:-3000}
OUT=${OUT:-$BUILD/udpblast-results}
TIMEOUT=${TIMEOUT:-900}
UB_IF=${UB_IF:-rtub0}
UB_PEER=${UB_PEER:-rtub1}
NET=${UB_NET:-192.168.150}
PORT=9200
GROUP=239.192.0.42

[ -n "${FSUAE_PCAP:-}" ] && [ -x "$FSUAE_PCAP" ] || { echo "SKIP: set FSUAE_PCAP to a patched fs-uae (tools/fs-uae)"; exit 0; }
[ "$(id -u)" = 0 ] || { echo "SKIP: needs root (veth, packet sockets)"; exit 0; }
for f in Riptide rtcheck udpblast netbench; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done

# as run_fsuae_netbench.sh
cpu_cfg() {
	case $1 in
	a1200-020) printf 'amiga_model = A1200\nfast_memory = 8192\n' ;;
	a1200-030) printf 'amiga_model = A1200\ncpu = 68030\nuae_mmu_model = 0\nuae_cpu_speed = real\nuae_cpu_cycle_exact = true\nuae_cpu_compatible = true\nuae_cpu_multiplier = 7\nfast_memory = 8192\n' ;;
	a4000-040) printf 'amiga_model = A4000/040\nuae_cpu_speed = real\nuae_cpu_cycle_exact = true\nuae_cpu_compatible = true\nuae_cpu_multiplier = 7\nfast_memory = 0\nzorro_iii_memory = 16384\n' ;;
	a4000-060) printf 'amiga_model = A4000/040\ncpu = 68060\nuae_mmu_model = 0\nuae_cpu_speed = real\nuae_cpu_cycle_exact = true\nuae_cpu_compatible = true\nuae_cpu_multiplier = 14\nfast_memory = 0\nzorro_iii_memory = 16384\n' ;;
	*) return 1 ;;
	esac
}

ip link del $UB_IF 2>/dev/null
ip link add $UB_IF type veth peer name $UB_PEER 2>/dev/null || { echo "SKIP: cannot create a veth pair"; exit 0; }
xpid=
spid=
cleanup() {
	[ -n "$spid" ] && kill "$spid" 2>/dev/null
	[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
	ip link del $UB_IF 2>/dev/null
}
trap cleanup EXIT
ip link set $UB_IF up
ip link set $UB_PEER up
ip addr add $NET.1/24 dev $UB_PEER
if command -v ethtool >/dev/null 2>&1; then
	for d in $UB_IF $UB_PEER; do ethtool -K $d tso off gso off gro off tx off rx off >/dev/null 2>&1; done
fi
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null 2>&1 || { echo "SKIP: no DISPLAY and no Xvfb"; exit 0; }
	Xvfb :117 -screen 0 1024x768x24 >/dev/null 2>&1 &
	xpid=$!
	DISPLAY=:117
	export DISPLAY
	sleep 1
fi
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

MODES="blk nb nock mmsg16 mcast blk64 mmsg64"
# a second boot with QoS on, one frame in the driver and 8 per band, so
# that the queue fills and the senders must wait (pacing)
QMODES="qblk qnb qmmsg16"
QOS_CONF='qos on eth0
qos inflight 1
qos limit 8'
mode_args() {
	case $1 in
	blk|qblk) echo "-s 1400 $NET.1 $PORT" ;;
	nb|qnb) echo "-s 1400 -N $NET.1 $PORT" ;;
	nock)   echo "-s 1400 -C $NET.1 $PORT" ;;
	mmsg16|qmmsg16) echo "-s 1400 -m 16 $NET.1 $PORT" ;;
	mcast)  echo "-s 1400 -t 1 $GROUP $PORT" ;;
	blk64)  echo "-s 64 $NET.1 $PORT" ;;
	mmsg64) echo "-s 64 -m 16 $NET.1 $PORT" ;;
	esac
}

fails=0
# run_one CPU NAME "MODES" [CONF LINES]
run_one() {
	cpu=$1
	name=$2
	modes=$3
	work=$OUT/$name
	rm -rf "$work"
	dh0=$work/dh0
	mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
	cp "$BUILD/Riptide" "$BUILD/rtcheck" "$BUILD/udpblast" "$BUILD/netbench" "$dh0/C/"
	cat >"$dh0/S/riptide.conf" <<CONF
hostname udpblast
interface eth0 device=uaenet.device unit=2
ifconfig eth0 $NET.2/24 up
route add default $NET.1
${4:-}
CONF
	{
		echo 'FailAt 30'
		echo 'DH0:C/rtcheck START="DH0:C/Riptide >DH0:out/riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start'
		echo 'DH0:C/netbench -s 3'
		for m in $modes; do
			echo "DH0:C/udpblast -n $COUNT -l $m $(mode_args $m) >>DH0:out/blast"
			echo 'DH0:C/netbench -s 3'
		done
		echo 'Echo done >DH0:out/zz_done'
	} >"$dh0/S/Startup-Sequence"
	{
		echo '[fs-uae]'
		cpu_cfg "$cpu"
		echo 'uae_sana2 = 1'
		echo "uaenet_pcap = $UB_IF"
		echo "hard_drive_0 = $dh0"
		echo 'hard_drive_0_label = DH0'
		echo 'sound_card = none'
		echo 'floppy_drive_count = 0'
		echo 'automatic_input_grab = 0'
		echo 'video_sync = off'
	} >"$work/test.fs-uae"
	python3 -I "$here/udpsink.py" $UB_PEER $NET.1 $PORT $GROUP >"$work/sink.log" 2>&1 &
	spid=$!
	SDL_AUDIODRIVER=dummy "$FSUAE_PCAP" "$work/test.fs-uae" >"$work/fs-uae.log" 2>&1 &
	upid=$!
	t=0
	while [ $t -lt "$TIMEOUT" ] && [ ! -s "$dh0/out/zz_done" ] && kill -0 $upid 2>/dev/null; do
		sleep 2
		t=$((t + 2))
	done
	sleep 3
	kill $upid 2>/dev/null
	wait $upid 2>/dev/null
	kill $spid 2>/dev/null
	wait $spid 2>/dev/null
	spid=
	tr -d '\r' <"$dh0/out/blast" >"$OUT/$name.txt" 2>/dev/null
	cat "$work/sink.log" >>"$OUT/$name.txt"
	[ -s "$dh0/out/zz_done" ] || { echo "FAIL $name: did not finish (${TIMEOUT}s)"; fails=$((fails + 1)); }
}

for cpu in $CPUS; do
	cpu_cfg "$cpu" >/dev/null || { echo "unknown CPU preset $cpu"; exit 2; }
	echo "---- $cpu"
	run_one "$cpu" "$cpu" "$MODES"
	echo "---- $cpu, QoS"
	run_one "$cpu" "$cpu-qos" "$QMODES" "$QOS_CONF"
done

# table: per mode the sender's line pair and the sink's burst (in order)
echo "| CPU | mode | sent | dgram/s | KB/s | us/dgram | us/call | errors | waits | wire | no csum |"
echo "|---|---|---|---|---|---|---|---|---|---|---|"
for name in $(for c in $CPUS; do echo $c $c-qos; done); do
	cpu=$name
	f=$OUT/$name.txt
	[ -f "$f" ] || continue
	case $name in *-qos) modes=$QMODES ;; *) modes=$MODES ;; esac
	i=0
	for m in $modes; do
		l1=$(grep "^udpblast: $m [0-9]* sent" "$f" | head -1)
		l2=$(grep "^udpblast: $m [0-9]* us/dgram" "$f" | head -1)
		if [ -z "$l1" ]; then
			echo "| $cpu | $m | $(grep "^udpblast: $m " "$f" | head -1 | cut -d' ' -f3-) |||||||||"
			continue
		fi
		i=$((i + 1))
		b=$(grep "^burst $i:" "$f" | head -1)
		set -- $l1
		sent=$3 rate=$9 kbs=${11}
		set -- $l2
		usd=$3 usc=$5 err=${10} waits=${12}
		err=${err%,}
		set -- $b
		wire=${3:-0} nock=${8:-0}
		echo "| $cpu | $m | $sent | $rate | $kbs | $usd | $usc | $err | $waits | $wire | $nock |"
		if [ "${CHECK:-0}" = 1 ]; then
			[ "$sent" = "$COUNT" ] || { echo "FAIL $cpu $m: sent $sent of $COUNT"; fails=$((fails + 1)); }
			[ "$err" = 0 ] || { echo "FAIL $cpu $m: $err send errors"; fails=$((fails + 1)); }
			[ $m != qnb ] || [ "$waits" -gt 0 ] || { echo "FAIL $cpu qnb: never waited for room"; fails=$((fails + 1)); }
			# the host end (fs-uae pcap, packet socket) may drop the odd frame
			[ "$wire" -ge $((sent - sent / 100)) ] || { echo "FAIL $cpu $m: the host saw $wire of $sent"; fails=$((fails + 1)); }
			if [ $m = nock ]; then
				[ "$nock" = "$wire" ] || { echo "FAIL $cpu nock: $nock of $wire without checksum"; fails=$((fails + 1)); }
			else
				[ "$nock" = 0 ] || { echo "FAIL $cpu $m: $nock without checksum"; fails=$((fails + 1)); }
			fi
		fi
	done
done
echo "(results in $OUT)"
if [ $fails -eq 0 ]; then
	echo "run_fsuae_udpblast: PASSED"
	exit 0
fi
echo "run_fsuae_udpblast: FAILED ($fails)"
exit 1
