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
#include <string.h>
#include <math.h>

#include "sim-assert.h"
#include "main.h"
#include "stash.h"
#include "cache.h"
#include "cache-array.h"
#include "smt.h"
#include "inst.h"
#include "core-resources.h"
#include "mem.h"
#include "sign-extend.h"
#include "quirks.h"
#include "jtimer.h"
#include "app-state.h"
#include "context.h"
#include "dyn-inst.h"
#include "sim-cfg.h"
#include "sim-params.h"
#include "mshr.h"


i64 misfetchtotal=0;


// This saves a copy of any architectural state that will be overwritten by
// the emulation of the next instruction.  This must called before
// stash->emulate (which performs the state changes), and thus before
// setup_instruction, so the instruction's activelist entry is not already
// initialized.
//
// This really belongs in emulate.c, but for the time being it's here so the
// compiler at least has a chance to inline it where it's used the most.
static inline void
save_instundo_info(context * restrict ctx, const StashData * restrict stash,
                   int inst_id)
{
    int destreg = stash->dest;
    inst_undo_info * restrict undo = &ctx->alist[inst_id].undo;

    if (!CHECKPOINT_CP_INSTS)
        sim_assert(ctx->wrong_path || ctx->follow_sync);

    undo->dest_reg_val = ctx->as->R[destreg].i;
    if (CHECKPOINT_STORES && (stash->mem_flags & SMF_Write)) {
        int mem_width = SMF_GetWidth(stash->mem_flags);
        mem_addr eff_addr = emu_calc_destmem(ctx->as, stash);
        undo->dest_mem_val = pmem_read_n(ctx->as->pmem, mem_width, 
                                         eff_addr, PMAF_NoExcept);
    }

    undo->dest_lastwriter = ctx->last_writer[destreg];

    if (SBF_UsesRetStack(stash->br_flags)) {
        if (stash->br_flags & SBF_RS_Push) {
            if (ctx->rs_size == ctx->params.retstack_entries) {
                // Save bottom of stack, which will be overwritten
                undo->retstack_val = ctx->return_stack[ctx->rs_start];
                sim_assert(undo->retstack_val != 0);
            } else {
                undo->retstack_val = 0;
            }
        } else {
            // Pop / pop-push
            if (ctx->rs_size > 0) {
                int idx = ctx->rs_size - 1 + ctx->rs_start;
                if (idx >= ctx->params.retstack_entries)
                    idx -= ctx->params.retstack_entries;
                // Save value to be popped from top of stack; if this is a
                // pop-push, this is the value that will be overwritten by the
                // push.
                undo->retstack_val = ctx->return_stack[idx];    
                sim_assert(undo->retstack_val != 0);
            } else {
                undo->retstack_val = 0;
            }
        }
    }

    undo->undone = 0;
#ifdef DEBUG
    undo->undo_ghr = ctx->ghr;
#endif
}


//
// This computes the "delay" value for instructions whose delay varies 
// deterministically with their inputs and other dynamic execution state
// that's available during emulation.
//

static int
calc_var_delay(const context * restrict ctx, const StashData * restrict st)
{
    int delay = ctx->fetching_inst_delay;
    u32 inst = st->inst;

    sim_assert(st->gen_flags & SGF_DetermVarDelay);

    switch (INST_OPCODE(inst)) {
    case FLTI:
        switch (INST_FLTI_SRCFNC(inst)) {
        case DIVS:
        case DIVT:
            if (FLOAT_DIVZERO_WP_SHORTDELAY &&
                ctx->wrong_path && (ctx->as->R[st->src_b].f == 0)) {
                delay = 4;
            }
            break;
        }
    }

    return delay;
}


void
emulate_inst_for_sim(context * restrict ctx, const StashData * restrict st,
                     int inst_id)
{
    // calc_inst_delay()
    sim_assert((st->delay_class >= 0) &&
           (st->delay_class < StashDelayClass_count));
    ctx->fetching_inst_delay =
        ctx->core->params.exec_timing.delay[SDC_all_extra] +
        ctx->core->params.exec_timing.delay[st->delay_class];
    if (st->gen_flags & SGF_DetermVarDelay)
        ctx->fetching_inst_delay = calc_var_delay(ctx, st);

    if (CHECKPOINT_CP_INSTS | ctx->wrong_path | ctx->follow_sync)
        save_instundo_info(ctx, st, inst_id);

    emulate_inst(ctx->as, st, &ctx->emu_inst, 
                 ctx->wrong_path || ctx->follow_sync);

    if (st->gen_flags & SGF_SysCall)
        ctx->stats.total_syscalls++;
}


/* decide on the next thread that has the opportunity to fetch this cycle.
 */

static context *
get_next_thread(CoreResources *core)
{
    context *ctx;

    // consider this core's contexts, in priority order, until we find
    // one that can fetch or we run out of free I-cache MSHR entries

    while (core->sched.priorityslot < core->n_contexts) {
        ctx = core->contexts[
            core->sched.priority[core->sched.priorityslot++]];
            
        /* this thread ok if it is running, not waiting for a cache miss,
           not blocked for some reason, and will not cause a bank conflict
           with a thread already accessed this cycle */
            
        if (!ctx->running || ctx->sync_lock_blocked ||
            (ctx->fetchcycle > cyc) || ctx->draining)
            continue;
        
        LongAddr fetch_addr;
        laddr_set(fetch_addr, ctx->as->npc, ctx->as->app_master_id);

        if (ctx->tc.avail) {
            // use remaining trace cache insts
        } else if (!mshr_is_avail(core->inst_mshr, fetch_addr)) {
            // I-MSHRs full: can't support any possible I-misses
            ctx->stats.i_mshr_conf++;
            continue;
        } else {
            int bank_port_ready =
                cache_probebank_avail(core->icache, fetch_addr, cyc, 0);
            if (!bank_port_ready)
                continue;
            // Bank/port will be updated by "doiaccess" call.
        }

        ctx->fthiscycle = 0;
        return ctx;
    }
        
    /* didn't find anything */
    return NULL;
}


