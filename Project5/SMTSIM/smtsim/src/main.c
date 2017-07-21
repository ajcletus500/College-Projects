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

Significant parts of the SMTSIM simulator were written by Gun Sirer
(before it became the SMTSIM simulator) and by Jack Lo (after it became
the SMTSIM simulator).  Therefore the following copyrights may also apply:

Copyright (C) Jack Lo
Copyright (C) E. Gun Sirer

Pieces of this code may have been derived from Jim Larus\' SPIM simulator,
which contains the following copyright:

==============================================================
   Copyright (C) 1990-1998 by James Larus (larus@cs.wisc.edu).
   ALL RIGHTS RESERVED.

   SPIM is distributed under the following conditions:

     You may make copies of SPIM for your own use and modify those copies.

     All copies of SPIM must retain my name and copyright notice.

     You may not sell SPIM or distributed SPIM in conjunction with a
     commerical product or service without the expressed written consent of
     James Larus.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.
===============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#include <signal.h>
#include <unistd.h>

#include "sim-assert.h"
#include "main.h"
#include "mem.h"
#include "smt.h"
#include "sys-types.h"
#include "cache.h"
#include "cache-params.h"
#include "core-resources.h"
#include "sim-cfg.h"
#include "jtimer.h"
#include "region-alloc.h"
#include "syscalls.h"
#include "sim-params.h"
#include "context.h"
#include "app-mgr.h"
#include "callback-queue.h"
#include "work-queue.h"
#include "bbtracker.h"
#include "adapt-mgr.h"

int warmup = 0;
i64 warmuptime;
const char *InitialWorkingDir = "(InitialWorkingDir unset)";
static char InitialWorkingDirBuf[PATH_MAX];

#ifdef DEBUG
    // Works like "debug" in utils.{h,c}
    int debugsync = 0;
#endif

int SignalHandlerActive = 0;

static const char *ConfigFileName = "smtsim.conf";
extern const char *StaticConfig;

int CoreCount = 0;
int CtxCount = 0;

struct JTimer *OverallTimer;    // Total SMTSIM run timer
struct JTimer *SimTimer;        // Simulation timer

struct RegionAlloc *GlobalAlloc;        // Segment allocation manager

struct AppMgr *GlobalAppMgr;
struct CallbackQueue *GlobalEventQueue;
struct WorkQueue *GlobalWorkQueue;

void *FILE_DevNullIn, *FILE_DevNullOut;


static void init_cores(void);


static void
init_contexts(void)
{
    sim_assert(CtxCount == 0);
    CtxCount = GlobalParams.num_contexts;

    for (int ctx_id = 0; ctx_id < CtxCount; ctx_id++) {
        ThreadParams params;
        simcfg_thread_params(&params, ctx_id);
        context *ctx = context_create(&params, ctx_id);
        if (!ctx) {
            fprintf(stderr, "%s (%s:%i): create_context failed for "
                    "context #%d\n", get_argv0(), __FILE__, __LINE__, ctx_id);
            exit(1);
        }
        Contexts[ctx_id] = ctx;
    }
}
        

static void
save_builtin_conf(const char *filename)
{
    printf("Writing built-in configuration to \"%s\".\n", filename);
    int use_stdout = strcmp(filename, "-") == 0;
    FILE *out;
    if (use_stdout) {
        out = stdout;
    } else {
        out = fopen(filename, "w");
        if (!out) {
            fprintf(stderr, "couldn't create \"%s\": %s\n", filename, 
                    strerror(errno));
            exit(1);
        }
    }
    if (StaticConfig) 
        fputs(StaticConfig, out);
    if (use_stdout) {
        if (fclose(out)) {
            fprintf(stderr, "I/O error writing \"%s\": %s\n", filename, 
                    strerror(errno));
            exit(1);
        }
    }
}


