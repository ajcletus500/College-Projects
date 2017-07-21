/*
 * Inter-core cache coherence manager
 *
 * Jeff Brown
 * $Id: coherence-mgr.cc,v 1.11.6.3.2.3.2.18 2009/07/29 19:24:45 jbrown Exp $
 */

const char RCSid_1037074903[] =
"$Id: coherence-mgr.cc,v 1.11.6.3.2.3.2.18 2009/07/29 19:24:45 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim-assert.h"
#include "hash-map.h"
#include "sys-types.h"
#include "coherence-mgr.h"
#include "core-resources.h"
#include "utils.h"
#include "utils-cc.h"
#include "sim-cfg.h"
#include "prng.h"

using std::string;
using std::ostringstream;
using std::vector;


// Level 1: report top-level operations
#define COHER_DEBUG_LEVEL       1
#ifdef DEBUG
    // Condition: "should I printf a level-x debug message?"
    #define COHER_DEBUG_COND(x) ((COHER_DEBUG_LEVEL >= (x)) && debug)
#else
    #define COHER_DEBUG_COND(x) (0)
#endif
#define COHER_DB(x) if (!COHER_DEBUG_COND(x)) { } else printf


#define USE_HASHMAP_NOT_MAP     (1 && HAVE_HASHMAP)

const char *CoherAccessType_names[] = {
    "InstRead", "DataRead", "DataReadExcl", NULL
};
const char *CoherAccessResult_names[] = {
    "NoStall", "EntryBusy", "StallForInvl", "StallForWB", "StallForXfer",
    "StallForShared", NULL
};


namespace {

typedef int CacheID;
typedef std::set<CacheID> CacheIDSet;

enum CoherEntryState { Coher_Shared, Coher_Exclusive, CoherEntryState_last };
const char *CoherEntryState_names[] = { "Shared", "Exclusive", NULL };

class CoherEntry {
    bool busy_shared_;          // protected access to SHARED memory underway
    // warning: test "is_busy()" before inferring from state/holders
    CoherEntryState state_;     // current(!busy)/next(busy) state
    CacheIDSet holders_;        // current(!busy)/next(busy) holders
    // outstanding replies expected; nonempty <=> access to peers underway
    CacheIDSet waiting_for_;

    inline bool invariant() const {
        bool result;
        if (state_ == Coher_Exclusive) {
            result = (holders_.size() == 1);
        } else if (state_ == Coher_Shared) {
            result = !holders_.empty();
        } else {
            result = false;
        }
        if (busy_shared_ && !waiting_for_.empty())
            result = false;
        if (!result) {
            err_printf("coher: entry invariant false: %s\n", fmt().c_str());
        }
        return result;
    }

public:
    CoherEntry(CoherEntryState init_state, CacheID first_holder) 
        : busy_shared_(false), state_(init_state) {
        holders_.insert(first_holder);
        sim_assert(invariant());
    }

    void assign(CoherEntryState new_state, CacheID first_holder) {
        sim_assert(invariant());
        sim_assert(first_holder >= 0);
        holders_.clear();
        holders_.insert(first_holder);
        state_ = new_state;
        sim_assert(invariant());
    }

    CoherEntryState get_state() const { return state_; }

    // Holder must not be in entry
    void add_holder(CacheID holder) {
        sim_assert(invariant());
        sim_assert(!holders_.count(holder));
        if (state_ == Coher_Exclusive) {
            state_ = Coher_Shared;
        }
        holders_.insert(holder);
        sim_assert(invariant());
    }

    // Holder must be in entry; be sure to check "any_holders()" afterwards
    // and remove the entire CoherEntry if it's empty (to maintain invariant)
    void remove_holder(CacheID holder) {
        sim_assert(invariant());
        sim_assert(holders_.count(holder));
        holders_.erase(holder);
        // if empty, caller must remove us
        sim_assert(!any_holders() || invariant());
    }

    bool is_holder(CacheID holder) const {
        bool result;
        sim_assert(holder >= 0);
        result = holders_.count(holder) > 0;
        return result;
    }
    const CacheIDSet& g_holders() const { return holders_; }

    bool any_holders() const {
        return !holders_.empty();
    }

