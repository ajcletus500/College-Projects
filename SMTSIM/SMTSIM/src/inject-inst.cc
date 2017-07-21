//
// Support for injecting instructions into the pipeline
//
// Jeff Brown
// $Id: inject-inst.cc,v 1.1.2.4.2.1.2.5 2008/10/29 09:33:36 jbrown Exp $
//

const char RCSid_1113588013[] =
"$Id: inject-inst.cc,v 1.1.2.4.2.1.2.5 2008/10/29 09:33:36 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "inject-inst.h"
#include "core-resources.h"
#include "app-state.h"
#include "dyn-inst.h"
#include "context.h"
#include "utils.h"
#include "stash.h"
#include "main.h"
#include "app-mgr.h"


struct activelist *
inject_alloc(struct context * restrict ctx)
{
    int next_index = alist_add(ctx, ctx->alisttop, 1);
    if (!(ctx->alist[next_index].status & INVALID))
        return NULL;
    ctx->alisttop = next_index;
    activelist * restrict top = &ctx->alist[next_index];

    // Tweaked version of setup_instruction(), to prevent leakage of info
    // from previous user of this slot
    top->fu = INTEGER;
    top->src1 = top->src2 = top->dest = IZERO_REG;
    top->br_flags = SBF_NotABranch;
    top->gen_flags = SGF_None;
    top->mem_flags = SMF_NoMemOp;
    top->syncop = 0;
    top->delay = 1;
    top->srcmem = top->destmem = 0;

    top->donecycle = MAX_CYC;
    top->iregs_used = 0;
    top->fregs_used = 0;
    top->robentry = 0;
    top->status = FETCHED;
    top->mispredict = MisPred_None;
    top->misfetch = MisPred_None;
    top->deps = 0;
    top->wait_sync = 0;
    top->thread = ctx->id;
    top->as = NULL;
    top->pc = 0;
    top->mb_epoch = ctx->core->mb.fetch_epoch;
    top->wmb_epoch = ctx->core->wmb.fetch_epoch;
    top->app_inst_num = -1;
    top->insts_discarded_before = 0;
    top->wp = ctx->wrong_path;

    top->tc.base_pc = 0;
    handle_commit_group_fetch(ctx, top, NULL);

    top->readycycle = cyc;
    top->numwaiting = 0;

    top->bmt.spillfill = BmtSF_None;

    top->icache_sim.service_level = 0;  // (SERVICED_NONE)
    top->icache_sim.was_merged = 0;
    top->dcache_sim.service_level = 0;  // (SERVICED_NONE)
    top->dcache_sim.was_merged = 0;

    return top;
}


static void
dump_inject_inst(const context * restrict ctx,
                 const activelist * restrict inst)
{
    int app_id = (ctx->as) ? ctx->as->app_id : -1;
    printf("T%ds%d/A%d:I:0x%s\t<%s>:\t[inject]\t", ctx->id, inst->id,
           app_id, fmt_x64(inst->pc), fmt_i64(cyc));
    int r_dest = inst->dest, r_src1 = inst->src1, r_src2 = inst->src2;
    char t_dest = 'r', t_src1 = 'r', t_src2 = 'r';
    if (IS_FP_REG(r_dest)) { r_dest = FP_UNREG(r_dest); t_dest = 'f'; }
    if (IS_FP_REG(r_src1)) { r_src1 = FP_UNREG(r_src1); t_src1 = 'f'; }
    if (IS_FP_REG(r_src2)) { r_src2 = FP_UNREG(r_src2); t_src2 = 'f'; }
    if (!IS_ZERO_REG(inst->dest))
        printf("%c%d", t_dest, r_dest);
    if (!IS_ZERO_REG(inst->dest) && (inst->mem_flags & SMF_Write))
        printf(",");
    if (inst->mem_flags & SMF_Write)
        printf("M%d[%s]", SMF_GetWidth(inst->mem_flags),
               fmt_x64(inst->destmem));
    printf(" <- op(%c%d,%c%d", t_src1, r_src1, t_src2, r_src2);
    if (inst->mem_flags & SMF_Read)
        printf(",M%d[%s]", SMF_GetWidth(inst->mem_flags),
               fmt_x64(inst->srcmem));
    printf(")\n");
}


