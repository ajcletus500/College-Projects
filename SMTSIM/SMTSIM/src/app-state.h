//
// Application execution state
//
// This structure holds everything needed to emulate application instructions:
// architected CPU state, application memory state, runtime/syscall support
// state, etc.  It doesn't contain any microarch. state; no branch prediction,
// cache info, etc.  Nothing in here should tie application execution to any
// particular hardware context; this is the bare minimum for emulation.
//
// Note that AppStates get forcibly steered down wrong paths, etc. during
// simulation.  It's up to whoever breaks it first, to fix it.
//
// Jeff Brown
// $Id: app-state.h,v 1.1.2.11.2.1.2.2 2009/07/29 10:52:49 jbrown Exp $
//

#ifndef APP_STATE_H
#define APP_STATE_H

#include "reg-defs.h"

// Defined elsewhere
struct RegionAlloc;
struct AppStateExtras;
struct AppStatsLog;
struct SyscallState;
struct Stash;


#ifdef __cplusplus
extern "C" {
#endif


typedef struct AppState AppState;
typedef struct AppParams AppParams;


struct AppState {
    // Note: timing and other simulation-related info, doesn't belong in here;
    // see "AppStateExtra" in context.h, for that.

    int app_id;                 // Globally-unique app ID
    int app_master_id;          // App ID of "master" when sharing mem
    mem_addr npc;               // The next instruction
    reg_u R[MAXREG];            // Architected CPU registers (int + FP + other)

    AppParams *params;
    struct Stash *stash;        // Owned by app_master_id; NULL iff vacated
    struct ProgMem *pmem;       // Program memory image; NULL iff "vacated"
    struct SyscallState *syscall_state;

    struct {
        // This stuff comes from the executable header, and is filled in by the
        // loader.  These values don't change during execution, and you
        // probably shouldn't use them at all since (ideally) they'd 
        // disappear into SyscallState?  (What about sharing mem.)
        mem_addr bss_start;             // Uninitialized data (variable size)
        mem_addr stack_upper_lim;       // Upper limit of stack (doesn't move)
        mem_addr stack_init_top;        // Top of stack before program entry
        mem_addr entry_point;           // First PC in program execution
        mem_addr gp_value;              // Initial GP value
    } seg_info;

    struct {
        // Warning: don't use these "total_insts" for performance calculations
        i64 total_insts;                // Includes NOPs, decreases for "undo"
        i64 total_syscalls;
    } stats;

    struct {
        int has_exit;
        i64 exit_code;
    } exit;

    // This provides a convenient link to per-app info which can be useful for
    // simulation, but which doesn't strictly belong here.  None of the
    // emulation code touches it.
    struct AppStateExtras *extra;
};


struct AppParams {
    char *bin_filename;                 // malloc'd
    char *initial_working_dir;          // malloc'd
    int argc;
    char **argv;                        // malloc'd array of malloc'd strings
    char **env;                         // like argv
    void *FILE_in, *FILE_out, *FILE_err;
};


AppParams *app_params_create(void);     // creates a dummy value to build from
AppParams *app_params_copy(const AppParams *ap);
void app_params_destroy(AppParams *ap);

// De-init an AppState, freeing any held memory.
void appstate_destroy(AppState *as);

// Mostly destroy an AppState and free its memory, but keep the top-level
// AppState object alive and linked into the global pool of AppStates, so that
// it can still be referred to for statistics reporting.
// (Don't forget to update AppStateExtras with the vacate time!)
void appstate_vacate(AppState *as);
int appstate_is_alive(const AppState *as);      // alive <=> not vacated

// NULL: not found
AppState *appstate_lookup_id(int app_id);

// NULL: failure
AppState *appstate_new_fromfile(const AppParams *params,
                                struct RegionAlloc *r_alloc);

//int appstate_fork(AppState *as, AppState *parent);
//int appstate_join(AppState *as, AppState *parent);


int appstate_count(void);
void appstate_global_iter_reset(void);
AppState *appstate_global_iter_next(void);


void appstate_dump_regs(const struct AppState *as, void *FILE_out,
                        const char *prefix);
void appstate_dump_mem(const struct AppState *as, void *FILE_out, 
                       mem_addr start, i64 len, const char *prefix);


#ifdef __cplusplus
}
#endif

#endif  // APP_STATE_H
