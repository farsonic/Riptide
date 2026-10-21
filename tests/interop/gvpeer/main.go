// gvpeer - an independent IPv6 peer for Riptide's interop tests: gVisor's
// userspace TCP/IP stack (netstack) attached to Riptide's simulated
// Ethernet (AF_UNIX datagrams carrying Ethernet frames, src/host/wire.c).
//
// It plays the router of the link: it sends router advertisements (built
// by hand: netstack only implements the host side of NDP) and owns
// fe80::1 and 2001:db8:1::1/64. Services: TCP echo on 7, TCP discard on 9,
// TCP source of N pattern bytes on 19 (the first 4 bytes received give N,
// big-endian), UDP echo on 7; ICMPv6 echo is answered by netstack itself.
//
// Commands on stdin (one per line), answers on stdout:
//   ra [mtu N] [lifetime S] [noprefix]  send an RA (prefix 2001:db8:1::/64, RDNSS 2001:db8:1::53)
//   connect ADDR PORT BYTES             TCP to Riptide: send BYTES of pattern, half-close,
//                                       expect them echoed: "ok N" / "fail ..."
//   ping ADDR                           ICMPv6 echo to Riptide: "ok" / "fail ..."
//   udp ADDR PORT BYTES                 one datagram to Riptide's UDP echo, expect it back
//   quit
//
// Usage: gvpeer -sock MYPATH -peer RIPTIDEPATH -mac 02:00:00:00:00:01
//
// SPDX-License-Identifier: GPL-2.0-only
package main

import (
	"bufio"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"gvisor.dev/gvisor/pkg/buffer"
	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/adapters/gonet"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/fdbased"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/icmp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
	"gvisor.dev/gvisor/pkg/waiter"
)

var (
	fd      int
	mac     net.HardwareAddr
	llAddr  = netip("fe80::1")
	glAddr  = netip("2001:db8:1::1")
	dnsAddr = netip("2001:db8:1::53")
	st      *stack.Stack
	outMu   sync.Mutex
)

func netip(s string) tcpip.Address {
	ip := net.ParseIP(s).To16()
	return tcpip.AddrFrom16Slice(ip)
}

func say(f string, a ...interface{}) {
	outMu.Lock()
	fmt.Printf(f+"\n", a...)
	os.Stdout.Sync()
	outMu.Unlock()
}

func pattern(i uint32) byte { return byte(i*7 + (i >> 9)) }

// ---- router advertisements, built by hand ----

func csum(b []byte, sum uint32) uint16 {
	for i := 0; i+1 < len(b); i += 2 {
		sum += uint32(b[i])<<8 | uint32(b[i+1])
	}
	if len(b)&1 == 1 {
		sum += uint32(b[len(b)-1]) << 8
	}
	for sum>>16 != 0 {
		sum = sum&0xffff + sum>>16
	}
	return ^uint16(sum)
}

