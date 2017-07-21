//
// Simulator-specific assert/abort handling
//
// Jeff Brown
// $Id: sim-assert.h,v 1.1.2.4.8.1 2009/07/29 10:52:55 jbrown Exp $
//

#ifndef SIM_ASSERT_H
#define SIM_ASSERT_H

#include "sys-types.h"  // for "i64" typedef


#ifdef __cplusplus
extern "C" {
#endif

// We'll use these simulator-specific definitions in place of C assert() /
// abort() invocations, to allow us to catch them for diagnostics before a
// signal handler gets invoked.  These aren't necessarily used in every single
// piece of source code, particularly not in code which is maintained 
// seperately and imported from time to time.

void sim_abort(void);

// Does not return
int sim_assert_failed(const char *file, long line, const char *assertion);


#ifndef NDEBUG
#   define sim_assert(x) \
    ((void) (SP_F(!(x)) ? sim_assert_failed(__FILE__, __LINE__, #x) : 0))
#else
#   define sim_assert(x) ((void) 0)
#endif

// Assert "p implies q".  It's a simple enough equivalence, this is just for
// source clarity.
#define assert_ifthen(p, q) sim_assert(!(p) || (q))


// Install signal handlers to catch fatal signals delivered to the simulator,
// and hopefully do something useful with them before exiting.
void install_signal_handlers(const char *argv0_or_null,
                             int *signal_active_flag_or_null,
                             i64 *cycle_counter_or_null);


#ifdef __cplusplus
}
#endif

#endif  // SIM_ASSERT_H
