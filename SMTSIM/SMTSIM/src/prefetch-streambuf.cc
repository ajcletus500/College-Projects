//
// Prefetcher "stream buffer" data storage
//
// Jeff Brown
// $Id: prefetch-streambuf.cc,v 1.1.2.21.4.1 2009/12/25 06:31:50 jbrown Exp $
//

const char RCSid_1219954695[] = 
"$Id: prefetch-streambuf.cc,v 1.1.2.21.4.1 2009/12/25 06:31:50 jbrown Exp $";

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "hash-map.h"
#include "prefetch-streambuf.h"
#include "utils.h"
#include "utils-cc.h"
#include "sim-cfg.h"
#include "callback-queue.h"
#include "assoc-array.h"
#include "cache-array.h"        // For CacheAccessType enum
#include "cache.h"
#include "online-stats.h"
#include "core-resources.h"
#include "main.h"               // For fmt_now() / cyc


using namespace SimCfg;
using std::list;
using std::map;
using std::multimap;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;


// Vague plan for PFSG_DEBUG_LEVEL:
// 0: off
// 1: top-level stream-group stuff
// 2: per-stream stuff
// 3: per-stream-entry stuff, per-predictor-entry stuff
// 4: add full dump after each top-level op (except commit & no-op service())
// 5: add full dump after each commit too

#define PFSG_DEBUG_LEVEL        1

#ifdef DEBUG
    // Condition: "should I printf a level-x debug message?"
    #define PFSG_DEBUG_COND(x) ((PFSG_DEBUG_LEVEL >= (x)) && debug)
#else
    // Never print
    #define PFSG_DEBUG_COND(x) (0)
#endif
#define PFSG_DB(x) if (!PFSG_DEBUG_COND(x)) { } else printf


// (add any exported enum name-arrays here)


namespace {

// A bunch of options affecting stream buffer operation at various levels.
// (These are lazily crammed into a common structure that we fill in once
// and then hand around const references to, instead of breaking these out
// at different levels.)

struct StreambufConf {
    int parent_core_id;

    int n_streams;
    int blocks_per_stream;
    int stride_pc_entries;
    int stride_pc_assoc;

    // flag: don't keep a copy of data in a stream buffer, after a tag-match
    // on an L1 miss.  (free the entry for re-use right away)
    bool always_free_on_match;

    bool prefetch_as_exclusive;
    bool prefetch_only_when_quiet;

    // don't prefetch the same block into multiple streams.  (also prevents a
    // single stream from holding multiple copies of a block.)
    bool force_no_overlap;

    // use the "two-miss filter" for stream allocation, instead of confidence
    bool use_two_miss_alloc_filter;
    // use round-robin scheduling, instead of priority
    bool use_round_robin_sched;

    // age all streams every N allocation requests
    int stream_priority_age_allocs;

    // saturating-counter limits and zero-points for various counters
    int predict_match_saturate;
    int predict_match_zero;             // auto-set
    int predict_miss_saturate;          // always considered >= 0
    int stream_priority_saturate;
    int alloc_min_confidence_thresh;    // read in zero-based, auto-shifted

    NoDefaultCopy nocopy;

public:

    StreambufConf(CoreResources *parent_core, const string& cfg_path);
    ~StreambufConf() { }
};


StreambufConf::StreambufConf(CoreResources *parent_core,
                             const string& cfg_path)
{
    const string& cp = cfg_path;        // short-hand

    parent_core_id = parent_core->core_id;

    n_streams = conf_int(cp + "/n_streams");
    if (n_streams < 1) {
        exit_printf("bad streambuf n_streams (%d)\n", n_streams);
    }
    blocks_per_stream = conf_int(cp + "/blocks_per_stream");
    if (blocks_per_stream < 1) {
        exit_printf("bad streambuf blocks_per_stream (%d)\n",
                    blocks_per_stream);
    }

    stride_pc_entries = conf_int(cp + "/stride_pc_entries");
    if (stride_pc_entries < 1) {
        exit_printf("bad streambuf stride_pc_entries (%d)\n",
                    stride_pc_entries);
    }
    stride_pc_assoc = conf_int(cp + "/stride_pc_assoc");
    if (stride_pc_assoc < 1) {
        exit_printf("bad streambuf stride_pc_assoc (%d)\n", stride_pc_assoc);
    }
    if (stride_pc_entries % stride_pc_assoc) {
        exit_printf("streambuf stride_pc_assoc (%d) doesn't divide "
                    "stride_pc_entries (%d)\n", stride_pc_assoc,
                    stride_pc_entries);
    }

    always_free_on_match = conf_bool(cp + "/always_free_on_match");
    prefetch_as_exclusive = conf_bool(cp + "/prefetch_as_exclusive");
    prefetch_only_when_quiet = conf_bool(cp + "/prefetch_only_when_quiet");
    force_no_overlap = conf_bool(cp + "/force_no_overlap");
    use_two_miss_alloc_filter = conf_bool(cp + "/use_two_miss_alloc_filter");
    use_round_robin_sched = conf_bool(cp + "/use_round_robin_sched");
    stream_priority_age_allocs = conf_int(cp + "/stream_priority_age_allocs");
    predict_match_saturate = conf_int(cp + "/predict_match_saturate");
    predict_match_zero = predict_match_saturate / 2;
    predict_miss_saturate = conf_int(cp + "/predict_miss_saturate");
    stream_priority_saturate = conf_int(cp + "/stream_priority_saturate");
    alloc_min_confidence_thresh =
        conf_int(cp + "/alloc_min_confidence_thresh");
    alloc_min_confidence_thresh += predict_match_zero;
}   


string
fmt_bstat_i64(const BasicStat_I64& bstat)
{
    return bstat.fmt();
}


string
fmt_bstat_double(const BasicStat_Double& bstat)
{
    return bstat.fmt();
}


struct GroupStats {
    i64 accesses;       // number of accesses to stream group
    i64 hits;           // subset of "accesses" which led to non-merged hits

    i64 pf_reqs_ok;     // number of prefetch requests sent down
    i64 pf_reqs_failed; // attempted PF requests rejected by memory system
    i64 pf_used;        // subset of "pf_reqs_ok" which were somehow useful
    BasicStat_Double full_win_time; // "pf_reqs_ok" which filled then hit
    BasicStat_Double partial_win_time;  // "pf_reqs_ok" which preceeded merges
    BasicStat_Double all_win_time;      // combo of full & partial wins
    BasicStat_Double pf_service_time;   // for pf_reqs_ok, fetch->fill time

    i64 stream_allocs;  // #times a stream was allocated

    struct {
        // stats sampled for each dynamic SBStream instance, updated when
        // a stream is replaced.  (so "replaced" is the overall sample count)
        i64 replaced;                   // #replaced instances
        i64 useless;                    // subset of "replaced" with no wins
        BasicStat_I64 accesses;
        BasicStat_I64 hits;
        BasicStat_I64 prefetches;
        BasicStat_I64 full_wins;
        BasicStat_I64 partial_wins;
        BasicStat_I64 all_wins;
        BasicStat_I64 lifetime;         // from alloc to re-alloc
    } per_sb;

    struct {
        i64 gr_total;   // # of group-wide import() invocations
        i64 gr_reject;  // subset of "gr_total" dropped due to prior deferred

        // st_* mutually exclusive, stats per-stream across all imports
        i64 st_acc_prompt;      // accepted promptly at target
        i64 st_acc_defer;       // accepted but deferred at target
        i64 st_rej_targ;        // rejected at target (priority, capacity)
        i64 st_rej_group;       // group-wide import() was rejected
    } import;

    void reset() {
        accesses = 0;
        hits = 0;
        pf_reqs_ok = 0;
        pf_reqs_failed = 0;
        pf_used = 0;
        full_win_time.reset();
        partial_win_time.reset();
        all_win_time.reset();
        pf_service_time.reset();
        stream_allocs = 0;
        per_sb.replaced = 0;
        per_sb.useless = 0;
        per_sb.accesses.reset();
        per_sb.hits.reset();
        per_sb.prefetches.reset();
        per_sb.full_wins.reset();
        per_sb.partial_wins.reset();
        per_sb.all_wins.reset();
        per_sb.lifetime.reset();
        import.gr_total = import.gr_reject = 0;
        import.st_rej_group = import.st_rej_targ = import.st_acc_prompt =
            import.st_acc_defer = 0;
    }
};


struct LookupCoord {
    int stream_id;
    int entry_id;
    LookupCoord(int stream_id_, int entry_id_)
        : stream_id(stream_id_), entry_id(entry_id_) { }
    bool operator < (const LookupCoord& c2) const {
        return (stream_id < c2.stream_id) ||
            ((stream_id == c2.stream_id) && (entry_id < c2.entry_id));
    }
    bool operator == (const LookupCoord& c2) const {
        return  (stream_id == c2.stream_id) && (entry_id == c2.entry_id);
    }
    size_t stl_hash() const {
        StlHashU32 hasher;
        return hasher((stream_id << 8) ^ entry_id);
    }
    string fmt() const {
        ostringstream ostr;
        ostr << "s" << stream_id << "e" << entry_id;
        return ostr.str();
    }
};


#if HAVE_HASHMAP
    typedef hash_set<LookupCoord, StlHashMethod<LookupCoord> > LookupCoordSet;
    typedef hash_map<LongAddr, LookupCoordSet,
                     StlHashMethod<LongAddr> > AddrCoordMap;
#else
    typedef set<LookupCoord> LookupCoordSet;
    typedef map<LongAddr, LookupCoordSet> AddrCoordMap;
#endif
typedef vector<LookupCoord> LookupCoordVec;



// GroupStreamIndex is a weird beast: it provides lookup (but not
// replacement) service across all stream buffers in a group, further tracking
// which entries within each stream match a given address.  Within each
// stream, we just manage things based on indices, not addresses.  This is
// weird and mixes up higher and lower-level ideas; the goal is to keep the
// associative book-keeping implemented in one place, so that lower-level code
// can easily ask things like "does anybody else have a copy of this block?",
// and also without having to have for() loops iterating over all streams
// testing second-level maps.
//
// The currency here is the LookupCoord, effectively an (x,y) coordinate
// which refers to a specific physical (stream,entry) SBEntry.

class GroupStreamIndex {
    AddrCoordMap addr_lookup;   // base_addr -> non-empty set<> of LookupCoords

public:
    GroupStreamIndex() { }
    void reset() {
        addr_lookup.clear();
    }

    // Search for the given block, across all streams.  Writes the coordinates
    // of any matches to "coords_ret"; it will be left empty if no matches are
    // found.  The written coordinates MAY have the data available; test them
    // to see.
    void lookup(const LongAddr& base_addr, LookupCoordVec *coords_ret) const {
        const LookupCoordSet *found = map_find(addr_lookup, base_addr);
        coords_ret->clear();
        if (found) {
            // the key shouldn't be in map with no holders
            sim_assert(!found->empty());
            coords_ret->reserve(found->size());
            // copy LookupCoord values from *found to *coords_ret
            coords_ret->insert(coords_ret->end(), found->begin(),
                               found->end());
        }
    }

    // Search for the any entries matching the given master_id, across all
    // streams.  Semantics are otherwise like lookup().
    void lookup_masterid_slow(int master_id, LookupCoordVec *coords_ret) const
    {
        // This is currently an inefficient walk over the entire tag set; we
        // expect this to be rare.  (If we end up doing this very often, we
        // can always add support for doing it quickly, but it's likely not
        // worth the overhead of maintaining additional structures.)
        coords_ret->clear();
        FOR_CONST_ITER(AddrCoordMap, addr_lookup, iter) {
            const LongAddr& key = iter->first;
            if (key.id == u32(master_id)) {
                const LookupCoordSet& found = iter->second;
                coords_ret->insert(coords_ret->end(), found.begin(),
                                   found.end());
            }
        }
    }

    // reduced version of lookup(), just reports whether a tag is present
    // somewhere
    bool tag_present(const LongAddr& base_addr) const {
        const LookupCoordSet *found = map_find(addr_lookup, base_addr);
        return (found != NULL);
    }

    // Note that a given (stream,block) is now tracking the given address.  We
    // do this per-block, since we don't expect many copies of a block to
    // spring into view at once.  The map::find is redundant in many cases,
    // since it immediately follows a lookup() on the same block, but the
    // interface is simpler than trying to hand back and re-use a result
    // pointer from the lookup() method.
    void insert_block(const LongAddr& base_addr, const LookupCoord& coord) {
        LookupCoordSet *result = map_find(addr_lookup, base_addr);
        if (!result) {
            result = &map_put_uniq(addr_lookup, base_addr, LookupCoordSet());
        }
        if (!(result->insert(coord).second)) {
            abort_printf("insert_block: base_addr %s, coord %s already"
                         " present\n", fmt_laddr(base_addr),
                         coord.fmt().c_str());
        }
    }

