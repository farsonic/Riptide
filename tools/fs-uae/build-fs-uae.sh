#!/bin/sh
# Riptide - build FS-UAE 3.2.35 with tools/fs-uae/fs-uae-3.2.35-riptide.patch:
#   * uaenet.device (and the A2065) bridged to a host network interface
#     through libpcap: the Amiga is on your LAN with its own MAC address,
#     IPv4 from your DHCP server, IPv6 from your router, no slirp, no NAT
#   * uaenet.device passes every waiting frame to the Amiga (3.2.35 drops
#     all but the first one per interrupt)
#   * uaenet.device lets multicast through to its multicast filter (IPv6
#     neighbour discovery needs it)
#
#   tools/fs-uae/build-fs-uae.sh [WORKDIR]      (default: ~/fs-uae-riptide)
#
# macOS: needs Homebrew (https://brew.sh) and the Xcode command line tools
# (xcode-select --install); the script installs the Homebrew packages.
# Linux: install the build dependencies first, e.g. on Debian/Ubuntu
#   apt install build-essential autoconf automake libtool pkg-config gettext \
#       zip libglib2.0-dev libsdl2-dev libpng-dev libopenal-dev libmpeg2-4-dev \
#       libpcap-dev
# The result is WORKDIR/fs-uae-3.2.35/fs-uae; see tools/fs-uae/README.md.
# SPDX-License-Identifier: GPL-2.0-only
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PATCH=$HERE/fs-uae-3.2.35-riptide.patch
VERSION=3.2.35
WORK=${1:-$HOME/fs-uae-riptide}

if [ "$(uname)" = Darwin ]; then
	command -v brew >/dev/null 2>&1 || { echo "Install Homebrew first: https://brew.sh"; exit 1; }
	xcode-select -p >/dev/null 2>&1 || { echo "Install the Xcode command line tools: xcode-select --install"; exit 1; }
	brew install autoconf automake libtool pkg-config gettext glib libpng sdl2 libmpeg2
	BREW=$(brew --prefix)
	# gettext is keg-only (msgfmt); aclocal needs pkg-config's pkg.m4
	PATH="$(brew --prefix gettext)/bin:$PATH"
	ACLOCAL_PATH="$BREW/share/aclocal${ACLOCAL_PATH:+:$ACLOCAL_PATH}"
	PKG_CONFIG_PATH="$BREW/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
	export PATH ACLOCAL_PATH PKG_CONFIG_PATH
	JOBS=$(sysctl -n hw.ncpu)
else
	JOBS=$(nproc 2>/dev/null || echo 4)
fi

mkdir -p "$WORK"
cd "$WORK"
if [ ! -d "fs-uae-$VERSION" ]; then
	git clone --depth 1 --branch "v$VERSION" https://github.com/FrodeSolheim/fs-uae.git "fs-uae-$VERSION"
fi
cd "fs-uae-$VERSION"
if git apply --reverse --check "$PATCH" >/dev/null 2>&1; then
	echo "Patch already applied."
else
	git apply "$PATCH"
fi
./bootstrap
./configure --enable-uaenet-pcap
make -j"$JOBS"
echo
echo "Built: $PWD/fs-uae"
grep -q "define WITH_UAENET_PCAP 1" config.h && echo "uaenet pcap bridging: enabled"
