#!/bin/sh
# Riptide - assemble the Amiga distribution package.
#
#   tools/mkpackage.sh BUILDDIR OUTDIR VERSION
#
# Builds OUTDIR/package/Riptide/ (+ Riptide.info):
#
#   Install_Riptide(.info)   Installer script and its icon
#   ReadMe(.info)            short instructions
#   Riptide.readme           Aminet readme
#   RiptidePrefs(.info)      the preferences editor for riptide.conf
#   C/                       Riptide, rtcheck, the network tools, ftpd-test
#   Docs/(.info)             AMIGA.md, COMPARISON.md, README.md, LICENSE,
#                            riptide-uaenet.conf (each with an icon)
#   SDK/                     sdk/ (headers and FD file for riptide.library)
#
# and from it
#
#   OUTDIR/riptide-VERSION.lha     with an LHA archiver that can write:
#                                  "LHa for UNIX" (lha) or jlha (Debian/Ubuntu
#                                  jlha-utils); LHA=... picks one. Otherwise a
#                                  .zip is made instead.
#   OUTDIR/riptide-VERSION.readme  the Aminet readme next to the archive
#   OUTDIR/riptide-VERSION.adf     an 880 KB FFS floppy image with the package
#                                  (needs amitools' xdftool), for emulators
#
# SPDX-License-Identifier: GPL-2.0-only
set -eu
BUILD=${1:?BUILDDIR}
OUT=${2:?OUTDIR}
VERSION=${3:?VERSION}
PYTHON=${PYTHON:-python3}
TOOLS="ip ifconfig route arp netstat ping traceroute host sysctl hostname nc ftp speedtest"
top=$(cd "$(dirname "$0")/.." && pwd)
TOOLS="$TOOLS ping6 traceroute6"      # IPv6 variants of ping and traceroute
TOOLS="$TOOLS rtdiag"                 # diagnostics for bug reports
TOOLS="$TOOLS stackprobe"             # compares TCP/IP stacks (any stack, 68000)
mkicon="$PYTHON $top/tools/mkicon.py"

for f in Riptide rtcheck $TOOLS ftpd-test RiptidePrefs; do
	[ -f "$BUILD/$f" ] || { echo "mkpackage: $BUILD/$f is missing (build first)" >&2; exit 1; }
done

stage=$OUT/package
pkg=$stage/Riptide
rm -rf "$stage"
mkdir -p "$pkg/C" "$pkg/Docs" "$pkg/SDK"

# programs
for f in Riptide rtcheck $TOOLS ftpd-test; do
	cp "$BUILD/$f" "$pkg/C/$f"
	chmod 755 "$pkg/C/$f"
done

cp "$BUILD/RiptidePrefs" "$pkg/RiptidePrefs"
chmod 755 "$pkg/RiptidePrefs"

# installer, readmes
cp "$top/install/Install_Riptide" "$pkg/Install_Riptide"
sed "s/@VERSION@/$VERSION/g" "$top/install/ReadMe" > "$pkg/ReadMe"
# the Aminet readme: the release notes (dist/RELEASE-NOTES-vVERSION.md, not
# in git) fill its Changes section when they exist; lines stay under 78
# columns, LF only
changes="$top/dist/RELEASE-NOTES-v$VERSION.md"
if [ -f "$changes" ]; then
	# drop the title and the "unpack and run" paragraph; headings become
	# plain lines, their bullets are indented, markup and non-ASCII go
	awk 'NR > 5 && !/^Details: /' "$changes" | sed -e 's/\*\*//g' -e 's/`//g' -e 's/\xe2\x86\x92/->/g' \
	    -e 's/^## \(.*\)/\1:/' -e 's/^\* /  * /' -e 's/^  \([^ *]\)/    \1/' | cat -s | sed -e '/./,$!d' | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' \
	    > "$stage/changes.txt"
else
	echo "See https://github.com/farsonic/riptide/releases" > "$stage/changes.txt"
fi
underline=$(printf 'Changes in %s' "$VERSION" | sed 's/./-/g')
sed -e "s/@VERSION@/$VERSION/g" -e "s/^@UNDERLINE@$/$underline/" -e "/@CHANGES@/{r $stage/changes.txt
d}" "$top/install/Riptide.readme" > "$pkg/Riptide.readme"
if LC_ALL=C grep -q '[^ -~]' "$pkg/Riptide.readme"; then
	echo "mkpackage: Riptide.readme has non-ASCII characters" >&2; exit 1
fi
rm -f "$stage/changes.txt"
if awk 'length > 78 { bad = 1 } END { exit bad }' "$pkg/Riptide.readme"; then :; else
	echo "mkpackage: Riptide.readme has a line over 78 columns" >&2; exit 1
