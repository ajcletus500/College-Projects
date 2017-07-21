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

 */

#define _IEEE 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>


#include "sim-assert.h"
#include "main.h"
#include "sim-params.h"
#include "cache.h"
#include "jtimer.h"
#include "sim-cfg.h"
#include "core-resources.h"
#include "context.h"
#include "app-state.h"
#include "callback-queue.h"
#include "app-mgr.h"
#include "dyn-inst.h"
#include "app-stats-log.h"      // For LongMemLogger
#include "work-queue.h"
#include "debug-coverage.h"
#include "adapt-mgr.h"

long long int cyc, allinstructions;
struct LongMemLogger *GlobalLongMemLogger = NULL;
struct DebugCoverageTracker *EmulateDebugCoverage = NULL;
struct DebugCoverageTracker *FltiRoundDebugCoverage = NULL;
struct DebugCoverageTracker *FltiTrapDebugCoverage = NULL;

static i64 DebugCycle = -1;
static i64 DebugExitCycle = -1;
static int DebugProgress = 0;
static int DebugShowStages = 0;
int j = 0, prev_cyc = 0, prev_instr = 0, count = 1;//modified
float ipc_temp = 0;


/*
 *  run() is the controller for virtually all simulation.
 *
 *  Each cycle it simulates each pipeline stage, then does some
 *    accounting and mop-up before advancing the cycle counter.
 */


int appstate_instcount_check(int);


static void
init_long_mem_log(void)
{
    const char *filename_key = "GlobalLongMemLog/name";
    if (!simcfg_have_val(filename_key))
        return;
    const char *filename = simcfg_get_str(filename_key);

    GlobalLongMemLogger = longmemlog_create(filename);
    if (!GlobalLongMemLogger) {
        fprintf(stderr, "(%s:%i): couldn't create GlobalLongMemLogger\n",
                __FILE__, __LINE__);
        exit(1);
    }
}


// Destroy objects which are global in scope, but also dynamically allocated
// (i.e. with manually-managed lifetime).  This allow various objects to
// perform final cleanup operations, particularly important when writing
// compressed files.
//
// This is typed to be callable via atexit()/exit(), but cleans up after itself
// enough that it may also be called manually, if need be.
//
// (This should probably be moved to a C++ source file, so that it can
// access "delete" if need be, but for now it fits well enough here.)
static void
cleanup_dynamic_globals(void)
{
    // reminder: this is called even on an error-exit
    if (SignalHandlerActive) {
        // We seem to get an extra cleanup "during" the stack trace, perhaps
        // it's the forked child?
        return;
    }
    DEBUGPRINTF("cleanup_dynamic_globals(), time %s\n", fmt_i64(cyc));
    longmem_destroy(GlobalLongMemLogger);
    GlobalLongMemLogger = NULL;
    debug_coverage_destroy(EmulateDebugCoverage);
    EmulateDebugCoverage = NULL;
    debug_coverage_destroy(FltiRoundDebugCoverage);
    FltiRoundDebugCoverage = NULL;
    debug_coverage_destroy(FltiTrapDebugCoverage);
    FltiTrapDebugCoverage = NULL;
}