    // Erase a sequence of (stream,block) entries that previously tracked the
    // given address.  We do this as an aggregate operation, since
    // group-invalidates are expected (when data is migrated to L1 on a hit,
    // particularly for exclusive access, the set of stream buffers needs to
    // let go of all copies).
    void erase_blocks(const LongAddr& base_addr,
                      const LookupCoordVec& coords) {
        AddrCoordMap::iterator found = addr_lookup.find(base_addr);
        if (found == addr_lookup.end()) {
            abort_printf("tried to erase from non-existent addr %s\n",
                         fmt_laddr(base_addr));
        }
        LookupCoordSet& this_addr_set = found->second;
        FOR_CONST_ITER(LookupCoordVec, coords, iter) {
            if (!this_addr_set.erase(*iter)) {
                abort_printf("tried to erase non-existent %s from %s\n",
                             fmt_laddr(base_addr), iter->fmt().c_str());
            }
        }
        if (this_addr_set.empty())
            addr_lookup.erase(found);
    }

    // single-block version of erase_blocks()
    void erase_block(const LongAddr& base_addr, const LookupCoord& coord) {
        AddrCoordMap::iterator found = addr_lookup.find(base_addr);
        if (SP_F(found == addr_lookup.end())) {
            abort_printf("tried to erase from non-existent addr %s\n",
                         fmt_laddr(base_addr));
        }
        LookupCoordSet& this_addr_set = found->second;
        if (SP_F(!this_addr_set.erase(coord))) {
            abort_printf("tried to erase non-existent %s from %s\n",
                         fmt_laddr(base_addr), coord.fmt().c_str());
        }
        if (this_addr_set.empty())
            addr_lookup.erase(found);
    }
};


// State for one predictor entry; right now, we lazily just use the same
// class in both the stream-independent predictor used to guide allocation,
// as well as in the per-stream predictors which guide prefetching.
//
// (currently this just captures one stride prediction sequence)
class PredictInfo {
    const StreambufConf *conf;
    i64 prev_addr;
    i64 prev_prev_addr;
    int match_counter;      // stride matches (+1 on match, -1 on mismatch)
    int miss_run_len;       // recent _consecutive_ misses for this block
public:
    // WARNING: consumer code relies on copy/assignment operations
    PredictInfo(const StreambufConf *conf_) : conf(conf_) { reset(); }
    ~PredictInfo() { }

    string fmt() const {
        ostringstream ostr;
        ostr << "prev " << fmt_mem(prev_addr)
             << " pprev " << fmt_mem(prev_prev_addr)
             << " matchcount0 " << (match_counter - conf->predict_match_zero)
             << " missrun " << miss_run_len;
        return ostr.str();
    }
    void reset() {
        prev_addr = 0;
        prev_prev_addr = 0;
        match_counter = conf->predict_match_zero;
        miss_run_len = 0;
    }
    void update(mem_addr addr, bool was_miss) {
        if (was_miss) {
            // predictor training based on miss stream
            if (prev_addr && prev_prev_addr) {
                i64 test_stride = prev_addr - prev_prev_addr;
                if ((prev_addr + test_stride) == static_cast<i64>(addr)) {
                    match_counter = INCR_SAT(match_counter,
                                             conf->predict_match_saturate);
                } else {
                    match_counter = DECR_SAT(match_counter);
                }
            }
            prev_prev_addr = prev_addr;
            prev_addr = static_cast<i64>(addr);
        } 
        // vaguely cheating: we'll allow knowledge of hits to creep in here,
        // strictly in order to perform the accounting needed for "two-miss
        // filtering".  we'll try not to let any additional info about hits
        // leak out, though.
        miss_run_len = (was_miss) ?
            INCR_SAT(miss_run_len, conf->predict_miss_saturate) : 0;
    }
    bool ready_to_free_predict() const {
        return prev_addr && prev_prev_addr && (prev_addr != prev_prev_addr);
    }
    void free_predict_and_update(mem_addr *predict_addr_ret,
                                 int block_bytes) {
        // free-running prediction generation, with cache-block granularity
        i64 stride = prev_addr - prev_prev_addr;
        sim_assert(stride != 0);
        // don't bother directly iterating strides less than a cache block
        // in size, since we're prefetching entire blocks.  
        if ((stride < 0) && (stride > -block_bytes)) {
            stride = -block_bytes;
        } else if ((stride > 0) && (stride < block_bytes)) {
            stride = block_bytes;
        }
        prev_prev_addr = prev_addr;
        prev_addr += stride;
        *predict_addr_ret = static_cast<mem_addr>(prev_addr);
    }
    int g_match_counter() const {
        // Sherwood et al's "accuracy counter"
        return match_counter;   // (not zero-adjusted)
    }
    bool two_miss_filter_pass(mem_addr next_addr) const {
        // "two-miss stride filtering" from Farkas, also Sherwood: A load miss
        // is allowed to allocate a streambuf if it misses two times in a row,
        // and the predictor would have correctly predicted both misses
        // (i.e. the stride values match).
        bool result = false;
        if ((miss_run_len >= 2) && prev_addr && prev_prev_addr) {
            i64 stride1 = prev_addr - prev_prev_addr;
            i64 stride2 = static_cast<i64>(next_addr) - prev_addr;
            result = (stride1 == stride2);
        }
        return result;
    }
    void rebase_addr(mem_addr new_miss_va) {
        // Adjust this predictor entry to pick up operation at "new_miss_va".
        // (This occurs when e.g. taking an old stride-table entry and using it
        // to start prefetching for that same PC, but at a different address)
        i64 stride = prev_addr - prev_prev_addr;
        prev_addr = new_miss_va;
        prev_prev_addr = new_miss_va - stride;
    }
};


// Predictor associated with each stream.
// (In the "full Sherwood" setup, this would hold additional per-stream
// local history)
class PerStreamPredictor {
    const StreambufConf *conf;
    PredictInfo state;          // lazy
    int priority;               // Sherwood's "priority counter"
    NoDefaultCopy nocopy;
public:
    // WARNING: consumer code relies on copy/assignment operations
    PerStreamPredictor(const StreambufConf *conf_)
        : conf(conf_), state(conf_) { reset(); }
    PerStreamPredictor(const PerStreamPredictor& src)
        : conf(src.conf), state(src.state), priority(src.priority) { }
    PerStreamPredictor& operator = (const PerStreamPredictor& src) {
        if (&src != this) {
            conf = src.conf;
            state = src.state;
            priority = src.priority;
        }
        return *this;
    }
    void reset() { state.reset(); priority = 0; }
    string fmt() const {
        ostringstream ostr;
        ostr << "pred_info " << state.fmt() << " prio " << priority;
        return ostr.str();
    }
    void init_from_predict(const PredictInfo *initial_state,
                           const LongAddr& miss_alloc_va) {
        // initialize from allocation-filter prediction state
        state = *initial_state;
        state.rebase_addr(miss_alloc_va.a);
        priority = state.g_match_counter();
    }
    void init_from_export(const PerStreamPredictor& initial_state,
                          const LongAddr& next_prefetch_addr) {
        // initialize from a PerStreamPredictor which was exported
        state = initial_state.state;
        state.rebase_addr(next_prefetch_addr.a);
        priority = initial_state.priority;
    }
    void note_stream_hit() {
        priority += 2;
        if (priority > conf->stream_priority_saturate)
            priority = conf->stream_priority_saturate - 1;
    }
    void age_stream() {
        priority = DECR_SAT(priority);
    }
    int g_priority() const { return priority; }
    const PredictInfo& pred_info() const { return state; }

    bool ready_to_free_predict() const {
        return state.ready_to_free_predict();
    }
    void free_predict_and_update(mem_addr *predict_addr_ret, int block_bytes) {
        state.free_predict_and_update(predict_addr_ret, block_bytes);
    }
};


// Stream-independent predictor, used to guide stream allocation and initialize
// per-stream predictor state
class StreamAllocPredictor {
    const StreambufConf *conf;

    AssocArray *cam;
    vector<PredictInfo> entries;

    void gen_aa_key(AssocArrayKey& key, int master_id, mem_addr addr) {
        key.lookup = addr >> 2;         // shift by lg(inst-bytes)
        key.match = master_id;
    }

    PredictInfo& ent_idx(long line_num, int way_num) {
        return vec_idx(entries, line_num * conf->stride_pc_assoc + way_num);
    }

public:
    StreamAllocPredictor(const StreambufConf *conf_)
        : conf(conf_), cam(0)
    {
        const char *fname = "StreamAllocPredictor::StreamAllocPredictor(...)";
        sim_assert(conf->stride_pc_entries > 0);
        sim_assert(conf->stride_pc_assoc > 0);
        sim_assert((conf->stride_pc_entries % conf->stride_pc_assoc) == 0);
        int n_lines = conf->stride_pc_entries / conf->stride_pc_assoc;
        if (!(cam = aarray_create(n_lines, conf->stride_pc_assoc, "LRU"))) {
            exit_printf("%s: couldn't create Stride-PC AssocArray\n", fname);
        }
        for (int i = 0; i < conf->stride_pc_entries; ++i) {
            entries.push_back(PredictInfo(conf));
        }
    }
    ~StreamAllocPredictor() {
        aarray_destroy(cam);
    }
    void reset() {
        aarray_reset(cam);
        FOR_ITER(vector<PredictInfo>, entries, iter) {
            iter->reset();
        }
    }

    // returns NULL if no prediction info is available
    const PredictInfo *
    predict_only(int master_id, mem_addr pc, mem_addr access_va) {
        AssocArrayKey key;
        long line_num; int way_num;
        gen_aa_key(key, master_id, pc);
        PredictInfo *ent;
        if (aarray_lookup(cam, &key, &line_num, &way_num)) {
            ent = &ent_idx(line_num, way_num);
        } else {
            ent = NULL;
        }
        PFSG_DB(3)("pfsg: stream predict_only: master %d pc %s va %s -> %s\n",
                   master_id, fmt_mem(pc), fmt_mem(access_va),
                   (ent) ? ent->fmt().c_str() : "(null)");
        return ent;
    }

    void
    update(int master_id, mem_addr pc, mem_addr access_va,
           bool was_cache_miss) {
        AssocArrayKey key;
        long line_num; int way_num;
        gen_aa_key(key, master_id, pc);
        PredictInfo *ent;
        if (aarray_lookup(cam, &key, &line_num, &way_num)) {
            ent = &ent_idx(line_num, way_num);
        } else {
            aarray_replace(cam, &key, &line_num, &way_num, NULL);
            ent = &ent_idx(line_num, way_num);
            ent->reset();
        }
        ent->update(access_va, was_cache_miss);
        PFSG_DB(3)("pfsg: stream predict update: master %d pc %s va %s"
                   " miss %s -> %s\n", master_id, fmt_mem(pc),
                   fmt_mem(access_va), fmt_bool(was_cache_miss),
                   ent->fmt().c_str());
    }

};


enum SBEntryState { 
    SB_Invalid,                 // entire ENTRY is invalid (not just data)
    SB_Fetchable,               // tag match; ready to PF (from predictor)
    SB_Fetching,                // prefetch pending
    SB_FetchCancel,             // prefetch pending but invalidate ack'd
    SB_Present,                 // data present for access
    SBEntryState_last
};
const char *SBEntryState_names[] = {
    "Invalid", "Fetchable", "Fetching", "FetchCancel", "Present", NULL
};


class SBEntry;
typedef list<SBEntry *> SBEntryPtrList;
typedef vector<LongAddr> EvictedBlockSeq;


// A single stream-buffer entry (one cache block)
class SBEntry {
    // WARNING: uses default copy/assignment operators
    const StreambufConf *conf;
    int entry_id;
    SBEntryState state;
    LongAddr base_addr;
    int block_offset;           // offset in this block of original prediction
    bool exclusive;             // ongoing or completed fetch has write perm
    bool data_read;             // data has participated in a hit
    bool was_merged;            // entry has been in a CacheRequest merge
    i64 fetch_time;             // <0: prefetch not started yet

    // parent_fifo points to our parent SBStream's "ents_to_pf" queue, 
    // iff this entry is present in that queue.  This pointer is only used
    // in the event this entry is invalidated while awaiting prefetch,
    // in order to remove it from that queue.
    SBEntryPtrList *parent_fifo;
    SBEntryPtrList::iterator parent_fifo_iter;  // (valid IFF parent_fifo set)