static void
set_nice(const char *nice_override)
{
    i64 nice_lev = GlobalParams.nice_level;
    if (nice_override && str2num_i64(&nice_lev, nice_override, 10)) {
        fprintf(stderr, "%s: malformed nice level '%s'\n", get_argv0(), 
                nice_override);
        exit(1);
    }
    if ((nice_lev > 20) || (nice_lev < -20)) {
        fprintf(stderr, "%s: illegal nice level '%s'\n", get_argv0(), 
                fmt_i64(nice_lev));
        exit(1);
    }
    renice((int) nice_lev);
}


// Print a command-line arg, weakly trying to make it amenable for copying
// and pasting back to a shell.  No guarantees, though.
static void
print_arg(FILE *out, const char *arg)
{
    int has_single = strchr(arg, '\'') != NULL;
    int has_space = 0;
    for (int ch_idx = 0; arg[ch_idx]; ch_idx++) {
        if (isspace(arg[ch_idx])) { has_space = 1; break; }
    }
    // If the string has embedded single-quotes, things get trickier.
    // We could give up, or we could switch to double-quoting than then
    // start spraying backslashes; instead, we'll stick with single-quoting,
    // and in the uncommon case of an embedded single quote, we'll switch
    // to double-quoting just long enough to emit a single quote.  It's ugly,
    // but it at least survives copy/paste into sh/bash.
    int add_single_quotes = has_space || has_single;
    if (add_single_quotes)
        fputc('\'', out);
    for (const char *ch = arg; *ch != '\0'; ++ch) {
        if (*ch == '\n') {
            fputc(' ', out);    // collapse into same line when printing
        } else if (*ch == '\'') {
            fprintf(out, "'\"'\"'");    // double-quoted single quote
        } else {
            fputc(*ch, out);
        }
    }
    if (add_single_quotes)
        fputc('\'', out);
}


static void
print_startup(int argc, char *argv[])
{
    printf("--Starting up.\n");
    printf("--Command line:");
    for (int i = 0; i < argc; i++) {
        printf(" ");
        print_arg(stdout, argv[i]);
    }
    printf("\n");
    {
        printf("--Host: %s, pid: %i\n", hostname_long(), getpid());
    }
    {
        char *cwd = getcwd(InitialWorkingDirBuf, sizeof(InitialWorkingDirBuf));
        if (!cwd) {
            exit_printf("couldn't get initial working directory: %s\n",
                        strerror(errno));
        }
        InitialWorkingDir = cwd;
        printf("--Working directory: %s\n", InitialWorkingDir);
    }
    {
        time_t now = time(0);
        printf("--Local time: %s", ctime(&now));
        printf("--Unix time: %s\n", fmt_i64(now));
    }
#ifdef DEBUG
    #define LOCAL_HAVE_DEBUG 1
#else
    #define LOCAL_HAVE_DEBUG 0
#endif
#ifdef NDEBUG
    #define LOCAL_HAVE_ASSERT 0
#else
    #define LOCAL_HAVE_ASSERT 1
#endif
    printf("--Note: DEBUG support %s compiled in.  Assertions are %s.\n",
           (LOCAL_HAVE_DEBUG) ? "was" : "not",
           (LOCAL_HAVE_ASSERT) ? "enabled" : "disabled");
#undef LOCAL_HAVE_ASSERT
#undef LOCAL_HAVE_DEBUG
}


static void
do_startup(int argc, char *argv[])
{
    OverallTimer = jtimer_create();
    jtimer_startstop(OverallTimer, 1);
    SimTimer = jtimer_create();

    install_signal_handlers(argv[0], &SignalHandlerActive, &cyc);
    systypes_init();
    set_argv0(argv[0]);

    if (sizeof(double) != sizeof(i64)) {
        fprintf(stderr, "%s: (%s:%i): sorry, sizeof(double) != sizeof(i64), "
                "%i != %i\n", get_argv0(), __FILE__, __LINE__,
                (int) sizeof(double), (int) sizeof(i64));
        sim_abort();
    }
    if (sizeof(reg_u) != sizeof(i64)) {
        fprintf(stderr, "%s: (%s:%i): sorry, sizeof(reg_u) != sizeof(i64), "
                "%i != %i\n", get_argv0(), __FILE__, __LINE__,
                (int) sizeof(reg_u), (int) sizeof(i64));
        sim_abort();
    }

#ifdef __alpha
#include <sys/sysinfo.h>
#include <sys/proc.h>
    /*
    ** In the case of unaligned accesses on alpha machines: suppress
    ** the printing of error messages and cause a SIGBUS signal to be
    ** delivered to the thread.
    */
    int buf[2] = { SSIN_UACPROC, UAC_NOPRINT | UAC_SIGBUS};
    if(setsysinfo((i64)SSI_NVPAIRS, (caddr_t)buf, 1L, 0L, 0L) < 0) {
        printf("setsysinfo failed\n");
        exit(2);
    }
#endif

    print_startup(argc, argv);

    simcfg_init();
    if (StaticConfig) {
        printf("--Loading built-in configuration.\n");
        simcfg_eval_cfg(StaticConfig);
    }
    if (file_readable(ConfigFileName)) {
        simcfg_load_cfg(ConfigFileName);
    }
}