int run(void)
{
    i64 statpoint = I64_LIT(1000000);

/*  Because I simulate for a constant number of TOTAL instructions,
     optimizations that subtly change the relative priority of
     threads can change (e.g., improve) reported performance without
     actually doing anything positive to the system.  I watch that
     carefully.  You should too.

    For a good discussion of how to report results more accurately
     in the context of changing thread contributions, see:

     Symbiotic Jobscheduling for a Simultaneous Multithreading Architecture,
     Snavely, Tullsen, ASPLOS IX, Nov 2000

     and 

     Handling Long-Latency Loads in a SMT Processor, Tullsen, Brown,
     Micro-34, Dec 2001.
 */
    {
        const char *key1 = "Debug/cycle";
        const char *key2 = "Debug/commit_num";
        const char *key3 = "Debug/progress";
        const char *key4 = "Debug/show_stages";
        const char *key6 = "Debug/exit_cycle";
        if (simcfg_have_val(key1))
            DebugCycle = simcfg_get_i64(key1);
        if (simcfg_have_val(key2))
            DebugCommitNum = simcfg_get_i64(key2);
        if (simcfg_have_val(key3))
            DebugProgress = simcfg_get_bool(key3);
        if (simcfg_have_val(key4))
            DebugShowStages = simcfg_get_bool(key4);
        if (simcfg_have_val(key6))
            DebugExitCycle = simcfg_get_i64(key6);
        init_long_mem_log();
    }
    if (atexit(cleanup_dynamic_globals)) {
        exit_printf("can't register cleanup_dynamic_globals() callback");
    }

    if (warmup) {
            allinstructions = (i64) CtxCount * (i64) warmuptime;
    } else {
        allinstructions = (GlobalParams.allinstructions >= 0) ?
            GlobalParams.allinstructions : 
            ((i64) CtxCount * GlobalParams.thread_length);
    }
    jtimer_startstop(SimTimer, 1);

    workq_sim_prestart_jobs(GlobalWorkQueue);

    while(1)
    {
      /*  going through the pipeline back to front ensures that instructions
          move forward through the pipe accurately (eg, they get backed up if
          later stages stall), but the danger is that information that moves
          backward through the pipeline moves too quickly (ie, in the same
          cycle).  To combat this, the fix_pcs() routine doesn't correct the
          fetch address on mispredicts and misfetches until after the fetch
          stage.  Also, fix_regs() ensures that rename registers aren't reused
          the same cycle */
        limit_resources(); // Adapt execution resources
        
        commit();
        regwrite();
        execute();
        regread();
        queue();
        regrename();
        decode();
        fetch();

        fix_pcs();
        fix_regs();
        calculate_priority();/* fetch priority */
        process_cache_queues();/* handle all memory events*/
        process_tcfill_queues();

		
        if (0 && debug)
            callbackq_dump(GlobalEventQueue, stdout, "  GEQ: ");
        callbackq_service(GlobalEventQueue, cyc, NULL);

	int instr_value = appstate_instcount_check(cyc); //modified code here
        cyc++;
		/*while ((as = appstate_global_iter_next()) != NULL){
		//printf("working12");
			
			curr_commit_instr = as->extra->total_commits;
			// 
            if ((curr_commit_instr > 9999) &&(curr_commit_instr % 10000 == 0) && (commit_instr <= 10000000)){
		if (a==0){
                curr_commit_instr = as->extra->total_commits;
				if(curr_commit_instr - prev_commit_instr)
					{commit_instr = curr_commit_instr - prev_commit_instr;
					cyc_interval = cyc - prev_cyc;
					current_ipc = (double) commit_instr/cyc_interval;
					//printf("curr_commit_instr : %s previous_commi: %s current Cycles: %s prev cycle: %s \n", fmt_i64(curr_commit_instr) , fmt_i64(prev_commit_instr), fmt_i64(cyc), fmt_i64(prev_cyc));
					printf("Cycles: %s IPC: %f \n", fmt_i64(cyc_interval), current_ipc);
		//printf("420");
		//printf("C_Cycle: %s",fmt_i64(cyc));
		prev_cyc = cyc;
                prev_commit_instr = curr_commit_instr;
		a=1;
		}		}
		else{
			if(curr_commit_instr - prev_commit_instr)
			{curr_commit_instr = as->extra->total_commits;
                commit_instr = curr_commit_instr - prev_commit_instr1;
                cyc_interval = cyc - prev_cyc1;
                current_ipc = (double) commit_instr/cyc_interval;
				pp ++;
        //        printf("curr_commit_instr : %s previous_commi: %s current Cycles: %s prev cycle: %s \n", fmt_i64(curr_commit_instr) , fmt_i64(prev_commit_instr1), fmt_i64(cyc), fmt_i64(prev_cyc1));
		printf(" Cycles1: %s IPC1: %f pp: %s\n",fmt_i64(cyc_interval), current_ipc,fmt_i64(pp));
		//printf("123");
		prev_cyc1 = cyc;
                prev_commit_instr1 = curr_commit_instr;
		a=0;
		}}
        }}

        cyc++;*/


#ifdef DEBUG
        if (cyc == DebugCycle) {
            debug = 1;
            fflush(0);
        }
        if (cyc == DebugExitCycle) {
            char msg[100];
            e_snprintf(msg, sizeof(msg), "DebugExitCycle %s reached",
                       fmt_now());
            sim_exit_ok(msg);
        }
        if (!(cyc % 1000000)) {
            if (DebugProgress) {
                i64 t_comm = 0, t_sys = 0;
                JTimerTimes sim_times;
                for (int i = 0; i < CtxCount; i++) {
                    t_comm += Contexts[i]->stats.total_commits;
                    t_sys += Contexts[i]->stats.total_syscalls;
                }
                jtimer_read(SimTimer, &sim_times);
                printf("--Progress: cyc %s commits %s syscalls %s "
                       "simtime %s\n", 
                       fmt_i64(cyc), fmt_i64(t_comm), fmt_i64(t_sys),
                       fmt_times(&sim_times));
            }
            fflush(0); 
        }

        DEBUGPRINTF("cyc = %s\n", fmt_i64(cyc));
        if (debug && DebugShowStages) {
            for (int i = 0; i < CoreCount; i++) 
                core_dump_queues(Cores[i]);
        }
        if (0)
            appstate_instcount_check(0);
#endif // DEBUG

        if (!workq_any_unfinished(GlobalWorkQueue)) {
            sim_exit_ok("GlobalWorkQueue finished");
        }

/*
 *  I print out stats regularly so I can monitor progress, predict
 *    finish times, and catch runaway simulations.  Delete this code
 *    if you don't want that.
 */
//try with no of instructions instead of cycles modified
     // if (cyc >= statpoint && !warmup) {
        //statpoint = cyc + I64_LIT(10000);
	
	
     // }
	
	

#ifndef LONG
     /* if -DLONG, don't stop until someone exits;  otherwise,
        exit after allinstructions counts down to zero.  */
      if ((GlobalParams.thread_length > 0) && (allinstructions == 0)) {
        if (warmup) {
            allinstructions = (GlobalParams.allinstructions >= 0) ?
                GlobalParams.allinstructions : 
                ((i64) CtxCount * GlobalParams.thread_length);
          zero_cstats();
          zero_pstats();
          zero_pipe_stats();
          warmup = 0;
          continue;
        }
        sim_exit_ok("allinstructions");
      }
#endif
    }
}



