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

Significant parts of the SMTSIM simulator were written by Jack Lo.
Therefore the following copyrights may also apply:

Copyright (C) Jack Lo
 */

#include <stdio.h>
#include <stdlib.h>

#include "sim-assert.h"
#include "main.h"
#include "inst.h"
#include "smt.h"
#include "core-resources.h"
#include "trace-fill-unit.h"
#include "branch-bias-table.h"
#include "context.h"
#include "callback-queue.h"
#include "dyn-inst.h"
#include "stash.h"
#include "app-state.h"
#include "app-mgr.h"
#include "sim-cfg.h"
#include "sim-params.h"
#include "app-stats-log.h"
#include "work-queue.h"
#include "sign-extend.h"
#include "cache.h"
#include "cache-req.h"
#include "prefetch-streambuf.h"
#include "deadblock-pred.h"
#include "adapt-mgr.h"


void *FILE_DumpCommitFile = 0;
i64 DebugCommitNum = -1;


/* We record the total number of renaming registers freed
   while we're committing instructions, but we don't want 
   to actually make them available until the end of the cycle,
   which is when this is called. Similarly for the lsq entries
   and the rob.
*/

void fix_regs(void) 
{
    int core_id, i;

    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        core->i_registers_used -= core->i_registers_freed;
        core->i_registers_freed = 0;
        core->f_registers_used -= core->f_registers_freed;
        core->f_registers_freed = 0;
        core->lsq_used -= core->lsq_freed;
        core->lsq_freed = 0;
    }

    for (i=0; i < CtxCount; i++) {
        context *ctx = Contexts[i];
        if (ctx) {
            if (ctx->as != NULL){
                ctx->as->extra->rob_acc += ctx->rob_freed_this_cyc;
                ctx->as->extra->lsq_acc += ctx->lsq_freed;
            }
            ctx->rob_used -= ctx->rob_freed_this_cyc;
            sim_assert(ctx->rob_used >= 0);
            ctx->rob_freed_this_cyc = 0;
            ctx->lsq_freed = 0;
        }
    }
    update_adapt_mgr_make_final();
}


// "oldest" NULL iff detecting an I-cache miss stall
void
long_mem_detect(context * restrict ctx, activelist * restrict inst,
                int at_commit)
{
    int long_mem_cyc = GlobalParams.long_mem_cyc;
    if ((long_mem_cyc <= 0) || !ctx->as || 
        (ctx->long_mem_stat != LongMem_None))
        return;

    int is_long_mem;
    if (inst) {
        // Detecting D-miss stall
        is_long_mem = (inst->status & MEMORY) &&
            (inst->donecycle == MAX_CYC) &&
            (cyc >= (inst->issuecycle + long_mem_cyc));
    } else {
        // Detecting I-miss stall
        is_long_mem = ctx->imiss_cache_entry &&
            (ctx->fetchcycle == MAX_CYC) &&
            (cyc >= (ctx->last_fetch_begin + long_mem_cyc));
    }

    if (is_long_mem) {
        DEBUGPRINTF("T%ds%d/A%d long_mem_detect: matched long_mem_op "
                    "(%s%s%s)\n", ctx->id,
                    (inst) ? inst->id : -1, ctx->as->app_id,
                    (inst) ? "" : "I",
                    (inst && (inst->mem_flags & SMF_Read)) ? "R" : "",
                    (inst && (inst->mem_flags & SMF_Write)) ? "W" : "");
        ctx->long_mem_stat = LongMem_Detecting;
        ctx->as->extra->long_mem_detected++;
        appmgr_signal_longmiss(GlobalAppMgr, ctx->as,
                               (inst) ? inst->id : -1);
        if (GlobalLongMemLogger)
            longmem_log_stall(GlobalLongMemLogger, ctx, inst);
    }
}


typedef struct TimeOrder {
    i64 time;
    context *ctx;
} TimeOrder;

static int timeord_cmp(const void *v1, const void *v2)
{
    const TimeOrder *o1 = v1;
    const TimeOrder *o2 = v2;
    i64 diff = o1->time - o2->time;
    int result = (diff > 0) ? 1 : ((diff < 0) ? -1 : 0);
    return result;
}


/* This is the final stage.  A number of things get done here.
   Instructions that have reached this stage commit in order 
   (per thread)..
   We are no longer keeping track of instructions through the
   pipeline (since they are no longer in order), we're just 
   going through the activelist, which is in-order, and 
   seeing who is ready to commit.

   Here, renaming registers/rob entries/lsq entries are freed.  
   Synch operationsi actually get performed and become visible 
   to other threads.  Prediction tables get updates.
 */

