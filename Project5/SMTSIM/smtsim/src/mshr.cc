//
// MSHR model
// (Miss Status Holding/Handling Register)
//
// Jeff Brown
// $Id: mshr.cc,v 1.1.2.12 2009/08/10 09:57:30 jbrown Exp $
//

const char RCSid_1219437148[] = 
"$Id: mshr.cc,v 1.1.2.12 2009/08/10 09:57:30 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "hash-map.h"
#include "mshr.h"
#include "utils.h"
#include "utils-cc.h"
#include "sim-cfg.h"
#include "main.h"               // For fmt_now(), cyc


using std::map;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using SimCfg::conf_bool;
using SimCfg::conf_int;


// Level 1: report top-level operations
// Level 2: also dump MSHR tables at each operation
#define MSHR_DEBUG_LEVEL        1
#ifdef DEBUG
    // Condition: "should I printf a level-x debug message?"
    #define MSHR_DEBUG_COND(x) ((MSHR_DEBUG_LEVEL >= (x)) && debug)
#else
    #define MSHR_DEBUG_COND(x) (0)
#endif
#define MSHR_DB(x) if (!MSHR_DEBUG_COND(x)) { } else printf


// external enum names
const char *MshrAllocOutcome_names[] = { 
    "Full", "AllocNew", "ReuseOld", NULL
};


namespace {

enum MshrConsumerType {
    MshrCons_Inst,
    MshrCons_Data,
    MshrCons_NestedCache,
    MshrConsumerType_last
};

// internal enum names
const char *MshrConsumerType_names[] = {
    "Inst", "Data", "NestedCache", NULL
};


struct MshrConfig {
    int entry_count;            // distinct trackable blocks ("producers")
    int waiters_per_entry;      // limit on per-entry waiters ("consumers")
    int max_alloc_per_cyc;      // -1: unlimited

    NoDefaultCopy nocopy;

public:
    MshrConfig(const string& cfg_path);
    ~MshrConfig() { }
};


MshrConfig::MshrConfig(const string& cfg_path)
{
    string cp = cfg_path + "/";          // short-hand for config-path

    entry_count = conf_int(cp + "entry_count");
    if (entry_count < 1) {
        exit_printf("bad %s (%d) \n", (cp + "entry_count").c_str(),
                    entry_count);
    }

    waiters_per_entry = conf_int(cp + "waiters_per_entry");
    if (waiters_per_entry < 1) {
        exit_printf("bad %s (%d) \n", (cp + "waiters_per_entry").c_str(),
                    waiters_per_entry);
    }

    max_alloc_per_cyc = conf_int(cp + "max_alloc_per_cyc");
}


class MshrConsumer {
    // We're using a lame tagged-struct representation; this is just a
    // glorified value type for use as a hash key; the sub-types are similar
    // enough without getting inheritance into the picture (yet).
    MshrConsumerType type_;
    // hardware ctx waiting for this request (I + D), or cache (NestedCache)
    int ctx_or_cache_id_;
    // hardware dynamic inst waiting, for Data consumers (-1 otherwise)
    int inst_id_;

    // WARNING: default copy/assignment operators in use

public:
    MshrConsumer(MshrConsumerType type__, int ctx_or_cache_id__, int inst_id__)
        : type_(type__), ctx_or_cache_id_(ctx_or_cache_id__),
          inst_id_(inst_id__) {
        sim_assert(ENUM_OK(MshrConsumerType, type_));
        assert_ifthen(type_ != MshrCons_Data,
                      inst_id_ == -1);
    }

    bool operator < (const MshrConsumer& o2) const {
        return (type_ < o2.type_) ||
            ((type_ == o2.type_) &&
             (ctx_or_cache_id_ < o2.ctx_or_cache_id_)) ||
            ((type_ == o2.type_) &&
             (ctx_or_cache_id_ == o2.ctx_or_cache_id_) &&
             (inst_id_ < o2.inst_id_));
    }
    bool operator == (const MshrConsumer& o2) const {
        return (type_ == o2.type_) &&
            (ctx_or_cache_id_ == o2.ctx_or_cache_id_) &&
            (inst_id_ == o2.inst_id_);
    }
    size_t stl_hash() const {
        StlHashU32 h;
        return h((type_ << 30) ^ (ctx_or_cache_id_ << 16) ^ inst_id_);
    }

