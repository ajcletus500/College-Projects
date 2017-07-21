//
// Cache (N-way-associative array with replacement) object
//
// Jeff Brown
// $Id: cache-array.h,v 1.13.10.7.2.3.2.11 2009/08/21 21:00:51 jbrown Exp $
//

#ifndef CACHE_ARRAY_H
#define CACHE_ARRAY_H

#include "cache-params.h"


#ifdef __cplusplus
extern "C" {
#endif

struct CoherenceMgr;
struct CoreResources;


typedef struct CacheEvicted CacheEvicted;
typedef struct CacheStats CacheStats;
typedef struct CacheBankStats CacheBankStats;
typedef struct CacheArray CacheArray;

typedef enum { Cache_Read, Cache_ReadExcl,
               Cache_Upgrade,   // Like Cache_ReadExcl, but already has data
               Cache_Write,
               CacheAccessType_last } CacheAccessType;
extern const char *CacheAccessType_names[];
typedef enum { Cache_Hit, Cache_Miss, Cache_UpgradeMiss, Cache_CoherBusy,
               CacheLOutcome_last } CacheLOutcome;
extern const char *CacheLOutcome_names[];
typedef enum { CacheFill_NoEvict, CacheFill_EvictClean, CacheFill_EvictDirty,
               CacheFillOutcome_last } CacheFillOutcome;
extern const char *CacheFillOutcome_names[];


typedef enum { CacheBank_LookupR, CacheBank_LookupW, CacheBank_LookupREx,
               CacheBank_LookupUpgrade,
               CacheBank_Fill, CacheBank_FillCont,
               CacheBank_WB,            // Write-back TO this cache
               CacheBank_CoherSync,     // Sync all ports (no additional op)
               // WB from this cache, maybe invalidate, sync all cache ports
               CacheBank_CoherPull,
               CacheBankOp_last
} CacheBankOp;


// Stats common to cache-like structures
struct CacheStats {
    // Collected at lookup time
    i64 lookups;                        // (sum of hits+misses+...)
    i64 hits, misses, upgrade_misses, coher_busy;   // mut.ex.
    i64 coher_misses;                   // SUBSET of "misses" field
    i64 reads, reads_ex, upgrades, writes;      // mut.ex.

    // Collected as tags are updated
    i64 dirty_evicts;

    // Collected at coher_yield()
    i64 coher_writebacks, coher_invalidates;

    // Collected by calls to cache_log_wbfull_conflict()
    i64 wbfull_confs;
};

// Per-bank usage stats
struct CacheBankStats {
    // Collected as timing is updated
    i64 lookups_r, lookups_rex, lookups_upgrade, lookups_w;
    i64 fills, fillconts, wbs;
    i64 coher_syncs, coher_pulls;
    double util;
};


// Evicted cache block info
struct CacheEvicted {
    LongAddr base_addr;
};


CacheArray *cache_create(int cache_id, const CacheGeometry *geom,
                         const CacheTiming *timing,
                         struct CoherenceMgr *cm,
                         struct CoreResources *parent_core, i64 now);
void cache_destroy(CacheArray *cache);

void cache_reset(CacheArray *cache, i64 now);
void cache_reset_stats(CacheArray *cache, i64 now);


// Perform a cache lookup on the given address/thread.  This does NOT update
// the per-bank statistics or timing.  You should call cache_update_bank() 
// as well, ALWAYS.  This updates replacement info, possibly dirty bits, etc.
//
// Returns: CacheLOutcome indicating miss / hit.
// Iff first_access_ret is non-NULL, a flag is written
// indicating whether this access was the first post-fill access to the
// given block.
CacheLOutcome
cache_lookup(CacheArray *cache, LongAddr addr,
             CacheAccessType access_type, int *first_access_ret);


// Peek to see if the cache contains the given block, with sufficient
// permission to allow "access_type".  No changes are made.
int cache_access_ok(const CacheArray *cache, LongAddr addr,
                    CacheAccessType access_type);

// If the block is present in the cache, "touch" it, updating replacement info
// (e.g. LRU).  Does not update other stats or timing, does not perform any
// replacement.  Returns true iff the block is present.
int cache_touch(CacheArray *cache, LongAddr addr);


// Test: is outbound WB buffer full?
int cache_wb_buffer_full(const CacheArray *cache);
// Log: fill/coher/etc. blocked due to full WB buffer (#ops times #cycles)
void cache_log_wbfull_conflict(CacheArray *cache);


// Insert the given block into the cache, in satisfaction of a request of type
// "access_type".  The block must not already be in the cache, with the
// exception of serving an upgrade miss (for blocks which are present but not
// writeable, and an access_type of Cache_ReadExcl or Cache_Write).  If
// necessary, a cache block is selected for eviction and replaced.
//
// The write-back buffer must not already be full. (Even if no writeback will
// eventually be needed, storage for a victim must be ready before replacement
// is attempted.)
//
// Outcomes:
// - If no block needed eviction, CacheFill_NoEvict is returned.
// - If the replaced block is valid and clean, its info is written to
//   "evicted_ret", and CacheFill_EvictClean is returned.
// - If the replaced block is valid and dirty, its info is written to
//   "evicted_ret", a writeback buffer entry is allocated, and
//   CacheFill_EvictDirty is returned.
CacheFillOutcome
cache_fill(CacheArray *cache, LongAddr addr,
           CacheAccessType access_type, CacheEvicted *evicted_ret);

// Process an inbound writeback on the given block.  The cache's writeback
// buffer must not be full.  If the block is in the cache, nonzero is
// returned, and the block is marked dirty.  If the block is not in the cache,
// zero is returned, and a writeback buffer is allocated.
int cache_writeback(CacheArray *cache, LongAddr addr);

// mark_dirty: block must be present and writeable
void cache_mark_dirty(CacheArray *cache, LongAddr addr);
// mark_writeable: block must be present (be careful with this)
void cache_mark_writeable(CacheArray *cache, LongAddr addr);

// Note that one outbound writeback (or coherence-related WBI reply) for the
// given address has been accepted for delivery.  Returns nonzero iff the
// writeback buffer was previously full.  This is a hack to get at least
// *some* flow control, before we add a proper interconnect model.
int cache_wb_accepted(CacheArray *cache, LongAddr base_addr);


// Disable future hits on a present block, in preparation for a
// cache_coher_yield().  The "lock-out" will persist until the block is either
// evicted or a yield call is made.  While locked out, any lookup attempts for
// this block will return Cache_CoherBusy, but cache_access_ok() will be
// unaffected.  (If the given block is totally absent from the cache, this is a
// no-op.)
void cache_coher_lockout(CacheArray *cache, LongAddr base_addr);

// Force the cache to "yield" the given block -- that is, give up any
// exclusive claim to it -- for coherence purposes.  Specifically, this
// forces a writeback if the block is dirty, and then optionally invalidates
// it.
//
// The writeback buffer must not already be full.  (If bypass_wb_alloc is
// set, writeback buffer checks and allocation are skipped.)
//
// We re-use CacheFillOutcome values here, with slightly different
// semantics:
// - A writeback buffer entry is always allocated.
// - If the given block is absent, CacheFill_NoEvict is returned.
// - If the given block is present either CacheFill_EvictClean or
//   CacheFill_EvictDirty is returned.
CacheFillOutcome
cache_coher_yield(CacheArray *cache, LongAddr base_addr, int invalidate,
                  int bypass_wb_alloc);

// Update the cache-bank accounting for the given operation on the given
// address.  This returns the time at which the operation will have completed.
// If the resource is already occupied, the operation will not begin
// immediately.
//
// Do this after each lookup, fill, writeback, or yield.
i64 cache_update_bank(CacheArray *cache, LongAddr addr, i64 now,
                      CacheBankOp bank_op);

void cache_get_stats(const CacheArray *cache, CacheStats *dest);
void cache_get_bankstats(const CacheArray *cache, i64 now, int bank,
                         CacheBankStats *dest);

void cache_align_addr(const CacheArray *cache, LongAddr *addr);

// Probe whether the cache has the necessary bank/port resources available to
// service a request for the given address.  
int cache_probebank_avail(const CacheArray *cache, LongAddr addr,
                          i64 now, int for_write);

int cache_get_population(const CacheArray *cache, int master_id);

// Returns a malloc'd array of all tags matching thread ID master_id, or all
// tags if master_id==-1.  n_tags_ret must be non-NULL; the number of of
// elements in the returned array is written there.  (If no matches are
// found, *n_tags_ret will be set to 0, and NULL returned.)
//
// This is expensive.
LongAddr *cache_get_tags(const CacheArray *cache, int master_id,
                         int *n_tags_ret);

int cache_get_id(const CacheArray *cache);
const CacheGeometry *cache_get_geom(const CacheArray *cache,
                                    int *n_lines_ret, int *n_blocks_ret);


#ifdef __cplusplus
}
#endif

#endif  // CACHE_ARRAY_H
