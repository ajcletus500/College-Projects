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

#include "sim-assert.h"
#include "main.h"
#include "stash.h"
#include "core-resources.h"
#include "context.h"
#include "dyn-inst.h"
#include "cache.h"
#include "inst.h"
#include "smt.h"
#include "sim-params.h"
#include "callback-queue.h"
#include "app-state.h"
#include "mshr.h"
#include "adapt-mgr.h"


#if defined(DEBUG)
#   define DEBUG_MEM_BARRIERS   0
#else
#   define DEBUG_MEM_BARRIERS   0
#endif


#define QUEUE_CYC       1
#define Q_RR_CYC(core)  (QUEUE_CYC + (core)->params.regread.n_stages)


static i64 instrcount[FUTYPES] = {0,0,0,0};
i64 wpexec;


i64 oldtotal, oldcycles;
int resetflag = 0;

/*  It turns out that most of the important action takes place
 *    here, so that the most important stats are both recorded
 *    in and printed out from this file.  
 *  The queue stage is when instructions are selected for issue
 *    from the instruction queues, then all resulting dependences
 *    are resolved to possibly wake up other instructions for
 *    execution. 
 */

/* can't set cyc to zero at the end of warmup, or nasty things
   will happen, so we just keep track of where we were when
   warmup ended.
*/
i64 warmupcyc = 0;

static void
schedstats_for_ctx(const context *ctx, i64 run_cyc, const char *pref)
{
    printf("%sAvg occupancy of ROB: %.2f (%.2f%%)\n", pref,
           (double) ctx->stats.robsizetotal/run_cyc,
           100 * (double) ctx->stats.robsizetotal/run_cyc/ctx->params.reorder_buffer_size);
    // activelist conflicts are a simulation artifact;
    // ROB conflicts simulate an architectural phenomenon.
    printf("%sFetch I-MSHR full: %s (%.2f%%)\n"
           "%sRename ROB full cyc: %s (%.2f%%), "
           "fetch alist full cyc: %s (%.2f%%)\n",
           pref, fmt_i64(ctx->stats.i_mshr_conf), 
           100.0 * (double) ctx->stats.i_mshr_conf/run_cyc,
           pref,
           fmt_i64(ctx->stats.robconf_cyc),
           100.0 * (double) ctx->stats.robconf_cyc/run_cyc,
           fmt_i64(ctx->stats.alistconf_cyc),
           100.0 * (double) ctx->stats.alistconf_cyc/run_cyc);
}