// Count the number of instructions which have been discarded (skipped, not
// allocated alist entries) by in-flight instructions.
static int
count_pending_discards(const context * restrict ctx, int app_id)
{
    const int alist_used = context_alist_used(ctx);
    int inst_id = ctx->next_to_commit;
    int result = 0;
    
    for (int i = 0; i < alist_used; i++) {
        const activelist * restrict inst = &ctx->alist[inst_id];
        if (!(inst->status & (SQUASHED | INVALID)) &&
            ((inst->as->app_id == app_id) || (app_id == -1)))
            result += inst->insts_discarded_before;
        inst_id = alist_add(ctx, inst_id, 1);
    }
    result += ctx->noop_discard_run_len;
    return result;
}


// Test to make sure the instruction counts add up properly for all apps.
// Note that this is pretty fragile, and is probably wrong in the 
// presence of sync instructions.
// 
// NOTE: this currently doesn't work when context_halt_signal is being used
// in "fast re-steer" mode.
int
appstate_instcount_check(int cyc)
{
    appstate_global_iter_reset();
    AppState *as;
    while ((as = appstate_global_iter_next()) != NULL) {
        int inflight_insts = 0;
        int pending_discards = 0;
        for (int i = 0; i < CtxCount; i++) { 
            const context * restrict ctx = Contexts[i];
            if (ctx->as == as) {
                inflight_insts = context_alist_inflight(ctx, as->app_id);
                pending_discards = count_pending_discards(ctx, as->app_id);
                break;
            }
        }

         // total_insts = commits + discards + inflight + ffdist  //...  maybe?
        i64 check_insts = as->extra->total_commits +
            as->extra->cp_insts_discarded + pending_discards + 
            inflight_insts + as->extra->fast_forward_dist;

	i64 check_comts = as->extra->total_commits;
	
//modified code
	if(check_comts >= (j))
	{
	int dif1 = (check_comts - prev_instr);
	float dif2 = cyc - prev_cyc;
	ipc_temp = dif1/dif2;
	printf("cycles : %d, commits no : %d, ipc : %f\n",cyc,check_comts,ipc_temp);
	j = j + 10000;
	prev_cyc = cyc;
	prev_instr = check_comts;
	}

        if (check_insts != as->stats.total_insts) {
            printf("\n");
            fflush(0);
            fprintf(stderr, "%s (%s:%d): instruction count mismatch for app "
                    "A%d, cyc %s\n", __func__, __FILE__, __LINE__, as->app_id,
                    fmt_i64(cyc));
            fprintf(stderr, "(%s commits + %s discards + %d pending_discards "
                    "+ %d inflight + %s ff) "
                    "== %s != %s total_insts\n",
                    fmt_i64(as->extra->total_commits),
                    fmt_i64(as->extra->cp_insts_discarded),
                    pending_discards, inflight_insts,
                    fmt_i64(as->extra->fast_forward_dist),
                    fmt_i64(check_insts),
                    fmt_i64(as->stats.total_insts));
            sim_abort();
        }
    }
}


