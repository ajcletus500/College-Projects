//
// Per-app statistics logging
//
// Jeff Brown
// $Id: app-stats-log.cc,v 1.1.2.11.2.2 2008/04/30 22:17:44 jbrown Exp $
//

const char RCSid_1124387573[] =
"$Id: app-stats-log.cc,v 1.1.2.11.2.2 2008/04/30 22:17:44 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>

#include "sim-assert.h"
#include "sys-types.h"
#include "app-stats-log.h"
#include "app-state.h"
#include "context.h"
#include "dyn-inst.h"
#include "utils.h"
#include "sim-cfg.h"
#include "cache-array.h"
#include "cache.h"
#include "main.h"
#include "core-resources.h"

using std::map;
using std::string;


enum { AS_cyc, AS_sched_cyc, AS_commits, AS_mem_commits, AS_itlb_hr,
       AS_dtlb_hr, AS_icache_hr, AS_dcache_hr, AS_l2cache_hr, AS_l3cache_hr,
       AS_bpred_hr, AS_retpred_hr,
       AS_mem_delay, AS_iq_conf, AS_icache_blocks, AS_dcache_blocks,
       AS_l2cache_blocks, AS_l3cache_blocks,
       AS_sched_count, AS_long_mem_detects, AS_long_mem_flushes,
       AS_app_insts_committed, AS_itlb_acc, AS_dtlb_acc, AS_icache_acc, 
       AS_dcache_acc, AS_l2cache_acc, AS_l3cache_acc, AS_bpred_acc, 
       AS_intalu_acc, AS_fpalu_acc, AS_ldst_acc, AS_iq_acc, 
       AS_fq_acc, AS_ireg_acc, AS_freg_acc, AS_iren_acc, AS_fren_acc, 
       AS_lsq_acc, AS_rob_acc, AS_iq_occ, AS_fq_occ, AS_ireg_occ, 
       AS_freg_occ, AS_lsq_occ, AS_rob_occ };


struct AppStatsLog {
protected:
    const AppState *as;         // Application to log
    string file_name;           // Output file name
    string config_path;         // Config path input for settings, ends with /

    u64 stat_mask;              // Bit flags: which stats to log
    FILE *file;                 // Actual output file
    i64 last_log_time;          // Time of last log output
    AppStateExtras *prev_extra; // Copy of last time's as->extra for compare
    AppStateExtras *extra_deltas;
    int out_field;              // Current out field number (for emit_* funcs)

    void read_stat_mask();
    string fmt_stat_mask() const;

    void emit_i64(i64 val) {
        if (out_field > 0) putc(' ', file);
        out_field++;
        fputs(fmt_i64(val), file);
    }
    void emit_float( float num)
    {
        if (out_field > 0) putc(' ', file);
        out_field++;
        fprintf(file, "%.2f",num);
    }
    void emit_frac(i64 numer, i64 denom) {
        if (out_field > 0) putc(' ', file);
        out_field++;
        fputs(fmt_i64(numer), file);
        putc('/', file);
        fputs(fmt_i64(denom), file);
    }
    void emit_hitrate(const ASE_HitRate& hr) {
        emit_frac(hr.hits, hr.acc);
    }
    void emit_corecache_blocks(int cache_select) {
        if (out_field > 0) putc(' ', file);
        out_field++;
        for (int i = 0; i < CoreCount; i++) {
            CacheArray *cache;
            switch (cache_select) {
            case 0: cache = Cores[i]->icache; break;
            case 1: cache = Cores[i]->dcache; break;
            case 2: cache = Cores[i]->l2cache; break;
            case 3: cache = Cores[i]->l3cache; break;
            default:
                cache = NULL;
                abort_printf("invalid cache_select %d\n", cache_select);
            }
            i64 val = cache_get_population(cache, as->app_id);
            if (i > 0) putc(',', file);
            fputs(fmt_i64(val), file);
        }
    }
    void emit_stats(i64 now_cyc);

public:
    AppStatsLog(const AppState *as_, string out_file_, string config_path_,
                i64 interval_, i64 job_id, string workload_path);
    ~AppStatsLog() {
        appextra_destroy(extra_deltas);
        appextra_destroy(prev_extra);
        if (file) fclose(file);
    }
    void log_point(i64 now_cyc);
    void flush() { fflush(file); }
};