/* fetch the next instruction for the current thread.*/

static int
get_next_instruction(context *ctx)
{
    int al_index;

    if (!(ctx->running)) {
        /*
          this should only happen if recent instructions caused this
        */
        return(0);
    }
  
    /* have we crossed a cache line boundary, or taken a branch? */
        
    if (!ctx->tc.avail && ctx->fthiscycle) {
        LongAddr prev_fetch, next_fetch;
        if (ctx->as->npc != ctx->pc + 4)
            return 0;
        laddr_set(prev_fetch, ctx->pc, ctx->as->app_master_id);
        laddr_set(next_fetch, ctx->as->npc, ctx->as->app_master_id);
        cache_align_addr(ctx->core->icache, &prev_fetch);
        cache_align_addr(ctx->core->icache, &next_fetch);
        if (!laddr_eq(prev_fetch, next_fetch)) 
            return 0;
    }
  
    if (ctx->draining)
        return 0;

    /* do we have the data structures available to handle this? */
    al_index = (ctx->alisttop + 1) & (ctx->params.active_list_size - 1);
    if (!(ctx->alist[al_index].status & INVALID) || 
        (&ctx->alist[al_index] == ctx->lock_failed) ||
        (&ctx->alist[al_index] == ctx->mispredict_discovered))
    {
        DEBUGPRINTF("T%d: uh-oh.  Out of activelist entries!\n", ctx->id);
        ctx->stats.alistconf_cyc++;
        if (GlobalParams.abort_on_alist_full) {
            // smtsim activelist entries don't correspond directly to a
            // physical processor resource (see comments in config file), so
            // stalling fetch for this is bogus.  This is counted and can be
            // tolerated, but we'll optionally make sure it never happens.
            abort_printf("T%d out of activelist entries\n", ctx->id);
        }
        return 0;
    }
    ctx->alisttop = al_index;
    return 1;
}


// Calc the trace block index of the current instruction
static inline int
tcache_instidx(const context *ctx)
{
    const TraceCacheBlock *tc_block = ctx->tc.block;
    sim_assert(tc_block->thread_id == ctx->as->app_master_id);
    sim_assert(ctx->tc.avail);
    sim_assert(ctx->tc.avail <= tc_block->inst_count);
    int inst_idx = tc_block->inst_count - ctx->tc.avail;
    return inst_idx;
}


// Compute the PC following the given instruction in the active trace block.
// Returns 0 if the trace block doesn't have that info.
//
// If "get_target_pred" is set, this returns the predicted branch target (but
// not the outcome) of the given instruction, for mispredict detection in the
// simulator.  The next trace block PC may differ from the branch target PC
// for taken branches, since the trace block may omit NOPs at the
// destination.  If the "get_targ_pred" result matches the correct
// branch target, then it is safe to skip ahead to the non-get_targ_pred 
// result.
static inline mem_addr
tcache_nextpc(const TraceCacheBlock *tc_block, int inst_idx, 
              int get_target_pred)
{
    const TraceCacheInst *tc_inst = &tc_block->insts[inst_idx];
    mem_addr nextpc;
    if ((inst_idx + 1) < tc_block->inst_count) {
        // Next inst is within same TC block: nextpc immediately available
        nextpc = (get_target_pred) ? tc_inst->target_pc : 
            tc_block->insts[inst_idx + 1].pc;
    } else {
        // Last inst of TC block: get nextpc from prediction
        int taken = (tc_inst->br_flags & TBF_Br_Taken);
        nextpc = (taken) ? tc_inst->target_pc : tc_block->fallthrough_pc;
    }           
    return nextpc;
}


// This MUST correctly update several bits of simulator state corresponding
// to branch instructions.  At the context level: 
//   - Next PC
//   - GHR (interacts with inst-undo code)
//   - Return stack (interacts with inst-undo code)
// At the instruction level:
//   - Emulated branch target and direction, from "ctx->emu_inst"
//   - Simulated branch target and direction which were used to guide fetch;
//     this is critical for detecting incorrect control flow in resim_branch()
//   - Whether simulation skipped the branch predictor or not
// ...if you add anything to these lists, make sure you update resim_branch()
// to match.

