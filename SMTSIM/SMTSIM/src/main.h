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

#ifndef MAIN_H
#define MAIN_H


// Defined elsewhere
struct CoreResources;
struct activelist;
struct context;
struct StashData;
struct TraceCacheInst;


#include "sys-types.h"
#include "utils.h"


#ifdef __cplusplus
extern "C" {
#endif

#define PRIO_INST_COUNT

#ifdef DEBUG
  #define SmtDISASSEMBLE(x, x2, y, z) if (!debug) { } else print(x, x2, y, z);
#else
  #define SmtDISASSEMBLE(x, x2, y, z)   ;
#endif

#define NONE            -1
#define MAX_CYC         I64_MAX


#define CHECKPOINT_CP_INSTS     1
#define CHECKPOINT_STORES       1


// These should move into per-core stats structures, along with some
// per-source-file "static" stats variables
    // queue.c
    extern i64 wpexec;
    // exec.c
    extern i64 flushed, execflushed;
    // fetch.c
    extern i64 misfetchtotal;

/*commit.c*/
extern void fix_regs(void);
void long_mem_detect(struct context * restrict ctx,
                     struct activelist * restrict inst, int at_commit);
extern void commit(void);
extern void process_tcfill_queues(void);
extern void reap_squashed_insts(struct context * restrict ctx);
extern i64 DebugCommitNum;
extern void *FILE_DumpCommitFile;
/*decode.c*/
void decode(void);
/*execute.c*/
extern void initsched(void);
extern void fix_pcs(void);
extern void synchexecute(struct activelist *);
extern void execute(void);
void cleanup_commit_group(struct context *ctx, int misspec_leader_id);
void commit_group_printstats(void);
u64 recover_old_regval(const struct context *ctx, int inst_id, int reg_num,
                       int after_not_before);

/*fetch.c*/
struct activelist *handle_src_dep(struct context * restrict ctx,
                                  struct activelist * restrict inst,
                                  int src_reg);
void handle_commit_group_fetch(struct context * restrict ctx, 
                               struct activelist *inst,
                               const struct TraceCacheInst *tc_inst);
void emulate_inst_for_sim(struct context * restrict ctx, 
        const struct StashData * restrict st, int inst_id);
extern void resim_branch(struct context * restrict ctx, 
                         struct activelist * restrict inst);
extern void calculate_priority(void);
extern void fetch(void);

/* main.c */
extern void time_stats(void);
extern void dump_memmap(void);
extern int warmup;
extern i64 warmuptime;
extern struct JTimer *OverallTimer, *SimTimer;
extern struct RegionAlloc *SegAlloc;
extern int CtxCount;
extern int CoreCount;
extern struct RegionAlloc *GlobalAlloc;
extern struct AppMgr *GlobalAppMgr;
extern struct CallbackQueue *GlobalEventQueue;
extern struct WorkQueue *GlobalWorkQueue;
extern void *FILE_DevNullIn, *FILE_DevNullOut;
extern const char *InitialWorkingDir;   // absolute path at startup
const char *fmt_now(void);      // shortcut, format current simulation time
extern int SignalHandlerActive;         // flag: signal handler running now?
#ifdef DEBUG
    extern int debugsync;       // in main.c
#else
    #define debugsync 0
#endif

/*predict.c*/
extern void init_pht(void);
extern void init_btb(void);
extern int get_bpredict(struct context *, u64, int);
extern int bpredict(struct context *, u64, int, int);
extern void update_pht(struct context *, u64, int, int);
extern u64 btblookup(struct context *, u64, u64, int, int);
extern u64 get_btblookup(struct context *, u64);
extern void predict_stats(void);
extern void zero_pstats(void);
extern void rs_push(struct context *, u64);
extern u64 rs_pop(struct context *, u64);
extern u64 wp_rs_pop(struct context *);
/*print.c*/
extern void print(int, int, u64, unsigned);
/*queue.c*/
extern void schedstats(void);
extern void finalstats(void);
extern void zero_pipe_stats(void);
extern void reset_stats(void);
extern void update_writers(struct activelist * restrict inst);
extern int release_waiters(struct activelist * restrict inst);
extern void remove_waiter(struct activelist * restrict writer,
                          const struct activelist * restrict waiter);
extern void synch_mem_resolve(struct CoreResources * restrict core,
                              struct activelist *, i64);
extern void mem_resolve(struct CoreResources * restrict core,
                        struct activelist *, i64);
extern void queue(void);
/*regread.c*/
extern void regread(void);
/*regrename.c*/
void regrename(void);
/*regwrite.c*/
extern void regwrite(void);
/* run.c */
extern int run(void);
void print_sim_stats(int final_stats);
void sim_exit_ok(const char *short_msg);
extern i64 cyc, warmupcyc, allinstructions;
extern struct LongMemLogger *GlobalLongMemLogger;
extern struct DebugCoverageTracker *EmulateDebugCoverage,
    *FltiRoundDebugCoverage, *FltiTrapDebugCoverage;

/* print.c */
extern void print(int, int, u64, unsigned int);
/*signals.c*/
/*stack.c*/
extern void init_stack_space(void);
extern mem_addr stack_allocate_contiguous(struct context *, u64);
extern void stack_free(mem_addr);
/*stash.c*/
extern void init_stash(void);
extern void print_stash_stats(void);

/* sim-params.c */
extern struct CoreResources **Cores;                    // [CoreCount]
extern struct context **Contexts;                       // [CtxCount]


#ifdef __cplusplus
}
#endif

#endif  /* MAIN_H */

