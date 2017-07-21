/*
 * Resources associated with each execution core
 *
 * Jeff Brown
 * $Id: core-resources.c,v 1.13.6.9.2.2.2.17.6.1 2009/12/25 06:31:49 jbrown Exp $
 */

const char RCSid_1036522097[] = 
"$Id: core-resources.c,v 1.13.6.9.2.2.2.17.6.1 2009/12/25 06:31:49 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "core-resources.h"
#include "utils.h"
#include "cache-array.h"
#include "tlb-array.h"
#include "btb-array.h"
#include "pht-predict.h"
#include "branch-bias-table.h"
#include "main.h"
#include "context.h"
#include "dyn-inst.h"
#include "sim-params.h"
#include "sim-cfg.h"
#include "prefetch-streambuf.h"
#include "deadblock-pred.h"
#include "mshr.h"


struct CoreBus {
    i64 avail_cyc;              // Cycle when next operation can begin
    i64 done_cyc;               // Cycle after all ops complete
    i64 xfers;
    i64 syncs;
    i64 total_idle_cyc;

    i64 sync_wait_cyc;
};


static CacheArray *
core_cache_create(const CoreCacheParams *params, CoreResources *parent_core,
                  int is_core_coherence_interface)
{
    // is_core_coherence_interface indicates that this is the cache that is
    // just "above" the interconnect, making it this core's point of contact
    // for inter-core coherence.  (This restricts possible topologies, but is
    // expedient "for now".)  Note that parent_core->coher_mgr may still be
    // NULL, if it's not in use.
    struct CoherenceMgr *coher_mgr = (is_core_coherence_interface) ?
        parent_core->params.coher_mgr : NULL;
    return cache_create(params->cache_id, params->geom, &params->timing,
                        coher_mgr, parent_core, cyc);
}


static int
cache_param_block_count(const CoreCacheParams *params)
{
    long cache_bytes = params->geom->size_kb * 1024;
    long block_bytes = params->geom->block_bytes;
    if ((cache_bytes % block_bytes) != 0) {
        abort_printf("block_bytes (%ld) doesn't divide cache_bytes (%ld), "
                     "should have been caught before\n", block_bytes,
                     cache_bytes);
    }
    return cache_bytes / block_bytes;
}


