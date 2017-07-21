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

Significant parts of the SMTSIM simulator were written by Jack Lo.
Therefore the following copyrights may also apply:

Copyright (C) Jack Lo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "main.h"
#include "mem.h"
#include "cache.h"
#include "smt.h"
#include "inst.h"
#include "core-resources.h"
#include "dyn-inst.h"
#include "context.h"
#include "callback-queue.h"
#include "app-state.h"
#include "stash.h"
#include "sim-params.h"
#include "app-mgr.h"
#include "sim-cfg.h"
#include "app-stats-log.h"
#include "adapt-mgr.h"


i64 lock_fail=0, lock_succeed=0;
i64 spec_rel_success=0, spec_rel_fail=0;

/* Because we don't want to recover from mispredicts and other such
   things until AFTER the cycle, we just record the event in these
   structures and do the recovery later */

i64 flushed=0, execflushed=0;

void
initsched()
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        int i;
        for (i=0;i<core->n_contexts;i++) {
            core->sched.priority[i] = i;
            core->sched.key[i] = 0;
        }
    }
}


// Calculate the (correct-path) correct next PC of a given instruction. 
static inline mem_addr 
calc_correct_nextpc(const activelist * restrict inst)
{
    return (inst->br_flags && inst->taken_branch) ? inst->br_target :
        (inst->pc + 4);
}


static void
reset_nextpc(context * restrict ctx, mem_addr next_pc, int right_path_for_sure)
{
    ctx->as->npc = next_pc;
    if (ctx->draining) {
        DEBUGPRINTF("T%d draining aborted due to fetch reset\n", ctx->id);
        ctx->draining = 0;
    }
    if (ctx->tc.avail) {
        DEBUGPRINTF("T%d discarding open TC block for fetch reset\n", ctx->id);
        ctx->tc.avail = 0;
    }
    if (ctx->imiss_cache_entry) {
        DEBUGPRINTF("T%d cancelling I-miss for fetch reset\n", ctx->id);
        clean_cache_queue_mispredict(ctx);
        ctx->stalled_for_prior_fetch = 0;        // don't skip doiaccess()
    }
    if (right_path_for_sure)
        ctx->wrong_path = ctx->misfetching = MisPred_None;
}


static void clean_up_misfetch(activelist * restrict brinst)
{
  context * restrict current = Contexts[brinst->thread];
  sim_assert((brinst->misfetch == MisPred_CorrectPath) ||
         (brinst->misfetch == MisPred_WrongPath));
  reset_nextpc(current, calc_correct_nextpc(brinst),
               (brinst->misfetch == MisPred_CorrectPath));
  current->misfetching = 0;
/* INVESTIGATE and fix -- only right for prio_inst_count */
  current->core->sched.key[current->core_thread_id] -= current->num_misfetches;
  current->fetchcycle = cyc;
  current->stalled_for_prior_fetch = 0;
  current->num_misfetches = 0;

  if (0) {
      // If you make use of commit-group speculation, make sure that you
      // handle misfetches of your speculative code here (as well as call
      // cleanup_commit_group()).
      cleanup_commit_group(current, -1);
  }
}


// Undo the architecturally-visible changes made by this instruction, as
// well as the speculatively updated branch predictor stuff.  This is the
// complement to save_instundo_info(); it's in this file so that it at least
// has a chance of being inlined where it's used.
static void
undo_inst(context * restrict ctx, activelist * restrict inst)
{
    inst_undo_info *undo = &inst->undo;
    int destreg = inst->dest;

#define DEBUG_REGS_UNDO 0

    // Reverse the effect of emulating the given instruction; this
    // restores the state saved by save_instundo_info().

    sim_assert(inst->status != INVALID);
    sim_assert(!(inst->gen_flags & SGF_PipeExclusive));
    sim_assert(!inst->bmt.spillfill);

    // Can't undo a correct-path inst unless we're checkpointing them
    if (!CHECKPOINT_CP_INSTS)
        sim_assert(inst->wp || ctx->follow_sync);
    // Can't undo a correct-path store unless we're checkpointing them;
    // (wrong-path stores are skipped if we're not checkpointing)
    if (!CHECKPOINT_STORES)
        sim_assert(inst->wp || ctx->follow_sync ||
                   !(inst->mem_flags & SMF_Write));

    sim_assert(!undo->undone);
    undo->undone = 1;

    if (DEBUG_REGS_UNDO) {
        DEBUGPRINTF("undo T%ds%d, r%d: %s -> %s", ctx->id, inst->id,
                    destreg, fmt_x64(ctx->as->R[destreg].i),
                    fmt_x64(undo->dest_reg_val));
    }
    ctx->as->R[destreg].i = undo->dest_reg_val;

    if (CHECKPOINT_STORES && (inst->mem_flags & SMF_Write) &&
        !(inst->gen_flags & SGF_SyncAtCommit)) {
        // Checkpoint store: this seems all well and good, except for
        // STL_C / STQ_C instructions, in which case we've got trouble.
        // If the "uni" flavors are in-use, they write their outputs when
        // emulated, so undo can proceed as usual.  If the non-"uni" flavors
        // are in-use, SGF_SyncAtCommit will be set and the outputs are not
        // written until commit/resim, so we shouldn't touch memory here.
        int mem_width = SMF_GetWidth(inst->mem_flags);
        mem_addr eff_addr = inst->destmem;
        pmem_write_n(ctx->as->pmem, mem_width, eff_addr, undo->dest_mem_val,
                     PMAF_NoExcept);
        if (DEBUG_REGS_UNDO) {
            DEBUGPRINTF(", destmem %s/%d -> %s", fmt_x64(inst->destmem), 
                        mem_width, fmt_x64(undo->dest_mem_val));
        }
    }

    if (DEBUG_REGS_UNDO) {
        DEBUGPRINTF("\n");
    }

    // Restore old last_writer, unless it has left the machine since we
    // saved it.
    if (ctx->last_writer[destreg] == inst->id) {
        const activelist *writer_inst = &ctx->alist[undo->dest_lastwriter];
        // Don't use the saved last_writer if it has left the machine: if
        // it's currently INVALID (committed/flushed) or if it's valid but
        // fetched later than the undo inst (meaning it's been
        // commited/flushed and then the activelist entry re-used since).
        if ((undo->dest_lastwriter != NONE) && 
            (writer_inst->status != INVALID) &&
            (writer_inst->fetchcycle <= inst->fetchcycle)) {
            sim_assert(writer_inst->dest == destreg);
            ctx->lasthazard[destreg] = writer_inst->donecycle;
            ctx->last_writer[destreg] = undo->dest_lastwriter;
        } else {
            ctx->lasthazard[destreg] = 0;
            ctx->last_writer[destreg] = NONE;
        }
    }

    if (SBF_UsesRetStack(inst->br_flags)) {
        if (inst->br_flags & SBF_RS_Push) {
            sim_assert(ctx->rs_size > 0);
            if (undo->retstack_val) {
                // retstack was full at push; restore saved bottom, don't
                // change size
                sim_assert(ctx->rs_size == ctx->params.retstack_entries);
                ctx->rs_start--;
                if (ctx->rs_start < 0)
                    ctx->rs_start = ctx->params.retstack_entries - 1;
                ctx->return_stack[ctx->rs_start] = undo->retstack_val;
            } else {
                ctx->rs_size--;
            }
        } else if (inst->br_flags & SBF_RS_Pop) {
            if (undo->retstack_val) {
                // push the popped value back onto stack
                int idx = ctx->rs_size + ctx->rs_start;
                if (idx >= ctx->params.retstack_entries)
                    idx -= ctx->params.retstack_entries;
                sim_assert(ctx->rs_size < ctx->params.retstack_entries);
                ctx->return_stack[idx] = undo->retstack_val;
                ctx->rs_size++;
            } else {
                // retstack was empty at pop
                sim_assert(ctx->rs_size == 0);
            }
        } else {
            sim_assert(inst->br_flags & SBF_RS_PopPush);
            sim_assert(ctx->rs_size > 0);
            if (undo->retstack_val) {
                // replace top-of-stack with original value
                int idx = ctx->rs_size - 1 + ctx->rs_start;
                if (idx >= ctx->params.retstack_entries)
                    idx -= ctx->params.retstack_entries;
                ctx->return_stack[idx] = undo->retstack_val;
            } else {
                // retstack was empty at pop-push; just undo the push
                sim_assert(ctx->rs_size == 1);
                ctx->rs_size--;
            }
        }
    }

    if (SBF_CondBranch(inst->br_flags)) {
        // We need to reverse the effects of conditional branches on the
        // thread's GHR.  "inst->ghr" was copied from the thread GHR
        // before any update.
        //
        // For normal emulation, the thread GHR has the branch outcome
        // shifted in on the right; however, when restore_from_sync_cp()
        // emulates branches, it does not always update the thread GHR.
        // In both cases, to recover the GHR as it was before updating, we
        // can just use the saved copy in "inst->ghr".
        ctx->ghr = inst->ghr;
#ifdef DEBUG    
        sim_assert(ctx->ghr == undo->undo_ghr);
#endif
    }
}


