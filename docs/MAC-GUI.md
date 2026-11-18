# RiptidePrefs on the Mac: one editor for two platforms

RiptidePrefs, the preferences editor for riptide.conf, exists once. The MUI editor in
`src/amiga/prefs` is compiled for the classic Mac OS as well: the same pages, settings, layout,
help bubbles, list forms, Status and Counters pages, the same reading, checking and writing of
riptide.conf. A new setting, page or wording change made for the Amiga reaches the Mac with the
next `make -f Makefile.mac`, and nothing in the Amiga build changes for the Mac: the Amiga
binary is byte-identical with and without the Mac work (checked with `sha256sum` on
`build/amiga-gcc/RiptidePrefs`).

![RiptidePrefs on a Mac, Interface page](mac-gui-interface.png)

## How it is put together

```
 src/amiga/prefs (unchanged)                      the Mac's side (src/mac)
 ---------------------------                      ------------------------
 mui_main.c   the window, pages, commands         mui/   the part of MUI 3.8 the editor uses,
 mui_classes.c, mui_status.c, mui_counters.c             on the Toolbox (mm_*.c)
              Riptide's own MUI classes           prefs/rp_main_mac.c    main (riptideprefs.c's job)
 mui_theme.c  colours, fonts, icons               prefs/mac_lang.c       tr(): the Mac's words
 mui_util.c, mui_thunks.S  BOOPSI glue            prefs/driver_mac.c     the driver line (.ENET)
 prefs_fields.c, conf_edit.c  settings, riptide.conf                     (driver_amiga.c's job)
 prefs_io.c   load, save, apply                   prefs/rtport_mac.c     the RIPTIDE port, carried
 prefs_status.c, prefs_counters.c, status_amiga.c                        to the .IPP driver
 prefs_driver.c                                   app/   crt0, the Toolbox glue, the packager
```

The editor's files are compiled with `-I` paths that give them the Amiga's own type headers
(`src/amiga/os`) and, in place of the jump-table calls the Amiga build generates, plain
prototypes (`tools/gen_lvos.py CC=proto`, from the same LVO tables). `src/mac/mui` implements
what those prototypes name:

| Amiga | Mac |
|---|---|
| BOOPSI classes, `DoMethodA`, `DoSuperMethodA` (mui_util.c, mui_thunks.S: dispatchers called with the class in a0, object in a2, message in a1) | the same calls, unchanged; the built-in classes use the same register thunks (mm_thunks.S) |
| MUI: Application, Window, Group (rows, columns, pages, weights), Area, Rectangle, String, Cycle, Image (check mark), List, Listview, menu strips from NewMenu, notifications, event handlers, input handlers (timers), MUI_Redraw | mm_boopsi.c, mm_app.c, mm_area.c, mm_group.c, mm_gadgets.c: a window per MUI window, TextEdit for strings, pop-up menus for cycles, the menu bar from the NewMenu table (Apple menu and the Mac's Edit items added), help bubbles after a second's rest |
| graphics.library (SetAPen, RectFill, Text, TextLength, ObtainBestPenA, ScrollRaster ...) | mm_gfx.c on QuickDraw: pens are RGB, colour screens use RGBForeColor, one-bit screens dither the theme's greys and dim disabled text |
| exec ports and lists, dos files (Lock, Open, Read, Write, Rename ...), ASL | mm_os.c: the File Manager by path, Standard File for Open and Save As |
| ENVARC:Riptide/ and ENV:Riptide/ | the Riptide folder in the Preferences folder, and its folder "In Use" (emptied by Riptide when the Mac starts, as ENV: goes with a reboot) |
| the RIPTIDE message port (COMMAND, TABLES, STATUS) | rtport_mac.c: FindPort("RIPTIDE") answers when Riptide's .IPP is installed; a message put there becomes a private .IPP Control call (`RTMAC_PORT`, src/mac/ipp.c), run in the stack's context with the core's own `rt_ctl_command` and `rt_sysctl_bymib` |

## The Mac's words and the Mac's card

The editor's texts speak of the Amiga's things: SANA-II drivers, DEVS:, ENV:, uaenet.device,
"the Amiga". Every text the editor shows passes through `tr()`, its translation hook
(mui_lang.c, English only on the Amiga). The Mac build links `src/mac/prefs/mac_lang.c`
instead, whose `tr()` replaces those words: whole help texts where the Mac's answer differs (the
Device field: always `.ENET`; Unit: which Ethernet card; Reads: not used on the Mac), words
elsewhere (SANA-II, uaenet.device, the Amiga). The rules are keyed on distinctive pieces of the
texts, so small changes of the Amiga's wording keep working; a new text that names something
only the Amiga has needs a rule there.

The card of a riptide.conf without one is a compile-time default (`RP_DEFAULT_IFNAME`,
`RP_DEFAULT_DEVICE` in conf_edit.c: eth0 on uaenet.device; Makefile.mac gives en0 on .ENET).
riptide.conf on the Mac uses the Amiga's form of the card line, which the Mac driver now
understands: `interface en0 device=.ENET unit=0 [mtu=] [ipv4=] [ipv6=] [tcpwindow=] [disabled]`.
On the first run, with no riptide.conf yet, the editor writes a starting one (the card and
DHCP), as the Amiga's Installer does.

## Save, Use, Apply and Cancel on the Mac

The buttons do what they do on the Amiga: **Save** writes riptide.conf for good (Preferences
folder) and gives the changes to the running Riptide; **Use** writes the In Use copy (until the
Mac restarts) and gives the changes; **Apply** gives the changes to the running Riptide only and
keeps the window open; **Cancel** forgets them. Riptide for the Mac cannot yet restart its
network while it runs (RTCMD_RESTART), so changes to the card and its addresses take effect
when the Mac restarts, and the editor says so.

## Keeping both versions in step

- A new setting: one line in `rp_fields[]` (prefs_fields.c). Both editors show it.
- A new page or class: in `src/amiga/prefs` as for the Amiga. A new class's dispatcher goes into
  `mui_thunks.S`, which both builds assemble. If it uses a MUI attribute, method or class the
  editor has not used before, it is added to `mui.h` (as for the Amiga) and to `src/mac/mui`.
- A new OS call: `src/mac/mui/mm_os.c` (the link fails until it is there).
- A text that names an Amiga thing: a rule in `src/mac/prefs/mac_lang.c`.
- Check the Amiga build stays the same: `make -f Makefile.amiga TOOLCHAIN=gcc build/amiga-gcc/RiptidePrefs`
  before and after, and compare the binaries.

## Building and trying it

```
make -f Makefile.mac          # build/mac/Riptide.bin (the extension) and build/mac/RiptidePrefs.bin
```

Copy both to an HFS image with `hcopy -m`: Riptide into the Extensions folder (MacTCP out of the
Control Panels folder), RiptidePrefs anywhere (the Control Panels folder or the Apple menu
suit it). The testbed (docs/MAC.md, "Testing on Aurora68") runs it under QEMU; Aurora68's
`mac.inputclick=SECONDS,X,Y,X,Y...` clicks through it for screenshots.

Screens under 560 usable lines (every classic size up to 640 x 480) get the editor's compact
layout, which fits a 640 x 480 screen in 610 x 361.

## Known gaps

- Riptide for the Mac drives one Ethernet card (unit 0).
- RESTART (applying card and address changes without a restart of the Mac) is not there yet.
- The window is laid out from the editor's sizes; the 512 x 342 screen of a compact Mac is
  narrower than the editor's smallest compact window (530 pixels).
