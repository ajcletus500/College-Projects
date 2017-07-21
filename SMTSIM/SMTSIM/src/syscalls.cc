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

#include "syscalls-includes.h"          // Host system #includes

// I'm trying to avoid any inclusion of the "context" structure definition,
// to prevent state leakage.
#include "sys-types.h"
#include "sim-assert.h"
#include "syscalls.h"
#include "app-state.h"
#include "prog-mem.h"
#include "sim-cfg.h"
#include "sim-params.h"
#include "utils.h"
#include "utils-cc.h"
#include "callback-queue.h"
#include "syscalls-sim-fd.h"
#include "debug-coverage.h"

#include "syscalls-private.h"
#include "syscalls-private-nums.h"
#include "syscalls-private-errno.h"
//Added by Hung-Wei
struct osf_iovec
{
  mem_addr iov_base;		/* starting address */
  unsigned int iov_len;		/* length in bytes */
  unsigned int pad;
};
u64 mmap_end;
//end


using std::map;
using std::set;
using std::string;
using std::vector;

using namespace SimCfg;

#define ENABLE_COVERAGE_SYSCALLS 0

#define CLOCK_RATE_HZ           1e9     // should be a knob in smtsim.conf

#define ALPHA_OSF_MAXFDS        4096
#define ALPHA_OSF_FD_SETSIZE    4096
#define ALPHA_OSF_NSIG          (48 + 1)


int SysTrace = 0;


namespace {

#define PERFORM_COVERAGE_SYSCALLS (ENABLE_COVERAGE_SYSCALLS && defined(DEBUG))
#if PERFORM_COVERAGE_SYSCALLS
    const char *SyscallsCoverageNames[] = {
        // Sorted by syscall name (except for error cases, at bottom)
        "close",
        "exit",
        "fcntl",
        "fstat",
        "fsync",
        "ftruncate",
        "getdirentries",
        "getgid",
        "getgroups",
        "getpagesize",
        "getpid",
        "getrlimit",
        "getrusage",
        "gettimeofday",
        "getuid",
        "ioctl",
        "lseek",
        "lstat",
        "new_fstat",
        "new_lstat",
        "new_stat",
        "new_statfs",
        "obreak",
        "open",
        "read",
        "rename",
        "sbrk",
        "select",
        "setitimer",
        "setrlimit",
        "setsysinfo",
        "sigaction",
        "sigprocmask",
        "sigreturn",
        "sigstack",
        "stat",
        "statfs",
        "syscall",
        "table",
        "umask",
        "uname",
        "unlink",
        "uswitch",
        "write",
        "out-of-range",
        "bad",
        "unimplemented-unix",
        "unimplemented-special",
        "unknown-type",
        NULL
    };
    DebugCoverageTracker SyscallsCoverage("Syscalls", SyscallsCoverageNames,
                                          true);
    #define COVERAGE_SYSCALL(point) SyscallsCoverage.reached(point)
#else
    #define COVERAGE_SYSCALL(point) ((void) 0)
#endif  // PERFORM_COVERAGE_SYSCALLS


struct SyscallConfig {
    bool report_resize_failures;
    bool uniquify_to_subdir;
    bool root_paths_at_cwd;
    bool sanitize_host_device_info;
    i64 debug_syscall_num;      // -1: none

    set<string> paths_to_uniquify;
    
    NoDefaultCopy nocopy;

    SyscallConfig(const string& cfg_path);
    ~SyscallConfig() { }
};


SyscallConfig::SyscallConfig(const string& cfg_path)
{
    //const char *fname = "SyscallConfig::SyscallConfig";
    string cp = cfg_path + "/";          // short-hand for config-path

    report_resize_failures = conf_bool(cp + "report_resize_failures");
    uniquify_to_subdir = conf_bool(cp + "uniquify_to_subdir");
    root_paths_at_cwd = conf_bool(cp + "root_paths_at_cwd");
    sanitize_host_device_info = conf_bool(cp + "sanitize_host_device_info");

    {
        // This is a global, but we share its values across all apps.
        // (Eh, it's really intended to help single-app debugging.)
        string key = "Debug/syscall";
        debug_syscall_num = (have_conf(key)) ? conf_i64(key) : -1;
    }

    conf_read_keys(cp + "ForceUniqueNames", &paths_to_uniquify);
}


typedef set<SimulatedFD *> SimulatedFDSet;


// Simple class to manage mapping of various host-dependent IDs (e.g.
// inode numbers) to a consistent set of values for simulation.
class SimulatedIDPool {
    typedef map<u64, u64> IDMap;
    IDMap host_to_alpha_;
    IDMap alpha_to_host_;       // reverse map: may come in handy later
    u64 next_alloc_id_;
    NoDefaultCopy nocopy;
public:
    SimulatedIDPool(u64 base_value);
    u64 host_to_alpha(u64 host_id);
};


SimulatedIDPool::SimulatedIDPool(u64 base_value)
    : next_alloc_id_(base_value)
{
}


u64
SimulatedIDPool::host_to_alpha(u64 host_id)
{
    u64 result;
    if (u64 *found = map_find(host_to_alpha_, host_id)) {
        result = *found;
    } else {
        result = next_alloc_id_;
        ++next_alloc_id_;
        sim_assert(next_alloc_id_ > result);
        map_put_uniq(host_to_alpha_, host_id, result);
        map_put_uniq(alpha_to_host_, result, host_id);
    }
    return result;
}


}       // Anonymous namespace close


struct SyscallState {
    class FmtHereCB;

    const SyscallConfig conf;
    AppState *as;

    // syscall's clock is decoupled from simulator "cyc", in case we need to
    // run it seperately.  (usually this will just be a copy of "cyc")
    i64 local_clock;
    scoped_ptr<FmtHereCB> fmt_here_cb;   // owned

    bool recording_delta_log;   // Flag: recording delta log of syscalls
    bool playing_delta_log;     // Flag: playing back delta log instead of
                                // actually making syscalls

    FILE *prog_stdin, *prog_stdout, *prog_stderr;       // not owned
    
    typedef map<int, SimulatedFD *> SimulatedFDMap;
    SimulatedFDMap valid_fds;           // owned pointers

    // storage for possibly-translated syscall pathname arguments
    string path_args[6];

    string initial_working_dir;
    // path from simulator's InitialWorkingDir to app's current working dir.
    // this isn't super-well-defined now, since we don't emulate chdir(2) yet.
    string local_path;

    u32 alpha_uid;
    u32 alpha_gid;
    u32 alpha_pid;

    SimulatedIDPool alpha_inode_nums;
    SimulatedIDPool alpha_dev_ids;

    struct {
        // (unclear, legacy stuff from long ago)
        // this struct gets memset to 0 at start
        unsigned char sigaction_array[ALPHA_OSF_NSIG][AlphaSigactionBytes];
        i64 sigstack;
        i32 sigonstack;
        u64 sigmask;
    } old_sig;

    struct {                    // State for delta-log recording
        FILE *dst;              // not owned
    } rec;

    // Delta-log playback: only valid if "playing_delta_log" is set
    struct {
        FILE *src;              // not owned
    } play;

    set<int> warned_missing_table_ops;

    SyscallState(const SyscallStateParams& params__,
                 struct AppState *nascent_astate__);
    ~SyscallState();

    string fmt_clock() const;   // for diagnostics: report local_clock
    string fmt_here() const;    // for diagnostics: report syscall#, clock, etc

    // selects a new simulated-FD number, or -1 if none available
    int fd_new_alpha_fdnum() const;
    // creates, numbers, and registers a new simulated FD; returns NULL
    // if unable (e.g. no alpha FDs available)
    SimulatedFD *fd_create();
    void fd_destroy(SimulatedFD *fd);   // must be valid & registered; deletes
    SimulatedFD *fd_lookup(i64 alpha_fd) const; // NULL <=> not found
};


