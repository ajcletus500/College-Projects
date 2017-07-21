/*
 * Cache request structure / types
 *
 * Jeff Brown
 * $Id: cache-req.h,v 1.8.10.3.6.4.2.10.6.1 2009/12/25 06:31:48 jbrown Exp $
 */

#ifndef CACHE_REQ_H
#define CACHE_REQ_H

#ifdef __cplusplus
extern "C" {
#endif

struct context;
struct activelist;
struct AppState;


// (Be sure to update CacheAction_names[], cache_action_incore(), and
// creq_handlers[] in cache.c)
typedef enum {
    L1FILL, BUS_REQ, BUS_REPLY, BUS_WB,
    COHER_WBI_L1, COHER_WBI_L2_UP,      // coherence writeback/invalidate
    COHER_WBI_L2_DOWN,
    COHER_WAIT, COHER_REPLY,
    L2ACCESS, L2FILL, L2_WB, L3ACCESS, L3FILL, L3_WB,
    MEMACCESS, MEM_WB,
    CacheAction_last
} CacheAction;
extern const char *CacheAction_names[];         // cache.c


// Some constants for "service_level".
// Be sure to update creq_invariant() and fmt_service_level()
// if you change these.
#define SERVICED_NONE           0       // didn't access memory subsystem
#define SERVICED_UNKNOWN        -1
#define SERVICED_MEM            -2
#define SERVICED_COHER          -3


// Per-core stuff tracked by each CacheRequest
typedef struct CacheRequestCore {
    struct CoreResources *core;
    // "core" NULL => end of array, no other fields valid
    CacheSource src;
} CacheRequestCore;


// If you add or change fields/semantics, be sure to update creq_invariant()
typedef struct CacheRequest {
    i64 request_time;           /* Earliest time this request can _begin_ */
    i64 serial_num;             /* Unique, counts up at each enqueue op */
    int blocked;                // Flag: not ready, ignore request_time
    CacheAction action;

    LongAddr base_addr;

    int access_type;            // CacheAccessType enum
    int coher_accessed;         // Has passed through coherence access
    int coher_wb_type;          // CoherAccessResult enum (only w/src COHER)

    // Where in the memory subsystem the request was satisfied.
    // -1: unknown, 0: none, 1..3: level N $ hit, SERVICED_MEM, SERVICED_COHER
    int service_level;

    i64 create_time;
    struct activelist *drequestor;
    struct context *irequestor;

    // requests which must wait for this one to finish, due to coherence
    struct CacheRequest *dependent_coher;       // Linked list (FIFO)

    // cores which have contributed to drequestor/irequestor
    // terminated by an entry with a NULL core pointer
    struct CacheRequestCore *cores;       // [CoreCount + 1]

    // For signaling AppMgr at memory completion
    struct AppState **blocked_apps;             // malloc'd, NULL-terminated

    // if this is an ancillary COHER request of some sort (COHER_WBI_*,
    // COHER_REPLY,..), then coher_msg_for is the "real" request for which
    // this was generated.  (it's NULL otherwise)
    struct CacheRequest *coher_for;

    int is_dirty_fill;          // Flag: fill contains modified data

    // Flag: for requests blocked waiting for coherence traffic from peers,
    // indicate whether any peer has supplied a copy of that data.
    int coher_data_seen;

    // If you add or change fields/semantics, update creq_invariant()
} CacheRequest;


// Classify a given CacheAction as something which either takes place
// within a (single) core, or outside of any core.
int cache_action_incore(CacheAction action);    // cache.c

// Format a CacheRequest into a (single-instance) static buffer, for debug
// printing.
const char *fmt_creq_static(const struct CacheRequest *creq);   // cache.c

// single-instance static buffer
const char *fmt_service_level(int service_level);       // cache.c


// Sanity-check the fields of a live CacheRequest; it need not have been
// enqueued yet, but its req-time must be set.  Returns 1 or 0.
// Somewhat expensive.  Prints to stderr when the check fails.
int creq_invariant(const struct CacheRequest *creq, int recurse); // cache.c


#ifdef __cplusplus
}
#endif

#endif  /* CACHE_REQ_H */