    string fmt() const {
        ostringstream ostr;
        switch (type_) {
        case MshrCons_Inst:
            ostr << "T" << ctx_or_cache_id_;
            break;
        case MshrCons_Data:
            ostr << "T" << ctx_or_cache_id_ << "s" << inst_id_;
            break;
        case MshrCons_NestedCache:
            ostr << "Cache" << ctx_or_cache_id_;
            break;
        default:
            ENUM_ABORT(MshrConsumerType, type_);
        }
        return ostr.str();
    }
};


class MshrEntry {
    #if HAVE_HASHMAP
        typedef hash_set<MshrConsumer,
                         StlHashMethod<MshrConsumer> > ConsumerSet;
    #else
        typedef set<MshrConsumer> ConsumerSet;
    #endif // HAVE_HASHMAP

    const MshrConfig *conf_;
    i64 alloc_time_;
    ConsumerSet consumers_;     // place-holder for more useful things, later?
    bool created_for_prefetch_;

public:
    MshrEntry(const MshrConfig *conf__, bool created_for_prefetch__) 
        : conf_(conf__), alloc_time_(cyc),
          created_for_prefetch_(created_for_prefetch__) {
        sim_assert(conf_->waiters_per_entry > 0);
    }
    bool empty() const { return consumers_.empty(); }
    bool full() const { return intsize(consumers_) ==
            conf_->waiters_per_entry;
    }
    int g_count() const { return int(consumers_.size()); }
    bool created_for_prefetch() const { return created_for_prefetch_; }
    void add_consumer(const MshrConsumer& new_cons) {
        sim_assert(intsize(consumers_) < conf_->waiters_per_entry);
        if (!consumers_.insert(new_cons).second) {
            abort_printf("consumer (%s) added when already present\n",
                         new_cons.fmt().c_str());
        }
    }
    void rem_consumer(const MshrConsumer& victim_cons) {
        sim_assert(consumers_.size() > 0);
        if (!consumers_.erase(victim_cons)) {
            abort_printf("consumer (%s) removed when not present\n",
                         victim_cons.fmt().c_str());
        }
    }

    string fmt() const {
        ostringstream ostr;
        ostr << "alloc: " << alloc_time_ << " consumers:";
        FOR_CONST_ITER(ConsumerSet, consumers_, iter) {
            ostr << " " << iter->fmt();
        }
        ostr << " for_prefetch: " << created_for_prefetch_;
        return ostr.str();
    }
};


#if HAVE_HASHMAP
    typedef hash_map<LongAddr, MshrEntry,
                     StlHashMethod<LongAddr> > MshrAddrMap;
#else
    typedef map<LongAddr, MshrEntry> MshrAddrMap;
#endif


} // Anonymous namespace close



struct MshrTable {
private:
    const string name_;         // for debug printing, etc.
    const char *name_c_;        // pointer owned by name_
    const string config_path_;
    const MshrConfig conf_;

    int block_bytes_;
    int block_bytes_lg_;

    MshrAddrMap addr_to_ent_;   // block base addr -> entry
    int prefetch_producers_;    // # of current entries created for PFs

    // We'll count entries freed within a cycle, and pretend they are occupied
    // within the same cycle they're freed, to prevent unintentional
    // "shoot-through" re-use by competing structures in that cycle.  At any
    // later time, we'll consider them free.  (Alternatively, we could
    // schedule a CBQ_Callback to notify us about freeing them later, but that
    // seems like overkill for always-next-cycle freeing.)  We'll similarly
    // limit the number of entries allocated per-cycle, to avoid unintentional
    // locally-infinite-bandwidth scenarios.  (We can't play the freeing trick
    // so easily within an MshrEntry, though, since callers like to free the
    // producer immediately after their last consumer free, which is
    // ordinarily the trigger for recovering storage.  If we care enough, we
    // can use a callback for that.)
    struct {
        i64 cyc;        // last cycle in which entries were allocd/freed
        int count;      // number of entries allocd/freed that cycle
    } last_prod_free_, last_alloc_;
    // warning: though last_prod_free_ and last_alloc_ have the same fields,
    // they're used in different senses

