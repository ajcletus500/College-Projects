//
// "Memory Unit": a simple model of a banked physical memory interface.
//
// Jeff Brown
// $Id: mem-unit.h,v 1.1.2.2.2.1 2009/07/29 10:52:53 jbrown Exp $
//

#ifndef MEM_UNIT_H
#define MEM_UNIT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MemUnit MemUnit;
typedef struct MemUnitStats MemUnitStats;
typedef struct MemBankStats MemBankStats;


typedef enum { MemUnit_Read, MemUnit_Write } MemUnitOp;


struct MemUnitStats {
    i64 reads, writes;
};

// Per-bank usage stats
struct MemBankStats {
    i64 reads, writes;
    double util;
};


// (MemUnitParams from "cache-params.h".)
MemUnit *memunit_create(const struct MemUnitParams *params, i64 now);
void memunit_destroy(MemUnit *mu);
void memunit_reset(MemUnit *mu, i64 now);


// Returns the time at which the operation will complete.

i64 memunit_access(MemUnit *mu, LongAddr addr, i64 now,
                   MemUnitOp mem_op);


void memunit_get_stats(const MemUnit *mu, MemUnitStats *dest);
void memunit_get_bankstats(const MemUnit *mu, i64 now, int bank_num,
                           MemBankStats *dest);


#ifdef __cplusplus
}
#endif

#endif  // MEM_UNIT_H
