/*
 * Cache Request Queue
 *
 * Jeff Brown
 * $Id: cache-queue.cc,v 1.9.6.3.2.2.2.11 2009/07/29 19:24:45 jbrown Exp $
 */

const char RCSid_1035918504[] =
"$Id: cache-queue.cc,v 1.9.6.3.2.2.2.11 2009/07/29 19:24:45 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <queue>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim-assert.h"
#include "hash-map.h"
#include "sys-types.h"
#include "cache-queue.h"
#include "sim-params.h"
#include "cache.h"
#include "cache-req.h"
#include "core-resources.h"
#include "utils.h"
#include "utils-cc.h"

using std::string;
using std::make_pair;
using std::vector;


#define USE_HASHMAP_NOT_MAP     (1 && HAVE_HASHMAP)


const char *CacheQFindSense_names[] = {
    "Miss", "WB", "Coher", "All", NULL
};


namespace {

struct CacheAddrKey {
    LongAddr base_addr;
    int core_id;        // CACHEQ_SHARED for shared outside of cores
    CacheQFindSense sense;

    CacheAddrKey(const LongAddr& base_addr_, int core_id_,
                 CacheQFindSense sense_)
        : base_addr(base_addr_), core_id(core_id_), sense(sense_) {
        sim_assert(ENUM_OK(CacheQFindSense, sense));
        sim_assert((core_id >= 0) || (core_id == CACHEQ_SHARED));
        // We shouldn't be using wildcard values as literal keys
        sim_assert(sense != CQFS_All);
        sim_assert(core_id != CACHEQ_ALL_CORES);
    }

    CacheAddrKey(const CacheRequest *creq) 
        : base_addr(creq->base_addr)
    {
        sim_assert(creq != NULL);
        CacheSource core_source;
        if (cache_action_incore(creq->action)) {
            // core-private actions shouldn't have multiple cores associated
            sim_assert(creq->cores[0].core);
            sim_assert(!creq->cores[1].core);
            core_id = creq->cores[0].core->core_id;
            core_source = creq->cores[0].src;
        } else {
            core_id = CACHEQ_SHARED;
            // A little shady, but we're just using this to classify the key
            // as Miss vs. Coher vs. WB
            core_source = creq->cores[0].src;
        }
        if (CACHE_SOURCE_L1_ONLY(core_source)) {
            sense = CQFS_Miss;
        } else if (core_source == CSrc_Coher) {
            sense = CQFS_Coher;
        } else if (core_source == CSrc_WB) {
            sense = CQFS_WB;
        } else {
            ENUM_ABORT(CacheSource, core_source);
        }
    }

    bool must_be_unique() const {
        return (sense == CQFS_Miss) || (sense == CQFS_Coher);
    }

    string fmt() const {
        std::ostringstream ostr;
        ostr << base_addr.fmt() << ",";
        if (core_id == CACHEQ_SHARED) {
            ostr << "Shared";
        } else if (core_id == CACHEQ_ALL_CORES) {
            ostr << "All";
        } else {
            ostr << "C" << core_id;
        }
        ostr << "," << CacheQFindSense_names[sense];
        return ostr.str();
    }

    void change_sense(CacheQFindSense new_sense) {
        sense = new_sense;
    }

    // be careful: cacheq_find_multi() and minimum_addr_key() know about this
    // ordering
    bool operator < (const CacheAddrKey& r2) const {
        return
            (base_addr < r2.base_addr) ||
            ((base_addr == r2.base_addr) && 
             ((core_id < r2.core_id) ||
              ((core_id == r2.core_id) && (sense < r2.sense))));
    }
    bool operator == (const CacheAddrKey& r2) const {
        return (base_addr == r2.base_addr) &&
            (core_id == r2.core_id) &&
            (sense == r2.sense);
    }
    size_t hash() const {
        StlHashU64 hasher;
        return base_addr.hash() ^ hasher((core_id << 16) ^ sense);
    }
};


// Returns a CacheAddrKey K such that no other CacheAddrKey with the same
// base_addr can compare as less-than K.
CacheAddrKey
minimum_addr_key(const LongAddr& base_addr)
{
    // note: CACHEQ_ALL is less than _SHARED, but it's not valid in keys
    return CacheAddrKey(base_addr, CACHEQ_SHARED, CQFS_Miss);
}


struct CacheTimeKey {
    i64 request_time;
    i64 serial_num;
    CacheTimeKey(i64 request_time_, i64 serial_num_)
        : request_time(request_time_), serial_num(serial_num_)
    { }
    CacheTimeKey(const CacheRequest *creq) 
        : request_time(creq->request_time), serial_num(creq->serial_num)
    { }