static void 
usage(void)
{
    const char *usage_msg =
"usage: %s [options] argfile1 ... argfileN\n"
"\n"
"options:\n"
" -d -- debug mode; prints a LOT of output\n"
" -systrace -- traces system calls\n"
" -dsync -- debugs synchronization actions\n"
" -confexpr <expr> -- evaluate expression as part of simulator config\n"
" -conffile <file> -- load file on top of simulator config\n"
" -ce / -cf -- shorthand for -confexpr / -conffile\n"
" -confdump <file> -- dump effective config to <file>\n"
" -confdump-builtin <file> -- dump built-in config to \"file\"\n"
" -wu <time> -- executes each thread for a warmup time.  Actually, not true,\n"
"        it executes in multithread mode for T*warmuptime cycles, where T\n"
"        is the number of threads.  Fast-forward doesn't fill caches, etc.,\n"
"        while wu does.\n"
" -nice <N> -- execute at nice level N instead of the config-set default\n"
" -cmp -- set thread->core mapping policy to CMP (one thread per core)\n"
" -contexts <N> -- simulate N contexts\n"
" -cores <N> -- simulate N cores\n"
"\n"
"For options that output to files, using the file name \"-\" sends the\n"
"output to stdout.\n"
        ;
    fprintf(stderr, usage_msg, get_argv0());
    exit(1);
}


