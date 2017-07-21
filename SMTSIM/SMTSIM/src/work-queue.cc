//
// "Work queue": queue of workloads to be executed within a simulator
// instance, plus the mechanisms to load, dispatch, and stop them.
//
// Jeff Brown
// $Id: work-queue.cc,v 1.1.2.6.2.3.2.14.4.1 2009/12/25 06:31:53 jbrown Exp $
// 

const char RCSid_1187823723[] =
"$Id: work-queue.cc,v 1.1.2.6.2.3.2.14.4.1 2009/12/25 06:31:53 jbrown Exp $";

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <sstream>

#include "sim-assert.h"
#include "hash-map.h"
#include "sys-types.h"
#include "work-queue.h"
#include "utils.h"
#include "utils-cc.h"
#include "sim-cfg.h"
#include "callback-queue.h"
#include "app-mgr.h"
#include "app-state.h"
#include "main.h"
#include "context.h"
#include "emulate.h"
#include "jtimer.h"
#include "app-stats-log.h"
#include "bbtracker.h"

using std::string;
using std::list;
using std::map;
using std::make_pair;
using std::ostringstream;
using std::set;
using std::vector;


namespace {

static bool DestroyFinishedApps = false;

void
report_app_init(const AppState *as)
{
    printf("App A%d: (", as->app_id);
    for (int i = 0; i < as->params->argc; i++)
        printf("%s%s", (i) ? " " : "", as->params->argv[i]);
    printf(")\n");
}


void 
fast_forward_single(AppState *as, i64 ff_dist)
{
    JTimer *ff_timer = jtimer_create();
    bool sim_timer_was_running;
    i64 start_insts = as->stats.total_insts, ff_steps;

    // as->extra is where we store the job ID; be sure its there,
    // in case fast-forwarding syscalls "exit"
    sim_assert(as->extra != NULL);
    printf("--Fast-forwarding app A%d for %s insts...\n", 
           as->app_id, fmt_i64(ff_dist));
    fflush(0);
    sim_assert(ff_dist >= 0);

    sim_timer_was_running = jtimer_startstop(SimTimer, 0);
    jtimer_startstop(ff_timer, 1);
    fast_forward_app(as, ff_dist);
    jtimer_startstop(ff_timer, 0);
    jtimer_startstop(SimTimer, sim_timer_was_running);

    JTimerTimes times;
    jtimer_read(ff_timer, &times);
    ff_steps = as->stats.total_insts - start_insts;
    printf("--FF'd %s insts in %s sec: %#.4g inst/s\n",
           fmt_i64(ff_steps), fmt_times(&times), 
           (double) ff_steps / (times.user_msec / 1000.0));
    jtimer_destroy(ff_timer);
    sim_assert((ff_steps == ff_dist) || as->exit.has_exit);
}


// Read older-style argv array from simcfg tree, from before we added the
// explicit "list-value" syntax.  Nobody is likely to use this, since
// argv-from-simcfg only existed for a few weeks before the list-value
// syntax was added, but it's not that hard to do.
bool
read_argv_old(AppParams *ap, const string& cfg_path)
{
    const char *fname = "read_argv_old";
    ap->argc = simcfg_get_int((cfg_path + "/argc").c_str());
    if (ap->argc <= 0) {
        err_printf("%s: empty arg list for workload %s; no "
                   "executable name to load\n", fname, cfg_path.c_str());
        return false;
    }
    ap->bin_filename = e_strdup(simcfg_get_str(
                                    (cfg_path + "/argv_0").c_str()));

    ap->argv = static_cast<char **>
        (emalloc_zero((1 + ap->argc) * sizeof(ap->argv[0])));
    for (int i = 0; i < ap->argc; i++) {
        string key = cfg_path + "/argv_" + fmt_i64(i);
        ap->argv[i] = e_strdup(simcfg_get_str(key.c_str()));
    }
    ap->argv[ap->argc] = NULL;
    return true;
}


bool
read_argv_list(AppParams *ap, const string& cfg_path)
{
    const char *fname = "read_argv_list";
    ap->argc = simcfg_get_int((cfg_path + "/argv/size").c_str());
    if (ap->argc <= 0) {
        err_printf("%s: empty arg list for workload %s; no "
                   "executable name to load\n", fname, cfg_path.c_str());
        return false;
    }
    ap->bin_filename = e_strdup(simcfg_get_str(
                                    (cfg_path + "/argv/_0").c_str()));

    ap->argv = static_cast<char **>
        (emalloc_zero((1 + ap->argc) * sizeof(ap->argv[0])));
    for (int i = 0; i < ap->argc; i++) {
        string key = cfg_path + "/argv/_" + fmt_i64(i);
        ap->argv[i] = e_strdup(simcfg_get_str(key.c_str()));
    }
    ap->argv[ap->argc] = NULL;
    return true;
}


// Create an AppParams struct based on the workload spec in the config tree
// at "cfg_path".  Exits on failure.
AppParams *
create_app_params(const string& cfg_path)
{
    const char *fname = "create_app_params";
    AppParams *ap = NULL;
    if (!(ap = app_params_create())) {
        err_printf("%s: app_params_create() failed\n", fname);
        goto err;
    }

    ap->initial_working_dir = e_strdup(InitialWorkingDir);

    {
        bool have_old = simcfg_have_val((cfg_path + "/argv_0").c_str()) ||
            simcfg_have_val((cfg_path + "/argc").c_str());
        bool have_new = simcfg_have_val((cfg_path + "/argv").c_str());
        if (have_old && !have_new) {
            if (!read_argv_old(ap, cfg_path))
                goto err;
        } else if (have_old && have_new) {
            err_printf("%s: both old and new style argv present in "
                       "config at \"%s\"\n", fname, cfg_path.c_str());
            goto err;
        } else {
            if (!read_argv_list(ap, cfg_path))  // let it gripe if missing
                goto err;
        }
    }

    // Empty environment
    ap->env = static_cast<char **>(emalloc_zero(1 * sizeof(ap->env[0])));
    ap->env[0] = NULL;

    {
        string redir_key = cfg_path + "/stdin";
        if (simcfg_have_val(redir_key.c_str())) {
            const char *in_name = simcfg_get_str(redir_key.c_str());
            if (!(ap->FILE_in = fopen(in_name, "rb"))) {
                err_printf("%s: can't open app stdin file \"%s\": %s\n",
                           fname, in_name, strerror(errno));
                goto err;
            }
        } else {
            ap->FILE_in = static_cast<FILE *>(FILE_DevNullIn);
        }
        redir_key = cfg_path + "/stdout";
        if (simcfg_have_val(redir_key.c_str())) {
            const char *out_name = simcfg_get_str(redir_key.c_str());
            if (!(ap->FILE_out = fopen(out_name, "wb"))) {
                err_printf("%s: can't open app stdout file \"%s\": %s\n",
                           fname, out_name, strerror(errno));
                goto err;
            }
        } else {
            ap->FILE_out = static_cast<FILE *>(FILE_DevNullOut);
        }
        ap->FILE_err = ap->FILE_out;
    }

    return ap;

err:
    // note, not all errors will make it here, the simcfg_* routines like to
    // exit on error.
    app_params_destroy(ap);
    exit(1);
    return NULL;
}


enum JobState {
    JS_NotScheduled,            // Never started, not scheduled to start
    JS_WaitingToStart,          // Waiting for start time, no JobInstance
    JS_Starting,                // JobInstance/AppStates being created
    JS_StartupCanceled,         // Startup canceled (e.g. exit during FF)
    JS_Started,                 // AppState(s) created and sent to AppMgr
    JS_WaitingToStop,           // AppMgr has been asked to de-schedule this
    JS_Stopped,                 // App is active, but removed from AppMgr
    JS_Finished                 // AppState vacated or destroyed
};

static const char *JobState_names[] = {
    "NotScheduled",
    "WaitingToStart",
    "Starting",
    "StartupCanceled",
    "Started",
    "WaitingToStop",
    "Stopped",
    "Finished",
    NULL
};



// Resources for one instantiated job (i.e., loaded into memory, AppState(s)
// created, etc.).  This represents the state corresponding to one JobInfo
// that's needed to actually run the job, but which is not needed before or
// afterwards.
class JobInstance {
    i64 job_id;
    string workload_path;
    AppMgr *app_mgr;                    // (linked: which one to use)
    WorkQueue *work_queue;              // linked: parent W.Q.
    CallbackQueue *cb_queue;            // linked: global sim-time event queue
    vector<AppState *> apps;
    bool maybe_running;
    i64 last_appstats_log;