namespace {

typedef enum {
    BAD_SYSCALL,
    UNIX_SYSCALL,
    SPC_SYSCALL,
    SyscallOutOfRange,
    SyscallType_last
} SyscallType;

typedef enum {
    NO_ARG,
    INT_ARG,            // generic register value (no translation)
    FD_ARG,             // integer file descriptor (gets translated)
    // ADDR_ arg-types must be either valid virtual addresses, or NULL.
    ADDR_R_ARG,         // virtual address, input (read-only)
    ADDR_W_ARG,         // virtual address, output (write-only)
    ADDR_RW_ARG,         // virtual address, i/o (may read and/or write)
    CSTR_ARG,           // NULL-terminated C string (or NULL)
    PATH_ARG,           // C string used as a file/dir name (may be edited)
    SyscallArgType_last
} SyscallArgType;  /* Type of argument */
extern const char *SyscallArgType_names[];

typedef struct SyscallDesc {
    int syscall_num;
    int syscall_type;
    SyscallArgType args[6];
    const char *syscall_name;
} SyscallDesc;

SyscallDesc syscall_table[] =
{
#include "syscalls-private-table.h"
};
#define MAX_SYSCALL     NELEM(syscall_table)


const char *SyscallArgType_names[] = { 
    "NO_ARG", "INT_ARG", "FD_ARG", "ADDR_R_ARG", "ADDR_W_ARG", "ADDR_RW_ARG",
    "CSTR_ARG", "PATH_ARG", NULL
};


// Register numbers from reg-defs.h
// REG_ERR=7: conflict with linux <ucontext.h>
enum {
    REG_V0 = AlphaReg_v0,
    REG_A0 = AlphaReg_a0,
    REG_A1 = AlphaReg_a1,
    REG_A2 = AlphaReg_a2,
    REG_A3 = AlphaReg_a3,
    REG_A4 = AlphaReg_a4,
    REG_A5 = AlphaReg_a5,
    REG_SP = AlphaReg_sp
};

// Dummy rusage struct, used to be:
//static int dummy_rusage[31] = {0,50000,0,50000,15000,0,30,0,2000,0,20,0,2000,
//                             0,0,0,0,0,1,0,4,0,2,0,0,0,2,0,250,0,30};
const u8 dummy_rusage[128] = 
{
  0x00, 0x00, 0x00, 0x00, 0x50, 0xc3, 0x00, 0x00, // ru_utime
  0x00, 0x00, 0x00, 0x00, 0x50, 0xc3, 0x00, 0x00, // ru_stime
  0x98, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_maxrss
  0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_ixrss
  0xd0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_idrss
  0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_isrss
  0xd0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_minflt
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_majflt
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nswap
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_inblock
  0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_oublock
  0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_msgsnd
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_msgrcv
  0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nsignals
  0xfa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nvcsw
  0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nivcsw
};

// Dummy rusage struct, used to be:
//static int dummy_rusage[31] = {0,50000,0,50000,15000,0,30,0,2000,0,20,0,2000,
//                             0,0,0,0,0,1,0,4,0,2,0,0,0,2,0,250,0,30};
const u8 dummy_linux_rusage[144] = 
{
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_utime
  0x00, 0x00, 0x00, 0x00, 0x50, 0xc3, 0x00, 0x00, // ru_utime_usec
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_stime
  0x00, 0x00, 0x00, 0x00, 0x50, 0xc3, 0x00, 0x00, // ru_stime_usec
  0x98, 0x3a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_maxrss
  0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_ixrss
  0xd0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_idrss
  0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_isrss
  0xd0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_minflt
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_majflt
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nswap
  0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_inblock
  0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_oublock
  0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_msgsnd
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_msgrcv
  0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nsignals
  0xfa, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nvcsw
  0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // ru_nivcsw
};

const u8 dummy_sysinfo[64] =
{
    0x89, 0x90, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, // si_user
    0xe8, 0x29, 0x92, 0x04, 0x00, 0x00, 0x00, 0x00, // si_nice
    0xeb, 0xc9, 0xd2, 0x02, 0x00, 0x00, 0x00, 0x00, // si_sys
    0x16, 0xf7, 0xf2, 0x00, 0x0a, 0x00, 0x00, 0x00, // si_idle
    0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // si_hz
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // si_phz
    0xc6, 0x76, 0x8e, 0x46, 0x00, 0x00, 0x00, 0x00, // si_bootime
    0x52, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // si_wait
};



// declarations for misc anonymous-namespace helpers
string collapse_path(const string& path, bool *is_absolute_ret);


// Translate a C string (corresponding to a syscall argument) from application
// to simulator memory, so that native C functions can use it.  The string
// needn't be at the same location as the original syscall arg, but there is
// only storage for one translation per arg_num.
//
// The string will be copied out into the AppState's SyscallState to ensure it
// ends up contiguous, a pointer to that temporary copy will be returned, and
// the string length will optionally be written to "len_ret".  The temporary
// copy will live until the next call to syscall_cstring_xlate() or
// syscall_path_xlate() with the same arg_num value.
const char *
syscall_cstring_xlate(AppState *as,
                      int arg_num, mem_addr va, int *len_ret)
{
    SyscallState *sst = as->syscall_state;
    sim_assert(IDX_OK(arg_num, NELEM(sst->path_args)));
    string& storage = sst->path_args[arg_num];
    int len = 0;

    storage.clear();
    {
        unsigned char next_ch;
        while ((next_ch = pmem_read_8(as->pmem, va + len, PMAF_R)) != 0) {
            // If we run outside of mapped memory, pmem_read_8 will catch it.
            //
            // (Right now, this will cause the simulator to freak out and
            // abort from down in ProgMem; it would be more proper to 
            // detect this explicitly, and fail the syscall with an errno
            // of EFAULT.
            storage += next_ch;
            ++len;
        }
    }

    sim_assert(len == int(storage.size()));
    if (len_ret)
        *len_ret = len;
    return storage.c_str();
}


static void
traceout(const char *fmt, ...) 
{
    va_list args;

    // Related: see conditions at use of print_syscall_ascall()
    if (SysTrace || debug) {
        va_start(args, fmt);
        vfprintf(stdout, fmt, args);
        va_end(args);
    }
}


// Legacy code: it's not even clear what this does is correct (for ages, it
// only wrote 32-bit pointers?), but it's left mostly as-is for backwards
// compatibility until something proper is done with syscalls (ha ha).
//
// Returns sigaction() result (0/-1).
int
dosigaction(AppState *as)
{
    SyscallState *sst = as->syscall_state; 
    i64 signum = as->R[REG_A0].i;
    unsigned char *sigaction_data;

    if ((signum < 0) || (signum >= NELEM(sst->old_sig.sigaction_array)))
        goto invalid_signal;

    sigaction_data = sst->old_sig.sigaction_array[signum];

    if (as->R[REG_A2].u) {      // "struct sigaction *restrict oact" or 0
        // store current sigaction struct to "oact" pointer
        pmem_write_memcpy(as->pmem, as->R[REG_A2].u,
                          sigaction_data, AlphaSigactionBytes, 0);
    }
    if (as->R[REG_A1].u) {      // "const struct sigaction *restrict act" or 0
        // copy from "act" pointer to sigaction struct
        pmem_read_memcpy(as->pmem, sigaction_data, as->R[REG_A1].u,
                         AlphaSigactionBytes, 0);
    }

    return 0;

invalid_signal:
    errno = EINVAL;             // Crufty but convenient
    return -1;
    /* Still need to add code so that on a signal, the 
       correct action is actually taken. */

    /* Still need to add support for returning the correct
       error messages (EFAULT, EINVAL) */
}


// Implement a "sigreturn" syscall.  This acts basically like the second half
// of a user-mode context switch: overwrites the PC and registers with the
// contents of a "struct sigcontext".  (we currently ignore several fields
// that the simulator doesn't really make use of)
void
dosigreturn(AppState *as)
{
    ProgMem *pmem = as->pmem;
    mem_addr va = as->R[REG_A0].u;

    as->npc = pmem_read_64(pmem, va + 16, 0);                   // sc_pc
    // silly hack: emulate_inst() is going to add 4 to advance the PC, so
    // we'll compensate
    as->npc -= 4;
    for (int i = 0; i < 31; i++) {
        // int reg #i <- sc_regs[i]
        as->R[i].u = pmem_read_64(pmem, va + 32 + 8*i, 0);
    }
    for (int i = 0; i < 31; i++) {
        // FP reg #i <- sc_fpregs[i]
        as->R[FP_REG(i)].u = pmem_read_64(pmem, va + 296 + 8*i, 0);
    }
    as->R[FPCR_REG].u = pmem_read_64(pmem, va + 552, 0);        // sc_fpcr
}


int
resize_seg(AppState *astate, mem_addr start_addr,
           i64 delta)
{
    if (!delta)
        return 0;
    ProgMemSegment *seg = pmem_get_seg(astate->pmem, start_addr);
    sim_assert(seg);    // Should have been checked by caller
    i64 old_size = pms_size(seg);
    i64 eff_delta = delta;
    int align = GlobalParams.mem.page_bytes;
    if (eff_delta % align)
        eff_delta += align - (eff_delta % align);
    i64 new_size = old_size + eff_delta;

    if (pms_resize(seg, new_size)) {
        if (astate->syscall_state->conf.report_resize_failures) {
            const char *fname = "resize_seg";
            // Benchmarks typically don't respond well to this, so it's
            // likely useful to report it explicitly.
            err_printf("%s: A%d failed to resize segment at va 0x%s, "
                       "%s -> %s KiB at time %s\n", fname, astate->app_id,
                       fmt_x64(start_addr), fmt_i64(old_size/1024),
                       fmt_i64(new_size/1024),
                       astate->syscall_state->fmt_clock().c_str());
        }
        return -1;
    }

    return 0;
}


// Format a syscall arg for debug printing.  Returns a pointer to a single
// static buffer.
// (non-const AppState: may alter e.g. as->syscall_state->path_args[])
const char *
fmt_syscall_arg(AppState *astate, int argno, int syscall_num,
                int arg_reg)
{
    static char buf[256];
    int buf_used = 0;
    const int arg_type = syscall_table[syscall_num].args[argno];
    const i64 arg_reg_val = astate->R[arg_reg].i;
    const char *hex_out = "0123456789abcdef";

    switch (arg_type) {
    case NO_ARG:
        break;
    case FD_ARG:
        buf_used += e_snprintf(buf + buf_used, sizeof(buf) - buf_used,
                               "%s", fmt_i64(arg_reg_val));
        break;
    case INT_ARG:
        buf_used += e_snprintf(buf + buf_used, sizeof(buf) - buf_used,
                               "%s", fmt_i64(arg_reg_val));
        break;
    case ADDR_R_ARG:
    case ADDR_W_ARG:
    case ADDR_RW_ARG:
        buf_used += e_snprintf(buf + buf_used, sizeof(buf) - buf_used,
                               "0x%s", fmt_x64(arg_reg_val));
        break;
    case CSTR_ARG:
    case PATH_ARG: {
        int src_len;
        const char *src_str = syscall_cstring_xlate(astate, argno,
                                                    arg_reg_val, &src_len);
        if (src_len > 80)
            src_len = 80;
        buf[buf_used++] = '"';
        for (int src_used = 0; src_used < src_len; src_used++) {
            unsigned char src_ch = src_str[src_used];
            if (src_ch == '\n') {
                buf[buf_used++] = '\\';
                buf[buf_used++] = 'n';
            } else if (src_ch == '"') {
                buf[buf_used++] = '\\';
                buf[buf_used++] = '"';
            } else if (isprint(src_ch)) {
                buf[buf_used++] = src_ch;
            } else {
                buf[buf_used++] = '\\';
                buf[buf_used++] = 'x';
                buf[buf_used++] = hex_out[(src_ch >> 4) & 0xf];
                buf[buf_used++] = hex_out[src_ch & 0xf];
            }
        }
        buf[buf_used++] = '"';
        break;
    }
    default:
        ENUM_ABORT(SyscallArgType, arg_type);
    }

    sim_assert(buf_used < NELEM(buf));
    buf[buf_used] = '\0';
    return buf;
}


// print syscall invocation info, in the style of a function call
void
print_syscall_ascall(FILE *out, AppState *as, int syscall_num)
{
    sim_assert(IDX_OK(syscall_num, NELEM(syscall_table)));
    const SyscallDesc *desc = &syscall_table[syscall_num];
    fprintf(out, "%s(", desc->syscall_name);
    for (int arg_num = 0; (arg_num < NELEM(desc->args)) &&
             (desc->args[arg_num] != NO_ARG); ++arg_num) {
        const char *str_arg = fmt_syscall_arg(as, arg_num, syscall_num,
                                              REG_A0 + arg_num);
        if (arg_num > 0)
            fputs(", ", out);
        fputs(str_arg, out);
    }
    fputs(")", out);
}


// Translate C-string "pathname" (as taken from simulated execution) into
// a path name to use in the native environment; returns a C-string.
// "arg_num" is used to select a storage buffer; different args must use
// different buffers.  This routine makes a temporary copy of the working
// string, so it's OK to use the output from syscall_cstring_xlate() here,
// though this will invalidate that pointer before returning (due to the
// re-use of the string storage).
const char *
syscall_path_xlate(AppState *as, int arg_num, const char *pathname)
{
    SyscallState *sst = as->syscall_state;
    sim_assert(IDX_OK(arg_num, NELEM(sst->path_args)));
    string *target = &sst->path_args[arg_num];  // final storage for output
    bool uniquify_name = false;
    const char *path_sep = "/";
    
    const char *last_slash = strrchr(pathname, '/');
    string all_but_final;       // up to final '/'; ends in '/' iff nonempty
    string final_component;     // everything after final '/'; has no '/'
    string new_path;            // temporary copy we're building up

    if (last_slash) {
        all_but_final.assign(pathname, (last_slash - pathname) + 1);
        final_component.assign(last_slash + 1);
    } else {
        all_but_final.clear();
        final_component.assign(pathname);
    }

    if (!final_component.empty() &&
        sst->conf.paths_to_uniquify.count(final_component)) {
        uniquify_name = true;
    }

    if (uniquify_name) {
        std::ostringstream pref;
        if (sst->conf.uniquify_to_subdir) {
            // We'll put the per-instance unique files in a subdirectory, to
            // avoid altering the directory-entry count if we can.
            const char *subdir_for_uniq = "smtsim-uniq";
            // using the simulator PID gives us a short, unique ID, but has
            // the disadvantage that a single simulation that's restarted
            // several times may end up generating extraneous files.
            if (mkdir(subdir_for_uniq, 0777) != 0) {
                if (errno != EEXIST) {
                    exit_printf("couldn't create subdirectory \"%s\" for "
                                "unique output files; %s\n", subdir_for_uniq,
                                strerror(errno));
                }
            }
            pref << subdir_for_uniq << path_sep;
        } else {
            pref << "uniq.";
        }
        pref << hostname_short() << "." << getpid() << "."
             << as->app_master_id << "_";
        new_path = all_but_final;
        new_path += pref.str();
        new_path += final_component;
    } else {
        new_path.assign(pathname);
    }

    if (sst->conf.root_paths_at_cwd) {
        // 1. prepend local_path, the path from InitialWorkingDir to
        //    the current working dir, along with a leading "/" to make
        //    things look absolute so that "../" can't climb out of the
        //    sandbox.
        // 2. collapse any ".." elements (and "." while we're at it)
        // 3. prepend "real" root dir, plus separating /
        string to_collapse = path_sep + sst->local_path + path_sep + new_path;
        bool is_absolute;
        string collapsed = collapse_path(to_collapse, &is_absolute);
        if (!is_absolute) {
            // shouldn't happen!
            abort_printf("non-absolute local path in translation; "
                         "local_path \"%s\" new_path \"%s\" "
                         "collapsed \"%s\"\n", sst->local_path.c_str(),
                         new_path.c_str(), collapsed.c_str());
        }
        // (collapsed path is absolute-looking, so we don't need an extra
        // separator)
        new_path = sst->initial_working_dir + collapsed;
    }

    if (new_path.compare(pathname) != 0) {
        // note that we've changed the path
        traceout(" [%s -> %s]", pathname, new_path.c_str());
    }
    *target = new_path;          // WARNING: may invalidate "pathname" pointer
    return target->c_str();
}


u64
syscall_arg_xlate(AppState *as, int syscall_num,
                  int arg_num, int arg_reg)
{
    SyscallArgType arg_type = syscall_table[syscall_num].args[arg_num];
    u64 arg_val = as->R[arg_reg].i;
    u64 result;

    switch (arg_type) {
    case NO_ARG:
        result = U64_MAX;
        break;
    case INT_ARG:
        result = arg_val;
        break;
    case FD_ARG:
        // we used to translate these to native fd's here, but now we
        // handle all that stuff via SimulatedFD
        result = arg_val;
        break;
    case ADDR_R_ARG:
    case ADDR_W_ARG:
    case ADDR_RW_ARG:
        // we used to translate these to native void-pointers here via but now
        // things are re-implemented with proper calls to ProgMem.
        result = arg_val;
        break;

    case CSTR_ARG:
        if (arg_val) {
            const void *addr =
                syscall_cstring_xlate(as, arg_num, arg_val, NULL);
            // addr points into SyscallState-held buffer for this arg_num
            result = u64_from_ptr(addr);
        } else {
            result = 0;
        }
        break;

    case PATH_ARG:
        if (arg_val) {
            const char *addr =
                syscall_cstring_xlate(as, arg_num, arg_val, NULL);
            addr = syscall_path_xlate(as, arg_num, addr);
            // addr points into SyscallState-held buffer for this arg_num
            result = u64_from_ptr(addr);
        } else {
            result = 0;
        }
        break;

    default:
        result = 0;
        ENUM_ABORT(SyscallArgType, arg_type);
    }


    return result;
}


// Translate a host-system "errno" value into an alpha syscall errno
i64
syscall_errno_xlate(int native_errno)
{
    i64 result = alpha_EACCES;
    sim_assert(native_errno != 0);

    switch (native_errno) {
    case EPERM: result = alpha_EPERM; break;
    case ENOENT: result = alpha_ENOENT; break;
    case ESRCH: result = alpha_ESRCH; break;
    case EINTR: result = alpha_EINTR; break;
    case EIO: result = alpha_EIO; break;
    case ENXIO: result = alpha_ENXIO; break;
    case E2BIG: result = alpha_E2BIG; break;
    case ENOEXEC: result = alpha_ENOEXEC; break;
    case EBADF: result = alpha_EBADF; break;
    case ECHILD: result = alpha_ECHILD; break;
    case EDEADLK: result = alpha_EDEADLK; break;
    case ENOMEM: result = alpha_ENOMEM; break;
    case EACCES: result = alpha_EACCES; break;
    case EFAULT: result = alpha_EFAULT; break;
    case ENOTBLK: result = alpha_ENOTBLK; break;
    case EBUSY: result = alpha_EBUSY; break;
    case EEXIST: result = alpha_EEXIST; break;
    case EXDEV: result = alpha_EXDEV; break;
    case ENODEV: result = alpha_ENODEV; break;
    case ENOTDIR: result = alpha_ENOTDIR; break;
    case EISDIR: result = alpha_EISDIR; break;
    case EINVAL: result = alpha_EINVAL; break;
    case ENFILE: result = alpha_ENFILE; break;
    case EMFILE: result = alpha_EMFILE; break;
    case ENOTTY: result = alpha_ENOTTY; break;
    case ETXTBSY: result = alpha_ETXTBSY; break;
    case EFBIG: result = alpha_EFBIG; break;
    case ENOSPC: result = alpha_ENOSPC; break;
    case ESPIPE: result = alpha_ESPIPE; break;
    case EROFS: result = alpha_EROFS; break;
    case EMLINK: result = alpha_EMLINK; break;
    case EPIPE: result = alpha_EPIPE; break;
    case EDOM: result = alpha_EDOM; break;
    case ERANGE: result = alpha_ERANGE; break;
    case EWOULDBLOCK: result = alpha_EWOULDBLOCK; break;
    case EINPROGRESS: result = alpha_EINPROGRESS; break;
    case EALREADY: result = alpha_EALREADY; break;
    case ENOTSOCK: result = alpha_ENOTSOCK; break;
    case EDESTADDRREQ: result = alpha_EDESTADDRREQ; break;
    case EMSGSIZE: result = alpha_EMSGSIZE; break;
    case EPROTOTYPE: result = alpha_EPROTOTYPE; break;
    case ENOPROTOOPT: result = alpha_ENOPROTOOPT; break;
    case EPROTONOSUPPORT: result = alpha_EPROTONOSUPPORT; break;
    case ESOCKTNOSUPPORT: result = alpha_ESOCKTNOSUPPORT; break;
    case EOPNOTSUPP: result = alpha_EOPNOTSUPP; break;
    case EPFNOSUPPORT: result = alpha_EPFNOSUPPORT; break;
    case EAFNOSUPPORT: result = alpha_EAFNOSUPPORT; break;
    case EADDRINUSE: result = alpha_EADDRINUSE; break;
    case EADDRNOTAVAIL: result = alpha_EADDRNOTAVAIL; break;
    case ENETDOWN: result = alpha_ENETDOWN; break;
    case ENETUNREACH: result = alpha_ENETUNREACH; break;
    case ENETRESET: result = alpha_ENETRESET; break;
    case ECONNABORTED: result = alpha_ECONNABORTED; break;
    case ECONNRESET: result = alpha_ECONNRESET; break;
    case ENOBUFS: result = alpha_ENOBUFS; break;
    case EISCONN: result = alpha_EISCONN; break;
    case ENOTCONN: result = alpha_ENOTCONN; break;
    case ESHUTDOWN: result = alpha_ESHUTDOWN; break;
    case ETOOMANYREFS: result = alpha_ETOOMANYREFS; break;
    case ETIMEDOUT: result = alpha_ETIMEDOUT; break;
    case ECONNREFUSED: result = alpha_ECONNREFUSED; break;
    case ELOOP: result = alpha_ELOOP; break;
    case ENAMETOOLONG: result = alpha_ENAMETOOLONG; break;
    case EHOSTDOWN: result = alpha_EHOSTDOWN; break;
    case EHOSTUNREACH: result = alpha_EHOSTUNREACH; break;
    case ENOTEMPTY: result = alpha_ENOTEMPTY; break;
    // case EPROCLIM: result = alpha_EPROCLIM; break;
    case EUSERS: result = alpha_EUSERS; break;
    case EDQUOT: result = alpha_EDQUOT; break;
    case ESTALE: result = alpha_ESTALE; break;
    case EREMOTE: result = alpha_EREMOTE; break;
    // case EBADRPC: result = alpha_EBADRPC; break;
    // case ERPCMISMATCH: result = alpha_ERPCMISMATCH; break;
    // case EPROGUNAVAIL: result = alpha_EPROGUNAVAIL; break;
    // case EPROGMISMATCH: result = alpha_EPROGMISMATCH; break;
    // case EPROCUNAVAIL: result = alpha_EPROCUNAVAIL; break;
    case ENOLCK: result = alpha_ENOLCK; break;
    case ENOSYS: result = alpha_ENOSYS; break;
    // case EFTYPE: result = alpha_EFTYPE; break;
    case ENOMSG: result = alpha_ENOMSG; break;
    case EIDRM: result = alpha_EIDRM; break;
    //freebsd case ENOSR: result = alpha_ENOSR; break;
    //freebsd case ETIME: result = alpha_ETIME; break;
    //freebsd case EBADMSG: result = alpha_EBADMSG; break;
    //freebsd case EPROTO: result = alpha_EPROTO; break;
    //freebsd case ENODATA: result = alpha_ENODATA; break;
    //freebsd case ENOSTR: result = alpha_ENOSTR; break;
    // case EDIRTY: result = alpha_EDIRTY; break;
    // case EDUPPKG: result = alpha_EDUPPKG; break;
    // case EVERSION: result = alpha_EVERSION; break;
    //freebsd case ENOPKG: result = alpha_ENOPKG; break;
    // case ENOSYM: result = alpha_ENOSYM; break;
    case ECANCELED: result = alpha_ECANCELED; break;
    // case EFAIL: result = alpha_EFAIL; break;
    // case EINPROG: result = alpha_EINPROG; break;
    // case EMTIMERS: result = alpha_EMTIMERS; break;
    // case EAIO: result = alpha_EAIO; break;
    //freebsd case EMULTIHOP: result = alpha_EMULTIHOP; break;
    //freebsd case ENOLINK: result = alpha_ENOLINK; break;
#if !defined(__alpha) || defined(EOVERFLOW)
    case EOVERFLOW: result = alpha_EOVERFLOW; break;
#endif
    case EILSEQ: result = alpha_EILSEQ; break;
    // case ECLONEME: result = alpha_ECLONEME; break;
    // case ESOFT: result = alpha_ESOFT; break;
    // case EMEDIA: result = alpha_EMEDIA; break;
    // case ERELOCATED: result = alpha_ERELOCATED; break;
    // case ERESTART: result = alpha_ERESTART; break;
    // case EJUSTRETURN: result = alpha_EJUSTRETURN; break;
    // case EEMULATE: result = alpha_EEMULATE; break;
    }

    // Check for native errno values which are duplicates on some host
    // platforms, so they can't share a single switch statement.
    switch (native_errno) {
    case ENOTSUP: result = alpha_ENOTSUP; break;
    case EAGAIN: result = alpha_EAGAIN; break;
    }

    return result;
}


// Convert a simulation cycle count to data fields for a "struct timeval"
void
cyc_to_timeval(i64 cyc_count, struct timeval *tv_ret)
{
    double cyc_sec = (double) cyc_count / CLOCK_RATE_HZ;
    tv_ret->tv_sec = static_cast<i64>(floor(cyc_sec));
    tv_ret->tv_usec = static_cast<i64>(
        floor((cyc_sec - floor(cyc_sec)) * 1e6));
}


int
store_alpha_statfs(const SyscallState *sst, ProgMem *pmem, mem_addr va,
                   const struct statfs *host_statfs)
{
    const int size = 336;
    const char fake_mntonname[] = "/home";
    const char fake_mntfromname[] = "/projects/home-for-now@whatever";
    const u32 fake_mount_info[20] = {
        0x0, 0x0, 0x0, 0x0, 0x60, 0x2000, 0x2000, 0xb, 0x4, 0x14,
        0x0, 0x0, 0x3, 0x3c, 0x1e, 0x3c, 0x0, 0x0, 0x0, 0x0 };
    struct statfs statfs_copy = *host_statfs;
    if (sst->conf.sanitize_host_device_info) {
        statfs_copy.f_blocks = 1337021264;
        statfs_copy.f_bfree = 164176252;
        statfs_copy.f_bavail = 96259460;
        statfs_copy.f_files = 169803776;
        statfs_copy.f_ffree = 163044199;
    }
    pmem_write_memset(pmem, va, 0, size, 0);
    pmem_write_16(pmem, va+ 0, 14, 0);                          // f_type
    pmem_write_16(pmem, va+ 2, 0x4000, 0);                      // f_flags
    pmem_write_32(pmem, va+ 4, 1024, 0);                        // f_fsize
    pmem_write_32(pmem, va+ 8, 8192, 0);                        // f_bsize
    pmem_write_32(pmem, va+12, statfs_copy.f_blocks, 0);        // f_blocks
    pmem_write_32(pmem, va+16, statfs_copy.f_bfree, 0);         // f_bfree
    pmem_write_32(pmem, va+20, statfs_copy.f_bavail, 0);        // f_bavail
    pmem_write_32(pmem, va+24, statfs_copy.f_files, 0);         // f_files
    pmem_write_32(pmem, va+28, statfs_copy.f_ffree, 0);         // f_ffree
    pmem_write_64(pmem, va+32, U64_LIT(0xe08100002), 0);        // f_fsid
    pmem_write_memcpy(pmem, va+76, fake_mntonname, sizeof(fake_mntonname), 0);
    pmem_write_memcpy(pmem, va+166, fake_mntfromname, 
                      sizeof(fake_mntfromname), 0);
    for (int i = 0; i < NELEM(fake_mount_info); i++)
        pmem_write_32(pmem, va + 256 + 4*i, fake_mount_info[i], 0);
    return size;
}


int
store_alpha_utsname(ProgMem *pmem, mem_addr va)
{
    const int size = 160;
    // These values were taken from olympic.ucsd.edu; watch field for overflow
    const char fake_sysname[]  = "OSF1";
    const char fake_nodename[] = "smtsim-virtual.ucsd.edu";
    const char fake_release[]  = "V4.0";
    const char fake_version[]  = "1091";
    const char fake_machine[]  = "alpha";
    pmem_write_memset(pmem, va, 0, size, 0);
    pmem_write_memcpy(pmem, va+  0, fake_sysname, sizeof(fake_sysname), 0);
    pmem_write_memcpy(pmem, va+ 32, fake_nodename, sizeof(fake_nodename), 0);
    pmem_write_memcpy(pmem, va+ 64, fake_release, sizeof(fake_release), 0);
    pmem_write_memcpy(pmem, va+ 96, fake_version, sizeof(fake_version), 0);
    pmem_write_memcpy(pmem, va+128, fake_machine, sizeof(fake_machine), 0);
    return size;
}

int
store_linux_alpha_utsname(ProgMem *pmem, mem_addr va)
{
    const int size = 320;
    // These values were taken from m5.
    const char fake_sysname[]  = "Linux";
    const char fake_nodename[] = "smtsim.ucsd.edu";
    const char fake_release[]  = "2.4.20";
    const char fake_version[]  = "#1 Mon Aug 18 11:32:15 EDT 2003";
    const char fake_machine[]  = "alpha";

    pmem_write_memset(pmem, va, 0, size, 0);
    pmem_write_memcpy(pmem, va+  0, fake_sysname, sizeof(fake_sysname), 0);
    pmem_write_memcpy(pmem, va+ 64, fake_nodename, sizeof(fake_nodename), 0);
    pmem_write_memcpy(pmem, va+128, fake_release, sizeof(fake_release), 0);
    pmem_write_memcpy(pmem, va+192, fake_version, sizeof(fake_version), 0);
    pmem_write_memcpy(pmem, va+256, fake_machine, sizeof(fake_machine), 0);
    return size;
}


int
load_alpha_timeval(ProgMem *pmem, struct timeval *host_tv_ret, mem_addr va)
{
    const int size = AlphaTimeValBytes;
    host_tv_ret->tv_sec = pmem_read_32(pmem, va+0, 0);
    host_tv_ret->tv_usec = pmem_read_32(pmem, va+4, 0);
    return size;
}

int
linux_load_alpha_timeval(ProgMem *pmem, struct timeval *host_tv_ret, mem_addr va)
{
    const int size = AlphaTimeValBytes;
    host_tv_ret->tv_sec = pmem_read_64(pmem, va+0, 0);
    host_tv_ret->tv_usec = pmem_read_64(pmem, va+8, 0);
    return size;
}


int
store_alpha_timeval(ProgMem *pmem, mem_addr va, const struct timeval *host_tv)
{
    const int size = AlphaTimeValBytes;
    pmem_write_32(pmem, va+0, host_tv->tv_sec, 0);
    pmem_write_32(pmem, va+4, host_tv->tv_usec, 0);
    return size;
}

int
linux_store_alpha_timeval(ProgMem *pmem, mem_addr va, const struct timeval *host_tv)
{
    const int size = LinuxAlphaTimeValBytes;
    pmem_write_64(pmem, va+0, host_tv->tv_sec, 0);
    pmem_write_64(pmem, va+8, host_tv->tv_usec, 0);
    return size;
}


int
store_alpha_timezone(ProgMem *pmem, mem_addr va)
{
    long alpha_tz_minuteswest = 480;
    long alpha_tz_dsttime = 0;
    pmem_write_32(pmem, va+0, alpha_tz_minuteswest, 0);
    pmem_write_32(pmem, va+4, alpha_tz_dsttime, 0);
    return 8;
}


int
store_alpha_rusage(SyscallState *sst, ProgMem *pmem, mem_addr va)
{
    const int size = 128;
    struct timeval fake_ru_utime;
    sim_assert(size == sizeof(dummy_rusage));
    pmem_write_memcpy(pmem, va, dummy_rusage, 128, 0);
    // Pretend that this app got some fraction of the total sim time
    cyc_to_timeval((i64)(0.9 * sst->local_clock), &fake_ru_utime);
    store_alpha_timeval(pmem, va + 0, &fake_ru_utime);  // ru_utime: 8 @ 0
    return size;
}

int
linux_store_alpha_rusage(SyscallState *sst, ProgMem *pmem, mem_addr va)
{
    const int size = 144;
    struct timeval fake_ru_utime;
    sim_assert(size == sizeof(dummy_linux_rusage));
    pmem_write_memcpy(pmem, va, dummy_linux_rusage, 144, 0);
    // Pretend that this app got some fraction of the total sim time
    cyc_to_timeval((i64)(0.9 * sst->local_clock), &fake_ru_utime);
    linux_store_alpha_timeval(pmem, va + 0, &fake_ru_utime);  // ru_utime: 8 @ 0
    return size;
}


int
alpha_fd_isset(ProgMem *pmem, int fd_num, mem_addr fdset_va)
{
    // Alpha fd_set is an array of 128 32-bit ints used as a bit vector.
    // Bit i is at array[i/32], mask 1<<(i%32).
    unsigned long fd_word = pmem_read_32(pmem, fdset_va + (fd_num / 32), 0);
    return (fd_word & (1LU << (fd_num % 32))) ? 1 : 0;
}


void
alpha_fd_set(ProgMem *pmem, int fd_num, mem_addr fdset_va)
{
    unsigned long fd_word = pmem_read_32(pmem, fdset_va + (fd_num / 32), 0);
    fd_word |= 1LU << (fd_num % 32);
    pmem_write_32(pmem, fdset_va + fd_num / 32, fd_word, 0);
}


// nonzero: bad file descriptor encountered.
// writes to "fd_set_ret"
int
load_alpha_fdset(ProgMem *pmem, const SyscallState *sst,
                 SimulatedFDSet *fd_set_ret, mem_addr src_va,
                 i64 num_fds)
{
    fd_set_ret->clear();
    for (int prog_fd = 0; prog_fd < num_fds; prog_fd++) {
        if (alpha_fd_isset(pmem, prog_fd, src_va)) {
            SimulatedFD *host_fd = sst->fd_lookup(prog_fd);
            if (!host_fd)
                return -1;
            fd_set_ret->insert(host_fd);
        }
    }
    return 0;
}


void
store_alpha_fdset(ProgMem *pmem, const SyscallState *sst,
                  mem_addr targ_va, const SimulatedFDSet *src,
                  i64 num_fds)
{
    pmem_write_memset(pmem, targ_va, 0, AlphaFDSetBytes, 0);
    FOR_CONST_ITER(SimulatedFDSet, *src, fd_iter) {
        int alpha_fd = (*fd_iter)->get_alpha_fd();
        sim_assert(alpha_fd < ALPHA_OSF_FD_SETSIZE);
        alpha_fd_set(pmem, alpha_fd, targ_va);
    }
}


// Returns -1 on error, select() retval otherwise.
// Only writes output memory on success.
int
do_alpha_select(ProgMem *pmem, const SyscallState *sst,
                i64 num_fds, mem_addr readfds_va,
                mem_addr writefds_va, mem_addr exceptfds_va,
                mem_addr timeout_va)
{
    SimulatedFDSet readfds_in, readfds_out;
    SimulatedFDSet writefds_in, writefds_out;
    SimulatedFDSet exceptfds_in, exceptfds_out;
    struct timeval host_timeout;
    int ret;

    if ((num_fds < 0) || (num_fds > ALPHA_OSF_FD_SETSIZE))
        goto bad_nfds;

    if (readfds_va) {
        if (!pmem_access_ok(pmem, readfds_va, AlphaFDSetBytes, PMAF_RW))
            goto bad_ptr;
        if (load_alpha_fdset(pmem, sst, &readfds_in, readfds_va, num_fds))
            goto invalid_fd;
    }
    if (writefds_va) {
        if (!pmem_access_ok(pmem, writefds_va, AlphaFDSetBytes, PMAF_RW))
            goto bad_ptr;
        if (load_alpha_fdset(pmem, sst, &writefds_in, writefds_va, num_fds))
            goto invalid_fd;
    }
    if (exceptfds_va) {
        if (!pmem_access_ok(pmem, exceptfds_va, AlphaFDSetBytes, PMAF_RW))
            goto bad_ptr;
        if (load_alpha_fdset(pmem, sst, &exceptfds_in, exceptfds_va, num_fds))
            goto invalid_fd;
    }

    if (!timeout_va) {
        // NULL user "timeout" pointer: an indefinite blocking-select is
        // requested, which we don't really support.  (We don't want to let
        // this thread actually block the entire simulation.)
    } else {
        if (!pmem_access_ok(pmem, timeout_va, AlphaTimeValBytes, PMAF_RW))
            goto bad_ptr;
        load_alpha_timeval(pmem, &host_timeout, timeout_va);
        if ((host_timeout.tv_sec == 0) && (host_timeout.tv_usec == 0)) {
            // Straightforward polling operation.
        } else {
            // A poll-with-timeout is requested, which we also don't really
            // support.  :(
        }
    }

    FOR_CONST_ITER(SimulatedFDSet, readfds_in, fd_iter) {
        if ((*fd_iter)->sim_select_readfd()) {
            readfds_out.insert(*fd_iter);
        }
    }
    FOR_CONST_ITER(SimulatedFDSet, writefds_in, fd_iter) {
        if ((*fd_iter)->sim_select_writefd()) {
            writefds_out.insert(*fd_iter);
        }
    }
    FOR_CONST_ITER(SimulatedFDSet, exceptfds_in, fd_iter) {
        if ((*fd_iter)->sim_select_exceptfd()) {
            exceptfds_out.insert(*fd_iter);
        }
    }

    ret = intsize(readfds_out) + intsize(writefds_out) +
        intsize(exceptfds_out);

    if (ret >= 0) {
        if (readfds_va)
            store_alpha_fdset(pmem, sst, readfds_va, &readfds_out, num_fds);
        if (writefds_va)
            store_alpha_fdset(pmem, sst, writefds_va, &writefds_out, num_fds);
        if (exceptfds_va)
            store_alpha_fdset(pmem, sst, exceptfds_va, &exceptfds_out,
                              num_fds);
        if (timeout_va)
            store_alpha_timeval(pmem, timeout_va, &host_timeout);
    }

    return ret;

    // It's crufty for us to write to errno.  It is handy, though.
bad_nfds:
    errno = EINVAL;
    return -1;
bad_ptr:
    errno = EFAULT;
    return -1;
invalid_fd:
    errno = EBADF;
    return -1;
}


// This one is a little odd: the C library sigprocmask() routine handles the
// loading and storing from the "set" and to the "oset" pointers, instead of
// passing them to the syscall.  The syscall takes the "set" /value/ in
// REG_A1, and returns the old sigmask value in REG_V0, or -1 on error.  If it
// returns anything other than -1, that value is stored at the "oset" pointer
// if it is non-NULL.  If the "set" pointer passed to the C routine was NULL,
// REG_A1 is set to 0 and REG_A0 is set to 1 (Alpha_SIG_BLOCK).
//
// Returns -1 on failure, else the previous "sigmask" value.
i64
do_alpha_sigprocmask(AppState *as,
                     i64 alpha_how, u64 set_val)
{
    SyscallState *sst = as->syscall_state;
    i64 prev_sigmask = sst->old_sig.sigmask;

    switch (alpha_how) {
    case Alpha_SIG_BLOCK:
        sst->old_sig.sigmask |= set_val;
        break;
    case Alpha_SIG_UNBLOCK:
        sst->old_sig.sigmask &= ~set_val;
        break;
    case Alpha_SIG_SETMASK:
        sst->old_sig.sigmask = set_val;
        break;
    default:
        goto bad_how;
    }

    return prev_sigmask;

bad_how:
    errno = EINVAL;     // crufty but convenient
    return -1;
}


// Returns alpha table-syscall return value, which is a positive element
// count on success, and -1 on any error.  On an error, set the _simulator_
// "errno" variable to simulator OS-native error code.
i64
do_alpha_table(AppState *as)
{
    i64 table_id        = as->R[REG_A0].i;      // System table ID
    i64 table_index     = as->R[REG_A1].i;      // Index within table
    mem_addr table_addr = as->R[REG_A2].i;      // User va to read/write
    i64 table_nel       = as->R[REG_A3].i;      // #elts to read/write
    u64 table_lel       = as->R[REG_A4].i;      // expected size of each elt
    i64 result;

    // table_nel is signed; positive means copy kernel->user
    //bool write_to_user = (table_nel >= 0);
    //i64 abs_nel = (table_nel >= 0) ? table_nel : -table_nel;

    switch (table_id) {
    case Alpha_TBL_SYSINFO: {
        if ((table_index != 0) || (table_nel < 0) || (table_nel > 1)) {
            errno = EINVAL;     // observed on OSF1
            goto misc_errno_set;
        }
        int copy_bytes = MAX_SCALAR(sizeof(dummy_sysinfo), table_lel);
        pmem_write_memcpy(as->pmem, table_addr, dummy_sysinfo, copy_bytes, 0);
        result = table_nel;
        break;
    }
    default:
        result = -1; // unused
        goto unimplmented_table_op;
    }
    
    sim_assert(result >= 0);
    return result;

 unimplmented_table_op:
    if (!as->syscall_state->warned_missing_table_ops.count(table_id)) {
        as->syscall_state->warned_missing_table_ops.insert(table_id);
        err_printf("WARNING: app A%d requested unimplemented SYS_table op, "
                   "id %s; returning EINVAL (first instance at cyc %s)\n",
                   as->app_id, fmt_i64(table_id),
                   as->syscall_state->fmt_clock().c_str());
    }
    errno = EINVAL;
    return -1;
 misc_errno_set:
    // errno already set
    return -1;
}


void
report_bad_syscall(FILE *out, const AppState *as, i64 syscall_num,
                   const char *reason)
{
    const SyscallDesc *desc = (IDX_OK(syscall_num, MAX_SYSCALL)) ?
        &syscall_table[syscall_num] : NULL;
    string print_name;
    if (!desc) {
        print_name = "(unknown)";
    } else if (!desc->syscall_name[0]) {
        print_name = "(empty name)";
    } else {
        print_name = string("\"") + desc->syscall_name + string("\"");
    }

    fprintf(out, "Bad syscall: %s, syscall ID %s %s\n", reason,
            fmt_i64(syscall_num), print_name.c_str());
    fprintf(out, "  App A%d pc 0x%s inst# %s syscall# %s cyc %s\n", 
            as->app_id, fmt_x64(as->npc), fmt_i64(as->stats.total_insts),
            fmt_i64(as->stats.total_syscalls),
            as->syscall_state->fmt_clock().c_str());

   fprintf(out, "  v0: 0x%s\n", fmt_x64(as->R[REG_V0].u));
    for (int a_reg = 0; a_reg <= 5; a_reg++)
        fprintf(out, "  a%d: 0x%s\n", a_reg, fmt_x64(as->R[REG_A0 + a_reg].u));
}


// Prepare for a indirect syscall invocation (SYS_syscall) by shuffling
// arguments around.  Note that this is currently _NOT_ really correct,
// since it needlessly scribbles on REG_A0+ which a real kernel wouldn't
// do; we really ought to have syscall args be explicit in SyscallState,
// instead of vandalizing user-space registers.  With C calling conventions
// at least, that's actually OK to do, so it's not clear we'll ever bother
// fixing this (and changing code all over this file to use the new
// args).
void
prepare_indirect_syscall(AppState *as)
{
    i64 child_syscall = as->R[REG_A0].i;
    int child_nargs = 0;        // number of arguments to child syscall

    if (!IDX_OK(child_syscall, MAX_SYSCALL)) {
        report_bad_syscall(stderr, as, child_syscall, "unrecognized child");
        sim_abort();
    }

    const SyscallDesc *sc_desc = &syscall_table[child_syscall];
    if (child_syscall == a_SYS_syscall) {
        // The caller is requestion more than one level of syscall
        // indirection.  In testing on OSF V4.0, attempting this maneuver
        // resulted in the process getting a SIGSYS, #12, "Invalid parameter
        // to system call.", so it's reasonable to freak out over it.
        report_bad_syscall(stderr, as, child_syscall, "illegal indirect");
        sim_abort();
    }

    while ((child_nargs < NELEM(sc_desc->args))
           && (sc_desc->args[child_nargs] != NO_ARG)) {
        ++child_nargs;
    }

    for (int child_arg_num = 0; child_arg_num < child_nargs; ++child_arg_num) {
        if (child_arg_num >= 6) {
            // Args past the 6th arg to child syscall (7th arg to this
            // syscall) come from the stack.  The 6th child-arg can fit in
            // reg a5, but beyond that, it's not clear how we should
            // proceed: a syscall would expect that arg at 0(sp), so we
            // may have to get into the business of shuffling the stack
            // pointer around here.  (Right now, our syscall table doesn't
            // allow for more than 6 args, so this can't occur.)
            abort_printf("not sure where to put >=7th arg to child "
                         "syscall (child_nargs: %d)", child_nargs);
        } else if (child_arg_num == 5) {
            mem_addr src_va = as->R[REG_SP].u;
            int dst_reg = REG_A5;
            as->R[dst_reg].i = pmem_read_64(as->pmem, src_va, 0);
        } else {
            int src_reg = REG_A0 + child_arg_num + 1,
                dst_reg = REG_A0 + child_arg_num;
            as->R[dst_reg].i = as->R[src_reg].i;
        }
    }
    as->R[REG_V0].i = child_syscall;
}


// edit a pathname, collapsing "/../" elements and removing "/./" and "//"
// elements.  this is sort of like realpath(3), but without actually checking
// the filesystem.
string
collapse_path(const string& path, bool *is_absolute_ret)
{
    const char *path_sep = "/";
    const char *path_up = "..";
    const char *path_nop = ".";
    int sep_len = strlen(path_sep); 

    vector<string> components;
    bool is_absolute = false;

    // split path into components, handling e.g. ".." as we go
    // based on good old KVPathParser::parse() and KVPathParser::resolve()
    bool done_scanning = false;
    string::size_type scan_idx = 0;
    while (!done_scanning) {
        string::size_type found_at = path.find(path_sep, scan_idx);
        string next_component;
        // first: figure out next component of path, and update scan state
        if (found_at != string::npos) {
            next_component = path.substr(scan_idx, found_at - scan_idx);
            scan_idx = found_at + sep_len;
        } else {
            next_component = path.substr(scan_idx);
            done_scanning = true;
        }

        // then, decide what to do with this component
        if (next_component.empty()) {
            // empty; ignore, but iff a separator came first, it's absolute
            if (found_at == 0) {
                is_absolute = true;
            }
        } else if (next_component == path_nop) {
            // ignore
        } else if (next_component == path_up) {
            // scanning left to right, ".." wants to eat the thing to the
            // left of it.  however, it can't eat past the start of the
            // path, and it can't eat another "..".  (we'll only
            // accumulate ".." at the start for non-absolute paths)
            if (components.empty()) {
                if (is_absolute) {
                    // eating leading ".." for absolute paths
                } else {
                    // accumulate leading ".." for relative paths
                    components.push_back(path_up);
                }
            } else {
                if (components.back() != path_up) {
                    components.pop_back();
                }
            }
        } else {
            components.push_back(next_component);
        }
    }

    // glue components back together into new path
    string result_path;
    if (is_absolute)
        result_path += path_sep;
    FOR_CONST_ITER(vector<string>, components, comp_iter) {
        if (comp_iter != components.begin())
            result_path += path_sep;
        result_path += *comp_iter;
    }

    if (is_absolute_ret)
        *is_absolute_ret = is_absolute;

    if (0) {        // in case we need to peek under the hood
        printf("\ncomponents:\n");
        for (int i = 0; i < intsize(components); ++i) {
            printf("  [%d]: \"%s\"\n", i, components[i].c_str());
        }
        printf("collapse_path(\"%s\") -> \"%s\", abs %d\n", path.c_str(),
               result_path.c_str(), is_absolute);
    }
    return result_path;
}


}       // Anonymous namespace close


