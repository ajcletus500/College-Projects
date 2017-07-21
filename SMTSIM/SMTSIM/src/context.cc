//
// Simulated hardware thread context utility routines
//
// Jeff Brown
// $Id: context.cc,v 1.1.2.28.2.5.2.14 2009/11/09 21:42:48 jbrown Exp $
//

const char RCSid_1107553048[] =
"$Id: context.cc,v 1.1.2.28.2.5.2.14 2009/11/09 21:42:48 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "context.h"
#include "app-state.h"
#include "dyn-inst.h"
#include "utils.h"
#include "main.h"
#include "core-resources.h"
#include "trace-fill-unit.h"
#include "sim-cfg.h"
#include "app-stats-log.h"
#include "callback-queue.h"
#include "app-mgr.h"            // for appmgr_signal_idlectx() callback


// This auto-grows as needed
#define INITIAL_WAITER_SIZE     32


const char *CtxHaltStyle_names[] = {
    "Full", "Fast", "AfterDrain", NULL
};

const char *CtxHalt_names[] = {
    "NoHalt", "FullSignaled", "FullFlushed", "FullDraining",
    "FastSignaled", "FastFlushed", NULL
};

const char *LongMem_names[] = {
    "None", "Detecting", "Ignored", "FlushedBlocked", "Completing", NULL
};

const char *MisPred_names[] = { 
    "None", "CorrectPath", "WrongPath", NULL
};


namespace {

class CtxSchedLog {
    FILE *file;
    i64 prev_log_cyc;
public:
    CtxSchedLog(const char *file_name) 
        : file(0), prev_log_cyc(0) {
        file = (FILE *) efopen(file_name, 1);
        fprintf(file,
                "# global context scheduling log\n"
                "# go: <delta_cyc> g <ctx> <app> <fetchcyc - logcyc>\n"
                "# halt: <delta_cyc> h <ctx> <num_inflight>\n");
    }
    ~CtxSchedLog() {
        if (file) fclose(file);
    }
    void log_go(i64 now_cyc, int ctx_id, int app_id, i64 fetch_cyc) {
        fprintf(file, "%s g %d %d %s\n", fmt_i64(now_cyc - prev_log_cyc),
                ctx_id, app_id, fmt_i64(fetch_cyc - now_cyc));
        prev_log_cyc = now_cyc;
    }
    void log_halt(i64 now_cyc, int ctx_id, int num_inflight) {
        fprintf(file, "%s h %d %d\n", fmt_i64(now_cyc - prev_log_cyc),
                ctx_id, num_inflight);
        prev_log_cyc = now_cyc;
    }
};


static struct {
    CtxSchedLog *logger;
    bool config_checked;
} GlobalSchedLog;


void
global_sched_log_setup_hook() {
    if (GlobalSchedLog.config_checked)
        return;
    GlobalSchedLog.config_checked = true;
    const char *config_key = "GlobalCtxSchedLog/name";
    if (simcfg_have_val(config_key)) {
        const char *filename = simcfg_get_str(config_key);
        GlobalSchedLog.logger = new CtxSchedLog(filename);
        printf("--Logging context scheduling events to \"%s\"\n", filename);
    }
}


// backward-compatibility halt-done callback: just call
// appmgr_signal_idlectx() and let it worry about what to do
class CtxHaltedAppMgrCB : public CBQ_Callback {
    AppMgr *amgr;
    context *ctx;
public:
    CtxHaltedAppMgrCB(AppMgr *amgr_, context *ctx_)
        : amgr(amgr_), ctx(ctx_) { }
    i64 invoke(CBQ_Args *args) {
        appmgr_signal_idlectx(amgr, ctx);
        // return value ignored by context_halt_cb()
        return -1;
    }
};


}       // Anonymous namespace close


static void
init_alist(activelist *dst, int waiter_size)
{
    sim_assert(waiter_size > 0);
    dst->waiter_size = waiter_size;
    dst->waiter = (activelist **) 
        emalloc_zero(waiter_size * sizeof(dst->waiter[0]));
}


static void
reset_alist(activelist *inst, int id)
{
    activelist temp = *inst;
    memset(inst, 0, sizeof(*inst));
    inst->waiter_size = temp.waiter_size;
    inst->waiter = temp.waiter;
    for (int i = 0; i < inst->waiter_size; i++)
        inst->waiter[i] = NULL;
    inst->id = id;
    inst->status = INVALID;
    inst->deps = 0;
}