static void
sim_branch(context * restrict ctx, activelist * restrict inst)
{
    const int will_commit = !(ctx->wrong_path | ctx->follow_sync);
    const StashBranchFlags br_flags = inst->br_flags;
    const mem_addr correct_target = ctx->emu_inst.br_target;
    const int correct_taken = ctx->emu_inst.taken_branch;
    int is_from_tc = inst->tc.base_pc != 0;
    mem_addr tc_nextpc = 0;
    mem_addr tc_target = 0;
    int tc_taken = 0;
    int skip_bpredict = !SBF_CondBranch(br_flags);
    static int perfect_bp = -1;

    if (perfect_bp < 0)
        perfect_bp = simcfg_get_bool("Hacking/perfect_branch_pred");

    sim_assert(br_flags);

    if (inst->gen_flags & SGF_SysCall) {
        // This isn't really a branch, but some parts of the simulator treat
        // it as one.  This isn't one of those parts.
        inst->taken_branch = 0;
        inst->br_target = 0;
        inst->taken_predict = 0;
        inst->target_predict = 0;
        inst->skipped_bpredict = 1;
        return;
    }

    if (is_from_tc) {
        int inst_idx = tcache_instidx(ctx);
        TraceBranchFlags tbr_flags = ctx->tc.block->insts[inst_idx].br_flags;
        sim_assert(tbr_flags);
        tc_taken = (tbr_flags & TBF_Br_Taken) != 0;
        tc_nextpc = tcache_nextpc(ctx->tc.block, inst_idx, 0);
        tc_target = tcache_nextpc(ctx->tc.block, inst_idx, 1);
        if (tc_taken && SBF_ReadsRetStack(br_flags) && (ctx->tc.avail == 1)) {
            // For retstack reads at the end of a TC block, ignore target
            // info from the trace, so we'll use the retstack value
            // instead.
            tc_nextpc = 0;
            tc_target = 0;
        }
        skip_bpredict = (tbr_flags & TBF_SkipPredict) != 0;
        if (!skip_bpredict) {
            inst->tc.predict_num = ctx->tc.predicts_used;
            ctx->tc.predicts_used++;
            sim_assert(ctx->tc.predicts_used <= ctx->tc.block->pred_count);
        }
    }

    mem_addr predict_target;
    {
        if (is_from_tc) {
            // Always use the TC target, skip the BTB lookup for TC hits.
            predict_target = tc_target;
        } else if (will_commit) {
            // true for jmp/jsr, not br/bsr/retn/jsr_coroutine/conditionals
            int is_jump = !(br_flags & (SBF_Br_Cond | SBF_StaticTargDisp |
                                        SBF_RS_Pop | SBF_RS_PopPush));
            predict_target  = btblookup(ctx, ctx->pc, correct_target, 
                                        correct_taken, is_jump);
        } else {
            if (CONDBRANCH_WP_MAGIC_BTB && SBF_CondBranch(br_flags))
                predict_target = correct_target;
            else if (RETURN_WP_MAGIC_BTB && SBF_ReadsRetStack(br_flags))
                // This will be overwritten by the return-stack read; we just
                // want to skip the BTB lookup and pretend the jump was in 
                // the BTB
                predict_target = 1;
            else
                predict_target = get_btblookup(ctx, ctx->pc);
        }
    }

    int targ_present = (predict_target != 0);

    if (SBF_StaticTarget(br_flags)) {
        // If this is a branch with a hard-coded displacement, we'll know its
        // target as soon as it's decoded.
        predict_target = correct_target;
    } else if (SBF_ReadsRetStack(br_flags)) {
        // If this instruction does a return-stack pop, ignore the BTB target
        // and use the return stack instead.
        predict_target = (will_commit) ? rs_pop(ctx, correct_target) : 
            wp_rs_pop(ctx);
        // If the TC specified a target, do the pop, but use the TC value
        // instead, to ensure sequencing consistency with future insts in this
        // block.
        if (tc_target)
            predict_target = tc_target;
    }

    if (is_from_tc && !targ_present && (predict_target != 0)) {
        // For instructions which 1. are in the trace cache, 2. have no 
        // target information in the TC, and 3. have target information
        // otherwise available (apart from the BTB), mark the target 
        // information as being available this cycle since their presence in
        // the TC means we have all of the decode info already.
        targ_present = 1;
    }

    if (perfect_bp && !is_from_tc) {
        predict_target = correct_target;
        targ_present = 1;
    }

    if (SBF_WritesRetStack(br_flags))
        rs_push(ctx, ctx->pc + 4);

    int predict_taken;
    if (SBF_CondBranch(br_flags)) {
        int ghr_taken;
        if (tc_target) {
            predict_taken = tc_taken;
            // Update GHR on promoted branches?
            ghr_taken = predict_taken;
        } else if (will_commit) {
            predict_taken = bpredict(ctx, ctx->pc, correct_taken,
                                     ctx->ghr);
            ghr_taken = (CONDBRANCH_MISP_PSYCHIC_GHR) ? correct_taken :
                predict_taken;
        } else {
            predict_taken = get_bpredict(ctx, ctx->pc, ctx->ghr);
            ghr_taken = predict_taken;
        }

        if (perfect_bp && !is_from_tc) {
            predict_taken = correct_taken;
            ghr_taken = correct_taken;
        }

        inst->ghr = ctx->ghr;
        // roll_back_insts() may also update ctx->ghr
        ctx->ghr = (ctx->ghr << 1) | ghr_taken;
    } else {    
        sim_assert(!is_from_tc || tc_taken);    // If in TC, must be "taken"
        predict_taken = 1;
    } 

    if (!(targ_present | 
          ((BTBMISS_NT_PSYCHIC_BPRED) ? correct_taken : predict_taken))) {
        /* fuzzy area -- it is ok if non-taken branch misses in the btb */
        // thanks to Marios Kleanthous for pointing out that this
        // already-weird test was using "correct_taken" inappropriately
        targ_present = 1;
        predict_target = correct_target;
    }

    //
    // At this point, "predict_target" has the branch target as predicted by
    // the BTB, return stack, or encoded within the instruction itself.  It is
    // 0 if no target information is available.  "predict_taken" has the 
    // branch direction prediction for conditional branches, or 1 for
    // unconditionals.
    //
    // If "targ_present" is set, the values "predict_target" / "predict_taken"
    // are genuinely available right now; if not, this branch PC was not in 
    // the BTB, but the predictions will be available immediately upon decode.
    //

    int use_target_info = targ_present;
        
    if (will_commit || !IGNORE_WP_MISPREDICTS) {
        int target_mismatch = (predict_target != correct_target);
        int predict_mismatch = (predict_taken ^ correct_taken);

        // Now, compute "misfetch" and "mispredict" as a function of
        // targ_present, target_match, and predict_match.

        // If either the branch target or direction prediction we have is 
        // incorrect, then we're not going to figure it out until the branch
        // resolves; it's a full-on mispredict.
        int mispredict = target_mismatch | predict_mismatch;

        // If the prediction info is correct, but the branch isn't in the BTB,
        // then the info won't be available until this instruction hits
        // decode.  In that case, the simulator calls it a "misfetch".
        int misfetch = !(targ_present | mispredict);

        sim_assert(!(misfetch && mispredict));

#ifdef DEBUG
        if (targ_present && target_mismatch) {
            // This should only be possible for dynamic branches
            sim_assert(!SBF_StaticTarget(br_flags));
            // All dynamic branches on the Alpha are unconditional
            sim_assert(!SBF_CondBranch(br_flags));
            sim_assert(!predict_mismatch);
        }
#endif
        
        if (mispredict) {
            DEBUGPRINTF("T%d mispredicting%s\n", ctx->id,
                        (will_commit) ? "" : " (on wrong path)");
            inst->mispredict = (will_commit) ? MisPred_CorrectPath :
                MisPred_WrongPath;
            ctx->wrong_path = 1;
            if (CP_MISPREDICT_MAGIC_BTB)
                use_target_info = 1;
        } else if (misfetch) {
            DEBUGPRINTF("T%d misfetching%s\n", ctx->id,
                        (will_commit) ? "" : " (on wrong path)");
            sim_assert(!is_from_tc);
            inst->misfetch = (will_commit) ? MisPred_CorrectPath :
                MisPred_WrongPath;
            ctx->misfetching = 1;
            ctx->wrong_path = 1;
        }
    }

    // We're re-steering the AppState here, possibly onto a wrong path!
    if (tc_nextpc)
        ctx->as->npc = tc_nextpc;
    else
        ctx->as->npc = (use_target_info & predict_taken &
                       (predict_target != 0)) ? predict_target : (ctx->pc + 4);

    inst->taken_branch = ctx->emu_inst.taken_branch;
    inst->br_target = ctx->emu_inst.br_target;

    inst->taken_predict = predict_taken;
    inst->skipped_bpredict = skip_bpredict;
    inst->target_predict = (use_target_info) ? predict_target : 0;
}