static void
commit_for_core(CoreResources *core)
{
  const int n_contexts = core->n_contexts;
  int threads_left = core->params.commit.thread_count_limit;
  int total_commits_left = core->params.commit.total_limit;
  TimeOrder thread_order[n_contexts];   // OK in C99
  int commits_this_cyc[n_contexts];     // (NOT global thread ID numbers!)

  for (int i = 0; i < n_contexts; i++) {
      context * restrict ctx = core->contexts[i];
      const activelist * restrict next_inst = &ctx->alist[ctx->next_to_commit];
      int ready = (next_inst->status & SQUASHED) ||
          (next_inst->status & RETIREABLE);
      i64 ord_time = (ready) ? next_inst->donecycle : MAX_CYC;
      thread_order[i].time = ord_time;
      thread_order[i].ctx = ctx;
      commits_this_cyc[i] = 0;
  }
  if (n_contexts > 1)
      qsort(thread_order, n_contexts, sizeof(thread_order[0]), timeord_cmp);

  if (threads_left > n_contexts)
      threads_left = n_contexts;

  for (int i = 0; i < threads_left; i++) {
    int thread_commits_left = core->params.commit.single_limit;
    context *current = thread_order[i].ctx;
    /*retire instructions */
    int next = current->next_to_commit;
    activelist *top = &current->alist[next];
    while (((top->status & SQUASHED) ||
            (top->status & RETIREABLE)) &&
           (total_commits_left > 0) && (thread_commits_left > 0)) {
      // assert: reap_alist_at_squash set -> "top" isn't SQUASHED
      sim_assert(!GlobalParams.reap_alist_at_squash || 
                 !(top->status & SQUASHED));
      if (next == top->commit_group.leader_id) {
        if (top->commit_group.remaining != 0) {
          current->commit_group.commit_block_cyc++;
          break;
        }
        current->commit_group.committed++;
        sim_assert(current->commit_group.in_flight > 0);
        current->commit_group.in_flight--;
      }
      total_commits_left--;
      thread_commits_left--;
      if (top->fu == SYNCH && !(top->status & SQUASHED) && 
          !(top->syncop == LDL_L || top->syncop == LDQ_L)) {
        synchexecute(top);
      }
      const int is_from_app = top->as != NULL;
      const int is_retireable = (top->status & RETIREABLE) && is_from_app;
      const int is_inject_retire = (top->status & RETIREABLE) && !is_from_app;
      if (is_retireable) {
        commits_this_cyc[current->core_thread_id]++;
        if (FILE_DumpCommitFile && i==0) {
          fprintf((FILE *) FILE_DumpCommitFile, "%s\n", fmt_x64(top->pc)); 
        }
        if (allinstructions > 0)
            allinstructions--;
        if (top->br_flags && !top->skipped_bpredict) {
            if (top->tc.base_pc) {
                sim_assert(top->tc.predict_num >= 0);
                DEBUGPRINTF("MBP update: base %s ghr %s predict #%i: "
                            "taken %i\n", fmt_x64(top->tc.base_pc),
                            fmt_x64(top->ghr), 
                            top->tc.predict_num, top->taken_branch);
                mbp_update(core->multi_bp, top->tc.base_pc, top->ghr,
                           top->tc.predict_num, top->taken_branch,
                           (top->taken_branch == top->taken_predict));
            } else {
                update_pht(current, top->pc, top->taken_branch, top->ghr);
            }
        }
        if (SBF_CondBranch(top->br_flags))
            bbt_update(core->br_bias, top->pc, i, top->taken_branch);
        if (core->tfill)
            tfu_inst_commit(core->tfill, current, top);
        if (current->long_mem_stat == LongMem_Completing) {
            DEBUGPRINTF("T%d long_mem_op committing.\n", current->id);
            current->long_mem_stat = LongMem_None;
        }
        if (!IS_ZERO_REG(top->dest))
            current->bmt_regdirty[top->dest] = 1;
        if ((core->d_streambuf || core->d_dbp) && top->mem_flags) {
            LongAddr addr;
            laddr_set(addr, (top->mem_flags & SMF_Read) ? top->srcmem
                      : top->destmem, current->as->app_master_id);
            if (core->d_streambuf) {
                pfsg_mem_commit(core->d_streambuf, top->pc, addr,
                                (top->mem_flags & SMF_Write),
                                top->dcache_sim.service_level != 1);
            }
            if (core->d_dbp) {
                dbp_mem_commit(core->d_dbp, top->pc, addr);
            }
        }
        current->commit_taken_br.br_flags =
            (top->br_flags && top->taken_branch) ? top->br_flags : 0;
        if (current->commit_taken_br.br_flags) {
            current->commit_taken_br.pc = top->pc;
            current->commit_taken_br.addr_regnum = top->src2;
        }
      }
      if (top->syncop == MB)
          current->core->mb.current_epoch++;
      else if (top->syncop == WMB)
          current->core->wmb.current_epoch++;
      top->status = INVALID;
      if (current->last_writer[top->dest] == next)
          current->last_writer[top->dest] = NONE;
      core->i_registers_freed += top->iregs_used;
      core->f_registers_freed += top->fregs_used;
      core->lsq_freed += top->lsqentry;
      current->lsq_freed += top->lsqentry;
      current->rob_freed_this_cyc += top->robentry;
      update_acc_occ_per_inst(current, top, 1, 2);
      update_adapt_mgr_dec_tentative(current, IREG, top->iregs_used);
      update_adapt_mgr_dec_tentative(current, FREG, top->fregs_used);
      update_adapt_mgr_dec_tentative(current, LSQ, top->lsqentry);
      update_adapt_mgr_dec_tentative(current, ROB, top->robentry);
      top->robentry = 0;
#ifdef DEBUG
      if (is_retireable && (current->stats.total_commits == DebugCommitNum)) {
          debug = 1;
          fflush(0);
      }
#endif
      DEBUGPRINTF("T%ds%d (A%d PC %s fetched %s group %d) %s, %d reg freed",
                  current->id, next,
                  (top->as) ? top->as->app_id : -1, 
                  fmt_x64(top->pc),
                  fmt_i64(top->fetchcycle), top->commit_group.leader_id,
                  (is_retireable || is_inject_retire) ? "retired" : "reaped",
                  top->iregs_used + top->fregs_used);
      if (is_retireable) {
          DEBUGPRINTF(", commit %s app_commit %s app_inst %s",
                      fmt_i64(current->stats.total_commits),
                      fmt_i64(top->as->extra->total_commits),
                      fmt_i64(top->app_inst_num));
          current->stats.total_commits++;
          top->as->extra->total_commits++;
          if (top->mem_flags)
              top->as->extra->mem_commits++;
          top->as->extra->cp_insts_discarded += top->insts_discarded_before;
          top->as->extra->app_inst_last_commit = top->app_inst_num;
      } else if (is_inject_retire) {
          DEBUGPRINTF(" (injected)");
      }
      DEBUGPRINTF("\n");

      if (is_inject_retire) {
          if (top->bmt.spillfill & BmtSF_Final) {
              if (top->bmt.spillfill & BmtSF_Fill)
                  appmgr_signal_finalfill(GlobalAppMgr, current, 1);
              if (top->bmt.spillfill & BmtSF_Spill)
                  appmgr_signal_finalspill(GlobalAppMgr, current, 1);
          }
      }

      next = (next + 1) & (current->params.active_list_size - 1);
      top = &current->alist[next];

      if (current->lock_failed)
          break;
    }
    current->next_to_commit = next;
    sim_assert(top->id == next);
  }

  for (int i = 0; i < n_contexts; i++) {        // NOT global thread IDs
      context * restrict ctx = core->contexts[i];
      activelist * restrict oldest_inst = &ctx->alist[ctx->next_to_commit];
      int app_id = (ctx->as) ? ctx->as->app_id : -1;    // ctx->as may be NULL
      if (oldest_inst->status & INVALID) {
          // Empty pipe
          if (!commits_this_cyc[i] && GlobalParams.long_mem_cyc &&
              GlobalParams.long_mem_at_commit) {
              long_mem_detect(ctx, NULL, 1);
          }
          if (ctx->draining) {
              DEBUGPRINTF("T%d/A%d draining complete\n", ctx->id, app_id);
              ctx->draining = 0;
          }
          if ((ctx->halting == CtxHalt_FullDraining) ||
              (ctx->halting == CtxHalt_AfterDraining)) {
              DEBUGPRINTF("T%d/A%d context_halt (%s) complete; resetting "
                          "context\n", ctx->id, app_id,
                          (ctx->halting == CtxHalt_FullDraining) ? 
                          "full" : "after");
              CBQ_Callback *halt_done_cb = ctx->halt_done_cb;
              ctx->halt_done_cb = NULL;
              appmgr_prereset_hook(GlobalAppMgr, ctx);
              context_reset(ctx);
              callback_invoke(halt_done_cb, NULL);
              callback_destroy(halt_done_cb);
          }
      } else if (!commits_this_cyc[i] && GlobalParams.long_mem_cyc &&
                 GlobalParams.long_mem_at_commit) {
          // Non-empty pipe but no commits: test if commit blocked on long-mem
          long_mem_detect(ctx, oldest_inst, 1);
      }

      // See if we've crossed some N-commits-since-go boundary this cycle
      // (note that we may overshoot by up to the per-thread-commit-limit)
      if (commits_this_cyc[i] && (ctx->as != NULL)) {
          AppState *as = ctx->as;
          if (callbackq_ready(as->extra->watch.commit_count,
                              as->extra->total_commits)) {
              CBQ_Args *cb_args =
                  commit_watchpoint_args(ctx, commits_this_cyc[i]);
              callbackq_service(as->extra->watch.commit_count,
                                as->extra->total_commits, cb_args);
              callback_args_destroy(cb_args);
          }
          if (callbackq_ready(as->extra->watch.app_inst_commit,
                              as->extra->app_inst_last_commit)) {
              CBQ_Args *cb_args =
                  commit_watchpoint_args(ctx, commits_this_cyc[i]);
              callbackq_service(as->extra->watch.app_inst_commit,
                                as->extra->app_inst_last_commit, cb_args);
              callback_args_destroy(cb_args);
          }
      }
  }
}


