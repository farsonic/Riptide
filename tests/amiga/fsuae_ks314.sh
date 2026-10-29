#!/bin/sh
# Riptide - run FS-UAE with a real Kickstart 3.1.4 instead of its AROS ROM,
# for test scripts that write their own configuration and boot their own
# disk (no Workbench needed: the tests' Startup-Sequences only use shell
# commands that are in the ROM, plus their own programs).
#
#   FSUAE_PCAP=tests/amiga/fsuae_ks314.sh FSUAE_REAL=path/to/fs-uae \
#     tests/amiga/run_fsuae_prefs_status.sh ...
#   or a directory with this script as "fs-uae" first in PATH
#
# Adds "kickstart_file = $KS314_ROM" to the configuration file it is given
# (unless one is there) and runs FSUAE_REAL (default: the next fs-uae in
# PATH that is not this script) with the same arguments.
# SPDX-License-Identifier: GPL-2.0-only
KS314_ROM=${KS314_ROM:-/root/amiga-os314/rom/kick.a1200.46.143}
[ -f "$KS314_ROM" ] || { echo "fsuae_ks314.sh: no ROM at $KS314_ROM" >&2; exit 1; }
for a in "$@"; do
	case $a in
	*.fs-uae)
		grep -q '^kickstart_file' "$a" || echo "kickstart_file = $KS314_ROM" >>"$a"
		;;
	esac
done
real=${FSUAE_REAL:-}
if [ -z "$real" ]; then
	me=$(readlink -f "$0")
	for d in $(echo "$PATH" | tr ':' ' '); do
		if [ -x "$d/fs-uae" ] && [ "$(readlink -f "$d/fs-uae")" != "$me" ]; then
			real=$d/fs-uae
			break
		fi
	done
fi
[ -n "$real" ] || { echo "fsuae_ks314.sh: no fs-uae found" >&2; exit 1; }
exec "$real" "$@"