void
syscalls_store_alpha_stat(SyscallState *sst,
                          ProgMem *pmem, mem_addr va,
                          const struct stat *host_stat)
{
    struct stat stat_copy = *host_stat;
    if (sst->conf.sanitize_host_device_info) {
        stat_copy.st_atime = 1233494464;
        stat_copy.st_mtime = 1261461159;
        stat_copy.st_ctime = 1261461159;
    }

    u64 dev_id = sst->alpha_dev_ids.host_to_alpha(stat_copy.st_dev);
    sim_assert(dev_id <= U32_MAX);      // overflow
    u64 inode_num = sst->alpha_inode_nums.host_to_alpha(stat_copy.st_ino);
    sim_assert(inode_num <= U32_MAX);   // overflow

    pmem_write_memset(pmem, va, 0, AlphaStructStatBytes, 0);
    pmem_write_32(pmem, va+ 0, dev_id, 0);                      // st_dev
    pmem_write_32(pmem, va+ 4, inode_num, 0);                   // st_ino
    // (Assumes mode values match)
    pmem_write_32(pmem, va+ 8, stat_copy.st_mode, 0);           // st_mode
    pmem_write_16(pmem, va+12, stat_copy.st_nlink, 0);          // st_nlink
    // 2 bytes padding
    pmem_write_32(pmem, va+16, sst->alpha_uid, 0);              // st_uid    
    pmem_write_32(pmem, va+20, sst->alpha_gid, 0);              // st_gid    
    pmem_write_32(pmem, va+24, 0, 0);                           // st_rdev
    // 4 bytes padding
    pmem_write_64(pmem, va+32, stat_copy.st_size, 0);           // st_size   
    pmem_write_32(pmem, va+40, stat_copy.st_atime, 0);          // st_atime  
    // pmem_write_32(pmem, va+44, ??, 0);                       // st_spare1 
    pmem_write_32(pmem, va+48, stat_copy.st_mtime, 0);          // st_mtime  
    // pmem_write_32(pmem, va+52, ??, 0);                       // st_spare2 
    pmem_write_32(pmem, va+56, stat_copy.st_ctime, 0);          // st_ctime  
    // pmem_write_32(pmem, va+60, ?? , 0);                      // st_spare3 
    pmem_write_32(pmem, va+64, 8192, 0);                        // st_blksize
    u32 block_count = 1 + (stat_copy.st_size / 8192);
    pmem_write_32(pmem, va+68, block_count, 0);                 // st_blocks 
    pmem_write_32(pmem, va+72, 0x79c850, 0);                    // st_flags
    pmem_write_32(pmem, va+76, 0, 0);                           // st_gen
}

