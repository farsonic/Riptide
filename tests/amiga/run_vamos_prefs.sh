#!/bin/sh
# Riptide - RiptidePrefs without its window under vamos: "FROM file SAVE"
# copies a configuration to ENVARC: and ENV: unchanged, a second SAVE
# keeps the old file as riptide.conf.bak, errors give return code 10.
#   tests/amiga/run_vamos_prefs.sh build/amiga/RiptidePrefs
# SPDX-License-Identifier: GPL-2.0-only
BIN=$1
command -v vamos >/dev/null 2>&1 || { echo "SKIP: vamos not installed"; exit 0; }
d=$(mktemp -d "${TMPDIR:-/tmp}/riptideprefs.XXXXXX")
trap 'rm -rf "$d"' EXIT
mkdir -p "$d/env" "$d/envarc"
printf '# test\r\ninterface eth0 device=uaenet.device unit=0\r\ndhcp eth0\r\nhostname amy  # x\r\nfoo bar' > "$d/in.conf"
fail=0
run() {
	vamos -q -C 68020 -V "ENV:$d/env" -V "ENVARC:$d/envarc" -V "T:$d" "$BIN" "$@" >"$d/out" 2>&1
}
run FROM T:in.conf SAVE; rc=$?
[ $rc -eq 0 ] && cmp -s "$d/in.conf" "$d/envarc/Riptide/riptide.conf" && cmp -s "$d/in.conf" "$d/env/Riptide/riptide.conf" ||
	{ echo "FAIL SAVE: rc $rc, ENVARC:/ENV: riptide.conf not identical to the input"; fail=1; }
run SAVE; rc=$?
[ $rc -eq 0 ] && cmp -s "$d/in.conf" "$d/envarc/Riptide/riptide.conf.bak" ||
	{ echo "FAIL second SAVE: rc $rc, no riptide.conf.bak"; fail=1; }
rm -rf "$d/env/Riptide"
run USE; rc=$?
[ $rc -eq 0 ] && cmp -s "$d/in.conf" "$d/env/Riptide/riptide.conf" ||
	{ echo "FAIL USE: rc $rc, ENV:Riptide/riptide.conf not written from ENVARC:"; fail=1; }
# the Phase 3 lines (addresses, ipv4, qos, sysctls) come back unchanged too
printf 'interface eth0 device=uaenet.device unit=2 qos=on\nifconfig eth0 10.0.0.2/24 up\nifconfig eth0 alias 10.0.1.2/24  # x\nipv4 on\nipv6 off\nqos on\nqos rule port=22,23 priority=interactive\nsysctl net.inet.tcp.cc=ledbat\nsysctl net.inet.tcp.bg_programs=AmiFTP,wget*\nsysctl net.inet.he.prefer=6\n' > "$d/p3.conf"
run FROM T:p3.conf USE; rc=$?
[ $rc -eq 0 ] && cmp -s "$d/p3.conf" "$d/env/Riptide/riptide.conf" ||
	{ echo "FAIL USE of the Phase 3 configuration: rc $rc, not identical"; fail=1; }
run FROM T:missing USE; rc=$?
[ $rc -eq 10 ] && grep -q 'Cannot read T:missing' "$d/out" || { echo "FAIL missing FROM: rc $rc"; cat "$d/out"; fail=1; }
run BADARG=1; rc=$?
[ $rc -eq 10 ] || { echo "FAIL bad argument: rc $rc"; fail=1; }
[ $fail -eq 0 ] && echo "run_vamos_prefs: all passed ($BIN)"
exit $fail
