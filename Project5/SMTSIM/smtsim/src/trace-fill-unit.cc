//
// Trace Fill Unit
//
// Jeff Brown
// $Id: trace-fill-unit.cc,v 1.2.2.5.2.1 2008/04/30 22:18:01 jbrown Exp $
//

const char RCSid_1054173444[] =
"$Id: trace-fill-unit.cc,v 1.2.2.5.2.1 2008/04/30 22:18:01 jbrown Exp $";

#include <stdio.h>

#include <vector>
#include <deque>

#include "sim-assert.h"
#include "sys-types.h"
#include "trace-fill-unit.h"
#include "trace-cache.h"
#include "main.h"
#include "core-resources.h"
#include "utils.h"
#include "branch-bias-table.h"
#include "dyn-inst.h"
#include "stash.h"
#include "context.h"
#include "app-state.h"


#if defined(DEBUG)
#   define TFILL_DEBUG          (debug)
#else
#   define TFILL_DEBUG          0
#endif
#define TFPRINTF                if (!TFILL_DEBUG) { } else printf


using std::vector;
using std::deque;


namespace {

void 
split_block(TraceCacheBlock& to_split, int split_idx, TraceCacheBlock& remain)
{
    TFPRINTF("TFU split: split %s", tcb_format(&to_split));
    sim_assert((split_idx > 0) && (split_idx < to_split.inst_count));
    tcb_reset(&remain);
    remain.thread_id = to_split.thread_id;

    for (int i = split_idx; i < to_split.inst_count; i++) {
        TraceCacheInst& inst = to_split.insts[i];
        remain.insts[remain.inst_count] = inst;
        if (TBF_UsesPredict(inst.br_flags))
            remain.pred_count++;
        remain.inst_count++;
    }

    if (remain.pred_count > 0) {
        to_split.pred_count -= remain.pred_count;
        remain.predict_bits = to_split.predict_bits >> to_split.pred_count;
        to_split.predict_bits = GET_BITS_32(to_split.predict_bits, 0,
                                            to_split.pred_count);
    } else {
        remain.predict_bits = 0;
    }
    
    to_split.inst_count -= remain.inst_count;
    to_split.fallthrough_pc = to_split.insts[to_split.inst_count - 1].pc + 4;

    TFPRINTF(", into %s", tcb_format(&to_split));
    TFPRINTF(", and %s\n", tcb_format(&remain));
}

int
find_last_branch(const TraceCacheBlock& blk)
{
    for (int i = blk.inst_count - 1; i >= 0; i--) {
        if (blk.insts[i].br_flags)
            return i;
    }
    return -1;
}

}


struct TraceFillUnit {
    typedef vector<TraceCacheBlock *> TCBlockVec;
    typedef deque<TraceCacheBlock *> TCBlockDQ;
    
private:
    TraceFillUnitParams params;
    CoreResources *core;
    TraceCache *tc;
    BranchBiasTable *bbt;
    TraceCacheParams tc_params;

    // Pending block: the block under construction
    TraceCacheBlock *pend_blk;
    int pend_thread;            // Pending thread id, _not_ master ID
    bool pend_full;
    TraceCacheBlock *split_remain;

    // Queue: completed blocks waiting to transfer to TC; blocks move back and
    // forth between these two vectors.
    TCBlockDQ output_ready_blks;
    TCBlockVec output_free_blks;
    i64 last_output_cyc;

    void flush_pending() {
        tcb_reset(pend_blk);
        pend_thread = -1;
        pend_full = false;
    }

    bool pend_empty() const {
        return pend_blk->inst_count == 0;
    }

    bool ends_trace(const activelist *alist_inst) const {
        return SBF_ReadsRetStack(alist_inst->br_flags) ||
            (alist_inst->gen_flags & SGF_SysCall) ||
            (!params.allow_indirect_jumps && 
             SBF_IndirectBranch(alist_inst->br_flags));
    }