    class SingleHaltedCB;
    set<int> pending_app_halts;         // indices into apps[]
    set<SingleHaltedCB *> pending_halt_cbs;     // (for destruction)
    CBQ_Callback *all_halted_cb;        // Called after pending_app_halts done

    void single_app_halted(int app_index, SingleHaltedCB *single_halt_cb);

    class AppStatsCB;
    void init_appstats_log(int app_index);

    NoDefaultCopy nocopy;

public:
    JobInstance(i64 job_id_, const string& workload_path_,
                AppMgr *app_mgr_, WorkQueue *work_queue_,
                CallbackQueue *cb_queue_);
    ~JobInstance();
    string fmt() const;
    string fmt_app_ids() const;
    void start();
    void halt_signal(CBQ_Callback *halt_done_cb);
    void vacate_apps();
    void final_stats();
    void simulator_exiting();
};


// Callback: executing job has hit some limit
class JobLimitReachedCB : public CBQ_Callback {
    WorkQueue *wq;
    i64 job_id;
public:
    JobLimitReachedCB(WorkQueue *wq_, i64 job_id_) 
        : wq(wq_), job_id(job_id_) { }
    i64 invoke(CBQ_Args *args) {
        workq_job_limit_reached(wq, job_id);
        return -1;
    }
};


// Callback: one single app halted (from AppMgr)
class JobInstance::SingleHaltedCB : public CBQ_Callback {
    bool aborted;
    JobInstance& jinst;
    int app_index;              // index into "apps" vector
public:
    SingleHaltedCB(JobInstance& jinst_, int app_index_)
        : aborted(false), jinst(jinst_), app_index(app_index_) { }
    void abort_cb() { aborted = true; }
    i64 invoke(CBQ_Args *args) {
        if (!aborted)
            jinst.single_app_halted(app_index, this);
        return -1;
    }
};


JobInstance::JobInstance(i64 job_id_, const string& workload_path_,
                         AppMgr *app_mgr_, WorkQueue *work_queue_,
                         CallbackQueue *cb_queue_)
    : job_id(job_id_), workload_path(workload_path_),
      app_mgr(app_mgr_), work_queue(work_queue_), cb_queue(cb_queue_),
      maybe_running(false), last_appstats_log(-1), all_halted_cb(0)
{
    const char *fname = "JobInstance::JobInstance";
    AppParams *app_params = NULL;
    simcfg_expand_workload(workload_path.c_str());

    app_params = create_app_params(workload_path);

    int n_threads = 1;
    {
        string key(workload_path + "/n_threads");
        if (simcfg_have_val(key.c_str())) {
            n_threads = simcfg_get_int(key.c_str());
        }
    }
    if (n_threads < 1) {
        exit_printf("%s: invalid n_threads (%d)\n", fname, n_threads);
    }

    // Create the AppStates needed to start this job
    for (int i = 0; i < n_threads; i++) {
        if (i == 0) {
            AppState *as = NULL;
            if (!(as = appstate_new_fromfile(app_params, GlobalAlloc))) {
                exit_printf("%s: AppState creation failed, app thread %d\n",
                            fname, i);
            }
            if (!(as->extra = appextra_create())) {
                exit_printf("%s: AppStateExtras creation failed, "
                            "app thread %d\n", fname, i);
            }
            as->extra->job_id = job_id;
            apps.push_back(as);
            report_app_init(as);
        } else {
            // simstate -- fix this, implement multithreading & sharing,
            // or just add thread/fork() support.  or something.  once
            // we have a decent parallel workload...
            exit_printf("pre-shared multi-threading stuff isn't fixed yet, "
                        "sorry\n");
        }
        init_appstats_log(i);
    }

    string ff_key = workload_path + "/ff_dist";
    i64 ff_dist = (simcfg_have_val(ff_key.c_str())) 
        ? simcfg_get_i64(ff_key.c_str()) : 0;
    
    string ff_forever_key("Debug/ff_forever");
    string disable_ff_key("Debug/disable_ff");
    if (simcfg_have_val(ff_forever_key.c_str()) &&
        simcfg_get_bool(ff_forever_key.c_str()))
        ff_dist = I64_MAX;
    if (simcfg_have_val(disable_ff_key.c_str()) &&
        simcfg_get_bool(disable_ff_key.c_str()))
        ff_dist = 0;

    // Complete the setup of, and fast-forward the first created AppState
    // (semantics for any additional pre-created apps[1...] are still in flux)
    {
        AppState *as = apps.at(0);
        string key = workload_path + "/commit_count";
        if (simcfg_have_val(key.c_str())) {
            i64 limit_val = simcfg_get_i64(key.c_str());
            if (limit_val >= 0) {
                callbackq_enqueue(as->extra->watch.commit_count, limit_val,
                                  new JobLimitReachedCB(work_queue, job_id));

            }
        }
        key = workload_path + "/inst_count";
        if (simcfg_have_val(key.c_str())) {
            i64 limit_val = simcfg_get_i64(key.c_str());
            if (limit_val >= 0) {
                callbackq_enqueue(as->extra->watch.app_inst_commit, limit_val,
                                  new JobLimitReachedCB(work_queue, job_id));

            }
        }

        as->extra->fast_forward_dist = ff_dist;
        if (ff_dist > 0) {
            // May stop early, or call exit(), due to exit syscall
            fast_forward_single(as, ff_dist);
        }
    }

    app_params_destroy(app_params);
}


JobInstance::~JobInstance()
{
    const char *fname = "JobInstance::~JobInstance";
    // If this job instance has been started but not properly halted,
    // destroying these AppStates will cause all kinds of havoc, as they're
    // linked from within AppMgr and possibly active contexts/dynamic insts.
    // Then again, if this job is still active, nobody should be deleting this
    // JobInstance handle.
    if (maybe_running) {
        fflush(0);
        fprintf(stderr, "%s: warning: destroying JobInstance with possibly "
                "active AppStates:\n", fname);
        for (int i = 0; i < (int) apps.size(); ++i)
            fprintf(stderr, "  A%d: %p\n", apps[i]->app_id, (void *) apps[i]);
        fprintf(stderr, "...which will likely spell doom.\n");
    }
    for (set<SingleHaltedCB *>::iterator iter = pending_halt_cbs.begin();
         iter != pending_halt_cbs.end(); ++iter) {
        // Flag the outstanding callback to have no effect, but leave it
        // alive: AppMgr owns it now
        (*iter)->abort_cb();
    }
    for (int i = 0; i < (int) apps.size(); ++i) {
        DEBUGPRINTF("--destroying A%d: %p\n", apps[i]->app_id, 
                    (const void *) apps[i]);
        appextra_destroy(apps[i]->extra);
        appstate_destroy(apps[i]);
    }
    delete all_halted_cb;
}


string
JobInstance::fmt() const
{
    ostringstream out;
    out << "apps: [";
    for (int i = 0; i < (int) apps.size(); ++i)
        out << " " << apps[i]->app_id;
    out << " ] maybe_running " << maybe_running
        << " all_halted_cb " << static_cast<void *>(all_halted_cb);
    return out.str();
}


// format just the app ids, e.g. "1 3 4"
string
JobInstance::fmt_app_ids() const
{
    ostringstream out;
    for (int i = 0; i < (int) apps.size(); ++i) {
        if (i > 0)
            out << " ";
        out << apps[i]->app_id;
    }
    return out.str();
}



// Callback: log stats for one job
class JobInstance::AppStatsCB : public CBQ_Callback {
    JobInstance& jinst;
    AppStatsLog *asl;
    i64 interval;
public:
    AppStatsCB(JobInstance& jinst_, AppStatsLog *asl_, i64 interval_)
        : jinst(jinst_), asl(asl_), interval(interval_) { }
    i64 invoke(CBQ_Args *args) {
        appstatslog_log_point(asl, cyc);
        jinst.last_appstats_log = cyc;
        return cyc + interval;          // reschedule callback
    }
};


// AppStatsLog has historically been a global setting, and this is just
// backward-compatible.  We may add the ability to be more discriminating,
// later.
void
JobInstance::init_appstats_log(int app_index)
{
    AppState *as = apps.at(app_index);
    if (!simcfg_get_bool("AppStatsLog/enable"))
        return;

    i64 interval = simcfg_get_i64("AppStatsLog/interval");
    if (interval <= 0) {
        exit_printf("variable-length log intervals are not "
                    "currently supported\n");
        exit(1);
    }

    string base_name(simcfg_get_str("AppStatsLog/base_name"));
    string file_name = base_name + ".A" + fmt_i64(as->app_id);

    sim_assert(!as->extra->stats_log && !as->extra->stats_log_cb);
    as->extra->stats_log =
        appstatslog_create(as, file_name.c_str(), "AppStatsLog/",
                           interval, job_id, workload_path.c_str());
    as->extra->stats_log_cb = new AppStatsCB(*this, as->extra->stats_log,
                                             interval);
    callbackq_enqueue(cb_queue, cyc + interval, as->extra->stats_log_cb);
}


void
JobInstance::start()
{
    maybe_running = true;
    appmgr_add_ready_app(app_mgr, apps.at(0));
}


// Callback method: the AppMgr has finished halting one of the AppStates for
// this JobInstance, and has invoked "single_halt_cb" as requested.  In
// the general case, our callback may not be the only one waiting for that
// AppState to be halted, and a prior waiter may conceivably change the
// scheduling state in a way which messes things up.  For now(ha), we know
// that "single_halt_cb" is solo and authoritative, so we'll go ahead
// without additional safety checking.
void
JobInstance::single_app_halted(int app_index, SingleHaltedCB *single_halt_cb)
{
    const char *fname = "JobInstance::single_app_halted";
    sim_assert(all_halted_cb != NULL);
    sim_assert(pending_app_halts.count(app_index));
    pending_app_halts.erase(app_index);
    pending_halt_cbs.erase(single_halt_cb);
    appmgr_remove_app(app_mgr, apps.at(app_index));
    DEBUGPRINTF("%s: index %d (A%d) halt done, %d apps remaining\n",
                fname, app_index, apps.at(app_index)->app_id,
                (int) pending_app_halts.size());
    if (pending_app_halts.empty()) {
        DEBUGPRINTF("%s: all apps halted, scheduling callback %p\n", fname,
                    (void *) all_halted_cb);
        maybe_running = false;
        // Set things so that all_halted_cb is called soon.  We can't invoke
        // it directly here, since it will likely turn around and destroy
        // this JobInstance while this method is still active.  This introduces
        // a window of vulnerability, if the JobInfo which all_halted_cb
        // refers to is destroyed between this callback_enqueue and the
        // callback invocation, bad things will happen.
        callbackq_enqueue(cb_queue, 0, all_halted_cb);
        all_halted_cb = NULL;
    }
}


void
JobInstance::halt_signal(CBQ_Callback *halt_done_cb)
{
    DEBUGPRINTF("JobInstance::halt_signal: halting %d apps, callback %p "
                "when done\n", (int) apps.size(), (void *) halt_done_cb);
    sim_assert(all_halted_cb == NULL);
    all_halted_cb = halt_done_cb;
    // Create one callback per halting AppState; whichever one is last will
    // invoke "halt_done_cb" from our parent
    for (int idx = 0; idx < (int) apps.size(); idx++) {
        AppState *app = apps[idx];
        SingleHaltedCB *single_halted_cb = new SingleHaltedCB(*this, idx);
        appmgr_signal_haltapp(app_mgr, app, CtxHaltStyle_Fast,
                              single_halted_cb);
        pending_app_halts.insert(idx);
        pending_halt_cbs.insert(single_halted_cb);
    }
}


void
JobInstance::vacate_apps()
{
    const char *fname = "JobInstance::vacate_apps";
    if (maybe_running) {
        fflush(0);
        fprintf(stderr, "%s: warning: JobInstance vacating possibly-active "
                "AppStates:\n", fname);
        for (int i = 0; i < (int) apps.size(); ++i)
            fprintf(stderr, "  A%d: %p\n", apps[i]->app_id, (void *) apps[i]);
        fprintf(stderr, "...which will likely spell doom.\n");
    }
    for (int i = 0; i < (int) apps.size(); ++i) {
        AppState *as = apps[i];
        AppStateExtras *ase = as->extra;
        DEBUGPRINTF("vacating A%d: %p\n", as->app_id, (void *) as);
        appstate_vacate(apps[i]);
        ase->vacate_time = cyc;
        if (ase->stats_log_cb) {
            if (last_appstats_log < cyc)
                callback_invoke(ase->stats_log_cb, NULL);
            // Cancel future stats logging
            callbackq_cancel(cb_queue, ase->stats_log_cb);      // destroys cb
            ase->stats_log_cb = NULL;
            appstatslog_flush(ase->stats_log);
        }
        if (ase->stats_log) {
            // We may as well destroy this: it's write-only from the
            // simulator's perspective, and we're done writing to it.
            appstatslog_destroy(ase->stats_log);
            ase->stats_log = NULL;
        }
    }
}


void
JobInstance::final_stats()
{
    for (int i = 0; i < (int) apps.size(); ++i) {
        AppState *as = apps[i];
        AppStateExtras *ase = as->extra;
        if (ase->stats_log_cb) {
            // This app is logging stats, and the sim is about to exit;
            // force an extra invocation to pick up the last (short)
            // interval.
            if (last_appstats_log < cyc)
                callback_invoke(ase->stats_log_cb, NULL);
            sim_assert(ase->stats_log);
            appstatslog_flush(ase->stats_log);
        }
    }
}


// The simulator is exiting; even if apps are still marked running, we're
// going to destroy them all.
void
JobInstance::simulator_exiting()
{
    // We'll force this to false, to avoid the warning messages about
    // deleting active AppStates.  (It's OK to delete them now, since
    // we're not going to simulate on them any more.)
    maybe_running = false;
}


// Information to track one queued/running/ran job.  Any data used only for
// the actual execution of the job should be put into "active_job"; other
// than that, the contents of JobInfo should be limited to what's need to
// track a pending or completed job.
class JobInfo {
    i64 job_id;                 // internal ID#
    JobState state;             // current scheduling state
    string job_path;            // WorkQueue/Jobs/<jobname>
    JobInstance *active_job;    // Active sim objects, when running; NULL o.w.