// Clean up after a squashed instruction, restoring the non-architecturally-
// visible state to free up resources for future instructions, etc.  Optionally
// updates "flushed" stats; the branch mispredict recovery code doesn't update
// these, since wrong-path insts are already accounted for seperately.
static void
cleanup_deadinst(CoreResources * restrict core, context * restrict ctx,
                 activelist * restrict inst, int update_flushed)
{
    sim_assert(!(inst->status & (INVALID | SQUASHED)));
    core->i_registers_freed += inst->iregs_used;
    core->f_registers_freed += inst->fregs_used;
    core->lsq_freed += inst->lsqentry;
    ctx->lsq_freed += inst->lsqentry; // We maintain this to keep stats per ctx
    ctx->rob_freed_this_cyc += inst->robentry;
    update_acc_occ_per_inst(ctx, inst, 1, 2); // For stats
    update_adapt_mgr_dec_tentative(ctx, ROB, inst->robentry);
    update_adapt_mgr_dec_tentative(ctx, LSQ, inst->lsqentry); 
    update_adapt_mgr_dec_tentative(ctx, IREG, inst->iregs_used); 
    update_adapt_mgr_dec_tentative(ctx, FREG, inst->fregs_used); 
    inst->robentry = 0;
    inst->deps = 0;
    inst->numwaiting = 0;
    inst->donecycle = cyc;
    inst->iregs_used = 0;
    inst->fregs_used = 0;
    inst->lsqentry = 0;
    inst->regaccs = 0;

    {
        int is_key = 0;
#if defined(PRIO_INST_COUNT)
        is_key = inst->status & FETCHED;
#elif defined(PRIO_BR_COUNT)
        is_key = (inst->status & FETCHED) && inst->br_flags;
#endif
        if (is_key)
            core->sched.key[ctx->core_thread_id]--;
    }

    if (update_flushed && !inst->wp) {
        if (!(inst->status & FETCHED))
            execflushed++;
        flushed++;
        ctx->stats.instrs--;
    }

    inst->status = SQUASHED;

    if (inst->commit_group.leader_id == inst->id) {
        ctx->commit_group.squashed++;
        sim_assert(ctx->commit_group.in_flight > 0);
        ctx->commit_group.in_flight--;
    }
    inst->commit_group.leader_id = -1;
}


// Rolls back instruction emulation state from "last_bad_inst" back to
// "last_good_inst".  If last_bad_inst == last_good_inst, no insts are flushed,
// though some corner-case changes may be made.  (Use roll_back_allinsts()
// to handle the specific full-pipeline flush case.)
//
// Note that "last_bad_inst" should almost always be "ctx->alisttop".
static void
roll_back_insts(context * restrict ctx, 
                int last_bad_id, int last_good_id,
                int deadinst_cleanup, int deadinst_update_flushed)
{
    CoreResources * restrict core = ctx->core;
    DEBUGPRINTF("%s: T%d s%d back to last good inst s%d, deadinst %d/%d\n",
                __func__, ctx->id, last_bad_id, last_good_id,
                deadinst_cleanup, deadinst_update_flushed);

    int wrap_mask = ctx->params.active_list_size - 1;
    for (int inst_id = last_bad_id; inst_id != last_good_id;
         inst_id = (inst_id - 1) & wrap_mask) {
        activelist * restrict inst = &ctx->alist[inst_id];
        if (!inst->undo.undone) {
            undo_inst(ctx, inst);
            if (deadinst_cleanup)
                cleanup_deadinst(core, ctx, inst, deadinst_update_flushed);
            ctx->as->stats.total_insts -= 1 + inst->insts_discarded_before;
            sim_assert(ctx->as->stats.total_insts >= 0);
        }
    }

    const activelist * restrict last_good_inst = &ctx->alist[last_good_id];

    if (1) {
        if ((last_good_inst->status != INVALID) &&
            SBF_CondBranch(last_good_inst->br_flags)) {
            // Special case: oldest good inst is a branch which may have
            // damaged the GHR, but which isn't being undone; we'll
            // fix the GHR ourselves.
            ctx->ghr = (last_good_inst->ghr << 1) |
                last_good_inst->taken_branch;
        }
        if (last_bad_id == ctx->alisttop) {
            // Special case: noops may have been discarded after the most
            // recently fetched inst, so no in-flight inst accounts for them.
            ctx->as->stats.total_insts -= ctx->noop_discard_run_len;
            ctx->noop_discard_run_len = 0;
            sim_assert(ctx->as->stats.total_insts >= 0);
        }
    }
}


