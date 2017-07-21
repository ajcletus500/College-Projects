//
// Application manager: schedule application execution/migration, including
// access to idle thread state storage
//
// Jeff Brown
// $Id: app-mgr.cc,v 1.1.2.88.2.4.2.19.6.1 2009/12/25 06:31:46 jbrown Exp $
//

const char RCSid_1108950676[] =
"$Id: app-mgr.cc,v 1.1.2.88.2.4.2.19.6.1 2009/12/25 06:31:46 jbrown Exp $";

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <set>
#include <map>
#include <vector>
#include <deque>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "sim-assert.h"
#include "sys-types.h"
#include "app-mgr.h"
#include "utils.h"
#include "utils-cc.h"
#include "app-state.h"
#include "context.h"
#include "dyn-inst.h"
#include "core-resources.h"
#include "sim-cfg.h"
#include "callback-queue.h"
#include "main.h"
#include "cache.h"
#include "online-stats.h"
#include "inject-inst.h"
#include "sim-params.h"
#include "prng.h"
#include "tlb-array.h"
#include "cache-array.h"
#include "cache.h"
#include "simple-pre.h"

using std::deque;
using std::make_pair;
using std::map;
using std::set;
using std::size_t;
using std::string;
using std::swap;
using std::vector;


namespace {

typedef std::set<int> IdSet;
typedef std::vector<int> IdVec;

// option: don't double-charge bus for core->core moves; make the fill free
const bool kMigrateFillsAreFree = true;
// spam swap/first-commit latencies, even outside debug mode
const bool kVerboseMigrateStats = true;


enum AppInfoState {
    AI_Running,                 // Running on a context
    AI_Running_LongMiss,        // Running on a context, blocked for miss
    AI_Ready,                   // Ready to swap in, not on a context

    AI_SwapIn,                  // Swapping in
    AI_SwapOut_LongMiss,        // Swapping out due to a long miss
    AI_SwapOut_LongMiss_Cancel, // Above, but miss finished before swap
    AI_SwapOut_Migrate,         // Swapping out to (explicitly) migrate
    AI_SwapOut_Sched,           // Swapping out for scheduling reasons

    AI_Wait_LongMiss            // Waiting for a long miss
    // (make sure to update app_state_name() with any changes)
};


void shuffle_idvec(PRNGState *prng, IdVec& vec);
void shuffle_idvec(IdVec& vec);
void idvec_from_idset(IdVec& dst, const IdSet& src);
const char *app_state_name(AppInfoState astate);
bool random_bool(PRNGState *prng, double prob);
void deque_erase_int(deque<int>& container, int value);
void dump_idvec(FILE *out, const IdVec& container);
void dump_iddeque(FILE *out, const deque<int>& container);
string fmt_bstat_i64(const BasicStat_I64& bstat);


// A sequence of callbacks.
//
// PerAppInfo::bundle_posthalt_callbacks() generates
// these so that callback invocation doesn't rely on the lifetime of the
// PerAppInfo.  (This is still sketchy, if multiple post-halt callbacks are
// registered, there's a good chance that an earlier one will do something
// that a later one won't like.  How to best handle this isn't clear yet, as
// this situation hasn't arisen yet.)
class CallbackSeq : public CBQ_Callback {
    vector<CBQ_Callback *> callbacks;
public:
    // Takes ownership of all callbacks in the vector
    CallbackSeq(const vector<CBQ_Callback *>& callbacks_)
        : callbacks(callbacks_) { }
    ~CallbackSeq() {
        for (int i = 0; i < (int) callbacks.size(); i++)
            delete callbacks[i];
    }
    i64 invoke(CBQ_Args *args) {
        for (int i = 0; i < (int) callbacks.size(); i++) {
            CBQ_Callback *cb = callbacks[i];
            DEBUGPRINTF("CallbackSeq %p invoking callback %p\n",
                        static_cast<void *>(this), static_cast<void *>(cb));
            callbacks[i] = NULL;        // vague attempt at safety
            callback_invoke(cb, NULL);
            delete cb;
        }
        callbacks.clear();
        DEBUGPRINTF("CallbackSeq %p complete.\n", static_cast<void *>(this));
        return -1;
    }
};

typedef std::set<CBQ_Callback *> CallbackSet;


// hack to get around declaration ordering constraint
const class MgrSchedInfo *
appmgr_get_schedinfo(const AppMgr *app_mgr);


class PerAppInfo {
    AppState *as;
    int id;
    AppInfoState state;
    int curr_ctx_id;            // -1 iff not on a context
    int prev_ctx_id;            // -1 if none
    int migrate_target_ctx;     // Only valid if state == AI_SwapOut_Migrate
    struct {
        i64 last_swapin_cyc;    // Time at last swap in
        i64 last_swapin_commits;
        i64 last_swapout_cyc;   // Time at last swap out completion
        i64 last_swapout_commits;
        i64 cyc_before_swapin;  // Total resident cyc, excluding current run
        i64 long_misses;        // Number of misses signalled
        i64 swap_outs;          // Number of completed swap-outs
        i64 swapin_repeats;     // # times swapped back in to same core
        i64 last_state_change;
        i64 migrates;           // Explicit migrates
    } st;                       // Stats struct (for lazy init code)
    HistCount_Int host_ctx_count;
    HistCount_Int host_ctx_cyc; // excludes current context
    BasicStat_I64 run_before_swap;
    HistCount_Int state_cyc;

    struct {
        i64 last_halt_start;      // last time context_halt called, or -1
        i64 last_migrate_start;   // (if == last_halt_start, halt was migrate)
        i64 last_halt_done;       // last time signal_idlectx called, or -1
        // see: st.last_swapout_cyc
        // see: st.last_swapin_cyc
        i64 last_swapin_done;   // last swapin-done CB -> swap_in_done(), or -1
        i64 last_finalfill_commit; // (or -1)
        // Various (independent) measurements of components of
        // thread migration-related latencies
        BasicStat_I64 deact_halt;       // halt_signal -> signal_idlectx
        BasicStat_I64 deact_swapout;    // signal_idlectx -> final_spill_commit
        BasicStat_I64 deact_sum;        // deact_halt + deact_swapout
        // activ_fetch: activate lat., stop when target fetchable (overlaps)
        // activ_commit: activate lat., stop when final fill commits
        BasicStat_I64 activ_fetch;      // start_app -> swapin_done_callback
        BasicStat_I64 activ_commit;     // start_app -> finalfill(commit=t)
        BasicStat_I64 migrate_fetch;    // deact_sum+activ_fetch, for migrate
        BasicStat_I64 migrate_commit;   // deact_sum+activ_commit, for migrate
    } migrate_timing;

    struct {                    // post-halt callbacks
        vector<CBQ_Callback *> ord;             // (in registration order)
        set<CBQ_Callback *> uniq;               // (to ensure uniqueness)
    } posthalt_cb;

public:    
    PerAppInfo(AppState *as_) 
        : as(as_), id(as->app_id), state(AI_Ready), curr_ctx_id(-1),
          prev_ctx_id(-1) {
        memset(&st, 0, sizeof(st));     // Zero stats
        sim_assert(id >= 0);
        migrate_timing.last_halt_start = -1;
        migrate_timing.last_migrate_start = -1;
        migrate_timing.last_halt_done = -1;
        migrate_timing.last_swapin_done = -1;
        migrate_timing.last_finalfill_commit = -1;
    }
    ~PerAppInfo() {
        for (int i = 0; i < (int) posthalt_cb.ord.size(); i++)
            delete posthalt_cb.ord[i];
    }

    AppState *g_as() { return as; }
    const AppState *g_as() const { return as; }
    int g_id() const { return id; }
    bool is_sched() const { return curr_ctx_id >= 0; }
    int g_ctx_id() const { sim_assert(is_sched()); return curr_ctx_id; }
    int g_prev_ctx() const { return prev_ctx_id; }
    AppInfoState g_state() const { return state; }
    bool is_resident_stalled() const {
        return state == AI_Running_LongMiss;
    }
        
    void set_state(AppInfoState state_) {
        state_cyc.add_count((int) state, cyc - st.last_state_change);
        if (0) {
            DEBUGPRINTF("appmgr set_state: cyc %s A%d %d -> %d\n", 
                        fmt_i64(cyc), id, state, state_);
        }
        state = state_;
        st.last_state_change = cyc;
    }

    i64 app_commits() const {
        return as->extra->total_commits;
    }
    i64 app_mem_commits() const {
        return as->extra->mem_commits;
    }
    void ignore_longmiss() {
        if (state == AI_Running) {
            set_state(AI_Running_LongMiss);
        }
        st.long_misses++;
    }
    void swapping_in(int new_ctx, bool is_core_repeat) {
        sim_assert(curr_ctx_id < 0);
        sim_assert(new_ctx >= 0);
        sim_assert(state == AI_Ready);
        set_state(AI_SwapIn);
        curr_ctx_id = new_ctx;
        host_ctx_count.add_count(curr_ctx_id);
        if (is_core_repeat)
            st.swapin_repeats++;
        st.last_swapin_cyc = cyc;
        st.last_swapin_commits = app_commits();
    }
    void swap_in_done(AppMgr& parent_app_mgr) {
        // be careful using up parent_app_mgr, since it's likely
        // already "open" on the stack, calling this function
        sim_assert(state == AI_SwapIn);
        set_state(AI_Running);
        migrate_timing.last_swapin_done = cyc;
    }
    void swap_in_finalfill_commit() {
        // note: still called when spill/fill instructions are not in use,
        // from swapin_done_callback() (for timing stats)
        migrate_timing.last_finalfill_commit = cyc;
        // thread "activation" time measurement
        i64 activate_fetchable_cyc = migrate_timing.last_swapin_done -
            st.last_swapin_cyc;
        i64 activate_fillcommit_cyc = migrate_timing.last_finalfill_commit -
            st.last_swapin_cyc;
        i64 migrate_fetchable_cyc = -1, migrate_fillcommit_cyc = 1;
        if (last_halt_was_for_migrate()) {
             migrate_fetchable_cyc = migrate_timing.last_swapin_done
                 - migrate_timing.last_migrate_start;
             migrate_fillcommit_cyc = migrate_timing.last_finalfill_commit
                 - migrate_timing.last_migrate_start;
        }
        sim_assert(activate_fetchable_cyc >= 0);
        sim_assert(activate_fillcommit_cyc >= 0);
        if (debug || kVerboseMigrateStats) {
            printf("appmgr: A%d swap-in C%d T%d final_fill_commit at %s; "
                   "swapin delay %s (targfetch) %s (fillcommit),"
                   " migrate delay %s (tf) %s (fc)\n", id,
                   Contexts[curr_ctx_id]->core->core_id,
                   curr_ctx_id, fmt_now(),
                   fmt_i64(activate_fetchable_cyc),
                   fmt_i64(activate_fillcommit_cyc),
                   fmt_i64(migrate_fetchable_cyc),
                   fmt_i64(migrate_fillcommit_cyc));
        }
        migrate_timing.activ_fetch.add_sample(activate_fetchable_cyc);
        migrate_timing.activ_commit.add_sample(activate_fillcommit_cyc);
        if (migrate_fetchable_cyc >= 0) {
            migrate_timing.migrate_fetch.add_sample(migrate_fetchable_cyc);
            migrate_timing.migrate_commit.add_sample(migrate_fillcommit_cyc);
        }
    }
    void swapping_out(AppInfoState next_state) {
        // decided to swap out, about to context_halt_signal()
        sim_assert(curr_ctx_id >= 0);
        sim_assert(state == AI_Running);
        sim_assert((next_state == AI_SwapOut_LongMiss) ||
                   (next_state == AI_SwapOut_Sched));
        if (next_state == AI_SwapOut_LongMiss)
            st.long_misses++;
        set_state(next_state);
        migrate_timing.last_halt_start = cyc;
    }
    void swap_out_ctx_halted() {
        sim_assert((state == AI_SwapOut_LongMiss) ||
                   (state == AI_SwapOut_Sched) ||
                   (state == AI_SwapOut_Migrate));
        // swapping out, context_halt done, ready to spill/fill or whatever
        migrate_timing.last_halt_done = cyc;
    }
    void swap_out_done(AppInfoState state_) {
        i64 cyc_this_time = cyc - st.last_swapin_cyc;
        sim_assert(curr_ctx_id >= 0);
        host_ctx_cyc.add_count(curr_ctx_id, cyc_this_time);
        set_state(state_);
        prev_ctx_id = curr_ctx_id;
        curr_ctx_id = -1;
        st.cyc_before_swapin += cyc_this_time;
        st.last_swapout_cyc = cyc;
        st.last_swapout_commits = app_commits();
        st.swap_outs++;
        run_before_swap.add_sample(cyc_this_time);
        // thread "deactivation" time measurement
        i64 deact_halt_cyc = migrate_timing.last_halt_done -
            migrate_timing.last_halt_start;
        i64 deact_swapout_cyc = st.last_swapout_cyc -
            migrate_timing.last_halt_done;
        sim_assert(migrate_timing.last_halt_start >= 0);
        sim_assert(migrate_timing.last_halt_done >= 0);
        sim_assert(deact_halt_cyc >= 0);
        sim_assert(deact_swapout_cyc >= 0);
        if (debug || kVerboseMigrateStats) {
            printf("appmgr: A%d swap_out_done from C%d T%d at %s; "
                   "halt delay %s, swapout delay %s\n", id,
                   Contexts[prev_ctx_id]->core->core_id, prev_ctx_id,
                   fmt_now(),
                   fmt_i64(deact_halt_cyc), fmt_i64(deact_swapout_cyc));
        }
        migrate_timing.deact_halt.add_sample(deact_halt_cyc);
        migrate_timing.deact_swapout.add_sample(deact_swapout_cyc);
        migrate_timing.deact_sum.add_sample(deact_halt_cyc +
                                            deact_swapout_cyc);
    }
    void migrating(int target_ctx_id) {
        sim_assert(curr_ctx_id >= 0);
        sim_assert(state == AI_Running);
        sim_assert(target_ctx_id >= 0);
        st.migrates++;
        set_state(AI_SwapOut_Migrate);
        migrate_target_ctx = target_ctx_id;
        migrate_timing.last_halt_start = cyc;
        migrate_timing.last_migrate_start = cyc;
    }
    bool last_halt_was_for_migrate() const {
        // test: have we ever migrated, and was the last halt for a migrate?
        return (migrate_timing.last_migrate_start >= 0) &&
            (migrate_timing.last_migrate_start ==
             migrate_timing.last_halt_start);
    }
    int get_migrate_target() const {
        sim_assert(state == AI_SwapOut_Migrate);
        sim_assert(migrate_target_ctx >= 0);
        return migrate_target_ctx;
    }
    void get_state_hist(HistCount_Int& dest) const {
        dest = state_cyc;
        dest.add_count(state, cyc - st.last_state_change);
    }
    
    i64 curr_run_cyc() const {
        return (is_sched()) ? (cyc - st.last_swapin_cyc) : 0;
    }
    i64 resident_cyc() const {
        return st.cyc_before_swapin + curr_run_cyc();
    }
    double mean_swappedin_cyc() const {
        BasicStat_I64 swap_stat = run_before_swap;
        if (is_sched())
            swap_stat.add_sample(cyc - st.last_swapin_cyc);
        return (swap_stat.g_count()) ? swap_stat.g_mean() : 0;
    };
    i64 ctx_resident_cyc(int ctx_id) const {
        i64 result = host_ctx_cyc.get_count(ctx_id);
        if (curr_ctx_id == ctx_id)
            result += curr_run_cyc();
        return result;
    }
    i64 ctx_swapin_count(int ctx_id) const {
        i64 result = host_ctx_count.get_count(ctx_id);
        return result;
    }
    double resident_ipc_commit() const {
        // IPC for all resident time
        return (double) app_commits() / resident_cyc();
    }
    double recent_ipc_commit() const {
        // IPC since last swap-in
        i64 commits, t;
        if (is_sched()) {
            commits = app_commits() - st.last_swapin_commits;
            t = cyc - st.last_swapin_cyc;
        } else {
            commits = st.last_swapout_commits - st.last_swapin_commits;
            t = st.last_swapout_cyc - st.last_swapin_cyc;
        }
        sim_assert(commits >= 0);
        sim_assert(t >= 0);
        return (double) commits / t;
    }
    double swapin_repeat_frac() const {
        return (double) st.swapin_repeats / 
            (st.swap_outs - ((is_sched()) ? 0 : 1));
    }
    i64 commits_since_swapin() const {
        sim_assert(is_sched());
        return app_commits() - st.last_swapin_commits;
    }
    i64 cyc_since_swapin() const {
        sim_assert(is_sched());
        return cyc - st.last_swapin_cyc;
    }
    i64 cyc_since_swapout() const {
        sim_assert(!is_sched());
        return cyc - st.last_swapout_cyc;
    }