    void align_addr(LongAddr& addr) const {
        addr.a &= ~(block_bytes_ - 1);  // works for power-of-two sizes
    }
    int prod_count() const {
        return intsize(addr_to_ent_) +
            ((cyc > last_prod_free_.cyc) ? 0 : last_prod_free_.count);
    }
    void note_producer_freed() {
        sim_assert(last_prod_free_.cyc <= cyc);
        if (last_prod_free_.cyc < cyc) {
            last_prod_free_.cyc = cyc;  // reset old last_prod_freed info
            last_prod_free_.count = 1;
        } else {
            ++last_prod_free_.count;    // add to this cycle's freed
        }
    }
    void note_entry_allocd() {          // counts any type of alloc operation
        sim_assert(last_alloc_.cyc <= cyc);
        if (last_alloc_.cyc < cyc) {
            last_alloc_.cyc = cyc;
            last_alloc_.count = 1;
        } else {
            ++last_alloc_.count;
        }
    }
    bool too_busy_this_cyc() const {
        return ((conf_.max_alloc_per_cyc < 0) ||
                (cyc > last_alloc_.cyc)) ? false :
            (last_alloc_.count >= conf_.max_alloc_per_cyc);
    }
    bool prod_table_full() const {
        sim_assert(prod_count() <= conf_.entry_count);
        return (prod_count() == conf_.entry_count);
    }

public:
    MshrTable(const char *name__, const char *config_path__,
              int block_bytes__);

    bool is_avail(const LongAddr& addr) const {
        LongAddr base_addr(addr);
        align_addr(base_addr);
        const MshrEntry *ent = map_find(addr_to_ent_, base_addr);
        bool result;
        if (too_busy_this_cyc()) {
            result = false;
        } else {
            result = (ent) ? !ent->full() : !prod_table_full();
        }
        return result;
    }

    // currently used for multiple types of entries (I, D, nested-cache).
    MshrAllocOutcome alloc_consumer(const char *external_fname,
                                    const LongAddr& addr,
                                    const MshrConsumer& new_cons) {
        LongAddr base_addr(addr);
        align_addr(base_addr);
        MshrAllocOutcome result;
        MshrEntry *ent = map_find(addr_to_ent_, base_addr);
        if (too_busy_this_cyc()) {
            result = MSHR_Full;
        } else if (ent) {
            // add consumer to existing producer, if it has room
            if (ent->full()) {
                result = MSHR_Full;
            } else {
                ent->add_consumer(new_cons);
                result = MSHR_ReuseOld;
            }
        } else if (prod_table_full()) {
            // no existing producer, and no free space for one
            result = MSHR_Full;
        } else {
            // add new producer, add this consumer to it
            ent = &map_put_uniq(addr_to_ent_, base_addr,
                                MshrEntry(&conf_, false));
            ent->add_consumer(new_cons);
            result = MSHR_AllocNew;
        }
        if (result != MSHR_Full)
            note_entry_allocd();
        MSHR_DB(1)("%s: %s time %s addr %s cons %s; base_addr %s;"
                   " result %s prod_count %d ent->count %s pf_count %d\n",
                   external_fname, name_c_, fmt_now(), fmt_laddr(addr),
                   new_cons.fmt().c_str(),
                   fmt_laddr(base_addr),
                   ENUM_STR(MshrAllocOutcome, result),
                   prod_count(),
                   (ent) ? fmt_i64(ent->g_count()) : "(undef)",
                   prefetch_producers_);
        if (MSHR_DEBUG_COND(2))
            this->dump(stdout, "  ");
        return result;
    }

    MshrAllocOutcome alloc_prefetch(const LongAddr& base_addr) {
        const char *fname = "mshr_alloc_prefetch";
        MshrAllocOutcome result;
        MshrEntry *ent = map_find(addr_to_ent_, base_addr);
        if (too_busy_this_cyc()) {
            result = MSHR_Full;
        } else if (ent) {
            result = MSHR_ReuseOld;
        } else if (prod_table_full()) {
            // no existing producer, and no free space for one
            result = MSHR_Full;
        } else {
            // add new producer, no consumers
            ent = &map_put_uniq(addr_to_ent_, base_addr,
                                MshrEntry(&conf_, true));
            ++prefetch_producers_;
            sim_assert(prefetch_producers_ <= prod_count());
            result = MSHR_AllocNew;
        }
        if (result != MSHR_Full)
            note_entry_allocd();
        MSHR_DB(1)("%s: %s time %s base_addr %s;"
                   " result %s prod_count %d ent->count %s pf_count %d\n",
                   fname, name_c_, fmt_now(), fmt_laddr(base_addr),
                   ENUM_STR(MshrAllocOutcome, result),
                   prod_count(),
                   (ent) ? fmt_i64(ent->g_count()) : "(undef)",
                   prefetch_producers_);
        if (MSHR_DEBUG_COND(2))
            this->dump(stdout, "  ");
        return result;
    }

