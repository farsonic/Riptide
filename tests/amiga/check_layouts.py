#!/usr/bin/env python3
# Riptide - verify the hand-written AmigaOS/AmiTCP definitions in src/amiga
# against the official NDK 3.2, the AmiTCP SDK and the SDK of the historical
# API that riptide.library and its compat alias provide (COMPAT_SDK).
# SPDX-License-Identifier: GPL-2.0-only
#
# 1. Structure offsets/sizes and constants: our headers are compiled for
#    m68k and the values extracted; then a test file is compiled against
#    the reference headers with _Static_assert()s that they are equal.
# 2. Library vector tables (bias + argument registers) are compared with
#    the reference FD files. riptide.library's names are mapped to the
#    reference names (COMPAT_FN_PREFIX), since its jump table is also
#    published as the compat alias and must stay binary compatible.
# Nothing from the reference headers is copied into the repository.
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import gen_lvos  # noqa: E402

CC = os.environ.get('CC_M68K', 'm68k-linux-gnu-gcc')

# The reference SDK of the compat alias: its function prefix, FD file and
# library header, and our constants that carry a different name there.
COMPAT_FN_PREFIX = 'Miami'
COMPAT_FD = 'miami_lib.fd'
COMPAT_HDR = 'libraries/miami.h'
COMPAT_CONSTS = {'RIPTIDECPU_M68KREG': 'MIAMICPU_M68KREG', 'RIPTIDECPU_PPCV4': 'MIAMICPU_PPCV4'}


def compat_name(name):
    return COMPAT_FN_PREFIX + name[len('Riptide'):] if name.startswith('Riptide') else name
OBJCOPY = os.environ.get('OBJCOPY_M68K', 'm68k-linux-gnu-objcopy')

# (our struct, field or None for sizeof, reference struct, reference field)
def S(name, *fields, ref=None):
    ref = ref or name
    out = [('struct ' + name, None, 'struct ' + ref, None)]
    out += [('struct ' + name, f, 'struct ' + ref, f) for f in fields]
    return out

