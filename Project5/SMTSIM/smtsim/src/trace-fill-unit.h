//
// Trace Fill Unit
//
// Jeff Brown
// $Id: trace-fill-unit.h,v 1.2.2.2.6.1 2008/04/30 22:18:01 jbrown Exp $
//

#ifndef TRACE_FILL_UNIT_H
#define TRACE_FILL_UNIT_H

#ifdef __cplusplus
extern "C" {
#endif

struct CoreResources;
struct activelist;
struct context;


typedef struct TraceFillUnit TraceFillUnit;
typedef struct TraceFillUnitParams TraceFillUnitParams;


struct TraceFillUnitParams {
    int output_fifo_len;
    int output_interval;
    int align_to_bblock;
    int branch_promote_thresh;          // 0: don't promote
    int allow_indirect_jumps;
};


TraceFillUnit *tfu_create(const TraceFillUnitParams *params,
                          struct CoreResources *core);

void tfu_destroy(TraceFillUnit *tfu);

void tfu_reset(TraceFillUnit *tfu);

void tfu_inst_commit(TraceFillUnit *tfu, const struct context *ctx,
                     const struct activelist *inst);
void tfu_context_threadswap(TraceFillUnit *tfu, const struct context *ctx);

void tfu_process_queue(TraceFillUnit *tfu);


#ifdef __cplusplus
}
#endif

#endif  // TRACE_FILL_UNIT_H 