// Roll back all instructions in a full window: roll_back_insts doesn't
// do this directly, as it always leaves one instruction untouched.
static void
roll_back_allinsts(context * restrict ctx,
                   int deadinst_cleanup, int deadinst_update_flushed)
{
    DEBUGPRINTF("%s: deadinst %d/%d\n", __func__,
                deadinst_cleanup, deadinst_update_flushed);
    sim_assert(context_alist_used(ctx) == ctx->params.active_list_size);
    int second_id = alist_add(ctx, ctx->alisttop, -1);
    // Roll back just one instruction, at alisttop
    roll_back_insts(ctx, ctx->alisttop, second_id, deadinst_cleanup,
                    deadinst_update_flushed);
    // Now, roll back the remaining instructions, wrapping back around to
    // alisttop
    roll_back_insts(ctx, second_id, ctx->alisttop, deadinst_cleanup,
                    deadinst_update_flushed);
}


// Clean up after a commit group that has had its speculative conditions
// violated or which was started by mistake.  If "misspec_leader_id" is
// non-negative, squash intructions back to the start of the group, reset the
// fetch PC, and cancel any fetching commit group (which may not be the same
// as the one violated).  The first instruction of this group isn't squashed;
// it's the "anchor" from which we figure out where to restart execution.  If
// "misspec_leader_id" is negative, group entry was misspeculated so the group
// will already be squashed; just prevent future fetching.

void
cleanup_commit_group(struct context *ctx, int misspec_leader_id)
{
    DEBUGPRINTF("T%d commit group s%d cleanup\n", ctx->id, misspec_leader_id);
    if (misspec_leader_id >= 0) {
        activelist *leader = &ctx->alist[misspec_leader_id];
        sim_assert(leader->status != SQUASHED);
        roll_back_insts(ctx, ctx->alisttop, misspec_leader_id, 1, 1);
        if (leader->commit_group.leader_id == misspec_leader_id) {
            leader->commit_group.leader_id = -1;
        } else {
            // "leader" overlaps with the previous commit group; unlink it
            int prev_leader_id = leader->commit_group.leader_id;
            sim_assert(prev_leader_id >= 0);
            activelist *prev_leader = &ctx->alist[prev_leader_id];
            sim_assert(prev_leader->commit_group.overlap_next_leader ==
                   misspec_leader_id);
            prev_leader->commit_group.overlap_next_leader = -1;
        }
        reset_nextpc(ctx, calc_correct_nextpc(leader), 1);
        DEBUGPRINTF("T%d commit group s%d: nextpc %s->%s (leader succ)\n",
                    ctx->id, misspec_leader_id, fmt_x64(leader->pc),
                    fmt_x64(ctx->as->npc));
        sim_assert(ctx->commit_group.in_flight > 0);
        ctx->commit_group.in_flight--;
        ctx->commit_group.aborted++;
    }
    ctx->commit_group.fetching_leader = -1;
    ctx->commit_group.fetching_last_inst = 0;
}


// Recover the value of register "reg_num" in context "ctx", from the point of
// view of the dynamic instruction ctx->alist[inst_id].  If "after_not_before"
// is set, use the value from after "inst_id" emulated, otherwise use the
// value from before that emulation.  (That flag only makes a difference if
// the subject instruction writes to reg_num.)
//
// This currently doesn't support operations on contexts which have active
// instructions from more than one AppState, including those with BMT
// spills/fills in flight.
//
// "inst_id" must be a valid, non-squashed instruction, as must all younger
// instructions on the context.
//
// (Inefficient linear scan.)
u64 recover_old_regval(const struct context *ctx, int inst_id, int reg_num,
                       int after_not_before)
{
    u64 result;
    const activelist * subj_inst = &ctx->alist[inst_id];
    sim_assert(IDX_OK(inst_id, ctx->params.active_list_size));
    sim_assert(IDX_OK(reg_num, MAXREG));
    sim_assert((subj_inst->as != NULL) && (ctx->as != NULL) &&
               (subj_inst->as == ctx->as));
    sim_assert(!(subj_inst->status & (INVALID | SQUASHED)));
    sim_assert(!subj_inst->undo.undone);
    assert_ifthen(!IS_ZERO_REG(reg_num) && (subj_inst->dest == reg_num),
                  ctx->last_writer[reg_num] != NONE);
    if (IS_ZERO_REG(reg_num)) {
        result = 0;
    } else if ((ctx->last_writer[reg_num] == NONE) ||
               ((ctx->last_writer[reg_num] == inst_id) && after_not_before)) {
        // Either the current value in the regfile is unchanged since before
        // this instruction (NONE case), or this instruction performed the most
        // recent change, and we want the result from _after_ that change.
        // In these cases, the register file holds the value we're after.
        result = ctx->as->R[reg_num].u;
    } else if ((subj_inst->dest == reg_num) && !after_not_before) {
        // We want the value of the output register of this instruction,
        // from just before emulation; that's stored in our undo info.
        result = subj_inst->undo.dest_reg_val;
    } else {
        // A younger instruction has overwritten the subject register;
        // we'll search in-flight instructions and extract the value from
        // that instruction's undo info.
        result = 0;     // (stop warning)
        for (int walk_idx = alist_add(ctx, inst_id, 1);
             1; walk_idx = alist_add(ctx, walk_idx, 1)) {
            const activelist * restrict walk_inst = &ctx->alist[walk_idx];
            sim_assert(walk_inst->as == ctx->as);
            sim_assert(!(walk_inst->status & (INVALID | SQUASHED)));
            sim_assert(!walk_inst->undo.undone);
            if (walk_inst->dest == reg_num) {
                result = walk_inst->undo.dest_reg_val;
                break;
            }
            // search for writer failed; shouldn't happen!
            sim_assert(walk_idx != ctx->alisttop);
        }
    }
    return result;
}


// This seems unnecessary at first glance: ctx->next_to_commit is the oldest
// instruction, and if it's squashed, all younger instructions ought to be
// as well.  However, depending on how commit() is configured, there may
// be younger instructions (fetched after the squash of next_to_commit),
// and commit() just hasn't finished reaping through the squashed entries yet.
// Returns -1 for "none found".
static int
oldest_notsquashed(const context * restrict ctx)
{
    int idx = ctx->next_to_commit;
    while (ctx->alist[idx].status & SQUASHED) {
        if (idx == ctx->alisttop) {
            idx = -1;
            break;
        }
        idx = alist_add(ctx, idx, 1);
        // we shouldn't have made it back to next_to_commit without passing
        // by alisttop
        sim_assert(idx != ctx->next_to_commit);
    }
    return idx;
}


