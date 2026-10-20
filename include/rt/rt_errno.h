/*
 * Riptide - error numbers. Values are the 4.4BSD/AmiTCP ones so that they
 * can be handed to Amiga applications unchanged. Internally functions return
 * negative values (-RT_EWOULDBLOCK etc), Linux style.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef RT_ERRNO_H
#define RT_ERRNO_H

#define RT_EPERM            1
#define RT_ENOENT           2
#define RT_ESRCH            3
#define RT_EINTR            4
#define RT_EIO              5
#define RT_ENXIO            6
#define RT_E2BIG            7
#define RT_EBADF            9
#define RT_ENOMEM          12
#define RT_EACCES          13
#define RT_EFAULT          14
#define RT_EBUSY           16
#define RT_EEXIST          17
#define RT_ENODEV          19
#define RT_EINVAL          22
#define RT_ENFILE          23
#define RT_EMFILE          24
#define RT_ENOTTY          25
#define RT_ENOSPC          28
#define RT_EPIPE           32
#define RT_EDOM            33
#define RT_ERANGE          34
#define RT_EAGAIN          35
#define RT_EWOULDBLOCK     RT_EAGAIN
#define RT_EINPROGRESS     36
#define RT_EALREADY        37
#define RT_ENOTSOCK        38
#define RT_EDESTADDRREQ    39
#define RT_EMSGSIZE        40
#define RT_EPROTOTYPE      41
#define RT_ENOPROTOOPT     42
#define RT_EPROTONOSUPPORT 43
#define RT_ESOCKTNOSUPPORT 44
#define RT_EOPNOTSUPP      45
#define RT_EPFNOSUPPORT    46
#define RT_EAFNOSUPPORT    47
#define RT_EADDRINUSE      48
#define RT_EADDRNOTAVAIL   49
#define RT_ENETDOWN        50
#define RT_ENETUNREACH     51
#define RT_ENETRESET       52
#define RT_ECONNABORTED    53
#define RT_ECONNRESET      54
#define RT_ENOBUFS         55
#define RT_EISCONN         56
#define RT_ENOTCONN        57
#define RT_ESHUTDOWN       58
#define RT_ETOOMANYREFS    59
#define RT_ETIMEDOUT       60
#define RT_ECONNREFUSED    61
#define RT_ELOOP           62
#define RT_ENAMETOOLONG    63
#define RT_EHOSTDOWN       64
#define RT_EHOSTUNREACH    65
#define RT_ENOTEMPTY       66
#define RT_ENOSYS          78

const char *rt_strerror(int err);  /* positive errno -> text */

#endif
