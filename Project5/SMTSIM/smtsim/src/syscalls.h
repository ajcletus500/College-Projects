//
// System call emulation
//
// Jeff Brown
// $Id: syscalls.h,v 1.9.6.1.12.3 2009/11/26 18:07:13 jbrown Exp $
//

#ifndef SYSCALLS_H
#define SYSCALLS_H

#ifdef __cplusplus
extern "C" {
#endif

// Defined elsewhere
struct AppState;


typedef struct SyscallState SyscallState;
typedef struct SyscallStateParams SyscallStateParams;


struct SyscallStateParams {
    void *FILE_stdin, *FILE_stdout, *FILE_stderr;
    const char *initial_working_dir;    // non-NULL; will be copied
};


SyscallState *syscalls_create(const SyscallStateParams *params,
                              struct AppState *nascent_astate);
void syscalls_destroy(SyscallState *sst);

int syscalls_dosyscall(struct AppState *astate, i64 local_clock);

extern int SysTrace;


#ifdef __cplusplus
}
#endif

#endif  // SYSCALLS_H