static void
schedstats_for_core(const CoreResources *core, i64 run_cyc, const char *pref)
{
    printf("%sAvg pre-sched occupancy of IQ: %.2f (%.2f%%), FQ: %.2f (%.2f%%)\n", pref,
           (double) core->q_stats.iqsizetotal/run_cyc, 
           100 * (double) core->q_stats.iqsizetotal/run_cyc/core->params.queue.int_queue_size, 
           (double) core->q_stats.fqsizetotal/run_cyc,
           100 * (double) core->q_stats.fqsizetotal/run_cyc/core->params.queue.float_queue_size ); 
    printf("%sRename IQ full cyc: %s (%.2f%%), FQ: %s (%.2f%%)\n", pref,
           fmt_i64(core->q_stats.iqconf_cyc),
           100.0 * (double) core->q_stats.iqconf_cyc/run_cyc,
           fmt_i64(core->q_stats.fqconf_cyc), 
           100.0 * (double) core->q_stats.fqconf_cyc/run_cyc);
    printf("%sAvg occupancy of I regs: %.2f (%.2f%%), FP regs: %.2f (%.2f%%)\n", pref,
           (double) core->q_stats.iregsizetotal/run_cyc,
           100.0 * (double) core->q_stats.iregsizetotal/run_cyc/core->params.rename.int_rename_regs,
           (double) core->q_stats.fregsizetotal/run_cyc,
           100.0 * (double) core->q_stats.fregsizetotal/run_cyc/core->params.rename.float_rename_regs);
    printf("%sRename I regs full cyc: %s (%.2f%%), FP regs: %s (%.2f%%)\n",
           pref,
           fmt_i64(core->q_stats.iregconf_cyc),
           100.0 * (double) core->q_stats.iregconf_cyc/run_cyc, 
           fmt_i64(core->q_stats.fregconf_cyc),
           100.0 * (double) core->q_stats.fregconf_cyc/run_cyc);
    printf("%sAvg occupancy of LSQ: %.2f (%.2f%%)\n", pref,
           (double) core->q_stats.lsqsizetotal/run_cyc,
           100.0 * (double) core->q_stats.lsqsizetotal/run_cyc/core->params.loadstore_queue_size);
    printf("%sRename LSQ full cyc: %s (%.2f%%)\n", pref,
           fmt_i64(core->q_stats.lsqconf_cyc),
           100.0 * (double) core->q_stats.lsqconf_cyc/run_cyc);
    printf("%sAvg occupancy of alus IALUS: %.2f (%.2f%%), FPALUS: %.2f (%.2f%%), LDST: %.2f (%.2f%%)\n", pref,
           (double) core->q_stats.ialuissuetotal/run_cyc,
           100.0 * (double) core->q_stats.ialuissuetotal/run_cyc/core->params.queue.max_int_issue,
           (double) core->q_stats.faluissuetotal/run_cyc,
           100.0 * (double) core->q_stats.faluissuetotal/run_cyc/core->params.queue.max_float_issue,
           (double) core->q_stats.ldstissuetotal/run_cyc,
           100.0 * (double) core->q_stats.ldstissuetotal/run_cyc/core->params.queue.max_ldst_issue);
    printf("%sIssue conflicts, I unit: %s (%.2f), FP: %s (%.2f), "
           "LDST: %s (%.2f)\n", pref,
           fmt_i64(core->q_stats.intfuconf),
           (double) core->q_stats.intfuconf/run_cyc,
           fmt_i64(core->q_stats.fpfuconf),
           (double) core->q_stats.fpfuconf/run_cyc,
           fmt_i64(core->q_stats.ldstfuconf),
           (double) core->q_stats.ldstfuconf/run_cyc);
    printf("%sIssue conflicts, D-MSHR: %s (%.2f), mb: %s (%.2f), "
           "wmb: %s (%.2f), mem: %s (%.2f)\n", pref,
           fmt_i64(core->q_stats.d_mshr_conf),
           (double) core->q_stats.d_mshr_conf/run_cyc,
           fmt_i64(core->q_stats.mb_conf),
           (double) core->q_stats.mb_conf/run_cyc,
           fmt_i64(core->q_stats.wmb_conf),
           (double) core->q_stats.wmb_conf/run_cyc,
           fmt_i64(core->q_stats.memconf),
           (double) core->q_stats.memconf/run_cyc);


    //TODO: Add stats for I-Cache, D-Cache, fetch, commit
    
    printf("%sTotal conflicts (non-issued, ready insts): %s (%.2f)\n",
           pref, fmt_i64(core->q_stats.total_conf),
           (double) core->q_stats.total_conf / run_cyc);
    printf("%sTotal conflict cyc buckets:", pref);
    {
        for (int i = 0; i < NELEM(core->q_stats.totalconf_lg_cyc); i++)
            printf(" %s", fmt_i64(core->q_stats.totalconf_lg_cyc[i]));
    }
    printf("\n");
}

void
schedstats(void)
{
  i64 realcyc = cyc - warmupcyc;
  int total_fetch_bw = 0;
  i64 totalcycles;
  i64 total=0, wptotal=0;
  int i;
  i64 totalexec;

    for (i = 0; i < CoreCount; i++)
        total_fetch_bw += Cores[i]->params.fetch.total_limit;
    totalcycles = total_fetch_bw * realcyc;

    totalexec = 0;
    for (i=0;i<4;i++) {
      totalexec += instrcount[i];
    }
    printf("Contexts: %d  Fetch Width: %d\n", CtxCount, total_fetch_bw);
    for (i=0;i<CtxCount;i++) { 
      total += Contexts[i]->stats.instrs;
      wptotal += Contexts[i]->stats.wpinstrs;
    }

    printf("No of useful instructions fetched: %s (%.2f%% proc util.)\n",
           fmt_i64(total),
           (float) 100 * total / totalcycles);
    printf("No of total instructions fetched:  %s (%.2f%% proc util.)\n", 
        fmt_i64(wptotal+total+misfetchtotal+flushed), 
        (float) 100.0 * (wptotal+total+misfetchtotal+flushed) / totalcycles);
 printf(" includes %s misfetch (%.1f%%), %s (%.1f%%) wrong-path fetches\n and "
        "%s non-WP flushed for failed lock and other (%.1f%%)\n",
        fmt_i64(misfetchtotal),
        (float)100.0 * ((float)misfetchtotal/
                        (misfetchtotal+wptotal+flushed+total)),
        fmt_i64(wptotal),
        (float)100.0 * ((float)wptotal/(misfetchtotal+wptotal+flushed+total)),
        fmt_i64(flushed),
        (float)100.0 * ((float)flushed/(misfetchtotal+wptotal+flushed+total)));

    printf("No of useful instructions issued/executed: %s "
           "(%.2f%% proc util.)\n", 
           fmt_i64(totalexec-wpexec-execflushed), 
           (float) 100.0 * (totalexec-wpexec-execflushed) / totalcycles);

    printf("No of total instructions issued/executed:  %s "
           "(%.2f%% proc util.)\n", 
           fmt_i64(totalexec), (float) 100.0 * (totalexec) / totalcycles);
    printf(" includes %s wrong-path (%.1f%%) insts and %s flushed (%.1f%%)\n",
           fmt_i64(wpexec), (float) 100.0 * ((float)wpexec/(totalexec)),
           fmt_i64(execflushed), 
           (float) 100.0 * ((float)execflushed/(totalexec)));
    printf("    fp %s, %.2f%%\n", fmt_i64(instrcount[0]),
           (float) 100.0 * (float) instrcount[0]/(float) totalexec);
    printf("    synch %s, %.2f%%\n", fmt_i64(instrcount[1]),
           (float) 100.0 * (float) instrcount[1]/(float) totalexec);
    printf("    integer %s, %.2f%%\n", fmt_i64(instrcount[2]),
           (float) 100.0 * (float) instrcount[2]/(float) totalexec);
    printf("    load/store %s, %.2f%%\n", fmt_i64(instrcount[3]),
           (float) 100.0 * (float) instrcount[3]/(float) totalexec);
    printf("No of cycles taken: %s\n", fmt_i64(realcyc));
    printf("Parallelism [");
    total = 0;
    for (i=0;i<CtxCount;i++) {
      total += Contexts[i]->stats.instrs;
      printf(" %.3f,", (double) Contexts[i]->stats.instrs / realcyc );
    }
  printf("] = %.3f\n", (double) total / realcyc);
  if (resetflag) {
    printf("Cycles since print_sim: %s\n", fmt_i64(realcyc-oldcycles));
    printf("Paral. since print_sim [");
    for (i=0;i<CtxCount;i++) {
      printf(" %.3f,", (double) (Contexts[i]->stats.instrs -
                                 Contexts[i]->stats.oldinstructions) /
             (realcyc - oldcycles) );
    }
    printf("] = %.3f\n", (double) (total-oldtotal) / (realcyc-oldcycles));
  }

  for (i = 0; i < CoreCount; i++) {
      printf("Core %d scheduling stats\n", i);
      schedstats_for_core(Cores[i], realcyc, "  ");
  }

  for (i = 0; i < CtxCount; i++) {
      printf("Context %d scheduling stats\n", i);
      schedstats_for_ctx(Contexts[i], realcyc, "  ");
  }
}


