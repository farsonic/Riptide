# FS-UAE with network bridging (for Riptide)

FS-UAE's `uaenet.device` normally reaches the network only through slirp,
a user-mode NAT that knows no IPv6. `fs-uae-3.2.35-riptide.patch` adds a
libpcap backend to FS-UAE 3.2.35: the emulated Amiga then sits on your
LAN with its own MAC address, gets its IPv4 address from your DHCP server
and its IPv6 addresses from your router, with no slirp and no NAT. The
patch also fixes two `uaenet.device` bugs:

* only the first of the frames waiting at one interrupt reached the Amiga,
  the others were dropped (downloads crawled; Riptide's `tcpwindow=1440`
  works around it on unpatched FS-UAE);
* multicast frames were dropped before the multicast filter, which breaks
  IPv6 neighbour discovery on a real network.

`fs-uae-3.2.35-riptide.patch` is the three of these in one, for building:

* `0001-uaenet-deliver-every-queued-frame.patch`: the frame-drop fix;
* `0002-uaenet-pass-multicast-to-the-multicast-filter.patch`: the
  multicast fix;
* `0003-uaenet-bridge-to-host-interfaces-libpcap.patch`: the libpcap
  backend (`src/uaenet_pcap.cpp`, configure option `--enable-uaenet-pcap`).

They apply to v3.2.35 in that order.
`0004-uaenet-sana2-dma-callbacks.patch` (optional, not in the combined
patch) applies on top: `uaenet.device` then honours the SANA-II R3 DMA
callbacks, which tests Riptide's DMA path (the frame goes straight into
Riptide's buffer, no `CopyToBuff` on the 68k). `issue-1-uaenet-frame-drops.md` and
`issue-2-uaenet-lan-bridge.md` are the reports for FS-UAE's maintainer.

The patch is GPL-2.0 like FS-UAE. It was tested on Linux and on macOS (a
wired interface: DHCP, IPv4, IPv6, 41.6/21.3 Mbit/s speedtest); the build script
supports macOS through Homebrew.

## Build

```
tools/fs-uae/build-fs-uae.sh            # into ~/fs-uae-riptide
```

On macOS it installs the Homebrew packages it needs (autoconf, automake,
libtool, pkg-config, gettext, glib, libpng, sdl2, libmpeg2); libpcap comes
with macOS. It clones FS-UAE 3.2.35, applies the patch and builds
`~/fs-uae-riptide/fs-uae-3.2.35/fs-uae`. On Apple Silicon the JIT is not
available (FS-UAE only has an x86 JIT).

## Permission to capture

Capturing and sending raw frames needs access to the packet filter:

* macOS: `sudo chmod o+rw /dev/bpf*` (until the next reboot), or install
  Wireshark and its "ChmodBPF" helper, which gives the `access_bpf` group
  permanent access.
* Linux: run as root, or `sudo setcap cap_net_raw,cap_net_admin+ep fs-uae`.

## Configure FS-UAE

Find the interface that has your network: `route get default` (macOS) or
`ip route` (Linux) shows it, e.g. `en0`. In your `.fs-uae` file:

```
uae_sana2 = 1
uaenet_pcap = en0
```

`uaenet_pcap` lists the interfaces to offer (comma separated; without it
every interface that is up and has an address). `uaenet.device` units 0
and 1 stay slirp; the bridged interfaces are units 2, 3 and so on. FS-UAE's
log shows the mapping and the Amiga's MAC address:

```
uaenet pcap: uaenet.device unit 2 = en0, Amiga MAC 02:...
```

The MAC is the host interface's address with the locally administered bit
set; `uaenet_pcap_mac = 02:12:34:56:78:9a` picks another one. Start the
patched binary with your configuration:

```
~/fs-uae-riptide/fs-uae-3.2.35/fs-uae ~/Documents/FS-UAE/Configurations/Amiga.fs-uae
```

## Configure Riptide

In `ENVARC:Riptide/riptide.conf`, use unit 2 and DHCP, without
`tcpwindow` (the patched FS-UAE does not drop frames):

```
interface eth0 device=uaenet.device unit=2 mtu=1500
dhcp eth0
```

IPv6 starts by itself when your router sends advertisements. Check with
`Riptide STATUS`, `ip -6 addr`, `ip -6 route`, `ping6 ipv6.google.com`.

## Limits

* **Use a wired interface.** Wi-Fi access points normally accept frames
  only from the MAC address that associated, so a second MAC address (the
  Amiga's) does not work over Wi-Fi.
* **The host and the Amiga cannot reach each other** through the bridge:
  frames the Amiga sends go out on the wire, not into the host's own
  stack, and the host's own frames never reach the Amiga. So `ping`
  from the Mac to the Amiga gets no answer. Ping it from another
  machine, your router or a phone on the same network. If the Mac has a
  second connection to the same network (e.g. Wi-Fi next to the bridged
  Ethernet), it can reach the Amiga through that one:
  `ping -S <the Mac's Wi-Fi address> <Amiga address>`.
* Everything the interface receives for the Amiga's MAC, broadcast and
  multicast goes into the emulator; the capture filter drops the rest.

## Testing

`tests/amiga/run_fsuae_bridge.sh` runs Riptide on a patched FS-UAE bridged
to a Linux veth pair (needs root): IPv4 ping and two 1 MB HTTP downloads
from the host end, and IPv6 address autoconfiguration and `ping6` against
`tests/amiga/v6responder.py`, a minimal IPv6 router for hosts whose kernel
has no IPv6.
