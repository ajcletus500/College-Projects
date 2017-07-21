//
// Loader: loads programs and sets up memory maps.
//
// Jeff Brown
// $Id: loader.cc,v 1.1.2.12.2.1.2.8 2009/12/21 05:44:38 jbrown Exp $
//

const char RCSid_1064945843[] =
"$Id: loader.cc,v 1.1.2.12.2.1.2.8 2009/12/21 05:44:38 jbrown Exp $";

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "loader.h"
#include "app-state.h"
#include "utils.h"
#include "mem.h"
#include "prog-mem.h"
#include "sim-params.h"

#define LOADER_PRIVATE_MAIN_MODULE
#include "loader-private.h"


// If set, the goal here is to match the in-memory stack layout and placement
// with the old simulator, for performance comparison.  This is -not-
// well-tested, and is intended for single-thread testing only.
#define EMULATE_20050120_STACK_INIT     0

#define USE_BIG_EMPTY_STRING_HACK       1


#if EMULATE_20050120_STACK_INIT
  #define STACK_TEXT_OFFSET     0
#else
  #define STACK_TEXT_OFFSET     0x10000
#endif

// Register numbers from reg-defs.h
enum {
    REG_SP = AlphaReg_sp,
    REG_GP = AlphaReg_gp
};


int 
loader_read_auto(AppState *dest, const char *filename) 
{
    const char *fname = "loader_read_auto";
    AppExecFileLoader *loader_use = 0;
    for (int loader_num = 0; loader_num < NELEM(LoaderCreators);
         loader_num++) {
        // Note: loader_try is a newly created object instance
        AppExecFileLoader *loader_try = LoaderCreators[loader_num]();
        if (loader_try != NULL) {
            DEBUGPRINTF("%s: testing loader: %s\n",
                        fname, loader_try->name().c_str());
            if (loader_try->file_type_match(filename)) {
                loader_use = loader_try;        // hand off "ownership"
                break;
            }
            delete loader_try;
        }
    }
    if (!loader_use) {
        fprintf(stderr, "%s: exec file format of \"%s\" not recognized\n",
                fname, filename);
        goto err;
    }

    DEBUGPRINTF("%s: matched, using loader: %s\n",
                fname, loader_use->name().c_str());
    {
        int load_result = loader_use->load_file(dest, filename);
        if (load_result < 0)
            goto err;
    }

    // Arbitrary choice: put the top of the stack just below the text segment,
    // and pray there's no overlap with anything else.  This needs to be made
    // smarter.
    {
        i64 stack_size = GlobalParams.mem.stack_initial_kb * I64_LIT(1024);
        mem_addr text_start = pmem_get_base(dest->pmem, 
                                            dest->seg_info.entry_point);
        mem_addr stack_upper_lim = text_start - STACK_TEXT_OFFSET;
        mem_addr stack_start = stack_upper_lim - stack_size;

        if (GlobalParams.mem.stack_max_kb <
            GlobalParams.mem.stack_initial_kb) {
            fprintf(stderr, "%s: invalid stack_max_kb %d, less than "
                    "stack_initial_kb %d\n", fname,
                    GlobalParams.mem.stack_max_kb,
                    GlobalParams.mem.stack_initial_kb);
            goto err;
        }

        if ((text_start == 0) && !pmem_get_seg(dest->pmem, text_start)) {
            fprintf(stderr, "%s: couldn't get start of segment containing "
                    "program entry point (%s)\n",
                    fname, fmt_mem(dest->seg_info.entry_point));
            goto err;
        }

        if (stack_start >= text_start) {
            fprintf(stderr, "%s: ouch, not enough room before text_start %s "
                    "to place stack_start, ended up with %s\n",
                    fname, fmt_mem(text_start), fmt_mem(stack_start));
            goto err;
        }

        dest->seg_info.stack_upper_lim = stack_upper_lim;

        if (pmem_map_new(dest->pmem, stack_size, stack_start, PMAF_RW,
                         PMCF_AutoGrowDown)) {
            fprintf(stderr, "%s: couldn't create stack segment (%s @ 0x%s)\n",
                    fname, fmt_i64(stack_size), fmt_x64(stack_start));
            goto err;
        }

        {
            ProgMemSegment *stack_seg = pmem_get_seg(dest->pmem, stack_start);
            sim_assert(stack_seg != NULL);      // we just mapped it!
            pms_set_maxsize(stack_seg, GlobalParams.mem.stack_max_kb *
                            I64_LIT(1024));
        }
            
        pmem_write_memset(dest->pmem, stack_start, 0, stack_size, 0);
        DEBUGPRINTF("%s: initial stack base %s, size %s (upper limit %s)\n",
                    fname, fmt_mem(stack_start), fmt_i64(stack_size),
                    fmt_mem(stack_upper_lim));
    }

    delete loader_use;
    return 0;

err:
    delete loader_use;
    return -1;
}