func sendRA(mtu uint32, lifetime uint16, prefix bool) {
	var m []byte
	m = append(m, 134, 0, 0, 0, 64, 0)
	m = binary.BigEndian.AppendUint16(m, lifetime)
	m = append(m, 0, 0, 0, 0, 0, 0, 0, 0) // reachable, retrans
	m = append(m, 1, 1)                   // SLLA
	m = append(m, mac...)
	if prefix {
		m = append(m, 3, 4, 64, 0xc0)
		m = binary.BigEndian.AppendUint32(m, 3600)
		m = binary.BigEndian.AppendUint32(m, 1800)
		m = append(m, 0, 0, 0, 0)
		p := netip("2001:db8:1::").As16()
		m = append(m, p[:]...)
	}
	if mtu != 0 {
		m = append(m, 5, 1, 0, 0)
		m = binary.BigEndian.AppendUint32(m, mtu)
	}
	m = append(m, 25, 3, 0, 0)
	m = binary.BigEndian.AppendUint32(m, 600)
	d := dnsAddr.As16()
	m = append(m, d[:]...)
	// pseudo header sum
	src, dst := llAddr.As16(), netip("ff02::1").As16()
	var ph []byte
	ph = append(ph, src[:]...)
	ph = append(ph, dst[:]...)
	ph = binary.BigEndian.AppendUint32(ph, uint32(len(m)))
	ph = append(ph, 0, 0, 0, 58)
	var s uint32
	for i := 0; i < len(ph); i += 2 {
		s += uint32(ph[i])<<8 | uint32(ph[i+1])
	}
	binary.BigEndian.PutUint16(m[2:], csum(m, s))
	frame := []byte{0x33, 0x33, 0, 0, 0, 1}
	frame = append(frame, mac...)
	frame = append(frame, 0x86, 0xdd, 0x60, 0, 0, 0)
	frame = binary.BigEndian.AppendUint16(frame, uint16(len(m)))
	frame = append(frame, 58, 255)
	frame = append(frame, src[:]...)
	frame = append(frame, dst[:]...)
	frame = append(frame, m...)
	if _, err := syscall.Write(fd, frame); err != nil {
		say("fail ra %v", err)
		return
	}
	say("ok ra")
}

// ---- services ----

func serveTCP(port uint16, h func(c net.Conn)) {
	l, err := gonet.ListenTCP(st, tcpip.FullAddress{NIC: 1, Port: port}, ipv6.ProtocolNumber)
	if err != nil {
		say("fail listen %d: %v", port, err)
		os.Exit(1)
	}
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			go h(c)
		}
	}()
}

func echo(c net.Conn) {
	defer c.Close()
	io.Copy(c, c)
}

func discard(c net.Conn) {
	defer c.Close()
	io.Copy(io.Discard, c)
}

func source(c net.Conn) {
	defer c.Close()
	var hdr [4]byte
	if _, err := io.ReadFull(c, hdr[:]); err != nil {
		return
	}
	n := binary.BigEndian.Uint32(hdr[:])
	buf := make([]byte, 16384)
	for i := uint32(0); i < n; {
		k := uint32(len(buf))
		if n-i < k {
			k = n - i
		}
		for j := uint32(0); j < k; j++ {
			buf[j] = pattern(i + j)
		}
		if _, err := c.Write(buf[:k]); err != nil {
			return
		}
		i += k
	}
}

func udpEcho() {
	c, err := gonet.DialUDP(st, &tcpip.FullAddress{NIC: 1, Port: 7}, nil, ipv6.ProtocolNumber)
	if err != nil {
		say("fail udp %v", err)
		os.Exit(1)
	}
	go func() {
		buf := make([]byte, 65536)
		for {
			n, a, err := c.ReadFrom(buf)
			if err != nil {
				return
			}
			c.WriteTo(buf[:n], a)
		}
	}()
}

func parseFull(addr string, port uint16) tcpip.FullAddress {
	zone := ""
	if i := strings.IndexByte(addr, '%'); i >= 0 {
		zone = addr[i+1:]
		addr = addr[:i]
	}
	_ = zone
	return tcpip.FullAddress{NIC: 1, Addr: netip(addr), Port: port}
}

func cmdConnect(addr string, port uint16, n uint32) {
	c, err := gonet.DialTCP(st, parseFull(addr, port), ipv6.ProtocolNumber)
	if err != nil {
		say("fail connect %v", err)
		return
	}
	defer c.Close()
	done := make(chan error, 1)
	go func() {
		buf := make([]byte, 16384)
		for i := uint32(0); i < n; {
			k := uint32(len(buf))
			if n-i < k {
				k = n - i
			}
			for j := uint32(0); j < k; j++ {
				buf[j] = pattern(i + j)
			}
			if _, err := c.Write(buf[:k]); err != nil {
				done <- err
				return
			}
			i += k
		}
		c.CloseWrite()
		done <- nil
	}()
	buf := make([]byte, 16384)
	got := uint32(0)
	c.SetReadDeadline(time.Now().Add(60 * time.Second))
	for {
		k, err := c.Read(buf)
		for j := 0; j < k; j++ {
			if buf[j] != pattern(got+uint32(j)) {
				say("fail data at %d", got+uint32(j))
				return
			}
		}
		got += uint32(k)
		if err != nil {
			break
		}
	}
	if err := <-done; err != nil {
		say("fail write %v", err)
		return
	}
	if got != n {
		say("fail got %d of %d", got, n)
		return
	}
	say("ok %d", got)
}

