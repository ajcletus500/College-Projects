//
// Trace Cache (excluding fill unit)
//
// Jeff Brown
// $Id: trace-cache.h,v 1.2.2.3.6.1.8.1 2009/12/25 06:31:52 jbrown Exp $
//

#ifndef TRACE_CACHE_H
#define TRACE_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TraceCache TraceCache;
typedef struct TraceCacheInst TraceCacheInst;
typedef struct TraceCacheBlock TraceCacheBlock;

typedef struct TraceCacheParams TraceCacheParams;
typedef struct TraceCacheStats TraceCacheStats;
typedef struct TraceCacheLookupInfo TraceCacheLookupInfo;


typedef enum {
    // TBF_Br_* is a mutually-exclusive set
    TBF_NotABranch = 0,
    TBF_Br_NotTaken = 0x1,
    TBF_Br_Taken = 0x2,
    TBF_SkipPredict = 0x4,      // Does not consume a prediction
    TBF_MultiTarg = 0x8
} TraceBranchFlags;

#define TBF_UsesPredict(x) ((x) && !((x) & TBF_SkipPredict))

typedef enum {
    // If "start" and "end" are both set, it indicates a one-inst overlap of
    // two contiguous commit groups, not a one-instruction commit group.
    // (Commit groups must therefore have >1 instructions)
    TCGF_StartsGroup = 0x1,
    TCGF_EndsGroup = 0x2
} TraceCGroupFlags;


// These are not meant to model every last bit held by the trace cache,
// only the info needed by the pipeline to simulate these instructions.
// (The idea is to keep the interface to the fetch routines narrow.)
struct TraceCacheInst {
    mem_addr pc;
    mem_addr target_pc;         // (0 for non-branches)
    u32 br_flags;
    u32 cgroup_flags;
};

struct TraceCacheBlock {
    int thread_id;                      // -1 => invalid (application ID)
    int inst_count;                     // entries used in insts, > 0
    TraceCacheInst *insts;              // [inst_count <= block_insts]
    int pred_count;                     // # of predicted branches
    u32 predict_bits;                   // First prediction is at 2^0
    mem_addr fallthrough_pc;
};


struct TraceCacheParams {
    long n_entries;             // Total number of trace entries
    int assoc;                  // Trace associativity (must divide n_entries)
    int block_insts;            // Number of max instructions per block
    int pred_per_block;         // Number of max branch predictions per block
    int is_path_assoc;          // Flag: do path-associative lookups
    int trim_partial_hits;      // Flag: trim partial hits at first mismatch
    
    int inst_bytes;     // Number of bytes per inst (to ignore low PC bits)
};


struct TraceCacheStats {
    // Don't forget to initialize these in TraceCache::reset_stats()
    i64 trace_hits;
    i64 trace_misses;
    i64 fills;
    i64 evicts;
    i64 hit_insts, hit_preds;
    i64 partial_hits;   // Subset of trace_hits
};


struct TraceCacheLookupInfo {
    int thread_id;              // -1: invalid (Application ID)
    mem_addr pc;
    u32 predict_bits;
};


TraceCache *tc_create(const TraceCacheParams *params);

void tc_destroy(TraceCache *tc);

void tc_reset(TraceCache *tc);
void tc_reset_stats(TraceCache *tc);

int tc_lookup(TraceCache *tc, const TraceCacheLookupInfo *lookup_info,
              TraceCacheBlock *block_ret);

// Like tc_lookup(), but doesn't change any state or stats
int tc_probe(const TraceCache *tc, const TraceCacheLookupInfo *lookup_info,
             TraceCacheBlock *block_ret);

void tc_fill(TraceCache *tc, const TraceCacheBlock *new_block);
void tc_inval(TraceCache *tc, const TraceCacheLookupInfo *lookup_info);

void tc_get_stats(const TraceCache *tc, TraceCacheStats *stats_ret);
void tc_get_params(const TraceCache *tc, TraceCacheParams *params_ret);


TraceCacheBlock *tcb_alloc(const TraceCache *tc);
TraceCacheBlock *tcb_copy(const TraceCache *tc, const TraceCacheBlock *tcb);
void tcb_assign(TraceCacheBlock *dst, const TraceCacheBlock *src);
void tcb_free(TraceCacheBlock *tcb);
void tcb_reset(TraceCacheBlock *tcb);
int tcb_compare(const TraceCacheBlock *tcb1, const TraceCacheBlock *tcb2);

// Returns pointer to static buffer
const char *tcinst_format(const TraceCacheInst *tci);

// Returns pointer to static buffer
const char *tcb_format(const TraceCacheBlock *tcb);

// Returns pointer to static buffer
const char *tcli_format(const TraceCacheLookupInfo *tcli);

void tcli_from_tcb(TraceCacheLookupInfo *dest, const TraceCacheBlock *tcb);

void tc_branch_mask(u32 *predict_bits, int pred_count);


#ifdef __cplusplus
}
#endif

#endif  // TRACE_CACHE_H 
