//
// Stash: instruction decoding memoization
//
// Jeff Brown
// $Id: stash.cc,v 1.1.2.3.2.1.2.5 2009/07/29 19:24:47 jbrown Exp $
//

const char RCSid_1107479606[] =
"$Id: stash.cc,v 1.1.2.3.2.1.2.5 2009/07/29 19:24:47 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>

#include <map>

#include "sim-assert.h"
#include "hash-map.h"
#include "sys-types.h"
#include "stash.h"
#include "utils.h"
#include "utils-cc.h"
#include "emulate.h"            // for decode_inst() declaration


#define USE_HASHMAP_NOT_MAP             (1 && HAVE_HASHMAP)


#if USE_HASHMAP_NOT_MAP
    typedef hash_map<mem_addr, StashData, StlHashMemAddr> StashInstMap;
#else
    typedef std::map<mem_addr, StashData> StashInstMap;
#endif


struct Stash {
private:
    AppState *as_;
    StashInstMap pc_to_data_;
    NoDefaultCopy no_copy_;

public:
    Stash(AppState *as) : as_(as) { }
    ~Stash() { }

    void reset() {
        pc_to_data_.clear();
    }

    // not named "decode_inst", due to conflict with C function of that name
    const StashData *lookup_decode(mem_addr pc) {
        StashInstMap::iterator ent_iter = pc_to_data_.find(pc);
        StashData *result;
        if (SP_T(ent_iter != pc_to_data_.end())) {
            result = &(ent_iter->second);
        } else {
            // create new entry, using the default constructor.  this doesn't
            // actually initialize the data; we're using this to allocate
            // storage for decode_inst() to use.  and woo, look at that STL
            // syntax.
            result = 
                &(pc_to_data_.insert(std::make_pair(pc, StashData())).
                  first->second);
            // decode_inst returns <0 iff failure.
            int decode_result = decode_inst(as_, result, pc);
            if (SP_F(decode_result < 0)) {
                // failure: delete newly-allocated, bogus entry
                pc_to_data_.erase(pc);
                result = NULL;
            }
        }
        return result;
    }

    bool probe_inst(mem_addr pc) const {
        return pc_to_data_.find(pc) != pc_to_data_.end();
    }

    void flush_inst(mem_addr pc) {
        pc_to_data_.erase(pc);
    }
};



//
// C interface
//

StashData *
stashdata_copy(const StashData *sdata)
{
    // lazy: default copy constructor
    StashData *result = new StashData(*sdata);
    return result;
}

void
stashdata_destroy(StashData *sdata)
{
    delete sdata;
}

Stash *
stash_create(struct AppState *as)
{
    return new Stash(as);
}

void
stash_destroy(Stash *stash)
{
    delete stash;
}

void
stash_reset(Stash *stash)
{
    stash->reset();
}

const StashData *
stash_decode_inst(Stash *stash, mem_addr pc)
{
    return stash->lookup_decode(pc);
}

int
stash_probe_inst(const Stash *stash, mem_addr pc)
{
    return stash->probe_inst(pc);
}

void
stash_flush_inst(Stash *stash, mem_addr pc)
{
    stash->flush_inst(pc);
}
