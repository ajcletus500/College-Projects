//
// Hack to dump some sort of stack backtrace
//
// Copyright (c) 2005 Jeff Brown
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice this list of conditions, and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Absolutely no warranty of function or purpose is made by the author.
//
// $JAB-Id: stack-trace.c,v 1.10 2006/03/09 07:38:24 jabrown Exp $
//

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "stack-trace.h"

const char RCSid_1116994251[] =
"$JAB-Id: stack-trace.c,v 1.10 2006/03/09 07:38:24 jabrown Exp $";


#ifdef __FreeBSD__
    #include <sys/param.h>
    #if (__FreeBSD_version) < 490000
        // This was fun to track down; FreeBSD PR kern/35175
        #define BROKEN_FREEBSD_PTRACE_SIGNAL
    #endif
#endif


int
stack_trace_implemented(void)
{
    static int result = -1;
    if (result >= 0)
        return result;
    result = 1;

#ifdef BROKEN_FREEBSD_PTRACE_SIGNAL
    result = 0;
#endif

    if (result) {
        if (system("sh -c 'gdb -batch' >/dev/null 2>&1") != 0)
            result = 0;
        if (system("sh -c 'awk \"\"' >/dev/null 2>&1") != 0)
            result = 0;
    }

    return result;
}


int
stack_trace_dump(void *c_FILE_out, const char *exe_name, int full_trace)
{
    const char *func = "stack_trace_dump";
    char cmd_fname[] = "/tmp/stacktrace-gdb-XXXXXX";
    int cmd_file_formed = 0;
    int parent_pid = getpid();
    char cmd_buf[1024];

    if (!stack_trace_implemented()) {
        fprintf(stderr, "%s: sorry, stack trace not implemented "
                "in this binary\n", func);
        goto fail;
    }

    {
        FILE *read_test = fopen(exe_name, "r");
        if (!read_test) {
            fprintf(stderr, "%s: couldn't open executable: \"%s\": %s\n",
                    func, exe_name, strerror(errno));
            goto fail;
        }
        fclose(read_test);
    }

    // It'd be nice if we could just pipe commands to gdb rather than
    // place them in a file; gdb is uncooperative, here.

    {
        int cmd_fd = mkstemp(cmd_fname);
        FILE *cmd_file;
        if (cmd_fd < 0) {
            fprintf(stderr, "%s: couldn't create temp file \"%s\": "
                    "%s\n", func, cmd_fname, strerror(errno));
            goto fail;
        }
        cmd_file_formed = 1;
        cmd_file = fdopen(cmd_fd, "w");
        if (!cmd_file) {
            fprintf(stderr, "%s: couldn't fdopen temp file: %s\n",
                    func, strerror(errno));
            goto fail;
        }
        fprintf(cmd_file,
                "set height 0\n"
                "set width 0\n"
                "file \"%s\"\n"
                "attach %d\n"
                "bt %s\n"
                "detach\n"
                , exe_name, parent_pid, (full_trace) ? "full" : "");
        fflush(cmd_file);
        if (ferror(cmd_file)) {
            fprintf(stderr, "%s: I/O error writing command file: %s\n",
                    func, strerror(errno));
            goto fail;
        }
        fclose(cmd_file);
    }

    {    
        const char *cmd_fmt = "gdb -batch -x \"%s\" 2>/dev/null | awk '%s'";

        // The cleanup script makes the input less spammy, at the cost of
        // eliminating the PC-only backtrace of a stripped binary.  The
        // backtrace of a stripped binary doesn't tell us much useful anyway,
        // though.
        const char *cleanup_awk_script =
            "on_next && /^#[0-9]+ / { on=1 } "
            "/^#[0-9]+ +0x[0-9a-f]+ in stack_trace_dump \\(/ { on_next=1 } "
            "/^No (symbol table info|locals)/ { next }"
            "on { if (sub(/^#[0-9]+ +/, \"#\" (0+out_fr) \" \")) { ++out_fr } "
            "      print; }";

        int cmd_maxlen = strlen(cmd_fmt) + strlen(cmd_fname) +
            strlen(cleanup_awk_script);
        if (cmd_maxlen >= sizeof(cmd_buf)) {
            fprintf(stderr, "%s: command buffer overflow\n", func);
            goto fail;
        }
        sprintf(cmd_buf, cmd_fmt, cmd_fname, cleanup_awk_script);
    }

    {
        FILE *out_file = (FILE *) c_FILE_out;
        int out_fd = fileno(out_file);
        int child_pid;
        fflush(0);
        child_pid = fork();
        if (child_pid < 0) {
            fprintf(stderr, "%s: couldn't fork: %s\n", func, strerror(errno));
            goto fail;
        } else if (child_pid > 0) {
            // parent
            //
            // Hack: force parent to wait just a bit, in the hopes that the
            // child's gdb will catch us before we enter waitpid.  One day,
            // for some reason, gdb in the linux started acting funny
            // and not showing a useful call stack from within waitpid?
            sleep(1);
            int wait_stat, exit_status;
            int ret_pid = waitpid(child_pid, &wait_stat, 0);
            if (ret_pid < 0) {
                fprintf(stderr, "%s: waitpid failed: %s\n", func,
                        strerror(errno));
                goto fail;
            }
            if (!WIFEXITED(wait_stat)) {
                fprintf(stderr, "%s: child died!\n", func);
                goto fail;
            }
            exit_status = WEXITSTATUS(wait_stat);
            if (exit_status != 0) {
                fprintf(stderr, "%s: child returned exit status %d\n",
                        func, exit_status);
                goto fail;
            }
        } else {
            // child
            int status;
            if (dup2(out_fd, 1) == -1) {
                fprintf(stderr, "%s: dup2(stdout) failed: %s\n",
                        func, strerror(errno));
                exit(1);
            }
            if (dup2(out_fd, 2) == -1) {
                fprintf(stderr, "%s: dup2(stderr) failed: %s\n",
                        func, strerror(errno));
                exit(1);
            }
            fflush(0);
            status = system(cmd_buf);
            fflush(0);
            exit(status);
        }
    }

    if (cmd_file_formed)
        unlink(cmd_fname);
    fflush(0);
    return 0;

fail:
    if (cmd_file_formed)
        unlink(cmd_fname);
    fflush(0);
    return -1;
}