// Like oldest_notsquashed(), but searches a suffix of the instruction window
// for a non-BmtSpillFill instruction, starting at a given point.  Returns -1
// for "none found".
static int
next_not_bmtspillfill(const context * restrict ctx, int first_to_test)
{
    int idx = first_to_test;
    while (ctx->alist[idx].bmt.spillfill) {
        if (idx == ctx->alisttop) {
            idx = -1;
            break;
        }
        idx = alist_add(ctx, idx, 1);
        // we shouldn't have made it back to next_to_commit without passing
        // by alisttop
        sim_assert(idx != ctx->next_to_commit);
    }
    return idx;
}


static void
flush_for_halt(struct context * restrict ctx)
{
    int oldest_good = oldest_notsquashed(ctx);
    int alist_used = context_alist_used(ctx);
    int flush_to;                       // -1: no flush
    mem_addr restart_pc = 0;
    int discarded_insts_skipped_by_restart = 0;

    sim_assert((ctx->halting == CtxHalt_FullSignaled) ||
               (ctx->halting == CtxHalt_FastSignaled));

    if ((alist_used == 0) || (oldest_good < 0)) {
        // Special case: this thread has no insts in the pipeline (most likely
        // due to an I-miss), so its current AppState next PC is correct.
        flush_to = -1;
        restart_pc = ctx->as->npc;
        DEBUGPRINTF("T%d/A%d flush_for_halt special case: empty or "
                    "all-squashed window\n", ctx->id, ctx->as->app_id);
    } else if (ctx->alist[oldest_good].gen_flags & SGF_PipeExclusive) {
        // Special case: pipe-exclusive instructions (e.g., syscalls) cannot
        // be undone.  So, we'll restart execution right after them.  Since
        // they must be alone in the pipeline, no further instructions from
        // the app have emulated, so the AppState's next PC indicates the
        // successor.  Finally, we'll force a full-reset context halt, to
        // maintain pipe-exclusiveness.
        sim_assert(alist_used == 1);
        flush_to = -1;
        restart_pc = ctx->as->npc;
        if (ctx->halting == CtxHalt_FastSignaled)
            ctx->halting = CtxHalt_FullSignaled;
        DEBUGPRINTF("T%d/A%d flush_for_halt special case: "
                    "PipeExclusive inst, forcing full reset, "
                    "not flushing s%d\n", ctx->id, ctx->as->app_id,
                    ctx->next_to_commit);
    } else if (ctx->alist[oldest_good].bmt.spillfill != 0) {
        // Special case: attemping to flush a window containing BMT spill/fill
        // instructions, which cannot be undone.  At the moment, this cannot
        // happen with spill instructions, since they are not injected until
        // after halt-signal processing has flushed the pipeline (or it has
        // been allowed to drain naturally) so we only consider fill
        // instructions.  Fill instructions are currently injected before any
        // regular instructions are fetched, so iff any fill instructions are
        // present, the pipeline will contain >0 fill instructions followed by
        // 0 or more regular instructions.
        int first_non_bmt = next_not_bmtspillfill(ctx, oldest_good);
        sim_assert(!(ctx->alist[oldest_good].bmt.spillfill & BmtSF_Spill));
        if (first_non_bmt >= 0) {
            flush_to = alist_add(ctx, first_non_bmt, -1);
            restart_pc = ctx->alist[first_non_bmt].pc;
            sim_assert(!(ctx->alist[flush_to].bmt.spillfill & BmtSF_Spill));
            // (copied from default flush case, below)
            discarded_insts_skipped_by_restart =
                ctx->alist[first_non_bmt].insts_discarded_before;
        } else {
            flush_to = -1;
            restart_pc = ctx->as->npc;
        }
        if (debug) {
            printf("T%d/A%d flush_for_halt special case: window has BMT "
                   "spill/fill insts", ctx->id, ctx->as->app_id);
            if (flush_to >= 0) {
                printf("; flushing normal insts");
            } else {
                printf(" only; no flush");
            }
            if (ctx->halting == CtxHalt_FastSignaled)
                printf(", promoting to Full halt");
            printf("\n");
        }
        if (ctx->halting == CtxHalt_FastSignaled) {
            ctx->halting = CtxHalt_FullSignaled;
        }
    } else {
        flush_to = alist_add(ctx, oldest_good, -1);
        restart_pc = ctx->alist[oldest_good].pc;
        // We have to be careful here not to mess up appstate "total_insts"
        // accounting: when oldest_good is flushed, we automatically
        // deduct the count of insts discarded just before it at fetch.
        // However, since we're not restarting fetch from a predecessor as
        // we usually do, we need to add that count back in, to keep the
        // instruction counts in sync.
        discarded_insts_skipped_by_restart =
            ctx->alist[oldest_good].insts_discarded_before;
    }

    DEBUGPRINTF("T%d flushing A%d for context_halt, n_t_c %d a_l_t %d, "
                "alist_used %d oldest_good %d, "
                "restart PC %s\n", ctx->id, ctx->as->app_id,
                ctx->next_to_commit, ctx->alisttop, alist_used, oldest_good,
                fmt_x64(restart_pc));
    if (!restart_pc) {
        abort_printf("T%d/A%d flush: no restart_pc!  next_to_commit %d, "
                     "alisttop %d, flush_to %d, as->npc %s\n", ctx->id, 
                     ctx->as->app_id, ctx->next_to_commit,
                     ctx->alisttop, flush_to, fmt_x64(ctx->as->npc));
    }

    if (flush_to >= 0) {
        if (flush_to == ctx->alisttop) {
            // The meaning of flushing from "alisttop" back to "flush_to" is
            // ambiguous when they're equal: do we want to flush 0 insts, or
            // the entire window?  roll_back_insts() chooses the former.
            // When we have nothing to flush, we set flush_to == -1, so this
            // should only occur for full windows.
            sim_assert(alist_used == ctx->params.active_list_size);
            roll_back_allinsts(ctx, 1, 1);
        } else {
            roll_back_insts(ctx, ctx->alisttop, flush_to, 1, 1);
        }
    }
    reset_nextpc(ctx, restart_pc, 1);
    ctx->as->stats.total_insts += discarded_insts_skipped_by_restart;

    // (two possible cases; asserted at top of function)
    ctx->halting = (ctx->halting == CtxHalt_FullSignaled) ?
        CtxHalt_FullFlushed : CtxHalt_FastFlushed;
}


static void
drain_for_halt(struct context * restrict ctx)
{
    DEBUGPRINTF("T%d/A%d context_halt (after) discovered; "
                "waiting for drain\n", ctx->id, ctx->as->app_id);
    sim_assert(ctx->halting == CtxHalt_AfterSignaled);
    ctx->halting = CtxHalt_AfterDraining;
    // The original context_halt_signal() will block future fetching; we
    // still call reset_nextpc() to cancel outstanding I-misses and such.
    reset_nextpc(ctx, ctx->as->npc, 1);
}