NDK_STRUCTS = (
    S('Node', 'ln_Succ', 'ln_Pred', 'ln_Type', 'ln_Pri', 'ln_Name') +
    S('MinNode', 'mln_Succ', 'mln_Pred') +
    S('List', 'lh_Head', 'lh_Tail', 'lh_TailPred', 'lh_Type', 'l_pad') +
    S('MinList', 'mlh_Head', 'mlh_Tail', 'mlh_TailPred') +
    S('Library', 'lib_Flags', 'lib_NegSize', 'lib_PosSize', 'lib_Version', 'lib_Revision',
      'lib_IdString', 'lib_Sum', 'lib_OpenCnt') +
    S('Task', 'tc_Flags', 'tc_State', 'tc_SigAlloc', 'tc_SigWait', 'tc_SigRecvd', 'tc_SigExcept',
      'tc_SPReg', 'tc_SPLower', 'tc_SPUpper', 'tc_MemEntry', 'tc_UserData') +
    S('MsgPort', 'mp_Flags', 'mp_SigBit', 'mp_SigTask', 'mp_MsgList') +
    S('Message', 'mn_ReplyPort', 'mn_Length') +
    S('SemaphoreRequest', 'sr_Link', 'sr_Waiter') +
    S('SignalSemaphore', 'ss_NestCount', 'ss_WaitQueue', 'ss_MultipleLink', 'ss_Owner',
      'ss_QueueCount') +
    S('Unit', 'unit_flags', 'unit_OpenCnt') +
    S('IORequest', 'io_Device', 'io_Unit', 'io_Command', 'io_Flags', 'io_Error') +
    S('IOStdReq', 'io_Command', 'io_Actual', 'io_Length', 'io_Data', 'io_Offset') +
    S('timeval', 'tv_secs', 'tv_micro', ref='timeval') +
    S('EClockVal', 'ev_hi', 'ev_lo') +
    S('timerequest', 'tr_node', 'tr_time') +
    S('Locale', 'loc_Flags', 'loc_CountryCode', 'loc_GMTOffset')[1:] +   # partial: no sizeof
    S('TagItem', 'ti_Tag', 'ti_Data') +
    S('Hook', 'h_Entry', 'h_SubEntry', 'h_Data') +
    S('DateStamp', 'ds_Days', 'ds_Minute', 'ds_Tick') +
    S('FileInfoBlock', 'fib_DirEntryType', 'fib_FileName', 'fib_Protection', 'fib_Size',
      'fib_Date', 'fib_Comment', 'fib_OwnerUID', 'fib_Reserved') +
    S('AnchorPath', 'ap_BreakBits', 'ap_FoundBreak', 'ap_Flags', 'ap_Strlen', 'ap_Info', 'ap_Buf') +
    [('struct Process', f, 'struct Process', f) for f in
     ('pr_MsgPort', 'pr_SegList', 'pr_StackSize', 'pr_Result2', 'pr_CurrentDir', 'pr_CIS',
      'pr_COS', 'pr_CLI', 'pr_WindowPtr')] +
    [('EXECBASE_LIBLIST_OFS', '=', 'struct ExecBase', 'LibList'),
     ('EXECBASE_PORTLIST_OFS', '=', 'struct ExecBase', 'PortList'),
     ('EXECBASE_ATTNFLAGS_OFS', '=', 'struct ExecBase', 'AttnFlags'),
     ('EXECBASE_ECLOCK_OFS', '=', 'struct ExecBase', 'ex_EClockFrequency')]
)
# intuition/gadtools/graphics/workbench (RiptidePrefs, src/amiga/os/intuition.h);
# Screen and Window are declared only up to the last field used
GUI_STRUCTS = (
    [('struct Screen', f, 'struct Screen', f) for f in
     ('Width', 'Height', 'BarHeight', 'WBorTop', 'WBorLeft', 'WBorRight', 'WBorBottom', 'Font', 'RastPort')] +
    [('struct Window', f, 'struct Window', f) for f in
     ('LeftEdge', 'Width', 'Height', 'WScreen', 'RPort', 'BorderLeft', 'BorderTop', 'BorderRight',
      'BorderBottom', 'FirstGadget', 'IDCMPFlags', 'UserPort')] +
    S('RastPort') +
    S('TextAttr', 'ta_Name', 'ta_YSize', 'ta_Style', 'ta_Flags') +
    S('IntuiMessage', 'Class', 'Code', 'Qualifier', 'IAddress', 'MouseX', 'MouseY', 'IDCMPWindow') +
    S('Gadget', 'LeftEdge', 'TopEdge', 'Width', 'Height', 'Flags', 'SpecialInfo', 'GadgetID', 'UserData') +
    S('NewGadget', 'ng_LeftEdge', 'ng_GadgetText', 'ng_TextAttr', 'ng_GadgetID', 'ng_Flags',
      'ng_VisualInfo', 'ng_UserData') +
    S('EasyStruct', 'es_Flags', 'es_Title', 'es_TextFormat', 'es_GadgetFormat') +
    S('WBArg', 'wa_Lock', 'wa_Name') +
    S('WBStartup', 'sm_Process', 'sm_NumArgs', 'sm_ArgList')
)
GUI_CONSTS = '''GFLG_SELECTED GFLG_DISABLED IDCMP_REFRESHWINDOW IDCMP_MOUSEBUTTONS IDCMP_MOUSEMOVE IDCMP_GADGETDOWN
IDCMP_GADGETUP IDCMP_CLOSEWINDOW IDCMP_RAWKEY IDCMP_VANILLAKEY IDCMP_INTUITICKS WA_Left WA_Top WA_Width WA_Height
WA_IDCMP WA_Gadgets WA_Title WA_InnerWidth WA_InnerHeight WA_PubScreen WA_DragBar WA_DepthGadget WA_CloseGadget
WA_Activate WA_SimpleRefresh WA_SmartRefresh WA_AutoAdjust WA_NewLookMenus GA_Disabled GT_TagBase GTCB_Checked
GTLV_Top GTLV_Labels GTTX_Text GTCY_Labels GTCY_Active GTST_String GTST_MaxChars GTIN_Number GTIN_MaxChars
GTLV_ShowSelected GTLV_Selected GTTX_Border GT_Underscore GTCB_Scaled GTLV_MakeVisible BUTTON_KIND CHECKBOX_KIND
INTEGER_KIND LISTVIEW_KIND CYCLE_KIND STRING_KIND TEXT_KIND ARROWIDCMP BUTTONIDCMP CHECKBOXIDCMP INTEGERIDCMP
LISTVIEWIDCMP CYCLEIDCMP STRINGIDCMP TEXTIDCMP PLACETEXT_LEFT PLACETEXT_RIGHT PLACETEXT_ABOVE PLACETEXT_IN
NG_HIGHLABEL'''.split()
SANA_STRUCTS = (
    S('IOSana2Req', 'ios2_WireError', 'ios2_PacketType', 'ios2_SrcAddr', 'ios2_DstAddr',
      'ios2_DataLength', 'ios2_Data', 'ios2_StatData', 'ios2_BufferManagement') +
    S('Sana2DeviceQuery', 'SizeAvailable', 'SizeSupplied', 'DevQueryFormat', 'DeviceLevel',
      'AddrFieldSize', 'MTU', 'BPS', 'HardwareType', 'RawMTU') +
    S('Sana2DeviceStats', 'PacketsReceived', 'PacketsSent', 'BadData', 'Overruns',
      'UnknownTypesReceived', 'Reconfigurations', 'LastStart') +
    S('Sana2SpecialStatRecord', 'Type', 'Count', 'String') +
    S('Sana2SpecialStatHeader', 'RecordCountMax', 'RecordCountSupplied')
)
NDK_CONSTS = '''NT_LIBRARY NT_MSGPORT NT_MESSAGE NT_REPLYMSG NT_TASK NT_PROCESS LIBF_CHANGED
LIBF_SUMUSED LIBF_DELEXP PA_SIGNAL PA_IGNORE SIGBREAKF_CTRL_C SIGBREAKF_CTRL_D SIGBREAKF_CTRL_E
SIGBREAKF_CTRL_F SIGF_SINGLE IOF_QUICK CMD_READ CMD_WRITE CMD_NONSTD IOERR_ABORTED MEMF_PUBLIC MEMF_CLEAR
MEMF_ANY TAG_DONE TAG_IGNORE TAG_MORE TAG_SKIP TAG_USER UNIT_MICROHZ UNIT_VBLANK TR_ADDREQUEST
TR_GETSYSTIME MODE_OLDFILE MODE_NEWFILE MODE_READWRITE OFFSET_BEGINNING OFFSET_END SHARED_LOCK
ACCESS_READ ERROR_DISK_FULL ERROR_NO_MORE_ENTRIES ERROR_BREAK GVF_GLOBAL_ONLY TICKS_PER_SECOND SYS_Input SYS_Output
SYS_Asynch'''.split()
SANA_CONSTS = '''SANA2IOF_RAW SANA2IOF_BCAST SANA2IOF_MCAST S2_CopyToBuff S2_CopyFromBuff S2_PacketFilter
S2_DEVICEQUERY S2_GETSTATIONADDRESS S2_CONFIGINTERFACE S2_BROADCAST S2_ONEVENT S2_ONLINE S2_OFFLINE
S2_GETGLOBALSTATS S2_GETSPECIALSTATS S2SS_ETHERNET_BADMULTICAST S2SS_ETHERNET_RETRIES S2SS_ETHERNET_FIFO_UNDERRUNS S2WireType_Ethernet S2WireType_PPP S2WireType_SLIP S2ERR_OUTOFSERVICE S2ERR_BAD_STATE
S2WERR_IS_CONFIGURED S2EVENT_ONLINE S2EVENT_OFFLINE S2EVENT_ERROR SANA2_MAX_ADDR_BYTES'''.split()