    // parent_evicted_blocks points to our grandparent PFStreamGroup's
    // "blocks_just_evicted" object: a sequence of blocks which we've 
    // evicted/invalidated, but haven't yet told the PrefetchAuditor system
    // about.  This is a hack to address an API mismatch between the
    // PFStreamGroup and PrefetchAuditor systems: PFStreamGroup can 
    // invalidate its own data immediately on hit, before returning; however
    // the PrefetchAuditor insists that the data not be logged as evicted 
    // until after any references to it are reported.  So, we'll defer
    // pfa_block_evict() notification until the next service() call.
    EvictedBlockSeq *parent_evicted_blocks;

public:
    SBEntry(const StreambufConf *conf_, int entry_id_,
            EvictedBlockSeq *parent_evicted_blocks_)
        : conf(conf_), entry_id(entry_id_), state(SB_Invalid),
          parent_fifo(NULL), parent_evicted_blocks(parent_evicted_blocks_)
    {
        // (will be reset() by parent)
    }
    void reset() {
        state = SB_Invalid;
        base_addr.clear();
        block_offset = 0;
        exclusive = false;
        data_read = false;
        was_merged = false;
        fetch_time = -1;
        parent_fifo = NULL;
    }
    string fmt() const;
    void dump(FILE *out, const char *pf) const;
    bool valid() const { return state != SB_Invalid; }
    bool present() const { return state == SB_Present; }
    int g_id() const { return entry_id; }

    void init_pf(const LongAddr& base_addr_, int block_offset_,
                 SBEntryPtrList *parent_fifo_,
                 const SBEntryPtrList::iterator& parent_fifo_iter_) {
        sim_assert(!valid());
        reset();
        state = SB_Fetchable;
        base_addr = base_addr_;
        sim_assert(block_offset_ >= 0);
        block_offset = block_offset_;
        sim_assert(!parent_fifo);
        parent_fifo = parent_fifo_;
        parent_fifo_iter = parent_fifo_iter_;
    }

    const LongAddr& g_base_addr() const {
        sim_assert(valid());
        return base_addr;
    }
    const LongAddr g_addr() const {
        sim_assert(valid());
        LongAddr result(base_addr);
        result.a += block_offset;
        return result;
    }
    bool g_present() const {
        sim_assert(valid());
        return state == SB_Present;
    }
    bool g_write_perm() const {
        sim_assert(valid());
        return (state == SB_Present) && exclusive;
    }
    bool g_data_read() const {
        sim_assert(valid());
        return data_read;
    }
    i64 g_fetch_time() const {
        sim_assert(valid());
        return fetch_time;
    }
    void downgrade() {
        sim_assert(valid());
        exclusive = false;
    }
    bool set_merged() {
        bool result = was_merged;
        was_merged = true;
        return result;
    }

    // (this entry may linger if it needs to track an outstanding, cancelled
    // request, but it won't allow hits in such a state) 
    // caller must test valid() after, and delete from index if invalid
    void inval_data() {
        sim_assert(valid());
        switch (state) {
        case SB_Fetchable:
            sim_assert(parent_fifo);
            parent_fifo->erase(parent_fifo_iter);
            parent_fifo = NULL;
            state = SB_Invalid;
            break;
        case SB_Fetching:
            state = SB_FetchCancel;     // still valid!
            break;
        case SB_FetchCancel:
            // already cancelled
            break;
        case SB_Present:
            state = SB_Invalid;
            break;
        default:
            ENUM_ABORT(SBEntryState, state);
        }
        exclusive = false;
    }

    // data fill from memory hierarchy
    // caller must test valid() after, and delete from index if invalid
    void fill(bool exclusive_, i64 ready_time) { 
        sim_assert(valid());
        switch (state) {
        case SB_Fetching:
            state = SB_Present;
            exclusive = exclusive_;
            data_read = false;
            break;
        case SB_FetchCancel:
            state = SB_Invalid;
            break;
        case SB_Present:
            sim_assert(!conf->force_no_overlap);
            break;
        case SB_Fetchable:
            sim_assert(!conf->force_no_overlap);
            break;
        default:
            ENUM_ABORT(SBEntryState, state);
        }
    }

    bool access_ok(CacheAccessType access_type) const {
        sim_assert(valid());
        bool result = false;
        switch (access_type) {
        case Cache_Read:
            result = (state == SB_Present);
            break;
        case Cache_ReadExcl:
        case Cache_Upgrade:
            result = (state == SB_Present) && exclusive;
            break;
        default:
            ENUM_ABORT(CacheAccessType, access_type);
        }
        return result;
    }

    bool hit_ok(CacheAccessType access_type, bool *first_use_ret) {
        bool result = access_ok(access_type);
        if (result) {
            *first_use_ret = (!data_read && !was_merged);
            data_read = true;
        }
        return result;
    }

    bool ok_to_reset() const {
        // it's not always OK to reset a streambuf entry: in particular, if a
        // prefetch is outstanding, but we've already replied to a
        // coherence invalidate for that block before it arrives, we must
        // remember that until it finally does arrive, to keep us from
        // hitting on that stale data, or otherwise getting requests/replies
        // for that block out of sync.
        //
        // also, we'll refuse to reset an entry for an outstanding,
        // non-cancelled prefetch; we'll assume that the stream buffer
        // tag-following hardware serves similarly to an MSHR, tracking
        // outstanding misses that have been sent to the memory subsystem.
        // this places a limit on the number of outstanding prefetches.
       
        return (state != SB_Fetching) && (state != SB_FetchCancel);
    }

    void fetching() {
        sim_assert(valid());
        sim_assert(state == SB_Fetchable);
        state = SB_Fetching;
        fetch_time = cyc;
        sim_assert(parent_fifo);
        parent_fifo = NULL;     // caller needs to remove us from ents_to_pf
    }

};


string
SBEntry::fmt() const
{
    ostringstream ostr;
    ostr << "st:" << SBEntryState_names[state]
         << ",ba:" << base_addr.fmt()
         << "+" << block_offset
         << ",ex:" << fmt_bool(exclusive)
         << ",dr:" << fmt_bool(data_read)
         << ",wm:" << fmt_bool(was_merged)
         << ",ft:" << fetch_time;
    return ostr.str();
}


void
SBEntry::dump(FILE *out, const char *pf) const
{
    fprintf(out, "%sstate: %s base: %s excl: %s read: %s merge: %s"
            " fetch: %s\n", pf,
            SBEntryState_names[state], base_addr.fmt().c_str(),
            fmt_bool(exclusive), fmt_bool(data_read), fmt_bool(was_merged),
            fmt_i64(fetch_time));
}


// a subset of control info exported from one SBStream; basically, just enough
// to call SBStream::init() in the future.
class SBStreamExported {
    mem_addr pc_;
    LongAddr next_addr_;
    PerStreamPredictor stream_pred_;
public:
    // WARNING: uses default copy/assignment
    SBStreamExported(mem_addr pc__, const LongAddr& next_addr__,
                    const PerStreamPredictor& stream_pred__)
        : pc_(pc__), next_addr_(next_addr__), stream_pred_(stream_pred__)
    {
        // We could do better, e.g. rolling back PerStreamPredictor state
        // to the earliest predicted, prefetched, or present address which
        // has not been read, under the assumption that the thread is
        // 1) going to need that data soon, but 2) about to be moved
    }

    mem_addr pc() const { return pc_; }
    const LongAddr& next_addr() const { return next_addr_; }
    const PerStreamPredictor& stream_pred() const { return stream_pred_; }
    
    string fmt() const {
        ostringstream ostr;
        ostr << "pc " << fmt_x64(pc_) << " next_addr " <<
            next_addr_.fmt() << " stream_pred " << stream_pred_.fmt();
        return ostr.str();
    }
};


// argument list to an SBStream post-self-destruct callback
struct PostDestructCBArgs : public CBQ_Args {
    int reset_stream_id;        // stream ID that was just reset
    PostDestructCBArgs(int reset_stream_id_)
        : reset_stream_id(reset_stream_id_) { }
};


const unsigned LookupMask_Hit = 1;
const unsigned LookupMask_FirstUse = 2;
const unsigned LookupMask_EraseAfter = 4;
const unsigned LookupMask_WritePerm = 8;

// A streambuffer "stream": a set of SBEntry storage blocks, with logic
// to make them track a particular miss stream
class SBStream {
    const StreambufConf *conf;

    GroupStreamIndex *group_idx; // Reference to parent's group-wide index
    int core_id;
    int stream_id;              // index into parent's array
    int n_entries;
    int block_bytes;

    bool allocd;                // is this stream currently allocated?
    // (ignore remaining members if !allocd)

    typedef vector<SBEntry> SBEntryVec;
    typedef vector<SBEntry *> SBEntryPtrVec;
    SBEntryVec entries;         // Block storage for this stream
    SBEntryPtrVec ents_free;    // Entries available for new predictions
    SBEntryPtrList ents_to_pf;  // Entries awaiting prefetch (front goes next)

    PerStreamPredictor stream_pred;

    bool self_destructing;      // self-destruct in progress
    CBQ_Callback *post_destruct_cb;     // to-do after destruct (not owned)
    i64 alloc_time;             // cyc# when stream allocated
    mem_addr alloc_pc;          // PC which led to this stream's allocation
    int master_id;              // Address space ID this stream is working in
    mem_addr last_va;           // Last addr seen (at alloc, or sent for PF)
    i64 last_match_time;        // cyc# of last tag match, or 0

    i64 accesses;               // Accesses to this stream
    i64 hits;                   // Lookup hits on this stream
    i64 prefetches;             // PFs generated by this stream
    i64 full_wins;              // PFs which came back, and then hit
    i64 partial_wins;           // PFs which "won", but merged before fill

    // get a reference to an entry by its index, possibly checking bounds and
    // verifying an address-match
    const SBEntry& ent_idx(int entry_num, const LongAddr& base_addr) const {
        const SBEntry& ent = vec_idx(entries, entry_num);
        sim_assert(ent.g_base_addr() == base_addr);
        return ent;
    }
    SBEntry& ent_idx(int entry_num, const LongAddr& base_addr) {
        return const_cast<SBEntry &>(const_cast<const SBStream *>(this)
                                     ->ent_idx(entry_num, base_addr));
    }

    void align_addr(LongAddr& addr) const {
        addr.a &= ~(block_bytes - 1);    // works for power-of-two block sizes
    }

    void self_destruct_reset() {
        sim_assert(ok_to_reset());
        CBQ_Callback *cb_copy = post_destruct_cb;
        post_destruct_cb = 0;
        reset();
        if (cb_copy) {
            PostDestructCBArgs *args = new PostDestructCBArgs(stream_id);
            i64 result = callback_invoke(cb_copy, args);
            if (result < 0)
                delete cb_copy;
            delete args;
        }
    }

public:

    SBStream(const StreambufConf *conf_,
             GroupStreamIndex *group_idx_, int core_id_, int stream_id_,
             int block_bytes_, EvictedBlockSeq *parent_evicted_blocks_)
        : conf(conf_), group_idx(group_idx_), core_id(core_id_),
          stream_id(stream_id_), block_bytes(block_bytes_), stream_pred(conf_),
          post_destruct_cb(0) {
        // It's ugly that the GroupStreamIndex is managed at both the SBStream
        // and the PFStreamGroup level.  We keep a pointer at this lower
        // level for inserting new entries we create, instead of requiring
        // methods hand back structures for PFStreamGroup to update the
        // index with, or using some sort of up-call.
        n_entries = conf->blocks_per_stream;
        sim_assert(n_entries > 0);
        entries.reserve(n_entries);
        for (int ent_id = 0; ent_id < n_entries; ++ent_id) {
            entries.push_back(SBEntry(conf, ent_id, parent_evicted_blocks_));
        }
        ents_free.reserve(n_entries);
        allocd = false;
        reset();
    }
    ~SBStream() {
        post_destruct_cb = 0;   // owned by parent group, don't delete it here
    }
    void dump(FILE *out, const char *pf) const;
    int g_id() const { return stream_id; }

    void reset() {
        // outside of constructor use, be sure to check ok_to_reset() first
        if (allocd) {
            // remove entries from group-wide address index
            // (this is on the expensive side, with one lookup per erase)
            for (int ent_id = 0; ent_id < n_entries; ent_id++) {
                SBEntry& ent = entries[ent_id];
                if (ent.valid()) {
                    group_idx->erase_block(ent.g_base_addr(),
                                           LookupCoord(stream_id, ent_id));
                }
            }
        }
        ents_free.clear();
        for (int entry_id = n_entries - 1; entry_id >= 0; --entry_id) {
            // we reset entries here instead of in init(), to help catch
            // problems earlier if there's a lag between reset() and init()
            // (reverse order: so allocs start from e0, less confusing)
            SBEntry& ent = entries[entry_id];
            ent.reset();
            ents_free.push_back(&ent);
        }
        ents_to_pf.clear();
        stream_pred.reset();
        allocd = false;
        self_destructing = false;
        post_destruct_cb = 0;   // owned by parent group
        last_match_time = 0;    // probed during LRU replacement
    }