context *
context_create(const ThreadParams *params, int ctx_id)
{
    context *n = NULL;
    int alist_size = params->active_list_size;

    // Minor cruft: check once to set up global schedule logging.  We check
    // here before any contexts have been created, since this must occur
    // before any contexts can be started or stopped.
    global_sched_log_setup_hook();

    sim_assert(alist_size > 0);
    sim_assert(params->retstack_entries > 0);

    // emalloc_zero: lazy
    n = (context *) emalloc_zero(sizeof(*n));
    n->params = *params;
    n->id = ctx_id;
    n->alist = (activelist *) emalloc_zero(alist_size * sizeof(n->alist[0]));

    for (int i = 0; i < alist_size; i++)
        init_alist(&n->alist[i], INITIAL_WAITER_SIZE);

    n->return_stack = (i64 *) emalloc_zero(params->retstack_entries *
                                           sizeof(n->return_stack[0]));

    context_reset(n);

    return n;
}


void
context_destroy(context *ctx)
{
    if (ctx) {
        for (int i = 0; i < ctx->params.active_list_size; i++)
            free(ctx->alist[i].waiter);
        free(ctx->alist);
        free(ctx->return_stack);
        free(ctx);
    }
}


static void
log_appstart_time(context *ctx, i64 start_cyc)
{
    AppStateExtras *ase = ctx->as->extra;
    sim_assert(ase->last_go_time == -1);
    ase->last_go_time = start_cyc;
    ase->total_go_count++;
    ase->commits_at_last_go = ase->total_commits;
    ase->last_go_ctx_id = ctx->id;
}


static void
log_appstop_time(context *ctx)
{
    sim_assert(ctx->as->extra->last_go_time != -1);
    if (cyc >= ctx->as->extra->last_go_time) {
        ctx->as->extra->sched_cyc_before_last +=
            cyc - ctx->as->extra->last_go_time;
    }
    ctx->as->extra->last_go_time = -1;
}


static void
context_reset_minstate(context *ctx)
{
    // Minimalist reset: clear up things just enough so that a future
    // application can successfully execute.
    ctx->running = 0;
    ctx->lock_box_entry.addr = 0;
    ctx->lock_box_entry.timestamp = MAX_CYC;
    ctx->lock_box_entry.valid = 0;
    ctx->nextsync = NONE;
    ctx->fetchcycle = MAX_CYC;
    ctx->stalled_for_prior_fetch = 0;
    ctx->last_fetch_begin = 0;
    ctx->commit_group.fetching_leader = -1;
    ctx->sync_lock_blocked = 0;
    ctx->follow_sync = 0;
    ctx->wrong_path = 0;
    ctx->misfetching = 0;
    ctx->draining = 0;
    ctx->halting = CtxHalt_NoHalt;
    sim_assert(!ctx->halt_done_cb);
    ctx->long_mem_stat = LongMem_None;
    ctx->tc.avail = 0;
    ctx->misfetch_discovered = NULL;
    ctx->lock_failed = NULL;
    ctx->mispredict_discovered = NULL;
    ctx->noop_discard_run_len = 0;
    ctx->commit_taken_br.br_flags = 0;
    sim_assert(!ctx->imiss_cache_entry);
    sim_assert(!ctx->mergethread);

    ctx->icache_sim.service_level = 0;          // (SERVICED_NONE)
    ctx->icache_sim.was_merged = 0;

    // When first created, contexts are "reset" while ctx->core is still NULL
    if (ctx->core) {
        if (ctx->core->tfill)
            tfu_context_threadswap(ctx->core->tfill, ctx);
    }
    if (ctx->as) {
        log_appstop_time(ctx);
        ctx->as = NULL;
    }
}