    // Parameters from config tree
    string workload_path;       // Workloads/<workname>
    i64 start_time;

    class HaltDoneCB;
    void halt_done();
    CBQ_Callback *job_finished_cb;

    NoDefaultCopy nocopy;

public:
    JobInfo(i64 job_id_, const string &job_path_);
    ~JobInfo();
    string fmt() const;
    string fmt_app_ids() const;
    i64 g_id() const { return job_id; }
    JobState g_state() const { return state; }
    i64 g_start_time() const { return start_time; }
    const string& g_path() const { return job_path; }

    void start(AppMgr *app_mgr, WorkQueue *work_queue, CallbackQueue *cb_queue,
               CBQ_Callback *job_finished_cb_);
    void limit_reached(AppMgr *app_mgr);
    void syscall_exit(AppMgr *amgr, AppState *as);
    void final_stats();
    void simulator_exiting();
};


JobInfo::JobInfo(i64 job_id_, const string& job_path_)
    : job_id(job_id_), state(JS_NotScheduled),
      job_path(job_path_), active_job(0), start_time(-1),
      job_finished_cb(0)
{
    string workload_name(simcfg_get_str((job_path + "/workload").c_str()));
    workload_path = string("Workloads") + "/" + workload_name;
    {
        string time_key(job_path + "/start_time");
        if (simcfg_have_val(time_key.c_str()))
            start_time = simcfg_get_i64(time_key.c_str());
    }
    if (start_time >= 0) {
        state = JS_WaitingToStart;
    }
}


JobInfo::~JobInfo()
{
    delete active_job;
    delete job_finished_cb;
}
   

string
JobInfo::fmt() const
{
    ostringstream out;
    out << "id " << job_id << " state " << JobState_names[state]
        << " job_path \"" << job_path << "\" workload_path \"" 
        << workload_path << "\" start_time " << start_time;
    if (active_job)
        out << "; active_job: " << active_job->fmt();
    return out.str();
}


string
JobInfo::fmt_app_ids() const
{
    return (active_job) ? active_job->fmt_app_ids() : string();
}


void
JobInfo::start(AppMgr *app_mgr, WorkQueue *work_queue, CallbackQueue *cb_queue,
               CBQ_Callback *job_finished_cb_)
{
    const char *fname = "JobInfo::start";
    DEBUGPRINTF("%s: starting job id %s (%s) at time %s\n", fname,
                fmt_i64(job_id), workload_path.c_str(), fmt_now());
    sim_assert(state == JS_WaitingToStart);
    sim_assert(!active_job);
    sim_assert(!job_finished_cb);
    state = JS_Starting;        // help syscall_exit() detect this
    job_finished_cb = job_finished_cb_;         // May be NULL
    active_job = new JobInstance(job_id, workload_path, app_mgr, work_queue,
                                 cb_queue);
    if (state != JS_Starting) {
        sim_assert(state == JS_StartupCanceled);
        // Job exited during fast-forward, etc.
        halt_done();
    } else{
        active_job->start();
        state = JS_Started;
    }
}


// This is called from several different places
void
JobInfo::halt_done()
{
    DEBUGPRINTF("JobInfo::halt_done: job_id %s halted.\n", fmt_i64(job_id));
    sim_assert((state == JS_WaitingToStop) || (state == JS_StartupCanceled));
    state = JS_Stopped;

    // Currently, the only reasons we halt are for "execution limit reached"
    // or "some AppState in this job syscall'd exit"; these conditions are
    // never reversed, so we'll go ahead and mark the jobs "Finished" and
    // destroy the JobInstance.
    state = JS_Finished;
    if (DestroyFinishedApps) {
        delete active_job;
        active_job = NULL;
    } else {
        // Leaves AppState skeleton and stats behind
        active_job->vacate_apps();
    }

    if (job_finished_cb) {
        callback_invoke(job_finished_cb, NULL);
        delete job_finished_cb;
        job_finished_cb = NULL;
    }
}


// Callback: all components in active_job halted
class JobInfo::HaltDoneCB : public CBQ_Callback {
    JobInfo& jinfo;
public:
    HaltDoneCB(JobInfo& jinfo_) : jinfo(jinfo_) { }
    i64 invoke(CBQ_Args *args) { jinfo.halt_done(); return -1; }
};


void
JobInfo::limit_reached(AppMgr *app_mgr)
{
    const char *fname = "JobInfo::limit_reached";
    DEBUGPRINTF("%s: job id %s (%s) exec limit reached, at time %s\n", fname,
                fmt_i64(job_id), workload_path.c_str(), fmt_now());
    sim_assert(state == JS_Started);
    state = JS_WaitingToStop;
    HaltDoneCB *halt_done_cb = new HaltDoneCB(*this);
    active_job->halt_signal(halt_done_cb);
}


void
JobInfo::syscall_exit(AppMgr *amgr, AppState *as)
{
    const char *fname = "JobInfo::syscall_exit";
    DEBUGPRINTF("%s: syscall_exit() at time %s, %s\n", fname, fmt_now(),
                this->fmt().c_str());
    // Up-call from emulate_call_pal_callsys(); must prevent further emulation
    if (state == JS_Starting) {
        // From inside JobInstance constructor, during fast-forward
        state = JS_StartupCanceled;
        // That's enough to stop emulation; fast_forward_app() will
        // detect the exit status and return early, JobInstance's
        // constructor will complete, then JobInfo::start() will
        // detect this state change and delete the JobInstance before it
        // ever gets submitted to an AppMgr for simulation.
    } else {
        // During simulation: job is fully constructed; we should be able to
        // re-use the existing "limit_reached" framework.  This is a little
        // shaky, since there are so many layers between limit_reached()
        // and a call to context_halt_signal(), and a single emulate call
        // in the meantime will trigger an abort.
        //
        // Since a syscall caused the exit, and syscalls have the
        // SGF_PipeExclusive flag set, the syscall instruction should be the
        // only one from that AppState in the pipeline.  As long as a call to
        // context_halt_signal() occurs before that syscall commits and
        // releases the "draining" flag, no further fetches or emulates should
        // occur, so the fetch-emulate loop will be broken for the app which
        // actually performed the exit syscall.
        sim_assert(state == JS_Started);
        limit_reached(amgr);
    }
}


void
JobInfo::final_stats()
{
    if (active_job)
        active_job->final_stats();
}


void
JobInfo::simulator_exiting()
{
    if (active_job)
        active_job->simulator_exiting();
}


} // Anonymous namespace close