// This is a weird version of sim_branch() which runs after some sync
// instructions; it's purpose is determining whether the previously simulated
// branches should be considered correct or not, based on the updated
// post-sync-commit program state.
//
// In addition to detecting incorrect branches, this also must ensure that the
// branch simulation state -- see the comments for sim_branch() -- is kept
// consistent for future undo_inst() calls.

void
resim_branch(context * restrict ctx, activelist * restrict inst)
{
    const StashBranchFlags br_flags = inst->br_flags;
    const int will_commit = !(ctx->wrong_path | ctx->follow_sync);

    if (inst->gen_flags & SGF_SysCall) 
        return;

    DEBUGPRINTF("resim_branch: T%ds%d\n", ctx->id, inst->id);

    if (will_commit && ((inst->taken_predict != ctx->emu_inst.taken_branch) ||
                        (inst->target_predict != ctx->emu_inst.br_target))) {
        DEBUGPRINTF("T%ds%d mispredicting (after sync)\n", ctx->id,
                    inst->id);

        if (!(inst->status & (FETCHED | EXECUTING))) {
            ctx->mispredict_discovered = inst;
        } else {
            // Don't bother with MisPred_WrongPath stuff for resim
            inst->mispredict = MisPred_CorrectPath;
        }
        ctx->wrong_path = 1;

        if (1) {
            // Prevent instructions following the incorrect branch from
            // committing, otherwise they'll commit immediately and be
            // invalidated before being cleaned up by fix_pcs().
            activelist * restrict nextinst = 
                &ctx->alist[alist_add(ctx, inst->id, 1)];
            // This isn't a good idea in general, since "donecycle" may be
            // reset as instructions complete this cycle.  That's alright for
            // this case, since we only need to keep it from committing in
            // this cycle, as fix_pcs() will take care of things before the
            // next.
            nextinst->donecycle = MAX_CYC;
        }
    }

    // Re-do the undone branch effects
    if (SBF_CondBranch(br_flags)) {
        inst->ghr = ctx->ghr;
        ctx->ghr = (ctx->ghr << 1) | inst->taken_predict;
    }
    if (SBF_ReadsRetStack(br_flags))
        (will_commit) ? rs_pop(ctx, ctx->emu_inst.br_target) : wp_rs_pop(ctx);
    if (SBF_WritesRetStack(br_flags))
        rs_push(ctx, inst->pc + 4);

    inst->taken_branch = ctx->emu_inst.taken_branch;
    inst->br_target = ctx->emu_inst.br_target;
}


static void
grow_waiter(activelist *inst)
{
    int old_size = inst->waiter_size;
    int new_size = (int) ceil(old_size * 1.5);
    activelist **new_waiter;
    long new_bytes = new_size * sizeof(new_waiter[0]);

    sim_assert(old_size > 0);
    sim_assert(new_size > old_size);
    
    DEBUGPRINTF("T%ds%i growing waiter[] arrays from %d entries to %d\n",
                inst->thread, inst->id, old_size, new_size);

    if (!(new_waiter = realloc(inst->waiter, new_bytes))) {
        fprintf(stderr, "%s (%s:%i): out of memory, growing T%ds%d "
                "waiter[] from %d to %d entries; needed %ld bytes.\n",
                __func__, __FILE__, __LINE__, inst->thread, inst->id, old_size,
                new_size, new_bytes);
        exit(1);
    }
    inst->waiter = new_waiter;
    inst->waiter_size = new_size;
    sim_assert(inst->numwaiting < inst->waiter_size);
}