    CacheID get_excl_holder() const {
        sim_assert(state_ == Coher_Exclusive);
        sim_assert(holders_.size() == 1);
        return *(holders_.begin());
    }

    bool is_busy_peers() const {
        return !waiting_for_.empty();
    }
    bool is_busy() const {
        return is_busy_peers() || busy_shared_;
    }
    void start_busy_shared() {
        sim_assert(!is_busy());
        busy_shared_ = true;
    }
    void end_busy_shared() {
        sim_assert(busy_shared_);
        busy_shared_ = false;
    }
    void start_busy_peers(const CacheIDSet& to_wait_for) {
        sim_assert(!is_busy());
        sim_assert(!to_wait_for.empty());
        waiting_for_ = to_wait_for;      // copy set
    }

    bool is_waiting_for(CacheID peer) const {
        return waiting_for_.count(peer);
    }
    // returns true iff this was the last reply expected
    bool reply_from(CacheID peer) {
        sim_assert(waiting_for_.count(peer));
        waiting_for_.erase(peer);
        return waiting_for_.empty();
    }

    string fmt() const {
        ostringstream ostr;
        ostr << ENUM_STR(CoherEntryState, state_);
        if (is_busy()) {
            ostr << "(busy";
            if (is_busy_peers())
                ostr << "_peers";
            if (busy_shared_)
                ostr << "_shared";
            ostr << ")";
        }
        ostr << " h{";
        FOR_CONST_ITER(CacheIDSet, holders_, i) {
            ostr << " " << *i;
        }
        ostr << " } w{";
        FOR_CONST_ITER(CacheIDSet, waiting_for_, i) {
            ostr << " " << *i;
        }
        ostr << " }";
        return ostr.str();
    }
};


CoherWaitInfo *
coherwaitinfo_create(int invl_needed, int node_count)
{
    CoherWaitInfo *cwi = new CoherWaitInfo;
    cwi->invl_needed = invl_needed;
    cwi->node_count = node_count;
    cwi->nodes = (node_count) ? (new int[node_count]) : NULL;
    return cwi;
}


#if USE_HASHMAP_NOT_MAP
    typedef hash_map<LongAddr, CoherEntry,
                     StlHashMethod<LongAddr> > CoherAddrMap;
#else
    typedef std::map<LongAddr, CoherEntry> CoherAddrMap;
#endif

}       // Anonymous namespace close


struct CoherenceMgr {
protected:
    CoherAddrMap addr_to_entry_;
    vector<CacheArray *> caches_;               // cache_id -> cache
    vector<CoreResources *> parent_cores_;      // cache_id -> parent core
    bool apply_evict_notifies_;
    bool prefer_neighbor_shared_;
    PRNGState local_prng_;
    NoDefaultCopy no_copy_;

    CoherWaitInfo *gen_wait_info(const CacheIDSet& to_wait_for,
                                 bool invl_for_excl);
    CoherWaitInfo *
    invalidate_shared_write(CoherEntry *ent, const LongAddr& addr,
                            int writer_cid);
    CoherWaitInfo *
    invalidate_excl_rw(CoherEntry *ent, const LongAddr& addr,
                       int requestor_cid, bool transfer_owner);
    CoherWaitInfo *
    read_shared_neighbor(CoherEntry *ent, const LongAddr& addr,
                         int requestor_cid);

public:
    CoherenceMgr();
    ~CoherenceMgr() { }

    void reset() {
        addr_to_entry_.clear();
    }

    void reset_cache(int cache_id);
    void reset_entry(const LongAddr& base_addr);

    void add_cache(CacheArray *cache, int cache_id,
                   CoreResources *parent_core) {
        sim_assert(cache_id >= 0);
        sim_assert(parent_core != NULL);
        if (intsize(caches_) < (cache_id + 1)) {
            caches_.resize(cache_id + 1);
            parent_cores_.resize(cache_id + 1);
        }
        sim_assert(caches_[cache_id] == NULL);
        sim_assert(parent_cores_[cache_id] == NULL);
        caches_.at(cache_id) = cache;
        parent_cores_.at(cache_id) = parent_core;
    }

