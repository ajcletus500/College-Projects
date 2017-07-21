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

 */

#include <stdio.h>

#include "sim-assert.h"
#include "main.h"
#include "core-resources.h"
#include "dyn-inst.h"
#include "context.h"
#include "app-state.h"
#include "stash.h"
#include "adapt-mgr.h"


// Log which apps have been blocked this cycle by an instruction queue conflict
static void
log_apps_qconf_cyc(const StageQueue * restrict stalled)
{
    const int n_insts = stageq_count(*stalled);
    int app_ids[n_insts];
    int app_ids_seen = 0;

    for (const activelist * restrict inst = stageq_head(*stalled);
         inst != NULL;
         inst = inst->next) {
        if (inst->as) {
            const int this_app_id = inst->as->app_id;
            int scan;
            // Crufty N^2 set implementation
            for (scan = 0; scan < app_ids_seen; scan++) {
                if (app_ids[scan] == this_app_id)
                    break;
            }
            if (scan == app_ids_seen) {
                // Haven't seen this one yet
                inst->as->extra->instq_conf_cyc++;
                sim_assert(app_ids_seen < n_insts);
                app_ids[app_ids_seen] = this_app_id;
                app_ids_seen++;
            }
        }
    }

#ifdef DEBUG
    if (app_ids_seen >= 32) {
        static int nagged = 0;
        if (!nagged) {
            fprintf(stderr, "(%s:%d): WARNING: large rename queue seen "
                    "(%d app-ids), N^2 queue-full stats may be inefficient!\n",
                    __FILE__, __LINE__, app_ids_seen);
            nagged = 1;
        }
    }
#endif

}



/* Here, instructions get stalled if there is no room in the instruction
 * queues, or if there is not space in the load-store queue, or if there are no
 * renaming registers available.  Load Store Queue is not accurately modeled.
 * It's merely a load and store counter, no address forwarding is done.  This
 * will lead to pessimistic performance estimation and higher LSQ occupancy.
 * Loads will be delayed when there is an earlier store that writes to the same
 * address (see queue.c).  Register renaming is not explicitly done, but the
 * effects are accurately modeled.  That is, I carefully track the number of
 * renaming registers that are being used, so I know when we run out.  It is
 * not necessary to model the renaming itself to know when we run out of
 * registers.
 */