AppStatsLog::AppStatsLog(const AppState *as_, string out_file_,
                         string config_path_, i64 interval, i64 job_id,
                         string workload_path)
    : as(as_), file_name(out_file_), config_path(config_path_),
      file(0), last_log_time(0), prev_extra(0), extra_deltas(0)
{
    if (config_path.empty())
        config_path = ".";
    if (config_path[config_path.size() - 1] != '/')
        config_path += "/";

    if (interval < 0) {
        fprintf(stderr, "AppStatsLog: invalid interval: %s\n",
                fmt_i64(interval));
        exit(1);
    }

    read_stat_mask();
    if (!stat_mask) {
        fprintf(stderr, "AppStatsLog: empty stat_mask.\n");
        exit(1);
    }

    file = (FILE *) efopen(file_name.c_str(), 1);
    if (!(prev_extra = appextra_create()) ||
        !(extra_deltas = appextra_create())) {
        fprintf(stderr, "(%s:%i): out of memory allocating "
                "prev_extra/extra_deltas\n",
                __FILE__, __LINE__);
        exit(1);
    }

    last_log_time = cyc;

    {
        // Emit file header
        time_t now = time(0);
        fprintf(file,
                "# app stats log started %s"
                "# app A%d (", ctime(&now), as->app_id);
        for (int i = 0; i < as->params->argc; i++)
            fprintf(file, "%s%s", (i) ? " " : "", as->params->argv[i]);
        fprintf(file, ")\n"
                "# job_id: %s\n"
                "# workload: %s\n"
                "# start_cyc: %s\n"
                "# interval: %s\n"
                "# fields: %s\n",
                fmt_i64(job_id), workload_path.c_str(), 
                fmt_now(),
                (interval > 0) ? 
                (string(fmt_i64(interval)) + " cyc").c_str() : "variable",
                fmt_stat_mask().c_str());

    }
    {
        // Spam simulator output
        printf("--Logging A%d stats", as->app_id);
        if (interval > 0)
            printf(" every %s cyc", fmt_i64(interval));
        printf(" to \"%s\": %s\n", file_name.c_str(), fmt_stat_mask().c_str());
    }
}


