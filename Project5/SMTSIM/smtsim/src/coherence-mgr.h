/*
 * Inter-core cache coherence manager
 *
 * Jeff Brown
 * $Id: coherence-mgr.h,v 1.4.18.2.2.12 2009/05/30 04:35:45 jbrown Exp $
 */

#ifndef COHERENCE_MGR_H
#define COHERENCE_MGR_H

#ifdef __cplusplus
extern "C" {
#endif


struct CacheArray;
struct CoreResources;


typedef struct CoherenceMgr CoherenceMgr;

typedef enum {
    Coher_InstRead, Coher_DataRead, Coher_DataReadExcl,
    CoherAccessType_last
} CoherAccessType;
extern const char *CoherAccessType_names[];

typedef enum {
    // Be careful adding stall types, take a long look at inclusive/exclusive
    // uses of existing stall types in cache.c, be sure to update those.
    Coher_NoStall,      // Cache access may proceed down normally
    Coher_EntryBusy,    // Operation already outstanding on this block
    Coher_StallForInvl, // Stall for small inval. (shared->excl holder upgrade)
    Coher_StallForWB,   // Stall for WB (excl->shared downgrade); maybe dirty
    Coher_StallForXfer, // Stall for owner transfer (->excl); maybe dirty
    Coher_StallForShared, // Stall for transfer from neighbor (shared, clean)
    CoherAccessResult_last
} CoherAccessResult;
extern const char *CoherAccessResult_names[];


#define COHER_WB_NEEDS_INVAL(c_a_r) \
    (((c_a_r) == Coher_StallForInvl) || ((c_a_r) == Coher_StallForXfer))

// predicate: a cm_access() result requires waiting for communication
// with at least one peer
#define COHER_STALLS_FOR_PEERS(c_a_r) \
    (((c_a_r) == Coher_StallForInvl) || ((c_a_r) == Coher_StallForWB) || \
     ((c_a_r) == Coher_StallForXfer) || ((c_a_r) == Coher_StallForShared))

// predicate: a cm_access() result requires waiting for a peer, and
// that the peer had exclusive-ownership rights to the block in question.
#define COHER_STALL_FOR_EXCL(c_a_r) \
    (((c_a_r) == Coher_StallForWB) || ((c_a_r) == Coher_StallForXfer))


typedef struct CoherWaitInfo {
    int invl_needed;            // flag: invalidate instead of just downgrade
    int node_count;
    int *nodes;                 // dynamic [node_count], or NULL
} CoherWaitInfo;
void coherwaitinfo_destroy(CoherWaitInfo *cwi);



CoherenceMgr *cm_create(void);
void cm_destroy(CoherenceMgr *cm);

void cm_reset(CoherenceMgr *cm);
void cm_reset_cache(CoherenceMgr *cm, int cache_id);
void cm_reset_entry(CoherenceMgr *cm, LongAddr base_addr);


/*
 * Add the given cache to the coherence domain managed by the given
 * coherence manager, with the given ID number.  The ID number must be
 * a small non-negative integer.
 */
void cm_add_cache(CoherenceMgr *cm, struct CacheArray *cache, int cache_id,
                  struct CoreResources *parent_core);


// The typical progression of a request is one of the following:
// 1. cm_access --uncached--> cm_shared_request -> 
//    (access memory, shared cache, etc.) cm_shared_reply -> fill
// 2. cm_access --on-chip--> (send out WBI messages) -> cm_peer_reply ...
//    final cm_peer_reply -> fill
//
// Less common caces
// 3. cm_access --on-chip--> (send out WBI messages) -> cm_peer_reply ...
//    final cm_peer_reply, but haven't found data -> cm_shared_request
//    -> (access memory, etc.) -> cm_shared_reply -> fill


/*
 * Perform the coherence-checking portion of a simulated cache access, and
 * update coherence state.
 *
 * A CoherAccessResult value is returned indicating whether the access may
 * proceed through the cache hierarchy normally, or wait for some sort of
 * traffic from a peer.  Iff the access must wait, one of the Coher_Stall*
 * outcomes is returned, and info identifying the relevant peers is written to
 * "coher_wait_ret" for future scheduling.  (coher_wait_ret must be non-NULL,
 * *coher_wait_ret should initially be NULL, and if written to, the written
 * pointer must be handed to coherwaitinfo_destroy() after use).
 *
 * "writable_ret", if non-NULL, is used as a flag to indicate whether the
 * requestor will have write permission for the block, after any additional
 * needed coherence activity.
 */
CoherAccessResult
cm_access(CoherenceMgr *cm, LongAddr base_addr, int cache_id,
          CoherAccessType access_type, CoherWaitInfo **coher_wait_ret,
          int *writeable_ret);

// Note that a request is being sent to, or a reply received from, the
// globally shared part of the memory subsystem.  These are the components
// "below" the CoherenceMgr, i.e. a shared cache or main memory.  The
// CoherenceMgr is in charge of mediating access to these shared resources;
// it is enforced that there may be at most one outstanding request in
// the shared part of the memory system, for a given block.
// (The shared access MUST have already been cleared via cm_access().)
void cm_shared_request(CoherenceMgr *cm, LongAddr base_addr);
void cm_shared_reply(CoherenceMgr *cm, LongAddr base_addr);

// Note receipt of a reply from the given peer cache ID, to a
// coherence-generated request.  Returns zero if further replies are needed,
// nonzero on the final reply.
int cm_peer_reply(CoherenceMgr *cm, LongAddr base_addr, int reply_cache_id);

//
// Notify the coherence manager that all resources represented by the given
// cache ID, 1) have evicted all copies of the given block, and 
// 2) may not possibly acquire another copy of the given block without first
// calling cm_access().
//
// This is a bit squirrely, and the interface may change if we ever add
// explicitly-simulation eviction notification, enforced inclusion/exclusion,
// etc.  At the moment, these calls may end up being ignored, and are only
// used to help recover simulator memory.
void cm_evict_notify(CoherenceMgr *cm, LongAddr base_addr,
                     int cache_id);


// Query for consistency checking; prints details on failure, but still
// returns.
int cm_holder_okay(const CoherenceMgr *cm, LongAddr base_addr,
                   int cache_id, int dirty, int writeable);


#ifdef __cplusplus
}
#endif

#endif  /* COHERENCE_MGR_H */