// WorkQueue: stores JobInfo objects, and implements a time-queue to 
// activate them.  (Callback management lives here.)
struct WorkQueue {
protected:
    string wq_config;
    AppMgr *target_amgr;
    CallbackQueue *callback_queue;
    bool enabled;
    bool verbose_sched;
    bool exit_on_app_exit;
    int max_running_jobs;
    bool final_stats_done;

    class JobStartCB;
    class JobFinishedCB;

    i64 next_job_id;
    typedef map<i64,JobInfo *> JobInfoMap;
    JobInfoMap jobs;
    set<i64> unfinished_jobs;
    set<i64> running_jobs;
    list<JobStartCB *> overload_start_fifo;     // CBs "owned" by this

    // Currently, we use one callback per pending job; we'll let the 
    // callback queue system deal with time ordering, since it has to do so
    // anyway.
    typedef map<i64,CBQ_Callback *> JobCallbackMap;     // job ID -> callback
    JobCallbackMap pending_starts;      // waiting for start time
    JobCallbackMap deferred_starts;     // start_time passed when !enabled

    set<i64> deferred_limit_reached;    // limit reached when !enabled

    void start_job_later(JobInfo& jinfo);
    void start_job(JobInfo& jinfo);
    void job_finished(JobInfo& jinfo);
    const JobInfo& get_jinfo(i64 job_id) const {
        JobInfoMap::const_iterator found = jobs.find(job_id);
        if (found == jobs.end()) {
            abort_printf("get_jinfo: invalid job id: %s\n", fmt_i64(job_id));
        }
        return *(found->second);
    }
    JobInfo& get_jinfo(i64 job_id) {
        return const_cast<JobInfo &>
            (static_cast<const WorkQueue *>(this)->get_jinfo(job_id));
    }