    void register_posthalt_callback(CBQ_Callback *cb) {
        sim_assert(posthalt_cb.uniq.size() == posthalt_cb.ord.size());
        sim_assert(!posthalt_cb.uniq.count(cb));
        posthalt_cb.uniq.insert(cb);
        posthalt_cb.ord.push_back(cb);
        sim_assert(posthalt_cb.uniq.size() == posthalt_cb.ord.size());
    }

    // Bundles all registered post-halt callbacks into one "meta-callback",
    // then removes them from this AppInfo.  (Returns NULL instead of an empty
    // CallbackSeq.
    CallbackSeq *bundle_posthalt_callbacks() {
        sim_assert(posthalt_cb.uniq.size() == posthalt_cb.ord.size());
        if (posthalt_cb.ord.empty())
            return NULL;
        CallbackSeq *cb_seq = new CallbackSeq(posthalt_cb.ord);
        DEBUGPRINTF("bundle_posthalt_callbacks: CallbackSeq %p created "
                    "for A%d, containing %d callback(s).\n",
                    (void *) cb_seq, id, (int) posthalt_cb.ord.size());
        posthalt_cb.ord.clear();
        posthalt_cb.uniq.clear();
        return cb_seq;
    }
    void report_migrate_timing(FILE *out, const char *pf) const {
        fprintf(out, "%sdeact_halt: %s\n", pf,
                fmt_bstat_i64(migrate_timing.deact_halt).c_str());
        fprintf(out, "%sdeact_swapout: %s\n", pf,
                fmt_bstat_i64(migrate_timing.deact_swapout).c_str());
        fprintf(out, "%sdeact_sum: %s\n", pf,
                fmt_bstat_i64(migrate_timing.deact_sum).c_str());
        fprintf(out, "%sactiv_fetch: %s\n", pf,
                fmt_bstat_i64(migrate_timing.activ_fetch).c_str());
        fprintf(out, "%sactiv_commit: %s\n", pf,
                fmt_bstat_i64(migrate_timing.activ_commit).c_str());
        fprintf(out, "%smigrate_fetch: %s\n", pf,
                fmt_bstat_i64(migrate_timing.migrate_fetch).c_str());
        fprintf(out, "%smigrate_commit: %s\n", pf,
                fmt_bstat_i64(migrate_timing.migrate_commit).c_str());
    }
};


class PerCtxInfo {
    context *ctx;
    int id;
    int core_id;
    int curr_app_id;            // -1 iff no app
    int reserved_app_id;        // -1 iff not reserved
    int spilling_app_id;        // -1 iff no spill in progress
    struct {
        int count;
        int next_reg;
        int reg_limit;
        bool ghr_spfilled;
        int rs_tospfill;
        int rs_spfilled;
        int dtlb_tospfill;
        int dtlb_spfilled;
    } spfill;
    bool spill_dirty_only;
    bool spill_ghr;
    int spill_retstack_size;
    int spill_dtlb_size;

    void seek_spfill_reg(bool spill_not_fill) {
        // Leave "next_reg" at the next reg to spill/fill.  Skip zero regs.
        if (spill_not_fill && spill_dirty_only) {
            while ((spfill.next_reg < spfill.reg_limit) &&
                   !ctx->bmt_regdirty[spfill.next_reg])
                spfill.next_reg++;
        } else {
            while ((spfill.next_reg < spfill.reg_limit) &&
                   IS_ZERO_REG2(spfill.next_reg))
                spfill.next_reg++;
        }
    }
    bool next_spfill_notreg() const {
        return (spill_ghr && !spfill.ghr_spfilled) ||
            (spfill.rs_spfilled < spfill.rs_tospfill) ||
            (spfill.dtlb_spfilled < spfill.dtlb_tospfill);
    }
    void consume_notreg_spfill() {
        if (spill_ghr && !spfill.ghr_spfilled) {
            spfill.ghr_spfilled = true;
        } else if (spfill.rs_spfilled < spfill.rs_tospfill) {
            spfill.rs_spfilled++;
        } else if (spfill.dtlb_spfilled < spfill.dtlb_tospfill) {
            spfill.dtlb_spfilled++;
        } else {
            fprintf(stderr, "(%s:%i) shouldn't have been called, uh-oh.\n",
                    __FILE__, __LINE__);
            fflush(0);
            sim_abort();
        }
    }
    void prepare_spfill(AppState *as, bool spill_not_fill) {
        spfill.count = 0;
        spfill.next_reg = 0;
        spfill.reg_limit = 64;
        spfill.ghr_spfilled = false;
        if (spill_dirty_only) {
            spfill.rs_tospfill = (spill_not_fill) ? ctx->rs_size : 
                as->extra->bmt.spill_retstack.used;
        } else {
            spfill.rs_tospfill = ctx->params.retstack_entries;
        }
        spfill.rs_tospfill = MIN_SCALAR(spfill.rs_tospfill,
                                        spill_retstack_size);
        spfill.rs_spfilled = 0;
        spfill.dtlb_tospfill = 2 * spill_dtlb_size;     // *2: virt+phys addr
        spfill.dtlb_spfilled = 0;
        seek_spfill_reg(spill_not_fill);
        if (spfill.next_reg == spfill.reg_limit)
            spfill.next_reg = IZERO_REG;        // Force at least one sp/fill
    }
    void consume_spfill_reg(bool spill_not_fill) {
        sim_assert((spfill.reg_limit >= 0) &&
               (spfill.next_reg < spfill.reg_limit));
        spfill.count++;
        if (next_spfill_notreg()) {
            consume_notreg_spfill();
        } else {
            spfill.next_reg++;
            seek_spfill_reg(spill_not_fill);
        }
    }

    void dump_retstack(int app_id, bool spill_not_fill) const {
        printf("T%d/A%d %s retstack: [", ctx->id, app_id,
               (spill_not_fill) ? "pre-spill" : "post-fill");
        for (int i = 0; i < ctx->rs_size; i++) {
            mem_addr rs_val = ctx->return_stack[(ctx->rs_start + i) %
                ctx->params.retstack_entries];
            printf(" %s", fmt_x64(rs_val));
        }
        printf(" ]\n");
    }

public:
    PerCtxInfo(context *ctx_)
        : ctx(ctx_), id(ctx_->id), core_id(ctx_->core->core_id),
          curr_app_id(-1), reserved_app_id(-1),
          spilling_app_id(-1) {
        sim_assert(id >= 0);
        spfill.reg_limit = -1;
        spill_dirty_only = simcfg_get_bool("Hacking/spill_dirty_only");
        spill_ghr = simcfg_get_bool("Hacking/spill_ghr");

        spill_retstack_size = simcfg_get_int("Hacking/spill_retstack_size");
        if (spill_retstack_size < 0) {
            fprintf(stderr, "(%s:%i) bad spill_retstack_size: %d\n",
                    __FILE__, __LINE__, spill_retstack_size);
            exit(1);
        }
        spill_retstack_size = MIN_SCALAR(spill_retstack_size,
                                         ctx->params.retstack_entries);
        if (spill_retstack_size >
            NELEM(ctx->as->extra->bmt.spill_retstack.ents)) {
            fprintf(stderr, "(%s:%i) crufty spill_retstack.ents[] array "
                    "too small (nelem=%d)!\n", __FILE__, __LINE__,
                    NELEM(ctx->as->extra->bmt.spill_retstack.ents));
            exit(1);
        }

        spill_dtlb_size = simcfg_get_int("Hacking/spill_dtlb_size");
        if (spill_dtlb_size < 0) {
            fprintf(stderr, "(%s:%i) bad spill_dtlb_size: %d\n",
                    __FILE__, __LINE__, spill_dtlb_size);
            exit(1);
        }
        spill_dtlb_size = MIN_SCALAR(spill_dtlb_size,
                                     ctx->core->params.dtlb_entries);
        if (spill_dtlb_size >
            (int) NELEM(ctx->as->extra->bmt.spill_dtlb.ents)) {
            fprintf(stderr, "(%s:%i) crufty spill_dtlb.ents[] array "
                    "too small (nelem=%d)!\n", __FILE__, __LINE__,
                    NELEM(ctx->as->extra->bmt.spill_dtlb.ents));
            exit(1);
        }

    }

    context *g_ctx() { return ctx; }
    const context *g_ctx() const { return ctx; }
    int g_id() const { return id; }
    int g_core_id() const { return core_id; }
    bool is_free() const { return (curr_app_id == -1) && 
                               (reserved_app_id == -1); }
    bool is_running() const { return (curr_app_id >= 0); }
    bool is_reserved() const { return (reserved_app_id != -1); }
    bool is_reserved_for(int app_id) const {
        return (reserved_app_id == app_id);
    }
    int g_curr_app() const { sim_assert(is_running()); return curr_app_id; }

    bool spill_pending() const { return spilling_app_id >= 0; }
    int g_spilling_app() const { 
        sim_assert(spill_pending());
        return spilling_app_id;
    }
    void app_spill_begin() {
        sim_assert(!spill_pending());
        sim_assert(is_running());
        spilling_app_id = curr_app_id;
    }
    void app_spill_end() {
        // Note: a new app may have been scheduled in the meantime
        sim_assert(spill_pending());
        spilling_app_id = -1;
    }
    void app_starting(int app_id) {
        sim_assert(app_id != -1);
        sim_assert(is_free() ||
               (is_reserved() && (app_id == reserved_app_id)));
        curr_app_id = app_id;
        reserved_app_id = -1;
    }
    void app_stopping() {
        sim_assert(is_running());
        curr_app_id = -1;
    }
    void reserve_for_app(int app_id) {
        sim_assert(is_reserved_for(app_id) || is_free() || 
               (curr_app_id == app_id));
        DEBUGPRINTF("appmgr: T%d reserved for A%d\n", id, app_id);
        reserved_app_id = app_id;
    }
    void cancel_reservation(int app_id) {
        sim_assert(is_reserved_for(app_id));
        DEBUGPRINTF("appmgr: T%d cancelling reservation for A%d\n", id,
                    reserved_app_id);
        reserved_app_id = -1;
    }

    void prepare_spill() { prepare_spfill(ctx->as, true); }
    int next_spill_reg() const {
        sim_assert(spfill.reg_limit >= 0);
        if (next_spfill_notreg())
            return IZERO_REG;
        return (spfill.next_reg < spfill.reg_limit) ? spfill.next_reg : -1;
    }
    void consume_spill_reg() { consume_spfill_reg(true); }
    int spills_so_far() const { return spfill.count; }

    void prepare_fill(AppState *as) { prepare_spfill(as, false); }
    int next_fill_reg() const { return next_spill_reg(); }
    void consume_fill_reg() { consume_spfill_reg(false); }
    int fills_so_far() const { return spills_so_far(); }

    void spill_extra_state() {
        AppStateExtras *outbound = ctx->as->extra;
        sim_assert(outbound != NULL);
        outbound->bmt.spill_cyc = cyc;
        if (spill_ghr)
            outbound->bmt.spill_ghr = ctx->ghr;
        if (spill_retstack_size) {
            if (debug)
                dump_retstack(ctx->as->app_id, true);
            outbound->bmt.spill_retstack.used = 0;
            while (outbound->bmt.spill_retstack.used < spill_retstack_size) {
                mem_addr next_pc = wp_rs_pop(ctx);
                if (!next_pc)
                    break;
                outbound->bmt.spill_retstack.ents
                    [outbound->bmt.spill_retstack.used++] = next_pc;
            }
        }
    }

    void fill_extra_state() {
        AppStateExtras *inbound = ctx->as->extra;
        sim_assert(inbound != NULL);
        if (spill_ghr)
            ctx->ghr = inbound->bmt.spill_ghr;
        if (spill_retstack_size) {
            for (int i = inbound->bmt.spill_retstack.used - 1; i >= 0; i--)
                rs_push(ctx, inbound->bmt.spill_retstack.ents[i]);
            if (debug)
                dump_retstack(ctx->as->app_id, false);
        }
        if (spill_dtlb_size) {
            int inject_count = MIN_SCALAR(spill_dtlb_size,
                                          inbound->bmt.spill_dtlb.used);
            for (int i = 0; i < inject_count; i++) {
                int fill_idx = inbound->bmt.spill_dtlb.head - i;
                if (fill_idx < 0)
                    fill_idx += inbound->bmt.spill_dtlb.size;
                if (inbound->bmt.spill_dtlb.ents[fill_idx].ready_time <
                    inbound->bmt.spill_cyc) {
                    // Only consume value if it was ready before the spill
                    tlb_inject(ctx->core->dtlb, cyc,
                               inbound->bmt.spill_dtlb.ents[fill_idx].
                               base_addr,
                               ctx->as->app_id);
                }
            }
        }
    }
};


class PerCoreInfo {
    CoreResources *core;
    int id;
    IdSet contexts;
    int num_apps_sched;                         // scheduled to run
    int num_apps_stalled;                       // stalled but not evicted
    i64 last_startstop_cyc, last_stall_cyc;
    HistCount_Int tlp_sched_hist;               // excludes current apps
    HistCount_Int tlp_hist;                     // excludes current apps
    map<int,i64> app_laststop_cyc;
    
public:
    PerCoreInfo(CoreResources *core_)
        : core(core_), id(core_->core_id), num_apps_sched(0),
          num_apps_stalled(0),
          last_startstop_cyc(0), last_stall_cyc(0) {
        sim_assert(id >= 0);
    }
    const IdSet& g_contexts() const { return contexts; }
    CoreResources *g_core() const { return core; }
    int g_id() const { return id; }
    void add_ctx(int ctx_id) {
        sim_assert(!contexts.count(ctx_id));
        contexts.insert(ctx_id);
    }
    void app_starting() {
        sim_assert(num_apps_sched < (int) contexts.size());
        tlp_sched_hist.add_count(num_apps_sched, cyc - last_startstop_cyc);
        tlp_hist.add_count(num_apps_sched - num_apps_stalled,
                           cyc - last_stall_cyc);
        num_apps_sched++;
        last_startstop_cyc = last_stall_cyc = cyc;
    }
    void app_stopping(int app_id) {
        sim_assert(num_apps_sched > 0);
        tlp_sched_hist.add_count(num_apps_sched, cyc - last_startstop_cyc);
        tlp_hist.add_count(num_apps_sched - num_apps_stalled,
                           cyc - last_stall_cyc);
        num_apps_sched--;
        last_startstop_cyc = last_stall_cyc = cyc;
        app_laststop_cyc[app_id] = cyc;
    }
    void app_stalling_noevict() {
        sim_assert(num_apps_stalled < num_apps_sched);
        tlp_hist.add_count(num_apps_sched - num_apps_stalled,
                           cyc - last_stall_cyc);
        num_apps_stalled++;
        last_stall_cyc = cyc;
    }
    void app_stalldone_noevict() {
        sim_assert(num_apps_stalled > 0);
        tlp_hist.add_count(num_apps_sched - num_apps_stalled,
                           cyc - last_stall_cyc);
        num_apps_stalled--;
        last_stall_cyc = cyc;
    }
    void get_tlp_hist(HistCount_Int& dest, bool deduct_nonrun) const {
        if (deduct_nonrun) {
            dest = tlp_hist;
            dest.add_count(num_apps_sched - num_apps_stalled,
                           cyc - last_stall_cyc);
        } else {
            dest = tlp_sched_hist;
            dest.add_count(num_apps_sched, cyc - last_startstop_cyc);
        }
    }
    int context_count() const { return contexts.size(); }
    int sched_count() const { return num_apps_sched; }
    int idle_count() const { return context_count() - num_apps_sched; }
    i64 app_last_seen(int app_id) const {
        return (app_laststop_cyc.count(app_id)) ? 
            (app_laststop_cyc.find(app_id)->second) : -1;
    }
    i64 get_last_startstop() const { return last_startstop_cyc; }
};


typedef map<int, PerAppInfo *> AppInfoMap;
typedef map<int, PerCtxInfo *> CtxInfoMap;
typedef map<int, PerCoreInfo *> CoreInfoMap;

// Info tracked by the manager, which is made available for read-only
// consultation by the scheduler.
class MgrSchedInfo {
    AppInfoMap app;
    CtxInfoMap ctx;
    CoreInfoMap core;
    IdSet app_ids, ctx_ids, core_ids;

public:
    MgrSchedInfo();
    ~MgrSchedInfo();
    void setup_done();