int
syscalls_store_alpha_stat_osf5(ProgMem *pmem, mem_addr va, const struct stat *host_stat)
{   // Added by VK
    // The statistics that are updated are the one updated in m5. 
    // The offsets were collected by compiling a simple program that calls lstat on metallica
    const int size = 160;
    pmem_write_memset(pmem, va, 0, size, 0);
    pmem_write_32(pmem, va+0,    host_stat->st_dev, 0);           // st_dev
    pmem_write_32(pmem, va+0x08, host_stat->st_mode, 0);       // st_mode
    pmem_write_16(pmem, va+0x0c, host_stat->st_nlink, 0);      // st_nlink
    pmem_write_32(pmem, va+0x10, host_stat->st_uid, 0);        // st_uid    
    pmem_write_32(pmem, va+0x14, host_stat->st_gid, 0);        // st_gid    
    pmem_write_32(pmem, va+0x18, host_stat->st_rdev, 0);       // st_rdev
    pmem_write_64(pmem, va+0x20, host_stat->st_size, 0);       // st_size   
    pmem_write_32(pmem, va+0x2c, host_stat->st_atime, 0);      // st_uatime  
    pmem_write_32(pmem, va+0x34, host_stat->st_mtime, 0);      // st_umtime
    pmem_write_32(pmem, va+0x3c, host_stat->st_ctime, 0);      // st_uctime  
    pmem_write_32(pmem, va+0x70, host_stat->st_ino, 0);        // st_ino
    pmem_write_32(pmem, va+0x78, host_stat->st_atime, 0);      // st_atimeX
    pmem_write_32(pmem, va+0x80, host_stat->st_mtime, 0);      // st_mtimeX
    pmem_write_32(pmem, va+0x88, host_stat->st_ctime, 0);      // st_ctimeX
    pmem_write_64(pmem, va+0x90, 8192, 0);                     // st_blksize // retired
    pmem_write_64(pmem, va+0x98, host_stat->st_blocks, 0);     // st_blocks
    return size;
}

