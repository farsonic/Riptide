#!/bin/sh
# Riptide - check the .info files of the Amiga package.
#
#   tests/amiga/check_icons.sh [PACKAGEDIR]
#
# 1. The struct layout tools/mkicon.py writes (DiskObject 78 bytes, Gadget 44,
#    OldDrawerData 56, Image 20, NewWindow 48 and the field offsets) is
#    compared with the NDK's workbench/workbench.h, compiled for m68k
#    (m68k-linux-gnu-gcc, 2-byte alignment like the Amiga compilers).
# 2. Every .info file of the package is parsed back (tools/mkicon.py dump):
#    magic, version, image sizes, default tool, tooltypes, drawer data and
#    no trailing bytes.
# SPDX-License-Identifier: GPL-2.0-only
set -u
PKG=${1:-build/package}
NDK=${NDK:-/opt/ndk3.2}
CC=${CROSS:-m68k-linux-gnu-}gcc
fails=0
if command -v "$CC" >/dev/null 2>&1 && [ -f "$NDK/Include_H/workbench/workbench.h" ]; then
	tmp=$(mktemp "${TMPDIR:-/tmp}/riptide-icons.XXXXXX.c")
	cat > "$tmp" <<'C'
#include <exec/types.h>
#include <workbench/workbench.h>
#include <stddef.h>
_Static_assert(sizeof(struct DiskObject) == 78, "DiskObject");
_Static_assert(sizeof(struct Gadget) == 44, "Gadget");
_Static_assert(sizeof(struct OldDrawerData) == 56, "OldDrawerData");
_Static_assert(sizeof(struct NewWindow) == 48, "NewWindow");
_Static_assert(sizeof(struct Image) == 20, "Image");
_Static_assert(offsetof(struct DiskObject, do_Gadget) == 4, "do_Gadget");
_Static_assert(offsetof(struct DiskObject, do_Type) == 48, "do_Type");
_Static_assert(offsetof(struct DiskObject, do_DefaultTool) == 50, "do_DefaultTool");
_Static_assert(offsetof(struct DiskObject, do_CurrentX) == 58, "do_CurrentX");
_Static_assert(offsetof(struct DiskObject, do_DrawerData) == 66, "do_DrawerData");
_Static_assert(offsetof(struct DiskObject, do_StackSize) == 74, "do_StackSize");
_Static_assert(offsetof(struct Gadget, GadgetRender) == 18, "GadgetRender");
_Static_assert(offsetof(struct Gadget, UserData) == 40, "UserData");
C
	if "$CC" -fsyntax-only -I"$NDK/Include_H" "$tmp"; then
		echo "ok   icon struct layout matches the NDK's workbench.h"
	else
		echo "FAIL icon struct layout differs from the NDK"; fails=$((fails + 1))
	fi
	rm -f "$tmp"
else
	echo "skip icon layout check (needs $CC and the NDK)"
fi
n=0
for f in $(find "$PKG" -name '*.info' | sort); do
	if python3 tools/mkicon.py dump "$f" > /dev/null; then
		n=$((n + 1))
	else
		echo "FAIL $f does not parse"; fails=$((fails + 1))
	fi
done
[ $n -gt 0 ] || { echo "SKIP: no .info files in $PKG (make -f Makefile.amiga package)"; exit 0; }
[ $fails -eq 0 ] && echo "ok   $n icons parse" && exit 0
exit 1