void
AppStatsLog::emit_stats(i64 now_cyc)
{
    // Be sure to keep this order sync'd with fmt_stat_mask()
    out_field = 0;
    i64 interval = now_cyc - last_log_time; 
    if (GET_BITS_64(stat_mask, AS_cyc, 1))
        emit_i64(interval);
    if (GET_BITS_64(stat_mask, AS_sched_cyc, 1))
        emit_i64(extra_deltas->sched_cyc_before_last);
    if (GET_BITS_64(stat_mask, AS_sched_count, 1))
        emit_i64(extra_deltas->total_go_count);
    if (GET_BITS_64(stat_mask, AS_commits, 1))
        emit_i64(extra_deltas->total_commits);
    if (GET_BITS_64(stat_mask, AS_mem_commits, 1))
        emit_i64(extra_deltas->mem_commits);
    if (GET_BITS_64(stat_mask, AS_app_insts_committed, 1))
        emit_i64(extra_deltas->app_inst_last_commit);
    if (GET_BITS_64(stat_mask, AS_long_mem_detects, 1))
        emit_i64(extra_deltas->long_mem_detected);
    if (GET_BITS_64(stat_mask, AS_long_mem_flushes, 1))
        emit_i64(extra_deltas->long_mem_flushed);
    if (GET_BITS_64(stat_mask, AS_itlb_hr, 1))
        emit_hitrate(extra_deltas->hitrate.itlb);
    if (GET_BITS_64(stat_mask, AS_dtlb_hr, 1))
        emit_hitrate(extra_deltas->hitrate.dtlb);
    if (GET_BITS_64(stat_mask, AS_icache_hr, 1)) 
        emit_hitrate(extra_deltas->hitrate.icache);
    if (GET_BITS_64(stat_mask, AS_dcache_hr, 1))
        emit_hitrate(extra_deltas->hitrate.dcache);
    if (GET_BITS_64(stat_mask, AS_l2cache_hr, 1))
        emit_hitrate(extra_deltas->hitrate.l2cache);
    if (GET_BITS_64(stat_mask, AS_l3cache_hr, 1))
        emit_hitrate(extra_deltas->hitrate.l3cache);
    if (GET_BITS_64(stat_mask, AS_bpred_hr, 1))
        emit_hitrate(extra_deltas->hitrate.bpred);
    if (GET_BITS_64(stat_mask, AS_retpred_hr, 1))
        emit_hitrate(extra_deltas->hitrate.retpred);
    if (GET_BITS_64(stat_mask, AS_mem_delay, 1))
        emit_frac(extra_deltas->mem_delay.delay_sum,
                  extra_deltas->mem_delay.sample_count);
    if (GET_BITS_64(stat_mask, AS_iq_conf, 1))
        emit_i64(extra_deltas->instq_conf_cyc);
    if (GET_BITS_64(stat_mask, AS_icache_blocks, 1))
        emit_corecache_blocks(0);
    if (GET_BITS_64(stat_mask, AS_dcache_blocks, 1))
        emit_corecache_blocks(1);
    if (GET_BITS_64(stat_mask, AS_l2cache_blocks, 1)) {
        if (SharedL2Cache) {
            emit_i64(cache_get_population(SharedL2Cache, as->app_id));
        } else {
            emit_corecache_blocks(2);
        }
    }
    if (GET_BITS_64(stat_mask, AS_l3cache_blocks, 1))
        emit_i64((SharedL3Cache) ?
                 cache_get_population(SharedL3Cache, as->app_id) : 0);
    if (GET_BITS_64(stat_mask, AS_itlb_acc, 1)) //VK
        emit_i64(extra_deltas->itlb_acc);
    if (GET_BITS_64(stat_mask, AS_dtlb_acc, 1)) //VK
        emit_i64(extra_deltas->dtlb_acc);
    if (GET_BITS_64(stat_mask, AS_icache_acc, 1)) //VK
        emit_i64(extra_deltas->icache_acc);
    if (GET_BITS_64(stat_mask, AS_dcache_acc, 1)) //VK
        emit_i64(extra_deltas->dcache_acc);
    if (GET_BITS_64(stat_mask, AS_l2cache_acc, 1)) //VK
        emit_i64(extra_deltas->l2cache_acc);
    if (GET_BITS_64(stat_mask, AS_l3cache_acc, 1)) //VK
        emit_i64(extra_deltas->l3cache_acc);
    if (GET_BITS_64(stat_mask, AS_bpred_acc, 1)) //VK
        emit_i64(extra_deltas->bpred_acc);
    if (GET_BITS_64(stat_mask, AS_intalu_acc, 1)) //VK
        emit_i64(extra_deltas->intalu_acc);
    if (GET_BITS_64(stat_mask, AS_fpalu_acc, 1)) //VK
        emit_i64(extra_deltas->fpalu_acc);
    if (GET_BITS_64(stat_mask, AS_ldst_acc, 1)) //VK
        emit_i64(extra_deltas->ldst_acc);
    if (GET_BITS_64(stat_mask, AS_iq_acc, 1)) //VK
        emit_i64(extra_deltas->iq_acc);
    if (GET_BITS_64(stat_mask, AS_fq_acc, 1)) //VK
        emit_i64(extra_deltas->fq_acc);
    if (GET_BITS_64(stat_mask, AS_ireg_acc, 1)) //VK
        emit_i64(extra_deltas->ireg_acc);
    if (GET_BITS_64(stat_mask, AS_freg_acc, 1)) //VK
        emit_i64(extra_deltas->freg_acc);
    if (GET_BITS_64(stat_mask, AS_iren_acc, 1)) //VK
        emit_i64(extra_deltas->iren_acc);
    if (GET_BITS_64(stat_mask, AS_fren_acc, 1)) //VK
        emit_i64(extra_deltas->fren_acc);
    if (GET_BITS_64(stat_mask, AS_lsq_acc, 1)) //VK
        emit_i64(extra_deltas->lsq_acc);
    if (GET_BITS_64(stat_mask, AS_rob_acc, 1)) //VK
        emit_i64(extra_deltas->rob_acc);
    if (GET_BITS_64(stat_mask, AS_iq_occ, 1)) //VK
        emit_float(1. * extra_deltas->iq_occ / interval );
    if (GET_BITS_64(stat_mask, AS_fq_occ, 1)) //VK
        emit_float(1. * extra_deltas->fq_occ / interval );
    if (GET_BITS_64(stat_mask, AS_ireg_occ, 1)) //VK
        emit_float(1. * extra_deltas->ireg_occ / interval );
    if (GET_BITS_64(stat_mask, AS_freg_occ, 1)) //VK
        emit_float(1. * extra_deltas->freg_occ / interval );
    if (GET_BITS_64(stat_mask, AS_lsq_occ, 1)) //VK
        emit_float(1. * extra_deltas->lsq_occ / interval );
    if (GET_BITS_64(stat_mask, AS_rob_occ, 1)) //VK
        emit_float(1. * extra_deltas->rob_occ / interval );
    putc('\n', file);
}