//
// An input register value, for the purposes of dependent-scheduling, can be
// in one of three states:
//  1. Ready immediately in the register file.
//     (last_writer NONE, or last_writer's donecycle < cyc)
//  2. Produced by an in-flight instruction for which the completion time is
//     known.
//     (last_writer's donecycle >= cyc, != MAX_CYC)
//  3. Produced by an in-flight instruction with a not-yet-known completion 
//     time.
//     (last_writer's donecycle == MAX_CYC)
//
// For cases 1 and 2, we can just use the lasthazard value for the input reg;
// for case 3, we block this instruction and queue it for wakeup when the
// producer is resolved.  We return NULL for cases 1 & 2, and a
// pointer to the producer instruction for case 3.

activelist *
handle_src_dep(context * restrict ctx, activelist * restrict inst,
               int src_reg)
{
    int writer_id = ctx->last_writer[src_reg];
    activelist * restrict writer = &ctx->alist[writer_id];

    sim_assert((writer_id == NONE) || (writer->status != INVALID));
    sim_assert((writer_id == NONE) || (writer->dest == src_reg));

    if ((writer_id != NONE) && (writer->donecycle == MAX_CYC)) {
        // The value we need for src_reg is produced by an in-flight 
        // instruction that we don't know the completion time of (yet)
        sim_assert(writer_id != inst->id);
        inst->deps++;
        inst->status |= BLOCKED;
        if (writer->numwaiting >= writer->waiter_size) 
            grow_waiter(writer);        // adds at least 1 entry
        sim_assert(writer->numwaiting < writer->waiter_size);
        writer->waiter[writer->numwaiting] = inst;
        writer->numwaiting++;
        // inst->src[12]_waiting_for set by caller
        return writer;
    } else {
        if (ctx->lasthazard[src_reg] > inst->readycycle)
            inst->readycycle = ctx->lasthazard[src_reg];
        return NULL;
    }
}


void
handle_commit_group_fetch(context * restrict ctx, activelist *inst,
                          const TraceCacheInst *tc_inst)
{
    int overlap_prev_cgroup = -1;
    if (tc_inst) {
        if (tc_inst->cgroup_flags & TCGF_EndsGroup) {
            sim_assert(ctx->commit_group.fetching_leader != -1);
            sim_assert(!ctx->commit_group.fetching_last_inst);
            ctx->commit_group.fetching_last_inst = 1;
        }
        if (tc_inst->cgroup_flags & TCGF_StartsGroup) {
            if (tc_inst->cgroup_flags & TCGF_EndsGroup) 
                overlap_prev_cgroup = ctx->commit_group.fetching_leader;
            ctx->commit_group.fetching_leader = inst->id;
        }
    }

    const int cg_leader_id = (overlap_prev_cgroup >= 0) ? 
        overlap_prev_cgroup : ctx->commit_group.fetching_leader;
    inst->commit_group.leader_id = cg_leader_id;
    inst->commit_group.remaining = -1;  // Special meaning, see main.h

    if (ctx->commit_group.fetching_last_inst) {
        activelist *leader = &ctx->alist[cg_leader_id];
        sim_assert(leader->commit_group.remaining < 0);
        int group_size = alist_count(ctx, leader->id, inst->id);
        int completed = (-leader->commit_group.remaining) - 1;
        leader->commit_group.remaining = group_size - completed;
        DEBUGPRINTF("T%d commit group s%d...%d fetched, group size %d "
                    "completed %d remaining %d\n", ctx->id, 
                    cg_leader_id, inst->id, group_size, completed, 
                    leader->commit_group.remaining);
        // The fetching instruction cannot be done yet; this ensures that
        // at least one instruction will go to regwrite after the size is
        // known (with a positive "remaining", so commit will be enabled.
        sim_assert(leader->commit_group.remaining > 0);
        if (ctx->commit_group.fetching_leader == cg_leader_id)
            ctx->commit_group.fetching_leader = -1;
        ctx->commit_group.fetching_last_inst = 0;
    }
    if (inst->id == ctx->commit_group.fetching_leader) {
        DEBUGPRINTF("T%d starting new commit group s%d\n", ctx->id,
                    inst->id);
        inst->commit_group.overlap_next_leader = -1;
        if (overlap_prev_cgroup >= 0) {
            DEBUGPRINTF("T%d commit group s%d overlaps with previous "
                        "s%d\n", ctx->id, inst->id, 
                        overlap_prev_cgroup);
            activelist *prev_leader = &ctx->alist[cg_leader_id];
            sim_assert(prev_leader != inst);
            sim_assert(prev_leader->status != INVALID);
            sim_assert(prev_leader->commit_group.remaining != 0);
            sim_assert(prev_leader->commit_group.overlap_next_leader == -1);
            prev_leader->commit_group.overlap_next_leader = inst->id;
        }
        ctx->commit_group.started++;
        ctx->commit_group.in_flight++;
    }
}


/*  This routines collects all the static information about a particular
       instruction--it's dependencies, it's earliest execution time,
       it's control dependencies, it's branch behavior, and much more.
 */