    bool ok_to_reset() const {
        bool result = true;
        FOR_CONST_ITER(SBEntryVec, entries, iter) {
            if (!iter->ok_to_reset()) {
                result = false;
                break;
            }
        }
        return result;
    }

    void init_common(mem_addr pc) {
        sim_assert(!allocd);
        allocd = true;
        alloc_time = cyc;
        alloc_pc = pc;
        last_match_time = 0;
        accesses = 0;
        hits = 0;
        prefetches = 0;
        full_wins = 0;
        partial_wins = 0;
    }

    void init(mem_addr pc, const LongAddr& source_addr,
              const PredictInfo *initial_predict) {
        init_common(pc);
        sim_assert(initial_predict != NULL);
        stream_pred.init_from_predict(initial_predict, source_addr);
        master_id = source_addr.id;
        last_va = source_addr.a;
    }

    void init_from_export(const SBStreamExported& ext) {
        init_common(ext.pc());
        stream_pred.init_from_export(ext.stream_pred(), ext.next_addr());
        master_id = ext.next_addr().id;
        last_va = ext.next_addr().a;
    }
    SBStreamExported *gen_export() const {
        return new SBStreamExported(alloc_pc, LongAddr(last_va, master_id),
                                    stream_pred);
    }

    void age_stream() { stream_pred.age_stream(); }

    i64 g_last_match() const { return last_match_time; }
    int g_priority_counter() const { return stream_pred.g_priority(); }
    bool g_allocd() const { return allocd; }
    i64 g_accesses() const { return accesses; }
    i64 g_hits() const { return hits; }
    i64 g_prefetches() const { return prefetches; }
    i64 g_full_wins() const { return full_wins; }
    i64 g_partial_wins() const { return partial_wins; }
    i64 g_all_wins() const { return full_wins + partial_wins; }
    i64 g_lifetime() const { return cyc - alloc_time; }

    // Test the entry for a hit for the given access.  entry_num _MUST_
    // contain a matching address; the outer-level GroupStreamIndex is used
    // to maintain that.  Parent _MUST_ check for and handle
    // _EraseAfter responses.
    //
    // result is OR-ed LookupMask_* values:
    //   _Hit: address found, data resident, and compatible with access_type
    //   _FirstUse: this _Hit was the first use of the block since fill
    //   _EraseAfter: block invalidated, caller must remove from group index
    //   _WritePerm: entry has write permission
    //
    // fetch_time is written to on any _Hit
    unsigned lookup_hit(int entry_num, const LongAddr& base_addr,
                        CacheAccessType access_type, i64 *fetch_time) {
        SBEntry& ent = ent_idx(entry_num, base_addr);
        unsigned result_flags = 0;
        last_match_time = cyc;
        bool was_first_use;
        accesses++;
        if (ent.hit_ok(access_type, &was_first_use)) {
            hits++;
            result_flags |= LookupMask_Hit;
            if (was_first_use) {
                result_flags |= LookupMask_FirstUse;
                full_wins++;
            }
            if (ent.g_write_perm()) {
                result_flags |= LookupMask_WritePerm;
            }
            *fetch_time = ent.g_fetch_time();
            sim_assert(*fetch_time >= 0);
            stream_pred.note_stream_hit();
        }
        if (conf->always_free_on_match || (access_type == Cache_ReadExcl) ||
            (access_type == Cache_Upgrade)) {
            ent.inval_data();
            // Rather than removing this entry from group_idx here, we rely on
            // the parent to do so after it's done passing over any matching
            // SBStreams.
            if (!ent.valid()) {
                ents_free.push_back(&ent);
                result_flags |= LookupMask_EraseAfter;
            }
        }
        return result_flags;
    }

    bool access_ok(int entry_num, const LongAddr& base_addr,
                   CacheAccessType access_type) const {
        const SBEntry& ent = ent_idx(entry_num, base_addr);
        return ent.access_ok(access_type);
    }

    // revoke any write permission, optionally invalidate the given entry
    void coher_yield(int entry_num, const LongAddr& base_addr,
                     bool invalidate) {
        SBEntry& ent = ent_idx(entry_num, base_addr);
        PFSG_DB(2)("pfsg: yield coord %s",
                   LookupCoord(stream_id, entry_num).fmt().c_str());
        if (invalidate) {
            PFSG_DB(2)(" (inval)");
            ent.inval_data();
            if (!ent.valid()) {
                PFSG_DB(2)(" (erasing)");
                ents_free.push_back(&ent);
                group_idx->erase_block(base_addr,
                                       LookupCoord(stream_id, entry_num));
            } else {
                PFSG_DB(2)(" (linger)");
            }
        } else {
            PFSG_DB(2)(" (downgrade)");
            ent.downgrade();
        }
        PFSG_DB(2)("\n");
    }

    void dirty_evict(int entry_num, const LongAddr& base_addr) {
        SBEntry& ent = ent_idx(entry_num, base_addr);
        PFSG_DB(2)("pfsg: evict coord %s",
                   LookupCoord(stream_id, entry_num).fmt().c_str());
        ent.inval_data();
        if (!ent.valid()) {
            PFSG_DB(2)(" (erasing)");
            ents_free.push_back(&ent);
            group_idx->erase_block(base_addr,
                                   LookupCoord(stream_id, entry_num));
        } else {
            PFSG_DB(2)(" (linger)");
        }
        PFSG_DB(2)("\n");
    }

    // Stop this streambuf from generating future requests, and ask that it
    // be destroyed ASAP.
    void start_self_destruct() {
        PFSG_DB(2)("pfsg: start_self_destruct stream %d", stream_id);
        if (self_destructing) {
            PFSG_DB(2)(" (pending)");
        } else if (int(ents_free.size()) == n_entries) {
            PFSG_DB(2)(" (empty;reset)");
            sim_assert(ok_to_reset());
            self_destruct_reset();
        } else if (ok_to_reset()) {
            PFSG_DB(2)(" (non-empty;reset)");
            self_destruct_reset();
        } else {
            PFSG_DB(2)(" (blocked;flagged)");
            self_destructing = true;
            // We can't reset due to in-flight requests; we'll flag this stream
            // so that it doesn't generate any new requests, and after the
            // last reply arrives (via pf_fill()), we'll reset then.
        }
        PFSG_DB(2)("\n");
    }

    bool is_self_destructing() const { return self_destructing; }

    // Add some work to do after self-destruct is complete.  Must already
    // be self-destructing; only one callback at a time is supported.
    void add_post_destruct_cb(CBQ_Callback *cb) {
        sim_assert(self_destructing);
        sim_assert(!post_destruct_cb);
        post_destruct_cb = cb;
    }

    void stop_pf_entry(int entry_num, int target_master_id) {
        SBEntry& ent = vec_idx(entries, entry_num);
        sim_assert(target_master_id == master_id);
        sim_assert(ent.g_base_addr().id == u32(target_master_id));
        LongAddr base_addr = ent.g_base_addr();
        PFSG_DB(2)("pfsg: stop_pf coord %s base_addr %s",
                   LookupCoord(stream_id, entry_num).fmt().c_str(),
                   base_addr.fmt().c_str());
        ent.inval_data();
        if (!ent.valid()) {
            PFSG_DB(2)(" (erasing)");
            ents_free.push_back(&ent);
            group_idx->erase_block(base_addr,
                                   LookupCoord(stream_id, entry_num));
        } else {
            PFSG_DB(2)(" (linger)");
        }
        PFSG_DB(2)("\n");
    }

    // returns PF "service time", ready_time - fetch_time
    i64 prefetch_fill(int entry_num, const LongAddr& base_addr, 
                      CacheAccessType eff_access_type, i64 ready_time,
                      bool filled_to_cache) {
        SBEntry& ent = ent_idx(entry_num, base_addr);
        // Note: we shouldn't have "effectively a write" access here for data
        // which wasn't filled_to_cache, as that implies dirty data that
        // we'd have to maintain (and we don't support dirty data
        // in the stream buffer).  For data which was filled_to_cache,
        // we'll just drop our copy.
        PFSG_DB(2)("pfsg: fill coord %s(%s) time %s",
                   LookupCoord(stream_id, entry_num).fmt().c_str(),
                   ent.fmt().c_str(), fmt_now());
        sim_assert(filled_to_cache || (eff_access_type == Cache_Read) ||
                   (eff_access_type == Cache_ReadExcl) ||
                   (eff_access_type == Cache_Upgrade));
        bool exclusive = (eff_access_type == Cache_ReadExcl) ||
            (eff_access_type == Cache_Upgrade);
        i64 service_time = ready_time - ent.g_fetch_time();
        sim_assert(ready_time >= cyc);
        sim_assert(service_time >= 0);
        if (!filled_to_cache) {
            PFSG_DB(2)(" (fill,ex:%s)", fmt_bool(exclusive));
            ent.fill(exclusive, ready_time);
        } else {
            PFSG_DB(2)(" (discard)");
            // cache is getting a copy; discard ours
            ent.fill(exclusive, ready_time);
            if (ent.valid()) {
                // (may already be invalid, if fetch was cancelled)
                ent.inval_data();
            }
        }
        if (!ent.valid()) {
            // valid check: the fill may have been cancelled by coherence, or
            // we may have just invalidated it due to fill-to-cache
            PFSG_DB(2)(" (erasing)");
            ents_free.push_back(&ent);
            group_idx->erase_block(base_addr,
                                   LookupCoord(stream_id, entry_num));
        }
        if (self_destructing && ok_to_reset()) {
            PFSG_DB(2)(" (self-destruct:reset)");
            self_destruct_reset();
        }
        PFSG_DB(2)(" -> service_time %s\n", fmt_i64(service_time));
        return service_time;
    }

    // returns partial hit win-time >0, <=0 if a loss or a tie
    i64 prefetch_merged(int entry_num, const LongAddr& base_addr, 
                         bool pf_came_first) {
        SBEntry& ent = ent_idx(entry_num, base_addr);
        i64 result = 0;
        PFSG_DB(2)("pfsg: merge, coord %s(%s)",
                   LookupCoord(stream_id, entry_num).fmt().c_str(),
                   ent.fmt().c_str());
        bool already_merged = ent.set_merged();
        if (!already_merged) {
            // First merge for this entry; win is possible
            PFSG_DB(2)(" (pf_first:%s)", fmt_bool(pf_came_first));
            if (pf_came_first) {
                result = cyc - ent.g_fetch_time();
                sim_assert(result >= 0);        // may also be a tie
                if (result > 0) {
                    PFSG_DB(2)(" (win)");
                }
            }
        } else {
            PFSG_DB(2)(" (already_merged)");
        }
        PFSG_DB(2)(" -> result %s\n", fmt_i64(result));
        if (result > 0) {
            partial_wins++;
        }
        return result;
    }


    bool ready_to_predict() const {
        return allocd && !self_destructing && !ents_free.empty() &&
            stream_pred.ready_to_free_predict();
    }

    // generate prediction(s) for future prefetches on this stream.  this may
    // occur more often than service_prefetch() calls (below), since this is
    // not subject to interconnect bandwidth limitations
    void service_predict() {
        // get addr from predictor
        mem_addr predict_addr = 0;
        PFSG_DB(3)("pfsg: service_predict on s%d: stream_pred: %s;",
                   stream_id, stream_pred.fmt().c_str());
        stream_pred.free_predict_and_update(&predict_addr, block_bytes);
        LongAddr next_pf_addr(predict_addr, master_id);
        LongAddr next_pf_base(next_pf_addr);
        align_addr(next_pf_base);

        PFSG_DB(3)(" -> next_pf_addr %s (base %s), stream_pred %s",
                   next_pf_addr.fmt().c_str(), next_pf_base.fmt().c_str(),
                   stream_pred.fmt().c_str());

        if (!conf->force_no_overlap || !group_idx->tag_present(next_pf_base)) {
            // allocate streambuf entry
            sim_assert(!ents_free.empty());
            SBEntry& ent = *(ents_free.back());
            PFSG_DB(3)(" (alloc ent %d)", ent.g_id());
            ents_free.pop_back();
            // initialize entry, add to index, add to prefetch queue
            ents_to_pf.push_back(&ent);
            // (give entry a pointer (iterator) to the ents_to_pf queue which
            // refers to it, so that we can remove the entry from the
            // queue if we invalidate it before it's submitted for prefetching
            sim_assert(next_pf_addr.a >= next_pf_base.a);
            ent.init_pf(next_pf_base, next_pf_addr.a - next_pf_base.a,
                        &ents_to_pf, --(ents_to_pf.end()));
            group_idx->insert_block(next_pf_base,
                                    LookupCoord(stream_id, ent.g_id()));
        } else {
            PFSG_DB(3)(" (tag present; skip)");
        }
        PFSG_DB(3)("\n");
    }
    