fi

# documentation
cp "$top/docs/AMIGA.md" "$top/docs/COMPARISON.md" "$top/README.md" "$top/LICENSE" \
   "$top/examples/riptide-uaenet.conf" "$pkg/Docs/"
cp -R "$top/sdk/." "$pkg/SDK/"

# icons (positions inside the drawer windows)
$mkicon drawer  "$stage/Riptide.info" --image drawer --window 60,40,420,140
$mkicon project "$pkg/Install_Riptide.info" --image install --tool Installer --stack 20000 --pos 20,10 \
	--tooltype APPNAME=Riptide --tooltype MINUSER=NOVICE --tooltype DEFUSER=AVERAGE
$mkicon project "$pkg/ReadMe.info" --image doc --tool SYS:Utilities/MultiView --pos 150,10
$mkicon drawer  "$pkg/Docs.info" --image docs --window 80,60,420,120 --pos 230,10
$mkicon tool    "$pkg/RiptidePrefs.info" --image prefs --stack 16384 --pos 310,10
x=10 y=8
for f in AMIGA.md COMPARISON.md README.md LICENSE riptide-uaenet.conf; do
	$mkicon project "$pkg/Docs/$f.info" --image doc --tool SYS:Utilities/MultiView --pos $x,$y
	x=$((x + 130))
	[ $x -gt 300 ] && x=10 y=$((y + 45))
done

# a stable order and time stamps inside the archives
find "$stage" -exec touch -d "@${SOURCE_DATE_EPOCH:-$(git -C "$top" log -1 --format=%ct 2>/dev/null || date +%s)}" {} +

name=riptide-$VERSION
rm -f "$OUT/$name.lha" "$OUT/$name.zip" "$OUT/$name.adf"
cp "$pkg/Riptide.readme" "$OUT/$name.readme"
# Aminet wants no version number in the file names: build/aminet/riptide.lha
# and riptide.readme are the pair to upload to ftp://main.aminet.net/new
mkdir -p "$OUT/aminet"
cp "$pkg/Riptide.readme" "$OUT/aminet/riptide.readme"

# ---- LHA ----
lha_kind() {
	# "LHa for UNIX" (classic, can write) vs lhasa's lha (extract only)
	case "$1" in
	*jlha*) command -v "$1" >/dev/null 2>&1 && echo jlha ;;
	*) command -v "$1" >/dev/null 2>&1 && "$1" --version 2>&1 | grep -q "LHa for UNIX" && echo lha ;;
	esac
}
kind=
if [ -n "${LHA:-}" ]; then
	kind=$(lha_kind "$LHA" || true)
	[ -n "$kind" ] || { echo "mkpackage: LHA=$LHA cannot write archives" >&2; exit 1; }
else
	for c in lha jlha; do
		kind=$(lha_kind "$c" || true)
		[ -n "$kind" ] && { LHA=$c; break; }
	done
fi
abs_out=$(cd "$OUT" && pwd)
case "$kind" in
lha)
	(cd "$stage" && "$LHA" -aq2o5 "$abs_out/$name.lha" Riptide.info Riptide >/dev/null)
	echo "mkpackage: $OUT/$name.lha" ;;
jlha)
	(cd "$stage" && "$LHA" a2o5q "$abs_out/$name.lha" Riptide.info $(find Riptide -type f | LC_ALL=C sort) >/dev/null 2>&1)
	echo "mkpackage: $OUT/$name.lha (jlha)" ;;
*)
	echo "mkpackage: no LHA archiver that can write archives (lha or jlha); making a zip" >&2
	(cd "$stage" && zip -qrX "$abs_out/$name.zip" Riptide.info Riptide)
	echo "mkpackage: $OUT/$name.zip" ;;
esac
[ -f "$OUT/$name.lha" ] && cp "$OUT/$name.lha" "$OUT/aminet/riptide.lha" && echo "mkpackage: $OUT/aminet/riptide.lha + riptide.readme (Aminet upload pair)"

# ---- ADF ----
if command -v xdftool >/dev/null 2>&1; then
	adf=$OUT/$name.adf
	xdftool "$adf" create + format "Riptide" ffs >/dev/null
	xdftool "$adf" write "$stage/Riptide.info" >/dev/null
	if xdftool "$adf" write "$pkg" >/dev/null 2>&1; then
		xdftool "$adf" info | sed -n 's/^\(.*\)$/mkpackage: adf \1/p' | head -3
		echo "mkpackage: $adf"
	else
		echo "mkpackage: the package does not fit on an 880 KB floppy; no ADF" >&2
		rm -f "$adf"
	fi
else
	echo "mkpackage: xdftool (amitools) not found; no ADF" >&2
fi