    void add_ready_app(AppState *app_) {
        if (app.count(app_->app_id))
            sim_abort();
        app.insert(make_pair(app_->app_id, new PerAppInfo(app_)));
        app_ids.insert(app_->app_id);
    }
    void remove_app(int app_id) {
        AppInfoMap::iterator found = app.find(app_id);
        if (found == app.end()) {
            abort_printf("remove_app: unknown app_id %d\n", app_id);
        }
        delete found->second;
        app.erase(found);
        app_ids.erase(app_id);
    }

    void add_ctx(context *ctx_) {
        if (ctx.count(ctx_->id))
            sim_abort();
        ctx.insert(make_pair(ctx_->id, new PerCtxInfo(ctx_)));
        ctx_ids.insert(ctx_->id);
        if (!core.count(ctx_->core->core_id)) {
            core.insert(make_pair(ctx_->core->core_id,
                                  new PerCoreInfo(ctx_->core)));
            core_ids.insert(ctx_->core->core_id);
        }
        get_coreinfo(ctx_->core->core_id).add_ctx(ctx_->id);
    }

    const PerAppInfo& get_appinfo(int app_id) const {
        sim_assert(app_id >= 0);
        AppInfoMap::const_iterator found = app.find(app_id);
        if (found == app.end()) {
            fflush(0);
            fprintf(stderr, "MgrInfo::get_appinfo(): app %d not found!\n",
                    app_id);
            sim_abort();
        }
        return *(found->second);
    }
    PerAppInfo& get_appinfo(int app_id) {
        const PerAppInfo& result =
            const_cast<const MgrSchedInfo *>(this)->get_appinfo(app_id);
        return const_cast<PerAppInfo&>(result);
    }
    const IdSet& get_app_ids() const { return app_ids; }
    const int app_count() const { return static_cast<int>(app_ids.size()); }

    const PerCtxInfo& get_ctxinfo(int ctx_id) const {
        sim_assert(ctx_id >= 0);
        CtxInfoMap::const_iterator found = ctx.find(ctx_id);
        if (found == ctx.end()) {
            fflush(0);
            fprintf(stderr, "MgrInfo::get_ctxinfo(): context %d not found!\n",
                    ctx_id);
            sim_abort();
        }
        return *(found->second);
    }
    PerCtxInfo& get_ctxinfo(int ctx_id) {
        const PerCtxInfo& result =
            static_cast<const MgrSchedInfo *>(this)->get_ctxinfo(ctx_id);
        return const_cast<PerCtxInfo&>(result);
    }
    const IdSet& get_ctx_ids() const { return ctx_ids; }
    const int ctx_count() const { return static_cast<int>(ctx_ids.size()); }

    const PerCoreInfo& get_coreinfo(int core_id) const {
        sim_assert(core_id >= 0);
        CoreInfoMap::const_iterator found = core.find(core_id);
        if (found == core.end()) {
            fflush(0);
            fprintf(stderr, "MgrInfo::get_coreinfo(): core %d not found!\n",
                    core_id);
            sim_abort();
        }
        return *(found->second);
    }
    PerCoreInfo& get_coreinfo(int core_id) {
        const PerCoreInfo& result = 
            static_cast<const MgrSchedInfo *>(this)->get_coreinfo(core_id);
        return const_cast<PerCoreInfo&>(result);
    }
    const IdSet& get_core_ids() const { return core_ids; }
    const int core_count() const { return static_cast<int>(core_ids.size()); }
    const IdVec g_core_idvec() const {
        const IdSet& ids = get_core_ids();
        IdVec result(ids.begin(), ids.end());
        return result;
    }
    IdSet get_core_apps(const PerCoreInfo& crinfo) const {
        IdSet core_apps;
        IdSet::const_iterator iter = crinfo.g_contexts().begin(),
            end = crinfo.g_contexts().end();
        for (; iter != end; ++iter) {
            const PerCtxInfo& cinfo = get_ctxinfo(*iter);
            if (cinfo.is_running()) {
                int app_id = cinfo.g_curr_app();
                sim_assert(!core_apps.count(app_id));
                core_apps.insert(app_id);
            }
        }
        return core_apps;
    };

    bool enough_contexts_hack() const {
        return ctx.size() >= app.size();
    }
    bool ctx_same_core(int ctx1, int ctx2) const {
        const PerCtxInfo& cinfo1 = get_ctxinfo(ctx1);
        const PerCtxInfo& cinfo2 = get_ctxinfo(ctx2);
        return cinfo1.g_core_id() == cinfo2.g_core_id();
    }
    bool ctx_app_running(int ctx_id) const {
        const PerCtxInfo& cinfo = get_ctxinfo(ctx_id);
        if (!cinfo.is_running())
            return false;
        const PerAppInfo& ainfo = get_appinfo(cinfo.g_curr_app());
        return ainfo.g_state() == AI_Running;
    }

    int appcount_state(int state) const {
        int count = 0;
        IdSet::const_iterator iter = app_ids.begin(), end = app_ids.end();
        for (; iter != end; ++iter) {
            const PerAppInfo& ainfo = get_appinfo(*iter);
            if (ainfo.g_state() == state)
                count++;
        }
        return count;
    }

    int core_appcount_state(const PerCoreInfo& crinfo, int state) const {
        int count = 0;
        IdSet::const_iterator iter = crinfo.g_contexts().begin(),
            end = crinfo.g_contexts().end();
        for (; iter != end; ++iter) {
            const PerCtxInfo& cinfo = get_ctxinfo(*iter);
            if (!cinfo.is_running())
                continue;
            const PerAppInfo& ainfo = get_appinfo(cinfo.g_curr_app());
            if (ainfo.g_state() == state)
                count++;
        }
        return count;
    }
    int core_running_apps(const PerCoreInfo& crinfo) const {
        return core_appcount_state(crinfo, AI_Running);
    }
    int core_swapout_apps(const PerCoreInfo& crinfo) const {
        return core_appcount_state(crinfo, AI_SwapOut_LongMiss) +
            core_appcount_state(crinfo, AI_SwapOut_Migrate) +
            core_appcount_state(crinfo, AI_SwapOut_Sched);
    }

    int app_current_ctx(const PerAppInfo& ainfo) const {
        return (ainfo.is_sched()) ? ainfo.g_ctx_id() : -1;
    };
    int app_current_core(const PerAppInfo& ainfo) const {
        int current_ctx = app_current_ctx(ainfo);
        return (current_ctx >= 0) ? get_ctxinfo(current_ctx).g_core_id() : -1;
    };
    int app_prev_core(const PerAppInfo& ainfo) const {
        int prev_ctx = ainfo.g_prev_ctx();
        return (prev_ctx >= 0) ? get_ctxinfo(prev_ctx).g_core_id() : -1;
    };

    int core_free_ctxs(const PerCoreInfo& crinfo) const {
        int result = 0;
        IdSet::const_iterator iter = crinfo.g_contexts().begin(),
            end = crinfo.g_contexts().end();
        for (; iter != end; ++iter) {
            const PerCtxInfo& cinfo = get_ctxinfo(*iter);
            if (cinfo.is_free())
                result++;
        }
        return result;
    }

    int total_free_ctxs() const {
        int result = 0;
        IdSet::const_iterator iter = core_ids.begin(),
            end = core_ids.end();
        for (; iter != end; ++iter)
            result += core_free_ctxs(get_coreinfo(*iter));
        return result;  
    }

    int total_notsched_apps() const {
        int result = 0;
        IdSet::const_iterator iter = app_ids.begin(),
            end = app_ids.end();
        for (; iter != end; ++iter) {
            const PerAppInfo& ainfo = get_appinfo(*iter);
            if (!ainfo.is_sched())
                result++;
        }
        return result;  
    }

    bool core_full(int core_id, bool only_running, bool deduct_swapout) 
        const {
        const PerCoreInfo& crinfo = get_coreinfo(core_id);
        bool result;
        if (only_running) {
            result = core_running_apps(crinfo) == crinfo.context_count();
        } else if (deduct_swapout) {
            result = (crinfo.sched_count() - core_swapout_apps(crinfo)) ==
                crinfo.context_count();
        } else {
            result = core_free_ctxs(crinfo) == 0;
        }
        return result;
    };

    int core_load_count(const PerCoreInfo& crinfo,
                        bool only_running,
                        const IdSet& to_ignore) const {
        IdSet apps = get_core_apps(crinfo);
        int load_count = 0;
        for (IdSet::const_iterator iter = apps.begin(); iter != apps.end();
             ++iter) {
            const PerAppInfo& ainfo = get_appinfo(*iter);
            bool count = true;
            if (only_running)
                count = (ainfo.g_state() == AI_Running);
            if (count && !to_ignore.count(*iter))
                load_count++;
        }
        return load_count;
    }
    int core_load_count(const PerCoreInfo& crinfo,
                        bool only_running) const {
        IdSet empty_set;
        return core_load_count(crinfo, only_running, empty_set);
    }

    double core_load_factor(const PerCoreInfo& crinfo,
                            bool only_running) const {
        sim_assert(crinfo.context_count() > 0);
        double result =
            (double) core_load_count(crinfo, only_running) /
            crinfo.context_count();
        return result;
    }

    // Find an idle context on this core, or -1
    int core_idle_ctx(const PerCoreInfo& crinfo) const {
        IdSet::const_iterator iter = crinfo.g_contexts().begin(),
            end = crinfo.g_contexts().end();
        for (; iter != end; ++iter) {
            const PerCtxInfo& cinfo = get_ctxinfo(*iter);
            if (cinfo.is_free())
                return cinfo.g_id();
        }
        return -1;
    }

    // Sum recent IPC values of apps scheduled on this core
    double core_recent_ipc(const PerCoreInfo& crinfo,
                           bool only_running) const {
        double sum = 0;
        IdSet::const_iterator iter = crinfo.g_contexts().begin(),
            end = crinfo.g_contexts().end();
        for (; iter != end; ++iter) {
            const PerCtxInfo& cinfo = get_ctxinfo(*iter);
            if ((!only_running && cinfo.is_running()) ||
                ctx_app_running(*iter)) {
                const PerAppInfo& ainfo = get_appinfo(cinfo.g_curr_app());
                sum += ainfo.recent_ipc_commit();
            }
        }
        return sum;
    }

    // cyc an app was resident on a core
    i64 core_resident_cyc(int core_id, int app_id) const {
        const PerCoreInfo& crinfo = get_coreinfo(core_id);
        const PerAppInfo& ainfo = get_appinfo(app_id);
        i64 sum = 0;
        IdSet::const_iterator iter = crinfo.g_contexts().begin(),
            end = crinfo.g_contexts().end();
        for (; iter != end; ++iter) {
            sum += ainfo.ctx_resident_cyc(*iter);
        }
        return sum;
    };

    // times an app was swapped in to a core
    i64 core_swapin_count(int core_id, int app_id) const {
        const PerCoreInfo& crinfo = get_coreinfo(core_id);
        const PerAppInfo& ainfo = get_appinfo(app_id);
        i64 sum = 0;
        IdSet::const_iterator iter = crinfo.g_contexts().begin(),
            end = crinfo.g_contexts().end();
        for (; iter != end; ++iter) {
            sum += ainfo.ctx_swapin_count(*iter);
        }
        return sum;
    };

    int biggest_core_contexts() const {
        CoreInfoMap::const_iterator iter = core.begin(), end = core.end();
        int max_val = 0;
        for (; iter != end; ++iter) {
            const PerCoreInfo& crinfo = *(iter->second);
            int val = crinfo.context_count();
            if (val > max_val)
                max_val = val;
        }
        sim_assert(max_val > 0);
        return max_val;
    }

    int least_loaded_core(const IdSet& cores, bool only_running,
                          int recent_app) const {
        // If recent_app is >= 0, use most-recent-seen core to break ties
        int min_id = -1;
        double min_val = 0;
        IdVec core_ord(cores.begin(), cores.end());
        shuffle_idvec(core_ord);
        IdVec::const_iterator iter = core_ord.begin(), end = core_ord.end();
        for (; iter != end; ++iter) {
            const PerCoreInfo& crinfo = get_coreinfo(*iter);
            if (!core_free_ctxs(crinfo)) {
                continue;
            }
            double val = core_load_factor(crinfo, only_running);
            if (((val < min_val) || (min_id == -1)) ||
                ((val == min_val) && (recent_app >= 0) && 
                 (crinfo.app_last_seen(recent_app) >
                  get_coreinfo(min_id).app_last_seen(recent_app)))) {
                min_id = crinfo.g_id();
                min_val = val;
            }
        }
        DEBUGPRINTF("least_loaded_core -> %d\n", min_id);
        sim_assert(min_id >= 0);
        return min_id;
    }

    int least_ipc_core(bool only_running) const {
        int min_id = -1;
        double min_val = 0;
        IdVec core_ord = g_core_idvec();
        shuffle_idvec(core_ord);
        IdVec::const_iterator iter = core_ord.begin(), end = core_ord.end();
        for (; iter != end; ++iter) {
            const PerCoreInfo& crinfo = get_coreinfo(*iter);
            if (!core_free_ctxs(crinfo)) { continue; }
            double val = core_recent_ipc(crinfo, only_running);
            if ((val < min_val) || (min_id == -1)) {
                min_id = crinfo.g_id();
                min_val = val;
            }
        }
        sim_assert(min_id >= 0);
        return min_id;
    }
};


MgrSchedInfo::MgrSchedInfo()
{
}


MgrSchedInfo::~MgrSchedInfo()
{
    {
        AppInfoMap::iterator iter = app.begin(), end = app.end();
        for (; iter != end; ++iter)
            delete iter->second;
    }
    {
        CtxInfoMap::iterator iter = ctx.begin(), end = ctx.end();
        for (; iter != end; ++iter)
            delete iter->second;
    }
    {
        CoreInfoMap::iterator iter = core.begin(), end = core.end();
        for (; iter != end; ++iter)
            delete iter->second;
    }
}


void
MgrSchedInfo::setup_done()
{
}


// Abstract application scheduler: selects next app to run
class AppSched {
protected:
    const MgrSchedInfo& mgr_info;

public:
    AppSched(const MgrSchedInfo& mgr_info_) : mgr_info(mgr_info_) { }
    virtual ~AppSched() { }