static int
should_flush_for_long_mem(const struct context * restrict ctx)
{
    const activelist * restrict oldest = &ctx->alist[ctx->next_to_commit];
    static int flush_long_loads = -1, flush_long_stores = -1,
        inhibit_solo_flush = -1;
    int do_flush;
    if (flush_long_loads < 0) {
        flush_long_loads = simcfg_get_bool("Hacking/flush_past_long_loads");
        flush_long_stores = simcfg_get_bool("Hacking/flush_past_long_stores");
        inhibit_solo_flush = simcfg_get_bool("Hacking/inhibit_solo_flush");
    }
    sim_assert(ctx->long_mem_stat == LongMem_Detecting);
    do_flush = (oldest->status & MEMORY) && (ctx->core->n_contexts > 1) &&
        (((oldest->mem_flags & SMF_Read) && flush_long_loads) ||
         ((oldest->mem_flags & SMF_Write) && flush_long_stores));
    if (do_flush && inhibit_solo_flush) {
        const CoreResources * restrict core = ctx->core;
        int others_on_core = 0;
        for (int i = 0; i < core->n_contexts; i++) {
            if ((core->contexts[i] != ctx) && core->contexts[i]->running) {
                others_on_core++;
                break;  // break: don't currently care how many, just >0
            }
        }
        if (!others_on_core)
            do_flush = 0;
    }
    return do_flush;
}


static void
flush_for_long_mem(struct context * restrict ctx)
{
    int oldest_good = oldest_notsquashed(ctx);
    int alist_used = context_alist_used(ctx);
    int flush_to;
    mem_addr restart_pc;

    sim_assert(ctx->long_mem_stat == LongMem_Detecting);
    if ((alist_used <= 1) || (oldest_good < 0)) {
        flush_to = -1;
        restart_pc = ctx->as->npc;
    } else {
        flush_to = oldest_good;
        restart_pc = calc_correct_nextpc(&ctx->alist[oldest_good]);
    }

    DEBUGPRINTF("T%ds%d: flushing past long_mem_op, blocking fetch\n",
                ctx->id, oldest_good);
    if (GlobalLongMemLogger)
        longmem_log_flush(GlobalLongMemLogger, ctx);
    ctx->as->extra->long_mem_flushed++;
    if (flush_to >= 0) {
        // Assert: we're not flushing a full window, or an empty range
        sim_assert(flush_to != ctx->alisttop);
        roll_back_insts(ctx, ctx->alisttop, flush_to, 1, 1);
    }
    reset_nextpc(ctx, restart_pc, 1);
    ctx->long_mem_stat = LongMem_FlushedBlocked;
    ctx->fetchcycle = MAX_CYC;
}


void 
commit_group_printstats(void)
{
    i64 total_started = 0;
    i64 total_squashed = 0;
    i64 total_aborted = 0;
    i64 total_committed = 0;
    i64 total_inflight;

    for (int i = 0; i < CtxCount; i++) {
        const context * restrict ctx = Contexts[i];
        sim_assert(ctx->commit_group.started == 
               (ctx->commit_group.squashed + ctx->commit_group.aborted +
                ctx->commit_group.committed + ctx->commit_group.in_flight));
        total_started += ctx->commit_group.started;
        total_squashed += ctx->commit_group.squashed;
        total_aborted += ctx->commit_group.aborted;
        total_committed += ctx->commit_group.committed;
    }

    total_inflight = total_started - (total_squashed + total_aborted +
                                      total_committed);

    printf("Commit group stats:\n");
    printf("  started %s, inflight %s, squashed %s, aborted %s, committed %s "
           "(%.2f%% of started, %.2f%% of not-squashed)\n",
           fmt_i64(total_started), fmt_i64(total_inflight),
           fmt_i64(total_squashed),
           fmt_i64(total_aborted), fmt_i64(total_committed),
           (100.0 * total_committed) / (total_started - total_inflight),
           (100.0 * total_committed) / (total_started - total_inflight - 
                                        total_squashed));
    printf("  blocked cyc/mean: [");
    for (int i = 0; i < CtxCount; i++) {
        const context * restrict ctx = Contexts[i];
        printf(" %s/%.0f", fmt_i64(ctx->commit_group.commit_block_cyc),
               (double) ctx->commit_group.commit_block_cyc /
               ctx->commit_group.committed);
    }
    printf(" ]\n");
}


static void clean_up_mispredict(activelist *brinst) 
{
  context *current = Contexts[brinst->thread];

  sim_assert((brinst->mispredict == MisPred_CorrectPath) ||
         (brinst->mispredict == MisPred_WrongPath));
  reset_nextpc(current, calc_correct_nextpc(brinst),
               (brinst->mispredict == MisPred_CorrectPath));
  DEBUGPRINTF("T%d fetch back on %s path\n", current->id,
              (brinst->mispredict == MisPred_CorrectPath) ? "right" : 
              "still-wrong");
  current->fetchcycle = cyc;
  current->stalled_for_prior_fetch = 0;

  /* restore from checkpoint */

  if (0) {
      // If you make use of commit-group speculation, be sure to handle
      // mispredicts both before, beginning at, and after the start of your
      // speculative region.  You'll want to roll execution back to to the
      // earliest misspeculation, and call cleanup_commit_group().
  } else {
      roll_back_insts(current, current->alisttop, brinst->id, 1, 0);
  }

}

static void
clean_up_lock(activelist *syncinst) 
{
  context *current = Contexts[syncinst->thread];

  reset_nextpc(current, syncinst->pc + 4, 0);
  current->fetchcycle = cyc;
  DEBUGPRINTF("T%d being flushed from queue\n", current->id);
  current->sync_lock_blocked = 1;

#ifdef DEBUG
  {
      int running = 0, blocked = 0;
      for (int i = 0; i < CtxCount; i++) {
          if (Contexts[i]->running)
              running++;
          if (Contexts[i]->sync_lock_blocked)
              blocked++;
      }
      if ((running > 0) && (blocked == running)) {
          fprintf(stderr, "All threads sync_lock_blocked; deadlock, cyc %s\n",
                  fmt_i64(cyc));
          fprintf(stderr, "Lock addr/cyc: [");
          for (int i = 0; i < CtxCount; i++) {
              const context *l_ctx = Contexts[i];
              if (l_ctx->lock_box_entry.valid) {
                  fprintf(stderr, " %s/%s", 
                          fmt_x64(l_ctx->lock_box_entry.addr),
                          fmt_i64(l_ctx->lock_box_entry.timestamp));
              } else {
                  fprintf(stderr, " -/-");
              }
          }
          fprintf(stderr, " ]\n");
          fflush(0);
          sim_abort();
      }
  }
#endif

  /* restore from checkpoint */
  roll_back_insts(current, current->alisttop, current->sync_restore_point,
                  1, 1);
}


