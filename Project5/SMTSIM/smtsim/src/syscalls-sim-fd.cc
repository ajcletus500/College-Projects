//
// SimulatedFD: simulator support for a simulated file descriptors and
// operations.
//
// Initially, almost all of these just redirect to a direct syscall of the
// same operation on a host-system file descriptor; however, with this
// abstraction in place, we'll have to freedom to implement things differently
// later, without having to worry about the guts of the monster
// syscalls_dosyscalls() switch-statement.
//
// Jeff Brown
// $Id: syscalls-sim-fd.cc,v 1.1.2.6.2.1 2009/12/25 06:31:51 jbrown Exp $
//

const char RCSid_1258860312[] = 
"$Id: syscalls-sim-fd.cc,v 1.1.2.6.2.1 2009/12/25 06:31:51 jbrown Exp $";

// Reminder: host-system includes are handled weirdly in syscall modules,
// due to cross-platform weirdness and fragility in headers.
#include "syscalls-includes.h"

#include "sys-types.h"
#include "syscalls-sim-fd.h"

#include "sim-assert.h"
#include "app-state.h"
#include "prog-mem.h"
#include "utils.h"
#include "utils-cc.h"
#include "hash-map.h"
#include "debug-coverage.h"

#include "syscalls-private.h"

using std::map;
using std::set;
using std::string;
using std::vector;


#define ENABLE_COVERAGE_SIMFD 0


namespace {

#define PERFORM_COVERAGE_SIMFD (ENABLE_COVERAGE_SIMFD && defined(DEBUG))
#if PERFORM_COVERAGE_SIMFD
    const char *FDCoverageNames[] = {
        // Sorted by name
        "sim_close", 
        "sim_fcntl", 
        "sim_fstat", 
        "sim_fsync", 
        "sim_ftruncate", 
        "sim_getdirentries", 
        "sim_ioctl", 
        "sim_lseek", 
        "sim_lseek.on_dir", 
        "sim_lseek.on_dir.nz", 
        "sim_open", 
        "sim_read", 
        "sim_select_exceptfd",
        "sim_select_readfd",
        "sim_select_writefd",
        "sim_write", 
        NULL
    };
    DebugCoverageTracker FDCoverage("SimulatedFD", FDCoverageNames, true);
    const char *FcntlCoverageNames[] = {
        "DUPFD",
        "GETFD",
        "SETFD",
        "GETFL",
        "SETFL",
        "GETOWN",
        "SETOWN",
        "GETLK",
        "SETLK",
        "SETLKW",
        "other",
        NULL
    };
    DebugCoverageTracker FcntlCoverage("SimulatedFD::sim_fcntl",
                                       FcntlCoverageNames, true);
    const char *IoctlCoverageNames[] = {
        "TIOCGETP",
        "other",
        NULL
    };
    DebugCoverageTracker IoctlCoverage("SimulatedFD::sim_ioctl",
                                       IoctlCoverageNames, true);
    #define COVERAGE_FD(point) FDCoverage.reached(point)
    #define COVERAGE_FCNTL(point) FcntlCoverage.reached(point)
    #define COVERAGE_IOCTL(point) IoctlCoverage.reached(point)
#else
    #define COVERAGE_FD(point) ((void) 0)
    #define COVERAGE_FCNTL(point) ((void) 0)
    #define COVERAGE_IOCTL(point) ((void) 0)
#endif  // PERFORM_COVERAGE_SIMFD


const bool kReportUnsupportedIoctls = true;
const bool kReportUnsupportedFcntls = true;
const bool kComplainAboutIoctlArgs = false;     // note: not rate-limited


// Used to read the host's "errno" global (to prevent accidental assignment)
int
read_system_errno()
{
    return errno;
}


// Extract static size/in/out info encoded in the ioctl request number
void
decode_alpha_ioctl(u64 alpha_request, u64 *command_ret,
                   int *argp_size_ret, bool *argp_void_ret,
                   bool *argp_in_ret, bool *argp_out_ret)
{
    // Bit allocation of request word:
    //   <63:32>: 0
    //   <31:29>: argp in/out/void flags
    //   <28:16>: argp size, in bytes>
    //   <15:8> : command "group"
    //   <7:0>  : command "number"

    // This is just reversing the _IO _IOR _IOW _IORW macros in
    // OSF/1's /usr/include/sys/ioctl.h

    *command_ret = GET_BITS_IDX(alpha_request, 15, 0);
    *argp_size_ret = GET_BITS_IDX(alpha_request, 28, 16);
    *argp_void_ret = (alpha_request & 0x20000000) ? 1 : 0;      // IOC_VOID
    *argp_in_ret =   (alpha_request & 0x80000000) ? 1 : 0;      // IOC_IN
    *argp_out_ret =  (alpha_request & 0x40000000) ? 1 : 0;      // IOC_OUT
}


}       // Anonymous namespace close



