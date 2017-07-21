//
// "Work queue": queue of workloads to be executed within a simulator
// instance, plus the mechanisms to load, dispatch, and stop them.
//
// Jeff Brown
// $Id: work-queue.h,v 1.1.2.4.2.1.2.4 2008/10/24 20:34:36 jbrown Exp $
//
 
#ifndef WORK_QUEUE_H
#define WORK_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif


// Defined elsewhere
struct AppMgr;
struct AppState;
struct CallbackQueue;


struct WorkQueue;
typedef struct WorkQueue WorkQueue;


WorkQueue *workq_create(const char *config_path,
                        struct AppMgr *target_amgr,
                        struct CallbackQueue *callback_queue);
void workq_destroy(WorkQueue *wq);
void workq_dump(const WorkQueue *wq, void *FILE_out, const char *prefix);

// Simulator is about to exit; finish up with any stats logging/etc.
// (This should not be called more than once.)
void workq_gen_final_stats(WorkQueue *wq);

// Add work (jobs) as specified within the subtree rooted at config_path
void workq_add_job_simcfg(WorkQueue *wq, const char *config_path);

// Enable / disable dispatch and halting actions by the WorkQueue.
// (Actions will be buffered while disabled.)
// Be warned that this isn't well tested.
int workq_is_enabled(const WorkQueue *wq);
int workq_set_enabled(WorkQueue *wq, int new_enabled);

// Signal a job that it has reached some limit, and ought to exit
void workq_job_limit_reached(WorkQueue *wq, i64 job_id);
// Signal that some appstate has called the "exit" syscall
void workq_app_sysexit(WorkQueue *wq, struct AppState *as);

// Test: are any jobs still waiting to start, or which may continue to execute
int workq_any_unfinished(const WorkQueue *wq);

// Wart: start any jobs set for time zero, just before simulation begins,
// in order to match stats with pre-WorkQueue simulators
void workq_sim_prestart_jobs(WorkQueue *wq);

// Notify workq that the simulator is exiting: no further simulator will
// take place.  (Disables some warnings in workq_destroy(), for instance.)
void workq_simulator_exiting(WorkQueue *wq);


#ifdef __cplusplus
}
#endif

#endif  /* WORK_QUEUE_H */