    bool ready_to_prefetch() const {
        return allocd && !self_destructing && !ents_to_pf.empty();
    }

    // get the next base addr that will be prefetched by this stream
    void peek_prefetch(LongAddr *base_addr_ret) const {
        sim_assert(!ents_to_pf.empty());
        const SBEntry& ent = *(ents_to_pf.front());
        *base_addr_ret = ent.g_base_addr();
        PFSG_DB(3)("pfsg: peek_prefetch on s%d: e%d, base_addr %s\n",
                   stream_id, ent.g_id(), ent.g_base_addr().fmt().c_str());
    }

    const SBEntry& peek_pf_entry() const {
        sim_assert(!ents_to_pf.empty());
        const SBEntry& ent = *(ents_to_pf.front());
        return ent;
    }

    // note that the given address (from peek_prefetch()) has been sent out
    // for prefetching
    void service_prefetch(const LongAddr& base_addr) {
        sim_assert(!ents_to_pf.empty());
        SBEntry& ent = *(ents_to_pf.front());
        ents_to_pf.pop_front();
        PFSG_DB(3)("pfsg: service_prefetch on s%d: e%d, base_addr %s\n",
                   stream_id, ent.g_id(), fmt_laddr(base_addr));
        sim_assert(ent.g_base_addr() == base_addr);
        ent.fetching();
        ++prefetches;
        last_va = ent.g_addr().a;
        // "ent" will hang around to accept the prefetch reply from the memory
        // subsystem, and then service any future misses for that block
    }
};


void
SBStream::dump(FILE *out, const char *pf) const
{
    fprintf(out, "%sallocd: %s", pf, fmt_bool(allocd));
    if (!allocd) {
        fprintf(out, "\n");
        return;
    }
    fprintf(out, " self_destructing: %s\n", fmt_bool(self_destructing));

    for (int entry_id = 0; entry_id < n_entries; ++entry_id) {
        const SBEntry& ent = entries.at(entry_id);
        fprintf(out, "%sentry %d:\n", pf, entry_id);
        ent.dump(out, (string(pf) + "  ").c_str());
    }

    fprintf(out, "%sents_free:", pf);
    FOR_CONST_ITER(SBEntryPtrVec, ents_free, iter) {
        fprintf(out, " %d", (*iter)->g_id());
    }
    fprintf(out, "\n");

    fprintf(out, "%sents_to_pf:", pf);
    FOR_CONST_ITER(SBEntryPtrList, ents_to_pf, iter) {
        fprintf(out, " %d", (*iter)->g_id());
    }
    fprintf(out, "\n");

    fprintf(out, "%sstream_pred: %s\n", pf, stream_pred.fmt().c_str());
    fprintf(out, "%salloc_time: %s alloc_pc: %s master_id: %d\n", pf,
            fmt_i64(alloc_time), fmt_mem(alloc_pc), master_id);
    fprintf(out, "%slast_va: %s last_match_time: %s\n", pf,
            fmt_mem(last_va), fmt_i64(last_match_time));
}


bool
core_prefetch_is_quiet(const CoreResources *core,
                       const LongAddr& pf_base_addr) {
    bool result;
    if (core->params.shared_l2cache) {
        // shared L2s: test bus ready-time
        result = corebus_probe_avail(core->request_bus, cyc);
    } else {
        // private L2s: test ready-time on L2 request port
        result = cache_probebank_avail(core->l2cache, pf_base_addr, cyc,
                                       false);
    }
    return result;
}


// StreamImportPlan: a plan for replacing some existing streams with imported
// control info from another streambuf.
class StreamImportPlan {
    vector<int> new_to_old_;    // inbound export# -> victim stream#, or -1
    vector<int> old_to_new_;    // target stream# -> inbound export# or -1
    vector<int> import_order_;  // ordered export#s, highest prio first
    set<int> victims_;          // stream IDs selecting for replacement
    NoDefaultCopy nocopy;
public:
    StreamImportPlan(int export_count_, int stream_count_)
        : new_to_old_(export_count_, -1),
          old_to_new_(stream_count_, -1)
    { }
    // add_import must be called with highest-priority imports first; note
    // that victim stream IDs are considered interchangeable for import
    // purposes (though not when building this plan)
    void add_import(int inbound_export_idx, int victim_stream_idx) {
        sim_assert(inbound_export_idx >= 0);
        sim_assert(inbound_export_idx < intsize(new_to_old_));
        sim_assert(victim_stream_idx >= 0);
        sim_assert(victim_stream_idx < intsize(old_to_new_));
        sim_assert(new_to_old_[inbound_export_idx] == -1);
        sim_assert(old_to_new_[victim_stream_idx] == -1);
        new_to_old_[inbound_export_idx] = victim_stream_idx;
        old_to_new_[victim_stream_idx] = inbound_export_idx;
        import_order_.push_back(inbound_export_idx);
        victims_.insert(victim_stream_idx);
    }
    int export_count() const {         // total # of candidate exports
        return intsize(new_to_old_);
    }
    int stream_count() const {          // total # of candidate targets
        return intsize(old_to_new_);
    }
    int import_count() const {          // number of actual planned imports
        return intsize(import_order_);
    }
    int get_import_ordered_id(int rank) const {  // returns export index
        return vec_idx(import_order_, rank);
    }
    const set<int>& get_victim_streams() const {
        return victims_;
    }
    string fmt() const {
        ostringstream ostr;
        bool first = true;
        ostr << "(";
        FOR_CONST_ITER(vector<int>, import_order_, iter) {
            ostr << ((first) ? "" : ",") << *iter;
            first = false;
        }
        ostr << ") -> {";
        first = true;
        FOR_CONST_ITER(set<int>, victims_, iter) {
            ostr << ((first) ? "" : ",") << *iter;
            first = false;
        }
        ostr << "}";
        return ostr.str();
    }
};

} // Anonymous namespace close



struct PFStreamGroup {
private:
    class RegularServiceCB;
    class DeferredStreamImportCB;
    typedef vector<SBStream> StreamVec;

    const string id;
    const string cfg_path;
    const StreambufConf conf;
    CoreResources * const parent_core;
    CallbackQueue * const time_queue;
    const int block_bytes;
    int block_bytes_lg;
    int n_streams;

    scoped_ptr<StreamAllocPredictor> shared_predict;
    GroupStreamIndex group_idx;
    StreamVec streams;                          // stream ID -> SBStream
    int alloc_tries_since_last_age;
    int rr_stream_predict, rr_stream_prefetch;  // round-robin stream IDs
    GroupStats stats;
    scoped_ptr<RegularServiceCB> service_cb;
    // non-null <=> deferred import outstanding
    scoped_ptr<DeferredStreamImportCB> deferred_import_cb;
    EvictedBlockSeq blocks_just_evicted;

    NoDefaultCopy nocopy;

    void align_addr(LongAddr& addr) const {
        addr.a &= ~(block_bytes - 1);    // works for power-of-two block sizes
    }

    StreamImportPlan *plan_import(const PFStreamExported& ext,
                                  bool prefer_imported_streams,
                                  bool imports_win_ties) const;
    int alloc_filter_and_replace(const LongAddr& base_addr, int addr_offset,
                                 const PredictInfo *pred_info);

    // Select a StreamBuf for [re]allocation.  Returns ID of victim stream, or
    // -1 if none could be allocated (e.g. all entries busy with coherence).
    int alloc_replace_match_lru() const {
        int vic_id = -1;
        i64 vic_time = -1;
        PFSG_DB(3)("pfsg: stream repl lru (id,ok,time):");
        for (int stream_id = 0; stream_id < n_streams; ++stream_id) {
            const SBStream& stream = streams[stream_id];
            // for now, at least, do "stream-wide LRU by tag-match"
            PFSG_DB(3)(" (%d,%s,%s)", stream_id,
                       fmt_bool(stream.ok_to_reset()),
                       fmt_i64(stream.g_last_match()));
            if (!stream.g_allocd()) {
                vic_id = stream_id;
                vic_time = -1;
                break;
            }
            if (((vic_id < 0) || (stream.g_last_match() < vic_time))
                && stream.ok_to_reset()) {
                vic_id = stream_id;
                vic_time = stream.g_last_match();
            }
        }
        PFSG_DB(3)(" -> %d\n", vic_id);
        return vic_id;
    }

    // Select a streambuf with "priority" confidence value <= the accuracy
    // counter from the miss, or -1 if none available
    int alloc_replace_priority(int miss_accuracy) const {
        int vic_id = -1;
        int vic_priority = -1;
        PFSG_DB(3)("pfsg: stream repl prio (id,ok,prio):");
        for (int stream_id = 0; stream_id < n_streams; ++stream_id) {
            const SBStream& stream = streams[stream_id];
            int stream_priority = stream.g_priority_counter();
            PFSG_DB(3)(" (%d,%s,%d)", stream_id,
                       fmt_bool(stream.ok_to_reset()), stream_priority);
            if (!stream.g_allocd()) {
                vic_id = stream_id;
                vic_priority = -1;
                break;
            }
            if (((vic_id < 0) || (stream_priority < vic_priority)) &&
                (stream_priority <= miss_accuracy) &&
                stream.ok_to_reset()) {
                vic_id = stream_id;
                vic_priority = stream_priority;
            }
        }
        PFSG_DB(3)(" -> %d\n", vic_id);
        return vic_id;
    }

    // >= 0: stream ID to access predictor(s) this cyc, -1: nobody
    // (not-const: updates RR counters)
    int arbitrate_for_predict() {
        int result = -1;
        if (conf.use_round_robin_sched) {
            PFSG_DB(3)("pfsg: predict arb rr (id,ok):");
            for (int i = 0; i < n_streams; i++) {
                int this_rr = rr_stream_predict;
                SBStream& stream = streams[this_rr];
                rr_stream_predict = INCR_WRAP(rr_stream_predict, n_streams);
                PFSG_DB(3)(" (%d,%s)", this_rr,
                           fmt_bool(stream.ready_to_predict()));
                if (stream.ready_to_predict()) {
                    result = this_rr;
                    break;
                }
            }
            PFSG_DB(3)(" -> %d\n", result);
        } else {
            int win_stream_id = -1, win_stream_prio = -1;
            i64 win_stream_lastmatch = -1;
            PFSG_DB(3)("pfsg: predict arb prio (id,ok,prio,lru):");
            // XXX: inefficient
            for (int stream_id = 0; stream_id < n_streams; stream_id++) {
                SBStream& stream = streams[stream_id];
                if (!stream.ready_to_predict())
                    continue;
                int stream_prio = stream.g_priority_counter();
                PFSG_DB(3)(" (%d,%s,%d,%s)", stream_id,
                           fmt_bool(stream.ready_to_predict()),
                           stream_prio, fmt_i64(stream.g_last_match()));
                if ((win_stream_id == -1) ||
                    (stream_prio > win_stream_prio) ||
                    ((stream_prio == win_stream_prio) &&
                     (stream.g_last_match() > win_stream_lastmatch))) {
                    win_stream_id = stream_id;
                    win_stream_prio = stream_prio;
                    win_stream_lastmatch = stream.g_last_match();
                }
            }
            result = win_stream_id;
            PFSG_DB(3)(" -> %d\n", result);
        }
        return result;
    }

    // >= 0: stream ID to access p.f. datapath this cyc, -1: nobody
    // (not-const: updates RR counters)
    int arbitrate_for_prefetch() {
        int result = -1;
        if (conf.use_round_robin_sched) {
            PFSG_DB(3)("pfsg: prefetch arb rr (id,ok):");
            for (int i = 0; i < n_streams; i++) {
                int this_rr = rr_stream_prefetch;
                SBStream& stream = streams[this_rr];
                rr_stream_prefetch = INCR_WRAP(rr_stream_prefetch, n_streams);
                PFSG_DB(3)(" (%d,%s)", this_rr,
                           fmt_bool(stream.ready_to_prefetch()));
                if (stream.ready_to_prefetch()) {
                    result = this_rr;
                    break;
                }
            }
            PFSG_DB(3)(" -> %d\n", result);
        } else {
            int win_stream_id = -1, win_stream_prio = -1;
            i64 win_stream_lastmatch = -1;
            PFSG_DB(3)("pfsg: prefetch arb prio (id,ok,prio,lru):");
            // XXX: inefficient
            for (int stream_id = 0; stream_id < n_streams; stream_id++) {
                SBStream& stream = streams[stream_id];
                if (!stream.ready_to_prefetch())
                    continue;
                int stream_prio = stream.g_priority_counter();
                PFSG_DB(3)(" (%d,%s,%d,%s)", stream_id,
                           fmt_bool(stream.ready_to_predict()),
                           stream_prio, fmt_i64(stream.g_last_match()));
                if ((win_stream_id == -1) ||
                    (stream_prio > win_stream_prio) ||
                    ((stream_prio == win_stream_prio) &&
                     (stream.g_last_match() > win_stream_lastmatch))) {
                    win_stream_id = stream_id;
                    win_stream_prio = stream_prio;
                    win_stream_lastmatch = stream.g_last_match();
                }
            }
            result = win_stream_id;
            PFSG_DB(3)(" -> %d\n", result);
        }
        return result;
    }

public:
    PFStreamGroup(const string& id_, const string& cfg_path_,
                  CoreResources *parent_core_,
                  CallbackQueue *time_queue_, int block_bytes_);
    ~PFStreamGroup();
    void reset();
    void print_stats(FILE *out, const char *pf) const;
    void dump(FILE *out, const char *pf) const;
    // note: "export" is a C++ keyword
    PFStreamExported *gen_export(int master_id) const;
    void import(const PFStreamExported& ext, bool prefer_imported_streams,
                bool imports_win_ties);