    bool is_fully_loaded() const {
        return (max_running_jobs >= 0) &&
            ((int) running_jobs.size() >= max_running_jobs);
    }
    void add_overload_startcb(JobStartCB *start_cb);
    void service_overload_queue();

public:
    WorkQueue(const string& wq_config_, AppMgr *target_amgr_,
              CallbackQueue *callback_queue_);
    ~WorkQueue();
    void dump(void *FILE_out, const string& prefix) const;
    void gen_final_stats();
    void add_job_simcfg(const string& config_path);

    bool is_enabled() const { return enabled; }
    bool set_enabled(bool new_enabled);
    void job_limit_reached(i64 job_id);
    void app_sysexit(AppState *as);
    bool any_unfinished() const;
    void sim_prestart_jobs();
    void simulator_exiting();
};


WorkQueue::WorkQueue(const string& wq_config_,
                     AppMgr *target_amgr_, CallbackQueue *callback_queue_)
    : wq_config(wq_config_), target_amgr(target_amgr_), 
      callback_queue(callback_queue_), final_stats_done(false),
      next_job_id(0)
{
    enabled = simcfg_get_bool((wq_config + "/enable").c_str());
    verbose_sched = simcfg_get_bool((wq_config + "/verbose_sched").c_str()) ||
        debug;
    exit_on_app_exit =
        simcfg_get_bool((wq_config + "/exit_on_app_exit").c_str());
    max_running_jobs =
        simcfg_get_int((wq_config + "/max_running_jobs").c_str());
}


