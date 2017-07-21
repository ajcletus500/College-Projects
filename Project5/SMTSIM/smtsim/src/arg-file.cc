//
// Argument file parser
//
// Jeff Brown
// $Id: arg-file.cc,v 1.1.2.3.2.1.2.1 2009/07/29 10:52:49 jbrown Exp $
//

const char RCSid_1107565649[] =
"$Id: arg-file.cc,v 1.1.2.3.2.1.2.1 2009/07/29 10:52:49 jbrown Exp $";

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "arg-file.h"
#include "utils.h"
#include "simple-pre.h"

using std::vector;


static const SimpleRESubstr NullSubstr = { -1, 0 };


// Hack: load file into memory.  Assumes file is "small".  Exits on error.
static char *
e_slurp_file(const char *filename)
{
    static const char *fname = "slurp_file";
    int buf_size = 10*1024;
    char *buf = (char *) emalloc_zero(buf_size);
    FILE *in = (FILE *) efopen(filename, 0);
    size_t rsize = fread(buf, 1, buf_size, in);
    if (rsize == (size_t) buf_size) {
        fprintf(stderr, "%s (%s:%i): file \"%s\" is too large to fit in "
                "wimpy static buffer, sorry.\n", fname, __FILE__, __LINE__,
                filename);
        exit(1);
    }
    if (memchr(buf, '\0', rsize)) {
        fprintf(stderr, "%s (%s:%i): file \"%s\" contains an embedded NUL "
                "which this wimpy code doesn't handle, sorry.\n", fname,
                __FILE__, __LINE__, filename);
        exit(1);
    }
    buf[rsize] = '\0';
    efclose(in, filename);
    return buf;
}


#if 0
static void
parse_redirs(const char *cmd, char **in_ret, char **out_ret)
{
    const char *in_pat = 
        "^[^<]+([[:space:]]<[[:space:]]+([^<[:space:]]+))[^<]*$";
    const char *out_pat = 
        "^[^>]+([[:space:]]>[[:space:]]+([^>[:space:]]+))[^>]*$";
    int match;
    SimpleRESubstr cap[4];

    *in_ret = *out_ret = 0;

    if ((match = simple_pre_fixcap(in_pat, cmd, 0, cap, NELEM(cap))) < 0)
        exit(1);
    if (match) {
        *in_ret = emalloc_zero(cap[2].len + 1);
        memcpy(in_ret, cmd + cap[2].st, cap[2].len);
    }

    if ((match = simple_pre_fixcap(out_pat, cmd, 0, cap, NELEM(cap))) < 0)
        exit(1);
    if (match) {
        *out_ret = emalloc_zero(cap[2].len + 1);
        memcpy(out_ret, cmd + cap[2].st, cap[2].len);
    }
}
#endif


static int
split_cmd(const char *txt, const SimpleRESubstr cmd,
          vector<SimpleRESubstr>& argv, SimpleRESubstr& r_in,
          SimpleRESubstr& r_out)
{
    const char *in_pat = "^<[[:space:]]*([^<>[:space:]]+)([[:space:]]|$)";
    const char *out_pat = "^>[[:space:]]*([^<>[:space:]]+)([[:space:]]|$)";
    const char *arg_pat = "^([^<>[:space:]]+)([[:space:]]|$)";
    SimpleRESubstr cap[10];
    r_in = NullSubstr;
    r_out = NullSubstr;

    int offs = cmd.st;
    while (offs < (cmd.st + cmd.len)) {
        if (isspace(txt[offs])) {
            offs++;
            continue;
        }
        int m = 0;
        if ((m = simple_pre_fixcap(in_pat, txt + offs, 0, cap, NELEM(cap)))) {
            if (m < 0) exit(1);
            cap[1].st += offs;
            r_in = cap[1];
        } else if ((m = simple_pre_fixcap(out_pat, txt + offs, 0, cap,
                                          NELEM(cap)))) {
            if (m < 0) exit(1);
            cap[1].st += offs;
            r_out = cap[1];
        } else if ((m = simple_pre_fixcap(arg_pat, txt + offs, 0, cap,
                                          NELEM(cap)))) {
            if (m < 0) exit(1);
            cap[1].st += offs;
            argv.push_back(cap[1]);
        }
        if (m) {
            offs += cap[0].st + cap[0].len;
        } else {
            // Non-matching stuff in command line
            fprintf(stderr, "%s (%s:%i): error parsing argfile command line.  "
                    "Unmatched: \"%.*s\"\n", get_argv0(), __FILE__, __LINE__,
                    (cmd.st + cmd.len) - offs, txt + offs);
            return -1;
        }
    }
    return 0;
}