void
context_reset(context *ctx)
{
    context temp = *ctx;

    memset(ctx, 0, sizeof(*ctx));

    // Copy back the things we don't want zeroed out: static properties of
    // this context, storage allocated for it, statistic counters, etc.
    ctx->params = temp.params;
    ctx->core = temp.core;
    ctx->id = temp.id;
    ctx->core_thread_id = temp.core_thread_id;
    ctx->alist = temp.alist;
    ctx->return_stack = temp.return_stack;
    ctx->tc.block = temp.tc.block;
    ctx->stats = temp.stats;

    // Copy any AppState pointer, so reset_minstate() can do stats on it before
    // clearing it.
    ctx->as = temp.as;

    for (int i = 0; i < MAXREG; i++)
        ctx->last_writer[i] = NONE;

    for (int i = 0; i < ctx->params.active_list_size; i++)
        reset_alist(&ctx->alist[i], i);

    for (int i = 0; i < ctx->params.retstack_entries; i++)
        ctx->return_stack[i] = 0;

    ctx->alisttop = 0;
    ctx->next_to_commit = 1;
    context_reset_minstate(ctx);
}


void
context_reset_resteer(context *ctx)
{
    // This is tricky: it's a "light" version of context_reset(), which 
    // cleans up enough state that a new application can use the context
    // right away, but leaves most of the state alone so that existing
    // in-flight instructions can proceed through.
    //
    // This requires that all
    // existing instructions in the context be either squashed or invalid,
    // to prevent any cases where a re-steer occurs, but then an existing 
    // instruction does something which monkeys with the application state
    // of the just-evicted application, or with the simulation state of the
    // incoming application.

    // This might be a bad idea.  Does just flushing and resetting really take
    // that long?

#ifdef DEBUG
    {
        int idx = ctx->next_to_commit;
        for (;;) {
            const activelist * restrict inst = &ctx->alist[idx];
            if ((inst->status != SQUASHED) && (inst->status != INVALID)) {
                fflush(0);
                fprintf(stderr, "context_resteer: T%d has non-squashed, "
                        "non-invalid insts!  Re-steer speculation isn't "
                        "supported.\n", ctx->id);
                sim_abort();
            }
            if (idx == ctx->alisttop)
                break;
            idx = alist_add(ctx, idx, 1);
        }
    }
#endif

    context_reset_minstate(ctx);
    // Invalidate the ret stack; it's almost certainly wrong for the next app.
    ctx->rs_size = ctx->rs_start = 0;
    // Nuke the GHR, for the same reason
    ctx->ghr = 0;
}


int
context_okay_to_halt(const context * restrict ctx)
{
    int result = (ctx->as != NULL) &&
        ctx->running &&
        (ctx->halting == CtxHalt_NoHalt);
    return result;    
}


void
context_halt_cb(context *ctx, CtxHaltStyle halt_style,
                struct CBQ_Callback *halt_done_cb)
{
    sim_assert(context_okay_to_halt(ctx));

    ctx->running = 0;   // This prevents future fetching

    switch (halt_style) {
    case CtxHaltStyle_Full:
        ctx->halting = CtxHalt_FullSignaled; break;
    case CtxHaltStyle_Fast:
        ctx->halting = CtxHalt_FastSignaled; break;
    case CtxHaltStyle_AfterDrain:
        ctx->halting = CtxHalt_AfterSignaled; break;
    default:
        ENUM_ABORT(CtxHaltStyle, halt_style);
    }
    sim_assert(halt_done_cb != NULL);
    ctx->halt_done_cb = halt_done_cb;

    DEBUGPRINTF("T%d/A%d context_halt (%s) signaled\n", ctx->id,
                ctx->as->app_id, CtxHaltStyle_names[halt_style]);
    if (GlobalSchedLog.logger)
        GlobalSchedLog.logger->
            log_halt(cyc, ctx->id, 
                     context_alist_inflight(ctx, ctx->as->app_id));
}


void 
context_halt_signal(context *ctx, CtxHaltStyle halt_style)
{
    CtxHaltedAppMgrCB *halt_done_cb = new CtxHaltedAppMgrCB(GlobalAppMgr, ctx);
    context_halt_cb(ctx, halt_style, halt_done_cb);
}


int
context_ready_to_go(const context * restrict ctx)
{
    int result = (ctx->as == NULL) &&
        !ctx->running &&
        (ctx->halting == CtxHalt_NoHalt);
    return result;    
}


static void
consider_late_longmem_flush(CoreResources * restrict core)
{
    static int consider_late_flush = -1;
    if (consider_late_flush < 0) {
        consider_late_flush = simcfg_get_bool("Hacking/consider_late_flush");
    }
    if (!consider_late_flush) 
        return;
    for (int i = 0; i < core->n_contexts; i++) {
        context * restrict ctx = core->contexts[i];
        if (ctx->long_mem_stat == LongMem_Ignored) {
            DEBUGPRINTF("T%d: blocked on ignored long_mem_op, re-considering "
                        "flush\n", ctx->id);
            ctx->long_mem_stat = LongMem_Detecting;
        }
    }
}


