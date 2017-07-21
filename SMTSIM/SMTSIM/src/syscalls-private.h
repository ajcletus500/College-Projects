/* SMTSIM simulator.
   
   Copyright (C) 1994-1999 by Dean Tullsen (tullsen@cs.ucsd.edu)
   ALL RIGHTS RESERVED.

   SMTSIM is distributed under the following conditions:

     You may make copies of SMTSIM for your own use and modify those copies.

     All copies of SMTSIM must retain all copyright notices contained within.

     You may not sell SMTSIM or distribute SMTSIM in conjunction with a
     commerical product or service without the express written consent of
     Dean Tullsen.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.

Significant parts of the SMTSIM simulator were written by Gun Sirer
(before it became the SMTSIM simulator) and by Jack Lo (after it became
the SMTSIM simulator).  Therefore the following copyrights may also apply:

Copyright (C) Jack Lo
Copyright (C) E. Gun Sirer

Pieces of this code may have been derived from Jim Larus\' SPIM simulator,
which contains the following copyright:

==============================================================
   Copyright (C) 1990-1998 by James Larus (larus@cs.wisc.edu).
   ALL RIGHTS RESERVED.

   SPIM is distributed under the following conditions:

     You may make copies of SPIM for your own use and modify those copies.

     All copies of SPIM must retain my name and copyright notice.

     You may not sell SPIM or distributed SPIM in conjunction with a
     commerical product or service without the expressed written consent of
     James Larus.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.
===============================================================
 */


/* Description of (most) system calls so SMTSIM can translate the arguments
   when it needs to really execute the system call. */

//#ifdef __alpha
//#define SYS_sigreturn  103
//#define SYS_sigcleanup 139
//#define SYS_sysmips    151
//#define SYS_cacheflush 152
//#define SYS_cachectl   153
//#define SYS_atomic_op  155
//#endif


// Utility code shared between syscall-implementing modules

struct ProgMem;
struct stat;
struct flock;
struct dirent;

// Convert a host "struct stat" value into alpha-format, and store it into
// simulated memory.
void syscalls_store_alpha_stat(SyscallState *ssr,
                               ProgMem *pmem, mem_addr va,
                               const struct stat *host_stat);

// Convert a host "struct flock" to and from alpha-format, writing or reading
// simulated memory.
void syscalls_load_alpha_flock(ProgMem *pmem, struct flock *host_flock_ret,
                               mem_addr src_va);
void syscalls_store_alpha_flock(ProgMem *pmem, mem_addr dst_va,
                                const struct flock *host_flock);

// Returns #bytes stored, or -1 if buffer too small
i64 syscalls_store_alpha_dirent(SyscallState *sst,
                                ProgMem *pmem, mem_addr va,
                                i64 buf_bytes_remaining,
                                const struct dirent *host_dirent);


/* the structure the Alpha expects "stat" to return */

//struct  alphastat
//{
//      // The "sim_" prefix is to avoid a name conflict with some OS macros
//      // (particularly st_atime/mtime/ctime).  No, I'm not kidding.
//      int     sim_st_dev;
//      int     sim_st_ino;     
//      int     sim_st_mode;
//      int     sim_st_nlink;
//      int     sim_st_uid;
//      int     sim_st_gid;
//      int     sim_st_rdev;
//        int     sim_pad;
//      int     sim_st_size;
//        int     sim_pad2;
//      int     sim_st_atime;           /* Time of last access */
//      int     sim_st_spare1;
//      int     sim_st_mtime;           /* Time of last data modification */
//      int     sim_st_spare2;
//      int     sim_st_ctime;           /* Time of last file status change */
//      int     sim_st_spare3;
//
//      int     sim_st_blksize;         /* Size of block in file */
//        int     sim_st_blocks;              /* blocks allocated for file */
//
//        int  sim_st_flags;               /* user defined flags for file */
//        int  sim_st_gen;                 /* file generation number */
//
//};
//                      /* End of the stat structure */