static char *
dup_substr(const char *txt, const SimpleRESubstr& sub)
{
    sim_assert((sub.st >= 0) && (sub.len >= 0));
    char *result = (char *) emalloc_zero(sub.len + 1);
    memcpy(result, txt + sub.st, sub.len);
    result[sub.len] = '\0';
    return result;
}


static int
substr2num_i64(i64 *result, const char *txt, const SimpleRESubstr& sub,
               int base)
{
    int ret;
    char *num = dup_substr(txt, sub);   // Lazy
    ret = str2num_i64(result, num, base);
    free(num);
    return ret;
}


// 0: ok  1: no match  2: match, but format error
static int
try_v1(ArgFileInfo *dst, const char *txt)
{
    // ffdist # numthreads cmd arg1 ... argN < input > output
    const char *pat = 
        "^[[:space:]]*(([0-9]+)[[:space:]]+)?#[[:space:]]+"
        "([0-9]+)[[:space:]]+([^#]+)(#.*)?$";
    int match;
    SimpleRESubstr cap[10], redir_in, redir_out;
    vector<SimpleRESubstr> argv;

    if ((match = simple_pre_fixcap(pat, txt, 0, cap, NELEM(cap))) < 0)
        exit(1);
    if (!match)
        goto no_match;

    memset(dst, 0, sizeof(*dst));

    sim_assert(cap[3].st >= 0);
    sim_assert(cap[4].st >= 0);

    if (cap[2].st >= 0) {
        if (substr2num_i64(&dst->ff_dist, txt, cap[2], 10) ||
            (dst->ff_dist < 0)) {
            fprintf(stderr, "%s: (%s:%i): bad ffdist '%.*s'\n", get_argv0(),
                    __FILE__, __LINE__, cap[2].len, txt+cap[2].st);
            goto fmt_err;
        }
    } else {
        dst->ff_dist = 0;
    }

    {
        i64 val;
        if (substr2num_i64(&val, txt, cap[3], 10) || (val <= 0)) {
            fprintf(stderr, "%s: (%s:%i): bad thread-count '%.*s'\n",
                    get_argv0(), __FILE__, __LINE__, cap[3].len,
                    txt+cap[3].st);
            goto fmt_err;
        }
        dst->num_threads = i64_to_int(val);
    }    
    
    if (split_cmd(txt, cap[4], argv, redir_in, redir_out))
        goto fmt_err;

    dst->argv = (char **) emalloc_zero((argv.size() + 1) * 
                                       sizeof(dst->argv[0]));
    for (int i = 0; i < (int) argv.size(); i++) {
        dst->argv[i] = dup_substr(txt, argv[i]);
    }
    dst->argv[argv.size()] = 0;

    dst->redir_name.in = 0;
    dst->redir_name.out = 0;
    if (redir_in.st >= 0)
        dst->redir_name.in = dup_substr(txt, redir_in);
    if (redir_out.st >= 0)
        dst->redir_name.out = dup_substr(txt, redir_out);

    return 0;

no_match:
    return 1;

fmt_err:
    return 2;
}


ArgFileInfo *
argfile_load(const char *filename)
{
    ArgFileInfo *result = (ArgFileInfo *) emalloc_zero(sizeof(*result));
    char *file_txt = e_slurp_file(filename);
    int matched = 0;

    if (!matched) {
        int stat = try_v1(result, file_txt);
        if (stat == 0) matched = 1;
        if (stat == 2) goto err;
    }

    if (!matched) goto err;

    free(file_txt);
    return result;

err:
    free(file_txt);
    free(result);
    return 0;
}


void 
argfile_destroy(ArgFileInfo *af)
{
    if (af) {
        strarray_free(af->argv);
        free(af->redir_name.in);
        free(af->redir_name.out);
        free(af);
    }
}


void
argfile_dump(void *FILE_out, const ArgFileInfo *af, const char *pref)
{
    FILE *out = (FILE *) FILE_out;
    fprintf(out, "%sff_dist: %s\n", pref, fmt_i64(af->ff_dist));
    for (int i = 0; af->argv[i]; i++)
        fprintf(out, "%sargv[%i]: \"%s\"\n", pref, i, af->argv[i]);
    fprintf(out, "%sredir_name\n", pref);
    fprintf(out, "%s  in: \"%s\"\n", pref, af->redir_name.in);
    fprintf(out, "%s  out: \"%s\"\n", pref, af->redir_name.out);
}
