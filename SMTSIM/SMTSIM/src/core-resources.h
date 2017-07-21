/*
 * Resources associated with each execution core
 *
 * Jeff Brown
 * $Id: core-resources.h,v 1.12.10.13.2.2.2.19.6.1 2009/12/25 06:31:49 jbrown Exp $
 */

#ifndef CORE_RESOURCES_H
#define CORE_RESOURCES_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stage-queue.h"
#include "cache-params.h"
#include "trace-cache.h"
#include "trace-fill-unit.h"
#include "multi-bpredict.h"
#include "stash.h"


/* Defined elsewhere */
struct CacheArray;
struct PFStreamGroup;
struct DeadBlockPred;
struct CoherenceMgr;
struct TLBArray;
struct BTBArray;
struct PHTPredict;
struct context;
struct CoherenceMgr;
struct TraceCache;
struct TraceFillUnit;
struct BranchBiasTable;
struct MshrTable;


typedef struct CoreParams CoreParams;
typedef struct CoreBus CoreBus;
typedef struct CoreResources CoreResources;


typedef struct CoreBusStats {
    i64 xfers;
    i64 syncs;
    i64 idle_cyc;       // bus available but no op started (excludes sync_cyc)
    i64 sync_cyc;       // available time skipped for sync
    i64 useful_cyc;
    double util;
} CoreBusStats;


typedef struct CoreCacheParams {
    int cache_id;
    CacheGeometry *geom;        // Dynamically allocated
    CacheTiming timing;
    int prefetch_nextblock;     // flag
} CoreCacheParams;


typedef struct CoreExecTiming {
    int delay[StashDelayClass_count];
} CoreExecTiming; 


struct CoreParams {
    // Warning: this is used in screwy ways, be sure to update these for any
    // dynamic members: coreparams_copy(), coreparams_destroy(),
    // core_destroy(), read_core_params()

    char *config_path;    // to core-specific, fully-populated tree (malloc'd)

    int inst_bytes;
    int page_bytes;

    int itlb_entries;
    int dtlb_entries;
    int tlb_miss_penalty;
    int tlb_filter_invalid;

    int btb_entries;
    int btb_assoc;
    int pht_entries;
    int br_bias_entries;
    int loadstore_queue_size;

    CoreExecTiming exec_timing;
    CoreCacheParams icache;
    CoreCacheParams dcache;
    CoreCacheParams private_l2cache;
    TraceCacheParams tcache;
    TraceFillUnitParams tfill;
    MultiBPredictParams multi_bp;

    struct {
        int single_limit;
        int total_limit;
        int thread_count_limit;
        int n_stages;
        int enable_trace_cache;         // Flag
        int tcache_skips_to_rename;     // Flag
    } fetch;

    struct {
        int n_stages;
    } decode;

    struct {
        int int_rename_regs;
        int float_rename_regs;
        int n_stages;
    } rename;

    struct {
        int n_stages;
    } regread;

    struct {
        int int_queue_size;
        int float_queue_size;
        int int_ooo_issue;
        int float_ooo_issue;
        int max_int_issue;
        int max_float_issue;
        int max_ldst_issue;
        int max_sync_issue;
    } queue;

    struct {
        int n_stages;
    } regwrite;

    struct {
        int single_limit;
        int total_limit;
        int thread_count_limit;
    } commit;

    // {request,reply}_bus are the (lame) core interconnect; it's at the level
    // of L1<->L2 for shared L2s, L2<->{L3 or memory} for private L2s
    CoreBus *request_bus;
    CoreBus *reply_bus;                 // may be alias of request_bus
    struct CoherenceMgr *coher_mgr;     // NULL when not in use
    struct CacheArray *shared_l2cache;  // NULL when "private_l2caches" set
    struct CacheArray *shared_l3cache;
};


struct CoreResources {
    int core_id;
    CoreParams params;          // crufty value-copy with some dynamic members

    int n_contexts;
    struct context **contexts;

    struct {
        int dyn_stages;         // Total # of dynamically-sized stages
        int decode1;            // Index of first decode stage
        int rename1;            // Index of first rename stage
        int rread1;             // Index of first regread stage
        int rwrite1;            // Index of first regwrite stage
        StageQueue *s;          // Dynamically-created stages
        StageQueue intq;
        StageQueue floatq;
        StageQueue exec;

        StageQueue rename_inject;
    } stage;

    // true iff rename_inject won arbitration for rename1 in last decode() call
    int rename_inject_won_last;

    struct {
        int priorityslot;
        int *priority;  /* Thread indices within this core */
        int *key;
    } sched;

    // These register counts are for "renaming" registers; 
    // physical_regs = rename_regs + (contexts * arch_regs)
    int i_registers_used, f_registers_used;
    int i_registers_freed, f_registers_freed;
    i64 rs_hits, rs_misses;
  