/*  The structure the Alpha expects a statfs call to return */
//struct alphastatfs {          
//0     short   sim_f_type;             /* type of filesystem (see below) */
//2     short   sim_f_flags;            /* copy of mount flags */
//4     long    sim_f_fsize;            /* fundamental filesystem block size */
//      long    sim_f_bsize;            /* optimal transfer block size */
//      long    sim_f_blocks;           /* total data blocks in file system, */
//                                      /*  note: may not represent fs size. */
//      long    sim_f_bfree;            /* free blocks in fs */
//      long    sim_f_bavail;           /* free blocks avail to non-su */
//      long    sim_f_files;            /* total file nodes in file system */
//      long    sim_f_ffree;            /* free file nodes in fs */
//      long    sim_f_fsid1;                    /* file system id */
//      long    sim_f_fsid2;                    /* file system id */
//      long    sim_f_spare[9];         /* spare for later */
//      char    sim_f_mntonname[90];    /* directory on which mounted */
//      char    sim_f_mntfromname[90];/* mounted filesystem */
//      /* union mount_info mount_info;  mount options */
//};

/*  The structure the Alpha expects a getdirentries call to return */

//struct  alphadirent {
//        int    sim_d_ino;               /* file number of entry */
//        short sim_d_reclen;            /* length of this record */
//        short sim_d_namlen;            /* length of string in sim_d_name */
//        char    sim_d_name[256];              /* DUMMY NAME LENGTH */
//                                      /* the real maximum length is */
//                                      /* returned by pathconf() */
//                                      /* At this time, this MUST */
//                                      /* be 256 -- the kernel */
//                                      /* requires it */
//};


/* file open flags for Alpha OSF */
/* File status flags accessible to open(2) and fcntl(2) */
#define Alpha_O_NONBLOCK 00000004        /* non-blocking I/O, POSIX style */
#define Alpha_O_APPEND   00000010        /* append (writes guaranteed at end) */

/* Flag values accessible to open(2) */
#define Alpha_O_RDONLY 00000000
#define Alpha_O_WRONLY 00000001
#define Alpha_O_RDWR      00000002
 
/* Flag values accessible only to open(2) */
#define Alpha_O_CREAT     00001000    /* open with create (uses third arg)*/
#define Alpha_O_TRUNC     00002000    /* open with truncation            */
#define Alpha_O_EXCL      00004000    /* exclusive open                  */
#define Alpha_O_NOCTTY 00010000    /* POSIX REQUIRED */

// lseek(2) "whence" values
#define Alpha_SEEK_SET   0
#define Alpha_SEEK_CUR   1
#define Alpha_SEEK_END   2

// fcntl(2) constants
// Requests
#define Alpha_F_DUPFD  0        // duplicate FD
#define Alpha_F_GETFD  1        // get file descriptor flags (i. FD_CLOEXEC)
#define Alpha_F_SETFD  2        // set file descriptor flags (i.e. FD_CLOEXEC)
#define Alpha_F_GETFL  3        // get file flags
#define Alpha_F_SETFL  4        // set file flags
#define Alpha_F_GETOWN 5        // get async I/O process/group for signalling
#define Alpha_F_SETOWN 6        // set async I/O process/group for signalling
#define Alpha_F_GETLK  7        // get first-lock-that-would-block
#define Alpha_F_SETLK  8        // set lock (don't wait)
#define Alpha_F_SETLKW 9        // set lock (wait for lock)
// Requests used by OSF's NFS lock manager
#define Alpha_F_RGETLK   10     // test remote lock
#define Alpha_F_RSETLK   11     // set/clear remote lock
#define Alpha_F_CNVT     12     // convert fhandle to fd
#define Alpha_F_RSETLKW  13     // set/clear remote lock (wait for lock)
#define Alpha_F_PURGEFS  14     // "Purge locks on fs (for ASE product)"
#define Alpha_F_PURGENFS 15     // "for DECsafe Product (for Digital Use Only)"
// Additional commands
#define Alpha_F_GETTIMES 16     // get a/m/ctime(s) (arg format?)
#define Alpha_F_SETTIMES 17     // set file a/m/ctime(s) (arg format?)
#define Alpha_F_RWREFS   18     // "Get open refs"
#define Alpha_F_ADVFS_OP 19     // "AdvFS-specific operations"

// File segment lock-type constants
#define Alpha_F_RDLCK 1         // read lock
#define Alpha_F_WRLCK 2         // write lock
#define Alpha_F_UNLCK 8         // unlock
// File descriptor flags
#define Alpha_FD_CLOEXEC 1      // close-on-exec

// These are from squinting at the manpages, trying to figure what's allowed
#define Alpha_SETFD_allowed_mask (Alpha_FD_CLOEXEC)
#define Alpha_SETFL_allowed_mask (Alpha_O_NONBLOCK | Alpha_O_APPEND)


