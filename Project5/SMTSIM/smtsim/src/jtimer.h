/*
 * Library for measuring how long things take
 *
 * Jeff Brown
 * $Id: jtimer.h,v 1.1 2003/03/12 06:12:37 jbrown Exp $
 */

#ifndef JTIMER_H
#define JTIMER_H

#ifdef __cplusplus
extern "C" {
#endif

// The "J" prefix on everything is to prevent a conflict with some timer_*
// stuff Linux has in its header files.
typedef struct JTimer JTimer;


typedef struct JTimerTimes  {
    i64 user_msec;
    i64 sys_msec;
    i64 real_msec;
} JTimerTimes;


// Create a timer (and reset it).
JTimer *jtimer_create(void);

void jtimer_destroy(JTimer *t);

// Reset a timer to zeros, and stopped (not running)
void jtimer_reset(JTimer *t);

// Start or stop a timer.  start_not_stop controls whether the timer is
// started or stopped.  If the timer was already started/stopped, this has no
// effect. This returns a flag indicating if the timer was running.
int jtimer_startstop(JTimer *t, int start_not_stop);

int jtimer_running(const JTimer *t);

// Read the current time values.  The timer may be running.
void jtimer_read(const JTimer *t, JTimerTimes *result_ret);


// Format the times for printing in seconds, returning a pointer to a 
// static buffer.
const char *fmt_times(const JTimerTimes *times);


#ifdef __cplusplus
}
#endif

#endif  /* JTIMER_H */
