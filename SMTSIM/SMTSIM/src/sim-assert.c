//
// Simulator-specific assert/abort handling
//
// Jeff Brown
// $Id: sim-assert.c,v 1.1.2.2.2.2.2.4 2009/11/17 23:58:51 jbrown Exp $
//

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys-types.h"
#include "sim-assert.h"
#include "stack-trace.h"


// Private state for communication from install_signal_handlers()
// to fatal_sig_handler().
static int HaveStackTrace = 0;
static const char *GlobalExeNameOrNull = NULL;
static i64 *GlobalCycOrNull = NULL;
static int *GlobalSignalActiveFlagOrNull = NULL;


static void
fatal_sig_handler(int signum)
{
    // avoid calls to other simulator functions, as they may re-trigger this
    signal(signum, SIG_DFL);
    fflush(0);
    // Casting "cyc" to a double here in order to use built-in type
    // conversion, rather than relying on fmt_i64() and friends, or on
    // specific printf format strings.  We'll add the trailing decimal as a
    // reminder that it's undergone conversion, and hence doesn't have all 64
    // bits of precision available.
    fprintf(stderr, "**** fatal signal %d at cyc ", signum);
    if (GlobalCycOrNull != NULL) {
        fprintf(stderr, "%.1f", (double) *GlobalCycOrNull);
    } else {
        fprintf(stderr, "(unknown)");
    }
    fprintf(stderr, " ****\n"); 
    if (HaveStackTrace) {
        if (GlobalSignalActiveFlagOrNull)
            *GlobalSignalActiveFlagOrNull = 1;
        fprintf(stderr, "**** stack trace start ****\n");
        // The full stack dumps can get crazy, when C++ is involved.
        stack_trace_dump(stderr, GlobalExeNameOrNull, 0);
        fprintf(stderr, "**** stack trace end ****\n");
    } else {
        fprintf(stderr, "**** (stack backtrace not enabled) ****\n");
    }
    fflush(0);
    raise(signum);
}


void
install_signal_handlers(const char *argv0_or_null,
                        int *signal_active_flag_or_null,
                        i64 *cycle_counter_or_null)
{
    GlobalExeNameOrNull = argv0_or_null;
    GlobalCycOrNull = cycle_counter_or_null;
    GlobalSignalActiveFlagOrNull = signal_active_flag_or_null;
    HaveStackTrace = (GlobalExeNameOrNull != NULL) &&
        stack_trace_implemented();
    signal(SIGFPE, SIG_IGN);
    signal(SIGABRT, fatal_sig_handler);
    signal(SIGSEGV, fatal_sig_handler);
}


void
sim_abort(void)
{  
    fflush(0);
    fatal_sig_handler(SIGABRT);
}


int
sim_assert_failed(const char *file, long line, const char *assertion)
{
    const char *short_name;
    if (GlobalExeNameOrNull != NULL) {
        short_name = strrchr(GlobalExeNameOrNull, '/');
        short_name = (short_name) ? short_name + 1 : GlobalExeNameOrNull;
    } else {
        short_name = "(GlobalExeName-not-set)";
    }
    fflush(0);
    fprintf(stderr, "%s (%s:%ld): assertion failed: %s\n", short_name, file, 
            line, assertion);
    sim_abort();
    abort();
    exit(1);
    return -1;
}