    virtual void app_ready(int app_id) = 0;
    virtual void app_notready(int app_id) = 0;
    virtual bool will_schedule() const = 0;
    virtual int schedule_one() = 0;
    virtual void undo_schedule(int app_id) = 0;
};


// Choose app which has been idle the longest (FIFO)
class ASched_OldestApp : public AppSched {
protected:
    deque<int> ready_order;
    set<int> ready_all;
public:
    ASched_OldestApp(const MgrSchedInfo& mgr_info_)
        : AppSched(mgr_info_) { }
    virtual ~ASched_OldestApp() { };
    virtual void app_ready(int app_id) {
        sim_assert(!ready_all.count(app_id));
        ready_order.push_back(app_id);
        ready_all.insert(app_id);
    }
    virtual void app_notready(int app_id) {
        if (ready_all.count(app_id)) {
            ready_all.erase(app_id);
            deque_erase_int(ready_order, app_id);
        }
    }
    virtual bool will_schedule() const { return !ready_order.empty(); }
    virtual int schedule_one() {
        sim_assert(!ready_order.empty());
        int next_app = ready_order.front();
        ready_order.pop_front();
        sim_assert(ready_all.count(next_app));
        ready_all.erase(next_app);
        return next_app;
    }
    virtual void undo_schedule(int app_id) {
        sim_assert(!ready_all.count(app_id));
        ready_order.push_front(app_id);
        ready_all.insert(app_id);
    }
};


// Abstract context scheduler: selects next context to run an app, -1 for none
class CtxSched {
protected:
    const MgrSchedInfo& mgr_info;
    bool deduct_nonrun;
    IdSet idle_set;

public:
    CtxSched(const MgrSchedInfo& mgr_info_) : mgr_info(mgr_info_) { 
        deduct_nonrun = simcfg_get_bool("Hacking/csched_deduct_nonrun");
    }
    virtual ~CtxSched() { }

    virtual void ctx_idle(int ctx_id) = 0;
    virtual void ctx_notidle(int ctx_id) = 0;
    virtual bool will_schedule() const = 0;
    virtual int schedule_one(int app_id) = 0;

    // If supported, say which core this app would be scheduled on, if
    // it was schedulable now and not already running.  Somewhat sketchy.
    virtual int schedule_guess_core(int app_id) {
        return -1;
    }
};


// A common scheduler pattern: selects contexts based on mgr_info
// contents, but without using extra state to track which contexts are idle,
// etc.
class CtxSched_MgrInfo : public CtxSched {
public:
    CtxSched_MgrInfo(const MgrSchedInfo& mgr_info_)
        : CtxSched(mgr_info_) { }
    virtual ~CtxSched_MgrInfo() { };
    virtual void ctx_idle(int ctx_id) {
        sim_assert(!idle_set.count(ctx_id));
        idle_set.insert(ctx_id);
    }
    virtual void ctx_notidle(int ctx_id) { 
        if (idle_set.count(ctx_id)) {
            idle_set.erase(ctx_id);
        }
    }
    virtual bool will_schedule() const { return !idle_set.empty(); }
    virtual int schedule_core(int app_id) {     // ret -1: wait
        // you must override this if you don't override schedule_one()
        abort_printf("schedule_core() called w/o implementation!\n");
        return 0;
    }
    virtual int schedule_one(int app_id) {
        // By default, use schedule_core() to select a core, then choose some
        // idle context from that.
        sim_assert(!idle_set.empty());
        int next_core = schedule_core(app_id);
        int next_ctx = -1;
        if (next_core >= 0) {
            next_ctx =
                mgr_info.core_idle_ctx(mgr_info.get_coreinfo(next_core));
            sim_assert(next_ctx >= 0);
            sim_assert(idle_set.count(next_ctx));
            idle_set.erase(next_ctx);
        }
        return next_ctx;
    }
};


// Select the context which became idle first
class CSched_FirstIdle : public CtxSched {
protected:
    deque<int> idle_order;
public:
    CSched_FirstIdle(const MgrSchedInfo& mgr_info_)
        : CtxSched(mgr_info_) { }
    virtual ~CSched_FirstIdle() { };
    virtual void ctx_idle(int ctx_id) {
        sim_assert(!idle_set.count(ctx_id));
        idle_set.insert(ctx_id);
        idle_order.push_back(ctx_id);
    }
    virtual void ctx_notidle(int ctx_id) { 
        if (idle_set.count(ctx_id)) {
            idle_set.erase(ctx_id);
            deque_erase_int(idle_order, ctx_id);
        }
    }
    virtual bool will_schedule() const { return !idle_order.empty(); }
    virtual int schedule_one(int app_id) {
        sim_assert(!idle_order.empty());
        int next_ctx = idle_order.front();
        idle_order.pop_front();
        idle_set.erase(next_ctx);
        return next_ctx;
    }
};


// Select least-loaded core
class CSched_LightestLoad : public CtxSched_MgrInfo {
    int get_lightest_ctx() const {
        int least_core = mgr_info.least_loaded_core(mgr_info.get_core_ids(),
                                                    deduct_nonrun, -1);
        const PerCoreInfo& crinfo = mgr_info.get_coreinfo(least_core);
        int ctx_id = mgr_info.core_idle_ctx(crinfo);
        if (ctx_id < 0) {
            printf("No lightest on C%d!\n", least_core);
            for (int i = 0; i<4; i++) {
                const PerCtxInfo& cinfo = mgr_info.get_ctxinfo(i);
                printf(" T%d:A%d%s", i,
                       cinfo.is_running() ? cinfo.g_curr_app() : -1,
                       cinfo.is_reserved() ? "(R)" : "");
            }
            printf("\n");
            sim_abort();
        }
        return ctx_id;
    }
public:
    CSched_LightestLoad(const MgrSchedInfo& mgr_info_)
        : CtxSched_MgrInfo(mgr_info_) { }
    virtual ~CSched_LightestLoad() { };
    virtual int schedule_one(int app_id) {
        sim_assert(!idle_set.empty());
        int next_ctx = get_lightest_ctx();
        sim_assert(idle_set.count(next_ctx));
        idle_set.erase(next_ctx);
        return next_ctx;
    }
};


class CSched_LeastIpc : public CtxSched_MgrInfo {
    int get_lightest_ctx() const {
        int least_core = mgr_info.least_ipc_core(deduct_nonrun);
        const PerCoreInfo& crinfo = mgr_info.get_coreinfo(least_core);
        int ctx_id = mgr_info.core_idle_ctx(crinfo);
        sim_assert(ctx_id >= 0);
        return ctx_id;
    }
public:
    CSched_LeastIpc(const MgrSchedInfo& mgr_info_)
        : CtxSched_MgrInfo(mgr_info_) { }
    virtual ~CSched_LeastIpc() { };
    virtual int schedule_one(int app_id) {      
        sim_assert(!idle_set.empty());
        int next_ctx = get_lightest_ctx();
        sim_assert(idle_set.count(next_ctx));
        idle_set.erase(next_ctx);
        return next_ctx;
    }
};


class CSched_Static : public CtxSched_MgrInfo {
protected:
    map<int,int> static_sched;  // maps app ID to ctx#
    map<int,int> static_rev;
    bool allow_missing_apps;
public:
    CSched_Static(const MgrSchedInfo& mgr_info_)
        : CtxSched_MgrInfo(mgr_info_) {
        allow_missing_apps =
            simcfg_get_bool("Hacking/StaticSched/allow_missing_apps");
    }
    virtual ~CSched_Static() { };
    virtual int schedule_one(int app_id) {
        sim_assert(!idle_set.empty());
        int next_ctx = -1;
        if (!static_sched.count(app_id)) {
            char key_name[80];
            e_snprintf(key_name, sizeof(key_name), "Hacking/StaticSched/A%d",
                       app_id);
            if (!simcfg_have_val(key_name)) {
                if (!allow_missing_apps) {
                    fprintf(stderr, "%s:%i: StaticSched missing app A%d\n",
                            __FILE__, __LINE__, app_id);
                    exit(1);
                }
            } else {
                next_ctx = simcfg_get_int(key_name);
                if (((next_ctx < 0) && !allow_missing_apps) ||
                    (next_ctx >= CtxCount)) {
                    fprintf(stderr, "%s:%i: invalid context %d in StaticSched "
                            "for A%d\n", __FILE__, __LINE__, next_ctx,
                            app_id);
                    exit(1);
                }
            }
            if (next_ctx >= 0) {
                if (static_rev.count(next_ctx)) {
                    fprintf(stderr, "%s:%i: invalid StaticSched; context %d "
                            "specified for A%d and A%d\n", __FILE__, __LINE__,
                            next_ctx, static_rev[next_ctx], app_id);
                    exit(1);
                }
                static_rev[next_ctx] = app_id;
            }
            static_sched[app_id] = next_ctx;
        } else {
            next_ctx = static_sched[app_id];
        }
        if (next_ctx >= 0) {
            sim_assert(idle_set.count(next_ctx));
            idle_set.erase(next_ctx);
        }
        return next_ctx;
    }
};


class CSched_StaticSetAffin : public CtxSched_MgrInfo {
protected:
    typedef map<int,IdSet> AppSetMap;
    AppSetMap app_to_cores;
    bool force_sched;           // schedule outside set rather than wait

    int get_lightest_ctx(const IdSet& cores, int app_id) const {
        int least_core = mgr_info.least_loaded_core(cores, deduct_nonrun,
                                                    app_id);
        const PerCoreInfo& crinfo = mgr_info.get_coreinfo(least_core);
        int ctx_id = mgr_info.core_idle_ctx(crinfo);
        return ctx_id;
    }

    void dump() const {
        AppSetMap::const_iterator iter = app_to_cores.begin(),
            end = app_to_cores.end();
        for (; iter != end; ++iter) {
            IdSet::const_iterator iter2 = iter->second.begin(),
                end2 = iter->second.end();
            printf("A%d{", iter->first);
            for (; iter2 != end2; ++iter2)
                printf(" %d", *iter2);
            printf(" }\n");
        }
    }

    void read_affin_set(int app_id) {
        char key_name[80];
        e_snprintf(key_name, sizeof(key_name), "Hacking/StaticAffin/A%d",
                   app_id);
        string corestr(simcfg_get_str(key_name)); // comma-seperated ints
        const char *core_cstr = corestr.c_str();
        int scan = 0;
        IdSet core_set;
        SimpleRESubstr cap[2];
        while (simple_pre_fixcap("^([0-9]+),?", core_cstr + scan, 0, cap,
                                 NELEM(cap)) > 0) {
            string sub(core_cstr + scan, cap[1].len);
            int core_id = atoi(sub.c_str());
            core_set.insert(core_id);
            scan += cap[0].len;
        }
        if (scan < (int) corestr.size()) {
            fflush(0);
            fprintf(stderr, "Unmatched text in %s: \"%s\"\n", key_name,
                    core_cstr + scan);
            exit(1);
        }
        app_to_cores[app_id] = core_set;
    }

public:
    CSched_StaticSetAffin(const MgrSchedInfo& mgr_info_)
        : CtxSched_MgrInfo(mgr_info_) { 
        force_sched = simcfg_get_bool("Hacking/StaticAffin/force_sched");
    }
    virtual ~CSched_StaticSetAffin() { }
    virtual int schedule_one(int app_id) {
        sim_assert(!idle_set.empty());
//      const PerAppInfo& ainfo = mgr_info.get_appinfo(app_id);
//      int prev_ctx_id = ainfo.g_prev_ctx();
        if (!app_to_cores.count(app_id))
            read_affin_set(app_id);
        const IdSet& cand_cores = app_to_cores[app_id];
        if (cand_cores.empty()) {
            fflush(0);
            fprintf(stderr, "Empty core set for app A%d\n", app_id);
            sim_abort();
        }
        int next_ctx = get_lightest_ctx(cand_cores, app_id);
        if (force_sched && (next_ctx < 0)) {
            // Nothing available in affinity-set; look outside
            next_ctx = get_lightest_ctx(mgr_info.get_core_ids(), -1);
            if (next_ctx < 0) {
                fflush(0);
                fprintf(stderr, "No context found for A%d, "
                        "even with force_sched\n", app_id);
                sim_abort();
            }
            printf("appmgr: force_sched A%d forced onto T%d\n", app_id,
                   next_ctx);
        }
        if (next_ctx >= 0)
            idle_set.erase(next_ctx);
        return next_ctx;
    }
};


class CSched_MutableMap : public CtxSched_MgrInfo {
protected:
    map<int,int> sched; // maps app ID to core number
    map<int,int> core_sched_count;      // core_id -> #apps sched

    void read_initial_sched() {
        char name[40];
        int app_num = 0;
        for (;;) {
            e_snprintf(name, sizeof(name), "Hacking/MutableMap/A%d", app_num);
            if (simcfg_have_val(name)) {
                int core_num = simcfg_get_int(name);
                sched_add_app(app_num, core_num);
                ++app_num;
            } else {
                break;
            }
        }
    }
public:
    CSched_MutableMap(const MgrSchedInfo& mgr_info_)
        : CtxSched_MgrInfo(mgr_info_) {
        read_initial_sched();
    }
    virtual ~CSched_MutableMap() { };
    virtual int schedule_core(int app_id) {
        int result;
        if (sched.count(app_id)) {
            result = sched.find(app_id)->second;
        } else {
            result = -1;
        }
        if ((result >= 0) && mgr_info.core_full(result, false, false)) {
            DEBUGPRINTF("MutableMap: C%d full, skipping A%d\n",
                        result, app_id);
            result = -1;
        }
        return result;
    }
    virtual int schedule_guess_core(int app_id) {
        return schedule_core(app_id);
    }

    // Extra methods for altering and querying the schedule
    void sched_clear() {
        sched.clear();
        core_sched_count.clear();
    }
    void sched_add_app(int app_id, int core_id) {
        if (sched.count(app_id)) {
            abort_printf("sched_add_app(A%d, C%d): app already sched on C%d\n",
                         app_id, core_id, sched.find(app_id)->second);
        }
        if (core_id >= 0) {
            core_sched_count[core_id]++;
        }
        sched[app_id] = core_id;
    }
    void sched_remove_app(int app_id) {
        if (sched.count(app_id)) {
            int core_id = sched[app_id];
            core_sched_count[core_id]--;
            sched.erase(app_id);
        }
    }
    int g_core_sched_count(int core_id) const {
        map<int,int>::const_iterator found = core_sched_count.find(core_id);
        int result = (found != core_sched_count.end()) ? found->second : 0;
        return result;
    }
    bool is_core_oversubscribed(int core_id) const {
        const PerCoreInfo& crinfo = mgr_info.get_coreinfo(core_id);
        bool is_full = mgr_info.core_full(core_id, false, true);
        return is_full && 
            (g_core_sched_count(core_id) > crinfo.context_count());
    }
    bool sched_empty() const {
        return sched.empty();
    }
    int g_app_core(int app_id) const {
        map<int,int>::const_iterator found = sched.find(app_id);
        int result = (found != sched.end()) ? found->second : -1;
        return result;
    }
};


class AppSwapGate {
protected:
    const MgrSchedInfo& mgr_info;
    bool deduct_nonrun, deduct_swapout;

public:
    AppSwapGate(const MgrSchedInfo& mgr_info_) : mgr_info(mgr_info_) { 
        deduct_nonrun = simcfg_get_bool("Hacking/swapgate_deduct_nonrun");
        deduct_swapout = simcfg_get_bool("Hacking/swapgate_deduct_swapout");
    }
    virtual ~AppSwapGate() { }
    virtual bool should_swap_out(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const = 0;
};


class Swap_IfProcFull : public AppSwapGate {
public:
    Swap_IfProcFull(const MgrSchedInfo& mgr_info_) 
        : AppSwapGate(mgr_info_) { }
    virtual ~Swap_IfProcFull() { };
    virtual bool should_swap_out(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const {
        // Swap only if: |apps| > |contexts|
        return !mgr_info.enough_contexts_hack();
    }
};


class Swap_IfCoreFull : public AppSwapGate {
public:
    Swap_IfCoreFull(const MgrSchedInfo& mgr_info_) 
        : AppSwapGate(mgr_info_) { }
    virtual ~Swap_IfCoreFull() { };
    virtual bool should_swap_out(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const {
        // Swap only if: this core has no idle context
        return mgr_info.core_full(cinfo.g_core_id(), deduct_nonrun,
                                  deduct_swapout);
    }
};


class Swap_IfNotSolo : public AppSwapGate {
public:
    Swap_IfNotSolo(const MgrSchedInfo& mgr_info_) 
        : AppSwapGate(mgr_info_) { }
    virtual ~Swap_IfNotSolo() { };
    virtual bool should_swap_out(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const {
        const PerCoreInfo& crinfo = mgr_info.get_coreinfo(cinfo.g_core_id());
        bool result;
        // Swap only if: this app isn't solo on core, or core has one context
        if (crinfo.context_count() == 1) {
            result = true;
        } else {
            int active_count = crinfo.sched_count();
            if (deduct_nonrun) {
                active_count = mgr_info.core_running_apps(crinfo);
            } else if (deduct_swapout) {
                active_count -= mgr_info.core_swapout_apps(crinfo);
            }
            result = active_count > 1;
        }
        return result;
    }
};


class Swap_Always : public AppSwapGate {
public:
    Swap_Always(const MgrSchedInfo& mgr_info_) 
        : AppSwapGate(mgr_info_) { }
    virtual ~Swap_Always() { };
    virtual bool should_swap_out(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const {
        return true;
    }
};


class Swap_Never : public AppSwapGate {
public:
    Swap_Never(const MgrSchedInfo& mgr_info_) 
        : AppSwapGate(mgr_info_) { }
    virtual ~Swap_Never() { };
    virtual bool should_swap_out(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const {
        return false;
    }
};


// Only works with MutableMap context-scheduler, at the moment
class Swap_IfCoreOversubscribed : public AppSwapGate {
protected:
    const CSched_MutableMap& ctx_sched;
public:
    Swap_IfCoreOversubscribed(const MgrSchedInfo& mgr_info_,
                              const CSched_MutableMap& ctx_sched_) 
        : AppSwapGate(mgr_info_), ctx_sched(ctx_sched_) {
    }
    virtual ~Swap_IfCoreOversubscribed() { };
    virtual bool should_swap_out(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const {
        bool result = ctx_sched.is_core_oversubscribed(cinfo.g_core_id());
        return result;
    }
};


struct PendingMigrateInfo {
    NoDefaultCopy nocopy;