/* This is the one thing that is only printed once, making
   it easy to grep for the bottom line */
void finalstats() {

  i64 total=0;
  int i;

  for (i=0;i<CtxCount;i++) {
    total += Contexts[i]->stats.instrs;
  }
  printf("total IPC %s/%s = %.3f\n", fmt_i64(total), fmt_i64(cyc - warmupcyc),
         (double) total / (cyc - warmupcyc));
}


/* Reset all stats to zero */

void 
zero_pipe_stats(void)
{
    for (int i = 0; i < NELEM(instrcount); i++) {
        instrcount[i] = 0;
    }
    for (int i = 0; i < CtxCount; i++) { 
        context *ctx = Contexts[i];
        // Lazy
        memset(&ctx->stats, 0, sizeof(ctx->stats));
    }
    for (int i = 0; i < CoreCount; i++) { 
        CoreResources *core = Cores[i];
        // Lazy
        memset(&core->q_stats, 0, sizeof(core->q_stats));
    }
    misfetchtotal = flushed = 0;
    warmupcyc = cyc;
    wpexec = execflushed = 0;
}


/* Doesn't actually reset anything, it just caches some
   old values so that it can compute parallelism over a
   particular interval.   Only used for embedded
   PRINT_SIM directives. */

void
reset_stats(void)
{
  int i;

  oldtotal = 0;
  for (i=0;i<CtxCount;i++) { 
    oldtotal += Contexts[i]->stats.instrs;
    Contexts[i]->stats.oldinstructions = Contexts[i]->stats.instrs;
  }
  oldcycles = cyc;
  resetflag = 1;
}


// Update any relevant "writer" and "lasthazard" entries to reflect when
// this instruction's result will be available
void
update_writers(struct activelist * restrict inst)
{
    context * restrict ctx = Contexts[inst->thread];

    sim_assert(inst->donecycle != MAX_CYC);

    if (!IS_ZERO_REG(inst->dest)) {
        // Update a context's writer[]: if "inst" is the last writer, mark the
        // register as available when inst is done.
        if (ctx->last_writer[inst->dest] == inst->id) {
            ctx->lasthazard[inst->dest] = inst->donecycle;
        }
    }
}