WorkQueue::~WorkQueue()
{
    for (JobInfoMap::iterator iter = jobs.begin(); iter != jobs.end();
         ++iter) {
        delete iter->second;
    }
    for (JobCallbackMap::iterator iter = pending_starts.begin();
         iter != pending_starts.end(); ++iter) {
        callbackq_cancel(callback_queue, iter->second);
    }
    for (JobCallbackMap::iterator iter = deferred_starts.begin();
         iter != deferred_starts.end(); ++iter) {
        delete iter->second;
    }
}


void
WorkQueue::add_job_simcfg(const string& config_path)
{
    JobInfo *new_job = new JobInfo(next_job_id, config_path);
    if (!jobs.insert(make_pair(next_job_id, new_job)).second) {
        abort_printf("duplicate job_id %s, shouldn't happen\n",
                     fmt_i64(next_job_id));
    }
    unfinished_jobs.insert(next_job_id);
    next_job_id++;
    sim_assert(next_job_id > 0);
    if (new_job->g_state() == JS_WaitingToStart)
        start_job_later(*new_job);
}


void
WorkQueue::job_limit_reached(i64 job_id)
{
    sim_assert(job_id >= 0);
    JobInfo& jinfo = get_jinfo(job_id);
    if (jinfo.g_state() == JS_WaitingToStop)
        return;
    if (enabled) {
        jinfo.limit_reached(target_amgr);
    } else {
        deferred_limit_reached.insert(job_id);
    }
    if (verbose_sched)
        printf("WorkQueue: job %s \"%s\" limit-reached at time %s%s\n",
               fmt_i64(jinfo.g_id()), jinfo.g_path().c_str(), fmt_now(),
               (enabled) ? "" : " (deferred)");
}


// An appstate has syscall'd exit; we need to make sure it doesn't
// emulate further.  There are two main ways to reach here, roughly:
//   1) JobInstance constructor -> fast_forward_app() -> emulate -> here,
//      (JobInfo state == JS_Starting)
//   2) fetch() simulation -> emulate -> here
// ...though we also allow for the possiblity of
//   3) Other code created an AppState which has called exit.
//
// In cases 1&2, there will be a fully-constructed JobInfo object corresponding
// to as->extra->job_id, though case 1 will have only a partially-constructed
// JobInstance.  For case 3, as->extra->job_id == -1, and we can't do much
// with it here.
void
WorkQueue::app_sysexit(AppState *as)
{
    i64 job_id = (as->extra) ? as->extra->job_id : -1;
    if (exit_on_app_exit) {
        printf("app_sysexit: A%d job_id %s syscall exit(%s) at "
               "inst %s time %s\n",
               as->app_id, fmt_i64(job_id), fmt_i64(as->exit.exit_code),
               fmt_i64(as->stats.total_insts), fmt_now());

        if (BBTrackerParams.create_bbv_file) // Block Vector generation 
            bbtracker_exit(); 
        sim_exit_ok("exit syscall");
    } else if (job_id == -1) {  // Case 3: externally-created AppState
        if (verbose_sched)
            printf("WorkQueue: non-managed app A%d syscall exit(%s) at "
                   "time %s; ignoring\n", as->app_id, 
                   fmt_i64(as->exit.exit_code), fmt_now());
        // It's the "external" code's responsibility to ensure that
        // the given app isn't emulated further
        sim_assert(!BBTrackerParams.create_bbv_file); // Only single thread
    } else {
        JobInfo& jinfo = get_jinfo(job_id);
        if (verbose_sched)
            printf("WorkQueue: job %s \"%s\", app A%d syscall exit(%s) at "
                   "time %s, job state %s\n", fmt_i64(jinfo.g_id()),
                   jinfo.g_path().c_str(), as->app_id, 
                   fmt_i64(as->exit.exit_code), fmt_now(),
                   JobState_names[jinfo.g_state()]);
        jinfo.syscall_exit(target_amgr, as);
        sim_assert(!BBTrackerParams.create_bbv_file); // Only single thread
    }
}


bool
WorkQueue::any_unfinished() const
{
    return !unfinished_jobs.empty();
}


void
WorkQueue::sim_prestart_jobs()
{
    const char *fname = "WorkQueue::sim_prestart_jobs";
    sim_assert(cyc == 0);
    sim_assert(running_jobs.empty());
    if (!enabled)
        return;
    vector<i64> to_start;
    for (JobInfoMap::const_iterator iter = jobs.begin(); iter != jobs.end();
         ++iter) {
        const JobInfo& jinfo = *(iter->second);
        if ((jinfo.g_state() == JS_WaitingToStart) &&
            (jinfo.g_start_time() == 0)){
            to_start.push_back(jinfo.g_id());
        }
    }
    //printf("to_start.size()=%d\n", to_start.size());
    if (to_start.size() > 1 && BBTrackerParams.create_bbv_file){
        exit_printf("BBV generation tested only for single-threaded"
                    " applications. You should disable create_bbv_file?\n");
    } 
    
    if ((max_running_jobs >= 0) &&
        ((int) to_start.size() > max_running_jobs)) {
        err_printf("%s: warning: max_running_jobs (%d) less than "
                   "pre-startable job count (%d); "
                   "preferring jobs with lower IDs\n",
                   fname, max_running_jobs, (int) to_start.size());
        if (verbose_sched)
            printf("%s: deferring pre-start jobs:", fname);
        while ((int) to_start.size() > max_running_jobs) {
            if (verbose_sched)
                printf(" %s", fmt_i64(to_start.back()));
            to_start.pop_back();
        }
        if (verbose_sched)
            printf("\n");
    }
    if (verbose_sched && !to_start.empty()) {
        printf("%s: pre-starting jobs:", fname);
        for (vector<i64>::const_iterator iter = to_start.begin();
             iter != to_start.end(); ++iter)
            printf(" %s", fmt_i64(*iter));
        printf("\n");
    }
    for (vector<i64>::const_iterator iter = to_start.begin();
         iter != to_start.end(); ++iter) {
        i64 job_id = *iter;
        JobInfo& jinfo = get_jinfo(job_id);
        start_job(jinfo);
    }
}


void
WorkQueue::simulator_exiting()
{
    for (JobInfoMap::iterator iter = jobs.begin(); iter != jobs.end();
         ++iter) {
        JobInfo *job = iter->second;
        job->simulator_exiting();
    }
}