    int app_id;
    int targ_core_id;
    int reserved_ctx_id;                // -1 => none
    // NULL => none (OWNED, but not in a CallbackQueue)
    scoped_ptr<CBQ_Callback> migrate_done_cb;
    // NULL => none; owned by GlobalEventQueue
    scoped_ptr<CBQ_Callback> recheck_callback; 
    bool migrate_in_progress;           // flag: migrate_app_soon2 reached

    PendingMigrateInfo(int app_id_, int targ_core_id_, int reserved_ctx_id_,
                       CBQ_Callback *migrate_done_cb_)
        : app_id(app_id_), targ_core_id(targ_core_id_),
          reserved_ctx_id(reserved_ctx_id_), migrate_done_cb(migrate_done_cb_),
          recheck_callback(NULL),
          migrate_in_progress(false)
    { }
    ~PendingMigrateInfo() {
        if (recheck_callback)
            callbackq_cancel_ret(GlobalEventQueue, recheck_callback.get());
    }
};


} // Anonymous namespace close


struct AppMgr {
private:
    class RunnerSampler;

    AppMgrParams params;
    MgrSchedInfo mgr_info;
    AppSched *app_sched;
    CtxSched *ctx_sched;
    AppSwapGate *swap_gate;
    bool setup_done_flag;
    bool swap_suppress_guess;   // swap-out supression based on schedule guess
    bool inst_spill_fill;
    bool inst_spill_fill_early;
    int regs_per_sf_block;
    i64 min_swapin_commits;
    i64 min_swapin_cyc;
    typedef map<int, PendingMigrateInfo *> PendingMigrateMap;
    PendingMigrateMap pending_migrates;
    IdSet pending_halts;

    void schedule_apps();
    void sched_hook() { schedule_apps(); }
    void start_app(int app_id, int ctx_id);

    enum CInfoCBSel {
        SwapOutGenSpill, SwapInGenFill, StaticSwapInDone, StaticSwapOutDone
    };
    class CInfoCB;
    CInfoCB *gen_cinfo_cb(PerCtxInfo& cinfo, CInfoCBSel which);
    class MigrateRecheckCB;
    class MigrateTimeoutCB;
    class MigrateCancelCB;
    class HaltRecheckCB;

    void swapin_done_callback(PerCtxInfo& cinfo);
    void swapout_done_callback(PerCtxInfo& cinfo,
                               bool context_now_avail,
                               bool final_spill_committed);
    void swapin_genfill_callback(PerCtxInfo& cinfo);
    void swapout_genspill_callback(PerCtxInfo& cinfo);

    bool any_progress_since_swapin(const PerAppInfo& ainfo) const;
    bool enough_progress_since_swapin(const PerAppInfo& ainfo) const;
    bool swap_suppress_guess_test(const PerAppInfo& ainfo,
                                 const PerCtxInfo& cinfo) const;
    void migrate_running_app(int app_id, int target_ctx_id,
                             CtxHaltStyle halt_style);
    bool migrate_can_begin(int app_id, int targ_core_id,
                           int reserved_ctx_id, int *targ_ctx_ret) const;
    void migrate_app_soon2(int app_id, int targ_core_id, int reserved_ctx_id,
                           i64 req_cyc, CtxHaltStyle halt_style);
    // Halt: Like migrate, but just stop (for scheduling)
    void halt_running_app(int app_id, CtxHaltStyle halt_style);
    bool halt_can_begin(int app_id) const;
    void halt_app_soon(int app_id, CtxHaltStyle halt_style);
    void halt_app_soon2(int app_id, CtxHaltStyle halt_style);
    bool is_halt_pending(const PerAppInfo& ainfo) const;
    bool is_swapout_pending(const PerAppInfo& ainfo) const;

public:
    PerAppInfo& get_ainfo(int app_id) { return mgr_info.get_appinfo(app_id); }
    const PerAppInfo& get_ainfo(int app_id) const {
        return mgr_info.get_appinfo(app_id);
    }
    PerCtxInfo& get_cinfo(int ctx_id) { return mgr_info.get_ctxinfo(ctx_id); }
    const PerCtxInfo& get_cinfo(int ctx_id) const {
        return mgr_info.get_ctxinfo(ctx_id);
    }
    PerCoreInfo& get_coreinfo(int core_id) { 
        return mgr_info.get_coreinfo(core_id); 
    }
    const PerCoreInfo& get_coreinfo(int core_id) const { 
        return mgr_info.get_coreinfo(core_id); 
    }

public:
    const MgrSchedInfo& get_mgrinfo() const { return mgr_info; }
    bool is_migrate_pending(int app_id) const;
    void migrate_app_soon(int app_id, int targ_core_id, int reserved_ctx_id,
                          i64 earliest_time,
                          bool cancel_on_app_move, i64 expire_cyc,
                          CtxHaltStyle halt_style,
                          CBQ_Callback *migrate_done_cb);
    void cancel_pending_migration(int app_id);
    bool is_waiting_offcore(const PerAppInfo& ainfo) const;

public:
    AppMgr(const AppMgrParams& params_);
    ~AppMgr();

    void register_idle_ctx(context *ctx) {
        sim_assert(!setup_done_flag);
        mgr_info.add_ctx(ctx);
        ctx_sched->ctx_idle(ctx->id);
    }
    
    void setup_done();
    void add_ready_app(AppState *app);
    void remove_app(AppState *app);

    void signal_longmiss(AppState *app, int dmiss_alist_id);
    void signal_missdone(AppState *app);
    void signal_idlectx(context *ctx);
    void prereset_hook(context *ctx);
    void signal_finalfill(context *ctx, bool commit_not_rename);
    void signal_finalspill(context *ctx, bool commit_not_rename);
    void signal_haltapp(AppState *app, CtxHaltStyle halt_style,
                        CBQ_Callback *halted_cb);

    void alter_mutablemap_sched(int app_id, int targ_core_or_neg1);

    void dump(void *FILE_out, const char *prefix) const {
//      FILE *out = static_cast<FILE *>(FILE_out);      
    }