void
AppStatsLog::log_point(i64 now_cyc)
{
    i64 sched_cyc = app_sched_cyc(as);
    appextra_subtract_stats(extra_deltas, as->extra, prev_extra);

    // We'll use the "sched_cyc_before_last" field of prev_extra/extra_deltas
    // to hold and compare the previous values of sched_cyc, since they don't
    // really make sense directly.
    extra_deltas->sched_cyc_before_last = sched_cyc -
        prev_extra->sched_cyc_before_last;
    extra_deltas->last_go_time = -1;

    emit_stats(now_cyc);

    appextra_assign_stats(prev_extra, as->extra);
    prev_extra->sched_cyc_before_last = sched_cyc;
    prev_extra->last_go_time = -1;
    last_log_time = now_cyc;
}


void
AppStatsLog::read_stat_mask()
{
    string path = config_path + "stat_mask/";
    u64 result = 0;
    if (simcfg_get_bool((path + "all").c_str()))
        result = U64_MAX;

    if (simcfg_get_bool((path + "cyc").c_str()))
        result |= SET_BIT_64(AS_cyc);
    if (simcfg_get_bool((path + "sched_cyc").c_str()))
        result |= SET_BIT_64(AS_sched_cyc);
    if (simcfg_get_bool((path + "sched_count").c_str()))
        result |= SET_BIT_64(AS_sched_count);
    if (simcfg_get_bool((path + "commits").c_str()))
        result |= SET_BIT_64(AS_commits);
    if (simcfg_get_bool((path + "mem_commits").c_str()))
        result |= SET_BIT_64(AS_mem_commits);
    if (simcfg_get_bool((path + "app_insts_committed").c_str()))
        result |= SET_BIT_64(AS_app_insts_committed);
    if (simcfg_get_bool((path + "long_mem_detects").c_str()))
        result |= SET_BIT_64(AS_long_mem_detects);
    if (simcfg_get_bool((path + "long_mem_flushes").c_str()))
        result |= SET_BIT_64(AS_long_mem_flushes);
    if (simcfg_get_bool((path + "itlb_hr").c_str()))
        result |= SET_BIT_64(AS_itlb_hr);
    if (simcfg_get_bool((path + "dtlb_hr").c_str()))
        result |= SET_BIT_64(AS_dtlb_hr);
    if (simcfg_get_bool((path + "icache_hr").c_str()))
        result |= SET_BIT_64(AS_icache_hr);
    if (simcfg_get_bool((path + "dcache_hr").c_str()))
        result |= SET_BIT_64(AS_dcache_hr);
    if (simcfg_get_bool((path + "l2cache_hr").c_str()))
        result |= SET_BIT_64(AS_l2cache_hr);
    if (simcfg_get_bool((path + "l3cache_hr").c_str()))
        result |= SET_BIT_64(AS_l3cache_hr);
    if (simcfg_get_bool((path + "bpred_hr").c_str()))
        result |= SET_BIT_64(AS_bpred_hr);
    if (simcfg_get_bool((path + "retpred_hr").c_str()))
        result |= SET_BIT_64(AS_retpred_hr);
    if (simcfg_get_bool((path + "mem_delay").c_str()))
        result |= SET_BIT_64(AS_mem_delay);
    if (simcfg_get_bool((path + "iq_conf").c_str()))
        result |= SET_BIT_64(AS_iq_conf);
    if (simcfg_get_bool((path + "icache_blocks").c_str()))
        result |= SET_BIT_64(AS_icache_blocks);
    if (simcfg_get_bool((path + "dcache_blocks").c_str()))
        result |= SET_BIT_64(AS_dcache_blocks);
    if (simcfg_get_bool((path + "l2cache_blocks").c_str()))
        result |= SET_BIT_64(AS_l2cache_blocks);
    if (simcfg_get_bool((path + "l3cache_blocks").c_str()))
        result |= SET_BIT_64(AS_l3cache_blocks);
    //VK
    if (simcfg_get_bool((path + "itlb_acc").c_str()))
        result |= SET_BIT_64(AS_itlb_acc);
    if (simcfg_get_bool((path + "dtlb_acc").c_str()))
        result |= SET_BIT_64(AS_dtlb_acc);
    if (simcfg_get_bool((path + "icache_acc").c_str()))
        result |= SET_BIT_64(AS_icache_acc);
    if (simcfg_get_bool((path + "dcache_acc").c_str()))
        result |= SET_BIT_64(AS_dcache_acc);
    if (simcfg_get_bool((path + "l2cache_acc").c_str()))
        result |= SET_BIT_64(AS_l2cache_acc);
    if (simcfg_get_bool((path + "l3cache_acc").c_str()))
        result |= SET_BIT_64(AS_l3cache_acc);
    if (simcfg_get_bool((path + "bpred_acc").c_str()))
        result |= SET_BIT_64(AS_bpred_acc);
    if (simcfg_get_bool((path + "intalu_acc").c_str()))
        result |= SET_BIT_64(AS_intalu_acc);
    if (simcfg_get_bool((path + "fpalu_acc").c_str()))
        result |= SET_BIT_64(AS_fpalu_acc);
    if (simcfg_get_bool((path + "ldst_acc").c_str()))
        result |= SET_BIT_64(AS_ldst_acc);
    if (simcfg_get_bool((path + "iq_acc").c_str()))
        result |= SET_BIT_64(AS_iq_acc);
    if (simcfg_get_bool((path + "fq_acc").c_str()))
        result |= SET_BIT_64(AS_fq_acc);
    if (simcfg_get_bool((path + "ireg_acc").c_str()))
        result |= SET_BIT_64(AS_ireg_acc);
    if (simcfg_get_bool((path + "freg_acc").c_str()))
        result |= SET_BIT_64(AS_freg_acc);
    if (simcfg_get_bool((path + "iren_acc").c_str()))
        result |= SET_BIT_64(AS_iren_acc);
    if (simcfg_get_bool((path + "fren_acc").c_str()))
        result |= SET_BIT_64(AS_fren_acc);
    if (simcfg_get_bool((path + "lsq_acc").c_str()))
        result |= SET_BIT_64(AS_lsq_acc);
    if (simcfg_get_bool((path + "rob_acc").c_str()))
        result |= SET_BIT_64(AS_rob_acc);
    if (simcfg_get_bool((path + "iq_occ").c_str()))
        result |= SET_BIT_64(AS_iq_occ);
    if (simcfg_get_bool((path + "fq_occ").c_str()))
        result |= SET_BIT_64(AS_fq_occ);
    if (simcfg_get_bool((path + "ireg_occ").c_str()))
        result |= SET_BIT_64(AS_ireg_occ);
    if (simcfg_get_bool((path + "freg_occ").c_str()))
        result |= SET_BIT_64(AS_freg_occ);
    if (simcfg_get_bool((path + "lsq_occ").c_str()))
        result |= SET_BIT_64(AS_lsq_occ);
    if (simcfg_get_bool((path + "rob_occ").c_str()))
        result |= SET_BIT_64(AS_rob_occ);
    //ENDVK
    stat_mask = result;
}