static int
setup_instruction(context * restrict current, const StashData * restrict stash)
{
    CoreResources *core = current->core;
    int src1, src2;
    activelist *top;
    const TraceCacheInst *tc_inst;

    top = &current->alist[current->alisttop];

    if (current->tc.avail) {
        int tc_idx = tcache_instidx(current);
        current->as->npc = tcache_nextpc(current->tc.block, tc_idx, 0);
        tc_inst = &current->tc.block->insts[tc_idx];
    } else {
//      current->nextpc = current->pc + 4;      // emulate_inst() does this
        tc_inst = 0;
    }

/* Don't send nops through the pipeline */
/*   Note that this currently eliminates some sw prefetch instructions */
    if (current->params.discard_static_noops && 
        (stash->gen_flags & SGF_StaticNoop)) {
        current->emu_inst.srcmem = 0;   // not sure why this is here
        current->alisttop = (current->alisttop - 1) & 
            (current->params.active_list_size - 1);
        if (!current->misfetching) {
            // Don't count misfetches, because they never actually emulate
            current->noop_discard_run_len++;
            sim_assert(current->noop_discard_run_len > 0);
        }
        return 0;
    }

#ifdef PRIO_INST_COUNT
    core->sched.key[current->core_thread_id]++;
#else
#ifdef PRIO_BR_COUNT
    if (stash->branch || stash->cond_branch) {
        core->sched.key[current->core_thread_id]++;
    }
#endif
#endif

/* A misfetch is a BTB miss that will be fixed (and predicted
    correctly) when the branch hits the decode stage.  We treat
    instructions after a misfetch differently than instructions
    after a mispredict, because misfetched instructions never
    reach the execute, or even issue, stage.  Therefore, we
    don't really care what they are.
 */

    if (current->misfetching) {
        misfetchtotal++;
        current->num_misfetches++;
        current->alisttop = (current->alisttop - 1) & 
            (current->params.active_list_size - 1);
        return 0;
    }
   
    // Note: most of this code is parodied in inject_alloc() and
    // inject_inst_prep()
    top->fetchcycle = cyc;

    src1 = stash->src_a;
    src2 = stash->src_b;

    /* Copy stash into activelist */
    top->fu = stash->whichfu;
    top->src1 = src1;
    top->src2 = src2;
    top->dest = stash->dest;
    top->br_flags = stash->br_flags;
    top->gen_flags = stash->gen_flags;
    top->mem_flags = stash->mem_flags;
    top->syncop = stash->syncop;
    top->regaccs = stash->regaccs; 

    top->delay = current->fetching_inst_delay;
    top->srcmem = current->emu_inst.srcmem;
    top->destmem = current->emu_inst.destmem;
    current->emu_inst.srcmem = 0;
    current->emu_inst.destmem = 0;

    top->addrcycle = MAX_CYC;
    top->donecycle = MAX_CYC;
    top->iregs_used = 0;
    top->fregs_used = 0;
    top->lsqentry = 0;
    top->robentry = 0;
    top->status = FETCHED;
    top->mispredict = MisPred_None;
    top->misfetch = MisPred_None;
    top->deps = 0;
    top->numwaiting = 0;
    top->wait_sync = 0;
    top->thread = current->id;
    top->as = current->as;
    top->pc = current->pc;
    top->mb_epoch = current->core->mb.fetch_epoch;
    top->wmb_epoch = current->core->wmb.fetch_epoch;
    top->app_inst_num = current->as->stats.total_insts - 1;
    top->insts_discarded_before = current->noop_discard_run_len;
    current->noop_discard_run_len = 0;
    top->bmt.spillfill = BmtSF_None;

    // Transfer any outstanding stats from an I-cache simulation to this
    // instruction and clear them from the context; the I-cache stats will
    // then be carried by the first instruction of that fetch block.
    // (By this point in setup_instruction(), we know it's not a no-op.)
    top->icache_sim.service_level = current->icache_sim.service_level;
    current->icache_sim.service_level = 0;      // (SERVICED_NONE)
    top->icache_sim.was_merged = current->icache_sim.was_merged;
    top->icache_sim.latency = current->icache_sim.latency;

    top->dcache_sim.service_level = 0;          // (SERVICED_NONE)
    top->dcache_sim.was_merged = 0;

    top->wp = current->wrong_path;
    if (current->wrong_path) {
        current->stats.wpinstrs++;
    } else {
        current->stats.instrs++;
    }

    if (current->tc.avail) {
        top->tc.base_pc = current->tc.base_pc;
        top->tc.predict_num = -1;
    } else {
        top->tc.base_pc = 0;
    }

    handle_commit_group_fetch(current, top, tc_inst);

    /* handle branch prediction, BTB, etc. */
    if (top->br_flags)
        sim_branch(current, top);

    /* top->readycycle contains the latest arriving known dependence.
       When all dependencies are known, top->status & BLOCKED is clear. */
    /* current->last_writer[reg] holds the last instruction which wrote to
       reg.  If the writer has donecycle MAX_CYC, it's completion time is
       not yet known; once that instruction's completion time is known, the
       completion time is written in current->lasthazard[reg], but
       last_writer is NOT cleared until that inst leaves the machine. */

    top->readycycle = cyc;

    /* Handle SRC 1 dependencies (INT reg 31/FLOAT reg 63 tied to zero) */
    if (!IS_ZERO_REG(src1)) {
        top->src1_waitingfor = handle_src_dep(current, top, src1);
    } else {
        top->src1_waitingfor = NULL;
    }

    /* Handle SRC 2 dependencies (INT reg 31/FLOAT reg 63 tied to zero) */
    if (!IS_ZERO_REG(src2) && (src1 != src2)) {
        top->src2_waitingfor = handle_src_dep(current, top, src2);
    } else {
        top->src2_waitingfor = NULL;
    }

    if (!IS_ZERO_REG(top->dest))
        current->last_writer[top->dest] = current->alisttop;

    if (current->follow_sync) {
        if ((top->fu == INTLDST || top->fu == SYNCH))
            top->wait_sync = 1;
    }
    /* instructions following certain synch instructions must be handled
       differently, because they are truly speculative (that is,
       even the simulator doesn't know if they will commit or not.
    */
    if (current->nextsync == NONE && !current->wrong_path) {
        if (top->syncop == SMT_HW_LOCK || top->syncop == LDL_L
            || top->syncop == LDQ_L || top->syncop == STL_C
            || top->syncop == STQ_C ) {
            current->nextsync = top->id;
            current->follow_sync = 1;
            current->sync_store_value = current->as->R[top->src1].i;

            DEBUGPRINTF("T%d in follow_sync mode\n", current->id);
            current->sync_restore_point = top->id;
        }
    }

    if (top->syncop == MB)
        current->core->mb.fetch_epoch++;
    else if (top->syncop == WMB)
        current->core->wmb.fetch_epoch++;

    return 1;
}