    void printstats(void *FILE_out, const char *prefix) const;
};


AppMgr::AppMgr(const AppMgrParams& params_)
    : params(params_), app_sched(0), ctx_sched(0), swap_gate(0),
      setup_done_flag(false)
{
    string sched_app_name(simcfg_get_str("Hacking/sched_app"));
    string sched_ctx_name(simcfg_get_str("Hacking/sched_ctx"));
    string swap_name(simcfg_get_str("Hacking/swap"));

    if (sched_app_name == "OldestApp") {
        app_sched = new ASched_OldestApp(mgr_info); 
    } else {
        fprintf(stderr, "AppMgr (%s:%i): unrecognized app-scheduler name: "
                "%s\n", __FILE__, __LINE__, sched_app_name.c_str());
        exit(1);
    }

    if (sched_ctx_name == "FirstIdle") {
        ctx_sched = new CSched_FirstIdle(mgr_info); 
    } else if (sched_ctx_name == "LightestLoad") {
        ctx_sched = new CSched_LightestLoad(mgr_info); 
    } else if (sched_ctx_name == "LeastIpc") {
        ctx_sched = new CSched_LeastIpc(mgr_info); 
    } else if (sched_ctx_name == "Static") {
        ctx_sched = new CSched_Static(mgr_info); 
    } else if (sched_ctx_name == "StaticSetAffin") {
        ctx_sched = new CSched_StaticSetAffin(mgr_info); 
    } else if (sched_ctx_name == "MutableMap") {
        ctx_sched = new CSched_MutableMap(mgr_info); 
    } else {
        fprintf(stderr, "AppMgr (%s:%i): unrecognized ctx-scheduler name: "
                "%s\n", __FILE__, __LINE__, sched_ctx_name.c_str());
        exit(1);
    }

    if (swap_name == "IfProcFull") {
        swap_gate = new Swap_IfProcFull(mgr_info);
    } else if (swap_name == "IfCoreFull") {
        swap_gate = new Swap_IfCoreFull(mgr_info);
    } else if (swap_name == "IfNotSolo") {
        swap_gate = new Swap_IfNotSolo(mgr_info);
    } else if (swap_name == "Always") {
        swap_gate = new Swap_Always(mgr_info);
    } else if (swap_name == "Never") {
        swap_gate = new Swap_Never(mgr_info);
    } else if (swap_name == "IfCoreOversubscribed") {
        if (sched_ctx_name != "MutableMap") {
            fprintf(stderr, "AppMgr (%s:%i): swap-gate %s only supports "
                    "MutableMap scheduler\n", __FILE__, __LINE__,
                    swap_name.c_str());
            exit(1);
        }
        swap_gate = new Swap_IfCoreOversubscribed(
            mgr_info, dynamic_cast<CSched_MutableMap &>(*ctx_sched));
    } else {
        fprintf(stderr, "AppMgr (%s:%i): unrecognized swap-gate name: %s\n",
                __FILE__, __LINE__, swap_name.c_str());
        exit(1);
    }
    swap_suppress_guess = simcfg_get_bool("Hacking/swap_suppress_guess");

    inst_spill_fill = simcfg_get_bool("Hacking/inst_spill_fill");
    inst_spill_fill_early = simcfg_get_bool("Hacking/inst_spill_fill_early");
    regs_per_sf_block = 0;
    if (inst_spill_fill) {
        // cache_block_bytes is a power of 2, so this should be fine
        regs_per_sf_block = GlobalParams.mem.cache_block_bytes / 8;
        if (regs_per_sf_block <= 0)
            sim_abort();
    }

    min_swapin_commits = simcfg_get_i64("Hacking/min_swapin_commits");
    min_swapin_cyc = simcfg_get_i64("Hacking/min_swapin_cyc");
}


AppMgr::~AppMgr()
{ 
    if (app_sched) delete app_sched;
    if (ctx_sched) delete ctx_sched;
    if (swap_gate) delete swap_gate;
    // XXX lots of other objects (samplers/callbacks/etc.) are leaked here

    FOR_ITER(PendingMigrateMap, pending_migrates, iter) {
        delete iter->second;
    }
    pending_migrates.clear();
}


void
AppMgr::setup_done()
{
    // Reminder: the set of managed apps can still change (starts out empty)
    sim_assert(!setup_done_flag);
    setup_done_flag = true;
    mgr_info.setup_done();
}


void
AppMgr::add_ready_app(AppState *app)
{
    sim_assert(setup_done_flag);
    mgr_info.add_ready_app(app);
    app_sched->app_ready(app->app_id);
    sched_hook();
}


void
AppMgr::remove_app(AppState *app)
{
    DEBUGPRINTF("AppMgr removing app A%d\n", app->app_id);
    app_sched->app_notready(app->app_id);
    mgr_info.remove_app(app->app_id);
}


void
AppMgr::signal_longmiss(AppState *app, int dmiss_alist_id)
{
    const char *fname = "AppMgr::signal_longmiss";
    sim_assert(setup_done_flag);
    if (!mgr_info.get_app_ids().count(app->app_id)) {
        DEBUGPRINTF("%s: ignoring signal for unmanaged app A%d\n", fname,
                    app->app_id);
        return;
    }

    PerAppInfo& ainfo = get_ainfo(app->app_id);
    if (ainfo.g_state() == AI_Running) {
        bool swapping_out = false;
        PerCtxInfo& cinfo = get_cinfo(ainfo.g_ctx_id());
        PerCoreInfo& crinfo = get_coreinfo(cinfo.g_core_id());

        bool want_swapout = swap_gate->should_swap_out(ainfo, cinfo);
        bool swap_suppressed = swap_suppress_guess &&
            want_swapout && swap_suppress_guess_test(ainfo, cinfo);
        if (swap_suppressed)
            want_swapout = false;

        // Don't swap out an app until it's made at least some forward
        // progress (to prevent deadlock).
        if (enough_progress_since_swapin(ainfo) && want_swapout) {
            DEBUGPRINTF("appmgr: swapping A%d out from T%d\n", app->app_id,
                        cinfo.g_ctx()->id);
            if (cache_register_blocked_app(cinfo.g_ctx(), dmiss_alist_id)) {
                DEBUGPRINTF("appmgr: aborting swapout, couldn't register "
                            "blocked application with cache\n");
                ainfo.ignore_longmiss();
                crinfo.app_stalling_noevict();
            } else {
                swapping_out = true;
                ainfo.swapping_out(AI_SwapOut_LongMiss);
                cinfo.app_spill_begin();
                context_halt_signal(cinfo.g_ctx(), CtxHaltStyle_Fast);
            }
        } else {
            ainfo.ignore_longmiss();
            crinfo.app_stalling_noevict();
        }
    } else {
        ainfo.ignore_longmiss();
    }
    sched_hook();
}


void
AppMgr::signal_missdone(AppState *app)
{
    const char *fname = "AppMgr::signal_missdone";
    sim_assert(setup_done_flag);
    if (!mgr_info.get_app_ids().count(app->app_id)) {
        DEBUGPRINTF("%s: ignoring signal for unmanaged app A%d\n", fname,
                    app->app_id);
        return;
    }

    PerAppInfo& ainfo = get_ainfo(app->app_id);
    switch (ainfo.g_state()) {
    case AI_Running_LongMiss:
        ainfo.set_state(AI_Running);
        {
            PerCtxInfo& cinfo = get_cinfo(ainfo.g_ctx_id());
            PerCoreInfo& crinfo = get_coreinfo(cinfo.g_core_id());
            crinfo.app_stalldone_noevict();
        }
        break;
    case AI_Wait_LongMiss:
        ainfo.set_state(AI_Ready);
        app_sched->app_ready(app->app_id);
        break;
    case AI_SwapOut_LongMiss:
        ainfo.set_state(AI_SwapOut_LongMiss_Cancel);
        break;
    case AI_SwapOut_LongMiss_Cancel:
        break;
    default:
        break;
    }   

    sched_hook();
}



class AppMgr::CInfoCB : public CBQ_Callback {
    AppMgr& amgr; PerCtxInfo& cinfo; CInfoCBSel which;
public:
    CInfoCB(AppMgr& amgr_, PerCtxInfo& cinfo_, CInfoCBSel which_)
        : amgr(amgr_), cinfo(cinfo_), which(which_) { }
    i64 invoke(CBQ_Args *args) {
        switch (which) {
        case SwapOutGenSpill:
            amgr.swapout_genspill_callback(cinfo);
            break;
        case SwapInGenFill:
            amgr.swapin_genfill_callback(cinfo);
            break;
        case StaticSwapInDone:
            sim_assert(!amgr.inst_spill_fill);
            amgr.swapin_done_callback(cinfo);
            break;
        case StaticSwapOutDone:
            sim_assert(!amgr.inst_spill_fill);
            amgr.swapout_done_callback(cinfo, true, true);
            break;
        default:
            sim_abort();
        }
        return -1;
    }
};


AppMgr::CInfoCB *
AppMgr::gen_cinfo_cb(PerCtxInfo& cinfo, CInfoCBSel which) {
    return new CInfoCB(*this, cinfo, which);
}


void
AppMgr::start_app(int app_id, int ctx_id)
{
    static OpTime swap_in_time = { -1, -1 };
    if (swap_in_time.latency < 0) {
        swap_in_time.latency = simcfg_get_int("Hacking/thread_swapin_cyc");
        if (swap_in_time.latency < 0) {
            fprintf(stderr, "invalid thread_swapin_cyc: %d\n",
                    swap_in_time.latency);
            exit(1);
        }
        swap_in_time.interval = swap_in_time.latency;
    }

    DEBUGPRINTF("appmgr: swapping A%d in to T%d\n", app_id, ctx_id);
    PerCtxInfo& cinfo = get_cinfo(ctx_id);
    PerAppInfo& ainfo = get_ainfo(app_id);
    PerCoreInfo& crinfo = get_coreinfo(cinfo.g_core_id());
    cinfo.app_starting(app_id);
    ainfo.swapping_in(ctx_id,
                      (ainfo.g_prev_ctx() >= 0) &&
                      mgr_info.ctx_same_core(ctx_id,
                                             ainfo.g_prev_ctx()));
    crinfo.app_starting();

    if (inst_spill_fill) {
        cinfo.prepare_fill(ainfo.g_as());
        swapin_genfill_callback(cinfo);
    } else {
        i64 swapin_done_cyc;
        if (kMigrateFillsAreFree && ainfo.last_halt_was_for_migrate()) {
            swapin_done_cyc = cyc;
        } else {
            swapin_done_cyc = corebus_access(cinfo.g_ctx()->core->reply_bus,
                                             swap_in_time);
        }
        callbackq_enqueue(GlobalEventQueue, swapin_done_cyc - 1,
                          gen_cinfo_cb(cinfo, StaticSwapInDone));
    }
}


void
AppMgr::swapin_done_callback(PerCtxInfo& cinfo)
{
    sim_assert(cinfo.is_running());
    PerAppInfo& ainfo = get_ainfo(cinfo.g_curr_app());
    DEBUGPRINTF("appmgr: A%d swap-in to T%d completing%s\n", ainfo.g_id(),
                cinfo.g_ctx()->id,
                (inst_spill_fill && inst_spill_fill_early) ? " (early)" : "");
    ainfo.swap_in_done(*this);
    if (!inst_spill_fill)
        ainfo.swap_in_finalfill_commit();
    context_go(cinfo.g_ctx(), ainfo.g_as(), cyc + 1);
    cinfo.fill_extra_state();

    if (PendingMigrateInfo *pmi = map_at_default(pending_migrates,
                                                 ainfo.g_id(), NULL)) {
        if (pmi->migrate_in_progress) {
            // assume that the next swap-in after migration start, is the
            // completion of that migration
            if (pmi->migrate_done_cb) {
                callback_invoke(pmi->migrate_done_cb.get(), NULL);
                // callback destroyed by scoped_ptr<> delete in pmi destructor
            }
            pending_migrates.erase(ainfo.g_id());
            delete pmi;
        }
    }

    sched_hook();
}


void
AppMgr::swapout_done_callback(PerCtxInfo& cinfo,
                              bool context_now_avail,
                              bool final_spill_committed)
{
    sim_assert(context_now_avail || final_spill_committed);
    sim_assert(cinfo.spill_pending());
    PerAppInfo& ainfo = get_ainfo(cinfo.g_spilling_app());
    PerCoreInfo& crinfo = get_coreinfo(cinfo.g_core_id());
    DEBUGPRINTF("appmgr: A%d swap-out from T%d %s\n", ainfo.g_id(),
                cinfo.g_id(),
                (final_spill_committed) ? "committed" : "issued (early)");

    int migrate_target = -1;

    if (final_spill_committed) {
        AppInfoState next_state;
        switch (ainfo.g_state()) {
        case AI_SwapOut_LongMiss:
            next_state = AI_Wait_LongMiss;
            break;
        case AI_SwapOut_LongMiss_Cancel:
        case AI_SwapOut_Sched:
            next_state = AI_Ready;
            app_sched->app_ready(ainfo.g_id());
            break;
        case AI_SwapOut_Migrate:
            next_state = AI_Ready;
            migrate_target = ainfo.get_migrate_target();
            break;
        default:
            next_state = AI_Ready;
            sim_abort();
        }
        ainfo.swap_out_done(next_state);
        cinfo.app_spill_end();
    }

    if (context_now_avail) {
        cinfo.app_stopping();
        crinfo.app_stopping(ainfo.g_id());
        if (cinfo.is_free())
            ctx_sched->ctx_idle(cinfo.g_id());
    }

    if (migrate_target >= 0) {
        // Immediately start swapping the app in to the target context
        start_app(ainfo.g_id(), migrate_target);
    }

    if (final_spill_committed && (ainfo.g_state() == AI_Ready)) {
        CallbackSeq *cb_seq = ainfo.bundle_posthalt_callbacks();
        // Warning: the callbacks may do all sorts of wacky things, including
        // removing the app (and hence invalidating "ainfo")
        if (cb_seq) {
            callback_invoke(cb_seq, NULL);
            delete cb_seq;
        }
    }
    sched_hook();
}


void
AppMgr::swapin_genfill_callback(PerCtxInfo& cinfo)
{
    int app_id = cinfo.g_curr_app();
    PerAppInfo& ainfo = get_ainfo(app_id);
    // Generate the next set of fill instructions and buffer them for
    // injection.  If we can't get enough alist entries this time, use as many
    // as we can get, and create a callback in the future to generate more.
    int next_reg;
    while ((next_reg = cinfo.next_fill_reg()) >= 0) {
        activelist *inst = inject_alloc(cinfo.g_ctx());
        if (!inst) {
            // Try again later
            callbackq_enqueue(GlobalEventQueue, cyc + 1, 
                              gen_cinfo_cb(cinfo, SwapInGenFill));
            break;
        }
        int fill_idx = cinfo.fills_so_far();
        cinfo.consume_fill_reg();
        bool is_final = cinfo.next_fill_reg() < 0;
        inject_set_bmtfill(inst, next_reg, is_final,
                           (fill_idx % regs_per_sf_block) == 0);
        if (kMigrateFillsAreFree && 
            ainfo.last_halt_was_for_migrate()) {
            inst->bmt.spillfill |= BmtSF_FreeTransfer;
        }
        inject_at_rename(cinfo.g_ctx(), inst);
    }
}


void
AppMgr::swapout_genspill_callback(PerCtxInfo& cinfo)
{
    // Generate the next set of spill instructions and buffer them for
    // injection.  If we can't get enough alist entries this time, use as many
    // as we can get, and create a callback in the future to generate more.
    int next_reg;
    while ((next_reg = cinfo.next_spill_reg()) >= 0) {
        activelist *inst = inject_alloc(cinfo.g_ctx());
        if (!inst) {
            // Try again later
            callbackq_enqueue(GlobalEventQueue, cyc + 1,
                              gen_cinfo_cb(cinfo, SwapOutGenSpill));
            break;
        }
        cinfo.consume_spill_reg();
        bool is_final = cinfo.next_spill_reg() < 0;
        int spill_count = cinfo.spills_so_far();
        bool is_block_end = (spill_count % regs_per_sf_block) == 0;
        inject_set_bmtspill(inst, next_reg, is_final,
                            is_block_end || is_final);
        inject_at_rename(cinfo.g_ctx(), inst);
    }
}


void 
AppMgr::signal_idlectx(context *ctx)
{
    static OpTime swap_out_time = { -1, -1 };
    if (swap_out_time.latency < 0) {
        swap_out_time.latency = simcfg_get_int("Hacking/thread_swapout_cyc");
        if (swap_out_time.latency < 0) {
            fprintf(stderr, "invalid thread_swapout_cyc: %d\n",
                    swap_out_time.latency);
            exit(1);
        }
        swap_out_time.interval = swap_out_time.latency;
    }

    sim_assert(setup_done_flag);
    PerCtxInfo& cinfo = get_cinfo(ctx->id);
    if (cinfo.is_running()) {
        // Context just went idle
        if (cinfo.spill_pending()) {
            PerAppInfo& ainfo = get_ainfo(cinfo.g_spilling_app());
            ainfo.swap_out_ctx_halted();
            if (inst_spill_fill) {
                swapout_genspill_callback(cinfo);
            } else {
                i64 swapout_done_cyc =
                    corebus_access(cinfo.g_ctx()->core->reply_bus,
                                   swap_out_time);
                callbackq_enqueue(GlobalEventQueue, swapout_done_cyc, 
                                  gen_cinfo_cb(cinfo, StaticSwapOutDone));
            }
        } else {
            // This shouldn't happen?  Context went idle without us 
            // telling it to.  We can tolerate this, though.
//          PerCoreInfo& crinfo = get_coreinfo(cinfo.g_core_id());
//          cinfo.curr_app_id = -1;
//          sched->ctx_idle(ctx->id);
//          crinfo.app_stopping();
            fprintf(stderr, "hmm, scheduled context went idle without a "
                    "swapout pending\n");
            fflush(0);
            sim_abort();
        }
    }
    sched_hook();
}


void 
AppMgr::prereset_hook(context *ctx)
{
    if (ctx->as) {
        PerCtxInfo& cinfo = get_cinfo(ctx->id);
        cinfo.prepare_spill();
        cinfo.spill_extra_state();
    } else {
        fprintf(stderr, "hmm, unscheduled context %d being reset\n", ctx->id);
        fflush(0);
        sim_abort();
    }
    sched_hook();
}


void 
AppMgr::signal_finalfill(context *ctx, bool commit_not_rename)
{
    sim_assert(setup_done_flag);
    PerCtxInfo& cinfo = get_cinfo(ctx->id);
    if (commit_not_rename != inst_spill_fill_early) {
        // Signal at (commit and not-early) or (not-commit and early)
        swapin_done_callback(cinfo);
    }
    if (commit_not_rename) {
        int app_id = cinfo.g_curr_app();
        PerAppInfo& ainfo = get_ainfo(app_id);
        ainfo.swap_in_finalfill_commit();
    }
}


void 
AppMgr::signal_finalspill(context *ctx, bool commit_not_rename)
{
    sim_assert(setup_done_flag);
    PerCtxInfo& cinfo = get_cinfo(ctx->id);
    if (commit_not_rename || inst_spill_fill_early) {
        // Free the context if either (early && !commit) or (!early && commit)
        int free_ctx = inst_spill_fill_early != commit_not_rename;
        swapout_done_callback(cinfo, free_ctx, commit_not_rename);
    }
}


void
AppMgr::signal_haltapp(AppState *app, CtxHaltStyle halt_style,
                       CBQ_Callback *halted_cb)
{
    sim_assert(setup_done_flag);
    PerAppInfo& ainfo = get_ainfo(app->app_id);
    DEBUGPRINTF("AppMgr::signal_haltapp: signaling A%d (state %s) to halt, "
                "post-halt callback %p, time %s\n", app->app_id, 
                app_state_name(ainfo.g_state()),
                static_cast<void *>(halted_cb), fmt_now());
    // callback may be NULL
    if (halted_cb) {
        ainfo.register_posthalt_callback(halted_cb);
    }
    if (!is_halt_pending(ainfo)) {
        halt_app_soon(app->app_id, halt_style);
    }
}


void
AppMgr::alter_mutablemap_sched(int app_id, int targ_core_or_neg1)
{
    const char *fname = "AppMgr::alter_mutablemap_sched";
    DEBUGPRINTF("%s: setting A%d sched to C%d\n", fname,
                app_id, targ_core_or_neg1);
    CSched_MutableMap *csched_cast =
        dynamic_cast<CSched_MutableMap *>(ctx_sched);
    if (!csched_cast) {
        const char *type_name = simcfg_get_str("Hacking/sched_ctx");
        abort_printf("%s: cannot cast context scheduler (%s) "
                     "to CSched_MutableMap; don't know how to apply!\n",
                     fname, type_name);
    }
    csched_cast->sched_remove_app(app_id);      // ignores non-sched apps
    csched_cast->sched_add_app(app_id, targ_core_or_neg1);
}


void
AppMgr::schedule_apps()
{
    sim_assert(setup_done_flag);
    vector<int> retry_apps;
    while (app_sched->will_schedule() &&
           ctx_sched->will_schedule()) {
        const int app_id = app_sched->schedule_one();
        int ctx_id = -1;
        if (ctx_id < 0) {
            ctx_id = ctx_sched->schedule_one(app_id);
        }
        sim_assert(app_id >= 0);
        if (ctx_id >= 0) {
            start_app(app_id, ctx_id);
        } else {
            retry_apps.push_back(app_id);
        }
    }
    for (size_t i = 0; i < retry_apps.size(); i++)
        app_sched->undo_schedule(retry_apps[i]);
}


bool
AppMgr::any_progress_since_swapin(const PerAppInfo& ainfo) const
{
    return ainfo.commits_since_swapin() > 0;
}


bool
AppMgr::enough_progress_since_swapin(const PerAppInfo& ainfo) const
{
    return (ainfo.commits_since_swapin() >= min_swapin_commits) &&
        (ainfo.cyc_since_swapin() >= min_swapin_cyc);
}


bool
AppMgr::swap_suppress_guess_test(const PerAppInfo& ainfo,
                                   const PerCtxInfo& cinfo) const
{
    // We're about to swap app ainfo out from context cinfo.  We'll test to
    // see where we'd schedule ainfo if it was ready right now, and if it
    // looks like we'd send it back to the same core as it's at now, then
    // choose not to swap out out unless override conditions are met (full
    // processor, waiting ready apps, etc.)
    int next_core_guess = ctx_sched->schedule_guess_core(ainfo.g_id());
    int curr_core = cinfo.g_core_id();
    bool same_core = (next_core_guess >= 0) && (next_core_guess == curr_core);
    bool override = mgr_info.total_free_ctxs() <
        mgr_info.total_notsched_apps();
    bool result = same_core && !override;
    DEBUGPRINTF("swap_suppress_guess_test: A%d T%d next_core "
                "C%d curr_core C%d "
                "same_core %d override %d => result %d\n", ainfo.g_id(),
                cinfo.g_id(), next_core_guess, curr_core, same_core,
                override, result);
    return result;
}


// See also: halt_running_app()
void
AppMgr::migrate_running_app(int app_id, int target_ctx_id,
                            CtxHaltStyle halt_style)
{
    const char *fname = "migrate_running_app";
    PerAppInfo& ainfo = get_ainfo(app_id);
    sim_assert(ainfo.is_sched());
    PerCtxInfo& cinfo = get_cinfo(ainfo.g_ctx_id());
    int src_core_id = cinfo.g_core_id();
    PerCtxInfo& targ_cinfo = get_cinfo(target_ctx_id);
    int targ_core_id = targ_cinfo.g_core_id();
    DEBUGPRINTF("appmgr: migrating A%d from T%d (C%d) to T%d (C%d)\n",
                app_id, cinfo.g_id(), src_core_id, target_ctx_id, 
                targ_core_id);
    if (src_core_id == targ_core_id) {
        abort_printf("%s: migrating A%d from T%d to T%d: both on "
                     "core C%d; shouldn't happen!\n", fname,
                     app_id, cinfo.g_id(), target_ctx_id, src_core_id);
    }
    if (ainfo.g_state() == AI_Running_LongMiss) {
        PerCoreInfo& curr_crinfo = get_coreinfo(cinfo.g_core_id());
        // Derived from signal_missdone(), for accounting purposes
        ainfo.set_state(AI_Running);
        curr_crinfo.app_stalldone_noevict();
    }
    ainfo.migrating(target_ctx_id);
    targ_cinfo.reserve_for_app(app_id);
    cinfo.app_spill_begin();
    context_halt_signal(cinfo.g_ctx(), halt_style);
}


class AppMgr::MigrateCancelCB : public CBQ_Callback {
    AppMgr& amgr; 
    int app_id;
public:
    MigrateCancelCB(AppMgr& amgr_, int app_id_)
        : amgr(amgr_), app_id(app_id_) { }
    i64 invoke(CBQ_Args *args) {
        amgr.cancel_pending_migration(app_id);
        return -1;
    }
};


class AppMgr::MigrateRecheckCB : public CBQ_Callback {
    AppMgr& amgr; 
    PendingMigrateInfo& pmi;
    int prev_ctx_id;
    int app_id; int targ_core_id; int reserved_ctx_id;
    bool cancel_on_app_move;
    i64 orig_request_cyc;
    CtxHaltStyle halt_style;
    bool app_moved() const {
        const PerAppInfo& ainfo = amgr.get_ainfo(app_id);
        int curr_ctx_id = (ainfo.is_sched()) ? ainfo.g_ctx_id() : -1;
        return (prev_ctx_id >= 0) && (curr_ctx_id != prev_ctx_id);
        // (should we also test "curr_ctx_id >= 0" ?)
    }
public:
    MigrateRecheckCB(AppMgr& amgr_, PendingMigrateInfo& pmi_, int prev_ctx_id_,
                     int app_id_, int targ_core_id_,
                     int reserved_ctx_id_,
                     bool cancel_on_app_move_, CtxHaltStyle halt_style_)
        : amgr(amgr_), pmi(pmi_), prev_ctx_id(prev_ctx_id_),
          app_id(app_id_), targ_core_id(targ_core_id_),
          reserved_ctx_id(reserved_ctx_id_),
          cancel_on_app_move(cancel_on_app_move_),
          orig_request_cyc(cyc), halt_style(halt_style_) { }
    i64 invoke(CBQ_Args *args) {
        i64 resched_cyc;
        sim_assert(amgr.pending_migrates.count(app_id));
        if (cancel_on_app_move && app_moved()) {
            const PerAppInfo& ainfo = amgr.get_ainfo(app_id);
            DEBUGPRINTF("canceling pending migration of A%d to C%d/T%d: "
                        "app moved T%d -> %d\n", app_id, targ_core_id,
                        reserved_ctx_id, prev_ctx_id,
                        (ainfo.is_sched()) ? ainfo.g_ctx_id() : -1);
            // Can't just call cancel: it will end up deleting this object,
            // and then returning here (ouch).
            callbackq_enqueue(GlobalEventQueue, 0,
                              new AppMgr::MigrateCancelCB(amgr, app_id));
            resched_cyc = -1;           // delete callback at return
        } else if (amgr.migrate_can_begin(app_id, targ_core_id,
                                          reserved_ctx_id, NULL)) {
            amgr.migrate_app_soon2(app_id, targ_core_id, reserved_ctx_id,
                                   orig_request_cyc, halt_style);
            resched_cyc = -1;           // delete callback at return
        } else {
            resched_cyc = cyc + 1;      // reschedule callback later
        }
        if (resched_cyc < 0) {
            // The callback itself will be deleted by the parent CallbackQueue
            // when we return.  The rest of the PendingMigrateInfo will be
            // handled via MigrateCancelCB -> cancel_pending_migration(), so
            // we'll unlink this callback first so it's not ref'd after delete.
            pmi.recheck_callback.reset();
        }
        return resched_cyc;
    }
};


class AppMgr::MigrateTimeoutCB : public CBQ_Callback {
    AppMgr& amgr; 
    int app_id;
    CBQ_Callback *callback_match;
public:
    MigrateTimeoutCB(AppMgr& amgr_, int app_id_, CBQ_Callback *callback_match_)
        : amgr(amgr_), app_id(app_id_),
          callback_match(callback_match_) { }
    i64 invoke(CBQ_Args *args) {
        // If the callback ID mismatches, a newer migrate is pending which
        // doesn't correspond to this timeout, so ignore it
        if (amgr.is_migrate_pending(app_id)) {
            const PendingMigrateInfo& pmi = 
                *map_at(amgr.pending_migrates, app_id);
            if (pmi.recheck_callback.get() == callback_match) {
                DEBUGPRINTF("MigrateTimeoutCB: cancelling A%d migration, "
                            "cyc %s (migrate callback %p)\n", app_id,
                            fmt_i64(cyc), (void *) callback_match);
                amgr.cancel_pending_migration(app_id);
            }
        }
        return -1;
    }
};


bool
AppMgr::migrate_can_begin(int app_id, int targ_core_id,
                          int reserved_ctx_id, int *targ_ctx_ret) const
{
    const PerAppInfo& ainfo = mgr_info.get_appinfo(app_id);
    const AppInfoState astate = ainfo.g_state();
    int targ_ctx_id = reserved_ctx_id;
    if (targ_ctx_id < 0) {
        const PerCoreInfo& targ_crinfo = get_coreinfo(targ_core_id);
        targ_ctx_id = mgr_info.core_idle_ctx(targ_crinfo);
    }
    bool can_begin = false;
    if (targ_ctx_id >= 0) {
        switch (astate) {
        case AI_Running:
        case AI_Running_LongMiss:
            can_begin = any_progress_since_swapin(ainfo);
            break;
        case AI_Ready:
            can_begin = true;
            break;
        default:
            break;
        }
    }
    if (!can_begin) {
        DEBUGPRINTF("migrate_can_begin: A%d not migratable: targ C%d/T%d "
                    "state %s cyc %s\n",
                    app_id, targ_core_id, targ_ctx_id,
                    app_state_name(ainfo.g_state()), fmt_i64(cyc));
    }
    if (can_begin && targ_ctx_ret)
        *targ_ctx_ret = targ_ctx_id;
    return can_begin;
}


// "private" function called by migrate_app_soon() / MigrateRecheckCB,
// when migration can actually begin.
void
AppMgr::migrate_app_soon2(int app_id, int targ_core_id, int reserved_ctx_id,
                          i64 req_cyc, CtxHaltStyle halt_style)
{
    sim_assert(pending_migrates.count(app_id));
    PendingMigrateInfo& pmi = *map_at(pending_migrates, app_id);
    sim_assert(!pmi.migrate_in_progress);
    pmi.migrate_in_progress = true;
    const PerAppInfo& ainfo = mgr_info.get_appinfo(app_id);
    int targ_ctx_id = reserved_ctx_id;
    if (targ_ctx_id < 0) {
        const PerCoreInfo& targ_crinfo = get_coreinfo(targ_core_id);
        targ_ctx_id = mgr_info.core_idle_ctx(targ_crinfo);
    }
    sim_assert(targ_ctx_id >= 0);
    // Inform CtxSched that the target context is no longer available
    ctx_sched->ctx_notidle(targ_ctx_id);
    if (ainfo.g_state() == AI_Ready) {
        // Remove the app from the set considered for scheduling by AppSched.
        // (This isn't needed for migrate_running_app() since it causes
        // the target app to go directly from swapout to swapin, so it doesn't
        // get seen by the AppSched.)
        app_sched->app_notready(app_id);
        start_app(app_id, targ_ctx_id);
    } else {
        migrate_running_app(app_id, targ_ctx_id, halt_style);
    }
    // PendingMigrateInfo will live on until migration is complete,
    // and swapin_done_callback() restarts execution
}


// Migrate the given app to the target core, as soon as possible.
// If "reserved_ctx_id" >= 0, the given target context must already be
// reserved for this app.
void
AppMgr::migrate_app_soon(int app_id, int targ_core_id, int reserved_ctx_id,
                         i64 earliest_time,
                         bool cancel_on_app_move, i64 expire_cyc,
                         CtxHaltStyle halt_style,
                         CBQ_Callback *migrate_done_cb)
{
    const char *fname = "migrate_app_soon";
    const PerAppInfo& ainfo = mgr_info.get_appinfo(app_id);
    sim_assert(!pending_migrates.count(app_id));
    PendingMigrateInfo& pmi =
        *map_put_uniq(pending_migrates, app_id, 
                      new PendingMigrateInfo(app_id, targ_core_id,
                                             reserved_ctx_id,
                                             migrate_done_cb));
    int targ_ctx_id = -1;
    if ((cyc >= earliest_time) &&
        migrate_can_begin(app_id, targ_core_id, reserved_ctx_id,
                          &targ_ctx_id)) {
        migrate_app_soon2(app_id, targ_core_id, reserved_ctx_id,
                          cyc, halt_style);
    } else {
        const int curr_ctx_id = (ainfo.is_sched()) ? ainfo.g_ctx_id() : -1;
        i64 callback_time = MAX_SCALAR(cyc + 1, earliest_time);
        MigrateRecheckCB *callback =
            new MigrateRecheckCB(*this, pmi, curr_ctx_id,
                                 app_id, targ_core_id, reserved_ctx_id,
                                 cancel_on_app_move,
                                 halt_style);
        callbackq_enqueue(GlobalEventQueue, callback_time, callback);
        DEBUGPRINTF("%s: A%d not migratable; earliest %s, "
                    "retest callback %p at %s (expire at %s).\n",
                    fname, app_id, fmt_i64(earliest_time), (void *) callback,
                    fmt_i64(callback_time), fmt_i64(expire_cyc));
        pmi.recheck_callback.reset(callback);
        if (expire_cyc >= cyc) {
            callbackq_enqueue(GlobalEventQueue, expire_cyc,
                              new MigrateTimeoutCB(*this, app_id,
                                                   callback));
        }
    }
}


bool
AppMgr::is_migrate_pending(int app_id) const
{
    return pending_migrates.count(app_id) != 0;
}


void 
AppMgr::cancel_pending_migration(int app_id)
{
    const char *fname = "AppMgr::cancel_pending_migration";
    PendingMigrateMap::iterator found = pending_migrates.find(app_id);
    if (found == pending_migrates.end()) {
        abort_printf("%s: nothing pending for A%d\n", fname, app_id);
    }
    PendingMigrateInfo *pmi = found->second;
    DEBUGPRINTF("%s: canceling migrate of A%d to C%d/T%d\n", fname,
                app_id, pmi->targ_core_id, pmi->reserved_ctx_id);
    if (pmi->reserved_ctx_id >= 0) {
        PerCtxInfo& reserved_cinfo = get_cinfo(pmi->reserved_ctx_id);
        reserved_cinfo.cancel_reservation(app_id);
    }
    pending_migrates.erase(found);
    // pmi's destructor will de-schedule recheck_callback
    // scoped_ptr<> will delete up recheck_callback and migrate_done_cb
    delete pmi;
}


// Halt an app for scheduling purposes.
// See also: migrate_running_app()
void
AppMgr::halt_running_app(int app_id, CtxHaltStyle halt_style)
{
    const char *fname = "AppMgr::halt_running_app";
    PerAppInfo& ainfo = get_ainfo(app_id);
    sim_assert(ainfo.is_sched());
    PerCtxInfo& cinfo = get_cinfo(ainfo.g_ctx_id());
    int core_id = cinfo.g_core_id();
    PerCoreInfo& crinfo = get_coreinfo(cinfo.g_core_id());
    DEBUGPRINTF("%s: halting A%d on T%d (C%d)\n", fname,
                app_id, cinfo.g_id(), core_id);
    if (ainfo.g_state() == AI_Running_LongMiss) {
        // Derived from signal_missdone(), for accounting purposes
        ainfo.set_state(AI_Running);
        crinfo.app_stalldone_noevict();
    }
    ainfo.swapping_out(AI_SwapOut_Sched);
    cinfo.app_spill_begin();
    context_halt_signal(cinfo.g_ctx(), halt_style);
}


class AppMgr::HaltRecheckCB : public CBQ_Callback {
    AppMgr& amgr; 
    int app_id;
    CtxHaltStyle halt_style;
public:
    HaltRecheckCB(AppMgr& amgr_, int app_id_, CtxHaltStyle halt_style_)
        : amgr(amgr_), app_id(app_id_), halt_style(halt_style_) { }
    i64 invoke(CBQ_Args *args) {
        i64 resched_cyc = -1;
        sim_assert(amgr.pending_halts.count(app_id));
        if (amgr.halt_can_begin(app_id)) {
            amgr.halt_app_soon2(app_id, halt_style);
        } else {
            resched_cyc = cyc + 1;
        }
        return resched_cyc;
    }
};


bool
AppMgr::halt_can_begin(int app_id) const
{
    const char *fname = "AppMgr::halt_can_begin";
    const PerAppInfo& ainfo = get_ainfo(app_id);
    const AppInfoState astate = ainfo.g_state();
    bool can_begin = false;
    switch (astate) {
    case AI_Running:
    case AI_Running_LongMiss:
        can_begin = any_progress_since_swapin(ainfo);
        break;
    case AI_Ready:      // Already halted
        can_begin = true;
        break;
    default:
        break;
    }   
    if (!can_begin) {
        DEBUGPRINTF("%s: A%d not haltable: state %s cyc %s\n", fname, app_id,
                    app_state_name(astate), fmt_i64(cyc));
    }
    return can_begin;
}


// Stop the given app ASAP, for scheduling reasons
// (Note: concurrent halt requests aren't allowed; one possible way around
// this is to register a post-halt callback with register_posthalt_callback(),
// and check is_halt_pending().)
void
AppMgr::halt_app_soon(int app_id, CtxHaltStyle halt_style)
{
    const char *fname = "AppMgr::halt_app_soon";
    sim_assert(!pending_halts.count(app_id));
    pending_halts.insert(app_id);
    if (halt_can_begin(app_id)) {
        halt_app_soon2(app_id, halt_style);
    } else {
        i64 callback_time = cyc + 1;
        HaltRecheckCB *callback =
            new HaltRecheckCB(*this, app_id, halt_style);
        callbackq_enqueue(GlobalEventQueue, callback_time, callback);
        DEBUGPRINTF("%s: A%d not haltable; retest callback %p at %s\n",
                    fname, app_id, (void *) callback, fmt_i64(callback_time));
    }
}


// "Private" function which actually initiates the halt
void
AppMgr::halt_app_soon2(int app_id, CtxHaltStyle halt_style)
{
    PerAppInfo& ainfo = get_ainfo(app_id);
    sim_assert(pending_halts.count(app_id));
    if (ainfo.g_state() == AI_Ready) {
        // Already halted
        CallbackSeq *cb_seq = ainfo.bundle_posthalt_callbacks();
        // Warning: the callbacks may do all sorts of wacky things, including
        // removing the app (and hence invalidating "ainfo")
        if (cb_seq) {
            callback_invoke(cb_seq, NULL);
            delete cb_seq;
        }
    } else {
        halt_running_app(app_id, halt_style);
    }
    pending_halts.erase(app_id);
}


bool
AppMgr::is_halt_pending(const PerAppInfo& ainfo) const
{
    return pending_halts.count(ainfo.g_id()) != 0;
}


bool
AppMgr::is_swapout_pending(const PerAppInfo& ainfo) const
{
    AppInfoState ai_state = ainfo.g_state();
    bool result;
    switch (ai_state) {
    case AI_SwapOut_LongMiss:   
    case AI_SwapOut_LongMiss_Cancel:    
    case AI_SwapOut_Migrate:            
    case AI_SwapOut_Sched:              
        result = true;
        break;
    default:
        result = false;
    }
    return result;
}


bool
AppMgr::is_waiting_offcore(const PerAppInfo& ainfo) const
{
    AppInfoState ai_state = ainfo.g_state();
    bool result;
    switch (ai_state) {
    case AI_Wait_LongMiss:
        result = true;
        break;
    default:
        result = false;
    }
    return result;
}


void
AppMgr::printstats(void *FILE_out, const char *pf) const
{
    FILE *out = static_cast<FILE *>(FILE_out);  
    const IdSet& apps = mgr_info.get_app_ids(),
        ctxs = mgr_info.get_ctx_ids(),
        cores = mgr_info.get_core_ids();

    {
        const char *columns[] = {
            "ctx_run", "ctx_stalled", "ready", "swapping", "waiting"
        };
        fprintf(out, "%sApp state/cycle breakdown:\n", pf);
        fprintf(out, "%s  \"cyc\"", pf);
        for (int i = 0; i < (int) NELEM(columns); i++)
            fprintf(out, ",\"%s\"", columns[i]);
        fprintf(out, "\n");
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            fprintf(out, "%s  \"A%d\"", pf, *i_app);
            const PerAppInfo& ainfo = mgr_info.get_appinfo(*i_app);
            HistCount_Int hist;
            ainfo.get_state_hist(hist);
            for (int col = 0; col < (int) NELEM(columns); col++) {
                i64 val;
                switch (col) {
                case 0: val = hist.get_count(AI_Running); break;
                case 1: val = hist.get_count(AI_Running_LongMiss); break;
                case 2: val = hist.get_count(AI_Ready); break;
                case 3:
                    val = hist.get_count(AI_SwapIn) +
                        hist.get_count(AI_SwapOut_LongMiss) + 
                        hist.get_count(AI_SwapOut_LongMiss_Cancel) +
                        hist.get_count(AI_SwapOut_Migrate) +
                        hist.get_count(AI_SwapOut_Sched);
                    break;
                case 4: val = hist.get_count(AI_Wait_LongMiss); break;
                default: val = -1; sim_abort();
                }
                fprintf(out, ",%s", fmt_i64(val));
            }
            fprintf(out, "\n");
        }
    }

