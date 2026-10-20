#!/usr/bin/env python3
# Riptide - verify the hand-written AmigaOS/AmiTCP/Miami definitions in
# src/amiga against the official NDK 3.2 and the AmiTCP/Miami SDKs.
# SPDX-License-Identifier: GPL-2.0-only
#
# 1. Structure offsets/sizes and constants: our headers are compiled for
#    m68k and the values extracted; then a test file is compiled against
#    the reference headers with _Static_assert()s that they are equal.
# 2. Library vector tables (bias + argument registers) are compared with
#    the reference FD files.
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
    S('TagItem', 'ti_Tag', 'ti_Data') +
    S('Hook', 'h_Entry', 'h_SubEntry', 'h_Data') +
    S('DateStamp', 'ds_Days', 'ds_Minute', 'ds_Tick') +
    [('struct Process', f, 'struct Process', f) for f in
     ('pr_MsgPort', 'pr_SegList', 'pr_StackSize', 'pr_Result2', 'pr_CurrentDir', 'pr_CIS',
      'pr_COS', 'pr_CLI', 'pr_WindowPtr')] +
    [('EXECBASE_LIBLIST_OFS', '=', 'struct ExecBase', 'LibList'),
     ('EXECBASE_PORTLIST_OFS', '=', 'struct ExecBase', 'PortList'),
     ('EXECBASE_ATTNFLAGS_OFS', '=', 'struct ExecBase', 'AttnFlags'),
     ('EXECBASE_ECLOCK_OFS', '=', 'struct ExecBase', 'ex_EClockFrequency')]
)
SANA_STRUCTS = (
    S('IOSana2Req', 'ios2_WireError', 'ios2_PacketType', 'ios2_SrcAddr', 'ios2_DstAddr',
      'ios2_DataLength', 'ios2_Data', 'ios2_StatData', 'ios2_BufferManagement') +
    S('Sana2DeviceQuery', 'SizeAvailable', 'SizeSupplied', 'DevQueryFormat', 'DeviceLevel',
      'AddrFieldSize', 'MTU', 'BPS', 'HardwareType', 'RawMTU')
)
NDK_CONSTS = '''NT_LIBRARY NT_MSGPORT NT_MESSAGE NT_REPLYMSG NT_TASK NT_PROCESS LIBF_CHANGED
LIBF_SUMUSED LIBF_DELEXP PA_SIGNAL PA_IGNORE SIGBREAKF_CTRL_C SIGBREAKF_CTRL_D SIGBREAKF_CTRL_E
SIGBREAKF_CTRL_F SIGF_SINGLE IOF_QUICK CMD_READ CMD_WRITE CMD_NONSTD IOERR_ABORTED MEMF_PUBLIC MEMF_CLEAR
MEMF_ANY TAG_DONE TAG_IGNORE TAG_MORE TAG_SKIP TAG_USER UNIT_MICROHZ UNIT_VBLANK TR_ADDREQUEST
TR_GETSYSTIME MODE_OLDFILE MODE_NEWFILE GVF_GLOBAL_ONLY TICKS_PER_SECOND SYS_Input SYS_Output
SYS_Asynch'''.split()
SANA_CONSTS = '''SANA2IOF_RAW SANA2IOF_BCAST SANA2IOF_MCAST S2_CopyToBuff S2_CopyFromBuff S2_PacketFilter
S2_DEVICEQUERY S2_GETSTATIONADDRESS S2_CONFIGINTERFACE S2_BROADCAST S2_ONEVENT S2_ONLINE S2_OFFLINE
S2_GETGLOBALSTATS S2WireType_Ethernet S2WireType_PPP S2WireType_SLIP S2ERR_OUTOFSERVICE S2ERR_BAD_STATE
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
     ('struct rt_ifconf', None, 'struct ifconf', None)]
)
NET_CONSTS = '''SBTF_REF SBTF_SET SBTC_BREAKMASK SBTC_SIGIOMASK SBTC_SIGURGMASK SBTC_SIGEVENTMASK SBTC_ERRNO
SBTC_HERRNO SBTC_DTABLESIZE SBTC_FDCALLBACK SBTC_LOGSTAT SBTC_LOGTAGPTR SBTC_LOGFACILITY SBTC_LOGMASK
SBTC_ERRNOSTRPTR SBTC_HERRNOSTRPTR SBTC_IOERRNOSTRPTR SBTC_S2ERRNOSTRPTR SBTC_S2WERRNOSTRPTR
SBTC_ERRNOBYTEPTR SBTC_ERRNOWORDPTR SBTC_ERRNOLONGPTR SBTC_HERRNOLONGPTR SBTC_RELEASESTRPTR FDCB_FREE
FDCB_ALLOC FDCB_CHECK HOST_NOT_FOUND TRY_AGAIN NO_RECOVERY NO_DATA AI_PASSIVE AI_CANONNAME NI_NOFQDN
NI_NUMERICHOST NI_NAMEREQD NI_NUMERICSERV NI_DGRAM EAI_ADDRFAMILY EAI_AGAIN EAI_BADFLAGS EAI_FAIL
EAI_FAMILY EAI_MEMORY EAI_NODATA EAI_NONAME EAI_SERVICE EAI_SOCKTYPE EAI_SYSTEM LOG_PRIMASK LOG_FACMASK
LOG_USER LOG_PID MIAMICPU_M68KREG MIAMICPU_PPCV4 NGROUPS MAXLOGNAME UGT_ERRNOBPTR UGT_ERRNOWPTR
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
ECONNREFUSED ELOOP ENAMETOOLONG EHOSTDOWN EHOSTUNREACH ENOTEMPTY'''.split()


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
            f.write('_Static_assert((unsigned long)(%s) == %uUL, "%s: ours=%u");\n' % (c, vals[n], c, vals[n]))
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
        if f.bias <= 24 or f.name in allow_extra:
            continue
        name = (rename or {}).get(f.name, f.name)
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
    miami = os.environ.get('MIAMI_SDK')
    amitcp = os.environ.get('AMITCP_SDK')
    if not ndk or not os.path.isdir(ndk):
        print('SKIP: set NDK=<path to NDK3.2> (and MIAMI_SDK, AMITCP_SDK)')
        return 0
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        inc = os.path.join(ndk, 'Include_H')
        ok &= check_against(tmp, 'ndk', NDK_STRUCTS, NDK_CONSTS, ['exec.h', 'dos.h', 'devices.h'],
                            ['exec/types.h', 'exec/execbase.h', 'exec/semaphores.h', 'exec/io.h', 'exec/devices.h',
                             'exec/memory.h', 'exec/errors.h', 'devices/timer.h', 'dos/dos.h',
                             'dos/dosextens.h', 'dos/var.h', 'dos/dostags.h', 'utility/tagitem.h', 'utility/hooks.h'],
                            [inc])
        sana = os.path.join(ndk, 'SANA+RoadshowTCP-IP', 'include')
        if os.path.isdir(sana):
            ok &= check_against(tmp, 'sana2', SANA_STRUCTS, SANA_CONSTS, ['exec.h', 'sana2.h'],
                                ['devices/sana2.h'], [sana, inc])
        if miami and os.path.isdir(miami):
            ni = os.path.join(miami, 'netinclude')
            refs = ['sys/types.h', 'sys/uio.h', 'sys/socket.h', 'sys/ioctl.h', 'sys/errno.h', 'netinet/in.h',
                    'netinet/tcp.h', 'net/if.h', 'netdb.h', 'bsdsocket/socketbasetags.h',
                    'sys/syslog.h', 'libraries/miami.h', 'libraries/usergroup.h']
            ok &= check_against(tmp, 'miami-sdk', NET_STRUCTS, NET_CONSTS,
                                ['exec.h', 'amiga_net.h', 'rt_bsd.h', 'rt_errno.h'], refs, [ni, inc, os.path.join(ROOT, 'src/amiga/include')],
                                rt_consts=RT_CONSTS)
            fdd = os.path.join(miami, 'fd')
            t = os.path.join(ROOT, 'src/amiga/lib_lvos.txt')
            ok &= check_lvos(t, 'bsdsocket', os.path.join(fdd, 'socket_lib.fd'))
            ok &= check_lvos(t, 'miami', os.path.join(fdd, 'miami_lib.fd'), allow_extra=('MiamiPrivate53',))
            ok &= check_lvos(t, 'usergroup', os.path.join(fdd, 'usergroup_lib.fd'))
            m = [f for f in gen_lvos.parse(t)['miami'].funcs]
            if max(f.bias for f in m) != 522:
                print('FAIL miami: table must extend to -522')
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
            fdd = os.path.join(amitcp, 'netinclude', 'fd')
            if os.path.isdir(fdd):
                t = os.path.join(ROOT, 'src/amiga/lib_lvos.txt')
                for lib, fdn in (('bsdsocket', 'bsdsocket_lib.fd'), ('usergroup', 'usergroup_lib.fd')):
                    if os.path.exists(os.path.join(fdd, fdn)):
                        ok &= check_lvos(t, lib, os.path.join(fdd, fdn))
        t = os.path.join(ROOT, 'src/amiga/os/os_lvos.txt')
        for lib, fdn in (('exec', 'exec_lib.fd'), ('dos', 'dos_lib.fd'), ('timer', 'timer_lib.fd')):
            ok &= check_lvos(t, lib, os.path.join(ndk, 'FD', fdn))
    print('ALL OK' if ok else 'MISMATCHES FOUND')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