SimulatedFD::SimulatedFD(SyscallState *nascent_parent_sst__,
                         CBQ_Callback *fmt_here_cb__)
    : parent_sst_(nascent_parent_sst__),
      fmt_here_cb_(fmt_here_cb__),
      host_fd_(-1), host_fd_owned_(false), alpha_fd_(-1),
      errno_(0),
      c_DIR_dir_(NULL), dir_desyncd_(false),
      alpha_fd_flags_(0), alpha_file_flags_(0), alpha_async_pid_(0)
{
    // Warning: "parent_sst_" may not be fully constructed yet, so don't use it
}


SimulatedFD::~SimulatedFD()
{
    if (host_fd_owned_ && (host_fd_ >= 0)) {
        close(host_fd_);
        host_fd_ = -1;
    }
    if (c_DIR_dir_) {
        DIR *dir = static_cast<DIR *>(c_DIR_dir_);
        closedir(dir);
        c_DIR_dir_ = NULL;
    }
}


// Internal helper: we were reading an FD as a directory, and then the caller
// did something to disturb us (e.g. write)
void
SimulatedFD::dir_teardown()
{
    dir_desyncd_ = true;
    dir_known_offsets_.clear();
}


// Return some string describing where in the execution stream this syscall
// occurs, to try and help error messages be more specific.
string
SimulatedFD::describe_syscall_loc() const
{
    string result;
    if (fmt_here_cb_) {
        SyscallFmtHereCBArgs cb_args(&result);
        fmt_here_cb_->invoke(&cb_args);         // overwrites "result"
    } else {
        result = "?";
    }    
    return result;
}


void
SimulatedFD::set_alpha_fd(int new_alpha_fd)
{
    sim_assert((alpha_fd_ < 0) && (new_alpha_fd >= 0));
    alpha_fd_ = new_alpha_fd;
}


void
SimulatedFD::open_cfile_read(void *c_FILE_src, const string& filename_or_id)
{
    FILE *src = static_cast<FILE *>(c_FILE_src);
    errno_ = 0;
    // Cruft: mixing "fd" and "FILE *" I/O
    host_fd_ = fileno(src);
    host_fd_owned_ = false;
    host_path_ = filename_or_id;
    if (host_fd_ < 0) {
        host_fd_ = -1;
        errno_ = read_system_errno();
        sim_assert(errno_ != 0);
    }
}


void
SimulatedFD::open_cfile_write(void *c_FILE_dest, const string& filename_or_id)
{
    FILE *dest = static_cast<FILE *>(c_FILE_dest);
    errno_ = 0;
    // Cruft: mixing "fd" and "FILE *" I/O
    host_fd_ = fileno(dest);
    host_fd_owned_ = false;
    host_path_ = filename_or_id;
    if (host_fd_ < 0) {
        host_fd_ = -1;
        errno_ = read_system_errno();
        sim_assert(errno_ != 0);
    }
}


