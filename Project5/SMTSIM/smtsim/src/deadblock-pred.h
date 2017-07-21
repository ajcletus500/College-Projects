// -*- C++ -*-
//
// Cache dead-block predictor (and perhaps prefetcher)
//
// Jeff Brown
// $Id: deadblock-pred.h,v 1.1.2.5 2008/11/20 12:17:31 jbrown Exp $
//

#ifndef DEADBLOCK_PRED_H
#define DEADBLOCK_PRED_H

#ifdef __cplusplus
extern "C" {
#endif


// Relevant papers to consider:
//
// Dead-Block Prediction & Dead-Block Correlating Prefetchers; An-Chow Lai,
// Cem Fide, Babak Falsafi; ISCA 2001
//
// Selective, Accurate, and Timely Self-Invalidation Using Last-Touch
// Prediction; An-Chow Lai, Babak Falsafi; ISCA 2000


// Defined elsewhere
struct CoreResources;
//struct CallbackQueue;


typedef struct DeadBlockPred DeadBlockPred;


DeadBlockPred *dbp_create(const char *id, const char *config_path,
                          struct CoreResources *parent_core,
                          int peer_cache_blocks, int block_bytes);
void dbp_destroy(DeadBlockPred *dbp); 

// Notify: the given pc accessed the given address
void dbp_mem_exec(DeadBlockPred *dbp, mem_addr pc, LongAddr va);
void dbp_mem_commit(DeadBlockPred *dbp, mem_addr pc, LongAddr va);

// missing: some way to meaningfully incorporate I-stream fetches and L2 cache
// accesses into a DeadBlockPred.  (For I-stream, perhaps use a per-thread
// signature of taken branches, combined with the PC used to fetch a given
// block, to access the prediction table?)
//
//void dbp_inst_fetch(DeadBlockPred *dbp, LongAddr va, ...);
//void dbp_inst_branch_exec(DeadBlockPred *dbp, mem_addr pc, LongAddr targ_va);
//void dbp_l2_access(DeadBlockPred *dbp, LongAddr va, ...);

// Notify: cache block being inserted into cache
// (Note: insert-after-insert without an intervening kill is not allowed here;
// those can happen with coherence, so be careful.)
void dbp_block_insert(DeadBlockPred *dbp, LongAddr base_va);
// Notify: cache block is being evicted or invalidated
void dbp_block_kill(DeadBlockPred *dbp, LongAddr base_va);

int dbp_predict_dead(const DeadBlockPred *dbp, LongAddr base_va);


// Reporting/debug routines
void dbp_print_stats(const DeadBlockPred *dbp, void *c_FILE_out,
                     const char *prefix);
void dbp_dump(const DeadBlockPred *dbp, void *c_FILE_out, const char *prefix);


#ifdef __cplusplus
}
#endif

#endif  // DEADBLOCK_PRED_H