    bool cache_miss(const LongAddr& base_addr, int addr_offset,
                    CacheAccessType access_type, mem_addr pc,
                    CacheAccessType *allowed_access_type_ret) {
        bool is_hit = false;
        bool hit_was_first_use = false;
        bool hit_has_write_perm = false;
        LookupCoordVec matches;
        LookupCoordVec to_erase;
        group_idx.lookup(base_addr, &matches);
        i64 earliest_prefetch = -1;     // earliest hitting PF

        if (PFSG_DEBUG_COND(1)) {
            printf("pfsg: access %s: addr %s +%d, pc %s; %d tag matches%s",
                   id.c_str(), base_addr.fmt().c_str(), addr_offset,
                   fmt_mem(pc),
                   int(matches.size()),
                   (matches.empty()) ? "" : ":");
            FOR_CONST_ITER(LookupCoordVec, matches, iter) {
                printf(" %s", iter->fmt().c_str());
            }
            printf("\n");
        }

        FOR_CONST_ITER(LookupCoordVec, matches, iter) {
            SBStream& stream = vec_idx(streams, iter->stream_id);
            i64 fetch_time = -1;
            unsigned lookup_stat =
                stream.lookup_hit(iter->entry_id, base_addr, access_type,
                                  &fetch_time);
            PFSG_DB(2)("pfsg: access coord %s ->%s%s%s%s\n",
                       iter->fmt().c_str(),
                       (lookup_stat & LookupMask_Hit) ? " Hit" : "",
                       (lookup_stat & LookupMask_FirstUse) ? " First" : "",
                       (lookup_stat & LookupMask_EraseAfter) ? " Erase" : "",
                       (lookup_stat & LookupMask_WritePerm) ? " Write" : "");
            if (lookup_stat & LookupMask_Hit) {
                is_hit = true;
                if ((earliest_prefetch < 0) ||
                    (fetch_time < earliest_prefetch)) {
                    earliest_prefetch = fetch_time;
                }
                if (lookup_stat & LookupMask_FirstUse)
                    hit_was_first_use = true;
                if (lookup_stat & LookupMask_WritePerm)
                    hit_has_write_perm = true;
            }
            if (lookup_stat & LookupMask_EraseAfter) {
                to_erase.push_back(*iter);
            }
        }

        const PredictInfo *pred_info =  // may be NULL
            shared_predict->predict_only(base_addr.id, pc,
                                         base_addr.a + addr_offset);

        if (matches.empty()) {  // no tag-matches; consider allocating stream
            int new_id;
            if ((new_id = alloc_filter_and_replace(base_addr, addr_offset,
                                                   pred_info)) >= 0) {
                LongAddr miss_va(base_addr);
                miss_va.a += addr_offset;
                SBStream& stream = vec_idx(streams, new_id);

                PFSG_DB(1)("pfsg: alloc stream, new_id %d\n", new_id);
                if (stream.g_allocd()) {
                    PFSG_DB(3)("pfsg: victim accesses: %s hits: %s"
                               " pfs: %s life: %s\n",
                               fmt_i64(stream.g_accesses()),
                               fmt_i64(stream.g_hits()),
                               fmt_i64(stream.g_prefetches()),
                               fmt_i64(stream.g_lifetime()));
                    stats.per_sb.replaced++;
                    if (!stream.g_all_wins())
                        stats.per_sb.useless++;
                    stats.per_sb.accesses.add_sample(stream.g_accesses());
                    stats.per_sb.hits.add_sample(stream.g_hits());
                    stats.per_sb.prefetches.add_sample(stream.g_prefetches());
                    stats.per_sb.full_wins.add_sample(stream.g_full_wins());
                    stats.per_sb.partial_wins.
                        add_sample(stream.g_partial_wins());
                    stats.per_sb.all_wins.add_sample(stream.g_all_wins());
                    stats.per_sb.lifetime.add_sample(stream.g_lifetime());
                }
                stats.stream_allocs++;

                sim_assert(stream.ok_to_reset());
                stream.reset();         // release & invalidate old stuff
                stream.init(pc, miss_va, pred_info);
            }
        } else if (!to_erase.empty()) {
            group_idx.erase_blocks(base_addr, to_erase);
        }

        stats.accesses++;
        if (is_hit) {
            stats.hits++;
            *allowed_access_type_ret =
                (hit_has_write_perm) ? Cache_ReadExcl : Cache_Read;
        }
        if (hit_was_first_use) {
            stats.pf_used++;
            stats.full_win_time.add_sample(cyc - earliest_prefetch);
            stats.all_win_time.add_sample(cyc - earliest_prefetch);
        }
        if (PFSG_DEBUG_COND(4)) {
            printf("pfsg: after-access dump:\n");
            this->dump(stdout, "  ");
        }

        return is_hit;
    }

    bool access_ok(const LongAddr& base_addr,
                   CacheAccessType access_type) const {
        bool result = false;
        LookupCoordVec matches;
        group_idx.lookup(base_addr, &matches);
        FOR_CONST_ITER(LookupCoordVec, matches, iter) {
            const SBStream& stream = vec_idx(streams, iter->stream_id);
            if (stream.access_ok(iter->entry_id, base_addr, access_type)) {
                result = true;
                break;
            }
        }
        return result;
    }

    void mem_commit(mem_addr pc, LongAddr va, bool was_write, bool was_miss) {
        //LongAddr base_addr(va);
        //align_addr(base_addr);
        PFSG_DB(2)("pfsg: commit %s: va %s pc %s write %s miss %s\n",
                   id.c_str(), fmt_laddr(va), fmt_x64(pc), fmt_bool(was_write),
                   fmt_bool(was_miss));
        shared_predict->update(va.id, pc, va.a, was_miss);
        if (PFSG_DEBUG_COND(5)) {
            printf("pfsg: after-commit dump:\n");
            this->dump(stdout, "  ");
        }
    }

    void coher_yield(const LongAddr& base_addr, bool invalidate) {
        LookupCoordVec matches;
        group_idx.lookup(base_addr, &matches);
        PFSG_DB(1)("pfsg: yield %s: base_addr %s inval %s; %d tag matches\n",
                   id.c_str(), fmt_laddr(base_addr), fmt_bool(invalidate),
                   int(matches.size()));
        FOR_CONST_ITER(LookupCoordVec, matches, iter) {
            SBStream& stream = vec_idx(streams, iter->stream_id);
            // individual streams will erase their entries from the
            // index as need (some may need to linger for a bit)
            stream.coher_yield(iter->entry_id, base_addr, invalidate);
        }
        if (PFSG_DEBUG_COND(4)) {
            printf("pfsg: after-yield dump:\n");
            this->dump(stdout, "  ");
        }
    }

    void cache_dirty_evict(const LongAddr& base_addr) {
        LookupCoordVec matches;
        group_idx.lookup(base_addr, &matches);
        PFSG_DB(1)("pfsg: dirty_evict %s: base_addr %s; %d tag matches\n",
                   id.c_str(), fmt_laddr(base_addr), int(matches.size()));
        FOR_CONST_ITER(LookupCoordVec, matches, iter) {
            SBStream& stream = vec_idx(streams, iter->stream_id);
            stream.dirty_evict(iter->entry_id, base_addr);
        }
        if (PFSG_DEBUG_COND(4)) {
            printf("pfsg: after-evict dump:\n");
            this->dump(stdout, "  ");
        }
    }

    void stop_thread_pf(int master_id) {
        LookupCoordVec matches;
        set<SBStream *> streams_to_kill;
        group_idx.lookup_masterid_slow(master_id, &matches);
        PFSG_DB(1)("pfsg: stop_thread_pf %s: master_id %d; %d tag matches\n",
                   id.c_str(), master_id, int(matches.size()));
        FOR_CONST_ITER(LookupCoordVec, matches, iter) {
            SBStream& stream = vec_idx(streams, iter->stream_id);
            stream.stop_pf_entry(iter->entry_id, master_id);
            streams_to_kill.insert(&stream);
        }
        for (set<SBStream *>::const_iterator iter = streams_to_kill.begin();
             iter != streams_to_kill.end(); ++iter) {
            (*iter)->start_self_destruct();
        }
        if (PFSG_DEBUG_COND(4)) {
            printf("pfsg: after-stop dump:\n");
            this->dump(stdout, "  ");
        }
    }

    void pf_fill(const LongAddr& base_addr,
                 CacheAccessType eff_access_type, i64 ready_time,
                 bool filled_to_cache) {
        LookupCoordVec matches;
        group_idx.lookup(base_addr, &matches);
        PFSG_DB(1)("pfsg: fill %s: base_addr %s e_a_t %s ready_time %s"
                   " filled_to_cache %s; %d tag matches\n",
                   id.c_str(),
                   fmt_laddr(base_addr),
                   CacheAccessType_names[eff_access_type],
                   fmt_i64(ready_time), fmt_bool(filled_to_cache),
                   int(matches.size()));
        if (SP_F(matches.empty())) {
            abort_printf("unmatched streambuf pf_fill,"
                         " time %s base_addr %s access %s ready_time %s\n",
                         fmt_now(), fmt_laddr(base_addr),
                         CacheAccessType_names[eff_access_type],
                         fmt_i64(ready_time));
        }
        FOR_CONST_ITER(LookupCoordVec, matches, iter) {
            SBStream& stream = vec_idx(streams, iter->stream_id);
            // ready_time - fetch_time
            i64 service_time =
                stream.prefetch_fill(iter->entry_id, base_addr,
                                     eff_access_type, ready_time,
                                     filled_to_cache);
            stats.pf_service_time.add_sample(service_time);
        }
        if (PFSG_DEBUG_COND(4)) {
            printf("pfsg: after-fill dump:\n");
            this->dump(stdout, "  ");
        }
    }

    void pf_merged(const LongAddr& base_addr, bool pf_came_first) {
        LookupCoordVec matches;
        group_idx.lookup(base_addr, &matches);
        PFSG_DB(1)("pfsg: merged %s: time %s base_addr %s pf_first %s;"
                   " %d tag matches\n", id.c_str(), fmt_now(),
                   fmt_laddr(base_addr), fmt_bool(pf_came_first),
                   int(matches.size()));
        if (SP_F(matches.empty())) {
            abort_printf("unmatched streambuf pf_merged,"
                         " time %s base_addr %s\n",
                         fmt_now(), fmt_laddr(base_addr));
        }
        FOR_CONST_ITER(LookupCoordVec, matches, iter) {
            SBStream& stream = vec_idx(streams, iter->stream_id);
            i64 win_time = 
                stream.prefetch_merged(iter->entry_id, base_addr,
                                       pf_came_first);
            if (win_time > 0) {
                stats.pf_used++;
                stats.partial_win_time.add_sample(win_time);
                stats.all_win_time.add_sample(win_time);
            }
        }
    }

