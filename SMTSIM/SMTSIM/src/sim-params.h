/*
 * Global simulator config parameters/allocators
 *
 * Jeff Brown
 * $Id: sim-params.h,v 1.12.6.12.2.2.2.2 2009/12/21 05:44:39 jbrown Exp $
 */

#ifndef SIM_PARAMS_H
#define SIM_PARAMS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "cache-params.h"

struct context;
struct activelist;
struct CacheRequest;


typedef enum {
    TCP_Smt, TCP_Cmp, TCP_last
} ThreadCorePolicy;


typedef struct SimParams {
    i64 thread_length;
    i64 allinstructions;        // <0: unset
    int num_contexts;
    int num_cores;
    int nice_level;
    int disable_coredump;
    int reap_alist_at_squash;
    int abort_on_alist_full;

    int long_mem_cyc;
    int long_mem_at_commit;
    int print_appmgr_stats;

    struct {
        ThreadCorePolicy policy;
        int *map;                               // [MAXTHREADS]
    } thread_core_map;

    struct {
        int cache_request_holders;
        int cache_block_bytes, cache_block_bytes_lg;
        int page_bytes, page_bytes_lg;
        int inst_bytes;
        int split_bus;
        OpTime bus_request_time;
        OpTime bus_transfer_time;
        int stack_initial_kb;
        int stack_max_kb;
        int use_coherence;
        int use_l3cache;
        int private_l2caches;
        CacheGeometry *l2cache_geom;            // Only for shared L2
        CacheTiming l2cache_timing;             // Only for shared L2
        CacheGeometry *l3cache_geom;
        CacheTiming l3cache_timing;
        MemUnitParams main_mem;
    } mem;
} SimParams;


extern SimParams GlobalParams;

// These are variables which used to be statically-sized arrays outside of
// functions; now they're allocated at startup
extern struct CacheRequest *CacheHolders;               // [HOLDERS]
extern struct CacheRequest **CacheFreeHolders;          // [HOLDERS]


void alloc_globals(void);
int tcp_parse_policy(const char *str);
int tcp_policy_core(int policy, int thread_id);


#ifdef __cplusplus
}
#endif

#endif  /* SIM_PARAMS_H */