int
syscalls_store_alpha_stat64(ProgMem *pmem, mem_addr va, const struct stat *host_stat)
{
    const int size = 136;
    pmem_write_memset(pmem, va, 0, size, 0);
    pmem_write_64(pmem, va+ 0, host_stat->st_dev, 0);           // st_dev
    pmem_write_64(pmem, va+ 8, host_stat->st_ino, 0);           // st_ino
    pmem_write_64(pmem, va+16, host_stat->st_rdev, 0);          // st_rdev
    pmem_write_64(pmem, va+24, host_stat->st_size, 0);          // st_size   
    pmem_write_64(pmem, va+32, host_stat->st_blocks, 0);        // st_blocks
    // (Assumes mode values match)
    pmem_write_32(pmem, va+40, host_stat->st_mode, 0);          // st_mode
    pmem_write_32(pmem, va+44, host_stat->st_uid, 0);           // st_uid    
    pmem_write_32(pmem, va+48, host_stat->st_gid, 0);           // st_gid    
    pmem_write_32(pmem, va+52, 8192, 0);                        // st_blksize // retired
    pmem_write_32(pmem, va+56, host_stat->st_nlink, 0);         // st_nlink
    // 4 bytes padding						// st_ldev
    pmem_write_64(pmem, va+64, host_stat->st_atime, 0);         // st_atimeX
    pmem_write_64(pmem, va+72, host_stat->st_atime, 0);         // st_uatime  
    pmem_write_32(pmem, va+80, host_stat->st_mtime, 0);         // st_mtimeX
    pmem_write_32(pmem, va+88, host_stat->st_mtime, 0);         // st_umtime
    pmem_write_32(pmem, va+96, host_stat->st_ctime, 0);         // st_ctimeX
    pmem_write_32(pmem, va+104, host_stat->st_ctime, 0);         // st_uctime  
    // Magic value: "/* arbitrary number I saw repeated a couple times */"

    return size;
}


void
syscalls_load_alpha_flock(ProgMem *pmem, struct flock *host_flock_ret,
                          mem_addr src_va)
{
    host_flock_ret->l_type =   pmem_read_16(pmem, src_va+ 0, 0);
    host_flock_ret->l_whence = pmem_read_16(pmem, src_va+ 2, 0);
    host_flock_ret->l_start =  pmem_read_64(pmem, src_va+ 8, 0);
    host_flock_ret->l_len =    pmem_read_64(pmem, src_va+16, 0);
    // consider using sst->alpha_pid instead of native PID, if appropriate
    // (we'll need to virtualize multiple PIDs if we want to allow inter-app
    // file locking... which we don't use at the moment)
    host_flock_ret->l_pid =    pmem_read_32(pmem, src_va+24, 0);
}


void
syscalls_store_alpha_flock(ProgMem *pmem, mem_addr dst_va,
                           const struct flock *host_flock)
{
    pmem_write_memset(pmem, dst_va, 0, AlphaStructFlockBytes, 0);
    pmem_write_16(pmem, dst_va+ 0, host_flock->l_type, 0);
    pmem_write_16(pmem, dst_va+ 2, host_flock->l_whence, 0);
    pmem_write_64(pmem, dst_va+ 8, host_flock->l_start, 0);
    pmem_write_64(pmem, dst_va+16, host_flock->l_len, 0);
    // consider using sst->alpha_pid instead of native PID, if appropriate
    // (we'll need to virtualize multiple PIDs if we want to allow inter-app
    // file locking... which we don't use at the moment)
    pmem_write_32(pmem, dst_va+24, host_flock->l_pid, 0);
}


i64
syscalls_store_alpha_dirent(SyscallState *sst,
                            ProgMem *pmem, mem_addr va,
                            i64 buf_bytes_remaining,
                            const struct dirent *host_de)
{
    const char *fname = "syscalls_store_alpha_dirent";
    u64 dir_entry_inode =
        sst->alpha_inode_nums.host_to_alpha(host_de->NATIVE_DIRENT_FILENUM);
    sim_assert(dir_entry_inode <= U32_MAX);     // overflow

    // "Record" length: total #bytes from the start of this record to the
    // start of the next, rounded up to the next 32-bit boundary.
    int dir_name_bytes = strlen(host_de->d_name);       // bytes in name
    int dir_rec_bytes = AlphaDirEntHeaderBytes +
        dir_name_bytes + 1;                             // +1 for NUL
    dir_rec_bytes = (dir_rec_bytes + 3) & ~3;
    if (dir_rec_bytes > buf_bytes_remaining)
        return -1;

    // At this point, we have enough target buffer space for the entire entry
    // name; before copying, make sure that we don't exceed alpha
    // _PC_NAME_MAX.
    int copy_bytes = dir_name_bytes;
    if (dir_name_bytes > Alpha_PC_NAME_MAX) {
        err_printf("%s: A%d, dir entry name length (%d) exceeds alpha "
                   "max (%d); truncating\n", fname, sst->as->app_id,
                   dir_name_bytes, Alpha_PC_NAME_MAX);
        copy_bytes = Alpha_PC_NAME_MAX;
    }

    pmem_write_memset(pmem, va, 0, dir_rec_bytes, 0);
    pmem_write_32(pmem, va+ 0, dir_entry_inode, 0);                // d_ino
    pmem_write_16(pmem, va+ 4, dir_rec_bytes, 0);                  // d_reclen
    pmem_write_16(pmem, va+ 6, dir_name_bytes, 0);                 // d_namlen
    pmem_write_memcpy(pmem, va+ 8, host_de->d_name, copy_bytes, 0);// d_name
    pmem_write_8(pmem, va + 8 + copy_bytes, '\0', 0);
    return dir_rec_bytes;
}