    CoherAccessResult
    access(const LongAddr& base_addr, int cache_id,
           CoherAccessType access_type,
           CoherWaitInfo **coher_wait_ret, int *writeable_ret);
    void shared_request(const LongAddr& base_addr);
    void shared_reply(const LongAddr& base_addr);
    int peer_reply(const LongAddr& base_addr, int reply_cache_id);

    void evict_notify(const LongAddr& base_addr, int cache_id);

    bool holder_okay(const LongAddr& base_addr, int cache_id, 
                     bool dirty, bool writeable) const;

    long entry_count() const { return long(addr_to_entry_.size()); }
};


CoherenceMgr::CoherenceMgr()
{
    prng_reset(&local_prng_, 69369601L);        // constant seed
    apply_evict_notifies_ =
        SimCfg::conf_bool("Global/Mem/Coher/apply_evict_notifies");
    prefer_neighbor_shared_ =
        SimCfg::conf_bool("Global/Mem/Coher/prefer_neighbor_shared");
}


// Note: here, we're assigning an order to the peers that this request
// needs to wait for.  Currently, they're sorted by cache ID, and
// the caller in cache.c re-uses this order when sending out invalidates.
//
// There's room for more intelligence in ordering these requests, especially
// if we move away from a shared-bus interconnect.
CoherWaitInfo *
CoherenceMgr::gen_wait_info(const CacheIDSet& to_wait_for,
                            bool invl_for_excl)
{
    CoherWaitInfo *cwi = coherwaitinfo_create(invl_for_excl,
                                              to_wait_for.size());
    if (cwi->node_count > 0) {
        sim_assert(cwi->node_count == intsize(to_wait_for));
        int next_write = 0;
        FOR_CONST_ITER(CacheIDSet, to_wait_for, iter) {
            cwi->nodes[next_write] = *iter;
            ++next_write;
        }
    }
    return cwi;
}


CoherWaitInfo *
CoherenceMgr::invalidate_shared_write(CoherEntry *ent, const LongAddr& addr,
                                      int writer_cid)
{
    CoherWaitInfo *cwi;
    sim_assert(ent->get_state() == Coher_Shared);

    CacheIDSet to_inval = ent->g_holders();      // set copy
    to_inval.erase(writer_cid);
    cwi = gen_wait_info(to_inval, true);        // allocate return object
    if (!to_inval.empty()) {
        ent->start_busy_peers(to_inval);
    }
    ent->assign(Coher_Exclusive, writer_cid);
    return cwi;
}


CoherWaitInfo *
CoherenceMgr::invalidate_excl_rw(CoherEntry *ent, const LongAddr& addr,
                                 int requestor_cid, bool transfer_owner)
{
    CoherWaitInfo *cwi;
    sim_assert(ent->get_state() == Coher_Exclusive);

    CacheIDSet to_wait_for = ent->g_holders();   // set copy
    to_wait_for.erase(requestor_cid);
    sim_assert(intsize(to_wait_for) == 1);
    cwi = gen_wait_info(to_wait_for, transfer_owner);      // alloc return obj
    if (transfer_owner) {
        ent->assign(Coher_Exclusive, requestor_cid);
    } else {
        ent->add_holder(requestor_cid);
    }
    ent->start_busy_peers(to_wait_for);
    return cwi;
}


CoherWaitInfo *
CoherenceMgr::read_shared_neighbor(CoherEntry *ent, const LongAddr& addr,
                                   int requestor_cid)
{
    CoherWaitInfo *cwi;
    sim_assert(ent->get_state() == Coher_Shared);

    CacheIDSet to_ask = ent->g_holders();        // set copy
    to_ask.erase(requestor_cid);
    if (!to_ask.empty()) {
        // select a single holder at random
        int rand_walk = prng_next_long(&local_prng_) % to_ask.size();
        int rand_id = -1;
        FOR_CONST_ITER(CacheIDSet, to_ask, iter) {
            if (rand_walk == 0) {   // count-down steps to desired element
                rand_id = *iter;
                break;
            }
            --rand_walk;
        }
        sim_assert((rand_id >= 0) && (rand_id != requestor_cid));
        to_ask.clear();
        to_ask.insert(rand_id);
    }

    cwi = gen_wait_info(to_ask, false);         // alloc return object
    if (!to_ask.empty()) {
        ent->start_busy_peers(to_ask);
    }
    return cwi;
}