    string fmt() const {
        string result;
        result += "(";
        result += fmt_i64(request_time);
        result += ",";
        result += fmt_i64(serial_num);
        result += ")";
        return result;
    }

    bool operator < (const CacheTimeKey& k2) const {
        return
            (request_time < k2.request_time) ||
            ((request_time == k2.request_time) &&
             (serial_num < k2.serial_num));
    }
    bool operator == (const CacheTimeKey& k2) const {
        return (request_time == k2.request_time) &&
            (serial_num == k2.serial_num);
    }
    size_t hash() const {
        StlHashU64 hasher;
        return hasher(request_time ^ serial_num);
    }
};


// This compares "backwards", to get min-heap behavior out of the standard
// max-heap priority-queue algorithms.
struct CacheReqTimeComp {
    inline bool
    operator() (const CacheRequest *r1, const CacheRequest *r2) const {
        return (r1->request_time > r2->request_time) ||
            ((r1->request_time == r2->request_time) &&
             (r1->serial_num > r2->serial_num));
    }
};


// We currently keep copies of CacheAddrMap iterators, to allow for easy
// deletion later.  The hash-based container iterators don't persist; if we
// used these, deletion would be more of a pain, since we'd have to hunt
// around for the specific CacheRequest* we're after.  So, we'll always use
// a non-hashed map<> for CacheAddrMap.  (This can change without mucking
// up the external API.)
typedef std::multimap<CacheAddrKey, CacheRequest *> CacheAddrMap;

#if USE_HASHMAP_NOT_MAP
    typedef hash_map<CacheRequest *, CacheAddrMap::iterator,
                     StlHashPointerValue<CacheRequest *> > CacheReqRevMap;
    typedef hash_set<CacheRequest *,
                     StlHashPointerValue<CacheRequest *> > CacheReqSet;
#else
    typedef std::map<CacheRequest *, CacheAddrMap::iterator> CacheReqRevMap;
    typedef std::set<CacheRequest *> CacheReqSet;
#endif

// We'll use a map from times to requests instead of priority-queue of
// requests here, specifically because we want to be able to delete 
// requests efficiently (which we can do, by searching on time keys).
//
// Recall that CacheTimeKeys are unique due to the "serial_num" field of
// CacheRequest; this gives us a total order (and unique CacheTimeKeys).
typedef std::map<CacheTimeKey, CacheRequest *> CacheTimeMap;


} // Anonymous namespace close


struct CacheQueue {
protected:
    CacheAddrMap addr_to_req;           // All requests
    // all_reqs gives us 1) creq uniqueness, 2) easy erase from addr_to_req
    CacheReqRevMap all_reqs;            // All reqs; iters from addr_to_req
    CacheTimeMap time_map;              // Non-blocked requests
    CacheReqSet blocked_reqs;           // Blocked requests

    CacheAddrMap::const_iterator user_iter1;

    bool invariant() const {
        return (addr_to_req.size() == all_reqs.size()) &&
            ((time_map.size() + blocked_reqs.size()) == all_reqs.size());
    }

private:
    // Disallow copy or assignment
    CacheQueue(const CacheQueue& src);
    CacheQueue& operator = (const CacheQueue &src);

public:
    CacheQueue() {
        sim_assert(invariant());
    }
    ~CacheQueue() { }

    int empty() const {
        sim_assert(invariant());
        return time_map.empty();
    }