/*
 * decides which syscall it should do
 * returns non-zero to signal exit, zero for continue
 */


// Macros specifically for use in the context of syscalls_dosyscalls(), just to
// save typing and clutter with all of the common variables used.

// Nonspeculative, aligned memory access within syscalls
#define SC_LOAD_16(addr) pmem_read_16(as->pmem, (addr), 0)
#define SC_LOAD_32(addr) pmem_read_32(as->pmem, (addr), 0)
#define SC_LOAD_64(addr) pmem_read_64(as->pmem, (addr), 0)
#define SC_STORE_16(addr, val) pmem_write_16(as->pmem, (addr), (val), 0)
#define SC_STORE_32(addr, val) pmem_write_32(as->pmem, (addr), (val), 0)
#define SC_STORE_64(addr, val) pmem_write_64(as->pmem, (addr), (val), 0)

// In "working-set-migration-branch" (and some others), these hook into
// AppStateDelta.  They're no-ops here, since that code was never finished;
// we put in dead-code uses to avoid compiler warnings
#define LOG_REG_USE(reg_num) \
    while (0) { fmt_i64(reg_num); }
#define LOG_REG_DEF(reg_num) \
    while (0) { fmt_i64(reg_num); }
#define LOG_MEM_USE(va, bytes) \
    while (0) { fmt_i64(va + bytes); }
#define LOG_MEM_DEF(va, bytes) \
    while (0) { fmt_i64(va + bytes); }
#define LOG_MEM_USE_IFNZ(va, bytes) \
    while (0) { fmt_i64(va + bytes); }
#define LOG_MEM_DEF_IFNZ(va, bytes) \
    while (0) { fmt_i64(va + bytes); }
#define LOG_EXIT()


// The syscall "call" interface is a little different than usual functions;
// it takes the syscall number in V0, and the arguments in A0-A3 (and some on
// the stack? -- see a_SYS_syscall case).  If the syscall succeeded, A3 is
// set to 0 and V0 to a return value.  On a failure, A3 is set to 1 and V0
// to an error code (alpha errno).
//
// In the FreeBSD Alpha trap handler (src/sys/alpha/alpha/trap.c), an
// additional register (A4) is used to hold an additional output only for
// successful syscalls.

// Oddly, the a_SYS_syscall and a_SYS_select routines used to pass their fifth
// argument (arg #4) through the stack in a 32-bit value at REG_SP+16 (search
// for REG_SP).  Inspection of code compiled and statically linked under OSF/1
// V4.0 shows the fifth argument being passed through A4, however.

