#!/bin/sh
# Riptide - run every test binary in <builddir>/tests.
#   exit 0 = pass, 77 = skip, anything else = fail.
# Usage: tests/run_tests.sh <builddir> [name-filter]
# Environment:
#   RUNNER        command prefix (default: qemu-m68k for m68k build dirs)
#   TEST_TIMEOUT  seconds per test (default 300)
# SPDX-License-Identifier: GPL-2.0-only

BUILD=${1:?usage: $0 <builddir> [filter]}
FILTER=${2:-}
TIMEOUT=${TEST_TIMEOUT:-300}

if [ -z "${RUNNER+x}" ]; then
	case "$BUILD" in
	*m68k*) RUNNER=qemu-m68k ;;
	*) RUNNER= ;;
	esac
fi

pass=0; fail=0; skip=0; failed=""
for t in "$BUILD"/tests/*; do
	[ -f "$t" ] && [ -x "$t" ] || continue
	name=$(basename "$t")
	case "$name" in *.*) continue ;; esac
	if [ -n "$FILTER" ]; then
		case "$name" in *$FILTER*) ;; *) continue ;; esac
	fi
	log="$BUILD/tests/$name.log"
	start=$(date +%s)
	timeout "$TIMEOUT" $RUNNER "$t" >"$log" 2>&1
	rc=$?
	secs=$(( $(date +%s) - start ))
	case $rc in
	0)  pass=$((pass + 1)); printf 'PASS  %-28s %3ss\n' "$name" "$secs" ;;
	77) skip=$((skip + 1)); printf 'SKIP  %-28s %s\n' "$name" "$(tail -n 1 "$log")" ;;
	*)  fail=$((fail + 1)); failed="$failed $name"
	    [ $rc -eq 124 ] && echo "TIMEOUT after ${TIMEOUT}s" >>"$log"
	    printf 'FAIL  %-28s rc=%d (log: %s)\n' "$name" "$rc" "$log"
	    tail -n 15 "$log" | sed 's/^/      | /' ;;
	esac
done

echo "----"
echo "$pass passed, $fail failed, $skip skipped${RUNNER:+ (runner: $RUNNER)}"
[ $fail -eq 0 ] || { echo "failed:$failed"; exit 1; }
exit 0
