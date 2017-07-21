/*
 * OSF/Alpha system-call "errno" values
 *
 * Jeff Brown
 * $Id: syscalls-private-errno.h,v 1.1.2.1 2006/03/22 02:45:21 jbrown Exp $
 */

#ifndef SYSCALLS_PRIVATE_ERRNO_H
#define SYSCALLS_PRIVATE_ERRNO_H
  
#define alpha_ESUCCESS 0
#define alpha_EPERM 1
#define alpha_ENOENT 2
#define alpha_ESRCH 3
#define alpha_EINTR 4
#define alpha_EIO 5
#define alpha_ENXIO 6
#define alpha_E2BIG 7
#define alpha_ENOEXEC 8
#define alpha_EBADF 9
#define alpha_ECHILD 10
#define alpha_EDEADLK 11
#define alpha_ENOMEM 12
#define alpha_EACCES 13
#define alpha_EFAULT 14
#define alpha_ENOTBLK 15
#define alpha_EBUSY 16
#define alpha_EEXIST 17
#define alpha_EXDEV 18
#define alpha_ENODEV 19
#define alpha_ENOTDIR 20
#define alpha_EISDIR 21
#define alpha_EINVAL 22
#define alpha_ENFILE 23
#define alpha_EMFILE 24
#define alpha_ENOTTY 25
#define alpha_ETXTBSY 26
#define alpha_EFBIG 27
#define alpha_ENOSPC 28
#define alpha_ESPIPE 29
#define alpha_EROFS 30
#define alpha_EMLINK 31
#define alpha_EPIPE 32
#define alpha_EDOM 33
#define alpha_ERANGE 34
#define alpha_EWOULDBLOCK 35
#define alpha_EAGAIN alpha_EWOULDBLOCK
#define alpha_EINPROGRESS 36
#define alpha_EALREADY 37
#define alpha_ENOTSOCK 38
#define alpha_EDESTADDRREQ 39
#define alpha_EMSGSIZE 40
#define alpha_EPROTOTYPE 41
#define alpha_ENOPROTOOPT 42
#define alpha_EPROTONOSUPPORT 43
#define alpha_ESOCKTNOSUPPORT 44
#define alpha_EOPNOTSUPP 45
#define alpha_EPFNOSUPPORT 46
#define alpha_EAFNOSUPPORT 47
#define alpha_EADDRINUSE 48
#define alpha_EADDRNOTAVAIL 49
#define alpha_ENETDOWN 50
#define alpha_ENETUNREACH 51
#define alpha_ENETRESET 52
#define alpha_ECONNABORTED 53
#define alpha_ECONNRESET 54
#define alpha_ENOBUFS 55
#define alpha_EISCONN 56
#define alpha_ENOTCONN 57
#define alpha_ESHUTDOWN 58
#define alpha_ETOOMANYREFS 59
#define alpha_ETIMEDOUT 60
#define alpha_ECONNREFUSED 61
#define alpha_ELOOP 62
#define alpha_ENAMETOOLONG 63
#define alpha_EHOSTDOWN 64
#define alpha_EHOSTUNREACH 65
#define alpha_ENOTEMPTY 66
#define alpha_EPROCLIM 67
#define alpha_EUSERS 68
#define alpha_EDQUOT 69
#define alpha_ESTALE 70
#define alpha_EREMOTE 71
#define alpha_EBADRPC 72
#define alpha_ERPCMISMATCH 73
#define alpha_EPROGUNAVAIL 74
#define alpha_EPROGMISMATCH 75
#define alpha_EPROCUNAVAIL 76
#define alpha_ENOLCK 77
#define alpha_ENOSYS 78
#define alpha_EFTYPE 79
#define alpha_ENOMSG 80
#define alpha_EIDRM 81
#define alpha_ENOSR 82
#define alpha_ETIME 83
#define alpha_EBADMSG 84
#define alpha_EPROTO 85
#define alpha_ENODATA 86
#define alpha_ENOSTR 87
#define alpha_EDIRTY 89
#define alpha_EDUPPKG 90
#define alpha_EVERSION 91
#define alpha_ENOPKG 92
#define alpha_ENOSYM 93
#define alpha_ECANCELED 94
#define alpha_EFAIL 95
#define alpha_EINPROG 97
#define alpha_EMTIMERS 98
#define alpha_ENOTSUP 99
#define alpha_EAIO 100
#define alpha_EMULTIHOP 101
#define alpha_ENOLINK 102
#define alpha_EOVERFLOW 103
#define alpha_EILSEQ 116
  

// Kernel internal
#define alpha_ECLONEME 88
#define alpha_ESOFT 123
#define alpha_EMEDIA 124
#define alpha_ERELOCATED 125
#define alpha_ERESTART (-1)
#define alpha_EJUSTRETURN (-2)
#define alpha_EEMULATE (-3)
  

#endif  /* SYSCALLS_PRIVATE_ERRNO_H */
