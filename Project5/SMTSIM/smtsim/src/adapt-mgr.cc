#include <stdio.h>
#include "adapt-mgr.h"
#include "sim-assert.h"
#include "core-resources.h"
#include "context.h"
#include "utils.h"
#include "limits.h"
#include "stage-queue.h"
#include <cstring>

int debug_adapt_mgr = 0;
extern int CtxCount;
extern int CoreCount;
extern struct CoreResources **Cores;                    // [CoreCount]
extern struct context **Contexts;                       // [CtxCount]
extern i64 cyc; 

using namespace std;

class AdaptMgr {
private:
    
    /*
     * FIXME: 
     * The logic in regrename is broken for CMPs with SMT.
     * When multiple contexts are present in a core when the first context stops
     * all of them do (break;...)
    */
    
    int enabled;
    order_policy op;
    limit_policy lp;
    int share_flags[last_shared_resource];
    int share_all;
    // 2D arrays . To access always keep format "sr*CtxCount+tid"
    int *occupancy_per_ctx;
    int *max_occupancy_per_ctx;
    int *to_be_reclaimed_per_ctx; 
    int *resource_size_per_ctx; 
    int already_initialized; //Used for limit policies
    
    int get_resource_size_per_ctx(shared_resource sr, int id)
    {
        assert (resource_size_per_ctx);
        return (resource_size_per_ctx[sr*CtxCount+id]);
    }
    
    void set_resource_size_per_ctx(shared_resource sr, int id, 
            int val)
    {
        assert (resource_size_per_ctx);
        resource_size_per_ctx[sr*CtxCount+id] = val;
    }
    
    void inc_resource_size_per_ctx(shared_resource sr, int id, 
            int val)
    {
        assert (resource_size_per_ctx);
        resource_size_per_ctx[sr*CtxCount+id] += val;
    }

    void dec_resource_size_per_ctx(shared_resource sr, int id, 
            int val)
    {
        assert (resource_size_per_ctx);
        resource_size_per_ctx[sr*CtxCount+id] -= val;
    }


    void aggregate_pooled_resources(float limit)
    {
        // Setting originally for all contexts to max value 
        for (int core_id = 0; core_id < CoreCount; core_id++)
        {
            // TODO: GET RID of Implicit assumption that all cores participate in 
            //       resource pooling (requires more complex data structures)
            //       If we migrate threads, this will fall apart 
            // TODO: This assumes homogeneous cores. Extend it to heterogeneous :-)
            
	 
	  for (int ctx_id = 0; ctx_id < Cores[core_id]->n_contexts; ctx_id++){
                int tid = Cores[core_id]->contexts[ctx_id]->id;
                CoreParams *p = &Cores[core_id]->params;
                set_resource_size_per_ctx(LSQ, tid, int(limit*p->loadstore_queue_size*CoreCount));
                //printf("LSQ th_id: %d res: %d ", tid, p->loadstore_queue_size);
                set_resource_size_per_ctx(IREG, tid, int(limit*p->rename.int_rename_regs*CoreCount));
                //printf("IREG th_id: %d res: %d ", tid, p->rename.int_rename_regs);
                set_resource_size_per_ctx(FREG, tid, int(limit*p->rename.float_rename_regs*CoreCount));
                //printf("FREG th_id: %d res: %d ", tid, p->rename.float_rename_regs);
                set_resource_size_per_ctx(IQ, tid, int(limit*p->queue.int_queue_size*CoreCount));
                //printf("IQ th_id: %d res: %d ", tid, p->queue.int_queue_size);
                set_resource_size_per_ctx(FQ, tid, int(limit*p->queue.float_queue_size*CoreCount));
                //printf("FQ th_id: %d res: %d ", tid, p->queue.float_queue_size);
                //printf("\n");
            }
	  

	 
        }
        // FIXME: This is fragile 
        for (int ctx_id = 0; ctx_id < CtxCount; ctx_id++) 
        {
            ThreadParams *p = &Contexts[ctx_id]->params;
            set_resource_size_per_ctx(ROB, ctx_id, int(limit*p->reorder_buffer_size*CoreCount));
        }


        print_pooled_resources();
    }


    
public:

    void debug_print_internal_state(const char *pref);
    
    void print_exit_stats(void)
    {
        debug_print_internal_state("  ");
    }

    order_policy get_or_policy()
    {
        return op;        
    }

    int get_size(shared_resource sr, context *ctx)
    {
        return get_resource_size_per_ctx(sr, ctx->id);
    }
   
    
    int is_shared(shared_resource sr)
    {
        sim_assert(sr < last_shared_resource);
        return share_flags[sr]; 
    }
    
    void update_stats(shared_resource sr, int val)
    {
        
    }
   
    void update_adapt_mgr_incr(context * ctx, shared_resource sr, int value)
    {
        assert ( occupancy_per_ctx[sr*CtxCount+ctx->id] < INT_MAX - value);
        occupancy_per_ctx[sr*CtxCount+ctx->id] += value;
    }

    void make_final()
    {
        for (int i = 0; i < CtxCount*last_shared_resource; i++)
        {
            // Check for underflow
            assert ( occupancy_per_ctx[i] > INT_MIN + to_be_reclaimed_per_ctx[i]);
            occupancy_per_ctx[i] -= to_be_reclaimed_per_ctx[i];
            to_be_reclaimed_per_ctx[i] = 0; 
            // Update max values
            max_occupancy_per_ctx[i] = MAX_SCALAR(max_occupancy_per_ctx[i], occupancy_per_ctx[i]);
        }
        
        // Debugging        
//        debug_print_internal_state("  ");
//        for (int i = 0; i < CtxCount; i++)
//        {
//            printf("iq:%d dq:%d  ",stageq_count(Contexts[i]->core->stage.intq), stageq_count(Contexts[i]->core->stage.floatq));
//        }
//        printf("\n");
        
        // Verification: ROB
        for (int i = 0; i < CtxCount; i++)
        {
            sim_assert( Contexts[i]->rob_used ==  occupancy_per_ctx[ROB*CtxCount+i]);
            sim_assert( Contexts[i]->core->lsq_used == occupancy_per_ctx[LSQ*CtxCount+i]);
            sim_assert( Contexts[i]->core->i_registers_used == occupancy_per_ctx[IREG*CtxCount+i]);
            sim_assert( Contexts[i]->core->f_registers_used == occupancy_per_ctx[FREG*CtxCount+i]);
            sim_assert( stageq_count(Contexts[i]->core->stage.intq) == occupancy_per_ctx[IQ*CtxCount+i]);
            sim_assert( stageq_count(Contexts[i]->core->stage.floatq) == occupancy_per_ctx[FQ*CtxCount+i]);
        }
    }
    
    void update_adapt_mgr_dec_tentative(context * ctx, shared_resource sr, int value)
    {
        to_be_reclaimed_per_ctx[sr*CtxCount+ctx->id] += value;
    }
    
    int parse_order_policy(const char * str)
    {
        for (int i = 0; i < LAST_ORDER_POLICY; i++)
            if(strcmp (OrderPolicyNames[i], str) == 0)
                return i;
        fprintf(stderr, "Unrecognized Order Policy\n");
        sim_abort();
        return 0;
    }

    int parse_limit_policy(const char * str)
    {
        for (int i = 0; i < LAST_LIMIT_POLICY; i++)
            if(strcmp (LimitPolicyNames[i], str) == 0)
                return i;
        fprintf(stderr, "Unrecognized Limit Policy\n");
        sim_abort();
        return 0;
    }
    