// Release the instructions that are waiting on this instruction for data.  If
// this is the last outstanding dependence for a waiter, then mark the waiter
// "ready".
int
release_waiters(struct activelist * restrict inst)
{
    int awakened = 0;

    sim_assert(inst->donecycle != MAX_CYC);
    sim_assert(!(inst->status & (INVALID | SQUASHED)));

    for (int i=0; i < inst->numwaiting; i++) {
        activelist * restrict waitinst = inst->waiter[i];
        inst->waiter[i] = 0;
        // Squashed instructions may be left in waiter[], but have deps == 0
        if (waitinst->deps > 0) {
            if (waitinst->readycycle < inst->donecycle)
                waitinst->readycycle = inst->donecycle;
            waitinst->deps--;
            if (waitinst->deps == 0) {
                waitinst->status &= ~BLOCKED;
                awakened++;
            }

            // Be sure to unlink the dependent child we just satisfied,
            // to avoid the case where "inst" is committed and freed, then
            // "waitinst" is squashed and checks the "*_waitingfor" pointers.
            if (waitinst->src1_waitingfor == inst) {
                waitinst->src1_waitingfor = NULL;
            } else if (waitinst->src2_waitingfor == inst) {
                waitinst->src2_waitingfor = NULL;
            } else {
                abort_printf("%s: dependence mismatch, producer inst T%ds%d "
                             "not reverse-linked from waiter T%ds%d\n",
                             __func__, inst->thread, inst->id,
                             waitinst->thread, waitinst->id);
            }
        } else {
            sim_assert(waitinst->deps == 0);
            sim_assert(waitinst->status == SQUASHED);
        }

    }
    inst->numwaiting = 0;

    return awakened;
}


// Remove "waiter" from the waiters[] array of "writer".
void
remove_waiter(activelist * restrict writer,
              const activelist * restrict waiter)
{
    int found_idx;
    for (found_idx = 0; found_idx < writer->numwaiting; ++found_idx) {
        if (writer->waiter[found_idx] == waiter) {
            break;
        }
    }
    if (found_idx < writer->numwaiting) {
        // It might be more efficient to just NULL this waiter[] entry
        // out and then have release_waiters() skip it.  For now, we'll
        // do the dumb-but-straightforward thing, instead of complicating
        // that "data structure".
        int ents_to_move = writer->numwaiting - found_idx - 1;
        memmove(&writer->waiter[found_idx], &writer->waiter[found_idx + 1],
                ents_to_move * sizeof(writer->waiter[found_idx]));
        writer->numwaiting--;
    } else {
        // "waiter" wasn't present in "writer"; something's gone wrong
        abort_printf("%s: waiter T%ds%d missing from writer T%sd%s!\n",
                     __func__, waiter->thread, waiter->id,
                     writer->thread, writer->id);
    }
}


static i64
longmem_test_cb(void *inst_ptr, CBQ_Args *invoke_args_ignored)
{
    activelist * restrict inst = (activelist *) inst_ptr;
    long_mem_detect(Contexts[inst->thread], inst, 0);
    return 0;
}


/*  There are several flavors of the resolve routine for various
 *    types of instructions.  We use the latency of this instruction
 *    to figure out when dependent instructions could be
 *    issued, if indeed this is the last unresolved dependence.
 *  Of course, this is seriously complicated by the fact that we
 *    continue to track and obey dependences even when we are
 *    executing down wrong paths.
 *  A memory instruction will come through the resolve() routine
 *    initially, but if it misses in the cache and has to go through
 *    the memory event queue, it eventually comes back to the 
 *    mem_resolve routines().
 */

// (XXX) It's really cheating to access the cache in resolve() and in 
// synchresolve(), when the instruction is being issued from the queue, because
// the proper memory address won't be "known" until after it's computed in
// execution.  Changing this would require the addition of load hit
// speculation and speculative issue, which we don't support right now.
// So, we cheat and access the cache here, but at least delay acting on the 
// result until one cycle after the execute stage generates the address.

