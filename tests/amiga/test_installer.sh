#!/bin/sh
# Riptide - run install/Install_Riptide with InstallerLG on the Linux host.
#
#   tests/amiga/test_installer.sh [PACKAGEDIR]
#
# Needs a Linux build of InstallerLG (Aminet util/sys/InstallerLG.src.lha,
# "make" in its build/ directory) in $INSTALLERLG. Its non-Amiga build
# answers every question with the default, so this exercises the script's
# default paths at each user level against a fake Amiga tree:
#
#   novice    fresh install: files copied, config/hosts written, User-Startup
#   novice    again: the existing config is kept, the block is replaced
#   average   fresh install + pretend mode: nothing may be written
#   expert    with a leftover LIBS:bsdsocket.library (renamed, not deleted)
#             and "another stack in memory" (no start, warning shown)
#   average   another SANA-II device and a static address
#   average   uninstall: programs and docs gone, block emptied, config removed
#
# The script's Amiga paths (C:, S:, ENV:, ENVARC:, LIBS:, SYS:) are mapped to
# directories by rewriting a copy of it; everything else runs unchanged.
# PACKAGEDIR defaults to build/package/Riptide (make -f Makefile.amiga package).
# SPDX-License-Identifier: GPL-2.0-only
set -u
PKG=${1:-build/package/Riptide}
LG=${INSTALLERLG:-}
[ -n "$LG" ] && [ -x "$LG" ] || { echo "SKIP: set INSTALLERLG to a Linux build of InstallerLG"; exit 0; }
[ -f "$PKG/Install_Riptide" ] || { echo "SKIP: $PKG not built (make -f Makefile.amiga package)"; exit 0; }
PKG=$(cd "$PKG" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/riptide-inst.XXXXXX")
fails=0
fail() { echo "FAIL $*"; fails=$((fails + 1)); }

# setup ROOT: a fresh fake Amiga
setup() {
	root=$work/$1
	rm -rf "$root"
	mkdir -p "$root/C" "$root/S" "$root/ENV" "$root/ENVARC" "$root/LIBS" "$root/SYS"
	cp -R "$PKG" "$root/SYS/Pkg"
	printf 'Echo "user startup"\n' > "$root/S/User-Startup"
}

# run NAME LEVEL [sed expressions...]: run the script at user level LEVEL
run() {
	name=$1 level=$2
	shift 2
	script=$root/Install_Riptide.lg   # InstallerLG on Linux wants the script in the current directory
	{
		printf '(set @user-startup "%s/S/User-Startup")\n' "$root"
		printf '(set @default-dest "%s/SYS/")\n' "$root"
		printf '(set @icon "%s/SYS/Pkg/Install_Riptide")\n' "$root"
		printf '(user %s)\n' "$level"
		[ -n "${PRETEND:-}" ] && printf '(set @pretend 1)\n'
		sed -e "s|\"ENVARC:|\"$root/ENVARC/|g" \
		    -e "s|\"ENV:|\"$root/ENV/|g" \
		    -e "s|\"LIBS:|\"$root/LIBS/|g" \
		    -e "s|\"SYS:|\"$root/SYS/|g" \
		    -e "s|\"C:\"|\"$root/C/\"|g" \
		    -e "s|(getassign \"ENVARC\" \"a\")|\"$root/ENVARC\"|" \
		    -e "s|(getassign \"ENV\" \"a\")|\"$root/ENV\"|" \
		    -e "s|(getversion \"exec.library\" (resident))|(* 40 65536)|" \
		    -e "s|(database \"cpu\")|\"68020\"|" \
		    "$@" "$PKG/Install_Riptide"
	} > "$script"
	(cd "$root" && "$LG" Install_Riptide.lg) > "$work/$name.out" 2>&1
	echo $? > "$work/$name.rc"
	if grep -q "^Line [0-9]*:" "$work/$name.out"; then
		fail "$name: $(grep '^Line [0-9]*:' "$work/$name.out" | head -3)"
	fi
}

has() { [ -f "$1" ] || fail "$name: missing $1"; }
hasnt() { [ ! -e "$1" ] || fail "$name: unexpected $1"; }
grepf() { grep -q -- "$2" "$1" 2>/dev/null || fail "$name: $1 lacks /$2/"; }
grepnot() { ! grep -q -- "$2" "$1" 2>/dev/null || fail "$name: $1 has /$2/"; }
TOOLS6="ping6 traceroute6"
TOOLS="ip ifconfig route arp netstat ping traceroute host sysctl hostname nc ftp"

# ---- 1. novice, fresh ------------------------------------------------------
setup novice
run novice 0
for f in Riptide rtcheck $TOOLS $TOOLS6; do
	has "$root/C/$f"
	cmp -s "$root/C/$f" "$PKG/C/$f" || fail "novice: C/$f differs"
done
hasnt "$root/C/ftpd-test"
has "$root/SYS/Riptide/Docs/AMIGA.md"
has "$root/SYS/Riptide/Docs/AMIGA.md.info"
has "$root/SYS/Riptide/Docs/LICENSE"
hasnt "$root/SYS/Riptide/SDK"
conf=$root/ENVARC/Riptide/riptide.conf
has "$conf"
grepf "$conf" '^interface eth0 device=uaenet.device unit=0 mtu=1500$'
grepf "$conf" '^dhcp eth0$'
grepf "$conf" '^hostname amiga$'
grepf "$conf" '^#ifconfig  eth0 10.0.2.15/24 up$'
grepf "$conf" '^#route     add default 10.0.2.2$'
grepf "$conf" '^#nameserver 10.0.2.3$'
grepf "$conf" '^host amiga 127.0.0.1$'
cmp -s "$conf" "$root/ENV/Riptide/riptide.conf" || fail "novice: ENV: copy of riptide.conf differs"
grepf "$root/ENVARC/Riptide/hosts" '^127.0.0.1 localhost$'
has "$root/ENV/Riptide/hosts"
grepf "$root/ENVARC/Riptide/BinDir" "$root/C/"
us=$root/S/User-Startup
grepf "$us" '^Echo "user startup"$'
grepf "$us" '^;BEGIN Riptide$'
grepf "$us" "^Run >NIL: $root/C/Riptide\$"
grepf "$us" '^;END Riptide$'
grepnot "$us" '^Path '
cp "$conf" "$work/novice-riptide.conf"

# ---- 2. novice again: keep the configuration, replace the block ------------
echo "# my own settings" >> "$conf"
run novice2 0
grepf "$conf" '^# my own settings$'
hasnt "$root/ENVARC/Riptide/riptide.conf.bak"
[ "$(grep -c '^;BEGIN Riptide$' "$us")" = 1 ] || fail "novice2: more than one Riptide block"

# ---- 3. average, pretend: nothing may change -----------------------------
setup pretend
PRETEND=1 run pretend 1
hasnt "$root/C/Riptide"
hasnt "$root/ENVARC/Riptide/riptide.conf"
grepnot "$root/S/User-Startup" 'Riptide'

# ---- 4. expert with conflicts ----------------------------------------------
setup expert
printf 'other stack\n' > "$root/LIBS/bsdsocket.library"
run expert 2 -e 's|(getversion "bsdsocket.library" (resident))|1|g'
hasnt "$root/LIBS/bsdsocket.library"
grepf "$root/LIBS/bsdsocket.library.disabled" '^other stack$'
grepf "$work/expert.out" 'Another TCP/IP stack is running'
grepnot "$work/expert.out" 'Start Riptide now'
has "$root/C/Riptide"
# expert default answers: tools + docs, no SDK/ftpd-test
hasnt "$root/C/ftpd-test"

# ---- 5. average: another SANA-II device and a static address -------------
setup static
run static 1 -e 's|^  (set #uae 1)$|  (set #uae 0)|' -e 's|^    (set #uae$|    (set #uae-ignored|' \
	-e 's|^  (set #dhcp 1)$|  (set #dhcp 0)|' -e 's|^    (set #dhcp$|    (set #dhcp-ignored|'
conf=$root/ENVARC/Riptide/riptide.conf
grepf "$conf" '^interface eth0 device=DEVS:Networks/a2065.device unit=0$'
grepf "$conf" '^#dhcp eth0$'
grepf "$conf" '^ifconfig  eth0 10.0.2.15 netmask 255.255.255.0 up$'
grepf "$conf" '^route     add default 10.0.2.2$'
grepf "$conf" '^nameserver 10.0.2.3$'
grepnot "$conf" '^dhcp'

# ---- 6. uninstall (average) ------------------------------------------------
root=$work/novice
# (the host build of InstallerLG answers every askbool with the first choice)
run uninstall 1 -e 's|^(set #mode 1) .*|(set #mode 0)|' -e 's|^  (set #mode$|  (set #mode-ignored|'
for f in Riptide rtcheck $TOOLS $TOOLS6; do hasnt "$root/C/$f"; done
hasnt "$root/SYS/Riptide/Docs/AMIGA.md"
# the host build answers "yes" to "Also delete your configuration?"
hasnt "$root/ENVARC/Riptide/riptide.conf"
hasnt "$root/ENV/Riptide/riptide.conf"
hasnt "$root/ENVARC/Riptide/BinDir"
grepnot "$root/S/User-Startup" 'Run >NIL:'
grepf "$root/S/User-Startup" '^Echo "user startup"$'

if [ $fails -eq 0 ]; then
	echo "ok   Install_Riptide with InstallerLG: novice, novice again, pretend, expert with conflicts, static, uninstall"
	[ -n "${KEEP:-}" ] && echo "outputs in $work" || rm -rf "$work"
	exit 0
fi
echo "$fails failures; outputs in $work"
exit 1
