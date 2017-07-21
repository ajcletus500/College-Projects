//
// "Memory Unit": a simple model of a banked physical memory interface.
//
// Jeff Brown
// $Id: mem-unit.cc,v 1.1.2.2.2.1 2009/07/29 10:52:52 jbrown Exp $
//

const char RCSid_1192500914[] = 
"$Id: mem-unit.cc,v 1.1.2.2.2.1 2009/07/29 10:52:52 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "cache-params.h"
#include "mem-unit.h"
#include "utils.h"

using std::vector;


namespace {

class MemBank {
    // Cycle# one past the completion of the most recent request completes:
    // the cycle that the result is available, and in which a new request
    // may begin.
    i64 ready_cyc;
    MemBankStats stats;

public:
    MemBank() { }
    void reset() { ready_cyc = 0; }
    void reset_stats() {
        stats.reads = stats.writes = 0;
    }
    i64 bill_time(i64 now, OpTime op_time, bool is_write) {
        i64 req_done_time;
        bill_resource_time(req_done_time, ready_cyc, now, op_time);
        if (is_write)
            stats.writes++;
        else
            stats.reads++;
        return req_done_time;
    }
    void get_stats(MemBankStats *dest) const {
        // Leaves "util" undefined, for caller to fill in
        *dest = stats;
    }
};

} // Anonymous namespace close



struct MemUnit {
private:
    MemUnitParams params;
    int block_bytes_lg, n_banks_lg;
    vector<MemBank> banks;              // 1D array [n_banks]
    i64 stats_reset_cyc;

    inline int block_bank_num(const LongAddr &addr) const {
        return static_cast<int>((addr.a >> block_bytes_lg) 
                                & (params.n_banks - 1));
    }

public:
    MemUnit(const MemUnitParams *params_, i64 now);
    ~MemUnit() { };
    void reset(i64 now);
    void reset_stats(i64 now);

    i64 access(const LongAddr& addr, i64 now, MemUnitOp mem_op) {
        int bank_num = block_bank_num(addr);
        MemBank& bank = banks[bank_num];
        i64 ready_time;
        sim_assert((mem_op == MemUnit_Read) || (mem_op == MemUnit_Write));
        bool is_write = (mem_op != MemUnit_Read);
        ready_time =
            bank.bill_time(now, (is_write) ? params.write_time
                           : params.read_time, is_write);
        return ready_time;
    }

    void get_stats(MemUnitStats *dest) const;
    void get_bankstats(i64 now, int bank_num, MemBankStats *dest) const;
};


MemUnit::MemUnit(const MemUnitParams *params_, i64 now)
    : params(*params_)
{
    block_bytes_lg = log2_exact(params.block_bytes);
    if (block_bytes_lg < 0) {
        err_printf("block_bytes (%i) not a power of 2\n", params.block_bytes);
        goto fail;
    }

    n_banks_lg = log2_exact(params.n_banks);
    if (n_banks_lg < 0) {
        err_printf("n_banks (%i) not a power of 2\n", params.n_banks);
        goto fail;
    }

    for (int bnum = 0; bnum < params.n_banks; bnum++)
        banks.push_back(MemBank());

    reset(now);
    return;

fail:
    exit_printf("MemUnit creation failed\n");
}


void
MemUnit::reset(i64 now)
{
    for (vector<MemBank>::iterator iter = banks.begin(); iter != banks.end();
         ++iter) {
        iter->reset();
    }
    reset_stats(now);
}


void
MemUnit::reset_stats(i64 now)
{
    stats_reset_cyc = now;
    for (vector<MemBank>::iterator iter = banks.begin(); iter != banks.end();
         ++iter) {
        iter->reset_stats();
    }
    // We don't currently store any top-level stats in MemUnit; we just
    // synthesize stats on demand from per-bank stats.
}


void
MemUnit::get_stats(MemUnitStats *dest) const
{
    dest->reads = 0;
    dest->writes = 0;
    for (vector<MemBank>::const_iterator iter = banks.begin();
         iter != banks.end(); ++iter) {
        const MemBank& bank = *iter;
        MemBankStats bank_stats;
        bank.get_stats(&bank_stats);
        dest->reads += bank_stats.reads;
        dest->writes += bank_stats.writes;
    }
}


void
MemUnit::get_bankstats(i64 now, int bank_num, MemBankStats *dest) const
{
    const MemBank& bank = banks.at(bank_num);
    bank.get_stats(dest);
    dest->util =
        ((double) params.read_time.interval * dest->reads) +
        ((double) params.write_time.interval * dest->writes);
    dest->util /= (now - stats_reset_cyc);
}


//
// C interface
//

MemUnit *
memunit_create(const MemUnitParams *params, i64 now)
{
    return new MemUnit(params, now);
}

void
memunit_destroy(MemUnit *mu)
{
    delete mu;
}

void
memunit_reset(MemUnit *mu, i64 now)
{
    mu->reset(now);
}

i64
memunit_access(MemUnit *mu, LongAddr addr, i64 now,
               MemUnitOp mem_op)
{
    return mu->access(addr, now, mem_op);
}

void
memunit_get_stats(const MemUnit *mu, MemUnitStats *dest)
{
    mu->get_stats(dest);
}

void
memunit_get_bankstats(const MemUnit *mu, i64 now, int bank_num,
                      MemBankStats *dest)
{
    mu->get_bankstats(now, bank_num, dest);
}