void 
CoherenceMgr::reset_cache(int cache_id)
{
    // I'm not yet totally clear on what the semantics of this ought to be,
    // but for now it's safe to no-op it; the coherence manager will still
    // assuming that the caches hold things which have been erased in the
    // reset, but that's OK, and it avoids having to worry about in-flight
    // requests.
    return;

    typedef std::vector<LongAddr> CoherAddrVec;
    CoherAddrVec emptied;
    // Warning: this hasn't been well-testing since the addition of
    // "busy" / "waiting_for" stuff.

    // Warning: this doesn't take care of in-flight cache requests, which
    // may lead to "unauthorized" fills.
    FOR_ITER(CoherAddrMap, addr_to_entry_, addr_ent) {
        CoherEntry &ent = addr_ent->second;
        if (ent.is_holder(cache_id) && !ent.is_busy()) {
            ent.remove_holder(cache_id);
            if (!ent.any_holders()) {
                emptied.push_back(addr_ent->first);
            }
        }
    }
    while (!emptied.empty()) {
        LongAddr& to_erase = emptied.back();
        addr_to_entry_.erase(to_erase);
        emptied.pop_back();
    }
}


void
CoherenceMgr::reset_entry(const LongAddr& base_addr)
{
    COHER_DB(1)("coher: reset_entry: base_addr %s:", fmt_laddr(base_addr));

    CoherEntry *ent = map_find(addr_to_entry_, base_addr);
    if (ent) {
        COHER_DB(1)(" %s\n", ent->fmt().c_str());
        addr_to_entry_.erase(base_addr);
        ent = NULL;
    } else {
        COHER_DB(1)("(uncached)\n");
    }
}


CoherAccessResult
CoherenceMgr::access(const LongAddr& base_addr, int cache_id,
                     CoherAccessType access_type,
                     CoherWaitInfo **coher_wait_ret,
                     int *writeable_ret)
{
    CoherAccessResult result = Coher_NoStall;
    CoherEntryState old_state, new_state;
    bool excl_access = (access_type == Coher_DataReadExcl);

    CoherEntry *ent = map_find(addr_to_entry_, base_addr);

    COHER_DB(1)("coher: access: base_addr %s cache_id %d "
                "access_type %s: ", fmt_laddr(base_addr), cache_id,
                CoherAccessType_names[access_type]);

    if (ent != NULL) {
        // Block already cached
        bool requestor_was_holder = ent->is_holder(cache_id);
        old_state = ent->get_state();

        COHER_DB(1)("%s", ent->fmt().c_str());

        if (ent->is_busy()) {
            result = Coher_EntryBusy;
        } else if (old_state == Coher_Shared) {
            // Unmodified, held by >= 1 sharers
            if (excl_access) {
                // If the accessing cache is already holds the data, only a
                // short invalidate needs to traverse the bus; otherwise, an
                // entire data transfer will be necessary.  (We could go to a
                // lower level of cache after invalidating, but we might as
                // well just assume that we ask one holder to write its
                // (clean) copy out on the bus.)
                if (requestor_was_holder) {
                    result = Coher_StallForInvl;
                } else {
                    result = Coher_StallForXfer;
                }
                CoherWaitInfo *cwi =
                    invalidate_shared_write(ent, base_addr, cache_id);
                if (cwi->node_count) {
                    *coher_wait_ret = cwi;
                } else {
                    result = Coher_NoStall;
                    coherwaitinfo_destroy(cwi);
                }
            } else {
                if (prefer_neighbor_shared_) {
                    CoherWaitInfo *cwi =
                        read_shared_neighbor(ent, base_addr, cache_id);
                    if (cwi->node_count) {
                        *coher_wait_ret = cwi;
                        result = Coher_StallForShared;
                    } else {
                        coherwaitinfo_destroy(cwi);
                    }
                }
                if (!requestor_was_holder)
                    ent->add_holder(cache_id);
            }
        } else if (old_state == Coher_Exclusive) {
            // Held by 1 owner, possibly modified
            if (ent->get_excl_holder() != cache_id) {
                // (We have the option of also transferring ownership for
                // non-exclusive accesses, but we don't at the moment->)
                bool transfer_owner = excl_access;
                if (transfer_owner) {   // excl->excl ownership transfer
                    result = Coher_StallForXfer;
                } else {                // excl->shared downgrade
                    // The data may be clean, in which case a full writeback
                    // from the exclusive holder isn't strictly required;
                    // however, since it has the data and it's right next to
                    // the requestor, it may as well write even clean data,
                    // versus transferring another copy from below.
                    // (We currently just gloss over "writeback arrives after
                    // data eviction" situations.) 
                    result = Coher_StallForWB;
                }
                CoherWaitInfo *cwi = 
                    invalidate_excl_rw(ent, base_addr, cache_id,
                                       transfer_owner);
                sim_assert(cwi->node_count > 0);
                *coher_wait_ret = cwi;
            }
        }
        new_state = ent->get_state();
    } else {
        COHER_DB(1)("uncached");
        // Have data accesses to uncached blocks default to exclusive mode,
        // but instruction access default to shared.
        new_state = (access_type == Coher_InstRead) 
            ? Coher_Shared : Coher_Exclusive;
        ent = & map_put_uniq(addr_to_entry_, base_addr,
                             CoherEntry(new_state, cache_id));
    }

    COHER_DB(1)(" -> %s %s\n", ent->fmt().c_str(),
                ENUM_STR(CoherAccessResult, result));
    if (writeable_ret)
        *writeable_ret = (result != Coher_EntryBusy) &&
            (new_state == Coher_Exclusive);
    return result;
}