// XXX -- this currently doesn't support the sharing of stack memory.
// Stacks could be placed far apart in the virtual address space and allowed to
// grow down, or size-limited in advance.  Mixing grow-down and sharing 
// will require changes in "prog-mem" to make auto-grow segments non-private,
// and to detect inter-ProgMem conflicts that can result from growth (yikes).
int 
loader_share_memory(AppState *dest, AppState *src) 
{
    DEBUGPRINTF("A%d sharing memory with A%d\n", dest->app_id, src->app_id);

    dest->seg_info = src->seg_info;

    i64 stack_size = GlobalParams.mem.stack_initial_kb * 1024;
    mem_addr stack_start = dest->seg_info.stack_upper_lim - stack_size;

    for (mem_addr copy_va = pmem_get_nextbase(src->pmem, 0);
         copy_va != 0; copy_va = pmem_get_nextbase(src->pmem, copy_va)) {
        ProgMemSegment *seg = pmem_get_seg(src->pmem, copy_va);
        if (!seg) {
            fprintf(stderr, "A%d couldn't get segment from A%d 0x%s\n",
                    dest->app_id, src->app_id, fmt_x64(copy_va));
            exit(1);
        }

        i64 seg_size = pms_size(seg);

        unsigned access_flags, create_flags;
        if (pmem_get_flags(src->pmem, copy_va, &access_flags, &create_flags)) {
            fprintf(stderr, "A%d couldn't get flags from A%d 0x%s\n",
                    dest->app_id, src->app_id, fmt_x64(copy_va));
            exit(1);
        }

        // Don't copy the source thread's stack segment
        if ((copy_va + seg_size) == src->seg_info.stack_upper_lim) {
            continue;
        } 

        if (pmem_map_seg(dest->pmem, seg, copy_va, access_flags, 
                         create_flags)) {
            fprintf(stderr, "T%d Couldn't map shared segment (%s @ 0x%s)\n",
                    dest->app_id, fmt_i64(seg_size), fmt_x64(copy_va));
            exit(1);
        }
    }

    // Give the new thread its own stack
    if (pmem_map_new(dest->pmem, stack_size, stack_start, PMAF_RW,
                     PMCF_AutoGrowDown)) {
        fprintf(stderr, "Couldn't create stack segment (%s @ 0x%s)\n",
                fmt_i64(stack_size), fmt_x64(stack_start));
        exit(1);
    }

    return 0;
}


// Returns the start address of the string
static mem_addr 
stack_push_string(AppState *dest, const char *src) 
{
    int copy_len = strlen(src) + 1;
    mem_addr str_start = dest->R[REG_SP].i - copy_len;
    pmem_write_memcpy(dest->pmem, str_start, src, copy_len, PMAF_None);
    dest->R[REG_SP].i = str_start;
    return str_start;
}


static void
stack_push_addr(AppState *dest, mem_addr addr) 
{
    dest->R[REG_SP].i -= 8;
    pmem_write_64(dest->pmem, dest->R[REG_SP].i, addr, 0);
}