string
AppStatsLog::fmt_stat_mask() const
{ 
    // Be sure to keep this order sync'd with emit_stats()
    string result;
    if (GET_BITS_64(stat_mask, AS_cyc, 1))
        result += "cyc ";
    if (GET_BITS_64(stat_mask, AS_sched_cyc, 1))
        result += "sched_cyc ";
    if (GET_BITS_64(stat_mask, AS_sched_count, 1))
        result += "sched_count ";
    if (GET_BITS_64(stat_mask, AS_commits, 1))
        result += "commits ";
    if (GET_BITS_64(stat_mask, AS_mem_commits, 1))
        result += "mem_commits ";
    if (GET_BITS_64(stat_mask, AS_app_insts_committed, 1))
        result += "app_insts_committed ";
    if (GET_BITS_64(stat_mask, AS_long_mem_detects, 1))
        result += "long_mem_detects ";
    if (GET_BITS_64(stat_mask, AS_long_mem_flushes, 1))
        result += "long_mem_flushes ";
    if (GET_BITS_64(stat_mask, AS_itlb_hr, 1))
        result += "itlb_hr ";
    if (GET_BITS_64(stat_mask, AS_dtlb_hr, 1))
        result += "dtlb_hr ";
    if (GET_BITS_64(stat_mask, AS_icache_hr, 1))
        result += "icache_hr ";
    if (GET_BITS_64(stat_mask, AS_dcache_hr, 1))
        result += "dcache_hr ";
    if (GET_BITS_64(stat_mask, AS_l2cache_hr, 1))
        result += "l2cache_hr ";
    if (GET_BITS_64(stat_mask, AS_l3cache_hr, 1))
        result += "l3cache_hr ";
    if (GET_BITS_64(stat_mask, AS_bpred_hr, 1))
        result += "bpred_hr ";
    if (GET_BITS_64(stat_mask, AS_retpred_hr, 1))
        result += "retpred_hr ";
    if (GET_BITS_64(stat_mask, AS_mem_delay, 1))
        result += "mem_delay ";
    if (GET_BITS_64(stat_mask, AS_iq_conf, 1))
        result += "iq_conf ";
    if (GET_BITS_64(stat_mask, AS_icache_blocks, 1))
        result += "icache_blocks ";
    if (GET_BITS_64(stat_mask, AS_dcache_blocks, 1))
        result += "dcache_blocks ";
    if (GET_BITS_64(stat_mask, AS_l2cache_blocks, 1))
        result += "l2cache_blocks ";
    if (GET_BITS_64(stat_mask, AS_l3cache_blocks, 1))
        result += "l3cache_blocks ";
    if (GET_BITS_64(stat_mask, AS_itlb_acc, 1)) //VK
        result += "itlb_acc ";
    if (GET_BITS_64(stat_mask, AS_dtlb_acc, 1)) //VK
        result += "dtlb_acc ";
    if (GET_BITS_64(stat_mask, AS_icache_acc, 1)) //VK
        result += "icache_acc ";
    if (GET_BITS_64(stat_mask, AS_dcache_acc, 1)) //VK
        result += "dcache_acc ";
    if (GET_BITS_64(stat_mask, AS_l2cache_acc, 1)) //VK
        result += "l2cache_acc ";
    if (GET_BITS_64(stat_mask, AS_l3cache_acc, 1)) //VK
        result += "l3cache_acc ";
    if (GET_BITS_64(stat_mask, AS_bpred_acc, 1)) //VK
        result += "bpred_acc ";
    if (GET_BITS_64(stat_mask, AS_intalu_acc, 1)) //VK
        result += "intalu_acc ";
    if (GET_BITS_64(stat_mask, AS_fpalu_acc, 1)) //VK
        result += "fpalu_acc ";
    if (GET_BITS_64(stat_mask, AS_ldst_acc, 1)) //VK
        result += "ldst_acc ";
    if (GET_BITS_64(stat_mask, AS_iq_acc, 1)) //VK
        result += "iq_acc ";
    if (GET_BITS_64(stat_mask, AS_fq_acc, 1)) //VK
        result += "fq_acc ";
    if (GET_BITS_64(stat_mask, AS_ireg_acc, 1)) //VK
        result += "ireg_acc ";
    if (GET_BITS_64(stat_mask, AS_freg_acc, 1)) //VK
        result += "freg_acc ";
    if (GET_BITS_64(stat_mask, AS_iren_acc, 1)) //VK
        result += "iren_acc ";
    if (GET_BITS_64(stat_mask, AS_fren_acc, 1)) //VK
        result += "fren_acc ";
    if (GET_BITS_64(stat_mask, AS_lsq_acc, 1)) //VK
        result += "lsq_acc ";
    if (GET_BITS_64(stat_mask, AS_rob_acc, 1)) //VK
        result += "rob_acc ";
    if (GET_BITS_64(stat_mask, AS_iq_occ, 1)) //VK
        result += "iq_occ ";
    if (GET_BITS_64(stat_mask, AS_fq_occ, 1)) //VK
        result += "fq_occ ";
    if (GET_BITS_64(stat_mask, AS_ireg_occ, 1)) //VK
        result += "ireg_occ ";
    if (GET_BITS_64(stat_mask, AS_freg_occ, 1)) //VK
        result += "freg_occ ";
    if (GET_BITS_64(stat_mask, AS_lsq_occ, 1)) //VK
        result += "lsq_occ ";
    if (GET_BITS_64(stat_mask, AS_rob_occ, 1)) //VK
        result += "rob_occ ";
    result.erase(result.size() - 1);
    return result;
}


