/*
 * Library for measuring how long things take
 *
 * Jeff Brown
 * $Id: jtimer.c,v 1.3.14.1 2008/04/30 22:17:50 jbrown Exp $
 */

const char RCSid_1047433847[] =
"$Id: jtimer.c,v 1.3.14.1 2008/04/30 22:17:50 jbrown Exp $";

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>

#include "sys-types.h"
#include "jtimer.h"
#include "utils.h"


typedef struct TimePoint {
    struct timeval tv;
    struct rusage ru;
} TimePoint;


struct JTimer {
    JTimerTimes stopped_accum;
    int running;
    TimePoint last_start;
};


static void
get_time_point(TimePoint *tp)
{
    if (gettimeofday(&tp->tv, NULL))
        fprintf(stderr, "%s (%s:%i): gettimeofday failed: %s\n", __func__,
                __FILE__, __LINE__, strerror(errno));
    if (getrusage(RUSAGE_SELF, &tp->ru))
        fprintf(stderr, "%s (%s:%i): getrusage failed: %s\n", __func__,
                __FILE__, __LINE__, strerror(errno));
}


// Computes tv2 - tv1, in msec
#define TV_DELTA_MSEC(tv2, tv1) \
        (1000 * ((i64) (tv2.tv_sec - tv1.tv_sec)) + \
         (tv2.tv_usec - tv1.tv_usec) / 1000)


// Add the elapsed times for the current running timer (since it was last
// started) to the given JTimerTimes.
static void
add_runtime(const JTimer *t, JTimerTimes *accum)
{
    TimePoint now;

    assert(t->running);
    get_time_point(&now);

    i64 user_delta = TV_DELTA_MSEC(now.ru.ru_utime, t->last_start.ru.ru_utime);
    if (user_delta >= 0)
        accum->user_msec += user_delta;

    i64 sys_delta = TV_DELTA_MSEC(now.ru.ru_stime, t->last_start.ru.ru_stime);
    if (sys_delta >= 0)
        accum->sys_msec += sys_delta;

    i64 real_delta = TV_DELTA_MSEC(now.tv, t->last_start.tv);
    if (real_delta >= 0)
        accum->real_msec += real_delta;
}


JTimer *
jtimer_create(void)
{
    JTimer *t = emalloc_zero(sizeof(*t));
    jtimer_reset(t);
    return t;
}


void 
jtimer_destroy(JTimer *t)
{
    if (t)
        free(t);
}


void 
jtimer_reset(JTimer *t)
{
    t->stopped_accum.user_msec = 0;
    t->stopped_accum.sys_msec = 0;
    t->stopped_accum.real_msec = 0;
    t->running = 0;
}


int 
jtimer_startstop(JTimer *t, int start_not_stop)
{
    int old_running = t->running;
    
    if (start_not_stop != t->running) {
        if (start_not_stop) 
            get_time_point(&t->last_start);
        else 
            add_runtime(t, &t->stopped_accum);
        t->running = start_not_stop;
    }

    return old_running;
}


int 
jtimer_running(const JTimer *t)
{
    return t->running;
}


void 
jtimer_read(const JTimer *t, JTimerTimes *result_ret)
{
    *result_ret = t->stopped_accum;
    if (t->running)
        add_runtime(t, result_ret);
}


const char *
fmt_times(const JTimerTimes *times)
{
    static char buf[80];
    double pct = (100.0 * (times->user_msec + times->sys_msec)) /
        times->real_msec;
    e_snprintf(buf,  sizeof(buf), "%#.4g/%#.4g/%#.4g(%#.4g%%)",
               times->user_msec / 1000.0,
               times->sys_msec / 1000.0,
               times->real_msec / 1000.0,
               pct);
    return buf;
}