    void do_add_inst(const TraceCacheInst& inst, 
                     const activelist *alist_inst,
                     mem_addr fallthrough_pc) {
        sim_assert(pend_thread >= 0);
        sim_assert(!pend_full);
        sim_assert(pend_blk->inst_count < tc_params.block_insts);
        pend_blk->insts[pend_blk->inst_count] = inst;
        pend_blk->inst_count++; 
        if (ends_trace(alist_inst) || 
            (pend_blk->inst_count == tc_params.block_insts) ||
            (pend_blk->pred_count == tc_params.pred_per_block))
            pend_full = true;
        if (pend_full)
            pend_blk->fallthrough_pc = fallthrough_pc;
    }
    
    void add_regular_inst(const activelist *alist_inst, mem_addr nextpc) {
        TraceCacheInst tc_inst;
        tc_inst.pc = alist_inst->pc;
        tc_inst.target_pc = 0;
        tc_inst.br_flags = TBF_NotABranch;
        tc_inst.cgroup_flags = 0;
        do_add_inst(tc_inst, alist_inst, nextpc);
    }

    void add_br_inst(const activelist *alist_inst, mem_addr fallthrough) {
        const bool taken = alist_inst->taken_branch;
        const bool multi_targ = !SBF_StaticTarget(alist_inst->br_flags);

        TraceCacheInst tc_inst;
        tc_inst.pc = alist_inst->pc;
        tc_inst.target_pc = alist_inst->br_target;
        tc_inst.br_flags = (taken) ? TBF_Br_Taken : TBF_Br_NotTaken;
        tc_inst.cgroup_flags = 0;

        bool promote = false;
        if (params.branch_promote_thresh) {
            int bbt_taken;
            i32 bbt_count;
            if (!alist_inst->skipped_bpredict &&
                bbt_lookup(bbt, alist_inst->pc, alist_inst->thread,
                           &bbt_taken, &bbt_count) &&
                (bbt_taken == taken) && 
                (bbt_count >= params.branch_promote_thresh)) {
                TFPRINTF("TFU promoting T%i pc %s, taken %i count %s\n",
                         alist_inst->thread, fmt_x64(alist_inst->pc),
                         bbt_taken, fmt_i64(bbt_count));
                promote = true;
            }
        }

// Non-sticky: if (!SBF_CondBranch(alist_inst->br_flags) || promote) {
        if (alist_inst->skipped_bpredict || promote) {
            tc_inst.br_flags |= TBF_SkipPredict;
        } else {
            sim_assert(pend_blk->pred_count < tc_params.pred_per_block);
            if (taken)
                pend_blk->predict_bits |= 
                    static_cast<u32>(1) << pend_blk->pred_count; 
            pend_blk->pred_count++;
        }

        if (multi_targ)
            tc_inst.br_flags |= TBF_MultiTarg;

        do_add_inst(tc_inst, alist_inst, fallthrough);
    }

    void do_fill() {
        if (output_free_blks.empty()) {
            // output queue full
            TFPRINTF("TFU fifo full, dropping block: %s\n",
                     tcb_format(pend_blk));
        } else {
            TraceCacheBlock *queue_ent = output_free_blks.back();
            output_free_blks.pop_back();
            tcb_assign(queue_ent, pend_blk);
            output_ready_blks.push_back(queue_ent);
        }
        flush_pending();
    }

private:
    // Disallow copy or assignment
    TraceFillUnit(const TraceFillUnit& src);
    TraceFillUnit& operator = (const TraceFillUnit &src);

public:
    TraceFillUnit(const TraceFillUnitParams& params_, CoreResources *core_);
    ~TraceFillUnit();

    void reset();

