**Title:** uaenet.device: bridge to a host network interface (libpcap backend) for direct LAN access and IPv6

### What it adds

FS-UAE's `ethernet.cpp` already has the hooks for a pcap backend
(`#ifdef WITH_UAENET_PCAP`, `uaenet_enumerate()`, `uaenet_open()`,
`uaenet_trigger()` ...), but only WinUAE has an implementation
(`od-win32/win32_uaenet.cpp`). This adds a portable one based on libpcap
for macOS, Linux and the BSDs. With it `uaenet.device` (and the A2065)
can be bridged to a host Ethernet interface: the emulated Amiga is on the
LAN with its own MAC address, gets its IPv4 address from the LAN's DHCP
server and its IPv6 addresses from the router's advertisements, with no
slirp and no NAT. slirp has no IPv6, so this is also the only way to use
IPv6 from an emulated Amiga today.

### Configuration

```
uae_sana2 = 1
uaenet_pcap = en0                 # interfaces to offer, comma separated
uaenet_pcap_mac = 02:...          # optional; default: the host interface's
                                  # MAC with the locally administered bit set
```

`uaenet.device` units 0 and 1 stay slirp and slirp_inbound; the bridged
interfaces are units 2, 3, ... and the log says which is which
(`uaenet pcap: uaenet.device unit 2 = en0, Amiga MAC 02:...`). For the
A2065: `uae_a2065 = en0`.

### Implementation (`src/uaenet_pcap.cpp`, about 350 lines)

* `pcap_create()` with immediate mode (otherwise macOS BPF holds received
  frames back until its buffer fills), promiscuous, inbound direction
  only, and a capture filter `ether dst <Amiga MAC> or ether multicast`,
  so only frames the Amiga would receive reach the emulator.
* A reader thread hands frames to `gotfunc()`; `uaenet_trigger()` sends
  what `getfunc()` returns with `pcap_inject()`.
* The Amiga's own frames are ignored where the direction filter is
  missing.
* `configure --enable-uaenet-pcap` (the default when libpcap is found)
  defines `WITH_UAENET_PCAP` and links `-lpcap`.

### One more sana2.cpp change needed for IPv6

`uaenet_gotdata()` drops every frame that is not broadcast and not
addressed to the Amiga's MAC before its own "drop if multicast with
unknown address" check runs, so multicast never reaches the multicast
filter, and IPv6 neighbour discovery (router advertisements, solicited
node addresses) fails on a real network:

```diff
-	/* drop if not promiscuous and dst != broadcast and dst != me */
-	if (!dev->promiscuous && !isbroadcast (d) && memcmp (d, dev->td->mac, ADDR_SIZE))
+	/* drop if not promiscuous and dst != broadcast and dst != me; multicast
+	 * goes on to handleread(), which filters by the S2_ADDMULTICASTADDRESS
+	 * ranges (IPv6 neighbour discovery needs it) */
+	if (!dev->promiscuous && !isbroadcast (d) && !ismulticast (d) && memcmp (d, dev->td->mac, ADDR_SIZE))
```

### Tested

* **macOS, FS-UAE 3.2.35 with the patches, wired Ethernet (en15),
  emulated 68030 with the Riptide TCP/IP stack:**
  * IPv4 address by DHCP from the LAN's router.
  * Ping to the router (1.2 ms) and to 1.1.1.1 (3.5 ms).
  * speedtest.net: 41.58 Mbit/s down, 21.29 Mbit/s up.
  * IPv6 addresses and the default route from the router's
    advertisements, plus a route learned from a second router on the
    LAN.
  * `ping6` to the router's link-local address.
  * A multicast `ping6 ff02::1` answered by 15 devices on the LAN.
* **Linux, over a veth pair:**
  * Ping.
  * Two 1 MB HTTP downloads with no retransmissions and nothing out of
    order.
  * IPv6 address autoconfiguration and `ping6` against a test router.

### Limits

* **Wired interfaces only, in practice.** Over Wi-Fi the access point
  delivers no unicast frames to a second MAC address: ARP replies never
  reach the Amiga (seen on macOS).
* **The host and the guest cannot reach each other** through the bridge:
  injected frames go out on the wire, not into the host's stack.
* **Capturing needs privileges:** root, read/write access to
  `/dev/bpf*` on macOS (e.g. Wireshark's ChmodBPF), or CAP_NET_RAW and
  CAP_NET_ADMIN on Linux.

### Patches (against v3.2.35)

* Multicast fix:
  https://github.com/farsonic/Riptide/blob/main/tools/fs-uae/0002-uaenet-pass-multicast-to-the-multicast-filter.patch
* libpcap backend (`configure.ac`, `Makefile.am`, `src/ethernet.cpp`,
  `src/uaenet_pcap.cpp`, `src/include/uaenet_pcap.h`):
  https://github.com/farsonic/Riptide/blob/main/tools/fs-uae/0003-uaenet-bridge-to-host-interfaces-libpcap.patch
* All three, including the frame-drop fix from issue #&lt;number of the
  first issue&gt;:
  https://github.com/farsonic/Riptide/blob/main/tools/fs-uae/fs-uae-3.2.35-riptide.patch

They apply in order 0001, 0002, 0003. I am happy to turn them into a pull
request against whichever branch you prefer. The current master has the
WinUAE-derived `sana2.cpp` and its own `ethernet.cpp`, so the backend
would need a small adaptation there.