/* The key[] structure is changed at various places in the code.
   This mechanism implements the ICOUNT fetch mechanism from 
   the ISCA96 smt paper, if PRIO_INST_COUNT defined.
   */

void calculate_priority()
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        const int * restrict key = core->sched.key;
        int * restrict priority = core->sched.priority;
        
        int changing = 1, j;
        for (j=1;j<core->n_contexts && changing;j++) {
            int i;
            changing = 0;
            for (i=0;i<core->n_contexts-j;i++) {
                if (key[priority[i]] > key[priority[i+1]]) {
                    int temp;
                    changing = 1;
                    temp = priority[i];
                    priority[i] = priority[i+1];
                    priority[i+1] = temp;
                }
            }
        }
    }
}


static int
tcache_fetch(CoreResources *core, context *ctx)
{
    TraceCache *tcache = core->tcache;
    MultiBPredict *multi_bp = core->multi_bp;
    mem_addr pc = ctx->as->npc;
    TraceCacheLookupInfo li;
    int hit;

    sim_assert(ctx->tc.avail == 0);

    li.pc = pc;
    li.thread_id = ctx->as->app_master_id;
    li.predict_bits = mbp_predict(multi_bp, pc, ctx->ghr);
    DEBUGPRINTF("MBP(%s, %s) -> %x\n", fmt_x64(pc), fmt_x64(ctx->ghr),
                li.predict_bits);

    hit = tc_lookup(tcache, &li, ctx->tc.block);
    if (hit) {
        ctx->tc.avail = ctx->tc.block->inst_count;
        ctx->tc.base_pc = pc;
        ctx->tc.predicts_used = 0;
        sim_assert(ctx->tc.avail > 0);
    }
    
    return hit;
}


static void
abort_thread_fetch(context *ctx, u64 old_pc)
{
    ctx->pc = old_pc;
    ctx->alisttop = alist_add(ctx, ctx->alisttop, -1);
}