CoreResources *
core_create(int core_id, const CoreParams *params)
{
    CoreResources *n = NULL;
    // (I wish I'd have made this module in C++)
    char temp_path[200];        // temp for building up config paths
    char temp_id[200];          // temp for building module IDs, etc.

    sim_assert(core_id >= 0);

    n = emalloc_zero(sizeof(*n));
    n->core_id = core_id;

    {
        CoreParams *params_embed = coreparams_copy(params);
        params = NULL;              // discard to avoid unintended use
        // Sketchy: we'll value-copy all fields of params_embed, and then
        // free only the containing struct, transferring ownership of any
        // dynamic fields to the embedded copy.  (This is shady, but it
        // beats breaking every use of "params" everywhere else.)
        n->params = *params_embed;
        free(params_embed);
    }

    n->n_contexts = 0;
    n->contexts = NULL;
    n->sched.priority = NULL;
    n->sched.key = NULL;

    sim_assert(n->params.fetch.n_stages > 0);
    sim_assert(n->params.decode.n_stages > 0);
    sim_assert(n->params.rename.n_stages > 0);
    n->stage.dyn_stages = n->params.fetch.n_stages - 1 + 
        n->params.decode.n_stages + n->params.rename.n_stages +
        n->params.regread.n_stages + n->params.regwrite.n_stages;
    n->stage.decode1 = n->params.fetch.n_stages - 1;
    n->stage.rename1 = n->stage.decode1 + n->params.decode.n_stages;
    n->stage.rread1 = n->stage.rename1 + n->params.rename.n_stages;
    n->stage.rwrite1 = n->stage.rread1 + n->params.regread.n_stages;
    n->stage.s = emalloc_zero(n->stage.dyn_stages * sizeof(n->stage.s[0]));

    // Br bias table must come before trace fill unit
    if (!(n->br_bias = bbt_create(n->params.br_bias_entries,
                                  n->params.inst_bytes))) {
        fprintf(stderr, "%s (%s:%i): couldn't create branch bias table\n",
                __func__, __FILE__, __LINE__);
        goto fail;
    }

    if (n->params.fetch.enable_trace_cache) {
        if (!(n->tcache = tc_create(&n->params.tcache))) {
            fprintf(stderr, "%s (%s:%i): couldn't create trace cache\n", 
                    __func__, __FILE__, __LINE__);
            goto fail;
        }
        // Fill unit must come after br bias table
        if (!(n->tfill = tfu_create(&n->params.tfill, n))) {
            fprintf(stderr, "%s (%s:%i): couldn't create trace fill unit\n",
                    __func__, __FILE__, __LINE__);
            goto fail;
        }
    }

    e_snprintf(temp_id, sizeof(temp_id), "C%d.i_mshr", core_id);
    e_snprintf(temp_path, sizeof(temp_path), "%s/InstMSHR",
               n->params.config_path);
    if (!(n->inst_mshr = mshr_create(temp_id, temp_path,
                                     GlobalParams.mem.cache_block_bytes))) {
        fprintf(stderr, "%s (%s:%i): couldn't create I-MSHR\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    e_snprintf(temp_id, sizeof(temp_id), "C%d.d_mshr", core_id);
    e_snprintf(temp_path, sizeof(temp_path), "%s/DataMSHR",
               n->params.config_path);
    if (!(n->data_mshr = mshr_create(temp_id, temp_path,
                                     GlobalParams.mem.cache_block_bytes))) {
        fprintf(stderr, "%s (%s:%i): couldn't create D-MSHR\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->icache = core_cache_create(&n->params.icache, n, 0))) {
        fprintf(stderr, "%s (%s:%i): couldn't create I-cache\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->dcache = core_cache_create(&n->params.dcache, n,
                                        !GlobalParams.mem.private_l2caches))) {
        fprintf(stderr, "%s (%s:%i): couldn't create D-cache\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    {
        int enable = 0;
        e_snprintf(temp_path, sizeof(temp_path), "%s/DataStreambuf/enable",
                   n->params.config_path);
        enable = simcfg_get_bool(temp_path);
        e_snprintf(temp_path, sizeof(temp_path), "%s/DataStreambuf",
                   n->params.config_path);
        e_snprintf(temp_id, sizeof(temp_id), "C%d.D", core_id);
        n->d_streambuf = NULL;
        if (enable && !(n->d_streambuf =
                        pfsg_create(temp_id, temp_path, n, GlobalEventQueue,
                                    GlobalParams.mem.cache_block_bytes))) {
            fprintf(stderr, "%s (%s:%i): couldn't create D-streambuf\n",
                    __func__, __FILE__, __LINE__);
            goto fail;
        }
    }

    if (!(n->itlb = tlb_create(n->params.itlb_entries, 
                               n->params.page_bytes))) {
        fprintf(stderr, "%s (%s:%i): couldn't create ITLB\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->dtlb = tlb_create(n->params.dtlb_entries,
                               n->params.page_bytes))) {
        fprintf(stderr, "%s (%s:%i): couldn't create DTLB\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->btb = btb_create(n->params.btb_entries, n->params.btb_assoc,
                              n->params.inst_bytes))) {
        fprintf(stderr, "%s (%s:%i): couldn't create BTB\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->pht = pht_create(n->params.pht_entries, n->params.inst_bytes))) {
        fprintf(stderr, "%s (%s:%i): couldn't create PHT\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->multi_bp = mbp_create(&n->params.multi_bp))) {
        fprintf(stderr, "%s (%s:%i): couldn't create multi branch predictor\n",
                __func__, __FILE__, __LINE__);
        goto fail;
    }

    n->d_dbp = NULL;
    {
        int enable, peer_blocks;
        e_snprintf(temp_path, sizeof(temp_path), "%s/DataDeadBlock/enable",
                   n->params.config_path);
        enable = simcfg_get_bool(temp_path);
        e_snprintf(temp_path, sizeof(temp_path), "%s/DataDeadBlock",
                   n->params.config_path);
        e_snprintf(temp_id, sizeof(temp_id), "C%d.D", core_id);
        peer_blocks = cache_param_block_count(&n->params.dcache);
        n->d_dbp = NULL;
        if (enable &&
            !(n->d_dbp = dbp_create(temp_id, temp_path, n, peer_blocks,
                                    GlobalParams.mem.cache_block_bytes))) {
            err_printf("couldn't create core %d DataDeadBlock predictor\n",
                       core_id);
            goto fail;
        }
    }

    n->i_dbp = NULL;
    if (0) {    // I-cache DBP not really working yet
        int enable, peer_blocks;
        e_snprintf(temp_path, sizeof(temp_path), "%s/InstDeadBlock/enable",
                   n->params.config_path);
        enable = simcfg_get_bool(temp_path);
        e_snprintf(temp_path, sizeof(temp_path), "%s/InstDeadBlock",
                   n->params.config_path);
        e_snprintf(temp_id, sizeof(temp_id), "C%d.I", core_id);
        peer_blocks = cache_param_block_count(&n->params.icache);

        if (enable &&
            !(n->i_dbp = dbp_create(temp_id, temp_path, n, peer_blocks,
                                    GlobalParams.mem.cache_block_bytes))) {
            err_printf("couldn't create core %d InstDeadBlock predictor\n",
                       core_id);
            goto fail;
        }
    }

    if (!(n->request_bus = n->params.request_bus) ||
        !(n->reply_bus = n->params.reply_bus)) {
        fprintf(stderr, "%s (%s:%i): missing inter-core bus\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    if (GlobalParams.mem.private_l2caches) {
        if (!(n->l2cache = core_cache_create(&n->params.private_l2cache, n,
                                             1))) {
            fprintf(stderr, "%s (%s:%i): couldn't create private L2 cache\n",
                    __func__, __FILE__, __LINE__);
            goto fail;
        }
        e_snprintf(temp_id, sizeof(temp_id), "C%d.l2_mshr", core_id);
        e_snprintf(temp_path, sizeof(temp_path), "%s/L2MSHR",
                   n->params.config_path);
        if (!(n->private_l2mshr =
              mshr_create(temp_id, temp_path,
                          GlobalParams.mem.cache_block_bytes))) {
            fprintf(stderr, "%s (%s:%i): couldn't create L2-MSHR\n", __func__,
                    __FILE__, __LINE__);
            goto fail;
        }
    } else {
        n->l2cache = n->params.shared_l2cache;
    }
    n->l2_dbp = NULL;   // place-holder
    if (!n->l2cache) {
        fprintf(stderr, "%s (%s:%i): missing L2 cache\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }

    // L3 cache is optional
    n->l3cache = n->params.shared_l3cache;
    n->l3_dbp = NULL;   // place-holder

    return n;

fail:
    fprintf(stderr, "%s (%s:%i): creation of core %i failed.\n", __func__,
            __FILE__, __LINE__, core_id);
    core_destroy(n);
    return NULL;
}


void 
core_destroy(CoreResources *core)
{
    if (core) {
        // begin CoreParams member freeing (crufty value-embedding)
        free(core->params.config_path);
        // end CoreParams member freeing

        free(core->stage.s);
        bbt_destroy(core->br_bias);
        tc_destroy(core->tcache);
        tfu_destroy(core->tfill);
        mshr_destroy(core->inst_mshr);
        mshr_destroy(core->data_mshr);
        cache_destroy(core->icache);
        cache_destroy(core->dcache);
        pfsg_destroy(core->d_streambuf);

        tlb_destroy(core->itlb);
        tlb_destroy(core->dtlb);
        btb_destroy(core->btb);
        pht_destroy(core->pht);
        mbp_destroy(core->multi_bp);
        dbp_destroy(core->i_dbp);
        dbp_destroy(core->d_dbp);
        if (GlobalParams.mem.private_l2caches) {
            mshr_destroy(core->private_l2mshr);
            cache_destroy(core->l2cache);
            dbp_destroy(core->l2_dbp);
        }
        free(core->sched.priority);
        free(core->sched.key);
        free(core->contexts);
        free(core);
    }
}


CoreParams *
coreparams_copy(const CoreParams *params)
{
    CoreParams *n = emalloc_zero(sizeof(*n));
    *n = *params;       // lazy value-copy; we'll fix dynamic members after
    n->config_path = e_strdup(params->config_path);
    return n;
}


void
coreparams_destroy(CoreParams *params)
{
    if (params) {
        // warning: member frees must be duplicated in core_destroy()
        free(params->config_path);
        free(params);
    }
}


void
core_add_context(CoreResources *core, struct context *ctx)
{
    struct context **new_contexts;
    int *new_key, *new_priority;

    if (!(new_contexts = realloc(core->contexts, (core->n_contexts + 1) *
                                 sizeof(core->contexts[0])))) 
        goto nomem;
    if (!(new_priority = realloc(core->sched.priority, (core->n_contexts + 1) *
                                 sizeof(core->sched.priority[0]))))
        goto nomem;
    if (!(new_key = realloc(core->sched.key, (core->n_contexts + 1) *
                            sizeof(core->sched.key[0]))))
        goto nomem;

    core->sched.priority = new_priority;
    core->sched.key = new_key;
    core->contexts = new_contexts;

    core->sched.priority[core->n_contexts] = 0;
    core->sched.key[core->n_contexts] = 0;
    core->contexts[core->n_contexts] = ctx;    

    ctx->core = core;
    ctx->core_thread_id = core->n_contexts;

    if (core->tcache)
        ctx->tc.block = tcb_alloc(core->tcache);

    core->n_contexts++;

    return;

nomem:
    fprintf(stderr, "%s (%s:%i): out of memory adding context %p to core "
            "%i\n", __func__, __FILE__, __LINE__, (void *) ctx, 
            core->core_id);
    exit(1);
}


void 
core_dump_queue(const StageQueue *stage)
{
    const activelist *inst;
    for (inst = stageq_head(*stage); inst; inst = inst->next)
        printf(" %ds%d", inst->thread, inst->id);
}


void 
core_dump_queues(const CoreResources *core)
{
    printf("C%i stages: ", core->core_id);
    printf("D1");
    core_dump_queue(&core->stage.s[core->stage.decode1]);
    printf(", RN1");
    core_dump_queue(&core->stage.s[core->stage.rename1]);
    printf(", IQ");
    core_dump_queue(&core->stage.intq);
    printf(", FQ");
    core_dump_queue(&core->stage.floatq);
    printf(", RR1");
    core_dump_queue(&core->stage.s[core->stage.rread1]);
    printf(", X");
    core_dump_queue(&core->stage.exec);
    printf(", W1");
    core_dump_queue(&core->stage.s[core->stage.rwrite1]);
    printf("\n");
}


CoreBus *
corebus_create(void)
{
    CoreBus *n = NULL;
    n = emalloc_zero(sizeof(*n));
    corebus_reset(n);
    return n;
}


void
corebus_destroy(CoreBus *bus)
{
    free(bus);
}


void 
corebus_reset(CoreBus *bus)
{
    bus->avail_cyc = 0;
    bus->done_cyc = 0;
    bus->xfers = 0;
    bus->syncs = 0;
    bus->total_idle_cyc = 0;
    bus->sync_wait_cyc = 0;
}


void
corebus_get_stats(const CoreBus *bus, CoreBusStats *stats_ret)
{
    stats_ret->xfers = bus->xfers;
    stats_ret->syncs = bus->syncs;
    stats_ret->idle_cyc = bus->total_idle_cyc;
    if (bus->avail_cyc < cyc)
        stats_ret->idle_cyc += cyc - bus->avail_cyc;
    stats_ret->sync_cyc = bus->sync_wait_cyc;
    stats_ret->useful_cyc = cyc - (stats_ret->idle_cyc + stats_ret->sync_cyc);
    stats_ret->util = (double) stats_ret->useful_cyc / cyc;
}


i64 
corebus_access(CoreBus *bus, OpTime op_time)
{
    i64 ready_time;
    if (bus->avail_cyc < cyc)
        bus->total_idle_cyc += cyc - bus->avail_cyc;
    bill_resource_time(ready_time, bus->avail_cyc, cyc, op_time);
    if (ready_time > bus->done_cyc)
        bus->done_cyc = ready_time;        
    bus->xfers++;
    return ready_time;
}


i64
corebus_sync_prepare(CoreBus *bus)
{
    // Ensure that the next bus operation will be delivered _after_ whatever
    // bus operations are currently outstanding.  (Typically, this simple
    // model orders only the start of operations with respect to each other,
    // but that's not enough to prevent "small" coherence messages from
    // skipping past "big" data fills.)
    i64 sync_time;
    if (bus->avail_cyc < cyc) {
        bus->total_idle_cyc += cyc - bus->avail_cyc;
        bus->avail_cyc = cyc;
    }
    sync_time = MAX_SCALAR(bus->avail_cyc, bus->done_cyc);
    if (bus->avail_cyc < sync_time)
        bus->sync_wait_cyc += sync_time - bus->avail_cyc;
    bus->avail_cyc = sync_time;
    bus->done_cyc = sync_time;
    bus->syncs++;
    return sync_time;
}


int
corebus_probe_avail(const CoreBus *bus, i64 test_time)
{
    return bus->avail_cyc <= test_time;
}