    {
        fprintf(out, "%sApp occupancy cycles on cores:\n", pf);
        fprintf(out, "%s  \"cyc\"", pf);
        for (IdSet::const_iterator i_core = cores.begin();
             i_core != cores.end(); ++i_core) {
            fprintf(out, ",\"C%d\"", *i_core);
        }
        fprintf(out, "\n");
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            fprintf(out, "%s  \"A%d\"", pf, *i_app);
            for (IdSet::const_iterator i_core = cores.begin();
                 i_core != cores.end(); ++i_core) {
                i64 val = mgr_info.core_resident_cyc(*i_core, *i_app);
                fprintf(out, ",%s", fmt_i64(val));
            }
            fprintf(out, "\n");
        }
    }

    {
        fprintf(out, "%sApp swap-ins on cores:\n", pf);
        fprintf(out, "%s  \"events\"", pf);
        for (IdSet::const_iterator i_core = cores.begin();
             i_core != cores.end(); ++i_core) {
            fprintf(out, ",\"C%d\"", *i_core);
        }
        fprintf(out, "\n");
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            fprintf(out, "%s  \"A%d\"", pf, *i_app);
            for (IdSet::const_iterator i_core = cores.begin();
                 i_core != cores.end(); ++i_core) {
                i64 val = mgr_info.core_swapin_count(*i_core, *i_app);
                fprintf(out, ",%s", fmt_i64(val));
            }
            fprintf(out, "\n");
        }
    }

