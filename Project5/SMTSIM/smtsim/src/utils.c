//
// Miscellaneous utility functions for SMTSIM
//
// Jeff Brown
// $Id: utils.c,v 1.22.6.11.2.3.2.7 2009/07/29 10:52:56 jbrown Exp $
//

const char RCSid_1211152844[] = 
"$Id: utils.c,v 1.22.6.11.2.3.2.7 2009/07/29 10:52:56 jbrown Exp $";

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "sys-types.h"
#include "sim-assert.h"
#include "utils.h"
// Try to keep simulation-specific #includes (e.g. main.h, context.h) out of
// here; this module is for mostly stand-alone utility code.

// simulation system hostname: these are all set at the first call 
// to hostname_short() or hostname_long()
static const char *HostnameLong = NULL;
static const char *HostnameShort = NULL;

// internal storage for get_argv0() and set_argv0().
static char *InternalArgv0Copy = NULL;


// DEBUGPRINTF() support
#ifdef DEBUG
    // Declared in utils.h
    int debug = 0;
#endif


int
floor_log2(u64 val, int *inexact_ret)
{
    int shifts = 0;
    int inexact = 0;

    sim_assert(val > 0);
    
    while (val != 1) {
        if (val & 1)
            inexact = 1;
        val >>= 1;
        shifts++;
    }

    if (inexact_ret)
        *inexact_ret = inexact;
    return shifts;
}


int 
log2_exact(u64 val)
{
    int result, log_inexact;
    result = floor_log2(val, &log_inexact);
    if (log_inexact)
        result = -1;
    return result;
}


void *
emalloc(size_t size)
{
    void *result;
    sim_assert(size > 0);
    result = malloc(size);
    if (!result) {
        err_printf("%s: out of memory, allocating %li bytes\n", __func__,
                   (long) size);
        exit(1);
    }
    return result;
}


void *
emalloc_zero(size_t size)
{
    void *result = emalloc(size);
    memset(result, 0, size);
    return result;
}


void *
erealloc(void *mem, size_t new_size)
{
    void *result;
    result = realloc(mem, new_size);
    if (!result) {
        err_printf("%s: out of memory, re-alloc'ing %li bytes\n", __func__,
                   (long) new_size);
        exit(1);
    }
    return result;
}


int
file_readable(const char *filename)
{
    FILE *f = fopen(filename, "r");
    if (f)
        fclose(f);
    return f != NULL;
}


void *
efopen(const char *filename, int truncate_not_read)
{
    FILE *result = NULL;
    result = fopen(filename, (truncate_not_read) ? "w" : "r");
    if (!result) {
        fprintf(stderr, "%s: %s (%s:%i): couldn't %s file \"%s\": %s\n",
                get_argv0(), __func__, __FILE__, __LINE__, 
                (truncate_not_read) ? "create" : "read", filename, 
                strerror(errno));
        exit(1);
    }
    return result;
}


void 
efclose(void *file_handle, const char *err_filename)
{
    FILE *f = file_handle;
    if (ferror(f)) {
        fprintf(stderr, "%s: %s (%s:%i): I/O error detected on filehandle for "
                "\"%s\": %s\n", get_argv0(), __func__, __FILE__, __LINE__, 
                err_filename, strerror(errno));
        exit(1);
    }
    if (fclose(f)) {
        fprintf(stderr, "%s: %s (%s:%i): I/O error closing filehandle for "
                "\"%s\": %s\n", get_argv0(), __func__, __FILE__, __LINE__, 
                err_filename, strerror(errno));
        exit(1);
    }
}


void
renice(int level)
{
    int pid = getpid();
    if (setpriority(PRIO_PROCESS, pid, level)) {
        if (0) {
            fprintf(stderr, "%s: setpriority(%i, %i, %i) failed: %s\n", 
                    get_argv0(), PRIO_PROCESS, pid, level, strerror(errno));
        }
    }
}


void
disable_coredump(void)
{
    const char *fname = "disable_coredump";
    struct rlimit lim;
    if (getrlimit(RLIMIT_CORE, &lim)) {
        err_printf("%s: getrlimit failed: %s\n", fname, strerror(errno));
    } else {
        lim.rlim_cur = 0;
        if (setrlimit(RLIMIT_CORE, &lim)) {
            err_printf("%s: setrlimit failed: %s\n", fname, strerror(errno));
        }
    }
}


