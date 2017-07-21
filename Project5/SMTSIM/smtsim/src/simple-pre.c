/*
 * POSIX regexp wrapper: extended regular expression matching, slow but simple
 *
 * Jeff Brown
 * $JAB-Id: simple-pre.c,v 1.1 2005/02/07 07:35:45 jabrown Exp $
 */

const char RCSid_1107681538[] = "$JAB-Id: simple-pre.c,v 1.1 2005/02/07 07:35:45 jabrown Exp $";

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Includes for POSIX regex library */
#include <sys/types.h>
#include <regex.h>

#include "simple-pre.h"


/*
 * Maximum number of sub-expressions to allow.  While this is crufty to do
 * in general, this should easily cover the simpler uses we're targeting.
 */
#define MAX_SUBEXPS     100

#define REG_ERR_BUF     128

#define NELEM(x) (sizeof(x)/sizeof((x)[0]))


static int
map_options(int enum_opts, int is_match_opts)
{
    int result = 0;
    if (!is_match_opts) {
        /* Compile options */
        if (enum_opts & SPRE_Caseless)
            result |= REG_ICASE;
        if (enum_opts & SPRE_MultiLine)
            result |= REG_NEWLINE;
    } else {
        /* Match options */
        if (enum_opts & SPRE_NotBOL)
            result |= REG_NOTBOL;
        if (enum_opts & SPRE_NotEOL)
            result |= REG_NOTEOL;
    }

    return result;
}


static int
simple_pre(const char *pattern, const char *text, int options, 
           SimpleRESubstr **capture_array, int *capture_size)
{
    const char *fname = "simple_pre";
    regex_t re;
    int num_subexps = -1;
    regmatch_t sub_matches[MAX_SUBEXPS + 1];
    int re_compile_opts, re_match_opts;
    int re_stat;
    int re_compiled = 0;

    re_compile_opts = map_options(options, 0);
    re_compile_opts |= REG_EXTENDED;
    re_match_opts = map_options(options, 1);

    re_stat = regcomp(&re, pattern, re_compile_opts);
    if (re_stat != 0) {
        char err[REG_ERR_BUF];
        regerror(re_stat, &re, err, sizeof(err));
        fprintf(stderr, "%s (%s:%d): regex compile failed on "
                "\"%s\": %s\n", fname, __FILE__, __LINE__,
                pattern, err);
        goto err;
    }
    re_compiled = 1;

    num_subexps = re.re_nsub;
    assert(num_subexps >= 0);
    if (num_subexps > MAX_SUBEXPS) {
        fprintf(stderr, "%s (%s:%d): pattern has too many subexps, %d > %d\n",
                fname, __FILE__, __LINE__, num_subexps, MAX_SUBEXPS);
        goto err;
    }

    re_stat = regexec(&re, text, num_subexps + 1, sub_matches, re_match_opts);

    if ((re_stat != 0) && (re_stat != REG_NOMATCH)) {
        char err[REG_ERR_BUF];
        regerror(re_stat, &re, err, sizeof(err));
        fprintf(stderr, "%s (%s:%d): regexec failed: %s\n",
                fname, __FILE__, __LINE__, err);
        goto err;
    }

    if ((re_stat == 0) && capture_array) {
        int idx_limit = num_subexps + 1;
        int i;
        assert(capture_size != NULL);
        if (!*capture_array) {
            *capture_array = malloc(idx_limit * sizeof(*capture_array));
            if (!*capture_array) {
                fprintf(stderr, "%s (%s:%d): out of memory malloc'ing capture "
                        "array\n", fname, __FILE__, __LINE__);
                goto err;
            }
            *capture_size = idx_limit;
        } else if (idx_limit > *capture_size) {
            idx_limit = *capture_size;
        }
        for (i = 0; i < idx_limit; i++) {
            (*capture_array)[i].st = sub_matches[i].rm_so;
            (*capture_array)[i].len = sub_matches[i].rm_eo - 
                sub_matches[i].rm_so;
        }
    }

    regfree(&re);

    return (re_stat != 0) ? 0 : (num_subexps + 1);

err:
    if (re_compiled)
        regfree(&re);
    return -1;
}


int
simple_pre_nocap(const char *pattern, const char *text, int options)
{
    int match_exps = simple_pre(pattern, text, options, NULL, NULL);
    return match_exps;
}


int
simple_pre_fixcap(const char *pattern, const char *text, int options, 
                  SimpleRESubstr *capture_array, int capture_size)
{
    SimpleRESubstr *cap_loc = capture_array;
    int cap_size = capture_size;
    int match_exps;
    if (!capture_array)
        abort();
    match_exps = simple_pre(pattern, text, options, &cap_loc, &cap_size);
    assert(cap_size == capture_size);
    assert(cap_loc == capture_array);
    return match_exps;
}


int
simple_pre_dyncap(const char *pattern, const char *text, int options, 
                  SimpleRESubstr **capture_ret, int *cap_size_ret)
{
    SimpleRESubstr *cap_loc = NULL;
    int cap_size = 0;
    int match_exps;
    if (!capture_ret || !cap_size_ret)
        abort();
    match_exps = simple_pre(pattern, text, options, &cap_loc, &cap_size);
    *capture_ret = cap_loc;
    *cap_size_ret = cap_size;
    return match_exps;
}
