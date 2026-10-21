#!/bin/sh
# Riptide - full-system test of the network tools (ip, ifconfig, route,
# arp, netstat, traceroute, ping, host, sysctl, hostname, nc) on FS-UAE
# with the AROS Kickstart. Riptide runs with loopback plus a uaenet.device
# interface (FS-UAE: uae_sana2 + network_card a2065). This FS-UAE build
# has no slirp backend, so frames go out but nothing comes back: eth0 gets
# a static address and route, neighbours come from "arp -s", and ping,
# traceroute, netstat and host run over loopback.
#
#   tests/amiga/run_fsuae_tools.sh [BUILDDIR] [TIMEOUT_SECONDS]
#
# Every tool's output is saved on DH0:out/ and checked here; the files stay
# in the work directory for inspection.
# SPDX-License-Identifier: GPL-2.0-only
set -u
BUILD=${1:-build/amiga}
TIMEOUT=${2:-300}
TOOLS="ip ifconfig route arp netstat traceroute ping host sysctl hostname nc ping6 traceroute6"
command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
for f in Riptide rtcheck $TOOLS; do
	[ -f "$BUILD/$f" ] || { echo "SKIP: $BUILD/$f not built"; exit 0; }
done
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-tools.XXXXXX")
dh0=$work/dh0
mkdir -p "$dh0/C" "$dh0/S" "$dh0/out"
for f in Riptide rtcheck $TOOLS; do cp "$BUILD/$f" "$dh0/C/"; done
# the SDK client (vbcc build only: it needs the NDK headers)
SDKTEST=
[ -f "$BUILD/sdk_test" ] && { cp "$BUILD/sdk_test" "$dh0/C/"; SDKTEST=1; }
cat > "$dh0/S/riptide.conf" <<'CONF'
hostname aros-test
domain test.lan
loglevel 2
host testhost 127.0.0.5
host localhost 127.0.0.1
host testhost6 2001:db8:5::5
interface eth0 device=uaenet.device unit=0
ifconfig eth0 10.0.2.15/24 up
route add default 10.0.2.2
nameserver 10.0.2.3
CONF
# One command per line: "NAME|COMMAND". Output goes to DH0:out/NAME.
cat > "$work/cmds" <<'CMDS'
ip_a|ip a
ip_addr_lo|ip addr show dev lo0
ip_br_a|ip -br a
ip_br_link|ip -br link
ip_s_link|ip -s -d link show eth0
ip_r|ip r
ip_route_add|ip route add 192.168.50.0/24 via 10.0.2.2 metric 5 mtu 1400
ip_route_add_dup|ip route add 192.168.50.0/24 via 10.0.2.2 metric 5
ip_route_replace|ip route replace default via 10.0.2.3
ip_r2|ip route show
ip_route_get|ip route get 8.8.8.8
ip_route_get_local|ip route get 127.0.0.1
ip_route_del|ip route del 192.168.50.0/24
ip_route_back|ip route replace default via 10.0.2.2
route_n|route -n
route_add|route add -net 172.16.0.0 netmask 255.255.0.0 gw 10.0.2.2
route_add_host|route add -host 172.17.0.1 gw 10.0.2.2
route_n2|route -n
route_del|route del -net 172.16.0.0 netmask 255.255.0.0 gw 10.0.2.2
route_del_host|route del -host 172.17.0.1
route_del_none|route del -host 172.17.0.1
route_n3|route -n
arp_s|arp -s 10.0.2.2 52:55:0a:00:02:02
ip_neigh_add|ip neigh add 10.0.2.4 lladdr 52:55:0a:00:02:04 dev eth0 nud permanent
arp_an|arp -an
arp_n|arp -n
ip_n|ip n
arp_d|arp -d 10.0.2.4
arp_d_none|arp -d 10.0.2.4
ip_n2|ip neigh show dev eth0
ifconfig|ifconfig
ifconfig_mtu|ifconfig eth0 mtu 1400
ip_promisc|ip link set eth0 promisc on
ifconfig_eth0|ifconfig eth0
ifconfig_s|ifconfig -s -a
ip_link_mtu|ip link set dev eth0 mtu 1500 promisc off
ip_link_eth0|ip link show eth0
ip_dns|ip dns
ip_dns_add|ip dns add 10.0.2.53
ip_dns2|ip dns show
ip_dns_del|ip dns del 10.0.2.53
ip_dhcp|ip dhcp show dev eth0
sysctl_ttl|sysctl net.inet.ip.ttl
sysctl_w|sysctl -w net.inet.ip.ttl=32
sysctl_n|sysctl -n net.inet.ip.ttl
sysctl_back|sysctl net.inet.ip.ttl=64
sysctl_a|sysctl -a
sysctl_bad|sysctl no.such.var
hostname|hostname
hostname_f|hostname -f
hostname_I|hostname -I
ping_lo|ping -c 3 -i 0.2 127.0.0.1
ping_amiga|ping 127.0.0.1 COUNT=2 INTERVAL=200 SIZE=100
ping_ttl|ping -c 1 -t 5 -s 1000 testhost
ping_unreach|ping -c 1 -W 5 10.0.2.9
BG|nc -d 127.0.0.1 7777
traceroute|traceroute 127.0.0.1
traceroute_n|traceroute -n -q 2 -w 2 127.0.0.1
traceroute_I|traceroute -n -I -q 1 127.0.0.1
traceroute_eth|traceroute -n -m 2 -q 1 -w 1 8.8.8.8
host_name|host testhost
host_addr|host 127.0.0.5
host_none|host nosuchhost.invalid
netstat_an|netstat -an
netstat_ant|netstat -ant
netstat_l|netstat -l -n
netstat_r|netstat -rn
netstat_i|netstat -i
netstat_s|netstat -s
netstat_T|netstat -Tn
netstat_T6l|netstat -T -6 -ln
sysctl_delack|sysctl -w net.inet.tcp.delack_segs=1
sysctl_delack_back|sysctl -w net.inet.tcp.delack_segs=2
ip_reads|ip link set eth0 reads 64
ip_reads_bad|ip link set eth0 reads 2
ip_reads_lo|ip link set lo0 reads 16
ip_reads_back|ip link set eth0 reads 48
nc_out|nc -d -w 3 127.0.0.1 7778
ip_link_add|ip link add eth1 type sana2 device uaenet.device unit 1
ip_link_after|ip -br link
ip_link_del|ip link del eth1
ip_link_del2|ip -br link
ip_link_down|ip link set eth0 down
ip_r_down|ip r
ip_link_up|ip link set eth0 up
ip_help|ip help
ifconfig_bad|ifconfig nosuch0 up
ip6_addr_add|ip -6 addr add 2001:db8:5::15/64 dev eth0 nodad
ip6_a|ip -6 addr
ip6_a_lo|ip a show dev lo0
ip6_r|ip -6 route
ip6_route_add|ip -6 route add 2001:db8:99::/48 via fe80::1 dev eth0
ip6_r2|ip -6 route show
ip6_route_get|ip -6 route get 2001:db8:99::1
ip6_route_get_lo|ip route get ::1
ip6_n|ip -6 neigh
ip6_br|ip -br a
ifconfig6|ifconfig lo0
ifconfig6_add|ifconfig eth0 inet6 add 2001:db8:6::16/64
ifconfig6_del|ifconfig eth0 inet6 del 2001:db8:6::16
ifconfig6_eth0|ifconfig eth0
ping6_lo|ping6 -c 2 -i 0.2 ::1
ping_6|ping -6 -c 1 ::1
ping6_eth|ping -c 1 2001:db8:5::15
traceroute6|traceroute6 -n -q 1 ::1
traceroute_6I|traceroute -6 -n -I -q 1 ::1
host6|host testhost6
host6_rev|host ::1
nc6_out|nc -6 -d -w 3 ::1 7779
nc6_dual|nc -d -w 3 127.0.0.1 7781
netstat6|netstat -6 -an
netstat6_s|netstat -s -6
ip6_dns|ip dns
ip6_addr_del|ip -6 addr del 2001:db8:5::15/64 dev eth0
ip6_a3|ip -6 addr show dev eth0
CMDS
[ -n "$SDKTEST" ] && echo "sdk_test|sdk_test" >> "$work/cmds"
{
	echo "FailAt 30"
	echo 'DH0:C/rtcheck START="DH0:C/Riptide >DH0:riptide.log CONFIG=DH0:S/riptide.conf" NOCHECK >DH0:out/start'
	# background sockets for netstat: a TCP listener, a TCP connection to it, and a server for nc
	echo 'DH0:C/rtcheck START="DH0:C/nc -d -l 7777" NOCHECK >NIL:'
	echo 'DH0:C/rtcheck START="DH0:C/nc -l -p 7778 <DH0:S/riptide.conf" NOCHECK >NIL:'
	# IPv6: a server for nc -6, a dual-stack one reached over IPv4, an idle listener
	echo 'DH0:C/rtcheck START="DH0:C/nc -6 -l -p 7779 <DH0:S/riptide.conf" NOCHECK >NIL:'
	echo 'DH0:C/rtcheck START="DH0:C/nc -6 -l -p 7781 <DH0:S/riptide.conf" NOCHECK >NIL:'
	echo 'DH0:C/rtcheck START="DH0:C/nc -6 -d -l 7780" NOCHECK >NIL:'
	while IFS='|' read -r name cmd; do
		[ -n "$name" ] || continue
		if [ "$name" = BG ]; then
			echo "DH0:C/rtcheck START=\"DH0:C/$cmd\" NOCHECK >NIL:"
			continue
		fi
		echo "DH0:C/$cmd >DH0:out/$name"
		echo "DH0:C/Riptide STATUS >NIL:"
	done < "$work/cmds"
	echo "DH0:C/Riptide STATUS >DH0:out/status"
	echo "DH0:C/hostname >DH0:out/zz_done"
} > "$dh0/S/Startup-Sequence"
cat > "$work/test.fs-uae" <<CFG
[fs-uae]
amiga_model = A1200
# 2 MB chip RAM alone is too tight for two copies of Riptide (resident + STATUS)
fast_memory = 8192
hard_drive_0 = $dh0
hard_drive_0_label = DH0
sound_card = none
floppy_drive_count = 0
automatic_input_grab = 0
video_sync = off
uae_sana2 = true
network_card = a2065
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
	[ -s "$dh0/out/zz_done" ] && { sleep 1; break; }
	kill -0 $upid 2>/dev/null || break
	sleep 1
	t=$((t + 1))