// Unlink squashed instructions from a StageQueue
static void
delete_squashed_insts(StageQueue *q, int need_to_update_stats_iq_or_fq)
{
    activelist *prev = NULL;
    activelist *inst = stageq_head(*q);

    while (inst) {
        if (inst->status & SQUASHED){
             if(need_to_update_stats_iq_or_fq == 1)
             {
                update_acc_occ_per_inst(Contexts[inst->thread], inst, 2, 1);
                update_adapt_mgr_dec_tentative(Contexts[inst->thread], FQ, 1);
             }
             else if(need_to_update_stats_iq_or_fq == 2)
             {
                update_acc_occ_per_inst(Contexts[inst->thread], inst, 2, 0);
                update_adapt_mgr_dec_tentative(Contexts[inst->thread], IQ, 1);
             }
            stageq_delete(*q, prev);
        }
        else
            prev = inst;
        inst = (prev) ? prev->next : stageq_head(*q);
    }
}


/* Look to see if any interesting mispeculations were detected
   that now need recovery action.
 */

void
fix_pcs() {
  int mispredict_found = 0;
  int halting_found = 0;

  for (int i=0;i<CtxCount;i++) {
    context *current = Contexts[i];
    if (!current)
        continue;
    if (current->misfetch_discovered) {
      DEBUGPRINTF("T%d misfetch(%s) discovered\n", i,
                  MisPred_names[current->misfetch_discovered->misfetch]);
      clean_up_misfetch(current->misfetch_discovered);
      current->misfetch_discovered = NULL;
    }
    if (current->mispredict_discovered) {
      mispredict_found = 1;
      clean_up_mispredict(current->mispredict_discovered);
      current->mispredict_discovered = NULL;
    }
    if (current->lock_failed) {
      mispredict_found = 1;
      clean_up_lock(current->lock_failed);
      current->lock_failed = NULL;
    }
    if ((current->halting == CtxHalt_FullSignaled) ||
        (current->halting == CtxHalt_FastSignaled)) {
        halting_found = 1;
        flush_for_halt(current);
    } else if (current->halting == CtxHalt_AfterSignaled) {
        halting_found = 1;
        drain_for_halt(current);
    } else if (current->long_mem_stat == LongMem_Detecting) {
        if (should_flush_for_long_mem(current)) {
            mispredict_found = 1;
            flush_for_long_mem(current);
        } else {
            current->long_mem_stat = LongMem_Ignored;
        }
    }
  }

  if (!mispredict_found && !halting_found)
    return;

  /* 
     take dead instructions out of queues 
       -brute force, inefficient.  but then, it works.
     */

  {
      int cnum;
      for (cnum = 0; cnum < CoreCount; cnum++) {
          CoreResources *core = Cores[cnum];
          for (int stage = 0; stage < core->stage.dyn_stages; stage++) {
              delete_squashed_insts(&core->stage.s[stage], 0);
          }
          delete_squashed_insts(&core->stage.intq, 2);
          delete_squashed_insts(&core->stage.floatq, 1);
          delete_squashed_insts(&core->stage.exec, 0);
          delete_squashed_insts(&core->stage.rename_inject, 0);
      }
  }

  clean_cache_queue_squash();
  
  // Recover "SQUASHED" alist entries
  if (GlobalParams.reap_alist_at_squash) {
      for (int i = 0; i < CtxCount; i++) {
          context *ctx = Contexts[i];
          if (ctx->alist[ctx->alisttop].status & SQUASHED) {
              reap_squashed_insts(ctx);
          }
      }
  }

  if (halting_found) {
      for (int i = 0; i < CtxCount; i++) {
          context *ctx = Contexts[i];
          if (ctx->halting == CtxHalt_FullFlushed) {
              // Full-reset halt signaled & flushed; wait for insts to drain
              ctx->halting = CtxHalt_FullDraining;
          } else if (ctx->halting == CtxHalt_FastFlushed) {
              // Fast halt signaled & flushed; halt processing complete
              DEBUGPRINTF("T%d/A%d context_halt (fast) complete; ready for "
                          "re-steer\n", ctx->id, ctx->as->app_id);
              CBQ_Callback *halt_done_cb = ctx->halt_done_cb;
              ctx->halt_done_cb = NULL;
              // Caution: these calls may trigger other halts, which we
              // shouldn't process this cycle (to be fair).
              appmgr_prereset_hook(GlobalAppMgr, ctx);
              context_reset_resteer(ctx);
              callback_invoke(halt_done_cb, NULL);
              callback_destroy(halt_done_cb);
          }
      }
  }
}


/* We checkpointed processor state at a lock instruction.  Now we
   are restarting the thread from that checkpointed state.  In this
   particular case, the lock in question succeeded.  Therefore, the
   speculative instructions beyond the lock must stay in the machine,
   but we have to re-execute them, now that we know they are not
   speculative, but also to make sure that any memory dependences
   protected by the lock are not violated.
 */

