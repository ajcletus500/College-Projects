//
// Branch bias table
//
// Jeff Brown
// $Id: branch-bias-table.h,v 1.2.2.1.6.1 2008/04/30 22:17:45 jbrown Exp $
//

#ifndef BRANCH_BIAS_TABLE_H
#define BRANCH_BIAS_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct BranchBiasTable BranchBiasTable;


BranchBiasTable *bbt_create(long n_entries, int inst_bytes);
void bbt_destroy(BranchBiasTable *bbt);

void bbt_reset(BranchBiasTable *bbt);
void bbt_reset_stats(BranchBiasTable *bbt);

int bbt_lookup(BranchBiasTable *bbt, mem_addr addr, int thread, 
               int *taken_ret, i32 *count_ret);

void bbt_update(BranchBiasTable *bbt, mem_addr addr, int thread, int taken);


#ifdef __cplusplus
}
#endif

#endif  /* BRANCH_BIAS_TABLE_H */
