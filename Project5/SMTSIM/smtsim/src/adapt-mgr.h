#include "sys-types.h"
#include "utils.h"
#include "core-resources.h"
#include "sim-cfg.h"
#include "context.h"
#include "sim-assert.h"
#include "context.h"

#ifndef ADAPT_MGR_H
#define ADAPT_MGR_H
  
#ifdef __cplusplus
extern "C" {
#endif



typedef enum {
    ROB, IQ, FQ, IREG, FREG, LSQ, last_shared_resource
} shared_resource ;

typedef enum  {
    FIXED, RROBIN, LAST_ORDER_POLICY
} order_policy;


typedef struct AdaptMgr AdaptMgr;
AdaptMgr *GlobalAdaptMgr;


int get_size (shared_resource sr, context * ctx);
int is_shared(shared_resource sr);
void limit_resources(void);
void update_adapt_mgr_incr(context * ctx, shared_resource sr, int value);
// This will become permanent from fix_regs() with update_adapt_mgr_make_final
void update_adapt_mgr_dec_tentative(context * ctx, shared_resource sr, int value);
void update_adapt_mgr_make_final(void);
int space_available(shared_resource sr, context * ctx);

AdaptMgr *adaptmgr_create(void);
void adaptmgr_destroy(AdaptMgr *adm);
void print_adaptmgr_stats(void);
order_policy get_order_policy(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
// C++ specific interface

static const char * OrderPolicyNames[LAST_ORDER_POLICY] =
{
    "FIXED", "RROBIN"
};


typedef enum  {
    NOLIMIT, LIMIT75, LIMIT50, LIMIT25, LIMIT3125, STEEPDES, LAST_LIMIT_POLICY
} limit_policy;

static const char * SharedResourceNames[last_shared_resource] =
{ //Names should fit in width of 5
    "ROB", "IQ", "FQ", "IREG", "FREG", "LSQ"
};

static const char * LimitPolicyNames[LAST_LIMIT_POLICY] =
{
  "NOLIMIT", "LIMIT75", "LIMIT50", "LIMIT25", "LIMIT3125", "STEEPDES"
};

#endif  // __cplusplus


#endif
