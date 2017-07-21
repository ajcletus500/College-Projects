//
// "syscalls" module, host OS #includes
//
// This is pretty nasty, and should one day go away when system calls get
// implemented directly.
//
// Jeff Brown
// $Id: syscalls-includes.h,v 1.1.2.6.2.1.2.3.4.1 2009/12/25 06:31:51 jbrown Exp $
//

#ifndef SYSCALLS_INCLUDES_H
#define SYSCALLS_INCLUDES_H


#ifdef __FreeBSD__
#  include <sys/types.h>
#  include <sys/time.h>
#  include <sys/signal.h>
#  include <dirent.h>
#  include <sys/param.h>
#  include <sys/ioctl.h>
#  include <sys/stat.h>
#  define NATIVE_DIRENT_FILENUM d_fileno
#else
#  define NATIVE_DIRENT_FILENUM d_ino
#endif

#include <assert.h>
#include <stdarg.h>
//#define _BSD_SIGNALS  1
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/mount.h>

#ifdef __alpha
#  include <stropts.h>
#  include <termios.h>
#  include <ustat.h>
#  include <dirent.h>
   /* These aren't in any of our alpha's /usr/include files */
    extern "C" {
        int getdirentries(int fd, char *buf, int nbytes, long *basep);
        // This causes an unavoidable warning in C++ compilation: the name of
        // the statfs function matches that of the implicit default
        // constructor for the statfs struct.
        int statfs(char *path, struct statfs *buffer);
    }
#endif

#ifdef __linux__
#  include <dirent.h>
#  include <sys/vfs.h>
#endif

#ifdef __APPLE__
#  include <sys/ioctl.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <map>

#endif  // SYSCALLS_INCLUDES_H
