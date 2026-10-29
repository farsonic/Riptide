# Riptide - helpers for FS-UAE tests on a real Kickstart 3.1.4 with an
# installed Workbench 3.1.4 (sourced by tests/amiga/run_fsuae_ks314_*.sh).
#
# The ROM and Workbench are copyrighted and not part of Riptide: point
# KS314_ROM at an A1200 Kickstart 3.1.4 image (46.143) and KS314_SYS at a
# folder holding an installed Workbench 3.1.4 (the contents of the
# Workbench3.1.4 disk plus icon.library and workbench.library from the
# Install disk, and Fonts and Locale). Tests SKIP without them. MUI_DIR
# (optional) is the MUI drawer of MUI 3.8's user archive (mui38usr.lha).
#
#   . tests/amiga/ks314.sh
#   ks314_init                       SKIP (exit 0) when the ROM or system is missing
#   ks314_system DEST PKG [CONF]     DEST: a copy of the system with Riptide
#                                    installed from PKG (build/package/Riptide)
#                                    as Install_Riptide does: C:, SYS:Prefs/
#                                    RiptidePrefs, ENVARC:Riptide/riptide.conf
#                                    (CONF, or a static eth0 on uaenet.device),
#                                    hosts and the S:User-Startup block; MUI
#                                    when MUI_DIR is set
#   ks314_hook DEST < SCRIPT         an AmigaDOS script run (Run >NIL:) at the
#                                    end of S:User-Startup, before LoadWB
#   ks314_leftout DEST PATH X Y [TT...]
#                                    put the icon of SYS:PATH on the Workbench
#                                    window at X,Y (.backdrop, do_CurrentX/Y);
#                                    with tool types TT a new tool icon
#                                    (tools/mkicon.py) carries them
#   ks314_config FILE DEST [LINES]   an FS-UAE configuration booting DEST with
#                                    the ROM (A1200, 8 MB Fast RAM, uaenet)
# SPDX-License-Identifier: GPL-2.0-only

KS314_REPO=${KS314_REPO:-$(pwd)}     # the Riptide tree (for tools/mkicon.py)
KS314_ROM=${KS314_ROM:-/root/amiga-os314/rom/kick.a1200.46.143}
KS314_SYS=${KS314_SYS:-/root/amiga-os314/system}

ks314_init()
{
	[ -f "$KS314_ROM" ] || { echo "SKIP: set KS314_ROM to an A1200 Kickstart 3.1.4 image"; exit 0; }
	[ -f "$KS314_SYS/C/LoadWB" ] && [ -f "$KS314_SYS/Libs/workbench.library" ] ||
		{ echo "SKIP: set KS314_SYS to an installed Workbench 3.1.4 folder"; exit 0; }
	command -v fs-uae >/dev/null 2>&1 || { echo "SKIP: fs-uae not installed"; exit 0; }
}

# ks314_system DEST PKG [CONF]
ks314_system()
{
	dest=$1
	pkg=$2
	rm -rf "$dest"
	cp -R "$KS314_SYS" "$dest" || return 1
	mkdir -p "$dest/Prefs/Env-Archive/Riptide" "$dest/T"
	cp "$pkg"/C/* "$dest/C/" || return 1
	cp "$pkg/RiptidePrefs" "$pkg/RiptidePrefs.info" "$dest/Prefs/" || return 1
	if [ -n "${3:-}" ]; then
		cp "$3" "$dest/Prefs/Env-Archive/Riptide/riptide.conf"
	else
		printf '%s\n' "# Riptide configuration (Kickstart 3.1.4 test)" "" \
			"interface eth0 device=uaenet.device unit=0 mtu=1500" \
			"ifconfig eth0 10.0.2.15/24 up" "hostname amiga" \
			>"$dest/Prefs/Env-Archive/Riptide/riptide.conf"
	fi
	printf '%s\n' "127.0.0.1 localhost" "::1 localhost" >"$dest/Prefs/Env-Archive/Riptide/hosts"
	{
		if [ -n "${MUI_DIR:-}" ] && [ -f "$MUI_DIR/Libs/muimaster.library" ]; then
			cp -R "$MUI_DIR" "$dest/MUI"
			echo ";BEGIN MUI"
			echo "Assign >NIL: MUI: SYS:MUI"
			echo "Assign >NIL: LIBS: MUI:Libs ADD"
			echo "Assign >NIL: IMAGES: MUI:Images"
			echo "Assign >NIL: LOCALE: MUI:Locale ADD"
			echo ";END MUI"
		fi
		echo ";BEGIN Riptide"
		echo "Run >NIL: C:Riptide"
		echo ";END Riptide"
		echo "If EXISTS SYS:ks314-hook"
		echo "  Run >NIL: Execute SYS:ks314-hook"
		echo "EndIf"
	} >"$dest/S/User-Startup"
}

ks314_hook()
{
	cat >"$1/ks314-hook"
}

# ks314_leftout DEST PATH X Y: PATH relative to SYS: (e.g. Prefs/RiptidePrefs)
ks314_leftout()
{
	echo ":$2" >>"$1/.backdrop"
	if [ $# -gt 4 ]; then
		d=$1 f=$2 x=$3 y=$4
		shift 4
		set -- tool "$d/$f.info" --image prefs --stack 16384 --pos "$x,$y" \
			$(for t in "$@"; do printf ' --tooltype %s' "$t"; done)
		python3 "$KS314_REPO/tools/mkicon.py" "$@" >/dev/null
		return
	fi
	python3 - "$1/$2.info" "$3" "$4" <<'EOF'
import struct, sys
p, x, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
d = bytearray(open(p, "rb").read())
assert d[0:2] == b"\xe3\x10"
struct.pack_into(">ii", d, 58, x, y)   # do_CurrentX, do_CurrentY
open(p, "wb").write(d)
EOF
}

# ks314_config FILE DEST [EXTRA LINES]
ks314_config()
{
	{
		echo "[fs-uae]"
		echo "amiga_model = A1200"
		echo "kickstart_file = $KS314_ROM"
		echo "fast_memory = 8192"
		echo "hard_drive_0 = $2"
		echo "hard_drive_0_label = System"
		echo "floppy_drive_count = 0"
		echo "sound_card = none"
		echo "automatic_input_grab = 0"
		echo "video_sync = off"
		echo "uae_sana2 = true"
		echo "network_card = a2065"
		[ -n "${3:-}" ] && printf '%s\n' "$3"
	} >"$1"
}