NET_STRUCTS = (
    S('hostent', 'h_name', 'h_aliases', 'h_addrtype', 'h_length', 'h_addr_list') +
    S('netent', 'n_name', 'n_aliases', 'n_addrtype', 'n_net') +
    S('servent', 's_name', 's_aliases', 's_port', 's_proto') +
    S('protoent', 'p_name', 'p_aliases', 'p_proto') +
    S('addrinfo', 'ai_flags', 'ai_family', 'ai_socktype', 'ai_protocol', 'ai_addrlen',
      'ai_canonname', 'ai_addr', 'ai_next') +
    S('iovec', 'iov_base', 'iov_len') +
    S('msghdr', 'msg_name', 'msg_namelen', 'msg_iov', 'msg_iovlen', 'msg_control',
      'msg_controllen', 'msg_flags') +
    S('cmsghdr', 'cmsg_len', 'cmsg_level', 'cmsg_type') +
    S('if_nameindex', 'if_index', 'if_name') +
    S('UserGroupCredentials', 'cr_ruid', 'cr_rgid', 'cr_umask', 'cr_euid', 'cr_ngroups',
      'cr_groups', 'cr_session', 'cr_login') +
    # Riptide's core ABI structures handed to applications verbatim
    [('struct rt_sockaddr_in', None, 'struct sockaddr_in', None),
     ('struct rt_sockaddr_in', 'sin_len', 'struct sockaddr_in', 'sin_len'),
     ('struct rt_sockaddr_in', 'sin_family', 'struct sockaddr_in', 'sin_family'),
     ('struct rt_sockaddr_in', 'sin_port', 'struct sockaddr_in', 'sin_port'),
     ('struct rt_sockaddr_in', 'sin_addr', 'struct sockaddr_in', 'sin_addr'),
     ('struct rt_sockaddr', None, 'struct sockaddr', None),
     ('struct rt_linger', None, 'struct linger', None),
     ('struct rt_ifreq', None, 'struct ifreq', None),
     ('struct rt_ifconf', None, 'struct ifconf', None)] +
    S('rt_ortentry', 'rt_hash', 'rt_dst', 'rt_gateway', 'rt_flags', 'rt_refcnt', 'rt_use', 'rt_ifp',
      ref='ortentry') +
    S('rt_ifaliasreq', 'ifra_name', 'ifra_addr', 'ifra_broadaddr', 'ifra_mask', ref='ifaliasreq')
)
NET_CONSTS = '''SBTF_REF SBTF_SET SBTC_BREAKMASK SBTC_SIGIOMASK SBTC_SIGURGMASK SBTC_SIGEVENTMASK SBTC_ERRNO
SBTC_HERRNO SBTC_DTABLESIZE SBTC_FDCALLBACK SBTC_LOGSTAT SBTC_LOGTAGPTR SBTC_LOGFACILITY SBTC_LOGMASK
SBTC_ERRNOSTRPTR SBTC_HERRNOSTRPTR SBTC_IOERRNOSTRPTR SBTC_S2ERRNOSTRPTR SBTC_S2WERRNOSTRPTR
SBTC_ERRNOBYTEPTR SBTC_ERRNOWORDPTR SBTC_ERRNOLONGPTR SBTC_HERRNOLONGPTR SBTC_RELEASESTRPTR FDCB_FREE
FDCB_ALLOC FDCB_CHECK HOST_NOT_FOUND TRY_AGAIN NO_RECOVERY NO_DATA AI_PASSIVE AI_CANONNAME NI_NOFQDN
NI_NUMERICHOST NI_NAMEREQD NI_NUMERICSERV NI_DGRAM EAI_ADDRFAMILY EAI_AGAIN EAI_BADFLAGS EAI_FAIL
EAI_FAMILY EAI_MEMORY EAI_NODATA EAI_NONAME EAI_SERVICE EAI_SOCKTYPE EAI_SYSTEM LOG_PRIMASK LOG_FACMASK
LOG_USER LOG_PID RIPTIDECPU_M68KREG RIPTIDECPU_PPCV4 NGROUPS MAXLOGNAME UGT_ERRNOBPTR UGT_ERRNOWPTR
UGT_ERRNOLPTR UGT_INTRMASK UGT_OWNER'''.split()
# rt_bsd.h / rt_errno.h constants: RT_X must equal the SDK's X
RT_CONSTS = '''AF_INET SOCK_STREAM SOCK_DGRAM SOCK_RAW SOL_SOCKET SO_DEBUG SO_ACCEPTCONN SO_REUSEADDR
SO_KEEPALIVE SO_DONTROUTE SO_BROADCAST SO_USELOOPBACK SO_LINGER SO_OOBINLINE SO_REUSEPORT SO_SNDBUF
SO_RCVBUF SO_SNDLOWAT SO_RCVLOWAT SO_SNDTIMEO SO_RCVTIMEO SO_ERROR SO_TYPE MSG_OOB MSG_PEEK
MSG_DONTROUTE MSG_EOR MSG_TRUNC MSG_CTRUNC MSG_WAITALL MSG_DONTWAIT IP_OPTIONS IP_HDRINCL IP_TOS
IP_TTL IP_RECVDSTADDR IP_MULTICAST_IF IP_MULTICAST_TTL IP_MULTICAST_LOOP IP_ADD_MEMBERSHIP
IP_DROP_MEMBERSHIP TCP_NODELAY TCP_MAXSEG FIONREAD FIONBIO FIOASYNC FIOSETOWN FIOGETOWN SIOCATMARK
SIOCSPGRP SIOCGPGRP SIOCSIFADDR SIOCSIFDSTADDR SIOCSIFFLAGS SIOCGIFFLAGS SIOCSIFBRDADDR SIOCSIFNETMASK
SIOCGIFMETRIC SIOCSIFMETRIC SIOCDIFADDR SIOCGIFADDR SIOCGIFDSTADDR SIOCGIFBRDADDR SIOCGIFCONF
SIOCGIFNETMASK SIOCGIFMTU SIOCSIFMTU EPERM ENOENT EINTR EBADF ENOMEM EACCES EFAULT EINVAL EMFILE EPIPE
EWOULDBLOCK EINPROGRESS EALREADY ENOTSOCK EDESTADDRREQ EMSGSIZE EPROTOTYPE ENOPROTOOPT
EPROTONOSUPPORT ESOCKTNOSUPPORT EOPNOTSUPP EPFNOSUPPORT EAFNOSUPPORT EADDRINUSE EADDRNOTAVAIL ENETDOWN
ENETUNREACH ENETRESET ECONNABORTED ECONNRESET ENOBUFS EISCONN ENOTCONN ESHUTDOWN ETOOMANYREFS ETIMEDOUT
ECONNREFUSED ELOOP ENAMETOOLONG EHOSTDOWN EHOSTUNREACH ENOTEMPTY SIOCADDRT SIOCDELRT SIOCAIFADDR
RTF_UP RTF_GATEWAY RTF_HOST RTF_REJECT RTF_DYNAMIC RTF_MASK RTF_STATIC'''.split()
# AmiTCP SDK: the ARP ioctls (struct arpreq is AmiTCP specific: 18 byte arp_ha)
AMITCP_ARP_CONSTS = '''SIOCSARP SIOCGARP SIOCDARP ATF_INUSE ATF_COM ATF_PERM ATF_PUBL'''.split()


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.stderr.write(' '.join(cmd) + '\n' + r.stdout + r.stderr)
        raise SystemExit(1)
    return r