    int lsq_used;
    int lsq_freed;
    
    struct {
        i64 fetch_epoch;
        i64 current_epoch;
    } mb, wmb;
    
    struct {
        // These all get memset-zeroed in zero_pipe_stats()

        // Originally from regrename.c
        i64 iqconf_cyc, fqconf_cyc;
        i64 iqsizetotal, fqsizetotal;
        i64 lsqconf_cyc;
        i64 lsqsizetotal;
        i64 iregconf_cyc, fregconf_cyc;
        i64 iregsizetotal, fregsizetotal;

        // Originally from queue.c
        i64 intfuconf, fpfuconf, ldstfuconf, d_mshr_conf;
        i64 ialuissuetotal, faluissuetotal, ldstissuetotal;
        i64 mb_conf, wmb_conf;

        i64 memconf;

        // Total non-issued, ready insts (when using OOO issue)
        i64 total_conf;

        // Index N: number of cycles where the number of non-issued, ready
        // insts was in [2^(N+1), 2^(N+2)), except for the last entry which
        // covers [2^(N+1),inf).
        i64 totalconf_lg_cyc[16];
    } q_stats;

    // "Owned" structures
    struct MshrTable *inst_mshr;
    struct MshrTable *data_mshr;
    struct MshrTable *private_l2mshr;   // may be NULL
    struct CacheArray *icache;
    struct CacheArray *dcache;
    struct PFStreamGroup *d_streambuf;  // may be NULL
    struct TLBArray *itlb;
    struct TLBArray *dtlb;
    struct BTBArray *btb;
    struct PHTPredict *pht;
    struct TraceCache *tcache;
    struct BranchBiasTable *br_bias;
    struct TraceFillUnit *tfill;
    struct MultiBPredict *multi_bp;
    struct DeadBlockPred *i_dbp;        // may be NULL
    struct DeadBlockPred *d_dbp;        // may be NULL

    // Links to possibly-shared structures
    CoreBus *request_bus;               // Uninspired interconnect model
    CoreBus *reply_bus;                 //   (it's pretty lame)
    struct CacheArray *l2cache;         // "owned" iff using private L2s
    struct CacheArray *l3cache;
    struct DeadBlockPred *l2_dbp;       // "owned" iff private; may be NULL
    struct DeadBlockPred *l3_dbp;       // may be NULL

    struct {
        i64 calls;              // #calls to cachesim_oracle_inject_core()
        i64 gave_up;            // subset of "calls" completely given up on
        i64 cache_inj;          // # individual cache fills desired
        i64 cache_wb_full;      // subset of "cache_inj" skipped for full WBuf
    } cache_inject_stats;
    struct {
        i64 calls;              // #calls to cachesim_oracle_discard_block()
        i64 gave_up;            // subset of "calls" completely given up on
        i64 cache_matches;      // # individual cache blocks discarded
    } cache_discard_stats;
    i64 private_l2mshr_confs;   // #cycles an L2 access stalled for an MSHR

    // the last_store_hash[] array is indexed with a hash of virtual address
    // bits (a simple bit-subset)
    struct {
        // "cyc" used to be the "mem_conflicts[]" array, thread_id_mask used
        // to be the "mem_conf_thds[]" array.  (jab: I didn't come up with
        // this mechanism, I just renamed & documented it)
        i64 cyc;                // last time a store hashed to this bucket
        u32 thread_id_mask;     // threads that stored in "cyc"
        // warning: bit-width of thread_id_mask is known to mem_conflict_*()
    } last_store_hash[4096];    // queue.c, mem_conflict_*() expect power-of-2
};


// Note: setup GlobalParams structure before calling these
CoreResources *core_create(int core_id, const CoreParams *params);
void core_destroy(CoreResources *core);
CoreParams *coreparams_copy(const CoreParams *params);
void coreparams_destroy(CoreParams *params);

void core_add_context(CoreResources *core, struct context *ctx);

void core_dump_queue(const StageQueue *stage);
void core_dump_queues(const CoreResources *core);


CoreBus *corebus_create(void);
void corebus_destroy(CoreBus *bus);
void corebus_reset(CoreBus *bus);
void corebus_get_stats(const CoreBus *bus, CoreBusStats *stats_ret);
// Returns request-done time
i64 corebus_access(CoreBus *bus, OpTime op_time);
// Wait for outstanding access (to force ordering for coherence ops)
// (this is a dumb way to get exclusion, in retrospect)
i64 corebus_sync_prepare(CoreBus *bus);
// Be careful with this one, its answer is not a promise
int corebus_probe_avail(const CoreBus *bus, i64 test_time);


#ifdef __cplusplus
}
#endif

#endif  /* CORE_RESOURCES_H */