void
context_go(context *ctx, struct AppState *app, i64 fetch_cyc)
{
    sim_assert(context_ready_to_go(ctx));
    ctx->as = app;
    ctx->running = 1;
    ctx->fetchcycle = MAX_SCALAR(fetch_cyc, cyc);
    DEBUGPRINTF("T%d context_go on A%d, next PC %s, fetch cycle %s\n",
                ctx->id, ctx->as->app_id, fmt_x64(ctx->as->npc),
                fmt_i64(ctx->fetchcycle));

    for (int i = 0; i < MAXREG; i++)
        ctx->bmt_regdirty[i] = 0;

    log_appstart_time(ctx, fetch_cyc);
    if (GlobalSchedLog.logger)
        GlobalSchedLog.logger->log_go(cyc, ctx->id, ctx->as->app_id,
                                      ctx->fetchcycle);
    consider_late_longmem_flush(ctx->core);
}


int
context_alist_used(const struct context * restrict ctx)
{
    int result = alist_count(ctx, ctx->next_to_commit, ctx->alisttop);

    // Corner case: when alisttop + 1 == next_to_commit (mod alist size),
    // alist[] is either completely full or completely empty.  In such a case,
    // alist_count() returns the alist size, so we'll test whether the oldest
    // instruction entry is valid to decide.
    if ((result == ctx->params.active_list_size) && 
        (ctx->alist[ctx->next_to_commit].status & INVALID)) {
        result = 0;
    }
    return result;
}


int 
context_alist_inflight(const struct context * restrict ctx, int app_id)
{
    const int alist_used = context_alist_used(ctx);
    int inst_id = ctx->next_to_commit;
    int result = 0;
    
    for (int i = 0; i < alist_used; i++) {
        const activelist * restrict inst = &ctx->alist[inst_id];
        if (!(inst->status & (SQUASHED | INVALID)) &&
            ((inst->as && (inst->as->app_id == app_id)) || (app_id == -1)))
            result++;
        inst_id = alist_add(ctx, inst_id, 1);
    }

    return result;
}


