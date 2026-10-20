#!/bin/sh
# Riptide - run "Riptide SELFTEST" (and the CLI error paths) under vamos.
#   tests/amiga/run_vamos.sh build/amiga/Riptide
# Needs amitools' vamos with machine68k 0.3.0 (pip install machine68k==0.3.0).
# SPDX-License-Identifier: GPL-2.0-only
set -u
BIN=${1:-build/amiga/Riptide}
command -v vamos >/dev/null 2>&1 || { echo "SKIP: vamos not installed"; exit 0; }
[ -f "$BIN" ] || { echo "SKIP: $BIN not built"; exit 0; }
fail=0
out=$(vamos -q -C 68020 -s 64 "$BIN" SELFTEST 2>&1); rc=$?
echo "$out"
echo "$out" | grep -q "SELFTEST PASSED" && [ $rc -eq 0 ] || { echo "FAIL: selftest (rc $rc)"; fail=1; }
out=$(vamos -q -C 68020 "$BIN" STATUS 2>&1); rc=$?
echo "$out" | grep -q "Riptide is not running" && [ $rc -eq 5 ] || { echo "FAIL: STATUS without a running stack (rc $rc): $out"; fail=1; }
out=$(vamos -q -C 68020 "$BIN" BADARG 2>&1); rc=$?
[ $rc -ne 0 ] || { echo "FAIL: bad argument accepted"; fail=1; }
[ $fail -eq 0 ] && echo "run_vamos: all passed"
exit $fail