// Initialize a thread's stack and initial register values, to prepare for
// program entry in a "main" thread.
void 
loader_init_main_entry(AppState *astate, int argc, char * const *argv, 
                       char * const *env) 
{
//    int i = 0, j = 0, k = 0, envj;
    std::vector<mem_addr> env_vars, args;

    astate->npc = astate->seg_info.entry_point;
    astate->R[REG_GP].i = astate->seg_info.gp_value;
    astate->R[REG_SP].i = astate->seg_info.stack_upper_lim;

    // Default FPCR value as read on copland (21264, OSF v4).  This has
    // just bit 59 set, which corresponds to FPCR<DYN> set to "normal
    // rounding", and all other bits 0.
    astate->R[FPCR_REG].u = U64_LIT(0x800000000000000);

    if (EMULATE_20050120_STACK_INIT) {
        astate->R[REG_SP].i -= 0x40;
        astate->R[REG_SP].i += 1;
    }

    // Create an environment on the stack.  To keep simulator results
    // repeatable, only put in the bare minimum of what you need.

    if (USE_BIG_EMPTY_STRING_HACK) {
        // Hack: in at least one instance, I've seen an alpha binary's
        // "__setenv" function perform non-speculative loads far into the
        // stack, e.g. "ldl r31, 296(r1)", while scanning for a zero quad at
        // r1.  If the stack segment ends shortly past r1, you end up with a
        // (simulated) segfault.  Quick workaround: put a bunch of empty space
        // on the stack.
        char big_empty_string[1024];
        e_snprintf(big_empty_string, sizeof(big_empty_string), 
                   "BIG_EMPTY_STRING_HACK");
        for (size_t i = strlen(big_empty_string); 
             i < (sizeof(big_empty_string) - 1); i++)
            big_empty_string[i] = '_';
        big_empty_string[sizeof(big_empty_string) - 1] = '\0';
        stack_push_string(astate, big_empty_string);
    }

    if (1) {
        // Create a static environment -- probably a good idea
        env_vars.push_back(stack_push_string(astate, "PATH=."));
    } else {
        // Copy some of the caller environment -- probably a bad idea
        int env_count;
        for (env_count = 0; env[env_count]; env_count++) 
            ;
        for (int i = env_count - 1; i >= 0; i--) {
            const char *src = env[i];
            int do_copy = 0;
            if (memcmp(src, "TERM=", 5) != 0)
                do_copy = 1;
            if (do_copy)
                env_vars.push_back(stack_push_string(astate, src));
        }
    }

    // Push arguments on the stack
    for (int i = argc - 1; i >= 0; --i)
        args.push_back(stack_push_string(astate, argv[i]));

    /* Round down to double-word boundary */
    astate->R[REG_SP].i = (astate->R[REG_SP].i - 15) & ~15;
    if (EMULATE_20050120_STACK_INIT)
        astate->R[REG_SP].i += 8;

    // Store pointers to env strings on stack, NULL-terminated
    stack_push_addr(astate, 0);
    for (unsigned i = 0; i < env_vars.size(); ++i) 
        stack_push_addr(astate, env_vars[i]);
    astate->R[4].i = astate->R[REG_SP].i;

    // Store pointers to arg strings on stack, NULL-terminated
    stack_push_addr(astate, 0);
    for (unsigned i = 0; i < args.size(); ++i) 
        stack_push_addr(astate, args[i]);
    astate->R[3].i = astate->R[REG_SP].i;

    stack_push_addr(astate, argc);
    astate->R[2].i = argc;

    // Need this so that when a new thread is created using an idle
    // context, it can reset its stack pointer correctly.
    astate->seg_info.stack_init_top = astate->R[REG_SP].i;

    if (0) {
        printf("Initial stack dump, A%d:\n", astate->app_id);
        // Dump application stack segment
        appstate_dump_mem(astate, stdout, astate->R[REG_SP].i, 
                          astate->seg_info.stack_upper_lim
                          - astate->R[REG_SP].i, "  ");
    }
}


// Initialize a thread's stack and initial register values, to prepare for
// program entry in a non-"main" thread.
void 
loader_init_shared_entry(AppState *astate, AppState *src, int argc,
                         char * const *argv, char * const *env) 
{
    // dst->R[REG_GP].i = src->R[REG_GP].i;
    loader_init_main_entry(astate, argc, argv, env);
}