class WorkQueue::JobStartCB : public CBQ_Callback {
    WorkQueue& wq;
    JobInfo& jinfo;
    i64 job_id;
public:
    JobStartCB(WorkQueue& wq_, JobInfo& jinfo_)
        : wq(wq_), jinfo(jinfo_), job_id(jinfo_.g_id()) { }
    i64 g_job_id() const { return job_id; }
    i64 invoke(CBQ_Args *args) {
        // Several invocation paths are possible:
        // 1) from CallbackQueue, job start_time has arrived while WQ enabled
        // 2) from set_enabled(), WQ re-enabled after start_time passed,
        //    bypassing callback queue
        // 3) from service_overload_queue() a previous invocation of this
        //    occurred when the system was too loaded, and load has dropped.

        if (wq.is_fully_loaded()) {
            // Guaranteed not from case #3; service_overload_queue() checks
            // the load first.  If in pending_starts, it's from case #1, so
            // we need to remove it from the CallbackQueue before it's
            // deleted: we're transferring ownership to the overload-queue
            if (wq.pending_starts.count(job_id)) {
                sim_assert(wq.pending_starts[job_id] == this);
                wq.pending_starts.erase(job_id);
                callbackq_cancel_ret(wq.callback_queue, this);
            }
            wq.add_overload_startcb(this);
        } else {
            // critical: must unlink callback before calling start_job, or
            // start_job will destroy it (while we're executing it).
            // (callback may be absent from pending_starts, if it's coming
            // through overload_start_fifo)
            assert_ifthen(wq.pending_starts.count(job_id),
                          wq.pending_starts[job_id] == this);
            wq.pending_starts.erase(job_id);
            wq.start_job(jinfo);
        }
        return -1;      // Destroy callback if not cancelled
    }
};


// signal "up" from JobInfo: the given job has finished
class WorkQueue::JobFinishedCB : public CBQ_Callback {
    WorkQueue& wq;
    JobInfo& jinfo;
public:
    JobFinishedCB(WorkQueue& wq_, JobInfo& jinfo_)
        : wq(wq_), jinfo(jinfo_) { }
    i64 invoke(CBQ_Args *args) {
        wq.job_finished(jinfo);
        return -1;      // Destroy callback
    }
};


void
WorkQueue::start_job_later(JobInfo& jinfo)
{
    sim_assert(jinfo.g_start_time() >= 0);
    JobStartCB *start_cb = new JobStartCB(*this, jinfo);
    callbackq_enqueue(callback_queue, jinfo.g_start_time(), start_cb);
    sim_assert(!pending_starts.count(jinfo.g_id()));
    pending_starts.insert(make_pair(jinfo.g_id(), start_cb));
}


// Take the task described by jinfo, create a JobInstance for it, and
// and submit it to the AppMgr for execution.  If there is an outstanding
// callback to start this job, delete that callback as superfluous.
void
WorkQueue::start_job(JobInfo& jinfo)
{
    const char *fname = "WorkQueue::start_job";
    i64 job_id = jinfo.g_id();

    sim_assert(enabled);

    if (pending_starts.count(job_id)) {
        // JobStartCB removes the subject job from pending_starts before
        // calling this.  If it's still in pending_starts, someone else has
        // called start_job(), and we need to cancel the outstanding,
        // now-redundant callback.
        callbackq_cancel(callback_queue, pending_starts[job_id]);
        pending_starts.erase(job_id);
    } else if (deferred_starts.count(job_id)) {
        delete deferred_starts[job_id];
        deferred_starts.erase(job_id);
    }

    jinfo.start(target_amgr, this, callback_queue,
                new JobFinishedCB(*this, jinfo));
    if (jinfo.g_state() == JS_Started) {
        sim_assert(!running_jobs.count(job_id));
        running_jobs.insert(job_id);
        if (debug) {
            DEBUGPRINTF("%s: job %s started, current queue:\n", fname,
                        fmt_i64(job_id));
            dump(stdout, "  ");
        }
        if (verbose_sched)
            printf("WorkQueue: job %s \"%s\" started at time %s, initial apps "
                   "[ %s ]\n",
                   fmt_i64(jinfo.g_id()), jinfo.g_path().c_str(), fmt_now(),
                   jinfo.fmt_app_ids().c_str());
    } else {
        if (verbose_sched)
            printf("WorkQueue: job %s \"%s\" startup canceled, apps [ %s ]\n",
                   fmt_i64(jinfo.g_id()), jinfo.g_path().c_str(),
                   jinfo.fmt_app_ids().c_str());
        // jinfo.start() should have detected this, and called halt_done()
        sim_assert(jinfo.g_state() == JS_Finished);
    }
}


// Up-call from JobInfo.halt_done() -- job has finished
void
WorkQueue::job_finished(JobInfo& jinfo)
{
    const char *fname = "WorkQueue::job_finished";
    sim_assert(unfinished_jobs.count(jinfo.g_id()));
    unfinished_jobs.erase(jinfo.g_id());
    sim_assert(running_jobs.count(jinfo.g_id()));
    running_jobs.erase(jinfo.g_id());
    if (debug) {
        DEBUGPRINTF("%s: job %s finished, current queue:\n", fname,
                    fmt_i64(jinfo.g_id()));
        dump(stdout, "  ");
    }
    if (verbose_sched)
        printf("WorkQueue: job %s \"%s\" finished at time %s\n",
               fmt_i64(jinfo.g_id()), jinfo.g_path().c_str(), fmt_now());
    service_overload_queue();
}


void
WorkQueue::add_overload_startcb(JobStartCB *start_cb)
{       
    const char *fname = "WorkQueue::add_overload_startcb";
    i64 job_id = start_cb->g_job_id();
    overload_start_fifo.push_back(start_cb);
    if (debug) {
        DEBUGPRINTF("%s: job %s load-deferred, current queue:\n", fname,
                    fmt_i64(job_id));
        dump(stdout, "  ");
    }
    if (verbose_sched)
        printf("WorkQueue: job %s \"%s\" load-deferred at time %s\n",
               fmt_i64(job_id), get_jinfo(job_id).g_path().c_str(),
               fmt_now());
}


void
WorkQueue::service_overload_queue()
{
    const char *fname = "WorkQueue::service_overload_queue";
    if (!enabled)
        return;
    while (!overload_start_fifo.empty() && !is_fully_loaded()) {
        JobStartCB *start_cb = overload_start_fifo.front();
        overload_start_fifo.pop_front();
        DEBUGPRINTF("%s: servicing cb %p for job %s\n", fname,
                    (void *) start_cb, fmt_i64(start_cb->g_job_id()));
        callback_invoke(start_cb, NULL);
        callback_destroy(start_cb);
    }
}