    void enqueue(CacheRequest *creq) {
        const char *fname = "CacheQueue::enqueue";
        CacheAddrKey addr_key(creq);
        sim_assert(creq->serial_num >= 0);
        sim_assert(invariant());

        if (addr_key.must_be_unique()) {
            CacheRequest **dupe = map_find(addr_to_req, addr_key);
            if (dupe) {
                string first_req(fmt_creq_static(*dupe));
                string new_req(fmt_creq_static(creq));
                abort_printf("%s: duplicate request for unique key (%s)\n"
                             "existing creq: %s\nnew creq: %s\n", fname,
                             addr_key.fmt().c_str(), first_req.c_str(),
                             new_req.c_str());
            }
        }

        CacheAddrMap::iterator addr_map_iter = 
            addr_to_req.insert(make_pair(addr_key, creq));
        if (!all_reqs.insert(make_pair(creq, addr_map_iter)).second) { 
            abort_printf("%s: cache request %p already in all_reqs; %s\n",
                         fname, (void *) creq, fmt_creq_static(creq));
        }

        if (creq->blocked) {
            if (!blocked_reqs.insert(creq).second) {
                abort_printf("duplicate blocked cache_request %p, "
                             "shouldn't be possible: %s\n", (void *) creq,
                             fmt_creq_static(creq));
            }
        } else {
            time_map.insert(make_pair(CacheTimeKey(creq), creq));
        }
        sim_assert(invariant());
    }

    CacheRequest *dequeue_ready(i64 now) {
        CacheRequest *result = NULL;
        sim_assert(invariant());
        if (!time_map.empty()) {
            CacheTimeMap::iterator earliest = time_map.begin();
            CacheRequest *creq = earliest->second;
            sim_assert(!creq->blocked);
            if (creq->request_time <= now) {
                result = creq;
                CacheReqRevMap::iterator rev_found = all_reqs.find(creq);
                if (rev_found != all_reqs.end()) {
                    addr_to_req.erase(rev_found->second);
                    all_reqs.erase(rev_found);
                } else {
                    abort_printf("cache req %p dequeued, but not in "
                                 "all_reqs: %s\n",
                                 (void *) creq, fmt_creq_static(creq));
                }
                time_map.erase(earliest);
            }
            sim_assert(invariant());
        }
        return result;
    }

    void dequeue(CacheRequest *creq);

    void dequeue_blocked(CacheRequest *creq)
    {
        sim_assert(invariant());
        sim_assert(blocked_reqs.count(creq));
        sim_assert(creq->blocked);
        CacheReqRevMap::iterator rev_found = all_reqs.find(creq);
        if (rev_found != all_reqs.end()) {
            addr_to_req.erase(rev_found->second);
            all_reqs.erase(rev_found);
        } else {
            abort_printf("cache req %p dequeued, but not in "
                         "all_reqs: %s\n",
                         (void *) creq, fmt_creq_static(creq));
        }
        sim_assert(blocked_reqs.count(creq));
        blocked_reqs.erase(creq);
        sim_assert(invariant());
    }

    CacheRequest *find(const LongAddr& base_addr, int core_id, 
                       CacheQFindSense sense) const {
        CacheRequest *result = NULL;
        sim_assert(core_id != CACHEQ_ALL_CORES);
        sim_assert(sense != CQFS_All);
        sim_assert(sense != CQFS_WB);
        CacheAddrKey addr_key(base_addr, core_id, sense);
        // try to exclude queries which could return multiple matches
        sim_assert(addr_key.must_be_unique());
        CacheAddrMap::const_iterator found = addr_to_req.find(addr_key);
        if (found != addr_to_req.end()) {
            result = found->second;
            // ensure there's not a duplicate match
            ++found;
            if ((found != addr_to_req.end()) &&
                (found->first == addr_key)) {
                // oh, we've done it now: there are multiple matching requests,
                // but the caller is only semantically equipped to handle one
                string first_req(fmt_creq_static(result));
                string second_req(fmt_creq_static(found->second));
                abort_printf("cacheq_find(): multiple matches for "
                             "key %s; first two:\n%s\n%s\n",
                             addr_key.fmt().c_str(), first_req.c_str(),
                             second_req.c_str());
            }
        }
        return result;
    }

    CacheRequest **find_multi(const LongAddr& base_addr, int core_id, 
                              CacheQFindSense sense) const;

    void iter_reset() {
        user_iter1 = addr_to_req.begin();
    }

    CacheRequest *iter_next() {
        CacheRequest *result;
        if (user_iter1 != addr_to_req.end()) {
            result = user_iter1->second;
            ++user_iter1;
        } else {
            result = NULL;
        }
        return result;
    }
};