int
main(int argc, char *argv[], char *mainenv[])
{
    const char *fname = "main";
    int i = 1, oldi, exitcode;
    int simple_cmp = 0;
    const char *confdump = NULL;
    const char *confdump_builtin = NULL;
    const char *nice_override = NULL;
    const char *contexts_override = NULL;
    const char *cores_override = NULL;

    do_startup(argc, argv);

    /*
    ** Handle command line variables.
    */

    if ((argc - i) > 0) {
        do {
            oldi = i;
            if (!strcmp("--", argv[i])) {
                i++; break;
            } else if (!strncmp("-h", argv[i], 2) ||
                       !strncmp("--h", argv[i], 3)) {
                usage();
            } else if (!strcmp("-systrace", argv[i])) {
                SysTrace = 1; i++;
            } else if (!strcmp("-d", argv[i])) {
#ifdef DEBUG
                debug = 1;
#else
                err_printf("option -d ignored; support not compiled\n");
#endif // DEBUG
                i++;
            } else if (!strcmp("-dsync", argv[i])) {
#ifdef DEBUG
                debugsync = 1; 
#else
                err_printf("option -dsync ignored; support not compiled\n");
#endif // DEBUG
                i++;
            } else if (!strcmp("-wu", argv[i]) && ((i + 1) < argc)) {
                warmup = 1;
                if (str2num_i64(&warmuptime, argv[i + 1], 10) ||
                    (warmuptime < 0)) {
                    fprintf(stderr, "%s: bad warmuptime '%s'\n", get_argv0(),
                            argv[i + 1]);
                    exit(1);
                }
                i += 2;
            } else if ((!strcmp("-confexpr", argv[i]) ||
                        !strcmp("-ce", argv[i])) && ((i + 1) < argc)) {
                simcfg_eval_cfg(argv[i + 1]);
                i += 2;
            } else if ((!strcmp("-conffile", argv[i]) ||
                        !strcmp("-cf", argv[i])) && ((i + 1) < argc)) {
                simcfg_load_cfg(argv[i + 1]);
                i += 2;
            } else if ((strcmp("-confdump", argv[i]) == 0) &&
                       ((i + 1) < argc)) {
                confdump = argv[i + 1];
                i += 2;
            } else if ((strcmp("-confdump-builtin", argv[i]) == 0) &&
                       ((i + 1) < argc)) {
                confdump_builtin = argv[i + 1];
                i += 2;
            } else if ((strcmp("-nice", argv[i]) == 0) && ((i + 1) < argc)) {
                nice_override = argv[i + 1];
                i += 2;
            } else if (!strcmp("-cmp", argv[i])) {
                simple_cmp = 1;
                i++;
            } else if ((strcmp("-contexts", argv[i]) == 0) &&
                       ((i + 1) < argc)) {
                contexts_override = argv[i + 1];
                i += 2;
            } else if ((strcmp("-cores", argv[i]) == 0) &&
                       ((i + 1) < argc)) {
                cores_override = argv[i + 1];
                i += 2;
            } else if (argv[i][0] == '-') {
                fprintf(stderr, "%s: unrecognized/missing argument: %s\n",
                        get_argv0(), argv[i]);
                exit(1);
            } else
                break; /*rest of args should be filenames*/
        } while((argc - i > 0) && (oldi != i));
    }

    if (confdump) {
        simcfg_save_cfg(confdump);
    }
    if (confdump_builtin) {
        save_builtin_conf(confdump_builtin);
    }

    simcfg_sim_params(&GlobalParams);
    if (simple_cmp)
        GlobalParams.thread_core_map.policy = TCP_Cmp;

    set_nice(nice_override);
    if (GlobalParams.disable_coredump)
        disable_coredump();

    if (contexts_override) {
        GlobalParams.num_contexts = atoi(contexts_override);
        if (GlobalParams.num_contexts <= 0) {
            fprintf(stderr, "%s: bad context count '%s'\n", get_argv0(), 
                    contexts_override);
            exit(1);
        }
    }
    if (cores_override) {
        GlobalParams.num_cores = atoi(cores_override);
        if (GlobalParams.num_cores <= 0) {
            fprintf(stderr, "%s: bad core count '%s'\n", get_argv0(), 
                    cores_override);
            exit(1);
        }
    }

    simcfg_init_thread_core_map(&GlobalParams);

    alloc_globals();

    // Zero-filling new memory is important for repeatable simulation results:
    // wrong-path execution which generates valid virtual addresses may read
    // uninitialized memory as returned from the region-allocator, altering
    // wrong-path execution and changing stats non-repeatably.
    if (!(GlobalAlloc = ralloc_create(1))) {
        exit_printf("%s: couldn't create GlobalAlloc region-allocator\n",
                    fname);
    }


    FILE_DevNullIn = (void *) efopen("/dev/null", 0);
    FILE_DevNullOut = (void *)efopen("/dev/null", 1);

    if (!(GlobalEventQueue = callbackq_create())) {
        exit_printf("%s: couldn't create GlobalEventQueue\n", fname);
    }

    {
        AppMgrParams app_mgr_params;
        simcfg_appmgr_params(&app_mgr_params);
        if (!(GlobalAppMgr = appmgr_create(&app_mgr_params))) {
            fprintf(stderr, "%s (%s:%i): couldn't create GlobalAppMgr.\n",
                    get_argv0(), __FILE__, __LINE__);
            exit(1);
        }
    }


    simcfg_bbv_params (&BBTrackerParams);
    
    if (!(GlobalWorkQueue = workq_create("WorkQueue",
                                         GlobalAppMgr, GlobalEventQueue))) {
        exit_printf("%s: couldn't create GlobalWorkQueue.\n", fname);
    }

    if(1)
    {
        initcache();

        init_coher();
        init_contexts();
        init_cores();
        initsched();
        zero_pstats();
       
        // We create the GlobalAdaptMgr here so that it can learn the 
        // core-context mapping 
        if (!(GlobalAdaptMgr = adaptmgr_create())) {
            fprintf(stderr, "%s (%s:%i): couldn't create GlobalAdaptMgr.\n",
                    get_argv0(), __FILE__, __LINE__);
            exit(1);
        }

        for (int app_arg = i; app_arg < argc; app_arg++)
            simcfg_gen_argfile_job(argv[app_arg]);
        simcfg_add_jobs(GlobalWorkQueue);

        appmgr_setup_done(GlobalAppMgr);        // generates sched. callbacks

        if (debug) {
            DEBUGPRINTF("Initial GlobalWorkQueue:\n");
            workq_dump(GlobalWorkQueue, stdout, "  ");
            DEBUGPRINTF("Sim config at end of startup:\n");
            simcfg_save_cfg("-");
        }

        if (!workq_any_unfinished(GlobalWorkQueue)) {
            exit_printf("GlobalWorkQueue is empty: nothing to simulate.\n"
                        "Did you forget some arg-files or WorkQueue "
                        "config files?\n");
        }

        printf("--Startup complete; beginning simulation.\n");
        fflush(0);

        exitcode = run() - 256;

        if(exitcode > 0)
          printf("Program exited with status %d.\n", exitcode);
        exit(exitcode);
    }

    return 0;
}