static const char *
fmt_missrate(const ASE_HitRate * restrict hr) 
{
    static char fixed_buff[80];
    e_snprintf(fixed_buff, sizeof(fixed_buff), "%.2f",
               100.0 * ((double) (hr->acc - hr->hits) / hr->acc));
    return fixed_buff;
}


static void
appstate_progress(FILE *out)
{
    const AppState *as;
    const char *pref = "  ";

    fprintf(out, "App statistics\n");
    fprintf(out, "%sID_order: [", pref);
    appstate_global_iter_reset();
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %d", as->app_id);
    }
    printf(" ]\n");

    fprintf(out, "%sinsts: [", pref);
    appstate_global_iter_reset();
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %s", fmt_i64(as->stats.total_insts));
    }
    printf(" ]\n");

    fprintf(out, "%scommits: [", pref);
    appstate_global_iter_reset();
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %s", fmt_i64(as->extra->total_commits));
    }
    printf(" ]\n");

    fprintf(out, "%sdiscards: [", pref);
    appstate_global_iter_reset();
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %s", fmt_i64(as->extra->cp_insts_discarded));
    }
    printf(" ]\n");

    fprintf(out, "%ssyscalls: [", pref);
    appstate_global_iter_reset();
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %s", fmt_i64(as->stats.total_syscalls));
    }
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%stotal_cyc: [", pref);
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %s", fmt_i64(app_alive_cyc(as)));
    }
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%ssched_cyc: [", pref);
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %s", fmt_i64(app_sched_cyc(as)));
    }
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%ssched_count: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_i64(as->extra->total_go_count));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%scommits/schedcyc: [", pref);
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %.3f", (double) as->extra->total_commits /
                app_sched_cyc(as));
    }
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%scommits/totalcyc: [", pref);
    while ((as = appstate_global_iter_next()) != NULL) {
        fprintf(out, " %.3f", (double) as->extra->total_commits / cyc);
    }
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sicache_miss_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.icache));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sdcache_accesses: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_i64(as->extra->hitrate.dcache.acc));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sdcache_non-merged_miss_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.dcache));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sl2cache_accesses: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_i64(as->extra->hitrate.l2cache.acc));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sl2cache_non-merged_miss_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.l2cache));
    printf(" ]\n");

    if (GlobalParams.mem.use_l3cache) {
        appstate_global_iter_reset();
        printf("%sl3cache_accesses: [", pref);
        while ((as = appstate_global_iter_next()) != NULL)
            fprintf(out, " %s", fmt_i64(as->extra->hitrate.l3cache.acc));
        printf(" ]\n");

        appstate_global_iter_reset();
        printf("%sl3cache_non-merged_miss_pct: [", pref);
        while ((as = appstate_global_iter_next()) != NULL)
            fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.l3cache));
        printf(" ]\n");
    }

    appstate_global_iter_reset();
    printf("%smem_accesses: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_i64(as->extra->mem_accesses));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sitlb_miss_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.itlb));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sdtlb_miss_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.dtlb));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sbpred_miss_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.bpred));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sretpred_miss_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_missrate(&as->extra->hitrate.retpred));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%savg_mem_delay_cyc: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %.1f", (double) as->extra->mem_delay.delay_sum /
                as->extra->mem_delay.sample_count);
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%sinstq_conf_cyc/schedcyc_pct: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %.2f", (double) 100.0 * as->extra->instq_conf_cyc /
                app_sched_cyc(as));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%slong_mem_detected: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_i64(as->extra->long_mem_detected));
    printf(" ]\n");

    appstate_global_iter_reset();
    printf("%slong_mem_flushed: [", pref);
    while ((as = appstate_global_iter_next()) != NULL)
        fprintf(out, " %s", fmt_i64(as->extra->long_mem_flushed));
    printf(" ]\n");
}