def our_values(tmp, items, consts, includes):
    """Compile our headers and return the numeric values of items/consts."""
    exprs = []
    for mine, field, _, _ in items:
        if field == '=':
            exprs.append(mine)
        elif field is None:
            exprs.append('sizeof(%s)' % mine)
        else:
            exprs.append('offsetof(%s, %s)' % (mine, field))
    exprs += consts
    src = os.path.join(tmp, 'ours.c')
    with open(src, 'w') as f:
        f.write('#include <stddef.h>\n')
        for i in includes:
            f.write('#include "%s"\n' % i)
        f.write('const unsigned long __attribute__((section(".rodata.vals"))) vals[] = {\n')
        f.write(',\n'.join('\t(unsigned long)(%s)' % e for e in exprs))
        f.write('\n};\n')
    gi = run([CC, '-print-file-name=include']).stdout.strip()
    obj = os.path.join(tmp, 'ours.o')
    run([CC, '-m68020', '-ffreestanding', '-nostdinc', '-isystem', gi,
         '-I', os.path.join(ROOT, 'src/amiga/include'), '-I', os.path.join(ROOT, 'src/amiga/os'),
         '-I', os.path.join(ROOT, 'src/amiga'), '-I', os.path.join(ROOT, 'include/rt'),
         '-c', src, '-o', obj])
    binf = os.path.join(tmp, 'ours.bin')
    run([OBJCOPY, '-O', 'binary', '-j', '.rodata.vals', obj, binf])
    data = open(binf, 'rb').read()
    return list(struct.unpack('>%dI' % (len(data) // 4), data))


def check_against(tmp, name, items, consts, our_inc, ref_inc, ref_dirs, defines=(), rt_consts=()):
    vals = our_values(tmp, items, consts + ['RT_' + c for c in rt_consts], our_inc)
    src = os.path.join(tmp, 'ref_%s.c' % name)
    with open(src, 'w') as f:
        for d in defines:
            f.write('#define %s\n' % d)
        f.write('#include <stddef.h>\n')
        for i in ref_inc:
            f.write('#include <%s>\n' % i)
        n = 0
        for mine, field, ref, rfield in items:
            if rfield is None:
                e = 'sizeof(%s)' % ref
            else:
                e = 'offsetof(%s, %s)' % (ref, rfield)
            f.write('_Static_assert((unsigned long)(%s) == %uUL, "%s.%s: ours=%u");\n'
                    % (e, vals[n], mine, field or 'sizeof', vals[n]))
            n += 1
        for c in consts + list(rt_consts):
            rc = COMPAT_CONSTS.get(c, c)
            f.write('_Static_assert((unsigned long)(%s) == %uUL, "%s: ours=%u");\n' % (rc, vals[n], c, vals[n]))
            n += 1
    gi = run([CC, '-print-file-name=include']).stdout.strip()
    cmd = [CC, '-m68020', '-std=gnu89', '-fsyntax-only', '-ffreestanding', '-nostdinc', '-isystem', gi,
           '-w', '-Wno-error']
    for d in ref_dirs:
        cmd += ['-I', d]
    r = subprocess.run(cmd + [src], capture_output=True, text=True)
    errs = [l for l in r.stderr.splitlines() if 'static assertion failed' in l or 'error' in l]
    if r.returncode:
        for l in errs[:60]:
            print('  ' + l)
        print('FAIL %s: %d problems' % (name, len(errs)))
        return False
    print('ok   %s: %d structure fields/sizes, %d constants' % (name, len(items), len(consts) + len(rt_consts)))
    return True


def parse_fd(path):
    funcs = {}
    bias = 0
    for line in open(path, encoding='latin-1'):
        line = line.strip()
        if line.startswith('##bias'):
            bias = int(line.split()[1])
        elif line.startswith('##') or line.startswith('*') or not line:
            continue
        else:
            m = re.match(r'(\w+)\((.*?)\)\((.*?)\)', line)
            if m:
                regs = [r.strip().lower() for r in re.split('[,/]', m.group(3)) if r.strip()]
                funcs[m.group(1)] = (bias, regs)
            bias += 6
    return funcs


def check_lvos(table, lib, fd, rename=None, allow_extra=()):
    libs = gen_lvos.parse(table)
    ok = True
    ref = parse_fd(fd)
    n = 0
    for f in libs[lib].funcs:
        if (f.bias <= 24 and 'resource' not in libs[lib].opts) or f.name in allow_extra:
            continue
        name = rename(f.name) if rename else f.name
        if name not in ref:
            print('  %s: %s not in %s' % (lib, f.name, os.path.basename(fd)))
            ok = False
            continue
        rb, rregs = ref[name]
        regs = [r for t, a, r in f.args]
        if rb != f.bias or rregs != regs:
            print('  %s.%s: ours -%d %s, reference -%d %s' % (lib, f.name, f.bias, regs, rb, rregs))
            ok = False
        n += 1
    print('%s %s: %d vectors match %s' % ('ok  ' if ok else 'FAIL', lib, n, os.path.basename(fd)))
    return ok


def main():
    ndk = os.environ.get('NDK')
    compat = os.environ.get('COMPAT_SDK')
    amitcp = os.environ.get('AMITCP_SDK')
    if not ndk or not os.path.isdir(ndk):
        print('SKIP: set NDK=<path to NDK3.2> (and COMPAT_SDK, AMITCP_SDK)')
        return 0
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        inc = os.path.join(ndk, 'Include_H')
        ok &= check_against(tmp, 'ndk', NDK_STRUCTS, NDK_CONSTS, ['exec.h', 'dos.h', 'devices.h'],
                            ['exec/types.h', 'exec/execbase.h', 'exec/semaphores.h', 'exec/io.h', 'exec/devices.h',
                             'exec/memory.h', 'exec/errors.h', 'devices/timer.h', 'libraries/locale.h', 'dos/dos.h',
                             'dos/dosextens.h', 'dos/dosasl.h', 'dos/var.h', 'dos/dostags.h', 'utility/tagitem.h', 'utility/hooks.h'],
                            [inc])
        ok &= check_against(tmp, 'ndk-gui', GUI_STRUCTS, GUI_CONSTS, ['exec.h', 'dos.h', 'intuition.h'],
                            ['exec/types.h', 'intuition/intuition.h', 'intuition/screens.h',
                             'libraries/gadtools.h', 'graphics/text.h', 'graphics/rastport.h',
                             'workbench/startup.h'], [inc])
        sana = os.path.join(ndk, 'SANA+RoadshowTCP-IP', 'include')
        if os.path.isdir(sana):
            ok &= check_against(tmp, 'sana2', SANA_STRUCTS, SANA_CONSTS, ['exec.h', 'devices.h'],
                                ['devices/timer.h', 'devices/sana2.h', 'devices/sana2specialstats.h'], [sana, inc])
        if compat and os.path.isdir(compat):
            ni = os.path.join(compat, 'netinclude')
            refs = ['sys/types.h', 'sys/uio.h', 'sys/socket.h', 'sys/ioctl.h', 'sys/errno.h', 'netinet/in.h',
                    'netinet/tcp.h', 'net/if.h', 'net/route.h', 'netdb.h', 'bsdsocket/socketbasetags.h',
                    'sys/syslog.h', COMPAT_HDR, 'libraries/usergroup.h']
            ok &= check_against(tmp, 'compat-sdk', NET_STRUCTS, NET_CONSTS,
                                ['exec.h', 'amiga_net.h', 'rt_bsd.h', 'rt_errno.h', 'rt_ip.h'], refs, [ni, inc, os.path.join(ROOT, 'src/amiga/include')],
                                rt_consts=RT_CONSTS)
            fdd = os.path.join(compat, 'fd')
            t = os.path.join(ROOT, 'src/amiga/lib_lvos.txt')
            ok &= check_lvos(t, 'bsdsocket', os.path.join(fdd, 'socket_lib.fd'))
            ok &= check_lvos(t, 'riptide', os.path.join(fdd, COMPAT_FD), rename=compat_name,
                             allow_extra=('RiptideReserved53',))
            ok &= check_lvos(t, 'usergroup', os.path.join(fdd, 'usergroup_lib.fd'))
            m = [f for f in gen_lvos.parse(t)['riptide'].funcs]
            if max(f.bias for f in m) != 522:
                print('FAIL riptide: table must extend to -522 (compat alias)')
                ok = False
        if amitcp and os.path.isdir(amitcp):
            ni = os.path.join(amitcp, 'netinclude')
            items = (S('passwd', 'pw_name', 'pw_passwd', 'pw_uid', 'pw_gid', 'pw_gecos', 'pw_dir',
                       'pw_shell') +
                     S('group', 'gr_name', 'gr_passwd', 'gr_gid', 'gr_mem') +
                     S('utmp', 'ut_time', 'ut_sid', 'ut_name', 'ut_host') +
                     S('lastlog', 'll_time', 'll_uid', 'll_name', 'll_host'))
            ok &= check_against(tmp, 'amitcp-sdk', items, ['UT_NAMESIZE', 'UT_HOSTSIZE'],
                                ['exec.h', 'amiga_net.h'], ['sys/types.h', 'pwd.h', 'grp.h', 'utmp.h'],
                                [ni, inc])
            arp = S('rt_arpreq', 'arp_pa', 'arp_ha', 'arp_flags', ref='arpreq')
            ok &= check_against(tmp, 'amitcp-arp', arp, [], ['exec.h', 'rt_bsd.h'],
                                ['sys/types.h', 'sys/socket.h', 'sys/ioctl.h', 'net/if_arp.h'],
                                [ni, inc], defines=('AMITCP', 'KERNEL'), rt_consts=AMITCP_ARP_CONSTS)
            fdd = os.path.join(amitcp, 'netinclude', 'fd')
            if os.path.isdir(fdd):
                t = os.path.join(ROOT, 'src/amiga/lib_lvos.txt')
                for lib, fdn in (('bsdsocket', 'bsdsocket_lib.fd'), ('usergroup', 'usergroup_lib.fd')):
                    if os.path.exists(os.path.join(fdd, fdn)):
                        ok &= check_lvos(t, lib, os.path.join(fdd, fdn))
        # the developer SDK's FD file (generated from the same table)
        ok &= check_lvos(os.path.join(ROOT, 'src/amiga/lib_lvos.txt'), 'riptide',
                         os.path.join(ROOT, 'sdk/fd/riptide_lib.fd'))
        t = os.path.join(ROOT, 'src/amiga/os/os_lvos.txt')
        for lib, fdn in (('exec', 'exec_lib.fd'), ('dos', 'dos_lib.fd'), ('timer', 'timer_lib.fd'),
                         ('locale', 'locale_lib.fd'), ('battclock', 'battclock_lib.fd'),
                         ('intuition', 'intuition_lib.fd'), ('gadtools', 'gadtools_lib.fd'),
                         ('graphics', 'graphics_lib.fd')):
            ok &= check_lvos(t, lib, os.path.join(ndk, 'FD', fdn))
    print('ALL OK' if ok else 'MISMATCHES FOUND')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
