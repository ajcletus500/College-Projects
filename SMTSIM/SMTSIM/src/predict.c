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

#include <stdio.h>
#include <stdlib.h>

#include "sim-assert.h"
#include "main.h"
#include "context.h"
#include "btb-array.h"
#include "pht-predict.h"
#include "core-resources.h"
#include "multi-bpredict.h"
#include "branch-bias-table.h"
#include "app-state.h"


#if defined(DEBUG)
#   define DEBUG_BTB            1
#   define DEBUG_PHT            1
#else
#   define DEBUG_BTB            0
#   define DEBUG_PHT            0
#endif


/*  Used when you are post-mispredict or post-synchronization
 *   when you want to know which direction to predict, but there
 *   isn't really a sense of right or wrong direction.
 */

int
get_bpredict(context *ctx, u64 pc, int ghr)
{
    int result = pht_probe(ctx->core->pht, pc, ghr);

    if (DEBUG_PHT && debug) {
        printf("pht: %s probe pc %s ghr %s -> %s\n", fmt_i64(cyc), fmt_x64(pc),
               fmt_x64(ghr), (result) ? "T" : "NT");
    }

    return result;
}


/*  This get's the branch prediction, but does not
 *    update the pht, which is not updated until the
 *    instruction retires.
 *  Uses the gshare predictor.
 */

// returns predicted taken/not-taken
int
bpredict(context *ctx, u64 pc, int taken, int ghr)
{
    int result = pht_lookup(ctx->core->pht, pc, taken, ghr);

    if (DEBUG_PHT && debug) {
        printf("pht: %s lookup pc %s taken %i ghr %s -> %s\n", fmt_i64(cyc),
               fmt_x64(pc), taken, fmt_x64(ghr), (result) ? "T" : "NT");
    }

    ctx->as->extra->hitrate.bpred.acc++;
    ctx->as->extra->bpred_acc++;
    if (!result == !taken)
        ctx->as->extra->hitrate.bpred.hits++;

    return result;
}


/* Committing instructions update the pht with
 *   their taken/not taken status.
 */

void
update_pht(context *ctx, u64 pc, int taken, int ghr)
{
    pht_update(ctx->core->pht, pc, taken, ghr);

    if (DEBUG_PHT && debug) {
        printf("pht: %s update pc %s taken %i ghr %s\n", fmt_i64(cyc),
               fmt_x64(pc), taken, fmt_x64(ghr));
    }
}


/*  Does the btb lookup.  This routine updates the btb immediately,
 *   which is optimistic.  This really should be decoupled, like the
 *   pht routines.
 */

// Returns the predicted target of this branch, or 0 if there was no entry
u64
btblookup(context *ctx, u64 pc, u64 nextpc, int taken, int is_jump)
{
    int thread = ctx->as->app_master_id;
    BTBArray *btb = ctx->core->btb;
    BTBLookupInfo l_info;

    sim_assert(nextpc != 0);
    u64 btb_dest = 0;
    
    l_info.dest = nextpc;
    l_info.taken = taken;
    l_info.is_jump = is_jump;

    // overwrites btb_dest with nonzero on a hit
    btb_lookup(btb, pc, thread, &l_info, &btb_dest);

    if ((btb_dest != nextpc) && taken) {
        // only put taken branches in the btb
        btb_update(btb, pc, thread, nextpc);
    }

    if (DEBUG_BTB && debug) {
        printf("btb: %s access pc %s nextpc %s masterid %i taken %i "
               "is_jump %i -> targ %s\n", fmt_i64(cyc), fmt_x64(pc),
               fmt_x64(nextpc), thread, taken, is_jump, fmt_x64(btb_dest));
    }

    return btb_dest;
}

/*   For when you want to do the btb lookup without altering
 *     the btb.
 */

u64
get_btblookup(context *ctx, u64 pc)
{
    int thread = ctx->as->app_master_id;
    u64 btb_dest = 0;

    btb_probe(ctx->core->btb, pc, thread, &btb_dest);

    if (DEBUG_BTB && debug) {
        printf("btb: %s probe pc %s masterid %i -> %s\n", 
               fmt_i64(cyc), fmt_x64(pc), thread, fmt_x64(btb_dest));
    }

    return btb_dest;
}


static void
predict_stats_core(const CoreResources *core)
{
    BTBStats btb_stats;
    PHTStats pht_stats;

    btb_get_stats(core->btb, &btb_stats);
    pht_get_stats(core->pht, &pht_stats);

    printf("Core %i:\n", core->core_id);
    printf("  branch prediction, hits = %s, misses = %s, hit rate = %.2f%%\n",
           fmt_i64(pht_stats.hits), fmt_i64(pht_stats.misses), 
           (float) (100.0*pht_stats.hits/(pht_stats.hits+pht_stats.misses)));
    printf("  jump prediction, hits = %s, misses = %s, hit rate = %.2f%%\n",
           fmt_i64(btb_stats.eff_hits), fmt_i64(btb_stats.eff_misses),
           (float) (100.0*btb_stats.eff_hits/
                    (btb_stats.eff_hits+btb_stats.eff_misses)));
    printf("  return_stack prediction, hits = %s, misses = %s, "
           "hit rate = %.2f%%\n",
           fmt_i64(core->rs_hits), fmt_i64(core->rs_misses),
           (float) (100.0*core->rs_hits/(core->rs_hits+core->rs_misses)));
    if (core->tcache)
        // Don't bother printing multi_bp stats if we're not using it
        mbp_print_stats(core->multi_bp, stdout, "  multi_bp, ");
}


void predict_stats()
{
    int i;
    for (i = 0; i < CoreCount; i++)
        predict_stats_core(Cores[i]);
}

void zero_pstats()
{
    int i;
    for (i = 0; i < CoreCount; i++) {
        CoreResources *core = Cores[i];
        pht_reset_stats(core->pht);
        btb_reset_stats(core->btb);
        mbp_reset_stats(core->multi_bp);
        bbt_reset_stats(core->br_bias);
    }
}


/*  Return stack routines */

void
rs_push(context *current, u64 addr)
{
    int idx = current->rs_size + current->rs_start;
    if (idx >= current->params.retstack_entries)
        idx -= current->params.retstack_entries;
    current->return_stack[idx] = addr;
    if (current->rs_size == current->params.retstack_entries) {
        current->rs_start++;
        if (current->rs_start == current->params.retstack_entries)
            current->rs_start = 0;
    } else
        current->rs_size++;
}

u64
rs_pop(context *current, u64 nextpc)
{
    u64 result;
    if (current->rs_size == 0) {
        result = 0;
    } else {
        int idx = current->rs_size - 1 + current->rs_start;
        if (idx >= current->params.retstack_entries)
            idx -= current->params.retstack_entries;
        current->rs_size--;
        result = current->return_stack[idx];
    }
    sim_assert(nextpc != 0);
    if (result == nextpc) {
        current->core->rs_hits++;
    } else {
        current->core->rs_misses++;
    }

    current->as->extra->hitrate.retpred.acc++;
    if (result == nextpc)
        current->as->extra->hitrate.retpred.hits++;

    return result;
}

/* Special case for wrong-path execution.*/

u64
wp_rs_pop(context *current)
{
  if (current->rs_size == 0) {
    return 0;
  }
  int idx = current->rs_size - 1 + current->rs_start;
  if (idx >= current->params.retstack_entries)
      idx -= current->params.retstack_entries;
  current->rs_size--;
  return current->return_stack[idx];
}