static void resolve(CoreResources * restrict core, struct activelist *exinst)
{
  int dmemdelay;
  const int ex_stage_delay = Q_RR_CYC(core) + exinst->delay;
  const i64 addr_ready_cyc = cyc + ex_stage_delay;
  context *current = Contexts[exinst->thread];

  exinst->status = EXECUTING;

  if (exinst->mem_flags & SMF_Read) {
    DEBUGPRINTF("T%ds%d mem read issued at %s, va %s, addr ready at %s\n",
                current->id, exinst->id, fmt_now(), fmt_x64(exinst->srcmem),
                fmt_i64(addr_ready_cyc));
    dmemdelay = dodaccess(exinst->srcmem, 0, current, exinst, addr_ready_cyc);
#ifdef DEBUG
    if (dmemdelay == MEMDELAY_LONG) {
        DEBUGPRINTF("T%ds%d data cache/dtlb miss, R%d, read 0x%s\n",
                    current->id, exinst->id, exinst->dest,
                    fmt_x64(exinst->srcmem));
    } else if (dmemdelay) {
        DEBUGPRINTF("T%ds%d data cache delay %d cycles, R%d, read 0x%s\n",
                    current->id, exinst->id, dmemdelay, exinst->dest,
                    fmt_x64(exinst->srcmem));
    }
#endif
  } else if (exinst->mem_flags & SMF_Write) {
    DEBUGPRINTF("T%ds%d mem write issued at %s, va %s, addr ready at %s\n",
                current->id, exinst->id, fmt_now(), fmt_x64(exinst->destmem),
                fmt_i64(addr_ready_cyc));
    dmemdelay = dodaccess(exinst->destmem, 1, current, exinst, addr_ready_cyc);
  } else if (exinst->bmt.spillfill) {
      dmemdelay = 0;
      if (exinst->bmt.spillfill & BmtSF_BlockMarker) {
          i64 ready_time = cyc;
          if (!(exinst->bmt.spillfill & BmtSF_FreeTransfer)) {
              ready_time =
                  corebus_access(core->reply_bus,
                                 GlobalParams.mem.bus_transfer_time);
          }
          current->bmt_spillfill_blockready = ready_time;
          DEBUGPRINTF("T%ds%d BMT %s, block ready at %s\n",
                      current->id, exinst->id,
                      (exinst->bmt.spillfill & BmtSF_Spill) ? "spill" : "fill",
                      fmt_i64(ready_time));
      }
      if (exinst->bmt.spillfill & BmtSF_Fill) {
          // All fills have to wait for the data to become ready
          if (current->bmt_spillfill_blockready > cyc)
              dmemdelay = current->bmt_spillfill_blockready - cyc;
      } else if ((exinst->bmt.spillfill & BmtSF_Spill) &&
                 (exinst->bmt.spillfill & BmtSF_BlockMarker)) {
          // Delay only the final spill in the block until it gets the bus
          i64 start_time = current->bmt_spillfill_blockready - 
              GlobalParams.mem.bus_transfer_time.latency;
          if (start_time > cyc)
              dmemdelay = start_time - cyc;
      }
  } else {
    // No memory operation
    dmemdelay = 0;
  }

  // If dmemdelay is MEMDELAY_LONG, the memory request was placed in the
  // memory simulator and will be handed to mem_resolve() when it's completed
  // at some unknown future time.  Otherwise, we have a definite completion
  // time, and we can schedule our waiters for release.

  if (dmemdelay == MEMDELAY_LONG) {
      exinst->donecycle = MAX_CYC;      
  } else {
      // For non-memory instructions, "addr_ready_cyc" is the cycle in
      // which the output value will be ready (since dmemdelay==0).
      // 
      // (note: latency also calculated in cache.c, dcache_sim.latency = ...)
      exinst->donecycle = addr_ready_cyc + dmemdelay;
      DEBUGPRINTF("T%ds%d resolved(1), donecycle %s\n", exinst->thread,
                  exinst->id, fmt_i64(exinst->donecycle));
      update_writers(exinst);
      release_waiters(exinst);
  }

  if (!GlobalParams.long_mem_at_commit &&
      (GlobalParams.long_mem_cyc > 0) &&
      ((exinst->donecycle - cyc) >= GlobalParams.long_mem_cyc)) {
      // Looks like this will be a long memory op; schedule a callback to test
      // when we "should" be able to realize it.  (Warning: this is old
      // code that may act funny if the thread was halted in the mean-time.)
      CBQ_Callback *test_cb_obj = callback_c_create(longmem_test_cb, exinst);
      callbackq_enqueue(GlobalEventQueue, cyc + GlobalParams.long_mem_cyc, 
                        test_cb_obj);
  }
}


static void brresolve(CoreResources * restrict core, activelist *exinst)
{
  exinst->status = EXECUTING;
  exinst->donecycle = cyc + Q_RR_CYC(core) + exinst->delay;
  DEBUGPRINTF("T%ds%d resolved(2), donecycle %s\n", exinst->thread,
              exinst->id, fmt_i64(exinst->donecycle));

  update_writers(exinst);
  release_waiters(exinst);
}


static void synchresolve(CoreResources * restrict core, 
                         struct activelist *exinst)
{
  int dmemdelay;
  const int ex_stage_delay = Q_RR_CYC(core) + exinst->delay;
  context *current = Contexts[exinst->thread];
  int syncop = exinst->syncop;

  exinst->status = EXECUTING;

  if (exinst->mem_flags & SMF_Read) {
    // SMT_HW_LOCK, LDL_L, LDQ_L
    sim_assert((syncop == SMT_HW_LOCK) || (syncop == LDL_L) ||
               (syncop == LDQ_L));
    dmemdelay = dodaccess(exinst->srcmem, 0, current, exinst,
                          cyc + ex_stage_delay);
#ifdef DEBUG
    if (dmemdelay) {
      if (dmemdelay == MEMDELAY_LONG) {
        DEBUGPRINTF("T%ds%d synch data cache or tlb miss, R%d, read 0x%s\n",
                    current->id, exinst->id, exinst->dest,
                    fmt_x64(exinst->srcmem));
      } else {
        DEBUGPRINTF("T%ds%d synch data cache or tlb miss %s cycles, R%d, "
                    "read 0x%s\n", current->id, exinst->id, fmt_i64(dmemdelay),
                    exinst->dest, fmt_x64(exinst->srcmem));
      }
    }
#endif
  } else if (exinst->mem_flags & SMF_Write) {
    // STL_C, STQ_C, SMT_RELEASE
    sim_assert((syncop == SMT_RELEASE) || (syncop == STL_C) ||
               (syncop == STQ_C));
    dmemdelay = dodaccess(exinst->destmem, 1, current, exinst,
                          cyc + ex_stage_delay);
  } else {
    // SMT_TERMINATE
    // crufty: non-TERMINATE instructions will end up here if they have mem 
    //   address 0, which can happen on a wrong path
    //sim_assert(syncop == SMT_TERMINATE);
    dmemdelay = 0;
  }