func cmdPing(addr string) {
	var wq waiter.Queue
	ep, terr := st.NewEndpoint(icmp.ProtocolNumber6, ipv6.ProtocolNumber, &wq)
	if terr != nil {
		say("fail ping %v", terr)
		return
	}
	defer ep.Close()
	req := make([]byte, header.ICMPv6EchoMinimumSize+32)
	req[0] = byte(header.ICMPv6EchoRequest)
	binary.BigEndian.PutUint16(req[6:], 7)
	for i := 8; i < len(req); i++ {
		req[i] = byte(i)
	}
	to := parseFull(addr, 0)
	var r bytesReader
	r.b = req
	if _, err := ep.Write(&r, tcpip.WriteOptions{To: &to}); err != nil {
		say("fail ping write %v", err)
		return
	}
	we, ch := waiter.NewChannelEntry(waiter.ReadableEvents)
	wq.EventRegister(&we)
	defer wq.EventUnregister(&we)
	deadline := time.After(5 * time.Second)
	for {
		var w bytesWriter
		_, err := ep.Read(&w, tcpip.ReadOptions{})
		if err == nil {
			if len(w.b) >= 8 && w.b[0] == byte(header.ICMPv6EchoReply) {
				say("ok")
				return
			}
			continue
		}
		select {
		case <-ch:
		case <-deadline:
			say("fail ping timeout")
			return
		}
	}
}

func cmdUDP(addr string, port uint16, n int) {
	to := parseFull(addr, port)
	c, err := gonet.DialUDP(st, &tcpip.FullAddress{NIC: 1}, &to, ipv6.ProtocolNumber)
	if err != nil {
		say("fail udp %v", err)
		return
	}
	defer c.Close()
	out := make([]byte, n)
	for i := range out {
		out[i] = pattern(uint32(i))
	}
	c.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := c.Write(out); err != nil {
		say("fail udp write %v", err)
		return
	}
	in := make([]byte, n+100)
	k, err := c.Read(in)
	if err != nil || k != n || string(in[:k]) != string(out) {
		say("fail udp read %d %v", k, err)
		return
	}
	say("ok %d", k)
}

type bytesReader struct{ b []byte }

func (r *bytesReader) Read(p []byte) (int, error) {
	if len(r.b) == 0 {
		return 0, io.EOF
	}
	n := copy(p, r.b)
	r.b = r.b[n:]
	return n, nil
}
func (r *bytesReader) Len() int { return len(r.b) }

type bytesWriter struct{ b []byte }

func (w *bytesWriter) Write(p []byte) (int, error) { w.b = append(w.b, p...); return len(p), nil }

