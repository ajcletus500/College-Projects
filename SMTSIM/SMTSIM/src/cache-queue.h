/*
 * Cache Request Queue
 *
 * Jeff Brown
 * $Id: cache-queue.h,v 1.3.10.1.6.2.2.7 2008/10/28 22:57:57 jbrown Exp $
 */

#ifndef CACHE_QUEUE_H
#define CACHE_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

struct CacheRequest;


typedef struct CacheQueue CacheQueue;


CacheQueue *cacheq_create(void);
void cacheq_destroy(CacheQueue *cq);

/* Test if the queue is empty */
int cacheq_empty(const CacheQueue *cq);

/*
 * Place a request in the queue.  If the "blocked" flag is set, the request
 * will never be scheduled; it will still match cacheq_find().
 */
void cacheq_enqueue(CacheQueue *cq, struct CacheRequest *creq);

/*
 * Dequeue the earliest request if it is ready by "now".  If the earliest
 * non-blocked request's time has not yet arrived, or if the queue is empty,
 * returns NULL.
 */
struct CacheRequest *
cacheq_dequeue_ready(CacheQueue *cq, i64 now);

// Remove a given request from the cache queue.  It must be present.
void cacheq_dequeue(CacheQueue *cq, struct CacheRequest *creq);

// Remove a given request from the cache queue; it must be "blocked".
void cacheq_dequeue_blocked(CacheQueue *cq, struct CacheRequest *creq);


// Locate an outstanding memory request, by address and type; returns NULL if
// none are relevant.  If "core_id" is CACHEQ_SHARED, the search scope is
// off-core shared structures; negative core_ids are not otherwise valid.  If
// core_id is nonnegative, cache resources for the given core are searched.
// "sense" determines which types of requests will match.
//
// The current construction is such that at most one matching request may
// exist for any <sense,core,addr> tuple, where "sense" is Miss or Coher.
// Requests which may generate multiple results (CQFS_WB, CACHEQ_ALL, etc.)
// may not be used with cacheq_find(); use cacheq_find_multi(), instead.
//
// (WB requests are modeled pretty lazily in cache.c; there's no explicit
// WB->Miss ordering, so multiple outstanding WBs are possible.  WB->Coher
// ordering is achieved coarsely through cache "port synchronization".)

// warning: minimum_addr_key() relies on CACHEQ_ value order
#define CACHEQ_SHARED -1
#define CACHEQ_ALL_CORES -2     // matches all cores, plus CACHEQ_SHARED

// warning: minimum_addr_key() relies on CQFS_ value order
typedef enum { 
    CQFS_Miss,          // Serving some sort of miss (0,1 only)
    CQFS_WB,            // Outbound writeback (currently not unique)
    CQFS_Coher,         // Invalidate request/response (0,1 only)
    CQFS_All,
    CacheQFindSense_last
} CacheQFindSense;
extern const char *CacheQFindSense_names[];

struct CacheRequest *
cacheq_find(CacheQueue *cq, LongAddr base_addr, int core_id, 
            CacheQFindSense sense);


// Find cache requests; is like cacheq_find(), but this can return
// multiple requests.  Returns an array of pointers to matching requests,
// terminated with a NULL pointer.  The return value is malloc()d, and must be
// free()d by the caller.
//
// This is more expensive than cacheq_find(), particularly when wildcards
// are used.
struct CacheRequest **
cacheq_find_multi(CacheQueue *cq, LongAddr base_addr, int core_id, 
                  CacheQFindSense sense);


/*
 * Each CacheQueue has one iterator.  cacheq_iter_reset() resets it.
 * cacheq_iter_next() returns a different queue entry each time it is called,
 * or NULL when all entries are exhausted.
 *
 * Do not modify the queue while iterating over it.
 */
void cacheq_iter_reset(CacheQueue *cq);
struct CacheRequest *cacheq_iter_next(CacheQueue *cq);


#ifdef __cplusplus
}
#endif

#endif  /* CACHE_QUEUE_H */
