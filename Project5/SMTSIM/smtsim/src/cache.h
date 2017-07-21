/* SMTSIM simulator.
   
   Copyright (C) 1994-1999 by Dean Tullsen (tullsen@cs.ucsd.edu)
   ALL RIGHTS RESERVED.

   SMTSIM is distributed under the following conditions:

     You may make copies of SMTSIM for your own use and modify those copies.

     All copies of SMTSIM must retain all copyright notices contained within.

     You may not sell SMTSIM or distribute SMTSIM in conjunction with a
     commerical product or service without the express written consent of
     Dean Tullsen.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.

Significant parts of the SMTSIM simulator were written by Gun Sirer
(before it became the SMTSIM simulator) and by Jack Lo (after it became
the SMTSIM simulator).  Therefore the following copyrights may also apply:

Copyright (C) Jack Lo
Copyright (C) E. Gun Sirer

Pieces of this code may have been derived from Jim Larus\' SPIM simulator,
which contains the following copyright:

==============================================================
   Copyright (C) 1990-1998 by James Larus (larus@cs.wisc.edu).
   ALL RIGHTS RESERVED.

   SPIM is distributed under the following conditions:

     You may make copies of SPIM for your own use and modify those copies.

     All copies of SPIM must retain my name and copyright notice.

     You may not sell SPIM or distributed SPIM in conjunction with a
     commerical product or service without the expressed written consent of
     James Larus.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.
===============================================================
 */


#ifndef CACHE_H
#define CACHE_H

// Defined elsewhere
struct context;
struct activelist;
struct CoreResources;

#ifdef __cplusplus
extern "C" {
#endif


// (Be sure to update enum strings in cache.c)
typedef enum {
    // NOTE: CSrc_L1_* are cruftily arranged as bit-fields which get 
    // ORed together, but we also maintain distinct strings for them so
    // we can easily print them by name.  (This is a wart.)
    CSrc_None = 0,
    CSrc_L1_ICache = 1,
    CSrc_L1_DCache = 2,
    CSrc_L1_D_I = 3,
    CSrc_L1_DStreamBuf = 4,
    CSrc_L1_Ds_I = 5,
    CSrc_L1_Ds_D = 6,
    CSrc_L1_Ds_D_I = 7,
    CSrc_L1_mask = 7,
    CSrc_WB = 8,
    CSrc_Coher,
    CSrc_L2,
    CSrc_L3,
    CacheSource_last } CacheSource;
extern const char *CacheSource_names[];         // cache.c

// predicate: CacheSource value is L1-only
#define CACHE_SOURCE_L1_ONLY(src) (!((src) & ~CSrc_L1_mask))
// predicate: L1 CacheSource "csrc_big" contains all of "csrc_small" (subset)
#define CACHE_SOURCE_L1_CONTAINS(csrc_big, csrc_small) \
    (CACHE_SOURCE_L1_ONLY((csrc_big) | (csrc_small)) && \
     (((csrc_small) & (~(csrc_big))) == 0))


#define MEMDELAY_LONG   -1


void initcache(void);
void init_coher(void);
void process_cache_queues(void);
int doiaccess(mem_addr addr, struct context *current);
int dodaccess(mem_addr addr, int is_write, struct context *current,
              struct activelist *meminst, i64 addr_ready_cyc);

void print_cstats(void);
void zero_cstats(void);
void init_tlbs(void);
void clean_cache_queue_mispredict(struct context *current);
void clean_cache_queue_squash(void);
mem_addr calc_lock_paddr(struct context *ctx, mem_addr addr);

int cache_register_blocked_app(struct context *ctx, int dmiss_alist_id);


struct CacheArray;
extern struct CacheArray *SharedL2Cache;        // May be NULL
extern struct CacheArray *SharedL3Cache;        // May be NULL
struct CoreBus;
extern struct CoreBus *SharedCoreRequestBus;
extern struct CoreBus *SharedCoreReplyBus;
struct MemUnit;
extern struct MemUnit *SharedMemUnit;
struct CoherenceMgr;
extern struct CoherenceMgr *GlobalCoherMgr;


// prefetch direct to an L1 cache, or an in-core stream buffer
// (only a subset of CacheSource values are allowed for pf_source; see
// comments with implementation)
//
// it's a little shady to push detailed info about the requestor through this
// interface (inflight_id, stream_id, etc.) for PrefetchAuditor purposes;
// however, that's less messy than allowing information about CacheRequests
// and merging to flow the other direction, and expecting the callers to
// deal with that.

int
cachesim_prefetch_for_streambuf(struct CoreResources *core, LongAddr base_addr,
                                int exclusive_access, const void *inflight_id,
                                int stream_id, int entry_id);

// support for simple "next-block on first touch" prefetcher
int
cachesim_prefetch_for_nextblock(struct CoreResources *core, LongAddr base_addr,
                                int exclusive_access, CacheSource pf_source);


#ifdef __cplusplus
}
#endif

#endif  /* CACHE_H */