void
CacheQueue::dequeue(CacheRequest *creq)
{
    if (creq->blocked) {
        dequeue_blocked(creq);
    } else {
        sim_assert(invariant());
        CacheReqRevMap::iterator rev_found = all_reqs.find(creq);
        if (rev_found != all_reqs.end()) {
            addr_to_req.erase(rev_found->second);
            all_reqs.erase(rev_found);
        } else {
            abort_printf("cache req %p dequeued, but not in "
                         "all_reqs: %s\n",
                         (void *) creq, fmt_creq_static(creq));
        }
        CacheTimeMap::iterator time_found = time_map.find(CacheTimeKey(creq));
        if (time_found != time_map.end()) {
            time_map.erase(time_found);
        } else {
            abort_printf("cache req %p dequeued, but not in "
                         "time_map: %s\n",
                         (void *) creq, fmt_creq_static(creq));
        }
        sim_assert(invariant());
    }
}


CacheRequest **
CacheQueue::find_multi(const LongAddr& base_addr, int core_id, 
                       CacheQFindSense sense) const
{
    vector<CacheRequest *> found;
    bool use_wildcard = (core_id == CACHEQ_ALL_CORES) ||
        ((sense == CQFS_All) || (sense == CQFS_WB));
    // (we treat CQFS_WB as a wildcard here, since we don't enforce any
    // uniqueness critera on them)

    if (use_wildcard) {
        // we have a few options here: we could do individual
        // addr_to_req.find() calls for each possible requested core/sense.
        // since we're already using an ordered multimap<>, we can also find
        // the "lowest" key with matching base_addr, and then step through the
        // cores/senses present for that address and pick out the ones we
        // want.  We'll do the latter since it's simpler; we can revisit this
        // later if need be, without changing the API.
        CacheAddrKey addr_key = minimum_addr_key(base_addr);
        CacheAddrMap::const_iterator iter = addr_to_req.lower_bound(addr_key);
        for (; (iter != addr_to_req.end()) &&
                 (iter->first.base_addr == base_addr); ++iter) {
            bool match = 
                ((core_id == CACHEQ_ALL_CORES) ||
                 (iter->first.core_id == core_id)) &&
                ((sense == CQFS_All) ||
                 (iter->first.sense == sense));
            if (match)
                found.push_back(iter->second);
            
        }
    } else {
        CacheAddrKey addr_key(base_addr, core_id, sense);
        CacheAddrMap::const_iterator iter = addr_to_req.lower_bound(addr_key);
        CacheAddrMap::const_iterator last = addr_to_req.upper_bound(addr_key);
        for (; iter != last; ++iter) {
            found.push_back(iter->second);
        }
    }

    CacheRequest **result = static_cast<CacheRequest **>
        (emalloc((found.size() + 1) * sizeof(result[0])));
    for (int i = 0; i < int(found.size()); ++i)
        result[i] = found[i];
    result[found.size()] = NULL;
    // caller must free() !
    return result;
}


//
// C interface
//

CacheQueue *
cacheq_create()
{
    CacheQueue *cq = new CacheQueue();
    return cq;
}

void 
cacheq_destroy(CacheQueue *cq)
{
    if (cq)
        delete cq;
}

int
cacheq_empty(const CacheQueue *cq)
{
    return cq->empty();
}

void 
cacheq_enqueue(CacheQueue *cq, struct CacheRequest *creq)
{
    cq->enqueue(creq);
}

struct CacheRequest *
cacheq_dequeue_ready(CacheQueue *cq, i64 now)
{
    return cq->dequeue_ready(now);
}

void
cacheq_dequeue(CacheQueue *cq, struct CacheRequest *creq)
{
    cq->dequeue(creq);
}

void
cacheq_dequeue_blocked(CacheQueue *cq, struct CacheRequest *creq)
{
    cq->dequeue_blocked(creq);
}

struct CacheRequest *
cacheq_find(CacheQueue *cq, LongAddr base_addr, int core_id, 
            CacheQFindSense sense)
{
    return cq->find(base_addr, core_id, sense);
}

struct CacheRequest **
cacheq_find_multi(CacheQueue *cq, LongAddr base_addr, int core_id, 
                  CacheQFindSense sense)
{
    return cq->find_multi(base_addr, core_id, sense);
}

void 
cacheq_iter_reset(CacheQueue *cq)
{
    cq->iter_reset();
}

struct CacheRequest *
cacheq_iter_next(CacheQueue *cq)
{
    return cq->iter_next();
}
