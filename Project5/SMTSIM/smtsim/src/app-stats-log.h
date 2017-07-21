//
// Per-app statistics logging
//
// Jeff Brown
// $Id: app-stats-log.h,v 1.1.2.3.2.1.2.1 2008/11/05 19:51:51 jbrown Exp $
//

#ifndef APP_STATS_LOG_H
#define APP_STATS_LOG_H


// Defined elsewhere
struct AppState;
struct activelist;
struct context;


#ifdef __cplusplus
extern "C" {
#endif

typedef struct AppStatsLog AppStatsLog;
typedef struct LongMemLogger LongMemLogger;

// interval=0 for "variable"
AppStatsLog *
appstatslog_create(const AppState *as, const char *out_file,
                   const char *config_path, i64 interval,
                   i64 job_id, const char *workload_path);
void appstatslog_destroy(AppStatsLog *asl);
void appstatslog_log_point(AppStatsLog *asl, i64 now_cyc);
void appstatslog_flush(AppStatsLog *asl);


LongMemLogger *
longmemlog_create(const char *out_file);
void longmem_destroy(LongMemLogger *logger);
void longmem_flush(LongMemLogger *logger);

// stall_inst NULL <=> I-miss stall
void longmem_log_stall(LongMemLogger *logger, const struct context *ctx,
                       const struct activelist *stall_inst);
void longmem_log_flush(LongMemLogger *logger, const struct context *ctx);

void longmem_log_complete(LongMemLogger *logger, int app_id, int ctx_id);


#ifdef __cplusplus
}
#endif

#endif  // APP_STATS_LOG_H