    inline void inst_commit(const struct context *ctx,
                            const struct activelist *alist_inst) {
        int thread = alist_inst->thread;
        mem_addr nextpc = alist_inst->pc + 4;

        if (pend_empty()) {
            pend_thread = thread;
            pend_blk->thread_id = ctx->as->app_master_id;
        } else if (pend_thread != thread) {
            return;
        }

        if (alist_inst->br_flags) {
            add_br_inst(alist_inst, nextpc);
        } else {
            add_regular_inst(alist_inst, nextpc);
        }

        if (pend_full) {
            int split_idx = -1;
            int split_thread = pend_thread;
            if (params.align_to_bblock &&
                (pend_blk->inst_count == tc_params.block_insts) &&
                (pend_blk->pred_count < tc_params.pred_per_block) &&
                !ends_trace(alist_inst)) {
                split_idx = find_last_branch(*pend_blk);
                if (split_idx == (pend_blk->inst_count - 1))
                    split_idx = -1;
                if (split_idx >= 0)
                    split_idx++;
            }
            if (split_idx > 0) {
                sim_assert(!split_remain->inst_count);
                split_block(*pend_blk, split_idx, *split_remain);
                sim_assert(split_remain->inst_count);
            }
            do_fill();
            if (split_idx > 0) {
                tcb_assign(pend_blk, split_remain);
                pend_thread = split_thread;
                split_remain->inst_count = 0;
            }
        }
    }

    void context_threadswap(const context *ctx) {
        if (!pend_empty() && (pend_thread == ctx->id))
            flush_pending();
    }

    inline void process_queue() {
        while (!output_ready_blks.empty() &&
               ((last_output_cyc + params.output_interval) <= cyc)) {
            TraceCacheBlock *next_blk = output_ready_blks.front();
            output_ready_blks.pop_front();
            tc_fill(tc, next_blk);
            tcb_reset(next_blk);
            output_free_blks.push_back(next_blk);
            last_output_cyc = cyc;
        }
    }
};


TraceFillUnit::TraceFillUnit(const TraceFillUnitParams& params_,
                             CoreResources *core_)
    : core(0), pend_blk(0), split_remain(0)
{
//    const char *func = "TraceFillUnit::TraceFillUnit";
    params = params_;
    sim_assert(params.output_fifo_len > 0);
    sim_assert(params.output_interval >= 0);
    sim_assert(params.branch_promote_thresh >= 0);

    core = core_;
    tc = core->tcache;
    bbt = core->br_bias;
    tc_get_params(tc, &tc_params);
    pend_blk = tcb_alloc(tc);
    split_remain = tcb_alloc(tc);

    for (int i = 0; i < params.output_fifo_len; i++)
        output_free_blks.push_back(tcb_alloc(tc));

    reset();
}


TraceFillUnit::~TraceFillUnit()
{
    tcb_free(pend_blk);
    tcb_free(split_remain);
    while (!output_free_blks.empty()) {
        tcb_free(output_free_blks.back());
        output_free_blks.pop_back();
    }
    while (!output_ready_blks.empty()) {
        tcb_free(output_ready_blks.back());
        output_ready_blks.pop_back();
    }
}


void
TraceFillUnit::reset() 
{
    tcb_reset(pend_blk);
    pend_thread = -1;
    pend_full = false;
    tcb_reset(split_remain);

    while (!output_ready_blks.empty()) {
        TraceCacheBlock *blk = output_ready_blks.back();
        output_ready_blks.pop_back();
        tcb_reset(blk);
        output_free_blks.push_back(blk);
    }

    last_output_cyc = 0;
}


//
// C interface
//

TraceFillUnit *
tfu_create(const TraceFillUnitParams *params, CoreResources *core)
{
    return new TraceFillUnit(*params, core);
}


void 
tfu_destroy(TraceFillUnit *tfu)
{
    if (tfu)
        delete tfu;
}


void 
tfu_reset(TraceFillUnit *tfu)
{
    tfu->reset();
}


void 
tfu_inst_commit(TraceFillUnit *tfu, const struct context *ctx,
                const struct activelist *inst)
{
    tfu->inst_commit(ctx, inst);
}


void
tfu_context_threadswap(TraceFillUnit *tfu, const struct context *ctx)
{
    tfu->context_threadswap(ctx);
}


void 
tfu_process_queue(TraceFillUnit *tfu)
{
    tfu->process_queue();
}
