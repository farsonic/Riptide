/*
 * Riptide - structures and constants of the AmiTCP-style application ABI
 * (written from the documented API; tests/amiga/check_layouts.sh compares
 * them with the SDK headers).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_AMIGA_NET_H
#define RT_AMIGA_NET_H

/* needs the exec types: include amiga_os.h (or os/exec.h) first */

/* ---- netdb ---- */
struct hostent {
	char  *h_name;
	char **h_aliases;
	LONG   h_addrtype;
	LONG   h_length;
	char **h_addr_list;
};
struct netent {
	char  *n_name;
	char **n_aliases;
	LONG   n_addrtype;
	ULONG  n_net;
};
struct servent {
	char  *s_name;
	char **s_aliases;
	LONG   s_port;           /* network order */
	char  *s_proto;
};
struct protoent {
	char  *p_name;
	char **p_aliases;
	LONG   p_proto;
};
struct addrinfo {
	LONG   ai_flags;
	LONG   ai_family;
	LONG   ai_socktype;
	LONG   ai_protocol;
	ULONG  ai_addrlen;
	char  *ai_canonname;
	APTR   ai_addr;
	struct addrinfo *ai_next;
};
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3
#define NO_DATA        4

#define AI_PASSIVE     1
#define AI_CANONNAME   2
#define AI_NUMERICHOST 4
#define NI_NOFQDN      1
#define NI_NUMERICHOST 2
#define NI_NAMEREQD    4
#define NI_NUMERICSERV 8
#define NI_DGRAM       16
#define EAI_ADDRFAMILY 1
#define EAI_AGAIN      2
#define EAI_BADFLAGS   3
#define EAI_FAIL       4
#define EAI_FAMILY     5
#define EAI_MEMORY     6
#define EAI_NODATA     7
#define EAI_NONAME     8
#define EAI_SERVICE    9
#define EAI_SOCKTYPE   10
#define EAI_SYSTEM     11

/* ---- sockets ---- */
struct iovec {
	APTR  iov_base;
	ULONG iov_len;
};
struct msghdr {
	APTR  msg_name;
	ULONG msg_namelen;
	struct iovec *msg_iov;
	ULONG msg_iovlen;
	APTR  msg_control;
	ULONG msg_controllen;
	LONG  msg_flags;
};
struct cmsghdr {
	ULONG cmsg_len;
	LONG  cmsg_level;
	LONG  cmsg_type;
};

struct if_nameindex {
	ULONG if_index;
	char *if_name;
};

/* ---- SocketBaseTagList ---- */
#define SBTF_VAL 0x0000
#define SBTF_REF 0x8000
#define SBTB_CODE 1
#define SBTS_CODE 0x3FFF
#define SBTF_GET 0x0
#define SBTF_SET 0x1
#define SBTM_GETREF(code) (TAG_USER | SBTF_REF | (((code) & SBTS_CODE) << SBTB_CODE))
#define SBTM_GETVAL(code) (TAG_USER | (((code) & SBTS_CODE) << SBTB_CODE))
#define SBTM_SETREF(code) (TAG_USER | SBTF_REF | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_SET)
#define SBTM_SETVAL(code) (TAG_USER | (((code) & SBTS_CODE) << SBTB_CODE) | SBTF_SET)

#define SBTC_BREAKMASK      1
#define SBTC_SIGIOMASK      2
#define SBTC_SIGURGMASK     3
#define SBTC_SIGEVENTMASK   4
#define SBTC_ERRNO          6
#define SBTC_HERRNO         7
#define SBTC_DTABLESIZE     8
#define SBTC_FDCALLBACK     9
#define SBTC_LOGSTAT        10
#define SBTC_LOGTAGPTR      11
#define SBTC_LOGFACILITY    12
#define SBTC_LOGMASK        13
#define SBTC_ERRNOSTRPTR    14
#define SBTC_HERRNOSTRPTR   15
#define SBTC_IOERRNOSTRPTR  16
#define SBTC_S2ERRNOSTRPTR  17
#define SBTC_S2WERRNOSTRPTR 18
#define SBTC_ERRNOBYTEPTR   21
#define SBTC_ERRNOWORDPTR   22
#define SBTC_ERRNOLONGPTR   24
#define SBTC_HERRNOLONGPTR  25
#define SBTC_RELEASESTRPTR  29

#define FDCB_FREE  0
#define FDCB_ALLOC 1
#define FDCB_CHECK 2

/* syslog */
#define LOG_EMERG   0
#define LOG_ALERT   1
#define LOG_CRIT    2
#define LOG_ERR     3
#define LOG_WARNING 4
#define LOG_NOTICE  5
#define LOG_INFO    6
#define LOG_DEBUG   7
#define LOG_PRIMASK 0x07
#define LOG_USER    (1 << 3)
#define LOG_FACMASK 0x03f8
#define LOG_PID     0x01
#define LOG_UPTO(pri) ((1 << ((pri) + 1)) - 1)

/* riptide.library */
#define RIPTIDE_LIB_NAME     "riptide.library"
#define RIPTIDE_LIB_VERSION  1
#define RIPTIDE_LIB_REVISION 0
#define RIPTIDE_LIB_VERSTR   "1.0"
/* RiptideSupportedCPUs()/RiptideSetFdCallback() CPU types */
#define RIPTIDECPU_M68KREG 1
#define RIPTIDECPU_PPCV4   2
/* The compat alias of riptide.library: the name and version under which
 * programs written for the historical API it implements open it
 * (docs/AMIGA.md, "Compat alias"). */
#define RT_COMPAT_LIBNAME    "miami.library"
#define RT_COMPAT_LIBVERSION 21
/* where the historical stack installs its MNI drivers; searched after
 * LIBS: and DEVS:Networks/ for an mni= name without a path */
#define RT_COMPAT_MNI_DIR    "Miami:MNI/"

/* ---- usergroup.library ---- */
struct passwd {
	char *pw_name;
	char *pw_passwd;
	LONG  pw_uid;
	LONG  pw_gid;
	char *pw_gecos;
	char *pw_dir;
	char *pw_shell;
};
struct group {
	char  *gr_name;
	char  *gr_passwd;
	LONG   gr_gid;
	char **gr_mem;
};
#define NGROUPS    32
#define MAXLOGNAME 32
struct UserGroupCredentials {
	LONG  cr_ruid;
	LONG  cr_rgid;
	UWORD cr_umask;
	UWORD cr_pad1;
	LONG  cr_euid;
	WORD  cr_ngroups;
	WORD  cr_pad2;
	LONG  cr_groups[NGROUPS];
	struct Task *cr_session;
	char  cr_login[MAXLOGNAME];
};
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 64
struct utmp {
	LONG ut_time;
	LONG ut_sid;
	char ut_name[UT_NAMESIZE];
	char ut_host[UT_HOSTSIZE];
};
struct lastlog {
	LONG ll_time;
	LONG ll_uid;
	char ll_name[UT_NAMESIZE];
	char ll_host[UT_HOSTSIZE];
};
#define UGT_ERRNOBPTR 0x80000001
#define UGT_ERRNOWPTR 0x80000002
#define UGT_ERRNOLPTR 0x80000004
#define UGT_INTRMASK  0x80000010
#define UGT_OWNER     0x80000011

#endif