char *
e_strdup(const char *src)
{
    char *dst = NULL;
    if (src) {
        size_t len = strlen(src);
        dst = malloc(len + 1);
        if (!dst) {
            fprintf(stderr, "%s: %s (%s:%i): out of memory\n",
                    get_argv0(), __func__, __FILE__, __LINE__);
            exit(1);
        }
        memcpy(dst, src, len + 1);
    }
    return dst;
}


char **
strarray_dup_e(char * const *src)
{
    int sz = 0;
    while (src[sz])
        sz++;
    char **ret = emalloc_zero((sz + 1) * sizeof(ret[0]));
    for (int i = 0; i < sz; i++)
        ret[i] = e_strdup(src[i]);
    ret[sz] = NULL;
    return ret;
}


int
strarray_size(char * const *src)
{
    int sz = 0;
    while (src[sz])
        sz++;
    return sz;
}


void 
strarray_free(char **src)
{
    if (src) {
        for (int i = 0; src[i]; i++)
            free(src[i]);
        free(src);
    }
}


int
enum_lookup(const char * const *enum_table, const char *str)
{
    int enum_val = -1;
    for (int i = 0; enum_table[i] != NULL; i++) {
        if (strcmp(enum_table[i], str) == 0) {
            enum_val = i;
            break;
        }
    }
    return enum_val;
}


int
s_t_u_b_abort(const char *file, int line)
{
    fflush(0);
    // break up function name so it doesn't show up in grep (with parens)
    fprintf(stderr, "s" "t" "u" "b"
            "() called from %s:%i; implementation missing.\n",
            file, line);
    sim_abort();
    return 0;
}


int
abort_printf_handle1(const char *file, int line)
{
    const char *short_name = strrchr(get_argv0(), '/');
    short_name = (short_name) ? short_name + 1 : get_argv0();
    fflush(0);
    fprintf(stderr, "%s (%s:%i): ", short_name, file, line);
    return 0;
}


void
abort_printf_handle2(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fflush(0);
    sim_abort();
}


void
exit_printf_handle2(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fflush(0);
    exit(73);           // value picked out of the air
}


void
err_printf_handle2(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fflush(0);
    // Continue
}


i64
ceil_mult_i64(i64 val, i64 mult)
{
    // mult * ceil(val / mult)
    i64 result = mult * ceil((double) val / mult);
    return result;
}


#ifdef NOTDEF
i64 
bill_resource_time(i64 ready_time, i64 now, i64 op_latency)
{
    if (ready_time < now)
        ready_time = now;
    ready_time += op_latency;
    return ready_time;
}
#endif


static void
read_sim_host_name(void)
{
    static char HostnameBufLong[256], HostnameBufShort[256];
    int stat = gethostname(HostnameBufLong, sizeof(HostnameBufLong));
    HostnameBufLong[NELEM(HostnameBufLong)-1] = '\0';
    if (stat == -1) {
        exit_printf("couldn't get hostname\n");
    }
    {
        char *first_dot;
        strcpy(HostnameBufShort, HostnameBufLong);
        if ((first_dot = strchr(HostnameBufShort, '.'))) {
            *first_dot = '\0';
        }
        HostnameLong = &HostnameBufLong[0];
        HostnameShort = &HostnameBufShort[0];
    }
}


const char *
hostname_short(void)
{
    if (!HostnameShort)
        read_sim_host_name();
    sim_assert(HostnameShort != NULL);
    return HostnameShort;
}


const char *
hostname_long(void)
{
    if (!HostnameLong)
        read_sim_host_name();
    sim_assert(HostnameLong != NULL);
    return HostnameLong;
}


const char *
get_argv0(void)
{
    return (InternalArgv0Copy) ? InternalArgv0Copy : "(argv0-UNSET)";
}


void
set_argv0(const char *new_argv0)
{
    if (InternalArgv0Copy) {
        free(InternalArgv0Copy);
        InternalArgv0Copy = NULL;
    }
    InternalArgv0Copy = e_strdup(new_argv0);
}