//
// C interface
//

AppStatsLog *
appstatslog_create(const AppState *as, const char *out_file,
                   const char *config_path, i64 interval,
                   i64 job_id, const char *workload_path)
{
    return new AppStatsLog(as, out_file, config_path, interval,
                           job_id, workload_path);
}

void
appstatslog_destroy(AppStatsLog *asl)
{
    delete asl;
}

void appstatslog_log_point(AppStatsLog *asl, i64 now_cyc)
{
    asl->log_point(now_cyc);
}

void appstatslog_flush(AppStatsLog *asl)
{
    asl->flush();
}



struct LongMemLogger {
    struct StallApp {
        i64 stall_cyc;          // Most recent stall
        i64 complete_cyc;       // Most recent stall completion
        mem_addr data_stall_pc; // Most recent data stall PC (not I-cache)
        StallApp() : 
            stall_cyc(0), complete_cyc(0), data_stall_pc(0) { }
    };

    FILE *file;
    i64 prev_log_cyc;
    map<int,StallApp> per_app_info;

public:
    LongMemLogger(const char *file_name) 
        : file(0), prev_log_cyc(0) {
        file = (FILE *) efopen(file_name, 1);
        fprintf(file,
                "# long memory event log\n"
                "# stall: <d_cyc> s <app> <ctx> <memtype> <d_pcshift|0>"
                " <addr> <stall - issue> <n_stalled>\n"
                "# flush: <d_cyc> f <app> <ctx> <ninst> <is_late>\n"
                "# complete: <d_cyc> c <app> <ctx|-1>\n");
    }
    ~LongMemLogger() {
        if (file) fclose(file);
    }
    void flush() {
        fflush(file);
    }