    // regularly-triggered method which updates internal state independent
    // of response to individual misses (this is the autonomous control
    // part of the stream buffer)
    void service() {
        // 1. arbitrate among streams[], winner generates a prediction
        // - or just run them all in parallel?  but the "is anybody else
        //   prefetching this?" test costs too much...
        // - we could easily run N stride predictors in parallel, but they're
        //   so trivial we don't really gain anything from running them far
        //   ahead of actual prefetching
        // 2. arbitrate among streams[], winner gets to prefetch
        int arb_prefetch_winner = arbitrate_for_prefetch();
        int arb_predict_winner = arbitrate_for_predict();

        PFSG_DB(1)("pfsg: service %s: time %s; pf stream %d,"
                   " pred stream %d\n", id.c_str(), fmt_now(),
                   arb_prefetch_winner, arb_predict_winner);

        blocks_just_evicted.clear();

        if (arb_prefetch_winner >= 0) {
            LongAddr base_addr;
            SBStream& stream = vec_idx(streams, arb_prefetch_winner);
            stream.peek_prefetch(&base_addr);    // doesn't change stream
            if (!conf.prefetch_only_when_quiet ||
                core_prefetch_is_quiet(parent_core, base_addr)) {
                const SBEntry& pf_ent = stream.peek_pf_entry();
                int pf_stat = 
                    cachesim_prefetch_for_streambuf(parent_core, base_addr,
                                                    conf.prefetch_as_exclusive,
                                                    (const void *) &pf_ent,
                                                    stream.g_id(),
                                                    pf_ent.g_id());
                PFSG_DB(1)("pfsg: service pf addr %s; pf_stat %d\n",
                           fmt_laddr(base_addr), pf_stat);
                if (pf_stat) {
                    stream.service_prefetch(base_addr);
                    stats.pf_reqs_ok++;
                } else {
                    PFSG_DB(3)("pfsg: prefetch request for %s failed\n",
                               fmt_laddr(base_addr));
                    stats.pf_reqs_failed++;
                }
            } else {
                PFSG_DB(1)("pfsg: skipped pf service for %s,"
                           " resources busy\n", fmt_laddr(base_addr));
            }
        }

        if (arb_predict_winner >= 0) {
            streams[arb_predict_winner].service_predict();
        }

        if (PFSG_DEBUG_COND(4) &&
            ((arb_prefetch_winner >= 0) || (arb_predict_winner >= 0))) {
            printf("pfsg: after-service dump:\n");
            this->dump(stdout, "  ");
        }
    }
};


// Container of exported control info from multiple streams
struct PFStreamExported {
private:
    friend class PFStreamGroup;                 // lazy, and I'm in a hurry
    typedef vector<SBStreamExported *> EntryVec;         // not sorted
    EntryVec entries;                           // entries pointers "owned"
    NoDefaultCopy nocopy;

public:
    PFStreamExported() { }
    PFStreamExported(const PFStreamExported& src)
        : entries(src.entries) { }
    ~PFStreamExported() {
        FOR_ITER(EntryVec, entries, iter) {
            delete *iter;
        }
    }

    bool empty() const { return entries.empty(); }
    int size() const { return intsize(entries); }

    long estimate_size_bits() const {
        // vague estimate per-entry:
        // - 64 bits for base address (exact)
        // - 20 bits for stride (not enforced)
        // - 4 bits for confidence or other metadata
        const int kBitsPerEnt = 88;
        return kBitsPerEnt * entries.size();
    }

    // claims ownership of "ent"
    void add_stream_export(SBStreamExported *ent) {
        entries.push_back(ent);
    }

    void dump(FILE *out, const char *pf) const {
        fprintf(out, "%ssize: %d\n", pf, intsize(entries));
        for (int ent_idx = 0; ent_idx < intsize(entries); ++ent_idx) {
            const SBStreamExported& stream_exp = *entries[ent_idx];
            fprintf(out, "%s  [%d]: %s\n", pf, ent_idx,
                    stream_exp.fmt().c_str());
        }
    }
};


// (callback instance re-used for the life of the PFStreamGroup)
class PFStreamGroup::RegularServiceCB : public CBQ_Callback {
    PFStreamGroup& sg;
public:
    RegularServiceCB(PFStreamGroup& sg_) : sg(sg_) { }
    i64 invoke(CBQ_Args *args) {
        sg.service();
        return cyc + 1;
    }
};


PFStreamGroup::PFStreamGroup(const string& id_, const string& cfg_path_,
                             CoreResources *parent_core_,
                             CallbackQueue *time_queue_,
                             int block_bytes_)
    : id(id_), cfg_path(cfg_path_), conf(parent_core_, cfg_path_),
      parent_core(parent_core_),
      time_queue(time_queue_), block_bytes(block_bytes_)
{
    if (block_bytes < 1) {
        exit_printf("bad streambuf block_bytes (%d)\n", block_bytes);
    }
    if ((block_bytes_lg = log2_exact(block_bytes)) < 0) {
        exit_printf("streambuf block_bytes (%d) not a power of 2\n",
                    block_bytes);
    }
    n_streams = conf.n_streams;

    shared_predict.reset(new StreamAllocPredictor(&conf));

    streams.reserve(n_streams);
    for (int stream_id = 0; stream_id < n_streams; ++stream_id) {
        streams.push_back(SBStream(&conf, &group_idx, parent_core->core_id,
                                   stream_id, block_bytes,
                                   &blocks_just_evicted));
    }

    service_cb.reset(new RegularServiceCB(*this));
    callbackq_enqueue(time_queue, cyc, service_cb.get());

    this->reset();
}


PFStreamGroup::~PFStreamGroup()
{
    callbackq_cancel_ret(time_queue, service_cb.get());
}


void
PFStreamGroup::reset()
{
    shared_predict->reset();
    group_idx.reset();
    FOR_ITER(StreamVec, streams, iter) {
        iter->reset();
    }
    alloc_tries_since_last_age = 0;
    rr_stream_predict = 0;
    rr_stream_prefetch = 0;
    stats.reset();
}


void
PFStreamGroup::print_stats(FILE *out, const char *pf) const
{
    fprintf(out, "%saccesses: %s hits: %s %.2f%%\n", pf,
            fmt_i64(stats.accesses), fmt_i64(stats.hits),
            100.0 * double(stats.hits) / stats.accesses);
    fprintf(out, "%spf_reqs_ok: %s failed: %s %.2f%%\n", pf,
            fmt_i64(stats.pf_reqs_ok), fmt_i64(stats.pf_reqs_failed),
            100.0 * double(stats.pf_reqs_failed) /
            (stats.pf_reqs_ok + stats.pf_reqs_failed));
    fprintf(out, "%s  used: %s %.2f%% full_wins: %s %.2f%% "
            "partial_wins: %s %.2f%%\n", pf,
            fmt_i64(stats.pf_used),
            100.0 * double(stats.pf_used) / stats.pf_reqs_ok,
            fmt_i64(stats.full_win_time.g_count()),
            100.0 * double(stats.full_win_time.g_count()) / stats.pf_reqs_ok,
            fmt_i64(stats.partial_win_time.g_count()),
            100.0 * double(stats.partial_win_time.g_count())
            / stats.pf_reqs_ok);
    fprintf(out, "%sfull_win_time: %s\n", pf,
            fmt_bstat_double(stats.full_win_time).c_str());
    fprintf(out, "%spartial_win_time: %s\n", pf,
            fmt_bstat_double(stats.partial_win_time).c_str());
    fprintf(out, "%sall_win_time: %s\n", pf,
            fmt_bstat_double(stats.all_win_time).c_str());
    fprintf(out, "%spf_service_time: %s\n", pf,
            fmt_bstat_double(stats.pf_service_time).c_str());
    fprintf(out, "%sstream_allocs: %s\n", pf, fmt_i64(stats.stream_allocs));

    fprintf(out, "%sper-replaced-stream instance stats:\n", pf);
    fprintf(out, "%s  replaced: %s useless: %s %.2f%%\n", pf,
            fmt_i64(stats.per_sb.replaced), fmt_i64(stats.per_sb.useless),
            100.0 * double(stats.per_sb.useless) / stats.per_sb.replaced);
    fprintf(out, "%s  accesses: %s\n", pf,
            fmt_bstat_i64(stats.per_sb.accesses).c_str());
    fprintf(out, "%s  hits: %s\n", pf,
            fmt_bstat_i64(stats.per_sb.hits).c_str());
    fprintf(out, "%s  prefetches: %s\n", pf,
            fmt_bstat_i64(stats.per_sb.prefetches).c_str());
    fprintf(out, "%s  full_wins: %s\n", pf,
            fmt_bstat_i64(stats.per_sb.full_wins).c_str());
    fprintf(out, "%s  partial_wins: %s\n", pf,
            fmt_bstat_i64(stats.per_sb.partial_wins).c_str());
    fprintf(out, "%s  all_wins: %s\n", pf,
            fmt_bstat_i64(stats.per_sb.all_wins).c_str());
    fprintf(out, "%s  lifetime: %s\n", pf,
            fmt_bstat_i64(stats.per_sb.lifetime).c_str());

    fprintf(out, "%simport stats:\n", pf);
    fprintf(out, "%s  gr_total: %s gr_reject: %s\n", pf,
            fmt_i64(stats.import.gr_total), fmt_i64(stats.import.gr_reject));
    fprintf(out, "%s  st_acc_prompt: %s st_acc_defer: %s\n", pf,
            fmt_i64(stats.import.st_acc_prompt),
            fmt_i64(stats.import.st_acc_defer));
    fprintf(out, "%s  st_rej_targ: %s st_rej_group: %s\n", pf,
            fmt_i64(stats.import.st_rej_targ),
            fmt_i64(stats.import.st_rej_group));
}


void
PFStreamGroup::dump(FILE *out, const char *pf) const
{
    fprintf(out, "%sid: %s cfg_path: %s\n", pf,
            id.c_str(), cfg_path.c_str());
    for (int stream_id = 0; stream_id < n_streams; stream_id++) {
        const SBStream& stream = streams.at(stream_id);
        fprintf(out, "%sstream %d:\n", pf, stream_id);
        stream.dump(out, (string(pf) + "  ").c_str());
    }
    fprintf(out, "%salloc_tries_since_last_age: %d\n", pf,
            alloc_tries_since_last_age);
    fprintf(out, "%srr_stream_predict: %d rr_stream_prefetch: %d\n", pf,
            rr_stream_predict, rr_stream_prefetch);
}


PFStreamExported *
PFStreamGroup::gen_export(int master_id) const
{
    scoped_ptr<PFStreamExported> ret(new PFStreamExported());
    LookupCoordVec matches;
    group_idx.lookup_masterid_slow(master_id, &matches);
    PFSG_DB(1)("pfsg: export %s: master_id %d; %d tag matches\n",
               id.c_str(), master_id, int(matches.size()));
    if (PFSG_DEBUG_COND(3)) {
        printf("pfsg: export %s: pre-export state:\n", id.c_str());
        this->dump(stdout, "  ");
    }

    set<const SBStream *> streams_to_export;
    FOR_CONST_ITER(LookupCoordVec, matches, iter) {
        const SBStream& stream = vec_idx(streams, iter->stream_id);
        sim_assert(stream.g_allocd());
        if (!stream.is_self_destructing()) {
            streams_to_export.insert(&stream);
        }
    }
    FOR_CONST_ITER(set<const SBStream *>, streams_to_export, iter) {
        const SBStream& stream = **iter;
        SBStreamExported *stream_ext = stream.gen_export();
        ret->add_stream_export(stream_ext);
    }
    if (PFSG_DEBUG_COND(3)) {
        printf("pfsg: export output:\n");
        ret->dump(stdout, "  ");
    }
    return scoped_ptr_release(ret);
}


namespace {

struct ImportPrioSort {
    int prio;
    int entry_id;
    ImportPrioSort(int prio_, int entry_id_)
        : prio(prio_), entry_id(entry_id_) { }
    bool operator < (const ImportPrioSort& o2) const {
        return prio < o2.prio;
    }
    bool operator == (const ImportPrioSort& o2) const {
        return prio == o2.prio;
    }
    string fmt() const {
        ostringstream ostr;
        ostr << prio << "," << entry_id;
        return ostr.str();
    }
};

typedef vector<ImportPrioSort> ImportPrioVec;


// helper for PFStreamGroup::plan_import()
// new_ents and old_ents sorted in ascending order of priority, with prio -1
// denoting free streams
void
import_plan_priority(StreamImportPlan *result, const ImportPrioVec& new_ents,
                     const ImportPrioVec& old_ents,
                     bool prefer_imported_streams, bool imports_win_ties)
{
    // replace each existing stream with the lowest-priority export that
    // meets/exceeds its priority (or unconditionally, if always_prefer_new)

    // target_idx will be the highest existing stream# that gets replaced.  we
    // start it with min of the two array endpoints: if |new_ents| >
    // |old_ents|, then it starts out pointing to the last (i.e. highest-prio)
    // entry in old_ents.  if |new_ents| < |old_ents|, it's trimmed back to
    // only consider the lowest-priority |new_ents| existing streams
    int target_idx = MIN_SCALAR(intsize(old_ents), intsize(new_ents)) - 1;
 
    if (!new_ents.empty() && !prefer_imported_streams) {
        // move target_idx past streams we won't replace due to priority
        const ImportPrioSort& highest_new = new_ents.back();
        while ((target_idx >= 0) &&
               ((highest_new < old_ents[target_idx]) ||
                (!imports_win_ties &&
                 (highest_new == old_ents[target_idx])))) {
            --target_idx;
        }
    }

    // now, target_idx points to the highest-priority entry which we will
    // actually replace; we can just fill down from here, starting with the
    // highest-priority export, until we run out of entries
    
    for (int export_idx = intsize(new_ents) - 1;
         (export_idx >= 0) && (target_idx >= 0);
         --export_idx, --target_idx) {
        result->add_import(new_ents[export_idx].entry_id,
                           old_ents[target_idx].entry_id);
    }
}


}       // Anonymous namespace close


