/*
 * BTB object
 *
 * Jeff Brown
 * $Id: btb-array.h,v 1.2.14.1 2008/04/30 22:17:45 jbrown Exp $
 */

#ifndef BTB_ARRAY_H
#define BTB_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct BTBArray BTBArray;
typedef struct BTBStats BTBStats;
typedef struct BTBLookupInfo BTBLookupInfo; 


/* Extra info passed to btb_lookup() to help accounting */
struct BTBLookupInfo {
    u64 dest;
    int taken;
    int is_jump;
};


struct BTBStats {
    i64 hits;           /* (PC, thread) matched */
    i64 misses;         /* One of (PC, thread) mismatched */

    i64 jump_dest_mismatch;  /* Jump (PC, thread) matched, dest mismatched */
    i64 miss_not_taken;         /* Not-taken branch missed */

    i64 eff_hits;       /* hits - jump_dest_mismatch */
    i64 eff_misses;     /* misses + jump_dest_mismatch - miss_not_taken */
};


BTBArray *btb_create(int n_entries, int assoc, int inst_bytes);
void btb_destroy(BTBArray *btb);

void btb_reset(BTBArray *btb);
void btb_reset_stats(BTBArray *btb);

/*
 * Lookup the given address/thread in the BTB.  On a miss, zero is returned.
 * On a hit, nonzero is returned, and the branch destination is
 * written to "dest_ret".
 *
 * The criteria for a hit is having (addr, thread_id) match.  If the
 * destination mismatches and the branch is a jump, it is counted as a hit,
 * but is recorded.  If the access misses in the BTB but the branch is not
 * taken, it is counted as a miss, but also recorded.
 * 
 * This also selects a line for (optional) future replacement on the next call
 * to btb_update().
 */
int btb_lookup(BTBArray *btb, u64 addr, int thread_id, 
               const BTBLookupInfo *info, u64 *dest_ret);


/* Like btb_lookup(), but doesn't change any state or stats */
int btb_probe(const BTBArray *btb, u64 addr, int thread_id, u64 *dest_ret);


/*
 * Update the BTB regarding the previous lookup: if it was a miss, this
 * replaces the BTB line selected by btb_lookup().  If the lookup was a hit,
 * this re-assigns "dest".  This should be done _immediately_ after
 * btb_lookup(), since it may not affect the LRU status bits.
 */
void btb_update(BTBArray *btb, u64 addr, int thread_id, u64 dest);

void btb_get_stats(const BTBArray *btb, BTBStats *dest);

u64 btb_calc_baseaddr(const BTBArray *btb, u64 addr);


#ifdef __cplusplus
}
#endif

#endif  /* BTB_ARRAY_H */