    void log_stall(const context *ctx, const activelist *stall_inst) {
        bool is_i_miss = (stall_inst == NULL);
        i64 now_cyc = cyc;
        char mem_type[10];
        mem_addr pc, addr;
        sim_assert(ctx->as != NULL);
        StallApp& per_app = per_app_info[ctx->as->app_id];
        int num_stalled = context_alist_inflight(ctx, ctx->as->app_id);
        i64 issue_age = (stall_inst) ? (now_cyc - stall_inst->issuecycle) : 0;
        i64 pc_delta;
        sim_assert(!stall_inst || (stall_inst->status & MEMORY));
        if (is_i_miss) {
            pc = 0;
            addr = ctx->as->npc;
            pc_delta = 0;
        } else {
            pc = stall_inst->pc;
            addr = ((stall_inst->mem_flags) & SMF_Read) ? stall_inst->srcmem :
                stall_inst->destmem;
            pc_delta = ((i64) pc) - ((i64) per_app.data_stall_pc);
            sim_assert((pc_delta & 0x3) == 0);
            pc_delta = ARITH_RIGHT_SHIFT(pc_delta, 2);
        }
        e_snprintf(mem_type, sizeof(mem_type), "%s%s%s",
                   (stall_inst) ? "" : "i",
                   (stall_inst && (stall_inst->mem_flags & SMF_Read))
                   ? "r" : "",
                   (stall_inst && (stall_inst->mem_flags & SMF_Write))
                   ? "w" : "");
        fprintf(file, "%s s %d %d %s %s %s %s %d\n",
                fmt_i64(now_cyc - prev_log_cyc),
                ctx->as->app_id, ctx->id, mem_type, fmt_i64(pc_delta),
                fmt_x64(addr), fmt_i64(issue_age), num_stalled);
        per_app.stall_cyc = now_cyc;
        if (!is_i_miss)
            per_app.data_stall_pc = pc;
        prev_log_cyc = now_cyc;
    }

