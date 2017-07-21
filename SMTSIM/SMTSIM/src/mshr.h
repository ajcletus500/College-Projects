// -*- C++ -*-
//
// MSHR model
// (Miss Status Holding/Handling Register)
//
// Jeff Brown
// $Id: mshr.h,v 1.1.2.6 2009/08/10 08:21:28 jbrown Exp $
//

#ifndef MSHR_H
#define MSHR_H

#ifdef __cplusplus
extern "C" {
#endif

// Relevant papers to consider:
//
// Scalable Cache Miss handling for High Memory-Level Parallism; James Tuck,
// Luis Ceze, Josep Torrellas; MICRO 2006
// 
// Complexity/Performance Tradeoffs with Non-Blocking Loads; Keith I Farkas,
// Norman P. Jouppi; WRL Research report 94/3, March 1994

// The terminology is a little different here than in some of the literature;
// we distinguish between "producers" (cache-block sized movements of data
// which will supply data to satisfy misses -- they're what we're waiting for)
// and "consumers" (individual loads/stores/I-fetches which are stalled for
// cache traffic).  Producers can exist without consumers, but not vice-versa,
// and when a producer becomes ready, it typically satisfies all waiting
// consumers.
//
// In literature, a "primary MSHR entry" or just "entry" correponds in this
// model to one producer with one waiting consumer; "secondary entries"
// associated with each primary entry correspond to additional waiters beyond
// the first.  Note that we do not distinguish between reads and writes (or
// deal with coherence permissions) in this model, nor in the entries
// themselves.

// Although this tracks producers and consumers in detail, it is not currently
// used to guide memory requests; it is merely acting as a very fancy counter,
// so the simulator can stall when MSHR resources are exhausted.  For
// historical reasons, the cache simulator (in cache.c) does most of the work
// of matching new requests against existing ones (through the use of the
// heavyweight, searchable CacheQueue) and merging requests for the same cache
// block (through the maintenance of "mergeinst", "mergethread",
// "dependent_coher" pointer chains).


typedef struct MshrTable MshrTable;


typedef enum {
    MSHR_Full,          // MSHR table full (or too busy); cannot continue
    MSHR_AllocNew,      // Access OK; new producer alloc'd, new consumer added
    MSHR_ReuseOld,      // Access OK; re-used procuder, new consumer added
    MshrAllocOutcome_last
} MshrAllocOutcome;
extern const char *MshrAllocOutcome_names[];


// block_bytes: bytes per (cache) block in associated cache
MshrTable *mshr_create(const char *name, const char *config_path,
                       int block_bytes);
void mshr_destroy(MshrTable *mshr);

// Non-modifying probe: can the MSHR table accomodate a new (consumer) access
// to the given address?
int mshr_is_avail(const MshrTable *mshr, LongAddr addr);


// Allocate MSHR resources for a new access to the given address.  (This lacks
// the capability to actually merge requests, to force e.g. write requests to
// not get merged with outstanding read-shared requests, and things like that.
// That stuff's all handled in cache.c)
//
// An MSHR entry for a "producer" (i.e. a cache request) is allocated for the
// enclosing cache block, iff one is not already present.  Then, the MSHR
// entry has an additional "consumer" (i.e. load/store/fetch) entry noted.
// (Note that this doesn't actually identify consumers; it will have to do
// so, if it ever wants to handle more complex stuff.)
//
// The ctx_id and inst_id (if appropriate) are currently used to identify the
// consumer; these are really just used for consistency checking and to help
// debugging, but if the functionality here ever grows, something sort of
// handle to waiting insts/contexts will be needed.
//
// Each successful mshr_alloc_{inst,data} call -- those which return
// MSHR_AllocNew or MSHR_ReuseOld -- needs a corresponding
// mshr_free_consumer() call performed later.  Additionally, each call which
// returns MSHR_AllocNew must also have mshr_free_producer() called, after its
// consumers are all freed.
MshrAllocOutcome
mshr_alloc_inst(MshrTable *mshr, LongAddr addr, int ctx_id);
MshrAllocOutcome
mshr_alloc_data(MshrTable *mshr, LongAddr addr, int ctx_id, int inst_id);

// Allocate an MSHR consumer entry for a request from another cache, for
// an entire cache block.  "requesting_cache_id" is treated opaquely
// by the MSHR itself, its meaning is up to the requesting caches.
// (perhaps a casted CacheSource enum?)
MshrAllocOutcome
mshr_alloc_nestedcache(MshrTable *mshr, LongAddr addr, 
                       int requesting_cache_id);

// Subset of mshr_alloc_{inst,data,etc.}, which stops shorts of allocating
// resources to track a consumer for the memory access.  (Used for hardware
// prefetching.)
MshrAllocOutcome
mshr_alloc_prefetch(MshrTable *mshr, LongAddr base_addr);


// Signal that one "consumer" (mem inst, fetch, etc.) has completed or been
// cancelled.  (This info is arguably flowing the wrong way across the
// interface here, but again, cache.c is doing most of the MSHR's work.)
void mshr_cfree_inst(MshrTable *mshr, LongAddr addr, int ctx_id);
void mshr_cfree_data(MshrTable *mshr, LongAddr addr, int ctx_id, int inst_id);
void mshr_cfree_nestedcache(MshrTable *mshr, LongAddr addr,
                            int requesting_cache_id);

// Signal that the "producer" (cache request/response arc) for the given block
// is complete, and free its entry.  There must not be any consumers still
// waiting.
void mshr_free_producer(MshrTable *mshr, LongAddr base_addr);

// Probe: is there an outstanding producer for the block at "base_addr"?
// (note: "normal" code probably shouldn't have to call this if it's designed
// right; e.g. the fill of an L1 miss MUST imply that there's a producer
// already allocated, so just go ahead and call mshr_free_producer() when
// done.)
int mshr_any_producer(const MshrTable *mshr, LongAddr base_addr);

// Probe: are there any outstanding consumers for the block at "base_addr"?
int mshr_any_consumers(const MshrTable *mshr, LongAddr base_addr);

// Non-modifying probe: how many current producer entries were created for
// prefetches (mshr_alloc_prefetch())?
int mshr_count_prefetch_producers(const MshrTable *mshr);


void mshr_dump(const MshrTable *mshr, void *c_FILE_out, const char *prefix);


#ifdef __cplusplus
}
#endif

#endif  // MSHR_H