static void
inject_inst_prep(context * restrict ctx, activelist * restrict inst)
{
    CoreResources * restrict core = ctx->core;
    if (debug) 
        dump_inject_inst(ctx, inst);

#ifdef PRIO_INST_COUNT
    core->sched.key[ctx->core_thread_id]++;
#else
#ifdef PRIO_BR_COUNT
    if (inst->br_flags) {
        core->sched.key[ctx->core_thread_id]++;
    }
#endif
#endif

    inst->fetchcycle = cyc;

    if (ctx->alisttop == inst->id) {
        if (!IS_ZERO_REG(inst->src1)) {
            inst->src1_waitingfor = handle_src_dep(ctx, inst, inst->src1);
        } else {
            inst->src1_waitingfor = NULL;
        }
        if (!IS_ZERO_REG(inst->src2) && (inst->src1 != inst->src2)) {
            inst->src2_waitingfor = handle_src_dep(ctx, inst, inst->src2);
        } else {
            inst->src2_waitingfor = NULL;
        }
        if (!IS_ZERO_REG(inst->dest))
            ctx->last_writer[inst->dest] = ctx->alisttop;
    } else {
        fprintf(stderr, "(%s:%d): uh-oh, we're injecting insts but others "
                "have entered the pipeline since allocation; dependences "
                "are hosed!  inject T%ds%d/A%d, cyc %s, alisttop %d\n",
                __FILE__, __LINE__, ctx->id, inst->id,
                (inst->as) ? inst->as->app_id : -1, fmt_i64(cyc),
                ctx->alisttop);
        sim_abort();
    }
}


void
inject_at_rename(struct context *ctx, activelist *inst)
{
    inject_inst_prep(ctx, inst);
    stageq_enqueue(ctx->core->stage.rename_inject, inst);
}


// nonzero: some instructions moved to rename1
void
service_rename_inject(CoreResources * restrict core)
{
    const int max_rename = core->params.fetch.total_limit;
    const int dst_st = core->stage.rename1;
    if (1 && (stageq_count(core->stage.rename_inject) > 0) &&
        (stageq_count(core->stage.s[dst_st]) > 0)) {
        // We've got instructions to inject, but the rename entry latch
        // is not clear
        DEBUGPRINTF("C%i: rename_inject backed up\n", core->core_id);
        return;
    } 
    //      const int contention = stageq_count(core->stage.s[dst_st - 1]) > 0;
    while ((stageq_count(core->stage.rename_inject) > 0) &&
           (stageq_count(core->stage.s[dst_st]) < max_rename)) {
        activelist * restrict inst =
            stageq_head(core->stage.rename_inject);
        stageq_dequeue(core->stage.rename_inject);
        stageq_enqueue(core->stage.s[dst_st], inst);
        DEBUGPRINTF("C%i: T%ds%d injected to rename1\n", core->core_id,
                    inst->thread, inst->id);
        if (inst->bmt.spillfill & BmtSF_Final) {
            context * restrict ctx = Contexts[inst->thread];
            if (inst->bmt.spillfill & BmtSF_Fill)
                appmgr_signal_finalfill(GlobalAppMgr, ctx, 0);
            if (inst->bmt.spillfill & BmtSF_Spill)
                appmgr_signal_finalspill(GlobalAppMgr, ctx, 0);
        }
    }
}



void
inject_set_bmtfill(struct activelist *inst, int dst_reg,
                   int is_final, int is_block_start)
{
    inst->dest = dst_reg;
    inst->fu = INTEGER;
    inst->delay = 1;
    inst->bmt.spillfill = BmtSF_Fill;
    if (is_final)
        inst->bmt.spillfill |= BmtSF_Final;
    if (is_block_start)
        inst->bmt.spillfill |= BmtSF_BlockMarker;
}


void
inject_set_bmtspill(struct activelist *inst, int src_reg,
                    int is_final, int is_block_end)
{
    inst->src1 = src_reg;
    inst->fu = INTEGER;
    inst->delay = 1;
    inst->bmt.spillfill = BmtSF_Spill;
    if (is_final) {
        sim_assert(is_block_end);
        inst->bmt.spillfill |= BmtSF_Final;
    }
    if (is_block_end)
        inst->bmt.spillfill |= BmtSF_BlockMarker;
}