static void
regrename_for_core(CoreResources *core)
{
    int rename1 = core->stage.rename1;
    int rename_n = core->stage.rename1 + core->params.rename.n_stages - 1;
    
    StageQueue * restrict rename_src = &core->stage.s[rename_n];
    int i;
    
    // Move instructions from renameN into IQ / FQ, if space available
    while (stageq_count(*rename_src) > 0) {
        activelist * restrict instrn = stageq_head(*rename_src);
        context * restrict current = Contexts[instrn->thread];

        if (instrn->status & (INVALID | SQUASHED)) {
            stageq_dequeue(*rename_src);
            continue;
        }

        /* ROB space available? */
        if (is_shared(ROB)){ //Resource Pooling
            if (space_available(ROB,current) < 1){
                current->stats.robconf_cyc++;
                break;
            }
        }
        else { //No Resource Pooling
            if (current->rob_used >= current->params.reorder_buffer_size) {
                current->stats.robconf_cyc++;
                break;
            }
        }

        /* LSQ space available? */
        if (instrn->mem_flags) { 
            if (is_shared(LSQ)){ //Resource Pooling
                if (space_available(LSQ,current) < 1){
                    core->q_stats.lsqconf_cyc++;
                    DEBUGPRINTF("C%i: LSQ full\n", core->core_id);
                    break;
                }
            }
            else { //No Resource Pooling
                if (core->lsq_used >= core->params.loadstore_queue_size) {
                    core->q_stats.lsqconf_cyc++;
                    DEBUGPRINTF("C%i: LSQ full\n", core->core_id);
                    break;
                }
            }
        }
        
        /* rename registers available? */
        if (!IS_ZERO_REG(instrn->dest)) {
            if (!IS_FP_REG(instrn->dest)) {
                if (is_shared(IREG)){ //Resource Pooling
                    if (space_available(IREG,current) < 1){
                        core->q_stats.iregconf_cyc++;
                        break;
                    }
                    else {
                        instrn->iregs_used = 1;
                        core->i_registers_used++;
                    }
                }
                else { //No Resource Pooling
                    if (core->i_registers_used >= 
                            core->params.rename.int_rename_regs) {
                        core->q_stats.iregconf_cyc++;
                        break;
                    }
                    else {
                        instrn->iregs_used = 1;
                        core->i_registers_used++;
                    }
               }
            } else {
                if (is_shared(FREG)){ //Resource Pooling
                    if (space_available(FREG,current) < 1){
                        core->q_stats.fregconf_cyc++;
                        break;
                    }
                    else {
                        instrn->fregs_used = 1;
                        core->f_registers_used++;
                    }
                }
                else { //No Resource Pooling
                    if (core->f_registers_used >=
                            core->params.rename.float_rename_regs) {
                        core->q_stats.fregconf_cyc++;
                        break;
                    } 
                    else {
                        instrn->fregs_used = 1;
                        core->f_registers_used++;
                    }
                }
            }
        }

        /* queue space available? */
        if (instrn->fu == FP) {
            if (is_shared(FQ)){ //Resource Pooling
                if (space_available(FQ,current) < 1){
                    DEBUGPRINTF("C%i: FQ full\n", core->core_id);
                    core->q_stats.fqconf_cyc++;
                    core->i_registers_used -= instrn->iregs_used;
                    core->f_registers_used -= instrn->fregs_used;
                    instrn->iregs_used = instrn->fregs_used = 0;
                    log_apps_qconf_cyc(rename_src);
                    break;
                } /* space available */
                else {
                    instrn->robentry = 1;
                    current->rob_used++;
                    update_acc_occ_per_inst(current, instrn, 0, 1);
                    update_adapt_mgr_incr(current, ROB, 1);
                    update_adapt_mgr_incr(current, FQ, 1);
                    update_adapt_mgr_incr(current, IREG, instrn->iregs_used);
                    update_adapt_mgr_incr(current, FREG, instrn->fregs_used);
                    if (instrn->mem_flags){
                        instrn->lsqentry = 1; 
                        core->lsq_used++;
                        update_adapt_mgr_incr(current, LSQ, instrn->lsqentry);
                    }
                        
                    stageq_dequeue(*rename_src);
                    stageq_enqueue(core->stage.floatq, instrn);
                }
            }
            else { //No Resource Pooling
                if (stageq_count(core->stage.floatq) >= core->params.queue.float_queue_size) {
                    DEBUGPRINTF("C%i: FQ full\n", core->core_id);
                    core->q_stats.fqconf_cyc++;
                    core->i_registers_used -= instrn->iregs_used;
                    core->f_registers_used -= instrn->fregs_used;
                    instrn->iregs_used = instrn->fregs_used = 0;
                    log_apps_qconf_cyc(rename_src);
                    break;
                } /* space available */
                instrn->robentry = 1;
                current->rob_used++;
                update_acc_occ_per_inst(current, instrn, 0, 1);
                update_adapt_mgr_incr(current, ROB, 1);
                update_adapt_mgr_incr(current, FQ, 1);
                update_adapt_mgr_incr(current, IREG, instrn->iregs_used);
                update_adapt_mgr_incr(current, FREG, instrn->fregs_used);
                if (instrn->mem_flags){
                    instrn->lsqentry = 1; 
                    core->lsq_used++;
                    update_adapt_mgr_incr(current, LSQ, instrn->lsqentry);
                }
                    
                stageq_dequeue(*rename_src);
                stageq_enqueue(core->stage.floatq, instrn);
            }
        } else { /* integer queue */
            if (is_shared(IQ)){ //Resource Pooling
                if (space_available(IQ,current) < 1){
                    DEBUGPRINTF("C%i: IQ full\n", core->core_id);
                    core->q_stats.iqconf_cyc++;
                    core->i_registers_used -= instrn->iregs_used;
                    core->f_registers_used -= instrn->fregs_used;
                    instrn->iregs_used = instrn->fregs_used = 0;
                    log_apps_qconf_cyc(rename_src);
                    break;
                } /* space available */
                else {
                    instrn->robentry = 1;
                    current->rob_used++;
                    update_acc_occ_per_inst(current, instrn, 0, 0);
                    update_adapt_mgr_incr(current, ROB, 1);
                    update_adapt_mgr_incr(current, IQ, 1);
                    update_adapt_mgr_incr(current, IREG, instrn->iregs_used);
                    update_adapt_mgr_incr(current, FREG, instrn->fregs_used);
                    if (instrn->mem_flags){
                        instrn->lsqentry = 1; 
                        core->lsq_used++;
                        update_adapt_mgr_incr(current, LSQ, instrn->lsqentry);
                    }
                        
                    stageq_dequeue(*rename_src);
                    stageq_enqueue(core->stage.intq, instrn);
                }
            }
            else { //No Resource Pooling
                if (stageq_count(core->stage.intq) >= core->params.queue.int_queue_size) {
                    DEBUGPRINTF("C%i: IQ full\n", core->core_id);
                    core->q_stats.iqconf_cyc++;
                    core->i_registers_used -= instrn->iregs_used;
                    core->f_registers_used -= instrn->fregs_used;
                    instrn->iregs_used = instrn->fregs_used = 0;
                    log_apps_qconf_cyc(rename_src);
                    break;
                } 
                /* space available */
                instrn->robentry = 1;
                current->rob_used++;
                update_acc_occ_per_inst(current, instrn, 0, 0);
                update_adapt_mgr_incr(current, ROB, 1);
                update_adapt_mgr_incr(current, IQ, 1);
                update_adapt_mgr_incr(current, IREG, instrn->iregs_used);
                update_adapt_mgr_incr(current, FREG, instrn->fregs_used);
                if (instrn->mem_flags) {
                    instrn->lsqentry = 1; 
                    core->lsq_used++;
                    update_adapt_mgr_incr(current, LSQ, instrn->lsqentry);
                }

                stageq_dequeue(*rename_src);
                stageq_enqueue(core->stage.intq, instrn);
            }
        }
        instrn->renamecycle = cyc;
    }

    // Update Context Occupancy stats
    for (i = 0; i < core->n_contexts; i++)
    {
        context * restrict ctx = core->contexts[i];
        ctx->stats.robsizetotal += core->contexts[i]->rob_used;
        if (ctx->as != NULL){
            ctx->as->extra->rob_occ += core->contexts[i]->rob_used;
            ctx->as->extra->iq_occ += ctx->as->extra->iqsize_this_cyc;
            ctx->as->extra->fq_occ += ctx->as->extra->fqsize_this_cyc;
            ctx->as->extra->ireg_occ += ctx->as->extra->iregs_this_cyc;
            ctx->as->extra->freg_occ += ctx->as->extra->fregs_this_cyc;
            ctx->as->extra->lsq_occ += ctx->as->extra->lsqsize_this_cyc;
        }
    }
    // Update core ireg occupancy stats
    core->q_stats.iregsizetotal += core->i_registers_used;
    // Update core freg occupancy stats
    core->q_stats.fregsizetotal += core->f_registers_used;
    // Update core lsq occupancy stats
    core->q_stats.lsqsizetotal += core->lsq_used;

    core->q_stats.iqsizetotal += stageq_count(core->stage.intq);
    core->q_stats.fqsizetotal += stageq_count(core->stage.floatq);

    // Shift instructions from rename1...N-1 to the next stage
    // (rename2...N) if clear
    for (int src_stage = rename_n - 1; src_stage >= rename1;
         src_stage--) {
        if (stageq_count(core->stage.s[src_stage + 1]) == 0) {
            stageq_assign(core->stage.s[src_stage + 1], 
                          core->stage.s[src_stage]);
        } else {
            DEBUGPRINTF("C%i: rename%i backed up\n", core->core_id,
                        src_stage - rename1 + 1);
        }
    }
}


void
regrename(void)
{
    order_policy op = get_order_policy();
    int core_id;
    switch (op){
    case FIXED: {
        for (core_id = 0; core_id < CoreCount; core_id++) {
            CoreResources *core = Cores[core_id];
            regrename_for_core(core);
        }
        break;
    }
    case RROBIN: {
        static int start_core_id;
        static int start_core_id_init = 0;
        int i;
        if (!start_core_id_init) {
            start_core_id = 0;
            start_core_id_init = 1;
        }
        for (i = 0; i < CoreCount; i++) {
            CoreResources *core = Cores[(start_core_id+i)%CoreCount];
            regrename_for_core(core);
        }
        start_core_id = (start_core_id+1) % CoreCount;
        break;
    }
    default:
        printf("ERROR: Unknown orderpolicy\n");
        sim_abort();
    }
}