// For sigprocmask syscall
#define Alpha_SIG_BLOCK         1
#define Alpha_SIG_UNBLOCK       2
#define Alpha_SIG_SETMASK       3

// ioctl(2) values
#define Alpha_TIOCGETP  0x40067408
#define Alpha_TIOCGETS  0x402c7413
#define Alpha_TIOCGETA  0x40127417
#define Alpha_TIOCISATTY 0x2000745e
#define Alpha_SYS_NMLN  32


//struct alpha_utsname {
//    char sim_sysname[Alpha_SYS_NMLN];
//    char sim_nodename[Alpha_SYS_NMLN];
//    char sim_release[Alpha_SYS_NMLN];
//    char sim_version[Alpha_SYS_NMLN];
//    char sim_machine[Alpha_SYS_NMLN];
//};


// OSF/Alpha rusage struct:
// struct rusage {
//     struct timeval ru_utime;        /* user time used */
//     struct timeval ru_stime;        /* system time used */
//     long    ru_maxrss;
//     long    ru_ixrss;               /* integral shared memory size */
//     long    ru_idrss;               /* integral unshared data " */
//     long    ru_isrss;               /* integral unshared stack " */
//     long    ru_minflt;              /* page reclaims - total vmfaults */
//     long    ru_majflt;              /* page faults */
//     long    ru_nswap;               /* swaps */
//     long    ru_inblock;             /* block input operations */
//     long    ru_oublock;             /* block output operations */
//     long    ru_msgsnd;              /* messages sent */
//     long    ru_msgrcv;              /* messages received */
//     long    ru_nsignals;            /* signals received */
//     long    ru_nvcsw;               /* voluntary context switches */
//     long    ru_nivcsw;              /* involuntary " */
// };



///
// Summary of OSF/1 structure fields, sizes, and placement;
//

#define AlphaDirEntHeaderBytes  8
#define AlphaDirEntMaxBytes     264
#define Alpha_PC_NAME_MAX       255
#define Alpha_PC_PATH_MAX       1024
#define AlphaFDSetBytes         512
#define AlphaTimeValBytes       8
#define AlphaRLimitBytes        16
#define AlphaSigStackBytes      16
#define AlphaSigSetBytes        8
#define AlphaSigactionBytes     24
#define AlphaSigcontextBytes    648
#define AlphaStructStatBytes    80
#define AlphaStructFlockBytes   32
#define LinuxAlphaTimeValBytes  16