void
SimulatedFD::sim_open(const char *path_xlated, u64 alpha_flags,
                      u64 alpha_mode)
{
    COVERAGE_FD("sim_open");
    sim_assert(!this->is_open());

    int host_flags = 0;
    if (alpha_flags & Alpha_O_WRONLY)
        host_flags |= O_WRONLY;
    if (alpha_flags & Alpha_O_RDWR)
        host_flags |= O_RDWR;
    if (alpha_flags & Alpha_O_APPEND)
        host_flags |= O_APPEND;
    if (alpha_flags & Alpha_O_NONBLOCK)
        host_flags |= O_NONBLOCK;
    if (alpha_flags & Alpha_O_CREAT)
        host_flags |= O_CREAT;
    if (alpha_flags & Alpha_O_TRUNC)
        host_flags |= O_TRUNC;
    if (alpha_flags & Alpha_O_EXCL)
        host_flags |= O_EXCL;
    if (alpha_flags & Alpha_O_NOCTTY)
        host_flags |= O_NOCTTY;

    // Cruftily assume mode bit meanings match
    mode_t host_mode = 0;
    if (host_flags & O_CREAT) {
        host_mode = alpha_mode;
        if (host_mode != alpha_mode) {
            // Value doesn't fit in narrower type, yikes
            // A quick glance at implementations indicates they don't seem to
            // care?             
        }
    }

    errno_ = 0;
    host_fd_ = open(path_xlated, host_flags, host_mode);
    host_fd_owned_ = true;
    host_path_ = path_xlated;

    if (host_fd_ < 0) {
        host_fd_ = -1;
        errno_ = read_system_errno();
        sim_assert(errno_ != 0);
    }

    const i32 open_flags_shared_with_fcntl = 
        (Alpha_O_NONBLOCK | Alpha_O_APPEND);
    // (not sure this is right)
    alpha_file_flags_ = alpha_flags & open_flags_shared_with_fcntl;

    alpha_async_pid_ = 1;       // fiction
}


int
SimulatedFD::sim_close()
{
    COVERAGE_FD("sim_close");
    sim_assert(this->is_open());
    errno_ = 0;
    int close_stat = close(host_fd_);
    host_fd_ = -1;
    if (close_stat < 0) {
        errno_ = read_system_errno();
        sim_assert(errno_ != 0);
    }
    if (doing_dir_io())
        dir_teardown();
    return close_stat;
}


i64
SimulatedFD::sim_read(ProgMem *pmem, mem_addr dst_va, u64 nbytes)
{
    COVERAGE_FD("sim_read");
    sim_assert(this->is_open());

    if (doing_dir_io())
        dir_teardown();

    i64 result;
    errno_ = 0;

    if (!pmem_access_ok(pmem, dst_va, nbytes, PMAF_W)) {
        errno_ = EFAULT;
        result = -1;
    } else {
        vector<char> buffer;
        buffer.resize(nbytes);     // zero-fills
        ssize_t read_stat = read(host_fd_, &buffer[0], nbytes);
        if (read_stat < 0) {
            errno_ = read_system_errno();
            sim_assert(errno_ != 0);
        } else if (read_stat > 0) {
            pmem_write_memcpy(pmem, dst_va, &buffer[0], read_stat, PMAF_W);
        }
        result = read_stat;
    }
    return result;
}


i64
SimulatedFD::sim_write(ProgMem *pmem, mem_addr src_va, u64 nbytes)
{
    COVERAGE_FD("sim_write");
    sim_assert(this->is_open());

    if (doing_dir_io())
        dir_teardown();

    i64 result;
    errno_ = 0;

    if (!pmem_access_ok(pmem, src_va, nbytes, PMAF_R)) {
        errno_ = EFAULT;
        result = -1;
    } else {
        vector<char> buffer;
        buffer.resize(nbytes);     // zero-fills
        pmem_read_memcpy(pmem, &buffer[0], src_va, nbytes, PMAF_R);
//        fprintf(stderr,"Output: %s\n",buffer[0]);
        ssize_t write_stat = write(host_fd_, &buffer[0], nbytes);
//        ssize_t write_stat = write(1, &buffer[0], nbytes);
                if (write_stat < 0) {
            errno_ = read_system_errno();
            sim_assert(errno_ != 0);
        }
        result = write_stat;
    }
    return result;
}