    {
        int max_tlp = mgr_info.biggest_core_contexts();
        fprintf(out, "%sCore \"scheduled - stalled\" TLP cycles:\n", pf);
        fprintf(out, "%s  \"cyc\"", pf);
        for (int tlp = 0; tlp <= max_tlp; tlp++)
            fprintf(out, ",%d", tlp);
        fprintf(out, "\n");
        for (IdSet::const_iterator i_core = cores.begin();
             i_core != cores.end(); ++i_core) {
            const PerCoreInfo& crinfo = mgr_info.get_coreinfo(*i_core);
            HistCount_Int tlp_hist;
            crinfo.get_tlp_hist(tlp_hist, true);
            fprintf(out, "%s  \"C%d\"", pf, *i_core);
            for (int tlp = 0; tlp <= max_tlp; tlp++)
                fprintf(out, ",%s", fmt_i64(tlp_hist.get_count(tlp)));
            fprintf(out, "\n");
        }
    }

    {
        int max_tlp = mgr_info.biggest_core_contexts();
        fprintf(out, "%sCore scheduled TLP cycles:\n", pf);
        fprintf(out, "%s  \"cyc\"", pf);
        for (int tlp = 0; tlp <= max_tlp; tlp++)
            fprintf(out, ",%d", tlp);
        fprintf(out, "\n");
        for (IdSet::const_iterator i_core = cores.begin();
             i_core != cores.end(); ++i_core) {
            const PerCoreInfo& crinfo = mgr_info.get_coreinfo(*i_core);
            HistCount_Int tlp_hist;
            crinfo.get_tlp_hist(tlp_hist, false);
            fprintf(out, "%s  \"C%d\"", pf, *i_core);
            for (int tlp = 0; tlp <= max_tlp; tlp++)
                fprintf(out, ",%s", fmt_i64(tlp_hist.get_count(tlp)));
            fprintf(out, "\n");
        }
    }

    {
        fprintf(out, "%sApp same-core fraction: [", pf);
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            const PerAppInfo& ainfo = mgr_info.get_appinfo(*i_app);
            fprintf(out, " %.3f", ainfo.swapin_repeat_frac());
        }
        fprintf(out, " ]\n");
    }

    {
        fprintf(out, "%sApp mean swapped-in cyc: [", pf);
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            const PerAppInfo& ainfo = mgr_info.get_appinfo(*i_app);
            fprintf(out, " %.0f", ainfo.mean_swappedin_cyc());
        }
        fprintf(out, " ]\n");
    }

    {
        fprintf(out, "%sApp scheduled IPC: [", pf);
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            const PerAppInfo& ainfo = mgr_info.get_appinfo(*i_app);
            fprintf(out, " %.3f", ainfo.resident_ipc_commit());
        }
        fprintf(out, " ]\n");
    }

    {
        fprintf(out, "%sApp L1D blocks on cores:\n", pf);
        fprintf(out, "%s  \"blocks\"", pf);
        for (IdSet::const_iterator i_core = cores.begin();
             i_core != cores.end(); ++i_core) {
            fprintf(out, ",\"C%d\"", *i_core);
        }
        fprintf(out, "\n");
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            fprintf(out, "%s  \"A%d\"", pf, *i_app);
            for (IdSet::const_iterator i_core = cores.begin();
                 i_core != cores.end(); ++i_core) {
                const CacheArray *dcache =
                    mgr_info.get_coreinfo(*i_core).g_core()->dcache;
                int count = cache_get_population(dcache, *i_app);
                fprintf(out, ",%d", count);
            }
            fprintf(out, "\n");
        }
    }

    if (SharedL2Cache) {
        fprintf(out, "%sApp L2 blocks: [", pf);
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            CacheArray *l2cache = SharedL2Cache;
            int count = cache_get_population(l2cache, *i_app);
            fprintf(out, " %d", count);
        }
        fprintf(out, " ]\n");
    } else {
        fprintf(out, "%sApp L2 blocks on cores:\n", pf);
        fprintf(out, "%s  \"blocks\"", pf);
        for (IdSet::const_iterator i_core = cores.begin();
             i_core != cores.end(); ++i_core) {
            fprintf(out, ",\"C%d\"", *i_core);
        }
        fprintf(out, "\n");
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            fprintf(out, "%s  \"A%d\"", pf, *i_app);
            for (IdSet::const_iterator i_core = cores.begin();
                 i_core != cores.end(); ++i_core) {
                const CacheArray *l2cache =
                    mgr_info.get_coreinfo(*i_core).g_core()->l2cache;
                int count = cache_get_population(l2cache, *i_app);
                fprintf(out, ",%d", count);
            }
            fprintf(out, "\n");
        }
    }

    if (GlobalParams.mem.use_l3cache) {
        fprintf(out, "%sApp L3 blocks: [", pf);
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            CacheArray *l3cache = SharedL3Cache;
            int count = cache_get_population(l3cache, *i_app);
            fprintf(out, " %d", count);
        }
        fprintf(out, " ]\n");
    }

    {
        string child_pf = string(pf) + "    ";
        fprintf(out, "%sApp activate/deactivate timing stats:\n", pf);
        for (IdSet::const_iterator i_app = apps.begin();
             i_app != apps.end(); ++i_app) {
            fprintf(out, "%s  A%d\n", pf, *i_app);
            const PerAppInfo& ainfo = get_ainfo(*i_app);
            ainfo.report_migrate_timing(out, child_pf.c_str());
        }
    }
}



//
// C interface
//

AppMgr *
appmgr_create(const AppMgrParams *params)
{
    return new AppMgr(*params);
}

void
appmgr_destroy(AppMgr *amgr)
{
    if (amgr)
        delete amgr;
}

void 
appmgr_register_idle_ctx(AppMgr *amgr, struct context *ctx)
{
    amgr->register_idle_ctx(ctx);
}

void
appmgr_setup_done(AppMgr *amgr)
{
    amgr->setup_done();
}

void
appmgr_add_ready_app(AppMgr *amgr, struct AppState *app)
{
    amgr->add_ready_app(app);
}

void
appmgr_remove_app(AppMgr *amgr, struct AppState *app)
{
    amgr->remove_app(app);
}

void
appmgr_signal_longmiss(AppMgr *amgr, struct AppState *app,
                       int dmiss_alist_id)
{
    amgr->signal_longmiss(app, dmiss_alist_id);
}

void
appmgr_signal_missdone(AppMgr *amgr, struct AppState *app)
{
    amgr->signal_missdone(app);
}

void
appmgr_prereset_hook(AppMgr *amgr, struct context *ctx)
{
    amgr->prereset_hook(ctx);
}

void
appmgr_signal_idlectx(AppMgr *amgr, struct context *ctx)
{
    amgr->signal_idlectx(ctx);
}

void 
appmgr_signal_finalfill(AppMgr *amgr, struct context *ctx,
                        int commit_not_rename)
{
    amgr->signal_finalfill(ctx, commit_not_rename);
}

void 
appmgr_signal_finalspill(AppMgr *amgr, struct context *ctx,
                        int commit_not_rename)
{
    amgr->signal_finalspill(ctx, commit_not_rename);
}

void
appmgr_signal_haltapp(AppMgr *amgr, struct AppState *app,
                      int ctx_halt_style,
                      struct CBQ_Callback *halted_cb)
{
    amgr->signal_haltapp(app,
                         static_cast<CtxHaltStyle>(ctx_halt_style),
                         halted_cb);
}

void
appmgr_migrate_app_soon(AppMgr *amgr, int app_id, int targ_core_id,
                        int ctx_halt_style,
                        struct CBQ_Callback *migrate_done_cb)
{
    if (amgr->is_migrate_pending(app_id)) {
        amgr->cancel_pending_migration(app_id);
    }
    amgr->migrate_app_soon(app_id, targ_core_id, -1, cyc, false, -1,
                           static_cast<CtxHaltStyle>(ctx_halt_style),
                           migrate_done_cb);
}

void appmgr_alter_mutablemap_sched(AppMgr *amgr, int app_id,
                                   int targ_core_or_neg1)
{
    amgr->alter_mutablemap_sched(app_id, targ_core_or_neg1);
}

void
appmgr_dump(const AppMgr *amgr, void *FILE_out, const char *prefix)
{
    amgr->dump(FILE_out, prefix);
}

void
appmgr_printstats(const AppMgr *amgr, void *FILE_out, const char *prefix)
{
    amgr->printstats(FILE_out, prefix);
}



//
// More miscellaneous utility stuff, tangled bits of code.
//

namespace {

void
shuffle_idvec(PRNGState *prng, IdVec& vec)
{
    {
        const size_t lim = vec.size();
        for (size_t i = 0; i < lim; i++) {
            size_t toswap = prng_next_long(prng) % (lim - i);   // [0..n-i-1]
            toswap += i;                                // toswap in [i..n-1]
            if (toswap != i) {
                int temp = vec[i];
                vec[i] = vec[toswap];
                vec[toswap] = temp;
            }
        }
    }
//    random_shuffle(vec.begin(), vec.end());
}


void
shuffle_idvec(IdVec& vec)
{
    static PRNGState prng;      // Use our own PRNG, for repeatability
    static bool prng_init = false;

    if (!prng_init) {
        prng_reset(&prng, 0);   // Constant seed
        prng_init = true;
    }
    shuffle_idvec(&prng, vec);
}


void
idvec_from_idset(IdVec& dest, const IdSet& src)
{
    dest.clear();
    for (IdSet::const_iterator iter = src.begin(); iter != src.end(); ++iter)
        dest.push_back(*iter);
}


const char *
app_state_name(AppInfoState astate)
{
    const char *result = "(invalid)";
    switch (astate) {
    case AI_Running:
        result = "AI_Running"; break;
    case AI_Running_LongMiss:
        result = "AI_Running_LongMiss"; break;
    case AI_Ready:
        result = "AI_Ready"; break;
    case AI_SwapIn:
        result = "AI_SwapIn"; break;
    case AI_SwapOut_LongMiss:
        result = "AI_SwapOut_LongMiss"; break;
    case AI_SwapOut_LongMiss_Cancel:
        result = "AI_SwapOut_LongMiss_Cancel"; break;
    case AI_SwapOut_Migrate:
        result = "AI_SwapOut_Migrate"; break;
    case AI_SwapOut_Sched:
        result = "AI_SwapOut_Sched"; break;
    case AI_Wait_LongMiss:
        result = "AI_Wait_LongMiss"; break;
    }
    return result;
}


// Consult a PRNG and return true with probability "prob".
// prob 0.0 -> always false, prob 1.0 -> always true
bool
random_bool(PRNGState *prng, double prob)
{
    sim_assert(prob >= 0.0);
    sim_assert(prob <= 1.0);
    double rand_0_1 = prng_next_double(prng);   // Random value in [0,1)
    return prob > rand_0_1;
}


// Slowly erase the first instance of "value" from "container".
void
deque_erase_int(deque<int>& container, int value)
{
    deque<int>::iterator iter = container.begin(),
        end = container.end();
    for (; iter != end; ++iter) {
        if (*iter == value) {
            container.erase(iter);
            break;
        }
    }
}


void
dump_idvec(FILE *out, const IdVec& container)
{
    IdVec::const_iterator iter = container.begin(), end = container.end();
    bool start = true;
    for (; iter != end; ++iter) {
        if (!start)
            fprintf(out, " ");
        fprintf(out, "%d", *iter);
        start = false;
    }
}


void
dump_iddeque(FILE *out, const deque<int>& container)
{
    IdVec tmp(container.begin(), container.end());
    dump_idvec(out, tmp);
}


string
fmt_bstat_i64(const BasicStat_I64& bstat)
{
    std::ostringstream out;
    if (bstat.g_count()) {
        out << std::fixed << std:: setprecision(0);
        out << bstat.g_min() << "/" << bstat.g_mean() << "/" << bstat.g_max()
            << " (n=" << bstat.g_count()
            << ",sd=" << sqrt(bstat.g_variance(false)) << ")";
    } else {
        out << "- (n=0)";
    }
    return out.str();
}


const class MgrSchedInfo *
appmgr_get_schedinfo(const AppMgr *app_mgr)
{
    return &app_mgr->get_mgrinfo();
}



} // Anonymous namespace close
