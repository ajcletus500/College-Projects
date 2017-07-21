//
// Hardware thread context
//
// This structure holds everything needed to simulate execution on a particular
// hardware context (thread).
//
// Jeff Brown
// $Id: context.h,v 1.1.2.23.2.6.2.14 2009/11/09 21:42:48 jbrown Exp $
//

#ifndef CONTEXT_H
#define CONTEXT_H

#include "reg-defs.h"
#include "emulate.h"            // For EmuInstState definition

#ifdef __cplusplus
extern "C" {
#endif


// Defined elsewhere
struct CoreResources;
struct activelist;
struct AppState;
struct CallbackQueue;
struct CBQ_Callback;
struct CBQ_Args;


typedef struct acq_entry {
  mem_addr addr;
  i64 timestamp;
  int valid;
  struct activelist *inst;
} acq_entry;


typedef struct ThreadParams ThreadParams;
typedef struct context context;


struct ThreadParams {
    int active_list_size, active_list_size_lg;
    int reorder_buffer_size;
    int retstack_entries;
    int discard_static_noops;
};


typedef enum {          // style of halt requested of context_halt_signal()
    CtxHaltStyle_Full=0,        // Flush ASAP, wait for pipe to drain & reset
    CtxHaltStyle_Fast,          // Flush ASAP, re-use context ASAP
    CtxHaltStyle_AfterDrain,    // Don't flush; block fetch & drain
    CtxHaltStyle_last
} CtxHaltStyle;
extern const char *CtxHaltStyle_names[];

typedef enum {          // internal states used by context-halting code
    CtxHalt_NoHalt=0,
    CtxHalt_FullSignaled,
    CtxHalt_FullFlushed,
    CtxHalt_FullDraining,
    CtxHalt_FastSignaled,
    CtxHalt_FastFlushed,
    CtxHalt_AfterSignaled,
    CtxHalt_AfterDraining,
    CtxHalt_last
} CtxHalt;
extern const char *CtxHalt_names[];

typedef enum {
    LongMem_None=0,
    LongMem_Detecting,
    LongMem_Ignored,
    LongMem_FlushedBlocked,
    LongMem_Completing,
    LongMem_last
} LongMem;
extern const char *LongMem_names[];


struct context {
    ThreadParams params;
    struct CoreResources *core;
    struct AppState *as;        // Application execution state: lots of stuff

    EmuInstState emu_inst;      // Data from emulate_inst()
    int fetching_inst_delay;    // "alist->delay" value of fetching inst

    int id;                     // Hardware context #
    int core_thread_id;         // Offset within CoreResources contexts[]
    mem_addr pc;                // Most recently fetched PC
    int running;
    int sync_lock_blocked;
    acq_entry lock_box_entry;
    int nextsync;
    int follow_sync;                    // Must be 0 or 1
    int sync_restore_point;
    i64 sync_store_value;
    i64 *return_stack;
    int rs_size;
    int rs_start;
    i64 lasthazard[MAXREG];
    i64 fetchcycle;                     // next cyc we can fetch, or MAX_CYC
    i64 last_fetch_begin;               // last cyc we started a fetch
    int stalled_for_prior_fetch;        // flag: waiting for prior fetch op
    struct context *mergethread;
    struct CacheRequest *imiss_cache_entry;
    struct activelist *alist;           // Array of per-dynamic-inst info
    int alisttop;
    int wrong_path;                     // Must be 0 or 1
    int last_writer[MAXREG];
    int misfetching, num_misfetches;
    unsigned ghr;
    int fthiscycle;
    int next_to_commit;
    int draining;
    CtxHalt halting;                    // 0: normal execution
    struct CBQ_Callback *halt_done_cb;  // valid IFF halting != 0
    LongMem long_mem_stat;
    int noop_discard_run_len;           // consecutive noops since non-noop

    // Trace cache block, transferred from last TC hit
    struct {
        int avail;                      // Insts remaining in TC block, or 0
        // Following are invalid iff avail is 0
        struct TraceCacheBlock *block;  // Thread-private copy of "open" block
        mem_addr base_pc;               // Base address of this block
        int predicts_used;              // Predictions used in this block
    } tc;

    struct {
        int fetching_leader;    // Currently fetching group leader, or -1
        int fetching_last_inst; // Flag: fetching inst terminates the group
        i64 started;            // Total commit groups started
        i64 squashed;           // Groups squashed before their 1st inst
        i64 aborted;            // Groups aborted due to spec. violation
        i64 committed;
        int in_flight;
        i64 commit_block_cyc;   // #cycles commit was held waiting for group
    } commit_group;

    struct {
        // These all get memset-zeroed in zero_pipe_stats()
        i64 total_commits;
        i64 total_syscalls;
        i64 oldinstructions;
        u64 instrs, wpinstrs;
        i64 robconf_cyc;
        i64 robsizetotal;
        i64 alistconf_cyc;
        i64 i_mshr_conf;
    } stats;

    int rob_used;
    int rob_freed_this_cyc;
    int lsq_freed;
    int lock_flag;
    u64 lock_physical_address;

    struct activelist *misfetch_discovered;
    struct activelist *lock_failed;
    struct activelist *mispredict_discovered;

    i64 bmt_spillfill_blockready;       // Most recent block ready time
    int bmt_regdirty[MAXREG];

    struct {
        // Status from most recent fetch attempt
        i64 last_startcyc;      // cyc in which we began most recent I-access

        int service_level;      // (from cache-req.h)
        int was_merged;         // flag: was merged onto an earlier request
        i64 latency;            // (MAX_CYC while unknown)
    } icache_sim;

    // Info from the commit of a taken branch/jump, if the most recent
    // committed instruction was in fact a taken branch.  "br_flags" indicates
    // whether this is the case.
    struct {
        int br_flags;
        // Remaining fields valid iff br_flags is nonzero
        mem_addr pc;
        int addr_regnum;        // (only for non-StaticTarget branches)
    } commit_taken_br;
};


// alist_count: return the number of instructions in [first...last], INclusive
// Beware: multiple evaluations
// [x,x] -> 1, [x,x+1] -> 2, [x+1,x] -> alistsize
#define alist_count(ctx, first_id, last_id) \
  ((((last_id) < (first_id)) ? (ctx)->params.active_list_size : 0) + \
   ((last_id) - (first_id)) + 1)

// alist_add: add to/subtract from an activelist number, with wrapping
#define alist_add(ctx, alist_id, offset) \
  (((alist_id) + (offset)) & ((ctx)->params.active_list_size - 1))


context *context_create(const ThreadParams *params, int ctx_id);
void context_destroy(context *ctx);

void context_reset(context *ctx);
void context_reset_resteer(context *ctx);

int context_okay_to_halt(const context * restrict ctx);

// Signal a context to: halt execution soon and invoke a callback afterward.
// Once the context has halted, "halt_done_cb" will be invoked once with a
// NULL parameter, and will then be destroyed upon return.
//
// Note: halting takes some time, and CtxHaltStyle_Fast may be overridden in
// some cases.
void context_halt_cb(context *ctx, CtxHaltStyle halt_style,
                     struct CBQ_Callback *halt_done_cb);

// Backward-compatibility version of context_halt_cb(): this performs the same
// types of halting, but will implicitly call appmgr_signal_idlectx(...) 
// afterward.
void context_halt_signal(context *ctx, CtxHaltStyle halt_style);

int context_ready_to_go(const context * restrict ctx);
void context_go(context *ctx, struct AppState *app, i64 fetch_cyc);

int context_alist_used(const struct context * restrict ctx);
int context_alist_inflight(const struct context * restrict ctx,
                           int app_id);


typedef struct AppStateExtras AppStateExtras;
typedef struct ASE_HitRate ASE_HitRate;

struct ASE_HitRate {
    i64 acc, hits;
};

struct AppStateExtras {
    i64 job_id;                 // -1: not from job/WorkQueue system

    i64 total_commits;
    i64 cp_insts_discarded;     // Correct-path insts discarded (noops)
    i64 fast_forward_dist;      // Insts fast-forwarded at startup
    i64 mem_commits;
    i64 app_inst_last_commit;   // app_inst_num last committed

    // FIXME: This stats will break with SMT. They assume only ONE thread/core
    i64 itlb_acc; 
    i64 dtlb_acc; 
    i64 icache_acc;
    i64 dcache_acc;
    i64 l2cache_acc; 
    i64 l3cache_acc; 
    i64 bpred_acc;
    i64 intalu_acc; 
    i64 fpalu_acc; 
    i64 ldst_acc; 
    i64 lsq_acc; 
    i64 iq_acc; 
    i64 fq_acc; 
    i64 ireg_acc; 
    i64 freg_acc; 
    i64 iren_acc; 
    i64 fren_acc; 
    i64 rob_acc; 
    i64 lsq_occ; 
    i64 iq_occ; 
    i64 fq_occ; 
    i64 ireg_occ; 
    i64 freg_occ; 
    i64 iren_occ; 
    i64 fren_occ; 
    i64 rob_occ; 

    // tmp for bookkeeping
    int lsqsize_this_cyc;
    int iregs_this_cyc;
    int fregs_this_cyc;
    int iqsize_this_cyc;
    int fqsize_this_cyc;

    // END FIXME    
    
    i64 create_time, vacate_time;
    i64 sched_cyc_before_last;  // #cyc scheduled on a context, excluding now
    i64 last_go_time;           // Time of most recent start, -1 if not running
    i64 total_go_count;         // #times this app was started
    i64 commits_at_last_go;     // value of "total_commits" at last start
    int last_go_ctx_id;         // context from most recent go, -1 for none

    i64 long_mem_detected;      // #times this app matched long_mem_op criteria
    i64 long_mem_flushed;       // #times this app flushed for a long_mem_op

    // Per-app portions of stats collected across the simulator
    struct {
        ASE_HitRate dcache, icache, dtlb, itlb, l2cache, l3cache;
        ASE_HitRate bpred, retpred;
    } hitrate;
    i64 mem_accesses;
    struct {
        i64 delay_sum;
        i64 sample_count;
    } mem_delay;
    i64 instq_conf_cyc;

    struct {
        i64 spill_cyc;
        unsigned spill_ghr;
        struct {
            int size, used;
            mem_addr ents[64];          // Hack: static size
        } spill_retstack;
        struct {
            int head;
            int used;
            int size;
            struct {
                mem_addr base_addr;
                i64 ready_time;
            } ents[256];                // Hack: static size
        } spill_dtlb;
    } bmt;

    struct AppStatsLog *stats_log;
    struct CBQ_Callback *stats_log_cb;  // non-null <=> in GlobalEventQueue

    struct {
        // always non-NULL
        struct CallbackQueue *commit_count;     // "time"=total_commits
        struct CallbackQueue *app_inst_commit;  // "time"=app_inst_last_commit
    } watch;
};

AppStateExtras * appextra_create(void);
void appextra_destroy(AppStateExtras *extra);
void appextra_assign_stats(AppStateExtras *out, 
                           const AppStateExtras *in);
void appextra_subtract_stats(AppStateExtras *out, 
                             const AppStateExtras *l,
                             const AppStateExtras *r);

i64 app_sched_cyc(const struct AppState *as);
i64 app_alive_cyc(const struct AppState *as);
void update_acc_occ_per_inst(context * ctx, 
        struct activelist * inst, int add_or_remove, int fp_or_int);

// Commit watchpoint argument object (sorry, C++ only).  The callback is
// triggered when the commit threshold is reached; commit_for_core() first
// NULLs the callback pointer and resets the stored threshold value to
// inactive, then invokes the callback, and the destroys it at return.
#ifdef __cplusplus
    #include "callback-queue.h"     // wanted to avoid; no time to prettify now

    class CommitWatchCBArgs : public CBQ_Args {
    public:
        context *ctx;               // context in which threshold was reached
        int commits_this_cyc;       // #commits in cycle when thresh. reached
        CommitWatchCBArgs(context *ctx_, int commits_this_cyc_)
            : ctx(ctx_), commits_this_cyc(commits_this_cyc_) { }
    };
#endif // __cplusplus

// C wrapper for constructor for commit_count_cb-specific arguments
struct CBQ_Args *
commit_watchpoint_args(context *ctx, int commits_this_cyc);


#ifdef __cplusplus
}
#endif

#endif  // CONTEXT_H