    int space_available(shared_resource sr, context * ctx)
    {
//        if (!(cyc%10000))
//            printf("Debugging time\n");
        int total_occupancy = 0;
        for (int i = sr*CtxCount; i < (sr+1)*CtxCount; i++){
            total_occupancy += occupancy_per_ctx[i];
        }
        return resource_size_per_ctx[sr*CtxCount+ctx->id] - total_occupancy;
    }

    
    void limit_resources(void)
    {
        switch (lp) {
        case NOLIMIT:  
            //Do nothing, contexts can access aggregate resources/core*CoreCount 
            break;
        case LIMIT75: {
            //Contexts can access aggregate 0.75 * resources/core*CoreCount 
            if (!already_initialized) // Run only once
            {
                already_initialized = 1;
                aggregate_pooled_resources(0.75); 
            }
            break;
        }
        case LIMIT50: {
            //Contexts can access aggregate 0.50 * resources/core*CoreCount 
            if (!already_initialized) // Run only once
            {
                already_initialized = 1;
                aggregate_pooled_resources(0.50); 
            }
            break;
        }
        case LIMIT25: {
            //Contexts can access aggregate 0.25 * resources/core*CoreCount 
            if (!already_initialized) // Run only once
            {
                already_initialized = 1;
                aggregate_pooled_resources(0.25); 
            }
            break;
	}
	case LIMIT3125: {
	  //Contexts can access aggregate 0.3125 * resources/core*CoreCount, only make sense when we have 4 cores, which means resources can grow by upto 25%
	  if (!already_initialized) // Run only once
            {
	      already_initialized = 1;
	      aggregate_pooled_resources(0.3125);
            }
	  break;


        }
        default:
            fprintf(stderr, "Unrecognized Limit Policy\n");
            sim_abort();
        } 
    }

    
    void print_pooled_resources()
    {
        printf("Printing Pooled Resources:\n");
        for (int i = 0; i < CtxCount*last_shared_resource; i++){
            if (i % CtxCount == 0)
                printf("%5s ", SharedResourceNames[i/CtxCount]);
            printf("%3d ", resource_size_per_ctx[i]);
            if (i % CtxCount == (CtxCount - 1))
                printf("\n");
        }
        printf("\n"); 
    }
   
    AdaptMgr()
    {
        enabled = simcfg_get_bool("ResourcePooling/enable");
        already_initialized = 0;

        share_all = enabled && simcfg_get_bool("ResourcePooling/share_all");
        if (share_all) {
           for ( shared_resource sr = static_cast<shared_resource>(0);
                   sr < last_shared_resource; sr = static_cast<shared_resource>(sr+1))
               share_flags[sr] = 1;
        }
        else {
            share_flags[LSQ] = enabled && 
                simcfg_get_bool("ResourcePooling/share_lsq"); 
            share_flags[ROB] = enabled && 
                simcfg_get_bool("ResourcePooling/share_rob"); 
            share_flags[IQ] = enabled && 
                simcfg_get_bool("ResourcePooling/share_iq"); 
            share_flags[FQ] = enabled && 
                simcfg_get_bool("ResourcePooling/share_fq"); 
            share_flags[IREG] = enabled && 
                simcfg_get_bool("ResourcePooling/share_iregs"); 
            share_flags[FREG] = enabled && 
                simcfg_get_bool("ResourcePooling/share_fregs"); 
        }
        
        to_be_reclaimed_per_ctx = (int *)emalloc_zero(CtxCount*last_shared_resource*
                sizeof(int));
        occupancy_per_ctx = (int *)emalloc_zero(CtxCount*last_shared_resource*
                sizeof(int));
        max_occupancy_per_ctx = (int *)emalloc_zero(CtxCount*last_shared_resource*
                sizeof(int));
        resource_size_per_ctx = (int *)emalloc_zero(CtxCount*last_shared_resource*
                sizeof(u32));
        aggregate_pooled_resources(1.0); 
        
        //print_pooled_resources();

        op = static_cast<order_policy> ( parse_order_policy(
                    simcfg_get_str("ResourcePooling/order_policy")) ); 

        lp = static_cast<limit_policy> ( parse_limit_policy(
                    simcfg_get_str("ResourcePooling/limit_policy")) ); 
    }

    ~AdaptMgr()
    {
        // Free memory
        free(to_be_reclaimed_per_ctx); 
        free(resource_size_per_ctx); 
        free(occupancy_per_ctx); 
        free(max_occupancy_per_ctx); 
    }
};