  if (dmemdelay == MEMDELAY_LONG) {
      exinst->donecycle = MAX_CYC;      
  } else {
      exinst->donecycle = cyc + ex_stage_delay + dmemdelay;
      DEBUGPRINTF("T%ds%d resolved(3), donecycle %s\n", exinst->thread,
                  exinst->id, fmt_i64(exinst->donecycle));
      // We'll only mark dependences on the result of exinst as ready for
      // non-store-conditionals; for STL_C/STQ_C, the value is not made ready
      // until commit, and dependences are marked from synchexecute().
      if ((syncop != STL_C) && (syncop != STQ_C)) {
          // We used to only update_writers() when we had srcmem; we'll assert
          // that anything that's a non-conditional-store SYNCH operation 
          // either has srcmem, or has no destination regs so that the call
          // to update_writers() doesn't change anything.
          sim_assert((exinst->mem_flags & SMF_Read) ||
                     IS_ZERO_REG(exinst->dest));
          update_writers(exinst);

          // We used to only release_waiters() for LD[LQ]_L; we'll assert
          // anything that's a non-conditional-store SYNCH operation is either
          // a load-locked, or has no waiters to release so that the call
          // to release_waiters() doesn't change anything.
          sim_assert((syncop == LDL_L) || (syncop == LDQ_L) || 
                 (exinst->numwaiting == 0));
          release_waiters(exinst);
      }
  }
}

/* Unlike the other resolve routines which get called at issue, 
 *  the mem_resolve routines cannot execute until after the request
 *  has made its way through the memory hierarchy (for a miss),
 *  that is, through our memory event queues.
 */

void synch_mem_resolve(CoreResources * restrict core,
                       struct activelist *exinst, i64 donetime)
{
    const int ex_stage_delay = Q_RR_CYC(core) + exinst->delay;
    exinst->donecycle = donetime;       // exinst->delay paid at resolve()
    if (exinst->donecycle < (exinst->issuecycle + ex_stage_delay))
        // Completing too early (due to D-merge with a completing request)
        exinst->donecycle = exinst->issuecycle + ex_stage_delay;
    DEBUGPRINTF("T%ds%d synch memory access complete at %s\n",
                exinst->thread, exinst->id, fmt_i64(exinst->donecycle));
    update_writers(exinst);
    release_waiters(exinst);
    exinst->status = EXECUTING;
}


void mem_resolve(CoreResources * restrict core,
                 struct activelist *exinst, i64 donetime)
{
    const int ex_stage_delay = Q_RR_CYC(core) + exinst->delay;
    exinst->donecycle = donetime;       // exinst->delay paid at resolve()
    if (exinst->donecycle < (exinst->issuecycle + ex_stage_delay))
        // Completing too early (due to D-merge with a completing request)
        exinst->donecycle = exinst->issuecycle + ex_stage_delay;
    DEBUGPRINTF("T%ds%d memory access complete at %s\n",
                exinst->thread, exinst->id, fmt_i64(exinst->donecycle));
    update_writers(exinst);
    release_waiters(exinst);
    exinst->status = EXECUTING;
}


static int
mem_barrier_wait(CoreResources *core, const activelist *new)
{
    int wait_mb = 0, wait_wmb = 0;

    if (new->mem_flags) {
        if (core->mb.current_epoch < new->mb_epoch) {
            wait_mb = 1;
            core->q_stats.mb_conf++;
        }
        if ((new->mem_flags & SMF_Write) &&
            (core->wmb.current_epoch < new->wmb_epoch)) {
            wait_wmb = 1;
            if (!wait_mb)
                core->q_stats.wmb_conf++;
        }
    }

    if (DEBUG_MEM_BARRIERS && debug) {
        if (wait_mb || wait_wmb) {
            printf("T%is%i mem barrier: mb %i (%s < %s), wmb %i (%s < %s)\n",
                   new->thread, new->id, wait_mb, 
                   fmt_i64(core->mb.current_epoch), fmt_i64(new->mb_epoch),
                   wait_wmb, fmt_i64(core->wmb.current_epoch), 
                   fmt_i64(new->wmb_epoch));
        }
    }

    return wait_mb || wait_wmb;
}