i64
SimulatedFD::sim_lseek(i64 offset, i64 alpha_whence)
{
    COVERAGE_FD("sim_lseek");
    sim_assert(this->is_open());

    errno_ = 0;

    int host_whence;
    switch (alpha_whence) {
    case Alpha_SEEK_SET:
        host_whence = SEEK_SET;
        break;
    case Alpha_SEEK_CUR:
        host_whence = SEEK_CUR;
        break;
    case Alpha_SEEK_END:
        host_whence = SEEK_END;
        break;
    default:
        errno_ = EINVAL;
        host_whence = -1;               // for warnings
    }

    i64 result_offset = -1;
    if (!errno_) {
        off_t lseek_stat = lseek(host_fd_, offset, host_whence);
        if (lseek_stat < 0) {
            errno_ = read_system_errno();
            sim_assert(errno_ != 0);
        }
        result_offset = lseek_stat;
    }

    if (doing_dir_io()) {
        COVERAGE_FD("sim_lseek.on_dir");
        if (result_offset >= 0) {
            DIR *dir_handle = static_cast<DIR *>(c_DIR_dir_);
            if (result_offset > 0) {
                COVERAGE_FD("sim_lseek.on_dir.nz");
                if (dir_known_offsets_.count(result_offset)) {
                    seekdir(dir_handle, result_offset);
                    dir_desyncd_ = false;
                } else {
                    // illegal for this DIR
                    dir_teardown();
                }
            } else if (result_offset == 0) {
                rewinddir(dir_handle);
                dir_desyncd_ = false;
            }
        }
    }

    return result_offset;
}


i64
SimulatedFD::sim_ioctl(ProgMem *pmem, i64 alpha_request,
                       mem_addr arg_va_maybe)
{
    COVERAGE_FD("sim_ioctl");
    sim_assert(this->is_open());

    if (doing_dir_io())
        dir_teardown();

    u64 alpha_cmd;      
    int argp_size;
    bool argp_void, argp_in, argp_out;
    decode_alpha_ioctl(alpha_request, &alpha_cmd, &argp_size,
                       &argp_void, &argp_in, &argp_out);

    if (kComplainAboutIoctlArgs) {
        if (argp_void == (argp_in || argp_out)) {
            err_printf("syscall(%s) sim_ioctl(%s,0x%s,0x%s) argp void/in/out "
                       "flags inconsistent: %d/%d/%d\n",
                       describe_syscall_loc().c_str(),
                       fmt_i64(alpha_fd_),
                       fmt_x64(alpha_request), fmt_x64(arg_va_maybe),
                       argp_void, argp_in, argp_out);
        }
        if (argp_void != (arg_va_maybe == 0)) {
            err_printf("syscall(%s) sim_ioctl(%s,0x%s,0x%s) argp presence "
                       "mismatches IOC_VOID flag (%d).\n",
                       describe_syscall_loc().c_str(),
                       fmt_i64(alpha_fd_), fmt_x64(alpha_request),
                       fmt_x64(arg_va_maybe), argp_void);
        }
        if (argp_void != (argp_size == 0)) {
            err_printf("syscall(%s) sim_ioctl(%s,0x%s,0x%s) argp_size (%d) "
                       "zero-ness mismatches IOC_VOID flag (%d)\n",
                       describe_syscall_loc().c_str(),
                       fmt_i64(alpha_fd_), fmt_x64(alpha_request),
                       fmt_x64(arg_va_maybe), argp_size, argp_void);
        }
    }

#if 0
    // Old code; I don't see TCGETP in any of the Alpha headers, though
    if(as->R[REG_A1].i == 0x40247455) /* TCGETP on OSF */
        as->R[REG_A1].i = 0x40067408; /* TIOCGETP on OSF */
#endif

    errno_ = 0;
    i64 alpha_result;
    int host_request = 0;      // 0: no native request to make

    if (!errno_ && (argp_in || argp_out) && (argp_size > 0)) {
        unsigned access_flags = ((argp_in) ? PMAF_R : 0) |
            ((argp_out) ? PMAF_W : 0);
        if (!pmem_access_ok(pmem, arg_va_maybe, argp_size, access_flags)) {
            errno_ = EFAULT;
            alpha_result = -1;
        }
    }

    switch (alpha_request) {
    case Alpha_TIOCGETP:
    case Alpha_TIOCGETS:
    case Alpha_TIOCGETA:
    case Alpha_TIOCISATTY:
        /* special that makes certain spec benchmarks work */
        COVERAGE_IOCTL("TIOCGETP");
        errno_ = ENOTTY;
        alpha_result = -1;
        break;
    default:
        COVERAGE_IOCTL("other");
        errno_ = EINVAL;        // unrecognized
        alpha_result = -1;
        if (kReportUnsupportedIoctls &&
            !alpha_ioctls_seen_.count(alpha_request)) {
            err_printf("syscall(%s) sim_ioctl(%s,0x%s,0x%s) unsupported\n",
                       describe_syscall_loc().c_str(),
                       fmt_i64(alpha_fd_), fmt_x64(alpha_request),
                       fmt_x64(arg_va_maybe));
        }
    }

    if (!errno_ && (host_request != 0)) {
        // Optional pass-through to host ioctl().  Use with caution.
        COVERAGE_IOCTL("hostreq");
        vector<char> buffer;
        if (argp_size > 0) {
            COVERAGE_IOCTL("hostreq.memread");
            buffer.resize(argp_size);
            pmem_read_memcpy(pmem, &buffer[0], arg_va_maybe, argp_size,
                             PMAF_R);
        }
        int ioctl_stat = ioctl(host_fd_, host_request,
                               (argp_size > 0) ? &buffer[0] : NULL);
        if (ioctl_stat == -1) {
            errno_ = read_system_errno();
            sim_assert(errno_ != 0);
            alpha_result = -1;
        } else {
            switch (host_request) {
            default:
                alpha_result = -1;      // for warnings
                abort_printf("syscall(%s) unhandled ioctl output "
                             "status mapping for host ioctl 0x%s\n",
                             describe_syscall_loc().c_str(),
                             fmt_x64(host_request));
            }
        }

        if (argp_size > 0) {
            COVERAGE_IOCTL("hostreq.memwrite");
            pmem_write_memcpy(pmem, arg_va_maybe, &buffer[0], argp_size,
                              PMAF_W);
        }
    }

    alpha_ioctls_seen_.insert(alpha_request);
    return alpha_result;
}