static void
restore_from_sync_cp(activelist *syncinst) {

  int next;
  activelist *nextinst;
  u64 oldnextpc;

  context *current = Contexts[syncinst->thread];
  DEBUGPRINTF("%s(T%ds%d)\n", __func__, syncinst->thread, syncinst->id);

  // The lock succeeded; we shouldn't be blocked.  This used to just clear the
  // blocked flag, but it shouldn't be possible for it to be set when we get
  // here.  As long as this is the case, we can ensure that clean_up_lock
  // always leads to a restart_thread(), so that we need not roll-back any
  // state in restart_thread.  (Otherwise, both clean_up_lock and
  // restart_thread try to roll back the same state, which causes problems.)
  sim_assert(current->sync_lock_blocked == 0);

  /* restore from checkpoint */
  roll_back_insts(current, current->alisttop, current->sync_restore_point,
                  0, 0);

  oldnextpc = current->as->npc;
  if (!syncinst->wp)
    current->wrong_path = 0;
  next = current->sync_restore_point;
  sim_assert(current->nextsync == syncinst->id );
  current->nextsync = NONE;
  current->follow_sync = 0;
  while (next != current->alisttop) {
    next = (next + 1) & (current->params.active_list_size - 1);
    nextinst = &current->alist[next];
    if (!current->follow_sync)
      nextinst->wait_sync = 0;
    /* if (nextinst->status & EXECUTING)
      break; */
    if (current->wrong_path) {
      if (!nextinst->wp) {
        nextinst->wp = 1;
        current->stats.wpinstrs++;
        current->stats.instrs--;
        if (!(nextinst->status & FETCHED)) 
          wpexec++;
      }
    }
    current->as->npc = nextinst->pc;
    current->pc = nextinst->pc;

    DEBUGPRINTF("(resim)");
    SmtDISASSEMBLE(current->id, nextinst->id, nextinst->pc, 1);
    const StashData *stash =
        stash_decode_inst(current->as->stash, current->as->npc);
    sim_assert(stash != NULL);
    emulate_inst_for_sim(current, stash, next);

    sim_assert(nextinst->br_flags == stash->br_flags);
    sim_assert(nextinst->gen_flags == stash->gen_flags);
    sim_assert(nextinst->mem_flags == stash->mem_flags);

    nextinst->srcmem = current->emu_inst.srcmem;
    nextinst->destmem = current->emu_inst.destmem;
    current->emu_inst.srcmem = 0;
    current->emu_inst.destmem = 0;

    if (nextinst->status & (FETCHED | MEMORY)) {
        if (!IS_ZERO_REG(nextinst->dest))
            current->last_writer[nextinst->dest] = nextinst->id;
    }
    if (!current->wrong_path && !current->follow_sync
        && (nextinst->syncop == SMT_HW_LOCK || nextinst->syncop == LDL_L
            || nextinst->syncop == LDQ_L || nextinst->syncop == STL_C
            || nextinst->syncop == STQ_C )) {
      current->nextsync = nextinst->id;
      current->follow_sync = 1;
      current->sync_store_value = current->as->R[nextinst->src1].i;
      nextinst->pc = current->pc;
          
      DEBUGPRINTF("T%d in follow_sync mode\n", current->id);
      current->sync_restore_point = nextinst->id;
    }
    if (nextinst->br_flags)
        resim_branch(current, nextinst);
  }
  reset_nextpc(current, oldnextpc, 0);
}


/* We checkpointed processor state at a lock instruction.  Now we
   are restarting the thread from that checkpointed state.
 */

static void
restart_thread(activelist *syncinst) {
  context *current = Contexts[syncinst->thread];
  sim_assert(current->sync_lock_blocked);
  current->sync_lock_blocked = 0;
  current->fetchcycle = cyc + 1;

  DEBUGPRINTF("%s(T%ds%d)\n", __func__, syncinst->thread, syncinst->id);

  // We won't roll-back the state, since that was done already in 
  // clean_up_lock(), which set sync_lock_blocked and led us here.

  syncinst->donecycle = cyc;
}

/*
 * This clears the lock_flag in any threads besides "this_thread" which have
 * it set at address "va".  This is to force any of their store-conditional
 * operations to fail, due to an intervening sync operation.
 */
static void
clear_others_ldlocks(u64 va, int this_thread)
{
    int i;

    for (i = 0; i < CtxCount; i++) {
        if ((i != this_thread) && Contexts[i] && 
            (Contexts[i]->lock_physical_address == va))
            Contexts[i]->lock_flag = 0;
    }
}


/* Lock instructions are not emulated in fetch, like everything else.
   They must execute in processor order, so they are emulated here.
   But even this is deceptive, because this routine is typically called
   from the commit stage, rather than the execute stage.  That is because
   I typically assume synch instructions don't take effect until they
   retire.  yes, that's pretty conservative.
 */

void synchexecute(activelist *exinst) {
  u64 va, new_dest_val;
  u64 least;
  int leastid;
  int i;
  context *current;

  current = Contexts[exinst->thread];
  if (exinst->syncop) {
    if (!exinst->wp) {
      switch(exinst->syncop) {
      case SMT_HW_LOCK:
        va = exinst->srcmem;
        if ((signed int) read_mem_32(current, va)) {
          /* someone else is holding the lock */
#ifdef DEBUG
          if (debugsync) 
            printf("T%ds%i SMT_HW_LOCKing 0x%s...someone else holding "
                   "lock, cyc=%s, pc=0x%s\n", current->id, exinst->id,
                   fmt_x64(va), fmt_i64(cyc), fmt_x64(exinst->pc));

#endif
          lock_fail++;
          current->lock_box_entry.addr = va;
          current->lock_box_entry.valid = 1;
          current->lock_box_entry.timestamp = cyc;
          current->lock_box_entry.inst = exinst;
          current->lock_failed = exinst;
          exinst->donecycle = MAX_CYC;
        } else {
#ifdef DEBUG
          if (debugsync)
            printf("T%ds%d SMT_HW_LOCKing 0x%s...obtained, cyc=%s, "
                   "pc = 0x%s\n", current->id, exinst->id, fmt_x64(va), 
                   fmt_i64(cyc), fmt_x64(exinst->pc));
#endif
          lock_succeed++;
          current->lock_box_entry.valid = 0;
          set_mem_32(current, va, 0x1);
          restore_from_sync_cp(exinst);
        }
        clear_others_ldlocks(va, current->id);
        break;
      case SMT_RELEASE:
        va = exinst->destmem;
        /* Check if anyone else is waiting for the lock */
        least = MAX_CYC;
        leastid = -1;
        for (i=0; i < CtxCount; i++) {
          if (Contexts[i]->lock_box_entry.valid &&
              (Contexts[i]->lock_box_entry.addr == va)) {
            if (Contexts[i]->lock_box_entry.timestamp < least) {
              least = Contexts[i]->lock_box_entry.timestamp;
              leastid = i;
            }
          }
        }
        if (leastid >= 0) {
          context *leastctx = Contexts[leastid];
#ifdef DEBUG
          if (debugsync)
            printf("T%ds%d SMT_RELEASEing 0x%s...context %d waiting, "
                   "cyc = %s, pc=0x%s\n", exinst->thread, exinst->id,
                   fmt_x64(va), leastid, fmt_i64(cyc),
                   fmt_x64(exinst->pc));
#endif

          leastctx->lock_box_entry.valid = 0;
          if (leastctx->lock_failed == leastctx->lock_box_entry.inst) {
            // If lock_failed is still set, then the lock failure has not
            // yet been discovered by fix_pcs() -> clean_up_lock().
            sim_assert(leastctx->lock_failed && !leastctx->sync_lock_blocked);
            leastctx->lock_failed = NULL;
            restore_from_sync_cp(leastctx->lock_box_entry.inst);
          }
          else {
            if (!leastctx->sync_lock_blocked) {
              spec_rel_success++;
              restore_from_sync_cp(leastctx->lock_box_entry.inst);
            }
            else {
              spec_rel_fail++;
              restart_thread(leastctx->lock_box_entry.inst);
              leastctx->follow_sync = 0;
              leastctx->nextsync = NONE;
            }
          }
        } else {
#ifdef DEBUG
            if (debugsync)
                printf("T%ds%d SMT_RELEASEing 0x%s...no one waiting, "
                       "cyc = %s, pc=0x%s\n", exinst->thread, exinst->id, 
                       fmt_x64(va), fmt_i64(cyc), fmt_x64(exinst->pc));
#endif
            set_mem_32(current, va, 0x0);
        }
        clear_others_ldlocks(va, current->id);
        break;    
      case SMT_TERMINATE:
#ifdef DEBUG
        if (debugsync)
          printf("SMT_terminate (context %d), cyc = %s, pc = 0x%s\n", 
                 current->id, fmt_i64(cyc), fmt_x64(exinst->pc));
#endif
        break;
      case LDL_L:
      case LDQ_L:
        va = exinst->srcmem;
        current->lock_flag = 1;
        current->lock_physical_address = va;
        new_dest_val = (i64) read_mem_32(current, va);
#ifdef DEBUG
        if (debugsync)
          printf("Context %d LDL_Locking 0x%s read 0x%s ...cyc = %s, "
                 "pc=0x%s\n", 
                 exinst->thread, fmt_x64(va), fmt_x64(new_dest_val),
                 fmt_i64(cyc), fmt_x64(exinst->pc));
#endif
        restore_from_sync_cp(exinst);
        current->as->R[exinst->dest].i = new_dest_val;
        break;
      case STL_C:
      case STQ_C:
        va = exinst->destmem;
        if (current->lock_flag) {
#ifdef DEBUG
          if (debugsync)
            printf("Context %d STL_C success 0x%s writing 0x%s ..."
                   "cyc = %s, pc=0x%s\n", 
                   exinst->thread, fmt_x64(va), 
                   fmt_x64(current->sync_store_value),
                   fmt_i64(cyc), fmt_x64(exinst->pc));
#endif
          if (exinst->syncop == STQ_C)
              set_mem_64(current, va, current->sync_store_value);
          else
              set_mem_32(current, va, current->sync_store_value & 0xffffffff);
          new_dest_val = 1;
          for (i=0;i<CtxCount;i++) {
            if (Contexts[i] && 
                (Contexts[i]->lock_physical_address == va)) {
              Contexts[i]->lock_flag = 0;
            }
          }
        }
        else {
          new_dest_val = 0;
#ifdef DEBUG
          if (debugsync)
            printf("Context %d STL_C fail 0x%s...cyc = %s, pc=0x%s\n", 
                   exinst->thread, fmt_x64(va), fmt_i64(cyc),
                   fmt_x64(exinst->pc));
#endif
        }
        current->lock_physical_address = 0;
        current->lock_flag = 0;

        // synchresolve() does _not_ update the writers[] or release any
        // waiters for conditional stores, since it doesn't know when they'll
        // actually complete, so we need to do it here.  This used to manually
        // update only current->writer_sync_cp; now, we just use
        // update_writers() to update them all.  This is fine, since the
        // following call to restore_from_sync_cp() overwrites the other
        // values with writer_sync_cp anyway.
        sim_assert(current->follow_sync);
        update_writers(exinst);
        release_waiters(exinst);
        restore_from_sync_cp(exinst);
        current->as->R[exinst->dest].i = new_dest_val;
        break;
      }
    }
  }
}