func main() {
	sock := flag.String("sock", "", "my AF_UNIX datagram path")
	peer := flag.String("peer", "", "Riptide's AF_UNIX datagram path")
	macs := flag.String("mac", "02:00:00:00:00:01", "my MAC address")
	mtu := flag.Uint("mtu", 1500, "link MTU")
	flag.Parse()
	var err error
	if mac, err = net.ParseMAC(*macs); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	fd, err = syscall.Socket(syscall.AF_UNIX, syscall.SOCK_DGRAM, 0)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	os.Remove(*sock)
	if err := syscall.Bind(fd, &syscall.SockaddrUnix{Name: *sock}); err != nil {
		fmt.Fprintln(os.Stderr, "bind", err)
		os.Exit(1)
	}
	syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_RCVBUF, 4<<20)
	syscall.SetsockoptInt(fd, syscall.SOL_SOCKET, syscall.SO_SNDBUF, 4<<20)
	// the peer socket may not exist yet: retry
	for i := 0; ; i++ {
		if err = syscall.Connect(fd, &syscall.SockaddrUnix{Name: *peer}); err == nil {
			break
		}
		if i > 200 {
			fmt.Fprintln(os.Stderr, "connect", err)
			os.Exit(1)
		}
		time.Sleep(50 * time.Millisecond)
	}
	st = stack.New(stack.Options{
		NetworkProtocols: []stack.NetworkProtocolFactory{ipv6.NewProtocolWithOptions(ipv6.Options{
			DADConfigs: stack.DADConfigurations{DupAddrDetectTransmits: 0},
		})},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol, udp.NewProtocol, icmp.NewProtocol6},
	})
	{
		opt := tcpip.TCPSACKEnabled(true)
		st.SetTransportProtocolOption(tcp.ProtocolNumber, &opt)
	}
	ep, err := fdbased.New(&fdbased.Options{
		FDs:                []int{fd},
		MTU:                uint32(*mtu),
		EthernetHeader:     true,
		Address:            tcpip.LinkAddress(mac),
		PacketDispatchMode: fdbased.RecvMMsg,
	})
	if err != nil {
		fmt.Fprintln(os.Stderr, "fdbased", err)
		os.Exit(1)
	}
	if terr := st.CreateNIC(1, ep); terr != nil {
		fmt.Fprintln(os.Stderr, "nic", terr)
		os.Exit(1)
	}
	for _, a := range []struct {
		addr tcpip.Address
		plen int
	}{{llAddr, 64}, {glAddr, 64}} {
		pa := tcpip.ProtocolAddress{Protocol: ipv6.ProtocolNumber,
			AddressWithPrefix: tcpip.AddressWithPrefix{Address: a.addr, PrefixLen: a.plen}}
		if terr := st.AddProtocolAddress(1, pa, stack.AddressProperties{}); terr != nil {
			fmt.Fprintln(os.Stderr, "addr", terr)
			os.Exit(1)
		}
	}
	st.SetRouteTable([]tcpip.Route{
		{Destination: tcpip.AddressWithPrefix{Address: netip("fe80::"), PrefixLen: 64}.Subnet(), NIC: 1},
		{Destination: tcpip.AddressWithPrefix{Address: netip("2001:db8:1::"), PrefixLen: 64}.Subnet(), NIC: 1},
	})
	serveTCP(7, echo)
	serveTCP(9, discard)
	serveTCP(19, source)
	udpEcho()
	_ = buffer.Buffer{}
	say("ready")

	in := bufio.NewScanner(os.Stdin)
	for in.Scan() {
		f := strings.Fields(in.Text())
		if len(f) == 0 {
			continue
		}
		switch f[0] {
		case "ra":
			var m uint64
			lt := uint64(1800)
			prefix := true
			for i := 1; i < len(f); i++ {
				switch f[i] {
				case "mtu":
					i++
					m, _ = strconv.ParseUint(f[i], 10, 32)
				case "lifetime":
					i++
					lt, _ = strconv.ParseUint(f[i], 10, 16)
				case "noprefix":
					prefix = false
				}
			}
			sendRA(uint32(m), uint16(lt), prefix)
		case "connect":
			p, _ := strconv.Atoi(f[2])
			n, _ := strconv.ParseUint(f[3], 10, 32)
			cmdConnect(f[1], uint16(p), uint32(n))
		case "ping":
			cmdPing(f[1])
		case "udp":
			p, _ := strconv.Atoi(f[2])
			n, _ := strconv.Atoi(f[3])
			cmdUDP(f[1], uint16(p), n)
		case "quit":
			say("bye")
			return
		default:
			say("fail unknown command")
		}
	}
}