int
SimulatedFD::sim_fstat(ProgMem *pmem, mem_addr dst_va)
{
    COVERAGE_FD("sim_fstat");
    sim_assert(this->is_open());
    errno_ = 0;

    int result;
    if (!pmem_access_ok(pmem, dst_va, AlphaStructStatBytes, PMAF_W)) {
        errno_ = EFAULT;
        result = -1;
    } else {
        struct stat host_stat_buf;
        int fstat_stat = fstat(host_fd_, &host_stat_buf);
        if (fstat_stat < 0) {
            errno_ = read_system_errno();
            sim_assert(errno_ != 0);
        } else {
            syscalls_store_alpha_stat(parent_sst_, pmem, dst_va,
                                      &host_stat_buf);
        }
        result = fstat_stat;
    }
    return result;
}


i64
SimulatedFD::sim_fcntl(ProgMem *pmem,
                       i64 alpha_request, u64 alpha_arg_maybe)
{
    COVERAGE_FD("sim_fcntl");
    sim_assert(this->is_open());

    // We're not too worried about faithfully implementing these at the
    // moment, since they almost certainly weren't working properly before
    // SimulatedFD: back then, the simulator translated the file descriptor,
    // but then passed the second and third arguments through verbatim
    // (vs. translating flag values, or worrying about the fcntls that use the
    // third argument as a pointer!)

    errno_ = 0;
    i64 alpha_result;

    switch (alpha_request) {
    case Alpha_F_DUPFD:
        COVERAGE_FCNTL("DUPFD");
        errno_ = EINVAL;                // Not supported now
        alpha_result = -1;
        // We can conceivably support this, if needed, by having SyscallState
        // register another descriptor# pointing to this SimulatedFD.
        // (We'd then need reference-counting, which will be nice to avoid
        // if possible.)
        break;
    case Alpha_F_GETFD:
        COVERAGE_FCNTL("GETFD");
        alpha_result = alpha_fd_flags_;
        // assertion per OSF4 fcntl(2)
        sim_assert((alpha_result == 0) || (alpha_result == Alpha_FD_CLOEXEC));
        break;
    case Alpha_F_SETFD:
        COVERAGE_FCNTL("SETFD");
        if (alpha_arg_maybe & ~u64(Alpha_SETFD_allowed_mask)) {
            errno_ = EINVAL;
            alpha_result = -1;
        } else {
            alpha_fd_flags_ = alpha_arg_maybe;
            alpha_result = 0;
        }
        break;
    case Alpha_F_GETFL:
        COVERAGE_FCNTL("GETFL");
        alpha_result = alpha_file_flags_;
        sim_assert(alpha_result >= 0);          // per OSF4 fcntl(2)
        break;
    case Alpha_F_SETFL:
        COVERAGE_FCNTL("SETFL");
        if (alpha_arg_maybe & ~u64(Alpha_SETFL_allowed_mask)) {
            errno_ = EINVAL;
            alpha_result = -1;
        } else {
            alpha_file_flags_ = alpha_arg_maybe;
            alpha_result = 0;
        }
        break;
    case Alpha_F_GETOWN:
        COVERAGE_FCNTL("GETOWN");
        alpha_result = alpha_async_pid_;
        sim_assert(alpha_result != -1);          // per OSF4 fcntl(2)
        break;
    case Alpha_F_SETOWN:
        COVERAGE_FCNTL("SETOWN");
        if (i32(alpha_arg_maybe) == -1) {
            errno_ = EINVAL;
            alpha_result = -1;
        } else {
            alpha_async_pid_ = i32(alpha_arg_maybe);
            alpha_result = 0;
        }
        break;
    case Alpha_F_GETLK:
        COVERAGE_FCNTL("GETLK");
        if (!pmem_access_ok(pmem, alpha_arg_maybe, AlphaStructFlockBytes,
                            PMAF_RW)) {
            errno_ = EFAULT;
            alpha_result = -1;
        } else {
            // fake it, just say there's no lock; set "l_type" to F_UNLCK
            pmem_write_16(pmem, alpha_arg_maybe+0, Alpha_F_UNLCK, 0);
            alpha_result = 0;
        }
        break;
    case Alpha_F_SETLK:
        COVERAGE_FCNTL("SETLK");
        if (!pmem_access_ok(pmem, alpha_arg_maybe, AlphaStructFlockBytes,
                            PMAF_RW)) {
            errno_ = EFAULT;
            alpha_result = -1;
        } else {
            // ignore request and feign success
            alpha_result = 0;
        }
        break;
    case Alpha_F_SETLKW:
        COVERAGE_FCNTL("SETLKW");
        if (!pmem_access_ok(pmem, alpha_arg_maybe, AlphaStructFlockBytes,
                            PMAF_RW)) {
            errno_ = EFAULT;
            alpha_result = -1;
        } else {
            // ignore request and feign success
            alpha_result = 0;
        }
        break;
    default:
        COVERAGE_FCNTL("other");
        errno_ = EINVAL;        // unrecognized, or unsupported by simulator
        alpha_result = -1;
        if (kReportUnsupportedFcntls &&
            !alpha_fcntls_seen_.count(alpha_request)) {
            err_printf("syscall(%s) sim_fcntl(%s,0x%s,0x%s) unsupported\n",
                       describe_syscall_loc().c_str(),
                       fmt_i64(alpha_fd_), fmt_x64(alpha_request),
                       fmt_x64(alpha_arg_maybe));
        }
    }

    sim_assert((alpha_result == -1) == (errno_ != 0));

    // "let's not, and say we did"
    //struct flock host_flock;
    //ioctl_stat = fcntl(native_fd_, host_request, &host_flock);

    alpha_fcntls_seen_.insert(alpha_request);
    return alpha_result;
}