//sizeof(struct stat) = 80
//  4   @ 0   : st_dev
//  4   @ 4   : st_ino
//  4   @ 8   : st_mode
//  2   @ 12  : st_nlink
//  pad 2 @ 14
//  4   @ 16  : st_uid
//  4   @ 20  : st_gid
//  4   @ 24  : st_rdev
//  pad 4 @ 28
//  8   @ 32  : st_size
//  4   @ 40  : st_atime
//  4   @ 44  : st_spare1
//  4   @ 48  : st_mtime
//  4   @ 52  : st_spare2
//  4   @ 56  : st_ctime
//  4   @ 60  : st_spare3
//  4   @ 64  : st_blksize
//  4   @ 68  : st_blocks
//  4   @ 72  : st_flags
//  4   @ 76  : st_gen
//sizeof(struct statfs) = 336
//  2   @ 0   : f_type
//  2   @ 2   : f_flags
//  4   @ 4   : f_fsize
//  4   @ 8   : f_bsize
//  4   @ 12  : f_blocks
//  4   @ 16  : f_bfree
//  4   @ 20  : f_bavail
//  4   @ 24  : f_files
//  4   @ 28  : f_ffree
//  8   @ 32  : f_fsid
//  36  @ 40  : f_spare
//  90  @ 76  : f_mntonname
//  90  @ 166 : f_mntfromname
//  80  @ 256 : mount_info
//sizeof(struct dirent) = 264
//  4   @ 0   : d_ino
//  2   @ 4   : d_reclen
//  2   @ 6   : d_namlen
//  256 @ 8   : d_name
//sizeof(struct utsname) = 160
//  32  @ 0   : sysname
//  32  @ 32  : nodename
//  32  @ 64  : release
//  32  @ 96  : version
//  32  @ 128 : machine
//sizeof(struct timeval) = 8
//  4   @ 0   : tv_sec
//  4   @ 4   : tv_usec
//sizeof(struct rusage) = 128
//  8   @ 0   : ru_utime
//  8   @ 8   : ru_stime
//  8   @ 16  : ru_maxrss
//  8   @ 24  : ru_ixrss
//  8   @ 32  : ru_idrss
//  8   @ 40  : ru_isrss
//  8   @ 48  : ru_minflt
//  8   @ 56  : ru_majflt
//  8   @ 64  : ru_nswap
//  8   @ 72  : ru_inblock
//  8   @ 80  : ru_oublock
//  8   @ 88  : ru_msgsnd
//  8   @ 96  : ru_msgrcv
//  8   @ 104 : ru_nsignals
//  8   @ 112 : ru_nvcsw
//  8   @ 120 : ru_nivcsw
//sizeof(struct rlimit) = 16
//  8   @ 0   : rlim_cur
//  8   @ 8   : rlim_max
//sizeof(struct sigstack) = 16
//  8   @ 0   : ss_sp
//  4   @ 8   : ss_onstack
//  pad 4 @ 12
//sizeof(struct aouthdr) = 80
//  2   @ 0   : magic
//  2   @ 2   : vstamp
//  2   @ 4   : bldrev
//  2   @ 6   : padcell
//  8   @ 8   : tsize
//  8   @ 16  : dsize
//  8   @ 24  : bsize
//  8   @ 32  : entry
//  8   @ 40  : text_start
//  8   @ 48  : data_start
//  8   @ 56  : bss_start
//  4   @ 64  : gprmask
//  4   @ 68  : fprmask
//  8   @ 72  : gp_value
//sizeof(struct filehdr) = 24
//  2   @ 0   : f_magic
//  2   @ 2   : f_nscns
//  4   @ 4   : f_timdat
//  8   @ 8   : f_symptr
//  4   @ 16  : f_nsyms
//  2   @ 20  : f_opthdr
//  2   @ 22  : f_flags
//sizeof(struct exec) = 104
//  24  @ 0   : ex_f
//  80  @ 24  : ex_o
//sizeof(struct sigaction) = 24
//  // _sa_un is a union of two function pointers (_handler, _sigaction)
//  8   @ 0   : _sa_un
//  8   @ 8   : sa_mask
//  4   @ 16  : sa_flags
//  4   @ 20  : sa_signo
//sizeof(struct sigcontext) = 648
//  8   @ 0   : sc_onstack
//  8   @ 8   : sc_mask
//  8   @ 16  : sc_pc
//  8   @ 24  : sc_ps
//  // regs[32]
//  256 @ 32  : sc_regs
//  8   @ 288 : sc_ownedfp
//  // fpregs[32]
//  256 @ 296 : sc_fpregs
//  8   @ 552 : sc_fpcr
//  8   @ 560 : sc_fp_control
//  8   @ 568 : sc_reserved1
//  4   @ 576 : sc_kreserved1
//  4   @ 580 : sc_kreserved2
//  8   @ 584 : sc_ssize
//  8   @ 592 : sc_sbase
//  8   @ 600 : sc_traparg_a0
//  8   @ 608 : sc_traparg_a1
//  8   @ 616 : sc_traparg_a2
//  8   @ 624 : sc_fp_trap_pc
//  8   @ 632 : sc_fp_trigger_sum
//  8   @ 640 : sc_fp_trigger_inst
//sizeof(struct tbl_sysinfo) = 64
//  8   @ 0   : si_user
//  8   @ 8   : si_nice
//  8   @ 16  : si_sys
//  8   @ 24  : si_idle
//  8   @ 32  : si_hz
//  8   @ 40  : si_phz
//  8   @ 48  : si_boottime
//  8   @ 56  : wait
//sizeof(struct flock) = 32
//  2   @ 0   : l_type
//  2   @ 2   : l_whence
//  pad 4 @ 4
//  8   @ 8   : l_start
//  8   @ 16  : l_len
//  4   @ 24  : l_pid
//  pad 4 @ 28