static int
mshr_wait(CoreResources *core, const activelist *new)
{
    int wait = 0;
    if (new->mem_flags) {
        LongAddr data_addr;
        if (new->mem_flags & SMF_Read) {
            laddr_set(data_addr, new->srcmem, new->as->app_master_id);
        } else {
            laddr_set(data_addr, new->destmem, new->as->app_master_id);
        }
        if (!mshr_is_avail(core->data_mshr, data_addr)) {
            wait = 1;
            core->q_stats.d_mshr_conf++;
        }
    }
    return wait;
}


// test: should we stall issue of this memory instruction, this cycle?
// warning: may be called multiple times per instruction in a cycle;
// "allow_stats_update" flag must only be set on one of these calls.
static int
mem_conflict_wait(CoreResources * restrict core, int core_thread_id,
                  const activelist * restrict meminst,
                  int allow_stats_update)
{
    int stall_issue;
    if (meminst->mem_flags & SMF_Read) {
        int hash_idx = (meminst->srcmem >> 2) % NELEM(core->last_store_hash);
        u32 thread_bit = SET_BIT_32(core_thread_id % 32);
        // stall issue of this load, if this thread MAY have already issued a
        // write to this address in this cycle.  (for later cycles, we assume
        // that the cache simulator takes care of things)  This is subject
        // to false-positives, and does not properly handle inter-cycle
        // dependences.
        stall_issue = (core->last_store_hash[hash_idx].cyc == cyc) &&
            (core->last_store_hash[hash_idx].thread_id_mask & thread_bit);
    } else {
        stall_issue = 0;
    }
    if (allow_stats_update && stall_issue)
        core->q_stats.memconf++;
    return stall_issue;
}


static void
mem_conflict_write_issued(CoreResources * restrict core, int core_thread_id,
                         const activelist * restrict meminst)
{
    int hash_idx = (meminst->destmem >> 2) % NELEM(core->last_store_hash);
    u32 thread_bit = SET_BIT_32(core_thread_id % 32);
    if (core->last_store_hash[hash_idx].cyc == cyc) {
        // some thread has written to this bucket this cycle; add to bit mask
        core->last_store_hash[hash_idx].thread_id_mask |= thread_bit;
    } else {
        // this is the first write to this bucket, this cycle; reset bit mask
        core->last_store_hash[hash_idx].cyc = cyc;
        core->last_store_hash[hash_idx].thread_id_mask = thread_bit;
    }
}


/*Tracks store-load memory conflicts.  It does so precisely, but
  only using part of the address to make it a bit conservative.
*/


/* The main action of the queue stage is just searching through
 *  the two instruction queues for instructions to issue.
 *  Unfortunately there is no real shortcut to a linear search
 *  here which makes this a particularly slow part of the code.
 */

