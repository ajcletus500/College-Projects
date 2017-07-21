//
// Application execution state
//
// Jeff Brown
// $Id: app-state.cc,v 1.1.2.9.2.1.2.5 2009/12/21 05:44:37 jbrown Exp $
//

const char RCSid_1107455886[] =
"$Id: app-state.cc,v 1.1.2.9.2.1.2.5 2009/12/21 05:44:37 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>

// context.h and main.h are omitted on purpose: simulation-related stuff
// doesn't belong in here
#include "sim-assert.h"
#include "sys-types.h"
#include "app-state.h"
#include "utils.h"
#include "loader.h"
#include "prog-mem.h"
#include "syscalls.h"
#include "stash.h"      // for stash_create() / destroy


typedef std::map<int, AppState*> AppIdMap;


static struct {
    int next_new_id;
    AppIdMap id_to_app;
    AppIdMap::iterator iter;
} GlobalAppInfo;


static int 
mem_err_handler(ProgMem *pmem, mem_addr va, int width, 
                unsigned flags, unsigned err_code,
                void *err_data)
{
    if (err_data) {
        const AppState *as = (const AppState *) err_data;
        fflush(0);
        fprintf(stderr, "%s: illegal memory access, address %s "
                "width %d flags 0x%x, "
                "by A%d inst# %s PC %s, reason: %s\n",
                get_argv0(), fmt_mem(va), width, 
                flags, as->app_id, fmt_i64(as->stats.total_insts),
                fmt_mem(as->npc), pmem_errcode_msg(pmem, err_code));
        fprintf(stderr, "final map:\n");
        pmem_dump_map(pmem, stderr, "  ");
        fflush(0);
        sim_abort();
    }
    return -1;
}


AppParams *
app_params_create(void)
{
    AppParams *n = (AppParams *) emalloc_zero(sizeof(*n));      // Lazy
    return n;
}


AppParams *
app_params_copy(const AppParams *ap)
{
    AppParams *n = app_params_create();
    n->bin_filename = (char *) e_strdup(ap->bin_filename);
    n->initial_working_dir = (char *) e_strdup(ap->initial_working_dir);
    n->argc = ap->argc;
    n->argv = strarray_dup_e(ap->argv);
    n->env = strarray_dup_e(ap->env);
    n->FILE_in = ap->FILE_in;
    n->FILE_out = ap->FILE_out;
    n->FILE_err = ap->FILE_err;
    return n;
}


void
app_params_destroy(AppParams *ap)
{
    if (ap) {
        free(ap->bin_filename);
        free(ap->initial_working_dir);
        strarray_free(ap->argv);
        strarray_free(ap->env);
        // Caller responsible for file handles
        free(ap);
    }
}


void
appstate_destroy(AppState *as)
{
    if (as) {
        appstate_vacate(as);
        if (as->app_id >= 0)
            GlobalAppInfo.id_to_app.erase(as->app_id);
        if (as->params)
            app_params_destroy(as->params);
        free(as);
    }
}


void
appstate_vacate(AppState *as)
{
    sim_assert(as != NULL);
    if (as->stash) {
        if (as->app_id == as->app_master_id)
            stash_destroy(as->stash);
        as->stash = NULL;
    }
    if (as->pmem) {
        pmem_destroy(as->pmem);
        as->pmem = NULL;
    }
    if (as->syscall_state) {
        syscalls_destroy(as->syscall_state);
        as->syscall_state = NULL;
    }
}


int
appstate_is_alive(const AppState *as)
{
    return (as->pmem != NULL);
}


static void
register_global_app(AppState *as)
{
    int new_id = GlobalAppInfo.next_new_id;
    sim_assert(!GlobalAppInfo.id_to_app.count(new_id));
    GlobalAppInfo.id_to_app[new_id] = as;
    as->app_id = new_id;
    GlobalAppInfo.next_new_id++;
}


AppState *
appstate_lookup_id(int app_id)
{
    AppState *result;
    AppIdMap::const_iterator iter = GlobalAppInfo.id_to_app.find(app_id);
    result = (iter != GlobalAppInfo.id_to_app.end()) ? iter->second : 0;
    return result;
}


AppState *
appstate_new_fromfile(const AppParams *params,
                      struct RegionAlloc *r_alloc)
{
    AppState *as = 0;
    int new_id = GlobalAppInfo.next_new_id;
    std::string pmem_name;

    if (!(as = (AppState *) malloc(sizeof(*as)))) {
        fprintf(stderr, "Out of memory allocating AppState\n");
        goto err;
    }
    memset(as, 0, sizeof(*as));
    as->app_id = -1;

    if (!(as->params = app_params_copy(params)))
        goto err;