static int
mispredict_applies(const context * restrict ctx,
                   const activelist * cand_misp,
                   const activelist * curr_misp)
{
    int result;
    if (cand_misp->mispredict == MisPred_CorrectPath) {
        // If candidate misp was from the correct path, candidate wins
        sim_assert(curr_misp->mispredict == MisPred_WrongPath);
        result = 1;
    } else if (curr_misp->mispredict == MisPred_CorrectPath) {
        // If we've already seen the correct-path misp, candidate loses
        sim_assert(cand_misp->mispredict == MisPred_WrongPath);
        result = 0;
    } else {
        // Both WP: iff candidate is older, it wins
        sim_assert((cand_misp->mispredict == MisPred_WrongPath) &&
               (curr_misp->mispredict == MisPred_WrongPath));
        int cand_commit_dist = alist_count(ctx, ctx->next_to_commit,
                                           cand_misp->id);
        int curr_commit_dist = alist_count(ctx, ctx->next_to_commit,
                                           curr_misp->id);
        result = (cand_commit_dist < curr_commit_dist);
    }
    return result;
}


/* for most instructions, not much interesting happens in the
   execute stage, because dependence resolution happens in the
   queue stage.

   This is, however, where I detect mispredictions.
 */

static void
execute_for_core(CoreResources *core)
{
    const int rwrite1 = core->stage.rwrite1;
    activelist *prev = NULL;
    activelist *instrn = stageq_head(core->stage.exec);

    while (instrn) {
        context *current = Contexts[instrn->thread];
        // cyc + 1: move to regwrite latch IFF the result will be ready then
        if (instrn->donecycle <= (cyc + 1)) {
            // It shouldn't be possible for stale insts to pile up here?
            // If this assertion fails, something's gone weird with timing
            // XXX assert too strong?  single-cyc fills screw it up :(
            //   sim_assert(instrn->donecycle == (cyc + 1));
            DEBUGPRINTF("T%ds%d completes execution%s, delay %d (%s)\n",
                        instrn->thread, instrn->id, instrn->wp ? " (wp)":"",
                        instrn->delay, fmt_i64(instrn->donecycle));
            if ((instrn->mispredict != MisPred_None) &&
                (!current->mispredict_discovered ||
                 mispredict_applies(current, instrn,
                                    current->mispredict_discovered))) {
                current->mispredict_discovered = instrn;
                DEBUGPRINTF("T%d mispredict(%s) discovered\n", instrn->thread,
                            MisPred_names[instrn->mispredict]);
            }
            if (instrn->status == EXECUTING)
                /* ensures that mispredict not recorded twice, in particular
                   by restore_from_sync_cp */
                instrn->status = EXECUTED;
            if (instrn->fu == SYNCH && 
                (instrn->syncop == LDL_L || instrn->syncop == LDQ_L)) {
                synchexecute(instrn);
            }
            
            if (current->as != NULL){
                if (instrn->fu == FP) {
                    sim_assert (current->as->extra != NULL);
                    current->as->extra->fpalu_acc++;
                }
                else if (instrn->fu == INTEGER) {
                    sim_assert (current->as->extra != NULL);
                    current->as->extra->intalu_acc++;
                }
                else if (instrn->fu == INTLDST) {
                    sim_assert (current->as->extra != NULL);
                    current->as->extra->ldst_acc++;
                }
            }
 
            stageq_delete(core->stage.exec, prev);
            stageq_enqueue(core->stage.s[rwrite1], instrn);
        } else {
            prev = instrn;
        }

        instrn = (prev) ? prev->next : stageq_head(core->stage.exec);
    }
}


void
execute(void)
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        execute_for_core(core);
    }
}