/*  Simulates the fetching of blocks of instructions. */
static void
fetch_for_core(CoreResources *core)
{
    const int single_limit = core->params.fetch.single_limit;
    const int total_limit = core->params.fetch.total_limit;
    const int thread_count_limit = core->params.fetch.thread_count_limit;
    const int use_trace_cache = core->params.fetch.enable_trace_cache;
    const int tc_skip_to_rename = core->params.fetch.tcache_skips_to_rename;
    const int fetch_stage_cyc = core->params.fetch.n_stages;
    int threadsfetched = 0;
    int totalfetch = 0;
    int imemdelay;
    context *ctx;
    int tc_rename_bypass_clear;

    // Shift instructions from fetch2...N to the next stage 
    // (fetch3...N, decode1), if clear.
    for (int src_stage = core->stage.decode1 - 1; src_stage >= 0;
         src_stage--) {
        if (stageq_count(core->stage.s[src_stage + 1]) == 0) {
            stageq_assign(core->stage.s[src_stage + 1], 
                          core->stage.s[src_stage]);
        } else {
            DEBUGPRINTF("C%i: fetch%i backed up\n", core->core_id,
                        src_stage + 2);
        }
    }

    // If fetch2 (or decode1 if there is no fetch2) is still occupied, skip
    // fetch for this cycle
    if (stageq_count(core->stage.s[0]) != 0) {
        DEBUGPRINTF("C%i: fetch1 backed up\n", core->core_id);
        return;
    }

    if (tc_skip_to_rename) {
        int ren1 = core->stage.rename1;
        int stage;
        for (stage = 0; stage <= ren1; stage++)
            if (stageq_count(core->stage.s[stage]) != 0)
                break;
        tc_rename_bypass_clear = stage > ren1;
    } else {
        tc_rename_bypass_clear = 0;
    }

    while((threadsfetched++ < thread_count_limit) && 
          (ctx = get_next_thread(core))) {
        /* here I'm assuming that an I fetch miss is buffered for future
           access, making the next fetch succeed without an I cache access.
           This is slightly optimistic, but avoids some race conditions.
        */
        // We only enter this loop body on a context if we're 1) starting a
        // new fetch operation, or 2) resuming fetch after a prior fetch stall
        // has completed; we use the stalled_for_prior_fetch flag to
        // distinguish.
        if (!ctx->stalled_for_prior_fetch) {
            // note when we're starting a new fetch attempt
            ctx->last_fetch_begin = cyc;
        }
        if (ctx->stalled_for_prior_fetch) {
            // arriving here implies the prior fetch stall has completed; as
            // mentioned above, we assume that the fill data is routed to
            // satisfy this miss as well, so we don't need to repeat the
            // access attempt.
            ctx->stalled_for_prior_fetch = 0;
        } else if (ctx->tc.avail || 
                   (use_trace_cache && tcache_fetch(core, ctx))) {
            // Hit in trace cache
        } else {
            imemdelay = doiaccess((mem_addr) ctx->as->npc, ctx);
            if (imemdelay == MEMDELAY_LONG) {
                /* We don't actually know the delay yet, until the access
                   goes through the memory subsystem */
                DEBUGPRINTF("T%d icache or itlb delay\n", ctx->id);
                ctx->stalled_for_prior_fetch = 1;
            } else {
                // On an I-cache hit, the data will still take one I-cache
                // access time to become ready.  If it will be ready in time
                // for decode, we're actually reading it right now, and
                // there's no need to stall.  ctx->fetchcycle is moved ahead
                // for the next I access.
                if (imemdelay > fetch_stage_cyc) {
                    DEBUGPRINTF("T%d icache delay %d cycles\n", ctx->id,
                                imemdelay);
                    ctx->stalled_for_prior_fetch = 1;
                }
            }
            if (ctx->stalled_for_prior_fetch) {
                // stop fetching on this context, in this cycle
                continue;
            }
        }

        ctx->fthiscycle = 0;

        while((totalfetch < total_limit) && 
              (ctx->fthiscycle < single_limit) &&
              get_next_instruction(ctx))
        {
            const u64 old_pc = ctx->pc;
            ctx->pc = ctx->as->npc;             // PC of this fetch
            const u64 pc = ctx->pc;             // (copy for convenience)

            if (ctx->tc.avail) {
#ifdef DEBUG          
                const TraceCacheInst *tci = 
                    &ctx->tc.block->insts[tcache_instidx(ctx)];
                if (pc != tci->pc) {
                    fflush(0);
                    fprintf(stderr, "T%is%i cyc %s: oh no, ctx->pc "
                            "%s != tci->pc %s!\n", ctx->id, 
                            ctx->alisttop, fmt_i64(cyc),
                            fmt_x64(pc), fmt_x64(tci->pc));
                }
#endif
                sim_assert(pc == tci->pc);
                if (tc_skip_to_rename && !tc_rename_bypass_clear) {
                    DEBUGPRINTF("C%i: fetch->rename1 backed up for T%i trace "
                                "bypass\n", core->core_id, ctx->id);
                    abort_thread_fetch(ctx, old_pc);
                    break;
                }
            }

            const StashData * restrict stash = 
                stash_decode_inst(ctx->as->stash, pc);

            if (!stash) {
                if (context_alist_used(ctx) == 1) {
                    err_printf("T%ds%d/A%d cyc %s: oh snap, invalid "
                               "fetch PC %s on an empty pipe!\n",
                               ctx->id, ctx->alisttop, ctx->as->app_id,
                               fmt_i64(cyc), fmt_x64(pc));
                    fprintf(stderr, "ProgMem map:\n");
                    pmem_dump_map(ctx->as->pmem, stderr, "  ");
                    sim_abort();
                } else {
                    if (pc != 1) {
                        DEBUGPRINTF("T%ds%d/A%d cyc %s: invalid "
                                    "fetch PC %s, draining\n",
                                    ctx->id, ctx->alisttop,
                                    ctx->as->app_id, fmt_i64(cyc),
                                    fmt_x64(pc));
                        // Force to zero: wait for either misspec cleanup
                        // to fix nextpc, or the pipe to empty out (error)
                        ctx->as->npc = 1;
                    }
                    ctx->draining = 1;
                    abort_thread_fetch(ctx, old_pc);
                    break;
                }
            }

            if (stash->gen_flags & SGF_PipeExclusive) {
                /*
                 * We've clairvoyantly determined that the next instruction
                 * (not fetched yet) requires exclusive access to the
                 * pipeline.  To ensure this, we'll delay fetching/emulation
                 * until the pipeline drains.  This isn't totally accurate,
                 * but it's not something we do often.
                 */
                if (context_alist_used(ctx) == 1) {
                    DEBUGPRINTF("T%is%i exclusive access granted\n",
                                ctx->id, ctx->alisttop);
                    // Set draining flag again, so that the next fetch waits
                    // for the exclusive inst to commit.
                    ctx->draining = 1; 
                } else {
                    DEBUGPRINTF("T%i blocking for exclusive access\n",
                                ctx->id);
                    ctx->draining = 1; 
                    abort_thread_fetch(ctx, old_pc);
                    break;
                }
            }

            SmtDISASSEMBLE(ctx->id, ctx->alisttop, pc, 1);

            /* instructions are emulated in the fetch stage instead of,
               for example, the execute stage, just because it is easier
               to do it in an in-order part of the pipeline. It also allows
               us to do some nice things with oracle knowledge of branch
               behavior and things like that. */
            if (!ctx->misfetching) {
                // Updates ctx->as->npc
                emulate_inst_for_sim(ctx, stash, ctx->alisttop);
            } else {
                // Forcefully move "next PC" ahead during misfetching
                // (Usually emulate_inst_for_sim() updates it.)
                ctx->as->npc += 4;
            }
            // setup_instruction sets ctx->nextpc, returns 0 for a no-op
            if (setup_instruction(ctx, stash)) {
                activelist *inst = &ctx->alist[ctx->alisttop];
                if (ctx->tc.avail && tc_skip_to_rename) {
                    stageq_enqueue(core->stage.s[core->stage.rename1], inst);
                } else {
                    stageq_enqueue(core->stage.s[0], inst);
                }
            }

            if (ctx->tc.avail > 0) {
                ctx->tc.avail--;
                if (!ctx->tc.avail)
                    break;
            }
            totalfetch++;
            ctx->fthiscycle++;
        }
    }
    core->sched.priorityslot = 0;
}


void
fetch(void)
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        fetch_for_core(core);
    }
}