StreamImportPlan *
PFStreamGroup::plan_import(const PFStreamExported& ext,
                           bool prefer_imported_streams,
                           bool imports_win_ties) const
{
    ImportPrioVec new_ents;   // new candidates from export
    ImportPrioVec old_ents;   // existing streams

    for (int idx = 0; idx < int(ext.entries.size()); ++idx) {
        const SBStreamExported& ent = *(ext.entries[idx]);
        // note: priority saturates at stream_priority_saturate
        new_ents.push_back(ImportPrioSort(ent.stream_pred().g_priority(),
                                          idx));
    }
    for (int idx = 0; idx < int(streams.size()); ++idx) {
        // note: priority saturates at stream_priority_saturate
        const SBStream& ent = streams[idx];
        // ent_prio -1: illegal priority value, signal that this stream is free
        int ent_prio = (ent.g_allocd()) ? ent.g_priority_counter() : -1;
        old_ents.push_back(ImportPrioSort(ent_prio, idx));
    }

    // sort in ascending order of priority
    std::stable_sort(new_ents.begin(), new_ents.end());
    std::stable_sort(old_ents.begin(), old_ents.end());

    if (PFSG_DEBUG_COND(2)) {
        printf("pfsg: plan_import, old_ents [");
        FOR_CONST_ITER(ImportPrioVec, old_ents, iter) {
            printf(" %s", iter->fmt().c_str());
        }
        printf(" ] new_ents [");
        FOR_CONST_ITER(ImportPrioVec, new_ents, iter) {
            printf(" %s", iter->fmt().c_str());
        }
        printf(" ]\n");
    }

    scoped_ptr<StreamImportPlan>
        result(new StreamImportPlan(int(new_ents.size()),
                                    int(old_ents.size())));

    import_plan_priority(result.get(), new_ents, old_ents,
                         prefer_imported_streams, imports_win_ties);

    if (PFSG_DEBUG_COND(2)) {
        printf("pfsg: plan_import, result (new->victim): %s\n",
               result->fmt().c_str());
    }

    return scoped_ptr_release(result);
}


// Callback that hangs around and processes stream imports that were
// requested via pfsg_import(), but which couldn't be handled right away.
// This is invoked by individual SBStreams just after a self-destruct reset(),
// when there is an outstanding stream-import operation pending.  (Only one
// may be pending at a time, across the entire PFStreamGroup.)
//
// This is re-used a small number of times; this "knows" how many times it
// should be called, and returns -1 after the last invocation (so the caller
// knows to destroy it).
class PFStreamGroup::DeferredStreamImportCB : public CBQ_Callback {
    PFStreamGroup& sg_;
    // owned by this object; highest prio first
    vector<SBStreamExported> deferred_imports_;
    int next_import_idx_;       // index in deferred_imports[]
    set<int> waiting_targets_;  // target stream IDs we're waiting for
public:
    DeferredStreamImportCB(PFStreamGroup& sg__,
                           const vector<const SBStreamExported *>&
                           source_imports,
                           const vector<int>& target_streams)
        : sg_(sg__), next_import_idx_(0) {
        for (int i = 0; i < intsize(source_imports); ++i) {
            // make our own copy of the stream exports
            deferred_imports_.push_back(*source_imports[i]);
        }
        for (int i = 0; i < intsize(target_streams); ++i) {
            if (!waiting_targets_.insert(target_streams[i]).second) {
                abort_printf("pfsg deferred import to %s, target stream %d "
                             "duplicated\n", sg_.id.c_str(),
                             target_streams[i]);
            }
        }
    }
    i64 invoke(CBQ_Args *base_args) {
        const PostDestructCBArgs *args =
            dynamic_cast<PostDestructCBArgs *>(base_args);
        sim_assert(args != NULL);
        int target_id = args->reset_stream_id;
        if (!waiting_targets_.erase(target_id)) {
            abort_printf("pfsg deferred import to %s: reset stream #%d "
                         "not in waiting set\n", sg_.id.c_str(), target_id);
        }
        SBStream& target_stream = vec_idx(sg_.streams, target_id);
        const SBStreamExported& source_export =
            vec_idx(deferred_imports_, next_import_idx_);
        ++next_import_idx_;
        sim_assert(target_stream.ok_to_reset());
        target_stream.init_from_export(source_export);

        bool is_final = waiting_targets_.empty();
        if (is_final) {
            // caller will delete us, so release from ownership
            sg_.deferred_import_cb.reset();
        }
        return (is_final) ? -1 : 1;
    }
};


void
PFStreamGroup::import(const PFStreamExported& ext,
                      bool prefer_imported_streams, bool imports_win_ties)
{
    PFSG_DB(1)("pfsg: import %s: %d streams submitted\n",
               id.c_str(), int(ext.size()));
    ++stats.import.gr_total;
    if (deferred_import_cb) {
        PFSG_DB(1)("pfsg: import rejected at %s, one already outstanding\n",
                   fmt_now());
        ++stats.import.gr_reject;
        stats.import.st_rej_group += ext.size();
        return;
    }

    if (PFSG_DEBUG_COND(2)) {
        printf("pfsg: streams to import:\n");
        ext.dump(stdout, "  ");
    }

    scoped_ptr<StreamImportPlan> import_plan;
    import_plan.reset(plan_import(ext, prefer_imported_streams,
                                  imports_win_ties));

    vector<int> victims_left(import_plan->get_victim_streams().rbegin(),
                             import_plan->get_victim_streams().rend());
    const int import_count = import_plan->import_count();
    stats.import.st_rej_targ += ext.size() - import_count;

    vector<int> busy_victims;   // victims we couldn't reset just yet
    // imports couldn't complete yet due to busy victims (note: points
    // to storage owned by PFStreamExported).  highest priority first.
    vector<const SBStreamExported*> waiting_imports;

    sim_assert(intsize(victims_left) == import_count);

    for (int import_prio_seq = 0; import_prio_seq < import_count;
         ++import_prio_seq) {
        int target_id = pop_back_ret(victims_left);
        SBStream& target_stream = vec_idx(streams, target_id);
        int source_export_id =
            import_plan->get_import_ordered_id(import_prio_seq);
        const SBStreamExported *source_export =
            vec_idx(ext.entries, source_export_id);
        if (target_stream.ok_to_reset()) {
            target_stream.reset();
            target_stream.init_from_export(*source_export);
            ++stats.import.st_acc_prompt;
        } else {
            busy_victims.push_back(target_id);
            waiting_imports.push_back(source_export);
            ++stats.import.st_acc_defer;
        }
    }

    sim_assert(busy_victims.size() == waiting_imports.size());
    
    if (!waiting_imports.empty()) {
        sim_assert(!deferred_import_cb);
        DeferredStreamImportCB *inj_cb = 
            new DeferredStreamImportCB(*this, waiting_imports, busy_victims);
        deferred_import_cb.reset(inj_cb);
        FOR_CONST_ITER(vector<int>, busy_victims, iter) {
            SBStream& target_stream = vec_idx(streams, *iter);
            target_stream.start_self_destruct();
            target_stream.add_post_destruct_cb(inj_cb);
        }
    }

    PFSG_DB(1)("pfsg: import %s: %d streams accepted promptly, %d deferred\n",
               id.c_str(), import_count - intsize(waiting_imports),
               intsize(waiting_imports));
    if (PFSG_DEBUG_COND(4)) {
        printf("pfsg: after-import dump:\n");
        this->dump(stdout, "  ");
    }
}


// explicit allocation filter: decide whether or not a given miss ought
// to have a new entry allocated, and if so, which entry should be replaced.
// (does not actually alter victim entry)
//
// returns N >=0: yes, do allocation, use stream N
//         -1: no allocation
//
// pred_info may be NULL
int
PFStreamGroup::alloc_filter_and_replace(const LongAddr& base_addr,
                                        int addr_offset,
                                        const PredictInfo *pred_info)
{
    int result = -1;
    if (conf.use_two_miss_alloc_filter) {
        // "two-miss stride filtering" from Farkas, also Sherwood:
        // A load miss is allowed to allocate a streambuf if it misses two
        // times in a row, and the last two strides are the same.
        if (pred_info &&
            pred_info->two_miss_filter_pass(base_addr.a + addr_offset)) {
            result = alloc_replace_match_lru();
        }
    } else {
        if (pred_info) {
            // accuracy counter of candidate mem inst, from shared predictor
            int miss_accuracy_counter = pred_info->g_match_counter();
            if (miss_accuracy_counter >= conf.alloc_min_confidence_thresh) {
                result = alloc_replace_priority(miss_accuracy_counter);
            }
        }
    }

    alloc_tries_since_last_age++;
    if (alloc_tries_since_last_age >= conf.stream_priority_age_allocs) {
        PFSG_DB(1)("pfsg: aging streams\n");
        FOR_ITER(StreamVec, streams, iter) {
            iter->age_stream();
        }
        alloc_tries_since_last_age = 0;
    }

    return result;
}


//
// C interface
//

PFStreamGroup *
pfsg_create(const char *id, const char *config_path,
            struct CoreResources *parent_core,
            struct CallbackQueue *time_queue,
            int block_bytes)
{
    return new PFStreamGroup(string(id), string(config_path), parent_core,
                             time_queue, block_bytes);
}

void
pfsg_destroy(PFStreamGroup *sg)
{
    delete sg;
}

int
pfsg_cache_miss(PFStreamGroup *sg, LongAddr base_addr, int addr_offset,
                int cache_access_type, mem_addr pc,
                int *allowed_access_type_ret)
{
    CacheAccessType tmp_access_ret;
    bool result = sg->cache_miss(base_addr, addr_offset,
                                 CacheAccessType(cache_access_type), pc,
                                 &tmp_access_ret);
    if (result && allowed_access_type_ret) {
        *allowed_access_type_ret = static_cast<int>(tmp_access_ret);
    }
    return result;
}

int
pfsg_access_ok(const PFStreamGroup *sg, LongAddr base_addr,
               int cache_access_type)
{
    return sg->access_ok(base_addr, CacheAccessType(cache_access_type));
}

void
pfsg_mem_commit(PFStreamGroup *sg, mem_addr pc, LongAddr va,
                int was_write, int was_miss)
{
    sg->mem_commit(pc, va, was_write, was_miss);
}

void
pfsg_coher_yield(PFStreamGroup *sg, LongAddr base_addr, int invalidate)
{
    sg->coher_yield(base_addr, invalidate);
}

void
pfsg_cache_dirty_evict(PFStreamGroup *sg, LongAddr base_addr)
{
    sg->cache_dirty_evict(base_addr);
}

void
pfsg_stop_thread_pf(PFStreamGroup *sg, int master_id)
{
    sg->stop_thread_pf(master_id);
}

void
pfsg_pf_fill(PFStreamGroup *sg, LongAddr base_addr,
                   int eff_access_type, i64 ready_time, int filled_to_cache)
{
    sg->pf_fill(base_addr, CacheAccessType(eff_access_type), ready_time,
                filled_to_cache);
}

void
pfsg_pf_merged(PFStreamGroup *sg, LongAddr base_addr, int pf_came_first)
{
    sg->pf_merged(base_addr, pf_came_first);
}

void
pfsg_print_stats(const PFStreamGroup *sg, void *c_FILE_out,
                 const char *prefix)
{
    sg->print_stats(static_cast<FILE *>(c_FILE_out), prefix);
}

void
pfsg_dump(const PFStreamGroup *sg, void *c_FILE_out, const char *prefix)
{
    sg->dump(static_cast<FILE *>(c_FILE_out), prefix);
}


PFStreamExported *
pfsg_export(const PFStreamGroup *sg, int master_id)
{
    return sg->gen_export(master_id);
}

void
pfsg_import(PFStreamGroup *sg, const PFStreamExported *pf_exp,
            int prefer_imported_streams, int imports_win_ties)
{
    sg->import(*pf_exp, prefer_imported_streams, imports_win_ties);
}

PFStreamExported *
pfse_copy(const PFStreamExported *pf_exp)
{
    return new PFStreamExported(*pf_exp);
}

void
pfse_destroy(PFStreamExported *pf_exp)
{
    delete pf_exp;
}

void
pfse_dump(const PFStreamExported *pf_exp, void *c_FILE_out,
          const char *prefix)
{
    pf_exp->dump(static_cast<FILE *>(c_FILE_out), prefix);
}

int
pfse_empty(const PFStreamExported *pf_exp)
{
    return pf_exp->empty();
}

long
pfse_estimate_size_bits(const PFStreamExported *pf_exp)
{
    return pf_exp->estimate_size_bits();
}