done
kill $upid 2>/dev/null
wait $upid 2>/dev/null
[ -n "$xpid" ] && kill "$xpid" 2>/dev/null
for f in "$dh0"/out/*; do
	tr -d '\r' < "$f" > "$f.tmp" && mv "$f.tmp" "$f"
done

fails=0
out() { cat "$dh0/out/$1" 2>/dev/null; }
# expect NAME REGEX...: every extended regex must match some line of the output
expect() {
	n=$1; shift
	for re in "$@"; do
		if ! out "$n" | grep -Eq -- "$re"; then
			echo "FAIL $n: no line matches /$re/"
			echo "----- $n:"; out "$n" | sed 's/^/  | /'
			fails=$((fails + 1))
		fi
	done
}
# reject NAME REGEX: no line may match
reject() {
	if out "$1" | grep -Eq -- "$2"; then
		echo "FAIL $1: unexpected /$2/"; out "$1" | sed 's/^/  | /'
		fails=$((fails + 1))
	fi
}

expect start 'stack ready'
expect ip_a '^1: lo0: <LOOPBACK,MULTICAST,UP,LOWER_UP> mtu 16384 state UNKNOWN' 'inet 127\.0\.0\.1/8 scope host lo0' \
	'^2: eth0: <BROADCAST,MULTICAST,UP' 'link/ether ([0-9a-f]{2}:){5}[0-9a-f]{2} brd ff:ff:ff:ff:ff:ff' \
	'inet 10\.0\.2\.15/24 brd 10\.0\.2\.255 scope global eth0' 'valid_lft forever preferred_lft forever'
reject ip_addr_lo 'eth0'
expect ip_br_a '^lo0 +UNKNOWN +127\.0\.0\.1/8' '^eth0 +(UP|DOWN) +10\.0\.2\.15/24'
expect ip_br_link '^eth0 +(UP|DOWN) +([0-9a-f]{2}:){5}'
expect ip_s_link 'RX: +bytes +packets' 'TX: +bytes +packets' 'driver uaenet\.device unit 0'
expect ip_r '^default via 10\.0\.2\.2 dev eth0 proto static' \
	'^10\.0\.2\.0/24 dev eth0 proto kernel scope link src 10\.0\.2\.15' '^127\.0\.0\.0/8 dev lo0 proto kernel'
expect ip_route_add_dup 'exists'
expect ip_r2 '^default via 10\.0\.2\.3 dev eth0 proto static' \
	'^192\.168\.50\.0/24 via 10\.0\.2\.2 dev eth0 proto static metric 5 mtu 1400'
reject ip_r2 'default via 10\.0\.2\.2'
expect ip_route_get '^8\.8\.8\.8 via 10\.0\.2\.3 dev eth0 src 10\.0\.2\.15'
expect ip_route_get_local '^local 127\.0\.0\.1 dev lo0'
expect route_n '^Kernel IP routing table' '^Destination +Gateway +Genmask +Flags Metric Ref +Use Iface' \
	'^0\.0\.0\.0 +10\.0\.2\.2 +0\.0\.0\.0 +UG +0 +0 +[0-9]+ eth0' '^10\.0\.2\.0 +0\.0\.0\.0 +255\.255\.255\.0 +U +0'
reject route_n '192\.168\.50'
expect route_n2 '^172\.16\.0\.0 +10\.0\.2\.2 +255\.255\.0\.0 +UG ' '^172\.17\.0\.1 +10\.0\.2\.2 +255\.255\.255\.255 +UGH '
expect route_del_none 'SIOCDELRT: No such process'
reject route_n3 '172\.1[67]'
expect arp_an '^\? \(10\.0\.2\.2\) at 52:55:0a:00:02:02 \[ether\] PERM on eth0' '^\? \(10\.0\.2\.4\) at 52:55:0a:00:02:04'
expect arp_n '^Address +HWtype +HWaddress +Flags Mask +Iface' '^10\.0\.2\.2 +ether +52:55:0a:00:02:02 +CM +eth0'
expect ip_n '^10\.0\.2\.2 dev eth0 lladdr 52:55:0a:00:02:02 PERMANENT'
expect arp_d_none 'No ARP entry for 10\.0\.2\.4'
reject ip_n2 '10\.0\.2\.4'
expect ifconfig '^eth0: flags=4163<UP,BROADCAST,RUNNING,MULTICAST>  mtu 1500' \
	'inet 10\.0\.2\.15  netmask 255\.255\.255\.0  broadcast 10\.0\.2\.255' 'ether .* \(Ethernet\)' \
	'^lo0: flags=4169<UP,LOOPBACK,RUNNING,MULTICAST>' 'RX packets [0-9]+  bytes' 'device uaenet\.device unit 0'
expect ifconfig_eth0 'PROMISC' 'mtu 1400'
expect ifconfig_s '^Iface +MTU +RX-OK' '^eth0 +1400 '
expect ip_link_eth0 'mtu 1500'
reject ip_link_eth0 'PROMISC'
expect ip_dns '^nameserver 10\.0\.2\.3' '^search test\.lan'
expect ip_dns2 '^nameserver 10\.0\.2\.53'
expect sysctl_ttl '^net\.inet\.ip\.ttl = 64'
expect sysctl_w '^net\.inet\.ip\.ttl = 32'
expect sysctl_n '^32$'
expect sysctl_a '^kern\.hostname = aros-test' '^net\.inet\.tcp\.'
expect sysctl_bad 'cannot stat no\.such\.var'
expect hostname '^aros-test$'
expect hostname_f '^aros-test\.test\.lan$'
expect hostname_I '10\.0\.2\.15'
expect ping_lo '^PING 127\.0\.0\.1 \(127\.0\.0\.1\) 56\(84\) bytes of data' \
	'^64 bytes from 127\.0\.0\.1: icmp_seq=1 ttl=64 time=' '^3 packets transmitted, 3 received, 0% packet loss' \
	'^rtt min/avg/max/mdev = '
expect ping_amiga '^108 bytes from 127\.0\.0\.1: icmp_seq=2' '2 packets transmitted, 2 received'
expect ping_ttl '^1008 bytes from 127\.0\.0\.5: icmp_seq=1'
expect ping_unreach 'From 10\.0\.2\.15 icmp_seq=1 Destination Host Unreachable' '1 packets transmitted, 0 received, \+1 errors'
expect traceroute '^traceroute to 127\.0\.0\.1 \(127\.0\.0\.1\), 30 hops max, 60 byte packets' \
	'^ 1 +localhost \(127\.0\.0\.1\) +[0-9.]+ ms +[0-9.]+ ms +[0-9.]+ ms'
expect traceroute_n '^ 1 +127\.0\.0\.1 +[0-9.]+ ms +[0-9.]+ ms$'
expect traceroute_I '^ 1 +127\.0\.0\.1 +[0-9.]+ ms$'
expect traceroute_eth '^ 1 +\*' '^ 2 +\*'
expect host_name '^testhost has address 127\.0\.0\.5'
expect host_addr '^5\.0\.0\.127\.in-addr\.arpa domain name pointer testhost\.'
expect host_none 'not found'
expect netstat_an '^Active Internet connections \(servers and established\)' \
	'^Proto Recv-Q Send-Q Local Address +Foreign Address +State' \
	'^tcp +0 +0 0\.0\.0\.0:7778 +0\.0\.0\.0:\* +LISTEN' \
	'^tcp +0 +0 127\.0\.0\.1:7777 +127\.0\.0\.1:[0-9]+ +ESTABLISHED'
expect netstat_l 'LISTEN'
expect netstat_T '^TCP connection details \(w/o servers\)' \
	'^tcp +127\.0\.0\.1:7777 127\.0\.0\.1:[0-9]+ ESTABLISHED' \
	'^  (cubic|reno) (open|disorder) age [0-9.]+m?s  rtt [0-9.]+/[0-9.]+ ms min' \
	'^  opts ts sack ws wscale [0-9]+/[0-9]+ mss [0-9]+/[0-9]+' '^  wnd snd [0-9]+ rcv [0-9]+' \
	'^  rx [0-9]+ bytes, [0-9]+ segs' '^  acks out [0-9]+ \(delayed [0-9]+, dup 0' \
	'^  retrans 0  fast recovery 0  rto 0' '^  time busy [0-9.]+m?s' '^       unread [0-9.]+m?s zero-wnd'
reject netstat_T 'LISTEN'
expect netstat_T6l '^tcp6 :::7780 :::\* LISTEN' '^  syn queue 0  rcvbuf [0-9]+'
reject netstat_T6l '^tcp '
expect sysctl_delack '^net\.inet\.tcp\.delack_segs = 1'
expect sysctl_delack_back '^net\.inet\.tcp\.delack_segs = 2'
reject ip_reads '.'
expect ip_reads_bad 'reads: 4\.\.128'
expect ip_reads_lo 'applies to SANA-II'
reject ip_reads_back '.'
reject netstat_l 'ESTABLISHED'
expect netstat_r '^Kernel IP routing table' '^0\.0\.0\.0 +10\.0\.2\.2 '
expect netstat_i '^Kernel Interface table' '^eth0 +1500 ' '^lo0 '
expect netstat_s '^Ip:' 'total packets received' '^Icmp:' 'echo replies' '^Tcp:' '^Udp:'
expect nc_out '^hostname aros-test'
expect ip_link_after '^eth1 '
reject ip_link_del2 '^eth1 '
expect ip_r_down '^default via 10\.0\.2\.2 dev eth0 proto static linkdown'
expect ip_help 'Usage: ip'
expect ifconfig_bad 'No such device'
expect status 'Routes:' 'default via 10\.0\.2\.2 dev eth0 proto static' 'DNS: 10\.0\.2\.3' \
	'^Libraries: bsdsocket\.library 4, riptide\.library 1, usergroup\.library 4; compat alias: miami\.library 21$'
# IPv6
expect ip6_a '^1: lo0: ' 'inet6 ::1/128 scope host' '^2: eth0: ' 'inet6 fe80::[0-9a-f:]+/64 scope link' \
	'inet6 2001:db8:5::15/64 scope global nodad' 'valid_lft forever preferred_lft forever'
reject ip6_a 'inet 127\.0\.0\.1'
expect ip6_a_lo 'inet 127\.0\.0\.1/8 scope host lo0' 'inet6 ::1/128 scope host'
expect ip6_r '^2001:db8:5::/64 dev eth0 proto kernel metric [0-9]+ pref medium' '^fe80::/64 dev eth0 proto kernel'
expect ip6_r2 '^2001:db8:99::/48 via fe80::1 dev eth0 proto static metric [0-9]+'
expect ip6_route_get '^2001:db8:99::1 via fe80::1 dev eth0 proto static src 2001:db8:5::15 metric'
expect ip6_route_get_lo '^local ::1 dev lo0'
expect ip6_br '^lo0 +UNKNOWN +127\.0\.0\.1/8 ::1/128' '^eth0 +(UP|DOWN) +10\.0\.2\.15/24 fe80::'
expect ifconfig6 'inet6 ::1  prefixlen 128  scopeid 0x10<host>'
expect ifconfig6_eth0 'inet6 fe80::[0-9a-f:]+  prefixlen 64  scopeid 0x20<link>' \
	'inet6 2001:db8:5::15  prefixlen 64  scopeid 0x0<global>'
reject ifconfig6_eth0 '2001:db8:6::16'
expect ping6_lo '^PING ::1\(::1\) 56 data bytes' '^64 bytes from ::1: icmp_seq=1 time=' \
	'^2 packets transmitted, 2 received, 0% packet loss'
expect ping_6 '^64 bytes from ::1: icmp_seq=1' '^1 packets transmitted, 1 received'
expect ping6_eth '^64 bytes from 2001:db8:5::15: icmp_seq=1'
expect traceroute6 '^traceroute6 to ::1 \(::1\), 30 hops max, 80 byte packets' '^ 1 +::1 +[0-9.]+ ms$'
expect traceroute_6I '^ 1 +::1 +[0-9.]+ ms$'
expect host6 '^testhost6 has IPv6 address 2001:db8:5::5'
expect host6_rev '^1(\.0){31}\.ip6\.arpa domain name pointer localhost'
expect nc6_out '^hostname aros-test'
expect nc6_dual '^hostname aros-test'
expect netstat6 '^tcp6 +0 +0 :::7780 +:::\* +LISTEN'
expect netstat6_s '^Ip6:' 'total packets received' '^Icmp6:' 'echo replies'
reject ip6_a3 '2001:db8:5::15'
expect status 'inet6 ::1/128'
[ -n "$SDKTEST" ] && expect sdk_test '^sdk_test: ok$'
if ! tr -d '\r' < "$dh0/riptide.log" 2>/dev/null | grep -Eq 'running: bsdsocket\.library 4, riptide\.library 1, usergroup\.library 4; compat alias: miami\.library 21'; then
	echo "FAIL riptide.log: no startup line naming the libraries"; fails=$((fails + 1))
fi

echo "---- work dir $work (outputs in $dh0/out)"
if [ ! -s "$dh0/out/zz_done" ]; then
	echo "run_fsuae_tools: did not finish (timeout ${TIMEOUT}s)"
	tail -5 "$dh0/riptide.log" 2>/dev/null
	fails=$((fails + 1))
fi
if [ $fails -eq 0 ]; then
	echo "run_fsuae_tools: PASSED"
	exit 0
fi
echo "run_fsuae_tools: FAILED ($fails)"
exit 1