static CoreParams *
get_core_parms(int core_id)
{
    CoreParams *dest = simcfg_core_params(core_id);
    if (dest) {
        dest->request_bus = SharedCoreRequestBus;
        dest->reply_bus = SharedCoreReplyBus;
        dest->coher_mgr = GlobalCoherMgr;           // May be NULL
        dest->shared_l2cache = SharedL2Cache;       // May be NULL
        dest->shared_l3cache = SharedL3Cache;       // May be NULL
    }
    return dest;
}


static void
create_cores(void)
{
    int core_num;

    sim_assert(CoreCount == 0);

    CoreCount = GlobalParams.num_cores;

    for (core_num = 0; core_num < CoreCount; core_num++) {
        CoreParams *p = get_core_parms(core_num);
        if (!p) {
            exit_printf("couldn't read core parameters for core %d\n",
                        core_num);
        }
        if (!(Cores[core_num] = core_create(core_num, p))) {
            coreparams_destroy(p);
            fprintf(stderr, "%s: %s (%s:%i): failed to create core #%i\n",
                    get_argv0(), __func__, __FILE__, __LINE__, core_num);
            exit(1);
        }
        coreparams_destroy(p);
    }
}


static void
init_cores(void)
{
    int thread;

    create_cores();
    for (thread = 0; thread < CtxCount; thread++) {
        int core_id = GlobalParams.thread_core_map.map[thread];
        core_add_context(Cores[core_id], Contexts[thread]);
        appmgr_register_idle_ctx(GlobalAppMgr, Contexts[thread]);
    }

    printf("Contexts: %i, cores: %i\n", CtxCount, CoreCount);
    for (thread = 0; thread < CtxCount; thread++)
        printf("  context %i on core %i\n", thread, 
               Contexts[thread]->core->core_id);
}


void 
time_stats(void)
{
    i64 total_insts = 0;
    i64 sim_cyc = (cyc - warmupcyc);

    JTimerTimes sim_times, tot_times;
    jtimer_read(SimTimer, &sim_times);
    jtimer_read(OverallTimer, &tot_times);

    for (int i = 0; i < CtxCount; i++) 
        total_insts += Contexts[i]->stats.instrs;

    printf("--Sim'd %#.4g cyc, %#.4g insts in %s sec\n",
           (double) sim_cyc, (double) total_insts, fmt_times(&sim_times));
    printf("--Sim rate: %#.4g cyc/s, %#.4g inst/s\n",
           (double) sim_cyc / (sim_times.user_msec / 1000.0),
           (double) total_insts / (sim_times.user_msec / 1000.0));
    printf("--Total run time: %s sec\n", fmt_times(&tot_times));
}


const char *
fmt_now(void)
{
    return fmt_i64(cyc);
}