int
SimulatedFD::sim_fsync()
{
    // Strangely, fsync() wasn't implemented at all in smtsim, before
    // SimulatedFD.  I included it in the "implement these" list by mistake.
    // It's simple enough to implement, so we might as well.
    COVERAGE_FD("sim_fsync");
    sim_assert(this->is_open());
    errno_ = 0;
    int sync_stat = fsync(host_fd_);
    if (sync_stat < 0) {
        errno_ = read_system_errno();
        sim_assert(errno_ != 0);
    }
    return sync_stat;
}


int
SimulatedFD::sim_ftruncate(i64 length)
{
    COVERAGE_FD("sim_ftruncate");
    sim_assert(this->is_open());
    if (doing_dir_io())
        dir_teardown();
    errno_ = 0;
    int trunc_stat = ftruncate(host_fd_, length);
    if (trunc_stat < 0) {
        errno_ = read_system_errno();
        sim_assert(errno_ != 0);
    }
    return trunc_stat;
}
    

i64
SimulatedFD::sim_getdirentries(ProgMem *pmem, mem_addr buf_va, i64 buf_bytes,
                               mem_addr basep_va)
{
    COVERAGE_FD("sim_getdirentries");

    i64 buf_written = 0;
    errno_ = 0;

    if (buf_bytes < AlphaDirEntMaxBytes) {
        // buffer too small
        errno_ = EINVAL;
    }

    if (!errno_ && !c_DIR_dir_) {
        // get a DIR* handle to use for I/O
        if (!host_fd_owned_) {
            // we didn't open this, so it's not necessarily safe to do
            // directory I/O on it (and we don't necessarily know the name
            // to give to opendir())
            errno_ = EINVAL;
        } else {
            c_DIR_dir_ = opendir(host_path_.c_str());
            if (!c_DIR_dir_) {
                errno_ = read_system_errno();
                sim_assert(errno_ != 0);
            }
        }
    }

    if (!errno_) {
        sim_assert(doing_dir_io());
        DIR *dir_handle = static_cast<DIR *>(c_DIR_dir_);
        while ((buf_bytes - buf_written) >= AlphaDirEntMaxBytes) {
            struct dirent host_dirent, *dirent_ptr_or_eof;
            int readdir_stat = readdir_r(dir_handle, &host_dirent,
                                         &dirent_ptr_or_eof);
            if (readdir_stat != 0) {
                errno_ = readdir_stat;  // error
            } else if (!dirent_ptr_or_eof) {
                break;                  // EOF
            } else {
                i64 this_write =
                    syscalls_store_alpha_dirent(parent_sst_, pmem,
                                                buf_va + buf_written,
                                                buf_bytes - buf_written,
                                                &host_dirent);
                // write shouldn't fail, since we checked buffer space at
                // the top of the loop
                sim_assert(this_write > 0);
                buf_written += this_write;
            }
        }

        long dir_location = telldir(dir_handle);
        sim_assert(dir_location >= 0);
        dir_known_offsets_.insert(dir_location);
        pmem_write_64(pmem, basep_va, dir_location, 0);
    }

    // ALWAYS >= 0; errno_ is set on any errors
    return buf_written;
}