    void free_consumer(const char *external_fname, const LongAddr& addr,
                       const MshrConsumer& cons_id) {
        LongAddr base_addr(addr);
        align_addr(base_addr);
        MshrEntry *ent = map_find(addr_to_ent_, base_addr);
        MSHR_DB(1)("%s: %s time %s addr %s cons %s; base_addr %s, "
                   "prod_count was %d, ent->count was %s\n", external_fname,
                   name_c_, fmt_now(), fmt_laddr(addr),
                   cons_id.fmt().c_str(), fmt_laddr(base_addr),
                   prod_count(),
                   (ent) ? fmt_i64(ent->g_count()) : "(undef)");
        if (ent) {
            ent->rem_consumer(cons_id);
        } else {
            fflush(0);
            this->dump(stderr, "");
            abort_printf("%s(%s): no entry for block at %s\n", external_fname,
                         fmt_laddr(addr), fmt_laddr(base_addr));
        }
        if (MSHR_DEBUG_COND(2))
            this->dump(stdout, "  ");
    }

    void free_producer(const LongAddr& base_addr) {
        const char *fname = "mshr_free_producer";
        MshrEntry *ent = map_find(addr_to_ent_, base_addr);
        MSHR_DB(1)("%s: %s time %s base_addr %s for_prefetch %s,"
                   " prod_count was %d, ent->count was %s,"
                   " pf_count was %d\n", fname, name_c_,
                   fmt_now(),
                   fmt_laddr(base_addr),
                   (ent) ? fmt_bool(ent->created_for_prefetch()) : "?",
                   prod_count(),
                   (ent) ? fmt_i64(ent->g_count()) : "(undef)",
                   prefetch_producers_);
        if (ent) {
            if (!ent->empty()) {
                fflush(0);
                this->dump(stderr, "");
                abort_printf("%s: entry %s not empty\n", fname,
                             fmt_laddr(base_addr));
            }
            if (ent->created_for_prefetch())
                --prefetch_producers_;
            sim_assert(prefetch_producers_ >= 0);
            sim_assert(prefetch_producers_ <= prod_count());
            addr_to_ent_.erase(base_addr);      // invalidates "ent"
        } else {
            abort_printf("%s: no entry for block at %s\n", fname,
                         fmt_laddr(base_addr));
        }
        note_producer_freed();
        if (MSHR_DEBUG_COND(2))
            this->dump(stdout, "  ");
    }

    bool any_producer(const LongAddr& base_addr) const {
        const MshrEntry *ent = map_find(addr_to_ent_, base_addr);
        bool result = (ent != NULL);
        return result;
    }

    bool any_consumers(const LongAddr& base_addr) const {
        const MshrEntry *ent = map_find(addr_to_ent_, base_addr);
        bool result = ent && !ent->empty();
        return result;
    }

    int count_prefetch_producers() const {
        return prefetch_producers_;
    }

    void dump(FILE *out, const char *pf) const;
};


MshrTable::MshrTable(const char *name__, const char *config_path__,
                     int block_bytes__)
    : name_(name__), config_path_(config_path__), conf_(config_path_), 
      block_bytes_(block_bytes__), prefetch_producers_(0)
{
    name_c_ = name_.c_str();

    if (block_bytes_ < 1) {
        exit_printf("bad MSHR block_bytes (%d)\n", block_bytes_);
    }
    if ((block_bytes_lg_ = log2_exact(block_bytes_)) < 0) {
        exit_printf("MSHR block_bytes (%d) not a power of 2\n", block_bytes_);
    }
    last_prod_free_.cyc = 0;
    last_prod_free_.count = 0;
    last_alloc_.cyc = 0;
    last_alloc_.count = 0;
}


