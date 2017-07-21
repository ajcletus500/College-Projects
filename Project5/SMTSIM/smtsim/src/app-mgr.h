//
// Application manager: schedule application execution/migration, including
// access to idle thread state storage
//
// Jeff Brown
// $Id: app-mgr.h,v 1.1.2.7.2.2.2.2 2009/11/13 06:18:45 jbrown Exp $
//

#ifndef APP_MGR_H
#define APP_MGR_H


// Defined elsewhere
struct CoreResources;
struct context;
struct AppState;
struct CBQ_Callback;


#ifdef __cplusplus
extern "C" {
#endif

typedef struct AppMgr AppMgr;

typedef struct AppMgrParams {
    int dummy;
} AppMgrParams;


AppMgr *appmgr_create(const AppMgrParams *params);
void appmgr_destroy(AppMgr *amgr);

void appmgr_register_idle_ctx(AppMgr *amgr, struct context *ctx);
void appmgr_setup_done(AppMgr *amgr);

void appmgr_add_ready_app(AppMgr *amgr, struct AppState *app);

// Remove an app from the domain of the AppMgr.  This is fairly low-level and
// dangerous; if the app is not idle or if the AppMgr has callbacks pending
// related to that app, chaos will likely follow.
void appmgr_remove_app(AppMgr *amgr, struct AppState *app);

void appmgr_signal_longmiss(AppMgr *amgr, struct AppState *app,
                            int dmiss_alist_id);
void appmgr_signal_missdone(AppMgr *amgr, struct AppState *app);

void appmgr_prereset_hook(AppMgr *amgr, struct context *ctx);
void appmgr_signal_idlectx(AppMgr *amgr, struct context *ctx);

void appmgr_signal_finalfill(AppMgr *amgr, struct context *ctx,
                             int commit_not_rename);
void appmgr_signal_finalspill(AppMgr *amgr, struct context *ctx,
                              int commit_not_rename);

// Signal the appmgr to stop the given app (if it's running), swap it out
// from any context where it may be resident, and trigger the given callback
// (if non-NULL) once that app is idle.
void appmgr_signal_haltapp(AppMgr *amgr, struct AppState *app,
                           int ctx_halt_style,
                           struct CBQ_Callback *halted_cb);

// Request an app be moved to a core.  This causes a bunch of stuff to
// happen, and might be fragile.  (It arguably doesn't belong here, but
// does make some experiments easier.)  migrate_done_cb is owned by the AppMgr
// after this call, and will be invoked after migration (iff non-null).
void appmgr_migrate_app_soon(AppMgr *amgr, int app_id, int targ_core_id,
                             int ctx_halt_style,
                             struct CBQ_Callback *migrate_done_cb);

void appmgr_alter_mutablemap_sched(AppMgr *amgr, int app_id,
                                   int targ_core_or_neg1);

void appmgr_dump(const AppMgr *amgr, void *FILE_out, const char *prefix);
void appmgr_printstats(const AppMgr *amgr, void *FILE_out, const char *prefix);


#ifdef __cplusplus
}
#endif

#endif  // APP_MGR_H
