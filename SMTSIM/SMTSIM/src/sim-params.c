/*
 * Global simulator config parameters/allocators
 *
 * Jeff Brown
 * $Id: sim-params.c,v 1.11.6.5.2.2.2.1 2008/10/29 09:33:42 jbrown Exp $
 */

const char RCSid_1042835086[] = 
"$Id: sim-params.c,v 1.11.6.5.2.2.2.1 2008/10/29 09:33:42 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "sim-params.h"
#include "main.h"
#include "cache.h"
#include "cache-req.h"
#include "utils.h"
#include "prog-mem.h"


static const char *TCPNames[TCP_last] = {
    "smt", "cmp"
};


SimParams GlobalParams;

struct context **Contexts;                      // [CtxCount]
struct CoreResources **Cores;                   // [CoreCount]
struct CacheRequest *CacheHolders;              // [HOLDERS]
struct CacheRequest **CacheFreeHolders;         // [HOLDERS]


void
alloc_globals(void)
{
    int num_cores = GlobalParams.num_cores;
    int num_contexts = GlobalParams.num_contexts;

    Cores = emalloc_zero(num_cores * sizeof(Cores[0]));

    { 
        int holders = GlobalParams.mem.cache_request_holders;
        int i;
        CacheHolders = emalloc_zero(holders * sizeof(CacheHolders[0]));
        CacheFreeHolders = emalloc_zero(holders * sizeof(CacheFreeHolders[0]));
        for (i = 0; i < holders; i++) {
            CacheHolders[i].cores = 
                emalloc_zero((num_cores + 1) 
                            * sizeof(CacheHolders[i].cores[0]));
            CacheHolders[i].blocked_apps = 
                emalloc_zero(1 * sizeof(CacheHolders[i].blocked_apps[0]));
        }
    }

    Contexts = emalloc_zero(num_contexts * sizeof(Contexts[0]));
}


int
tcp_parse_policy(const char *str)
{
    int i;
    for (i = 0; i < TCP_last; i++)
        if (strcmp(TCPNames[i], str) == 0)
            return i;
    return -1;
}


int 
tcp_policy_core(int policy, int thread_id)
{
    int core_id;

    switch (policy) {
    case TCP_Smt:
        core_id = 0;
        break;
    case TCP_Cmp:
        core_id = thread_id;
        break;
    default:
        fprintf(stderr, "%s (%s:%i): unmatched policy #%i\n", __func__, 
                __FILE__, __LINE__, policy);
        core_id = -1;
        sim_abort();
    }

    return core_id;
}
