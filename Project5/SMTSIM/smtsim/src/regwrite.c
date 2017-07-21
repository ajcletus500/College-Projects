/* SMTSIM simulator.
   
   Copyright (C) 1994-1999 by Dean Tullsen (tullsen@cs.ucsd.edu)
   ALL RIGHTS RESERVED.

   SMTSIM is distributed under the following conditions:

     You may make copies of SMTSIM for your own use and modify those copies.

     All copies of SMTSIM must retain all copyright notices contained within.

     You may not sell SMTSIM or distribute SMTSIM in conjunction with a
     commercial product or service without the express written consent of
     Dean Tullsen.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.

 */

#include <stdio.h>

#include "sim-assert.h"
#include "main.h"
#include "core-resources.h"
#include "dyn-inst.h"
#include "app-state.h"
#include "context.h"


static void
unblock_commit_group(context * restrict ctx, activelist *leader)
{
    int leader_id = leader->id;    
    int final_id = -1;
    for (int id = leader_id; 
         ctx->alist[id].commit_group.leader_id == leader_id;
         id = alist_add(ctx, id, 1))
        final_id = id;
    sim_assert(final_id >= 0);
    activelist *final = &ctx->alist[final_id];
    DEBUGPRINTF("T%d: commit group s%d...%d done, unblocking\n", 
                leader->thread, leader_id, final_id);
    if (leader->commit_group.overlap_next_leader >= 0) {
        sim_assert(leader->commit_group.overlap_next_leader == final_id);
        final->commit_group.leader_id = final->id;
        sim_assert(final->commit_group.remaining != 0);
        // This inst has been completed once already, in the previous group
        final->commit_group.remaining--;
        DEBUGPRINTF("T%d: setting commit group s%d final "
                    "inst s%d as overlapping group leader, remain ->%d\n",
                    leader->thread, leader_id, final_id, 
                    final->commit_group.remaining);
        if (final->commit_group.remaining == 0)
            unblock_commit_group(ctx, final);
    }
}


void
regwrite(void)
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources * restrict core = Cores[core_id];
        const int rwrite1 = core->stage.rwrite1;
        const int rwriteN = core->stage.rwrite1 + 
            core->params.regwrite.n_stages - 1;
        activelist *instrn;

        // Move instructions from final regwrite stage to commit (not a StageQ)
        for (instrn = stageq_head(core->stage.s[rwriteN]); instrn != NULL;
             instrn = instrn->next) {
            // Instructions shouldn't show up here before completing;
            // they also shouldn't come strolling in late.
            // XXX assert too strong?  single-cyc fills screw it up :(
            //   sim_assert(instrn->donecycle == cyc);
            sim_assert(instrn->donecycle <= cyc);
            instrn->status = RETIREABLE;

            // FIXME: here we account for both regs read and written. 
            //        Should be split in regread and regwrite
            context *ctx = Contexts[instrn->thread];
            if (ctx->as != NULL) { // Not an injected inst
                if (instrn->fu == FP){
                    ctx->as->extra->freg_acc += instrn->regaccs;
                    ctx->as->extra->fq_acc += 1;
                }
                else{
                    ctx->as->extra->ireg_acc += instrn->regaccs;
                    ctx->as->extra->iq_acc += 1;
                }
            }
            if (instrn->commit_group.leader_id >= 0) {
                int leader_id = instrn->commit_group.leader_id;
                activelist *leader = &ctx->alist[leader_id];
                int remain = (--leader->commit_group.remaining);
                DEBUGPRINTF("T%d: s%d done, group s%d remain ->%d\n", ctx->id,
                            instrn->id, leader_id, remain);
                if (remain == 0)
                    unblock_commit_group(ctx, leader);
            }
        }

        stageq_clear(core->stage.s[rwriteN]);

        // Shift instructions from (rwrite1...N-1) to the next stage
        // (rwrite2...N)
        for (int src_stage = rwriteN - 1; src_stage >= rwrite1;
             src_stage--) {
            sim_assert(stageq_count(core->stage.s[src_stage + 1]) == 0);
            stageq_assign(core->stage.s[src_stage + 1], 
                          core->stage.s[src_stage]);
        }
    }
}