bool
SimulatedFD::sim_select_readfd() const
{
    COVERAGE_FD("sim_select_readfd");
    sim_assert(this->is_open());
    fd_set host_fdset;
    struct timeval zero_timeout;
    zero_timeout.tv_sec = 0;
    zero_timeout.tv_usec = 0;           // don't stall simulator!
    FD_ZERO(&host_fdset);
    FD_SET(host_fd_, &host_fdset);
    int select_stat = select(host_fd_ + 1, &host_fdset, NULL, NULL,
                             &zero_timeout);
    return (select_stat > 0);
}


bool
SimulatedFD::sim_select_writefd() const
{
    COVERAGE_FD("sim_select_writefd");
    sim_assert(this->is_open());
    fd_set host_fdset;
    struct timeval zero_timeout;
    zero_timeout.tv_sec = 0;
    zero_timeout.tv_usec = 0;
    FD_ZERO(&host_fdset);
    FD_SET(host_fd_, &host_fdset);
    int select_stat = select(host_fd_ + 1, NULL, &host_fdset, NULL,
                             &zero_timeout);
    return (select_stat > 0);
}


bool
SimulatedFD::sim_select_exceptfd() const
{
    COVERAGE_FD("sim_select_exceptfd");
    sim_assert(this->is_open());
    fd_set host_fdset;
    struct timeval zero_timeout;
    zero_timeout.tv_sec = 0;
    zero_timeout.tv_usec = 0;
    FD_ZERO(&host_fdset);
    FD_SET(host_fd_, &host_fdset);
    int select_stat = select(host_fd_ + 1, NULL, NULL, &host_fdset,
                             &zero_timeout);
    return (select_stat > 0);
}