    void log_flush(const context *ctx) {
        i64 now_cyc = cyc;
        sim_assert(ctx->as != NULL);
        StallApp& per_app = per_app_info[ctx->as->app_id];
        int num_flushed = context_alist_inflight(ctx, ctx->as->app_id);
        bool is_late = per_app.stall_cyc < now_cyc;
        fprintf(file, "%s f %d %d %d %d\n",
                fmt_i64(now_cyc - prev_log_cyc),
                ctx->as->app_id, ctx->id, num_flushed, is_late);
        prev_log_cyc = now_cyc;
    }

    void log_complete(int app_id, int ctx_id) {
        StallApp& per_app = per_app_info[app_id];
        i64 now_cyc = cyc;
        // If ctx_id == -1, this may or may not be redundant: when the AppMgr
        // signals the context to halt and evict the app, it registers the app
        // to be awakened when the cache request is completed.  However, if
        // the cache request completes before the actual memory instruction
        // has been flushed, this will be called when the memory instruction
        // is satisfied, and then immediately after when the cache request
        // itself is considered completed.  It's easy enough to filter: this
        // happens in the same cycle.
        if ((ctx_id != -1) || (per_app.complete_cyc < now_cyc)) {
            fprintf(file, "%s c %d %d\n", fmt_i64(now_cyc - prev_log_cyc),
                    app_id, ctx_id);
        }
        per_app.complete_cyc = now_cyc;
        prev_log_cyc = now_cyc;
    }
};


//
// C interface
//

LongMemLogger *
longmemlog_create(const char *out_file)
{
    return new LongMemLogger(out_file);
}

void
longmem_destroy(LongMemLogger *logger)
{
    delete logger;
}

void
longmem_flush(LongMemLogger *logger)
{
    logger->flush();
}

void
longmem_log_stall(LongMemLogger *logger, const struct context *ctx,
                  const struct activelist *stall_inst)
{
    logger->log_stall(ctx, stall_inst);
}

void
longmem_log_flush(LongMemLogger *logger, const struct context *ctx)
{
    logger->log_flush(ctx);
}

void
longmem_log_complete(LongMemLogger *logger, int app_id, int ctx_id)
{
    logger->log_complete(app_id, ctx_id);
}