AppStateExtras * 
appextra_create(void)
{
    AppStateExtras *n;
    if (!(n = (AppStateExtras *) malloc(sizeof(*n)))) {
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->job_id = -1;
    n->create_time = cyc;
    n->vacate_time = -1;
    n->last_go_time = -1;
    n->last_go_ctx_id = -1;

    n->bmt.spill_retstack.size = NELEM(n->bmt.spill_retstack.ents);
    n->bmt.spill_dtlb.size = NELEM(n->bmt.spill_dtlb.ents);

    n->watch.commit_count = callbackq_create();
    n->watch.app_inst_commit = callbackq_create();
    return n;
}


void 
appextra_destroy(AppStateExtras *extra)
{
    if (extra) {
        appstatslog_destroy(extra->stats_log);
        if (extra->stats_log_cb)
            callbackq_cancel(GlobalEventQueue, extra->stats_log_cb);
        callbackq_destroy(extra->watch.commit_count);
        callbackq_destroy(extra->watch.app_inst_commit);
        free(extra);
    }
}


void
appextra_assign_stats(AppStateExtras *out, 
                      const AppStateExtras *in)
{
    // don't forget to sync appextra_subtract_stats()
    out->total_commits = in->total_commits;
    out->cp_insts_discarded = in->cp_insts_discarded;
    out->mem_commits = in->mem_commits;
    out->app_inst_last_commit = in->app_inst_last_commit;
    out->hitrate.dcache = in->hitrate.dcache;
    out->hitrate.icache = in->hitrate.icache;
    out->hitrate.dtlb = in->hitrate.dtlb;
    out->hitrate.itlb = in->hitrate.itlb;
    out->hitrate.l2cache = in->hitrate.l2cache;
    out->hitrate.l3cache = in->hitrate.l3cache;
    out->hitrate.bpred = in->hitrate.bpred;
    out->hitrate.retpred = in->hitrate.retpred;
    out->mem_accesses = in->mem_accesses;
    out->mem_delay.delay_sum = in->mem_delay.delay_sum;
    out->mem_delay.sample_count = in->mem_delay.sample_count;
    out->instq_conf_cyc = in->instq_conf_cyc;
    out->total_go_count = in->total_go_count;
    out->long_mem_detected = in->long_mem_detected;
    out->long_mem_flushed = in->long_mem_flushed;
    out->itlb_acc = in->itlb_acc;
    out->dtlb_acc = in->dtlb_acc;
    out->icache_acc = in->icache_acc;
    out->dcache_acc = in->dcache_acc;
    out->l2cache_acc = in->l2cache_acc;
    out->l3cache_acc = in->l3cache_acc;
    out->bpred_acc = in->bpred_acc;
    out->intalu_acc = in->intalu_acc;
    out->fpalu_acc = in->fpalu_acc;
    out->ldst_acc = in->ldst_acc;
    out->iq_acc = in->iq_acc;
    out->fq_acc = in->fq_acc;
    out->ireg_acc = in->ireg_acc;
    out->freg_acc = in->freg_acc;
    out->iren_acc = in->iren_acc;
    out->fren_acc = in->fren_acc;
    out->lsq_acc = in->lsq_acc;
    out->rob_acc = in->rob_acc;
    out->iq_occ = in->iq_occ;
    out->fq_occ = in->fq_occ;
    out->ireg_occ = in->ireg_occ;
    out->freg_occ = in->freg_occ;
    out->iren_occ = in->iren_occ;
    out->fren_occ = in->fren_occ;
    out->lsq_occ = in->lsq_occ;
    out->rob_occ = in->rob_occ;
}


static void
appextra_sub_hitrate(ASE_HitRate *out,
                     const ASE_HitRate *l,
                     const ASE_HitRate *r)
{
    out->acc = l->acc - r->acc;
    out->hits = l->hits - r->hits;
}


void
appextra_subtract_stats(AppStateExtras *out, 
                        const AppStateExtras *l,
                        const AppStateExtras *r)
{
    // don't forget to sync appextra_assign_stats()
    memset(out, 0, sizeof(*out));
    out->total_commits = l->total_commits - r->total_commits;
    out->cp_insts_discarded = l->cp_insts_discarded - r->cp_insts_discarded;
    out->mem_commits = l->mem_commits - r->mem_commits;
    out->app_inst_last_commit = l->app_inst_last_commit -
        r->app_inst_last_commit;
    appextra_sub_hitrate(&out->hitrate.dcache, &l->hitrate.dcache,
                         &r->hitrate.dcache);
    appextra_sub_hitrate(&out->hitrate.icache, &l->hitrate.icache, 
                         &r->hitrate.icache);
    appextra_sub_hitrate(&out->hitrate.dtlb, &l->hitrate.dtlb, 
                         &r->hitrate.dtlb);
    appextra_sub_hitrate(&out->hitrate.itlb, &l->hitrate.itlb, 
                         &r->hitrate.itlb);
    appextra_sub_hitrate(&out->hitrate.l2cache, &l->hitrate.l2cache, 
                         &r->hitrate.l2cache);
    appextra_sub_hitrate(&out->hitrate.l3cache, &l->hitrate.l3cache,
                         &r->hitrate.l3cache);
    appextra_sub_hitrate(&out->hitrate.bpred, &l->hitrate.bpred,
                         &r->hitrate.bpred);
    appextra_sub_hitrate(&out->hitrate.retpred, &l->hitrate.retpred,
                         &r->hitrate.retpred);
    out->mem_accesses = l->mem_accesses - r->mem_accesses;
    out->mem_delay.delay_sum = l->mem_delay.delay_sum - r->mem_delay.delay_sum;
    out->mem_delay.sample_count = l->mem_delay.sample_count -
        r->mem_delay.sample_count;
    out->instq_conf_cyc = l->instq_conf_cyc - r->instq_conf_cyc;
    out->total_go_count = l->total_go_count - r->total_go_count;
    out->long_mem_detected = l->long_mem_detected - r->long_mem_detected;
    out->long_mem_flushed = l->long_mem_flushed - r->long_mem_flushed;
    out->itlb_acc = l->itlb_acc - r->itlb_acc;
    out->dtlb_acc = l->dtlb_acc - r->dtlb_acc;
    out->icache_acc = l->icache_acc - r->icache_acc;
    out->dcache_acc = l->dcache_acc - r->dcache_acc;
    out->l2cache_acc = l->l2cache_acc - r->l2cache_acc;
    out->l3cache_acc = l->l3cache_acc - r->l3cache_acc;
    out->bpred_acc = l->bpred_acc - r->bpred_acc;
    out->intalu_acc = l->intalu_acc - r->intalu_acc;
    out->fpalu_acc = l->fpalu_acc - r->fpalu_acc;
    out->ldst_acc = l->ldst_acc - r->ldst_acc;
    out->iq_acc = l->iq_acc - r->iq_acc;
    out->fq_acc = l->fq_acc - r->fq_acc;
    out->ireg_acc = l->ireg_acc - r->ireg_acc;
    out->freg_acc = l->freg_acc - r->freg_acc;
    out->iren_acc = l->iren_acc - r->iren_acc;
    out->fren_acc = l->fren_acc - r->fren_acc;
    out->lsq_acc = l->lsq_acc - r->lsq_acc;
    out->rob_acc = l->rob_acc - r->rob_acc;
    out->iq_occ = l->iq_occ - r->iq_occ;
    out->fq_occ = l->fq_occ - r->fq_occ;
    out->ireg_occ = l->ireg_occ - r->ireg_occ;
    out->freg_occ = l->freg_occ - r->freg_occ;
    out->iren_occ = l->iren_occ - r->iren_occ;
    out->fren_occ = l->fren_occ - r->fren_occ;
    out->lsq_occ = l->lsq_occ - r->lsq_occ;
    out->rob_occ = l->rob_occ - r->rob_occ;
}

/*
 * add_or_remove = 0: add resources
 * add_or_remove = 1: remove resources (iregs,lsq)
 * add_or_remove = 2: remove resources (iq,fq)
 * fp_or_int = 2: Don't care (add_or_remove is 1) 
 * fp_or_int = 1: fp queue
 * fp_or_int = 0: int queue
 * 
 */
void update_acc_occ_per_inst(context * restrict ctx, activelist * restrict inst, int add_or_remove, int fp_or_int)
{
    if (ctx->as != NULL){ // Not an injected inst
        if (add_or_remove == 0){// Add is called from regrename to add resources
            ctx->as->extra->rob_acc++;
            ctx->as->extra->iregs_this_cyc += inst->iregs_used;
            ctx->as->extra->fregs_this_cyc += inst->fregs_used;
            if (inst->mem_flags) {
                ctx->as->extra->lsq_acc++;
                ctx->as->extra->lsqsize_this_cyc++;
            }
            if (fp_or_int == 1){
                ctx->as->extra->fqsize_this_cyc += 1;
                ctx->as->extra->fren_acc += inst->regaccs;
            }
            else if (fp_or_int == 0){
                ctx->as->extra->iqsize_this_cyc += 1;
                ctx->as->extra->iren_acc += inst->regaccs;
            }
        }
        else if (add_or_remove == 1){ // Remove is called from commit and undo to remove resources
            ctx->as->extra->iregs_this_cyc -= inst->iregs_used;
            ctx->as->extra->fregs_this_cyc -= inst->fregs_used;
            ctx->as->extra->lsqsize_this_cyc -= inst->lsqentry;
        }
        else if (add_or_remove == 2){
            if (fp_or_int == 1)
                ctx->as->extra->fqsize_this_cyc -= 1;
            else if (fp_or_int == 0)
                ctx->as->extra->iqsize_this_cyc -= 1;
        }
    }
}

i64
app_sched_cyc(const struct AppState *as)
{
    i64 result = as->extra->sched_cyc_before_last;
    if (as->extra->last_go_time >= 0)
        result += cyc - as->extra->last_go_time;
    return result;
}


i64
app_alive_cyc(const struct AppState *as)
{
    i64 end_time;
    if (appstate_is_alive(as)) {
        sim_assert(as->extra->vacate_time < 0);
        end_time = cyc;
    } else {
        sim_assert(as->extra->vacate_time >= 0);
        end_time = as->extra->vacate_time;
    }
    return end_time - as->extra->create_time;
}


struct CBQ_Args *
commit_watchpoint_args(context *ctx, int commits_this_cyc)
{
    return new CommitWatchCBArgs(ctx, commits_this_cyc);
}