void
CoherenceMgr::shared_request(const LongAddr& base_addr)
{
    const char *fname = "shared_request";
    CoherEntry *ent = map_find(addr_to_entry_, base_addr);
    COHER_DB(1)("coher: %s: base_addr %s;", fname, fmt_laddr(base_addr));
    if (!ent) {
        abort_printf("error: shared request for unregistered block %s\n",
                     fmt_laddr(base_addr));
    }
    COHER_DB(1)(" %s", ent->fmt().c_str());
    ent->start_busy_shared();
    COHER_DB(1)("\n");
}


void
CoherenceMgr::shared_reply(const LongAddr& base_addr)
{
    const char *fname = "shared_reply";
    CoherEntry *ent = map_find(addr_to_entry_, base_addr);
    COHER_DB(1)("coher: %s: base_addr %s;", fname, fmt_laddr(base_addr));
    if (!ent) {
        abort_printf("error: shared reply for unregistered block %s\n",
                     fmt_laddr(base_addr));
    }
    COHER_DB(1)(" %s", ent->fmt().c_str());
    ent->end_busy_shared();
    COHER_DB(1)("\n");
}


int
CoherenceMgr::peer_reply(const LongAddr& base_addr, int reply_cache_id)
{
    const char *fname = "peer_reply";
    CoherEntry *ent = map_find(addr_to_entry_, base_addr);
    COHER_DB(1)("coher: %s: base_addr %s cache_id %d:", fname,
                fmt_laddr(base_addr), reply_cache_id);
    if (!ent) {
        abort_printf("error: coher reply from cache #%d for "
                     "uncached block %s\n", reply_cache_id,
                     fmt_laddr(base_addr));
    }
    COHER_DB(1)(" %s", ent->fmt().c_str());
    sim_assert(ent->is_busy());
    int is_final = ent->reply_from(reply_cache_id);
    COHER_DB(1)(" -> is_final %d\n", is_final);
    return is_final;
}


void 
CoherenceMgr::evict_notify(const LongAddr& base_addr, int cache_id)
{
    CoherEntry *ent = map_find(addr_to_entry_, base_addr);
    COHER_DB(1)("coher: evict_notify: base_addr %s cache_id %d:",
                fmt_laddr(base_addr), cache_id);
           
    if (!ent) {
        // The block must be cached, otherwise it wouldn't be getting evicted
        abort_printf("error: cache id %d evicting uncached block %s\n",
                     cache_id, fmt_laddr(base_addr));
    }

    COHER_DB(1)(" %s", ent->fmt().c_str());
    if (ent->is_busy() && ent->is_waiting_for(cache_id)) {
        // A cache has evicted a block for which there is an outstanding
        // writeback/invalidate request; the eviction occurred before
        // the WBI was processed.  We can ignore the eviction, and let
        // the WBI perform any state updates needed.
        COHER_DB(1)(" (waiting-for this peer)");
    } else {
        sim_assert(ent->is_holder(cache_id));
        if (apply_evict_notifies_) {
            // note: entry may be "busy"
            ent->remove_holder(cache_id);
            if (!ent->any_holders()) {
                COHER_DB(1)(" (sole holder)");
                addr_to_entry_.erase(base_addr);
                ent = NULL;
            }
        }
    }

    COHER_DB(1)("\n");
}