    // note: if appstate is shared, we should share Stash pointers
    // (master owns id)
    if (!(as->stash = stash_create(as))) {
        fprintf(stderr, "Couldn't create stash (decode cache)\n");
        goto err;
    }

    pmem_name = std::string("A") + fmt_i64(new_id) + ".pmem";
    if (!(as->pmem = pmem_create(pmem_name.c_str(), r_alloc, mem_err_handler,
                                 as))) {
        fprintf(stderr, "Couldn't create program memory manager\n");
        goto err;
    }

    SyscallStateParams sys_params;
    sys_params.FILE_stdin = as->params->FILE_in;
    sys_params.FILE_stdout = as->params->FILE_out;
    sys_params.FILE_stderr = as->params->FILE_err;
    sys_params.initial_working_dir = as->params->initial_working_dir;

    if (!(as->syscall_state = syscalls_create(&sys_params, as))) {
        fprintf(stderr, "Couldn't create syscall state\n");
        goto err;
    }

    if (loader_read_auto(as, as->params->bin_filename) < 0) {
        fprintf(stderr, "Couldn't load input file \"%s\".\n", 
                as->params->bin_filename);
        goto err;
    }   
    loader_init_main_entry(as, as->params->argc, 
                           as->params->argv, as->params->env);

    register_global_app(as);
    as->app_master_id = as->app_id;
    sim_assert(as->app_id >= 0);
    return as;

err:
    fprintf(stderr, "AppState creation from file \"%s\" failed "
            "(would've been app %d)\n", params->bin_filename, new_id);
    appstate_destroy(as);
    return 0;
}


int
appstate_count(void)
{
    return static_cast<int>(GlobalAppInfo.id_to_app.size());
}


void
appstate_global_iter_reset(void)
{
    GlobalAppInfo.iter = GlobalAppInfo.id_to_app.begin();
}


AppState *
appstate_global_iter_next(void)
{
    AppState *result = NULL;
    if (GlobalAppInfo.iter != GlobalAppInfo.id_to_app.end()) {
        result = GlobalAppInfo.iter->second;
        ++GlobalAppInfo.iter;
    }
    return result;
}


void
appstate_dump_regs(const struct AppState *as, void *FILE_out,
                   const char *prefix)
{
    FILE *out = (FILE *) FILE_out;
    for (int i = 0; i < 32; i++)
        fprintf(out, "%sR[%d].i = 0x%s\n", prefix, i, fmt_x64(as->R[i].i));
    for (int i = 32; i < 64; i++)
        fprintf(out, "%sR[%d].f = %.15g\n", prefix, i, as->R[i].f);
    for (int i = 64; i < MAXREG; i++)
        fprintf(out, "%sR[%d].i = 0x%s\n", prefix, i, fmt_x64(as->R[i].i));
}


void
appstate_dump_mem(const struct AppState *as, void *FILE_out, 
                  mem_addr start, i64 len, const char *prefix)
{
    FILE *out = (FILE *) FILE_out;
    mem_addr addr = start, limit = start + len;
    struct {
        unsigned byte;
        int state;              // <0: not requested, 0: unreadable, >0: valid
    } row_data[16];

    while (addr < limit) {
        mem_addr row_start_addr = addr;

        for (int col = 0; col < NELEM(row_data); col++) {
            if (addr >= limit) {
                row_data[col].byte = 0;
                row_data[col].state = -1;
            } else if (pmem_access_ok(as->pmem, addr, 1, PMAF_R)) {
                row_data[col].byte = 
                    pmem_read_8(as->pmem, addr, PMAF_NoExcept);
                row_data[col].state = 1;
            } else {
                row_data[col].byte = 0;
                row_data[col].state = 0;
            }
            addr++;
        }

        fprintf(out, "%s", prefix);

        {
            const char *addr_str = fmt_x64(row_start_addr);
            for (int i = strlen(addr_str); i < 8; i++)
                putc('0', out);
            fprintf(out, "%s", addr_str);
        }

        for (int col = 0; col < NELEM(row_data); col++) {
            if ((col % 8) == 0)
                putc(' ', out);
            if (row_data[col].state > 0) {
                fprintf(out, " %02x", row_data[col].byte);
            } else if (row_data[col].state == 0) {
                fprintf(out, " ??");
            } else {
                fprintf(out, "   ");
            }
        }           

        fprintf(out, "  |");
        for (int col = 0; col < NELEM(row_data); col++) {
            unsigned out_char = '?';
            if (row_data[col].state > 0) {
                out_char = row_data[col].byte;
                if (!isprint(out_char))
                    out_char = '.';
            } else if (row_data[col].state < 0) {
                break;
            }
            putc(out_char, out);
        }
        fprintf(out, "|\n");
    }
}
