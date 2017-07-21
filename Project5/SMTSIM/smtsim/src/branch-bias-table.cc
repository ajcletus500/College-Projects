//
// Branch bias table
//
// Jeff Brown
// $Id: branch-bias-table.cc,v 1.2.2.3.2.1.2.1 2008/11/12 23:23:13 jbrown Exp $
//

const char RCSid_1055261740[] = 
"$Id: branch-bias-table.cc,v 1.2.2.3.2.1.2.1 2008/11/12 23:23:13 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "branch-bias-table.h"
#include "assoc-array.h"
#include "utils.h"


class BBTEntry {
    // Bits 31...2: count, bit 1: single fault pending, bit 0: taken
    u32 bits;

public:
    BBTEntry() { }

    void reset() { bits = 0; }
    bool get_taken() const { return bits & 1; }
    i32 get_count() const { return static_cast<i32>(bits >> 2); }

    void update(bool taken) {
        if (taken == (bits & 1)) {
            u32 new_bits = bits + (1 << 2);
            if (new_bits > bits)
                bits = new_bits;
        } else if (!(bits & 2)) {
            // Don't zero out counter until two consecutive faults
            bits |= 2;
        } else {
            bits = (1 << 2) | taken;
        }
    }
};


struct BranchBiasTable {
private:
    long n_entries;
    int inst_bytes;

    long n_lines;
    int assoc;
    int inst_bytes_lg;

    AssocArray *cam;
    BBTEntry *entries;
    
    void calc_lookup_key(AssocArrayKey& key_ret, mem_addr addr, int thread) {
        key_ret.lookup = addr >> inst_bytes_lg;
        key_ret.match = thread;
    }

public:
    BranchBiasTable(long n_entries_, int inst_bytes_);
    ~BranchBiasTable();
    
    void reset();
    void reset_stats();

    bool lookup(mem_addr addr, int thread, int *taken_ret, i32 *count_ret) {
        AssocArrayKey lookup_key;
        calc_lookup_key(lookup_key, addr, thread);
        long line;
        int way;
        bool hit = aarray_lookup(cam, &lookup_key, &line, &way);
        if (hit) {
            BBTEntry& ent = entries[line * assoc + way];
            if (taken_ret)
                *taken_ret = ent.get_taken();
            if (count_ret)
                *count_ret = ent.get_count();
        }
        return hit;
    }

    void update(mem_addr addr, int thread, int taken) {
        AssocArrayKey lookup_key;
        calc_lookup_key(lookup_key, addr, thread);
        long line;
        int way;
        if (!aarray_probe(cam, &lookup_key, &line, &way))
            aarray_replace(cam, &lookup_key, &line, &way, NULL);
        BBTEntry& ent = entries[line * assoc + way];
        ent.update(taken);
    }
};


BranchBiasTable::BranchBiasTable(long n_entries_, int inst_bytes_)
    : n_entries(n_entries_), inst_bytes(inst_bytes_),
      cam(0), entries(0)
{
    const char *func = "BranchBiasTable::BranchBiasTable";

    sim_assert(n_entries > 0);
    sim_assert(inst_bytes > 0);

    n_lines = n_entries;
    assoc = 1;

    if ((inst_bytes_lg = log2_exact(inst_bytes)) < 0) {
        fprintf(stderr, "%s (%s:%i): inst_bytes (%i) not a power of 2\n",
                func, __FILE__, __LINE__, inst_bytes);
        exit(1);
    }

    cam = aarray_create(n_lines, assoc, "LRU");
    entries = new BBTEntry[n_lines * assoc];

    reset();
}


BranchBiasTable::~BranchBiasTable()
{
    aarray_destroy(cam);
    if (entries)
        delete[] entries;
}


void
BranchBiasTable::reset() 
{
    aarray_reset(cam);
    for (long ent = 0; ent < n_entries; ent++)
        entries[ent].reset();

    reset_stats();
}


void
BranchBiasTable::reset_stats() 
{
}


//
// C interface
//

BranchBiasTable *
bbt_create(long n_entries, int inst_bytes)
{
    return new BranchBiasTable(n_entries, inst_bytes);
}

void 
bbt_destroy(BranchBiasTable *bbt)
{
    if (bbt)
        delete bbt;
}

void 
bbt_reset(BranchBiasTable *bbt)
{
    bbt->reset();
}

void 
bbt_reset_stats(BranchBiasTable *bbt)
{
    bbt->reset_stats();
}

int 
bbt_lookup(BranchBiasTable *bbt, mem_addr addr, int thread, int *taken_ret,
           i32 *count_ret)
{
    return bbt->lookup(addr, thread, taken_ret, count_ret);
}

void 
bbt_update(BranchBiasTable *bbt, mem_addr addr, int thread, int taken)
{
    bbt->update(addr, thread, taken);
}