// SYS_table syscall constants
// (From olympic.ucsd.edu:/usr/include/sys/table.h , OSF1 V4.0)
#define Alpha_TBL_TTYLOC                0       /* index by device number */
#define Alpha_TBL_U_TTYD                1       /* index by process ID */
#define Alpha_TBL_UAREA                 2       /* index by process ID */
#define Alpha_TBL_LOADAVG               3       /* (no index) */
#define Alpha_TBL_INCLUDE_VERSION       4       /* (no index) */
#define Alpha_TBL_FSPARAM               5       /* index by device number */
#define Alpha_TBL_ARGUMENTS             6       /* index by process ID */
#define Alpha_TBL_MAXUPRC               7       /* index by process ID */
#define Alpha_TBL_AID                   8       /* index by process ID */
#define Alpha_TBL_MODES                 9       /* index by process ID */
#define Alpha_TBL_PROCINFO              10      /* index by proc table slot */
#define Alpha_TBL_ENVIRONMENT           11      /* index by process ID */
#define Alpha_TBL_SYSINFO               12      /* (no index) */
#define Alpha_TBL_DKINFO                13      /* index by disk */
#define Alpha_TBL_TTYINFO               14      /* (no index) */
#define Alpha_TBL_MSGDS                 15      /* index by array index */
#define Alpha_TBL_SEMDS                 16      /* index by array index */
#define Alpha_TBL_SHMDS                 17      /* index by array index */
#define Alpha_TBL_MSGINFO               18      /* index by structure element */
#define Alpha_TBL_SEMINFO               19      /* index by structure element */
#define Alpha_TBL_SHMINFO               20      /* index by structure element */
#define Alpha_TBL_INTR                  21      /* (no index) */
#define Alpha_TBL_SWAPINFO              22      /* index by vm_swap element */
#define Alpha_TBL_SCALLS                23      /* system call info table (no index) */
#define Alpha_TBL_FILEINFO              24      /* file access status (no index) */
#define Alpha_TBL_TBLSTATS              25      /* system tables status (no index) */
#define Alpha_TBL_RUNQ                  26      /* run queue status (no index) */
#define Alpha_TBL_BUFFER                27      /* buffer activity (no index) */
#define Alpha_TBL_KMEM                  28      /* kernel memory activity (no index) */
#define Alpha_TBL_PAGING                29      /* paging activity (no index) */
#define Alpha_TBL_MALLOCT               30      /* same as TBL_MALLOCTYPES */
#define Alpha_TBL_MAPINFO               31      /* process address map */
#define Alpha_TBL_MALLOCBUCKETS         32      /* get malloc allocator kmembuckets */
#define Alpha_TBL_MALLOCTYPES           33      /* get malloc allocator kmemtypes */
#define Alpha_TBL_MALLOCNAMES           34      /* get malloc memory type names */
#define Alpha_TBL_KNLIST                35      /* get kernel namelist table */
#define Alpha_TBL_NETINFO               36      /* get netisr info */
#define Alpha_TBL_MALLOCLEAK            37      /* get malloc leak info */
#define Alpha_TBL_MALLOCLEAKCOUNT       38      /* get malloc leak count */
#define Alpha_TBL_MALLOCNTYPES          39      /* get number of malloc types - M_LAST*/
#define Alpha_TBL_MALLOCNBUCKETS        40      /* get number of malloc buckets*/
#define Alpha_TBL_PMEMSTATS             41      /* physical memory stats */
#define Alpha_TBL_PMEMCLUSTERTYPE       42      /* physical memory cluster type */
#define Alpha_TBL_PMEMUSETYPE           43      /* physical memory use type */
#define Alpha_TBL_PMEMUSAGE             44      /* physical memory usage */
#define Alpha_TBL_PMEMCLUSTERS          45      /* physical memory clusters */
#define Alpha_TBL_VMSTATS               46      /* vmstat */
#define Alpha_TBL_MALLOCMSTATS          47      /* malloc */
#define Alpha_TBL_MACHINE_SLOT          48      /* machine slot */
#define Alpha_TBL_SYSINFO_MP            49
#define Alpha_TBL_BUFFER_MP             50
#define Alpha_TBL_SCALLS_MP             51
#define Alpha_TBL_FILEINFO_MP           52
#define Alpha_TBL_PAGING_MP             53
#define Alpha_TBL_INTR_MP               54
#define Alpha_TBL_TCPCONN               55      /* set(delteTCB) tcp conn state */
#define Alpha_TBL_UIDINFO               56      /* get uid_max; ignore arguments */
#define Alpha_TBL_PROCESSOR_STATS_INFO  58      /* Get all processor_stats structs */
#define Alpha_Alpha_TBL_UMAST_INFO      60      /* Get unix_master_info */
#define Alpha_Alpha_TBL_XCPU_INTR_MP    61      /* Get per-CPU TB shootdown xintrs */
#define Alpha_TBL_PROCESSOR_INFO        62      /* Get all tbl_processor structs */