void
MshrTable::dump(FILE *out, const char *pf) const
{
    fprintf(out, "%smshr table %s, %d of %d entries in use (pf_count %d)\n"
            "%slast_alloc %d at cyc %s, last_prod_free %d at cyc %s,"
            " entries:\n", pf, name_c_, prod_count(), conf_.entry_count,
            prefetch_producers_,
            pf, last_alloc_.count, fmt_i64(last_alloc_.cyc),
            last_prod_free_.count, fmt_i64(last_prod_free_.cyc));
    vector<LongAddr> sorted_addrs;
    FOR_CONST_ITER(MshrAddrMap, addr_to_ent_, iter) {
        sorted_addrs.push_back(iter->first);
    }
    std::stable_sort(sorted_addrs.begin(), sorted_addrs.end());
    FOR_CONST_ITER(vector<LongAddr>, sorted_addrs, iter) {
        const MshrEntry& ent = map_at(addr_to_ent_, *iter);
        fprintf(out, "%s  %s -> %s\n", pf, fmt_laddr(*iter),
                ent.fmt().c_str());
    }
}



//
// C interface
//

MshrTable *
mshr_create(const char *name, const char *config_path, int block_bytes)
{
    return new MshrTable(name, config_path, block_bytes);
}

void
mshr_destroy(MshrTable *mshr)
{
    delete mshr;
}

int
mshr_is_avail(const MshrTable *mshr, LongAddr addr)
{
    return mshr->is_avail(addr);
}

MshrAllocOutcome
mshr_alloc_inst(MshrTable *mshr, LongAddr addr, int ctx_id)
{
    MshrConsumer new_cons(MshrCons_Inst, ctx_id, -1);
    return mshr->alloc_consumer("mshr_alloc_inst", addr, new_cons);
}

MshrAllocOutcome
mshr_alloc_data(MshrTable *mshr, LongAddr addr, int ctx_id, int inst_id)
{
    sim_assert(inst_id >= 0);
    MshrConsumer new_cons(MshrCons_Data, ctx_id, inst_id);
    return mshr->alloc_consumer("mshr_alloc_data", addr, new_cons);
}

MshrAllocOutcome
mshr_alloc_nestedcache(MshrTable *mshr, LongAddr addr, 
                       int requesting_cache_id)
{
    MshrConsumer new_cons(MshrCons_NestedCache, requesting_cache_id, -1);
    return mshr->alloc_consumer("mshr_alloc_nestedcache", addr, new_cons);
}

MshrAllocOutcome
mshr_alloc_prefetch(MshrTable *mshr, LongAddr base_addr)
{
    return mshr->alloc_prefetch(base_addr);
}

void
mshr_cfree_inst(MshrTable *mshr, LongAddr addr, int ctx_id)
{
    MshrConsumer cons_id(MshrCons_Inst, ctx_id, -1);
    mshr->free_consumer("mshr_free_inst", addr, cons_id);
}

void
mshr_cfree_data(MshrTable *mshr, LongAddr addr, int ctx_id, int inst_id)
{
    sim_assert(inst_id >= 0);
    MshrConsumer cons_id(MshrCons_Data, ctx_id, inst_id);
    mshr->free_consumer("mshr_free_data", addr, cons_id);
}

void
mshr_cfree_nestedcache(MshrTable *mshr, LongAddr addr, int requesting_cache_id)
{
    MshrConsumer cons_id(MshrCons_NestedCache, requesting_cache_id, -1);
    mshr->free_consumer("mshr_free_nestedcache", addr, cons_id);
}

void
mshr_free_producer(MshrTable *mshr, LongAddr base_addr)
{
    mshr->free_producer(base_addr);
}

int
mshr_any_producer(const MshrTable *mshr, LongAddr base_addr)
{
    return mshr->any_producer(base_addr);
}

int
mshr_any_consumers(const MshrTable *mshr, LongAddr base_addr)
{
    return mshr->any_consumers(base_addr);
}

int
mshr_count_prefetch_producers(const MshrTable *mshr)
{
    return mshr->count_prefetch_producers();
}

void
mshr_dump(const MshrTable *mshr, void *c_FILE_out, const char *prefix)
{
    mshr->dump(static_cast<FILE *>(c_FILE_out), prefix);
}