// Warning: this function has screwy control flow for the SYS_syscall case,
// it goto-jumps back near the start of itself.
int 
syscalls_dosyscall(struct AppState * as, i64 local_clock) 
{
    //const char *fname = "syscalls_dosyscall";
    mem_addr arg0, arg1, arg2;
    int ret;
    SyscallState * sst = as->syscall_state;
    i64 this_syscall = as->stats.total_syscalls;
    int return_value = 0;
    int syscall_errno;          // 0 for success, otherwise *NATIVE* errno
    i64 syscall_retval;         // output value for successful syscalls

    sim_assert(sst->as == as);

    sst->local_clock = local_clock;
    if (!sst->alpha_pid) {
        // Pick a fixed PID to avoid leaking in values from host
        sst->alpha_pid = u32(as->app_id + 100);
    }

#ifdef DEBUG
    if (this_syscall == sst->conf.debug_syscall_num) {
        debug = 1;
        fflush(0);
    }
    if (debug)
        printf("A%d syscall# %s cyc %s\n", as->app_id,
               fmt_i64(this_syscall), sst->fmt_clock().c_str());
#endif
    traceout("SYSCALL A%d ", as->app_id);       // newline added per-syscall
//        fprintf(stderr,"A%d syscall(%d)# %s cyc %s\n", as->app_id,(int)as->R[REG_V0].i,
//               fmt_i64(this_syscall), sst->fmt_clock().c_str());

    sim_assert(as->app_id >= 0);
    as->stats.total_syscalls++;

    LOG_REG_USE(REG_V0);

    if (as->R[REG_V0].i == a_SYS_syscall) {
        COVERAGE_SYSCALL("syscall");
        traceout("syscall - ");
        // cruftily rewrites REG_V0, REG_A0...
        prepare_indirect_syscall(as);
    }

    int syscall_num, syscall_type;
    const SyscallDesc *syscall_desc;
    if (IDX_OK(as->R[REG_V0].i, MAX_SYSCALL)) {
        syscall_num = (int) as->R[REG_V0].i;
        syscall_desc = &syscall_table[syscall_num];
        syscall_type = syscall_desc->syscall_type;

        for (int arg_num = 0; arg_num < NELEM(syscall_desc->args);
             arg_num++) {
            int arg_type = syscall_desc->args[arg_num];
            // Cheesy: exploit that arg. registers are contiguous
            if (arg_type != NO_ARG) {
                LOG_REG_USE(REG_A0 + arg_num);
            }
        }
    } else {
        syscall_num = -1;
        syscall_type = SyscallOutOfRange;
        syscall_desc = NULL;
    }

    syscall_errno = 0;

    switch (syscall_type) {
    case SyscallOutOfRange:
        COVERAGE_SYSCALL("out-of-range");
        report_bad_syscall(stderr, as, syscall_num,
                           "unrecognized (out-of-range)");
        syscall_retval = 0;     // unused
        sim_abort();
        break;

    case BAD_SYSCALL: {
        COVERAGE_SYSCALL("bad");
        report_bad_syscall(stderr, as, syscall_num, "unknown/unimplmented");
        syscall_retval = 0;     // unused
        sim_abort();
        break;
    }

    /* the difference between UNIX_SYSCALL and SPC_SYSCALL definitions
       is now more historical than useful, but we still have the 
       distinction here. */
    case UNIX_SYSCALL:
        if(SysTrace || debug) {
            print_syscall_ascall(stdout, as, syscall_num);
            printf(" =");
        }
        
        // XXX these sketchy non-type-safe translation routines also don't
        // necessarily do all the LOG_MEM_USE / LOG_MEM_DEF we'll need to
        // to make AppStateDelta logs.  In general, they should just go away.
        arg0 = syscall_arg_xlate(as, syscall_num, 0, REG_A0);
        arg1 = syscall_arg_xlate(as, syscall_num, 1, REG_A1);
        arg2 = syscall_arg_xlate(as, syscall_num, 2, REG_A2);
        //(unused in case-statement-o-rama)
        //arg3 = syscall_arg_xlate(as, syscall_num, 3, REG_A3);
        //arg4 = syscall_arg_xlate(as, syscall_num, 4, REG_A4);
        //arg5 = syscall_arg_xlate(as, syscall_num, 5, REG_A5);
        
        if (syscall_num == a_SYS_write) {
            if (arg0 == 0) {
                // No writing to stdin; redirect to stdout.  It seems like
                // we ought to just make this return -1 for error, but
                // some *nix allow it.
                arg0 = 1;
            }
        }

        switch(syscall_num) {
        case a_SYS_open: {
            COVERAGE_SYSCALL("open");
            SimulatedFD *new_fd = sst->fd_create();
            if (!new_fd) {
                syscall_retval = -1;
                syscall_errno = EMFILE;     // no more FDs for this app
            } else {
                new_fd->sim_open((const char *) u64_to_ptr(arg0), arg1, arg2);
                if (new_fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = new_fd->host_errno();
                    sst->fd_destroy(new_fd);
                } else {
                    // success
                    syscall_retval = new_fd->get_alpha_fd();
                }
            }
            break;
        }
        case a_SYS_close: {
            COVERAGE_SYSCALL("close");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else if (fd->sim_close() != 0) {
                syscall_retval = -1;
                syscall_errno = fd->host_errno();
            } else {
                sst->fd_destroy(fd);
                syscall_retval = 0;         // success
            }
            break;
        }
        case a_SYS_fsync: {
            COVERAGE_SYSCALL("fsync");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_fsync();
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_lseek: {
            COVERAGE_SYSCALL("lseek");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_lseek(arg1, arg2);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_ftruncate: {
            COVERAGE_SYSCALL("ftruncate");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_ftruncate(arg1);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_write: {
            COVERAGE_SYSCALL("write");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_write(as->pmem, arg1, arg2);
                if (syscall_retval > 0)
                    LOG_MEM_USE(arg1, syscall_retval);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_read: {
            COVERAGE_SYSCALL("read");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_read(as->pmem, arg1, arg2);
                if (syscall_retval > 0)
                    LOG_MEM_DEF(arg1, syscall_retval);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_unlink: {
            COVERAGE_SYSCALL("unlink");
            ret = unlink((const char *) u64_to_ptr(arg0));
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
        }
        case a_SYS_rename: {
            COVERAGE_SYSCALL("rename");
            ret = rename((const char *) u64_to_ptr(arg0),
                         (const char *) u64_to_ptr(arg1));
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
        }
        case linux_gettimeofday:
        case a_SYS_gettimeofday: {
            COVERAGE_SYSCALL("gettimeofday");
            struct timeval fake_time;
            cyc_to_timeval(sst->local_clock, &fake_time);
            int size = store_alpha_timeval(as->pmem, as->R[REG_A0].u,
                                           &fake_time);
            LOG_MEM_DEF(as->R[REG_A0].u, size);
            if (as->R[REG_A1].u) {
                size = store_alpha_timezone(as->pmem, as->R[REG_A1].u);
                LOG_MEM_DEF(as->R[REG_A1].u, size);
            }
            syscall_retval = 0;
            break;
        }
        case linux_getrusage: {
            COVERAGE_SYSCALL("linux_getrusage");
            int size = linux_store_alpha_rusage(sst, as->pmem, as->R[REG_A1].u);
            LOG_MEM_DEF(as->R[REG_A1].u, size);
            syscall_retval = 0;
            break;
        }
        case a_SYS_getrusage: {
            COVERAGE_SYSCALL("getrusage");
            int size = store_alpha_rusage(sst, as->pmem, as->R[REG_A1].u);
            LOG_MEM_DEF(as->R[REG_A1].u, size);
            syscall_retval = 0;
            break;
        }

	    case a_SYS_getgroups: // Added by VK
            COVERAGE_SYSCALL("getgroups");
            // Get only sst->alpha_gid, always succeeds
            syscall_retval = 1;
            if (as->R[REG_A0].i != 0 && as->R[REG_A1].u != 0)
            {
                pmem_write_32(as->pmem, as->R[REG_A1].u, sst->alpha_gid, 0);
                LOG_REG_DEF(REG_A1);
                traceout("[ gid: %s --> 0x%s ]", fmt_u64(sst->alpha_gid), 
                        fmt_x64(as->R[REG_A1].u));
            }
            break;
        case a_SYS_getgid:
            COVERAGE_SYSCALL("getgid");
            syscall_retval = sst->alpha_gid;
            as->R[REG_A4].u = sst->alpha_gid;   // for getegid() library call
            LOG_REG_DEF(REG_A4);
            break;
        case a_SYS_getuid:
            COVERAGE_SYSCALL("getuid");
            syscall_retval = sst->alpha_uid;
            as->R[REG_A4].u = sst->alpha_uid;   // for geteuid() library call
            LOG_REG_DEF(REG_A4);
            break;
        case a_SYS_setsysinfo:
            COVERAGE_SYSCALL("setsysinfo");
            if(as->R[REG_A0].u == 14)
            {
              as->R[FPCR_REG].u = pmem_read_64(as->pmem, as->R[REG_A1].u, 0);
              traceout("[set FPCR to %s]",fmt_x64(as->R[FPCR_REG].u));              
            }
            else
              traceout("[unknown setsysinfo op %s]",fmt_x64(as->R[REG_A0].u));              
            syscall_retval = 0; /* good enough in most cases. */
            break;
        case a_SYS_getsysinfo:
            if(as->R[REG_A0].u == 45)
            {
              pmem_write_64(as->pmem, as->R[REG_A1].u, as->R[FPCR_REG].u, 0);        // f_fsid
              traceout("[get FPCR as %s]",fmt_x64(as->R[FPCR_REG].u));              
            }
            else
              traceout("[unknown getsysinfo op %s]",fmt_x64(as->R[REG_A0].u));              
          syscall_retval = 0; /* good enough in most cases. */
          break;
        case a_SYS_statfs: {
            COVERAGE_SYSCALL("statfs");
            struct statfs statfsbuf;
            // (not const char: alpha statfs prototype lacks const)
            ret = statfs((char *) u64_to_ptr(arg0), &statfsbuf);
            if (ret >= 0) {
                int size = store_alpha_statfs(sst, as->pmem,
                                              as->R[REG_A1].u, &statfsbuf);
                LOG_MEM_DEF(as->R[REG_A1].u, size);
            }
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
          }
        case a_SYS_fcntl: {
            COVERAGE_SYSCALL("fcntl");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_fcntl(as->pmem, arg1, arg2);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_getdirentries: {
            COVERAGE_SYSCALL("getdirentries");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_getdirentries(as->pmem, arg1, arg2,
                                                       as->R[REG_A3].u);
                if (syscall_retval > 0)
                    LOG_MEM_DEF(arg1, syscall_retval);
                LOG_MEM_DEF(as->R[REG_A3].u, 8);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_umask:
            COVERAGE_SYSCALL("umask");
            syscall_retval = 0077;      // Octal
            break;
        case a_SYS_getpid:
            COVERAGE_SYSCALL("getpid");
            syscall_retval = sst->alpha_pid;
            break;
        case linux_alpha_uname: { 
            int size = store_linux_alpha_utsname(as->pmem,
                                           as->R[REG_A0].u);
            LOG_MEM_DEF(as->R[REG_A0].u, size);
            syscall_retval = 0;
            break;
            }
        case a_SYS_uname: {
            COVERAGE_SYSCALL("uname");
            int size = store_alpha_utsname(as->pmem,
                                           as->R[REG_A0].u);
            LOG_MEM_DEF(as->R[REG_A0].u, size);
            syscall_retval = 0;
            break;
        }
        case a_SYS_fstat: {
            COVERAGE_SYSCALL("fstat");
            SimulatedFD *fd = sst->fd_lookup(arg0);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_fstat(as->pmem, arg1);
                if (syscall_retval != -1)
                    LOG_MEM_DEF(arg1, AlphaStructStatBytes);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            break;
        }
        case a_SYS_lstat:
            COVERAGE_SYSCALL("lstat");
        case a_SYS_stat: {
            if (syscall_num == a_SYS_stat) { COVERAGE_SYSCALL("stat"); }
            struct stat host_stat;
            if (syscall_num == a_SYS_lstat) {
                ret = lstat((const char *) u64_to_ptr(arg0), &host_stat);
            } else {
                ret = stat((const char *) u64_to_ptr(arg0), &host_stat);
            }
            if (ret != -1) {
                syscalls_store_alpha_stat(sst, as->pmem, as->R[REG_A1].u,
                                          &host_stat);
                LOG_MEM_DEF(as->R[REG_A1].u, AlphaStructStatBytes);
            }
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
        }
        case a_SYS_new_lstat: // Added by VK
            COVERAGE_SYSCALL("new_lstat");
        case a_SYS_new_stat: { // Added by VK
            if (syscall_num == a_SYS_stat) { COVERAGE_SYSCALL("stat"); }
            struct stat host_stat;
            if (syscall_num == a_SYS_new_lstat) {
                ret = lstat((const char *) u64_to_ptr(arg0), &host_stat);
            } else {
                ret = stat((const char *) u64_to_ptr(arg0), &host_stat);
            }
            if (ret != -1) {
                int size = syscalls_store_alpha_stat_osf5(as->pmem, as->R[REG_A1].u,
                                          &host_stat);
                LOG_MEM_DEF(as->R[REG_A1].u, size);
            }
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
        }
        case a_SYS_new_fstat: { // Added by VK
            struct stat host_stat;
            ret = fstat(arg0, &host_stat);
            if (ret != -1) {
                int size = syscalls_store_alpha_stat_osf5(as->pmem, as->R[REG_A1].u,
                                            &host_stat);
                LOG_MEM_DEF(as->R[REG_A1].u, size);
            }
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
        }
        case linux_alpha_fstat64: {
            struct stat host_stat;
            ret = fstat(arg0, &host_stat);
                // if ((as->R[REG_A0].i == 0) || (as->R[REG_A0].i == 1))
                //   host_stat.st_mode = 4096;          // Magic value (?)
            if (ret != -1) {
                int size = syscalls_store_alpha_stat64(as->pmem, as->R[REG_A1].u,
                                            &host_stat);
                LOG_MEM_DEF(as->R[REG_A1].u, size);
            }
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
        }

        case linux_alpha_stat64: {
            struct stat host_stat;
            ret = stat((const char *) u64_to_ptr(arg0), &host_stat);
                // if ((as->R[REG_A0].i == 0) || (as->R[REG_A0].i == 1))
                //   host_stat.st_mode = 4096;          // Magic value (?)
            if (ret != -1) {
                int size = syscalls_store_alpha_stat64(as->pmem, as->R[REG_A1].u,
                                            &host_stat);
                LOG_MEM_DEF(as->R[REG_A1].u, size);
            }
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            break;
        }

        case a_SYS_mmap: {
//            mem_addr start = as->R[REG_A0].u;
            u64 length = as->R[REG_A1].u;
//            i64 prot = as->R[REG_A2].i;
//            i64 flags = as->R[REG_A3].i;
//            mem_addr fd = as->R[REG_A4].u;
//            i64 offset = as->R[REG_A5].i;
//            ProgMemSegment *seg = pmem_get_seg(as->pmem, start_addr);
//            sim_assert(seg);
            if(mmap_end == 0)
            {
              mem_addr current_break = pmem_get_base(as->pmem,as->seg_info.bss_start);
              mmap_end = current_break << 4;
            }
            LOG_MEM_USE(mmap_end, as->R[REG_A1].u);
            traceout("[mmap: start_addr: %s length: %s]",fmt_x64(mmap_end),fmt_i64(length));
//            fprintf(stderr,"[mmap: start_addr: %s length: %s]\n",fmt_x64(mmap_end),fmt_i64(length));
            if(pmem_map_new(as->pmem, length, mmap_end, PMAF_RW,PMCF_None))
            {
              printf("Cannot create mmap\n");
              fprintf(stderr,"[Cannot create mmap]\n");
              syscall_retval = u64_from_ptr(MAP_FAILED);
              syscall_errno = ENOMEM;
            } else {
              syscall_retval = mmap_end; // return the pointer
              mmap_end += length;
	    }
            break;
        }
        case a_SYS_munmap: {
            mem_addr start = as->R[REG_A0].u;
//            u64 length = as->R[REG_A1].u;
//            i64 prot = as->R[REG_A2].i;
//            i64 flags = as->R[REG_A3].i;
//            mem_addr fd = as->R[REG_A4].u;
//            i64 offset = as->R[REG_A5].i;
            if(start != 0)
              pmem_unmap(as->pmem,start);
//            mmap_end -= length;
            syscall_retval = 0; // return the pointer
//            syscall_errno = ENOMEM;
            break;
        }
        case linux_mremap: {
            mem_addr start = as->R[REG_A0].u;
//            u64 oldsize = as->R[REG_A1].u;
              u64 length = as->R[REG_A2].u;
//            i64 flags = as->R[REG_A3].i;
//            mem_addr fd = as->R[REG_A4].u;
//            i64 offset = as->R[REG_A5].i;
            ProgMemSegment *seg = pmem_get_seg(as->pmem, start); 
            sim_assert(seg); // seg cannot be NULL since it's resize!
            if(pms_resize(seg,length)) // try to resize the segment
            {
              pmem_unmap(as->pmem,start); //
              traceout("[mremap: start_addr: %s length: %s]",fmt_x64(mmap_end),fmt_i64(length));
              //fprintf(stderr,"[mremap: start_addr: %s length: %s]\n",fmt_x64(mmap_end),fmt_i64(length));
              if(pmem_map_new(as->pmem, length, mmap_end, PMAF_RW,PMCF_None))
              {
                traceout("[Cannot create mmap]");
                syscall_retval = u64_from_ptr(MAP_FAILED);
                syscall_errno = ENOMEM;
              } 
              else 
              {
                 syscall_retval = mmap_end; // return the new pointer
                 mmap_end += length;
              }
            }
	    else// if the segment can be resized in place, return the start address
	      syscall_retval = start;
            break;
        }

        case linux_exit_group: {
            /* close all file descriptors */
            traceout("exit(%s) = ??\n", fmt_i64(as->R[REG_A0].i));
            as->npc = 0;
            as->exit.has_exit = 1;
            as->exit.exit_code = as->R[REG_A0].i;
            LOG_EXIT();
            syscall_retval = 0; // unused
            break;
        }
        case linux_times:
        case rt_sigaction:{
            syscall_retval = 0; // ignore the syscall
            break;
        } 
        default:
            COVERAGE_SYSCALL("unimplemented-unix");
            report_bad_syscall(stderr, as, syscall_num,
                               "unimplmented (unix)");
            syscall_retval = 0;   // unused
            sim_abort();
            break;
        }
        traceout(" %s\n", fmt_i64(syscall_retval));
        break;
        // end of UNIX_SYSCALL case

      case SPC_SYSCALL:
        /* These special syscalls need to be emulated */
        switch(syscall_num) {
        case a_SYS_exit: {
            COVERAGE_SYSCALL("exit");
            /* close all file descriptors */
            traceout("exit(%s) = ??\n", fmt_i64(as->R[REG_A0].i));
            as->npc = 0;
            as->exit.has_exit = 1;
            as->exit.exit_code = as->R[REG_A0].i;
            LOG_EXIT();
            syscall_retval = 0; // unused
            break;
        }
        case a_SYS_select: {
            COVERAGE_SYSCALL("select");
            traceout("select(%s, 0x%s, 0x%s, 0x%s, 0x%s) =", 
                     fmt_i64(as->R[REG_A0].i), 
                     fmt_x64(as->R[REG_A1].i),
                     fmt_x64(as->R[REG_A2].i),
                     fmt_x64(as->R[REG_A3].i),
                     fmt_x64(as->R[REG_A4].i));
            LOG_MEM_USE_IFNZ(as->R[REG_A1].u, AlphaFDSetBytes);
            LOG_MEM_USE_IFNZ(as->R[REG_A2].u, AlphaFDSetBytes);
            LOG_MEM_USE_IFNZ(as->R[REG_A3].u, AlphaFDSetBytes);
            LOG_MEM_USE_IFNZ(as->R[REG_A4].u, AlphaTimeValBytes);
            ret = do_alpha_select(as->pmem, sst,
                                  as->R[REG_A0].i,
                                  as->R[REG_A1].u,
                                  as->R[REG_A2].u,
                                  as->R[REG_A3].u,
                                  as->R[REG_A4].u);
            if (ret >= 0) {
                LOG_MEM_DEF_IFNZ(as->R[REG_A1].u, AlphaFDSetBytes);
                LOG_MEM_DEF_IFNZ(as->R[REG_A2].u, AlphaFDSetBytes);
                LOG_MEM_DEF_IFNZ(as->R[REG_A3].u, AlphaFDSetBytes);
                LOG_MEM_DEF_IFNZ(as->R[REG_A4].u, AlphaTimeValBytes);
            }
            syscall_retval = ret;
            if (ret == -1) syscall_errno = errno;
            traceout(" %s\n", fmt_i64(syscall_retval));
        }
        case a_SYS_sbrk: {
            COVERAGE_SYSCALL("sbrk");
            traceout("sbrk(%s) =", fmt_i64(as->R[REG_A0].i));
            mem_addr start_addr = pmem_get_base(as->pmem,
                                                as->seg_info.bss_start);
            ProgMemSegment *seg = pmem_get_seg(as->pmem, start_addr);
            sim_assert(seg);
            if (!resize_seg(as, start_addr, as->R[REG_A0].i)) {
                syscall_retval = start_addr + pms_size(seg); // "break"
            } else {
                syscall_retval = 0;     // unused
                syscall_errno = ENOMEM;
            }
            traceout(" 0x%s\n", fmt_x64(syscall_retval));
            break;
        }
        case a_SYS_obreak: {
            COVERAGE_SYSCALL("obreak");
            mem_addr start_addr = pmem_get_base(as->pmem,
                                                as->seg_info.bss_start);
            i64 new_break = as->R[REG_A0].i;
            ProgMemSegment *seg = pmem_get_seg(as->pmem, start_addr);
            sim_assert(seg);
            i64 old_break = start_addr + pms_size(seg);
            traceout("brk(0x%s) =", fmt_x64(new_break));
            if(as->R[REG_A0].i == 0)	{
                syscall_retval = old_break;     // return current break if passing 0
            }
            else if (!resize_seg(as, start_addr, new_break - old_break)) {
                syscall_retval = new_break;
            } else {
                syscall_retval = 0;     // unused
                syscall_errno = ENOMEM;
            }
	    traceout(" 0x%s\n", fmt_x64(syscall_retval));
            break;
        }
        case a_SYS_setitimer: {
            COVERAGE_SYSCALL("setitimer");
            traceout("setitimer(%s, 0x%s, 0x%s) =",
                     fmt_i64(as->R[REG_A0].i),
                     fmt_x64(as->R[REG_A1].i),
                     fmt_x64(as->R[REG_A2].i));
            // Don't run setitimer() directly; 1) it'd make the simulator get
            // alarm signals which it currently doesn't do anything useful
            // with anyway, and 2) we don't have getitimer() anyway.
            //struct itimerval it;
            //it = *((struct itimerval *) 
            //       smem_address_r(as, as->R[REG_A1].i,
            //                    sizeof(struct itimerval)));
            //syscall_retval =
            //    setitimer(as->R[REG_A0].i, &it, (struct itimerval *)
            //            smem_address_w_nullok(as, as->R[REG_A2].i, 
            //                                sizeof(struct itimerval)));
            //as->R[REG_A3].i = (syscall_retval == -1) ? -1 : 0;
            syscall_retval = 0;         // unused
            syscall_errno = EINVAL;
            traceout(" %s\n", fmt_i64(syscall_retval));
            break;
        }
        case a_SYS_getrlimit:
            COVERAGE_SYSCALL("getrlimit");
            traceout("getrlimit(...) = 0\n");
            pmem_write_64(as->pmem, as->R[REG_A1].u, 
                          U64_LIT(0x7fffffffffffffff), 0);
            pmem_write_64(as->pmem, as->R[REG_A1].u + 8, 
                          U64_LIT(0x7fffffffffffffff), 0);
            LOG_MEM_DEF(as->R[REG_A1].u, AlphaRLimitBytes);
            syscall_retval = 0;
          break;
        case a_SYS_setrlimit:
            COVERAGE_SYSCALL("setrlimit");
            /* as is evident, we fool it to think that we set some limits */
            traceout("setrlimit(...) = 0\n");
            LOG_MEM_USE(as->R[REG_A1].u, AlphaRLimitBytes);
            syscall_retval = 0;
            break;
        case a_SYS_table: {
            COVERAGE_SYSCALL("table");
            if (SysTrace || debug) {
                print_syscall_ascall(stdout, as, syscall_num);
            }
            syscall_retval = do_alpha_table(as);
            if (syscall_retval == -1) syscall_errno = errno;
            traceout(" = %s\n", fmt_i64(syscall_retval));
            break;
        }
        case a_SYS_sigstack:
            COVERAGE_SYSCALL("sigstack");
            // Not terribly important since we currently don't support the
            // delivery of signals to simulated processes
            traceout("sigstack(0x%s, 0x%s) = 0\n", 
                     fmt_x64(as->R[REG_A0].i),
                     fmt_x64(as->R[REG_A1].i));
            syscall_retval = 0;
            if (as->R[REG_A1].u != 0) {
                pmem_write_64(as->pmem, as->R[REG_A1].u,
                              sst->old_sig.sigstack, 0);
                pmem_write_32(as->pmem, as->R[REG_A1].u + 8,
                              sst->old_sig.sigonstack, 0);
                pmem_write_32(as->pmem, as->R[REG_A1].u + 12, 0, 0);    // pad
                LOG_MEM_DEF(as->R[REG_A1].u, AlphaSigStackBytes);
            }
            if (as->R[REG_A0].u != 0) {
                LOG_MEM_USE(as->R[REG_A0].u, AlphaSigStackBytes);
                sst->old_sig.sigstack = pmem_read_64(as->pmem, 
                                                     as->R[REG_A0].u, 0);
                sst->old_sig.sigonstack = pmem_read_32(as->pmem, 
                                                       as->R[REG_A0].u + 8, 0);
            }
            break;
        case a_SYS_sigreturn:
            COVERAGE_SYSCALL("sigreturn");
            traceout("sigreturn(0x%s) ", fmt_x64(as->R[REG_A0].i));
            dosigreturn(as);
            traceout("-> reset A%d PC to %s + 4\n", as->app_id,
                     fmt_x64(as->npc));
            // Return directly: don't fall-through and scribble on
            // REG_V0 / REG_A3.
            syscall_retval = 0;
            return_value = 0;
            goto direct_return;
            break;
        case a_SYS_sigaction:
            COVERAGE_SYSCALL("sigaction");
            traceout("sigaction(%s,0x%s,0x%s) =",
                     fmt_i64(as->R[REG_A0].i),
                     fmt_x64(as->R[REG_A1].i),
                     fmt_x64(as->R[REG_A2].i));
            syscall_retval = dosigaction(as);
            if (syscall_retval == -1) syscall_errno = errno;
            traceout(" %s\n", fmt_i64(syscall_retval));
            break;
        case a_SYS_sigprocmask:
            COVERAGE_SYSCALL("sigprocmask");
            traceout("sigprocmask(%s, 0x%s) =",
                     fmt_i64(as->R[REG_A0].i),
                     fmt_x64(as->R[REG_A1].i));
            syscall_retval = do_alpha_sigprocmask(as,
                                                  as->R[REG_A0].i,
                                                  as->R[REG_A1].u);
            if (syscall_retval == -1) syscall_errno = errno;
            traceout(" %s\n", fmt_i64(syscall_retval));
            break;
        case a_SYS_getpagesize:
            COVERAGE_SYSCALL("getpagesize");
            traceout("getpagesize() = %d\n", 
                     GlobalParams.mem.page_bytes);
            syscall_retval = GlobalParams.mem.page_bytes;
            break;
        case a_SYS_ioctl: {
            COVERAGE_SYSCALL("ioctl");
            traceout("ioctl(%s, 0x%s, 0x%s) =", 
                     fmt_i64(as->R[REG_A0].i),
                     fmt_x64(as->R[REG_A1].i),
                     fmt_x64(as->R[REG_A2].i));
            SimulatedFD *fd = sst->fd_lookup(as->R[REG_A0].i);
            if (fd == NULL) {
                syscall_retval = -1;
                syscall_errno = EBADF;
            } else {
                syscall_retval = fd->sim_ioctl(as->pmem, as->R[REG_A1].i,
                                               as->R[REG_A2].u);
                if (fd->error()) {
                    syscall_retval = -1;
                    syscall_errno = fd->host_errno();
                }
            }
            traceout(" %s\n", fmt_i64(syscall_retval));
            break;
        }
        case a_SYS_uswitch:
            COVERAGE_SYSCALL("uswitch");
            traceout("uswitch(%s, %s) =",
                     fmt_i64(as->R[REG_A0].i),
                     fmt_i64(as->R[REG_A1].i));
            /*not really implemented, obviously*/
            syscall_retval = as->R[REG_A1].i; 
            traceout(" %s\n", fmt_i64(syscall_retval));
            break;
        default:
            COVERAGE_SYSCALL("unimplemented-special");
            report_bad_syscall(stderr, as, syscall_num,
                               "unimplmented (special)");
            syscall_retval = 0; // unused
            sim_abort();
            break;
        }
        break;
        // end of SPC_SYSCALL case

    default:
        COVERAGE_SYSCALL("unknown-type");
        report_bad_syscall(stderr, as, syscall_num, "unknown type");
        syscall_retval = 0;     // unused
        sim_abort();
        break;
    }

    if (syscall_errno == 0) {
        as->R[REG_V0].i = syscall_retval;
        as->R[REG_A3].i = 0;
    } else {
        as->R[REG_V0].i = syscall_errno_xlate(syscall_errno);
        as->R[REG_A3].i = 1;
    }
    LOG_REG_DEF(REG_V0);
    LOG_REG_DEF(REG_A3);

#ifdef DEBUG
    if (debug)
        printf("A%d syscall# %s return: value 0x%s error-flag %s\n",
               as->app_id, fmt_i64(this_syscall),
               fmt_x64(as->R[REG_V0].i),
               fmt_i64(as->R[REG_A3].i));
#endif

 direct_return:

    return return_value;

}


class SyscallState::FmtHereCB : public CBQ_Callback {
    SyscallState *sst_;
public:
    FmtHereCB(SyscallState *sst__) : sst_(sst__) { }
    i64 invoke(CBQ_Args *args) {
        SyscallFmtHereCBArgs *cast_args =
            dynamic_cast<SyscallFmtHereCBArgs *>(args);
        if (!cast_args) {
            abort_printf("bad args to SyscallState::FmtHereCB");
        }
        *(cast_args->output) = sst_->fmt_here();
        return 0;       // ignored
    }
};


SyscallState::SyscallState(const SyscallStateParams& params__,
                           struct AppState *nascent_astate__)
    : conf("Syscall"), as(nascent_astate__),
      local_clock(0),
      recording_delta_log(false), playing_delta_log(false),
      alpha_inode_nums(2851660),
      alpha_dev_ids(0x8100002)
{
    // WARNING: "nascent_astate__" may still be under construction, so don't
    // go monkeying with it!

    fmt_here_cb.reset(new FmtHereCB(this));

    prog_stdin = static_cast<FILE *>(params__.FILE_stdin);
    prog_stdout = static_cast<FILE *>(params__.FILE_stdout);
    prog_stderr = static_cast<FILE *>(params__.FILE_stderr);

    {
        // Manually set up FD numbers for simulated-stdio
        scoped_ptr<SimulatedFD>
            fd_stdin(new SimulatedFD(this, fmt_here_cb.get()));
        fd_stdin->open_cfile_read(prog_stdin, "(sim-stdin)");
        sim_assert(fd_stdin->is_open());
        sim_assert(!fd_stdin->error());
        fd_stdin->set_alpha_fd(0);
        map_put_uniq(valid_fds, 0, scoped_ptr_release(fd_stdin));

        scoped_ptr<SimulatedFD>
            fd_stdout(new SimulatedFD(this, fmt_here_cb.get()));
        fd_stdout->open_cfile_write(prog_stdout, "(sim-stdout)");
        sim_assert(fd_stdout->is_open());
        sim_assert(!fd_stdout->error());
        fd_stdout->set_alpha_fd(1);
        map_put_uniq(valid_fds, 1, scoped_ptr_release(fd_stdout));

        scoped_ptr<SimulatedFD>
            fd_stderr(new SimulatedFD(this, fmt_here_cb.get()));
        fd_stderr->open_cfile_write(prog_stderr, "(sim-stderr)");
        sim_assert(fd_stderr->is_open());
        sim_assert(!fd_stderr->error());
        fd_stderr->set_alpha_fd(2);
        map_put_uniq(valid_fds, 2, scoped_ptr_release(fd_stderr));
    }

    initial_working_dir.assign(params__.initial_working_dir);

    // local_path isn't so interesting without chdir(2) emulation
    local_path.assign(".");

    // Use constant UID/GID values so they don't leak in from host
    alpha_uid = 1367;
    alpha_gid = 313;
    // We'll set the PID based on the app ID#, but we can't do it yet --
    // "nascent_astate__" may still be bogus.  This will be reset on the
    // first syscall.
    alpha_pid = 0;

    memset(&old_sig, 0, sizeof(old_sig));
    rec.dst = NULL;
    play.src = NULL;
}


SyscallState::~SyscallState()
{
    FOR_ITER(SimulatedFDMap, valid_fds, iter) {
        SimulatedFD *fd = iter->second;
        delete fd;
    }
    valid_fds.clear();
}


string
SyscallState::fmt_clock() const
{
    string result;
    result = fmt_i64(local_clock);
    return result;
}


string
SyscallState::fmt_here() const
{
    std::ostringstream ostr;
    ostr << "A" << as->app_id << " pc 0x" << fmt_x64(as->npc)
         << " inst# " << fmt_i64(as->stats.total_insts)
         << " syscall# " << fmt_i64(as->stats.total_syscalls)
         << " cyc " << fmt_i64(local_clock);
    return ostr.str();
}


int
SyscallState::fd_new_alpha_fdnum() const
{
    // This is O(n lg n) worst-case -- needlessly slow -- but 1) we expect
    // small n, and 2) we can improve it later without changing the API.
    int scan_fd = -1;
    FOR_CONST_ITER(SimulatedFDMap, valid_fds, fd_map_iter) {
        // At loop top: scan_fd == "last-tried, already-exists FD" or -1
        i64 existing_fd = fd_map_iter->first;
        if (existing_fd != (scan_fd + 1)) {
            // we found a gap between last time and existing_fd; use it
            break;
        }
        scan_fd = existing_fd;
    }
    // Post-loop: scan_fd is either
    //   1) -1, if valid_fds is empty
    //   2) the highest existing FD, if valid_fds is (fully) densely packed
    //   3) an existing FD which sits just before a gap in the FD sequence
    // ...in all three cases, the (scan_fd + 1) is the first FD we can use.
    int new_alpha_fd = scan_fd + 1;

    if (new_alpha_fd >= ALPHA_OSF_MAXFDS)
        new_alpha_fd = -1;
    return new_alpha_fd;
}


SimulatedFD *
SyscallState::fd_create()
{
    SimulatedFD *new_fd = NULL;
    i64 new_alpha_fd = this->fd_new_alpha_fdnum();
    if (new_alpha_fd >= 0) {
        sim_assert(new_alpha_fd < ALPHA_OSF_MAXFDS);
        int new_alpha_fd_narrowed = new_alpha_fd;
        sim_assert(new_alpha_fd_narrowed == new_alpha_fd);
        new_fd = new SimulatedFD(this, fmt_here_cb.get()); 
        new_fd->set_alpha_fd(new_alpha_fd_narrowed);
        map_put_uniq(valid_fds, new_alpha_fd_narrowed, new_fd);
    }
    return new_fd;
}


void
SyscallState::fd_destroy(SimulatedFD *fd)
{
    int alpha_fd = fd->get_alpha_fd();
    SimulatedFDMap::iterator found = valid_fds.find(alpha_fd);
    if (found == valid_fds.end()) {
        abort_printf("fd_destroy: invalid simulated FD (%s) internally\n", 
                     fmt_i64(alpha_fd));
    }
    valid_fds.erase(found);
    delete fd;
}


SimulatedFD *
SyscallState::fd_lookup(i64 alpha_fd) const
{
    return map_at_default(valid_fds, alpha_fd, NULL);
}


struct SyscallState *
syscalls_create(const SyscallStateParams *params,
                struct AppState *nascent_astate)
{
    SyscallState *n = NULL;

    if (!(n = new SyscallState(*params, nascent_astate))) {
        fprintf(stderr, "%s: out of memory\n", __func__);
        exit(1);
    }
    return n;
}


void
syscalls_destroy(SyscallState *sst)
{
    delete sst;
}