void
AdaptMgr::debug_print_internal_state(const char *pref)
{
    printf("GlobalAdaptMgr Stats:\n");
    printf("%slast_occupancy_per_ctx\n%s%s", pref,pref,pref);
    for (int i = 0; i < CtxCount*last_shared_resource; i++){
        if (i % CtxCount == 0)
            printf("%-5s ", SharedResourceNames[i/CtxCount]);
        printf("%d ", occupancy_per_ctx[i]);
        if ((i % CtxCount == (CtxCount - 1)) && 
                (i != (CtxCount*last_shared_resource - 1)))
            printf("\n%s%s", pref,pref);
    }
    printf("\n"); 

    printf("%smax_occupancy_per_ctx\n%s%s", pref,pref,pref);
    for (int i = 0; i < CtxCount*last_shared_resource; i++){
        if (i % CtxCount == 0)
            printf("%-5s ", SharedResourceNames[i/CtxCount]);
        printf("%d ", max_occupancy_per_ctx[i]);
        if ((i % CtxCount == (CtxCount - 1)) && 
                (i != (CtxCount*last_shared_resource - 1)))
            printf("\n%s%s", pref, pref);
    }
    printf("\n"); 
   
    // This should be always 0, since we print after fix_regs() 
    /*
    printf("%sto_be_reclaimed_per_cts\n%s%s", pref,pref,pref);
    for (int i = 0; i < CtxCount*last_shared_resource; i++){
        if (i % CtxCount == 0)
            printf("%-5s ", SharedResourceNames[i/CtxCount]);
        printf("%d ", to_be_reclaimed_per_ctx[i]);
        if ((i % CtxCount == (CtxCount - 1)) && 
                (i != (CtxCount*last_shared_resource - 1)))
            printf("\n%s%s", pref,pref);
    }
    printf("\n"); 
    */

    printf("%sresource_size_per_ctx\n%s%s", pref,pref,pref);
    for (int i = 0; i < CtxCount*last_shared_resource; i++){
        if (i % CtxCount == 0)
            printf("%-5s ", SharedResourceNames[i/CtxCount]);
        printf("%d ", resource_size_per_ctx[i]);
        if ((i % CtxCount == (CtxCount - 1)) && 
                (i != (CtxCount*last_shared_resource - 1)))
            printf("\n%s%s", pref, pref);
    }
    printf("\n"); 
}


/***********   C Interface  ********************/
AdaptMgr *adaptmgr_create()
{
    return (new AdaptMgr());
}

void adaptmgr_destroy(AdaptMgr *adm)
{
    return (delete adm);
}

void print_adaptmgr_stats(void)
{
    assert (GlobalAdaptMgr != NULL);
    return GlobalAdaptMgr->print_exit_stats();
}

int get_size(shared_resource sr, context *ctx)
{
    assert (GlobalAdaptMgr != NULL);
    return GlobalAdaptMgr->get_size(sr, ctx);
}

int is_shared(shared_resource sr)
{
    assert (GlobalAdaptMgr != NULL);
    return GlobalAdaptMgr->is_shared(sr);
}

void update_stats (shared_resource sh, int val)
{
    assert (GlobalAdaptMgr != NULL);
    GlobalAdaptMgr->update_stats(sh, val);
}

order_policy get_order_policy()
{
    assert (GlobalAdaptMgr != NULL);
    return GlobalAdaptMgr->get_or_policy();
}

void update_adapt_mgr_incr(context * ctx, shared_resource sr, int value)
{   
    assert (GlobalAdaptMgr != NULL);
    GlobalAdaptMgr->update_adapt_mgr_incr(ctx, sr, value);
}

void update_adapt_mgr_dec_tentative(context * ctx, shared_resource sr, int value)
{
    assert (GlobalAdaptMgr != NULL);
    GlobalAdaptMgr->update_adapt_mgr_dec_tentative(ctx, sr, value);
}

int space_available(shared_resource sr, context * ctx)
{
    assert (GlobalAdaptMgr != NULL);
    return GlobalAdaptMgr->space_available(sr, ctx);
}

void update_adapt_mgr_make_final(void)
{
    assert (GlobalAdaptMgr != NULL);
    GlobalAdaptMgr->make_final();
}

void limit_resources(void)
{
    assert (GlobalAdaptMgr != NULL);
    GlobalAdaptMgr->limit_resources();
}