static void
print_progress_stats(void)
{
    i64 sum_commits = 0, sum_syscalls = 0;
    printf("Commits: [");
    for (int i = 0; i < CtxCount; i++) { 
        const context * restrict ctx = Contexts[i];
        printf(" %s", fmt_i64(ctx->stats.total_commits));
        sum_commits += ctx->stats.total_commits;
    }
    printf(" ] = %s\n", fmt_i64(sum_commits));
    printf("Syscalls: [");
    for (int i = 0; i < CtxCount; i++) { 
        const context * restrict ctx = Contexts[i];
        printf(" %s", fmt_i64(ctx->stats.total_syscalls));
        sum_syscalls += ctx->stats.total_syscalls;
    }
    printf(" ] = %s\n", fmt_i64(sum_syscalls));
    printf("In-flight: [");
    int sum_inflight = 0;
    for (int i = 0; i < CtxCount; i++) { 
        const context * restrict ctx = Contexts[i];
        printf(" %d", context_alist_inflight(ctx, -1));
        sum_inflight += context_alist_inflight(ctx, -1);
    }
    printf(" ] = %d\n", sum_inflight);

    appstate_progress(stdout);
    if (0)
        appstate_instcount_check(0);
}


void
print_sim_stats(int final_stats)
{
    static int printed_final = 0;
    schedstats();
    print_progress_stats();
    print_cstats();
    predict_stats();
    
    commit_group_printstats();

    if (GlobalParams.print_appmgr_stats) {
        printf("GlobalAppMgr stats:\n");
        appmgr_printstats(GlobalAppMgr, stdout, "  ");
    }
    if (GlobalLongMemLogger)
        longmem_flush(GlobalLongMemLogger);

    print_adaptmgr_stats();
    
    if (final_stats) {
        sim_assert(!printed_final);
        printed_final = 1;
        finalstats();
        workq_gen_final_stats(GlobalWorkQueue);
    }
    printf("\n");
    if (final_stats)
        time_stats();
    fflush(0);
}


void
sim_exit_ok(const char *short_msg)
{
    fflush(0);
    printf("***** exiting (%s) *****\n", short_msg);
    print_sim_stats(1);
    fflush(0);

    // Destroy adapt manager
    adaptmgr_destroy(GlobalAdaptMgr);
    
    // workq_destroy() destroys appstates and such as well
    workq_simulator_exiting(GlobalWorkQueue);
    workq_destroy(GlobalWorkQueue);
    GlobalWorkQueue = NULL;

    exit(0);
    // this function cannot safely return: it may be called from inside
    // functions working with various objects that it's just destroyed
    sim_abort();
}