//
// Support for injecting instructions into the pipeline
//
// Jeff Brown
// $Id: inject-inst.h,v 1.1.2.1.6.1.2.3 2008/10/24 20:34:31 jbrown Exp $
//

#ifndef INJECT_INST_H
#define INJECT_INST_H

#ifdef __cplusplus
extern "C" {
#endif


// Defined elsewhere
struct context;
struct activelist;
struct CoreResources;


struct activelist *inject_alloc(struct context *ctx);
void inject_at_rename(struct context *ctx, struct activelist *inst);

void service_rename_inject(struct CoreResources *core);

void inject_set_bmtfill(struct activelist *inst, int dst_reg,
                        int is_final, int is_block_start);
void inject_set_bmtspill(struct activelist *inst, int src_reg,
                         int is_final, int is_block_end);


#ifdef __cplusplus
}
#endif

#endif  // INJECT_INST_H