void
commit(void)
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        commit_for_core(core);
    }
}


void 
process_tcfill_queues(void)
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        if (core->tfill)
            tfu_process_queue(core->tfill);
    }
}


// "reap" squashed instructions from a context: ready their alist entries for
// immediate re-use, rather than forcing them to take up space until they pass
// through commit().  This is like a mini-commit(), performing much of the
// same accounting that commit() does to non-is_retireable instructions, or
// asserting that such accounting is unneeded.
//
// Background:
//
// Typically, a squash is done by calling roll_back_insts() to undo a
// contiguous sequence of dynamic instructions from the youngest (at alisttop)
// back to some given point.  This leaves many alist entries with status
// SQUASHED between that point and alisttop.  Traditionally, smtsim leaves
// those squashed entries in-place, occupying their alist entries until they
// pass through commit().  Running out of alist entries leads to blocking
// fetch until more are freed, but that is an artifact of the simulator's
// management of alist memory; it doesn't correspond to any real-world
// processor resource constraint.  (The ROB-entry accounting takes care
// of modeling a constraint on outstanding dynamic instruction count.)
//
// This routine moves "alisttop" backwards and invalidates those squashed
// instructions, so that fetch() can starting using them again next cycle.
// This can only recover entries immediately after a squash takes place,
// before subsequent fetch allocations move alisttop forward.
//
// This is potentially dangerous, as pointers to alist entries are held in
// many places in the simulator.  Make sure that any routines which check for
// status SQUASHED -- typically those that unlink squashed instructions from
// various queues -- are called before this, since it clears the SQUASHED
// status.
void
reap_squashed_insts(context * restrict ctx)
{
    int num_reaped = 0;
    for (int inst_id = ctx->alisttop; (ctx->alist[inst_id].status & SQUASHED);
         inst_id = alist_add(ctx, inst_id, -1)) {
        activelist * restrict inst = &ctx->alist[inst_id];
        DEBUGPRINTF("%s: reaping T%ds%d (A%d PC %s fetched %s group %d)\n",
                    __func__, ctx->id, inst_id, 
                    (inst->as) ? inst->as->app_id : -1, fmt_x64(inst->pc),
                    fmt_i64(inst->fetchcycle),
                    inst->commit_group.leader_id);
        // resources should have already been freed by cleanup_deadinst()
        sim_assert(!inst->robentry);
        sim_assert(!inst->iregs_used);
        sim_assert(!inst->fregs_used);
        sim_assert(!inst->lsqentry);

        // Unlink inbound dependence edges, if they're still outstanding;
        // these producer instructions have this instruction linked in their
        // "waiters" array, waiting to be dereferenced by release_waiters().
        // (For producer which are squashed, don't bother; they won't get to
        // release_waiters() anyway.)
        if (inst->src1_waitingfor &&
            !(inst->src1_waitingfor->status & SQUASHED))
            remove_waiter(inst->src1_waitingfor, inst);
        if (inst->src2_waitingfor &&
            !(inst->src2_waitingfor->status & SQUASHED))
            remove_waiter(inst->src2_waitingfor, inst);

        // undo_inst() shouldn't have left last_writer referring to this
        sim_assert(ctx->last_writer[inst->dest] != inst_id);

        if (inst->syncop == MB)
            ctx->core->mb.current_epoch++;
        else if (inst->syncop == WMB)
            ctx->core->wmb.current_epoch++;
        
        inst->status = INVALID;
        num_reaped++;
    }
    int new_alist_top = alist_add(ctx, ctx->alisttop, -num_reaped);
    DEBUGPRINTF("%s: reaped %d entries of T%d, alisttop %d -> %d, time %s\n",
                __func__, num_reaped, ctx->id, ctx->alisttop, new_alist_top,
                fmt_now());
    ctx->alisttop = new_alist_top;
}