static void
queue_for_core(CoreResources * restrict core)
{
    const int max_int_issue = core->params.queue.max_int_issue;
    const int max_float_issue = core->params.queue.max_float_issue;
    const int max_ldst_issue = core->params.queue.max_ldst_issue;
    const int max_sync_issue = core->params.queue.max_sync_issue;
    const int int_ooo_issue = core->params.queue.int_ooo_issue;
    const int float_ooo_issue = core->params.queue.float_ooo_issue;
    // The cycle after regread will have finished, if we issue now
    const i64 rr_done_cyc = cyc + Q_RR_CYC(core);
    const int rread1 = core->stage.rread1;
    activelist * restrict new;
    activelist * restrict prev;
    int intissue=0, fpissue=0, ldstissue=0, synchissue=0;
    int i;
    // used to make sure that synch instructions do not pass memory 
    // operations in the queue
    int *mem_acc_in_q = (int *)emalloc_zero(CtxCount*sizeof(int)); 
    
    int *iqueue_sel = (int *)emalloc_zero(CtxCount*sizeof(int));
    int *fqueue_sel = (int *)emalloc_zero(CtxCount*sizeof(int));

    int totalconfs_this_cyc = 0;

    new = stageq_head(core->stage.intq);
    prev = NULL;

    while (new != NULL) {
        const int new_ready = !(new->status & BLOCKED) &&
            (new->readycycle <= rr_done_cyc);
        int new_issued = 0;

        iqueue_sel[new->thread] = 1;
        context * restrict new_ctx = Contexts[new->thread];
        /* recall--new->status & BLOCKED is set unless the instruction's
           dependencies have all been resolved.  There are an awful
           lot of other reasons that an instruction may not be 
           issuable, however.
        */
        if (new_ready && 
            (intissue < max_int_issue) &&
            !new->wait_sync &&
            !(new->fu == INTLDST && 
              (ldstissue >= max_ldst_issue)) &&
            !(new->fu == SYNCH && 
              ((synchissue >= max_sync_issue) || 
               mem_acc_in_q[new->thread]))
            && !mem_conflict_wait(core, new_ctx->core_thread_id, new, 1)
            && !mshr_wait(core, new) && !mem_barrier_wait(core, new)) {
            new_issued = 1;
            intissue++;
            if (new->fu == INTLDST)
                ldstissue++;
            if (new->fu == SYNCH)
                synchissue++;
#ifdef PRIO_INST_COUNT
            core->sched.key[new_ctx->core_thread_id]--;
#else
#ifdef PRIO_BR_COUNT
            if (new->branch || new->cond_branch) {
                core->sched.key[new_ctx->core_thread_id]--;
            }
#endif
#endif
            if (new->wp) wpexec++;
            if (new->br_flags)
                brresolve(core, new);
            else if (new->fu == SYNCH) {
                synchresolve(core, new);
            }
            else
                resolve(core, new);
        } else { /* new not scheduled */
          if (new_ready) {
            totalconfs_this_cyc++;
          }
          /*expensive stats -- think about commenting out */
          if (new_ready && 
              !new->wait_sync &&
              !(new->fu == SYNCH && 
                (synchissue >= max_sync_issue))
              && !mem_conflict_wait(core, new_ctx->core_thread_id, new, 0)) {
            if (intissue >= max_int_issue)
              core->q_stats.intfuconf++;
            if (new->fu == INTLDST &&
                (ldstissue >= max_ldst_issue))
              core->q_stats.ldstfuconf++;
          }
          /* establish an implied memory barrier to use in scheduling
             release operations */
          if (new->mem_flags)
              mem_acc_in_q[new->thread] = 1;

          if (new->mem_flags & SMF_Write) {
              mem_conflict_write_issued(core, new_ctx->core_thread_id, new);
          }
        }

        if (new_issued) {
            update_acc_occ_per_inst(new_ctx, new, 2, 0);  
            update_adapt_mgr_dec_tentative(new_ctx, IQ, 1);
            new->issuecycle = cyc;
            stageq_delete(core->stage.intq, prev);
            stageq_enqueue(core->stage.s[rread1], new);
        } else {
            prev = new;
            if (!int_ooo_issue)
                break;
        }
        new = (prev) ? prev->next : stageq_head(core->stage.intq);
      }


  /* do pretty much the same things for the fp queue */

      new = stageq_head(core->stage.floatq);
      prev = NULL;

      while (new) {
          const int new_ready = !(new->status & BLOCKED) &&
              (new->readycycle <= rr_done_cyc);
          int new_issued = 0;
          context * restrict new_ctx = Contexts[new->thread];


        fqueue_sel[new->thread] = 1;
        
        if ((fpissue < max_float_issue) &&
            new_ready) {
          new_issued = 1;
          fpissue++;
#ifdef PRIO_INST_COUNT
          core->sched.key[new_ctx->core_thread_id]--;
#else
#ifdef PRIO_BR_COUNT
          if (new->branch || new->cond_branch) {
              core->sched.key[new_ctx->core_thread_id]--;
          }
#endif
#endif
          if (new->wp) wpexec++;
          if (new->br_flags)
            brresolve(core, new);
          else
            resolve(core, new); 
        } else { /* not scheduled */
          if (new_ready) {
            totalconfs_this_cyc++;
            if (fpissue >= max_float_issue)
              core->q_stats.fpfuconf++;
          }
        }

        if (new_issued) {
            new->issuecycle = cyc;
            update_acc_occ_per_inst(new_ctx, new, 2, 1);  
            update_adapt_mgr_dec_tentative(new_ctx, FQ, 1);
            stageq_delete(core->stage.floatq, prev);
            stageq_enqueue(core->stage.s[rread1], new);
        } else {
            prev = new;
            if (!float_ooo_issue)
                break;
        }
        new = (prev) ? prev->next : stageq_head(core->stage.floatq);
      }

      for (i = 0; i < CtxCount; i++)
      {
        context * restrict ctx = Contexts[i];
        if (iqueue_sel[i] && ctx->as != NULL)
            ctx->as->extra->iq_acc++;
        if (fqueue_sel[i] && ctx->as != NULL)
            ctx->as->extra->fq_acc++;
      }
      
      instrcount[INTEGER] += intissue - ldstissue - synchissue;
      instrcount[SYNCH] += synchissue;
      instrcount[INTLDST] += ldstissue;
      instrcount[FP] += fpissue;

      core->q_stats.ialuissuetotal += intissue - ldstissue - synchissue;
      core->q_stats.ldstissuetotal += ldstissue;
      core->q_stats.faluissuetotal += fpissue;
      
      if (totalconfs_this_cyc) {
          int lg_index = floor_log2(totalconfs_this_cyc, NULL);
          if (lg_index >= NELEM(core->q_stats.totalconf_lg_cyc))
              lg_index = NELEM(core->q_stats.totalconf_lg_cyc) - 1;
          core->q_stats.totalconf_lg_cyc[lg_index]++;
          core->q_stats.total_conf += totalconfs_this_cyc;
      }

    // Do not return earlier or it will leak
    free(mem_acc_in_q);
    free(iqueue_sel);
    free(fqueue_sel);

}


void
queue(void)
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources *core = Cores[core_id];
        queue_for_core(core);
    }
}