void
WorkQueue::dump(void *FILE_out, const string& prefix) const
{
    FILE *out = static_cast<FILE *>(FILE_out);
    const char *pf = prefix.c_str();
    fprintf(out, "%sjobs:\n", pf);
    for (JobInfoMap::const_iterator iter = jobs.begin(); iter != jobs.end();
         ++iter) {
        fprintf(out, "%s  %s: %s\n", pf, fmt_i64(iter->first),
                iter->second->fmt().c_str());
    }
    fprintf(out, "%sunfinished_jobs:", pf);
    for (set<i64>::const_iterator iter = unfinished_jobs.begin();
         iter != unfinished_jobs.end(); ++iter) {
        fprintf(out, " %s", fmt_i64(*iter));
    }
    fprintf(out, "\n");
    fprintf(out, "%srunning_jobs:", pf);
    for (set<i64>::const_iterator iter = running_jobs.begin();
         iter != running_jobs.end(); ++iter) {
        fprintf(out, " %s", fmt_i64(*iter));
    }
    fprintf(out, "\n");
    fprintf(out, "%soverload_start_fifo:\n", pf);
    for (list<JobStartCB *>::const_iterator 
             iter = overload_start_fifo.begin();
         iter != overload_start_fifo.end(); ++iter) {
        fprintf(out, "%s  cb %p for job %s\n", pf, (void *) *iter,
                fmt_i64((*iter)->g_job_id()));
    }
    fprintf(out, "%spending starts:\n", pf);
    for (JobCallbackMap::const_iterator iter = pending_starts.begin();
         iter != pending_starts.end(); ++iter) {
        fprintf(out, "%s  cb %p for job %s\n", pf, (void *) iter->second,
                fmt_i64(iter->first));
    }
    fprintf(out, "%sdeferred starts:\n", pf);
    for (JobCallbackMap::const_iterator iter = deferred_starts.begin();
         iter != deferred_starts.end(); ++iter) {
        fprintf(out, "%s  cb %p for job %s\n", pf, (void *) iter->second,
                fmt_i64(iter->first));
    }
    fprintf(out, "%sdeferred_limit_reached:", pf);
    for (set<i64>::const_iterator iter = deferred_limit_reached.begin();
         iter != deferred_limit_reached.end(); ++iter) {
        fprintf(out, " %s", fmt_i64(*iter));
    }
    fprintf(out, "\n");
}


void
WorkQueue::gen_final_stats()
{
    sim_assert(!final_stats_done);
    final_stats_done = true;
    for (JobInfoMap::iterator iter = jobs.begin(); iter != jobs.end();
         ++iter) {
        iter->second->final_stats();
    }
}


bool
WorkQueue::set_enabled(bool new_enabled)
{
    bool prev_enabled = enabled;
    if (prev_enabled && !new_enabled) {
        // Disable: cancel pending callbacks, keep objects for later re-enable
        sim_assert(deferred_starts.empty());
        for (JobCallbackMap::const_iterator iter = pending_starts.begin(); 
             iter != pending_starts.end(); ++iter) {
            i64 job_id = iter->first;
            CBQ_Callback *cb = iter->second;
            callbackq_cancel_ret(callback_queue, cb);
            deferred_starts.insert(make_pair(job_id, cb));
        }
        pending_starts.clear();
    } else if (!prev_enabled && new_enabled) {
        // Re-enable: reschedule disabled callbacks at their original times

        // Service overload queue first, before possibly trying to start more
        service_overload_queue();

        sim_assert(pending_starts.empty());
        for (JobCallbackMap::const_iterator iter = deferred_starts.begin();
             iter != deferred_starts.end(); ++iter) {
            CBQ_Callback *cb = iter->second;
            JobInfo& jinfo = get_jinfo(iter->first);
            if (jinfo.g_start_time() <= cyc) {
                callback_invoke(cb, NULL);
                // problem: who owns cb now?  we'll sketchily infer the
                // action which JobStartCB took, from overflow-queue state
                sim_assert(dynamic_cast<JobStartCB *>(cb));
                if (overload_start_fifo.empty()) {
                    // start-cb started job, and is now orphaned; free it
                    callback_destroy(cb);
                } else {
                    // start-cb added itself to overload queue, which owns it
                    sim_assert(overload_start_fifo.back() == cb);
                }
            } else {
                callbackq_enqueue(callback_queue, jinfo.g_start_time(), cb);
                pending_starts.insert(make_pair(jinfo.g_id(), cb));
            }
        }
        deferred_starts.clear();

        for (set<i64>::const_iterator iter = deferred_limit_reached.begin();
             iter != deferred_limit_reached.end(); ++iter) {
            JobInfo& jinfo = get_jinfo(*iter);
            jinfo.limit_reached(target_amgr);
        }
        deferred_limit_reached.clear();
    }
    enabled = new_enabled;
    return prev_enabled;
}



//
// C interface
//

WorkQueue *
workq_create(const char *config_path,
             struct AppMgr *target_amgr,
             struct CallbackQueue *callback_queue)
{
    return new WorkQueue(string(config_path), target_amgr, callback_queue);
}

void
workq_destroy(WorkQueue *wq)
{
    delete wq;
}

void
workq_dump(const WorkQueue *wq, void *FILE_out, const char *prefix)
{
    wq->dump(FILE_out, string(prefix));
}

void
workq_gen_final_stats(WorkQueue *wq)
{
    wq->gen_final_stats();
}

void
workq_add_job_simcfg(WorkQueue *wq, const char *config_path)
{
    wq->add_job_simcfg(string(config_path));
}

int
workq_is_enabled(const WorkQueue *wq)
{
    return wq->is_enabled();
}

int
workq_set_enabled(WorkQueue *wq, int new_enabled)
{
    return wq->set_enabled(new_enabled);
}

void
workq_job_limit_reached(WorkQueue *wq, i64 job_id)
{
    wq->job_limit_reached(job_id);
}

void
workq_app_sysexit(WorkQueue *wq, struct AppState *as)
{
    wq->app_sysexit(as);
}

int
workq_any_unfinished(const WorkQueue *wq)
{
    return wq->any_unfinished();
}

void
workq_sim_prestart_jobs(WorkQueue *wq)
{
    return wq->sim_prestart_jobs();
}

void
workq_simulator_exiting(WorkQueue *wq)
{
    wq->simulator_exiting();
}
