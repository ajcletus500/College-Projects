// -*- C++ -*-
//
// SimulatedFD: simulator support for a simulated file descriptors and
// operations.
//
// Jeff Brown
// $Id: syscalls-sim-fd.h,v 1.1.2.3.2.1 2009/12/25 06:31:52 jbrown Exp $
//

#ifndef SYSCALLS_SIM_FD_H
#define SYSCALLS_SIM_FD_H

#ifndef __cplusplus
#error "C++ only"
#endif

#include "utils-cc.h"
#include "callback-queue.h"

#include <set>
#include <string>


// Declared elsewhere
struct ProgMem;
struct SyscallState;


// OSF/alpha file-descriptor-related syscall emulation.  These mostly don't
// touch other simulator structures, they just try to emulate the direct
// effects of the given operations on a single file descriptor.  Some must
// touch simulated memory, but we mostly avoid AppStateDelta updates here to
// try and ease compatibility with simulators that don't use that
// not-yet-useful mechanism.
//
// The "sim_" prefixes on method names are to try and avoid symbol clashes
// or other confusion with e.g. the system's own open(2).
//
// These set the internal "errno_" to host-system "errno" codes as
// appropriate.


// Callback arguments for asking a SyscallState to format a string describing
// the current syscall location in the execution stream.
// (We could have also had an explicit up-call via a "parent SyscallState" 
// pointer; I'm just experimenting with this.)
struct SyscallFmtHereCBArgs : public CBQ_Args {
    std::string *output;        // output string value written here
    SyscallFmtHereCBArgs(std::string *output__) 
        : output(output__) { }
};


class SimulatedFD {
    typedef std::set<long> DirOffsetSet;
    typedef std::set<i64> AlphaRequestSet;

    SyscallState *parent_sst_;  // now owned; try not to use
    CBQ_Callback *fmt_here_cb_; // not owned; for printing errors; maybe NULL

    std::string host_path_;     // pathname of underlying file/dir/etc.
    int host_fd_;               // -1: invalid
    bool host_fd_owned_;        // flag: I opened this host_fd_
    int alpha_fd_;              // simulator-assigned "fake" fd number
    int errno_;                 // last error; 0: no error (uses host errno's)

    void *c_DIR_dir_;           // C "DIR *" pointer (NULL: not in use)
    bool dir_desyncd_;          // flag: later call has messed up DIR position
    DirOffsetSet dir_known_offsets_;    // legal values for seeking on this dir

    // we don't actually use alpha_fd_flags_ / alpha_fd_flags_ meaningfully;
    // we just keep them around to hand back in case we're asked about them
    i32 alpha_fd_flags_;        // fcntl: GETFD, SETFD 
    i32 alpha_file_flags_;      // fcntl: GETFL, SETFL
    i32 alpha_async_pid_;       // fcntl: GETOWN, SETOWN; not -1

    AlphaRequestSet alpha_ioctls_seen_;
    AlphaRequestSet alpha_fcntls_seen_;

    NoDefaultCopy nocopy;

    bool doing_dir_io() const { return c_DIR_dir_ != NULL; }
    void dir_teardown();
    std::string describe_syscall_loc() const;

public:
    SimulatedFD(SyscallState *nascent_parent_sst__,
                CBQ_Callback *fmt_here_cb__);
    ~SimulatedFD();

    bool is_open() const { return (host_fd_ >= 0) || (c_DIR_dir_ != NULL); }
    bool error() const { return errno_ != 0; }
    int host_errno() const { return errno_; }

    int get_alpha_fd() const { return alpha_fd_; }
    void set_alpha_fd(int new_alpha_fd);

    // Set up already-open FILE* streams as sources/targets for simulated I/O,
    // e.g. application stdout.
    void open_cfile_read(void *c_FILE_src, const std::string& filename_or_id);
    void open_cfile_write(void *c_FILE_dest,
                          const std::string& filename_or_id);

    // void-method to avoid confusion with the open(2) interface returning
    // a file descriptor
    void sim_open(const char *path_xlated, u64 alpha_flags, u64 alpha_mode);

    // returns 0 if ok, -1 on error
    int sim_close();

    // returns #bytes stored to memory, -1 on error
    i64 sim_read(ProgMem *pmem, mem_addr dst_va, u64 nbytes);

    // returns #bytes written to file, -1 on error
    i64 sim_write(ProgMem *pmem, mem_addr src_va, u64 nbytes);

    // returns offset, -1 on error
    i64 sim_lseek(i64 offset, i64 alpha_whence);

    // may read or write at "maybe_va"; returns -1 on error
    i64 sim_ioctl(ProgMem *pmem, i64 alpha_request,
                  mem_addr arg_va_maybe);

    // returns 0 if ok, -1 on error
    int sim_fstat(ProgMem *pmem, mem_addr dst_va);

    // return value varies by request, sigh, we don't support this well
    i64 sim_fcntl(ProgMem *pmem,
                  i64 alpha_request, u64 alpha_arg_maybe);

    // returns 0 if ok, -1 on error
    int sim_fsync();

    // returns 0 if ok, -1 on error
    int sim_ftruncate(i64 length);
    
    // returns #bytes written to "buf_va", EVEN ON ERROR; check error()
    i64 sim_getdirentries(ProgMem *pmem, mem_addr buf_va, i64 buf_bytes,
                          mem_addr basep_va);


    // These return true iff this descriptor fulfills criteria for matching
    // a select(2) call in each of the corresponding file-descriptor sets.
    bool sim_select_readfd() const;
    bool sim_select_writefd() const;
    bool sim_select_exceptfd() const;

    // Not implemented (yet, since they've not been used in simulated apps):
    //   dup
    //   dup2
    //   old_fstat
    //   readv
    //   writev
    //   fchown
    //   fchmod
};


#endif  // SYSCALLS_SIM_FD_H
