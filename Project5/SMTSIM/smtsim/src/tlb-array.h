/*
 * Fully-associative TLB object with simplified accounting
 *
 * Jeff Brown
 * $Id: tlb-array.h,v 1.2.10.1.2.1.2.3 2009/08/05 22:40:03 jbrown Exp $
 */

#ifndef TLB_ARRAY_H
#define TLB_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct TLBArray TLBArray;
typedef struct TLBStats TLBStats;


struct TLBStats {
    i64 hits, misses;
};


TLBArray *tlb_create(int n_entries, int page_bytes);
void tlb_destroy(TLBArray *tlb);

void tlb_reset(TLBArray *tlb);
void tlb_reset_stats(TLBArray *tlb);

/* Returns any penalty */
i64 tlb_lookup(TLBArray *tlb, i64 now, u64 addr, int thread_id, 
               i64 miss_penalty, int *is_hit_ret);
int tlb_probe(const TLBArray *tlb, u64 addr, int thread_id);
void tlb_inject(TLBArray *tlb, i64 ready_time, u64 addr, int thread_id);

void tlb_get_stats(const TLBArray *tlb, TLBStats *dest);

u64 tlb_calc_baseaddr(const TLBArray *tlb, u64 addr);


// Returns a malloc'd array of all base addresses matching thread ID
// master_id, or all tags if master_id==-1.  n_tags_ret must be non-NULL; the
// number of of elements in the returned array is written there.  (If no
// matches are found, *n_tags_ret will be set to 0, and NULL returned.)
LongAddr *tlb_get_tags(const TLBArray *tlb, int master_id,
                       int *n_tags_ret);

// count the entries matching master_id
int tlb_get_population(const TLBArray *tlb, int master_id);

// flush all entries matching master_id
void tlb_flush_app(TLBArray *tlb, int master_id);


#ifdef __cplusplus
}
#endif

#endif  /* TLB_ARRAY_H */