bool
CoherenceMgr::holder_okay(const LongAddr& base_addr, int cache_id, 
                          bool dirty, bool writeable) const
{
    const CoherEntry *ent = map_find(addr_to_entry_, base_addr);
    bool result;

    if (ent != NULL) {
        CoherEntryState state = ent->get_state();

        if (!ent->is_holder(cache_id)) {
            result = false;
        } else if (state == Coher_Shared) {
            result = !dirty && !writeable;
        } else {
            sim_assert(state == Coher_Exclusive);
            result = true;
        }
        if (!result && ent->is_busy() && ent->is_waiting_for(cache_id)) {
            // It may look like cache_id has the block illegally, but we've
            // also got an invalidate/etc. outstanding from that same peer.
            // This is OK iff the invalidate ends up being applied after
            // whatever action is checking its access; we don't have a way to
            // test for that, here, so we'll give it the benefit of the doubt.
            // (We rely on cache port synchronization before invalidate
            // processing to accomplish the ordering.)
            result = true;
        }
    } else {
        result = false;                 // Block not registered for caching
    }

    if (!result) {
        FILE *out = stderr;
        fprintf(out, "coher: holder_okay: base_addr %s cache_id %d "
                "dirty %d writeable %d: ", fmt_laddr(base_addr),
                cache_id, dirty, writeable);
        if (ent) {
            fprintf(out, "%s", ent->fmt().c_str());
        } else {
            fprintf(out, "uncached");
        }
        fprintf(out, ", failed!\n");
    }

    return result;
}


CoherenceMgr *
cm_create(void)
{
    return new CoherenceMgr();
}



void 
cm_destroy(CoherenceMgr *cm)
{
    if (cm)
        delete cm;
}


void 
cm_reset(CoherenceMgr *cm)
{
    cm->reset();
}


void 
cm_reset_cache(CoherenceMgr *cm, int cache_id)
{
    cm->reset_cache(cache_id);
}

void
cm_reset_entry(CoherenceMgr *cm, LongAddr base_addr)
{
    cm->reset_entry(base_addr);
}

void 
cm_add_cache(CoherenceMgr *cm, struct CacheArray *cache, int cache_id,
             struct CoreResources *parent_core)
{
    cm->add_cache(cache, cache_id, parent_core);
}

CoherAccessResult
cm_access(CoherenceMgr *cm, LongAddr base_addr, int cache_id,
          CoherAccessType access_type,
          CoherWaitInfo **coher_wait_ret, int *writeable_ret)
{
    return cm->access(base_addr, cache_id, access_type, 
                      coher_wait_ret, writeable_ret);
}

void
cm_shared_request(CoherenceMgr *cm, LongAddr base_addr)
{
    cm->shared_request(base_addr);
}

void
cm_shared_reply(CoherenceMgr *cm, LongAddr base_addr)
{
    cm->shared_reply(base_addr);
}

int
cm_peer_reply(CoherenceMgr *cm, LongAddr base_addr,
         int reply_cache_id)
{
    return cm->peer_reply(base_addr, reply_cache_id);
}

void 
cm_evict_notify(CoherenceMgr *cm, LongAddr base_addr, int cache_id)
{
    cm->evict_notify(base_addr, cache_id);
}

int 
cm_holder_okay(const CoherenceMgr *cm, LongAddr base_addr, 
               int cache_id, int dirty, int writeable)
{
    return cm->holder_okay(base_addr, cache_id, dirty, writeable);
}

void
coherwaitinfo_destroy(CoherWaitInfo *cwi)
{
    if (cwi) {
        delete[] cwi->nodes;
        delete cwi;
    }
}
