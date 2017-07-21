//
// Trace Cache (excluding fill unit)
//
// Jeff Brown
// $Id: trace-cache.cc,v 1.2.2.5.2.1.2.1.6.1 2009/12/25 06:31:52 jbrown Exp $
//

const char RCSid_1053739677[] =
"$Id: trace-cache.cc,v 1.2.2.5.2.1.2.1.6.1 2009/12/25 06:31:52 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "assoc-array.h"
#include "trace-cache.h"
#include "utils.h"

#include <vector>

using std::vector;


#if defined(DEBUG)
#   define TCACHE_DEBUG         (debug)
#else
#   define TCACHE_DEBUG         0
#endif
#define TDPRINTF                if (!TCACHE_DEBUG) { } else printf


static inline void 
block_assign(TraceCacheBlock& dst, const TraceCacheBlock& src)
{
    dst.thread_id = src.thread_id;
    dst.inst_count = src.inst_count;
    for (int i = 0; i < src.inst_count; i++)
        dst.insts[i] = src.insts[i];
    dst.pred_count = src.pred_count;
    dst.predict_bits = src.predict_bits;
    dst.fallthrough_pc = src.fallthrough_pc;
}


struct TCEntry {
    TraceCacheBlock block;
    
    TCEntry() { block.insts = 0; }

    ~TCEntry() {
        if (block.insts)
            delete[] block.insts;
    }

    // Crufty, but saves us a level of indirection in accessing entries
    void alloc_insts(int insts_per_block) {
        sim_assert(insts_per_block > 0);
        sim_assert(!block.insts);
        block.insts = new TraceCacheInst[insts_per_block];
    }

    void reset() {
        tcb_reset(&block);
    }
};


struct TraceCache {
    typedef vector<TCEntry> TCEntryVec;
    
private:
    TraceCacheParams params;
    long n_lines;
    int inst_bytes_lg;

    AssocArray *cam;
    TCEntryVec entries;
    TraceCacheStats stats;

    void calc_lookup_key(AssocArrayKey& key_ret,
                         const TraceCacheLookupInfo& lookup_info,
                         bool include_predict_bits) const {
        u32 pred_bits;
        int pred_shift = 8 * sizeof(pred_bits) - params.pred_per_block;
        sim_assert((pred_shift >= 0) && 
               (pred_shift < static_cast<int>(8 * sizeof(pred_bits))));

        key_ret.lookup = lookup_info.pc >> inst_bytes_lg;

        // Make sure that the thread ID and prediction bits that we're
        // hackishly mixing together don't overlap, as best we can.
        // (There's currently no pre-defined limit on the thread id)
        sim_assert(lookup_info.thread_id >= 0);
        key_ret.match = static_cast<u32>(lookup_info.thread_id);
        sim_assert(key_ret.match < (static_cast<u32>(1) << pred_shift));

        if (include_predict_bits) {
            // Scoot the predict bits to the far left, and OR them into the
            // "match" field
            pred_bits = lookup_info.predict_bits;
            sim_assert((pred_bits >> params.pred_per_block) == 0);
            pred_bits <<= pred_shift;
            sim_assert((key_ret.match & pred_bits) == 0);
            key_ret.match |= pred_bits;
        }
    }

    // Really nasty hack to brute-force probe the cache to determine the 
    // longest matching path; works like calc_lookup_key().
    void path_assoc_hack(AssocArrayKey& key_ret,
                         const TraceCacheLookupInfo& lookup_info) const {
        TraceCacheLookupInfo ugly_hack = lookup_info;
        int longest_match = 0;
        AssocArrayKey lm_key;
        ugly_hack.predict_bits = 0;
        calc_lookup_key(lm_key, ugly_hack, true);
        for (int len = 1; len < params.pred_per_block; len++) {
            ugly_hack.predict_bits = GET_BITS_32(lookup_info.predict_bits, 0,
                                                 len);
            calc_lookup_key(key_ret, ugly_hack, true);
            long line_num;
            int way_num;
            if (aarray_probe(cam, &key_ret, &line_num, &way_num)) {
                long entry_num = line_num * params.assoc + way_num;
                const TCEntry& ent = entries[entry_num];
                if (ent.block.pred_count == len) {
                    longest_match = len;
                    lm_key = key_ret;
                }
            }
        }
        key_ret = lm_key;
    }

    static int find_pred_idx(const TraceCacheBlock& blk,
                             int pred_num) {
        int pred_left = pred_num;
        for (int i = 0; i < blk.inst_count; i++) {
            const TraceCacheInst& inst = blk.insts[i];
            if (TBF_UsesPredict(inst.br_flags)) {
                if (pred_left == 0)
                    return i;
                pred_left--;
            }
        }
        return -1;
    }
    
    int partial_match_trim(TraceCacheBlock& to_trim,
                           const TraceCacheLookupInfo& lookup_info) const {
        u32 mismatch_preds = GET_BITS_32(lookup_info.predict_bits, 0,
                                         to_trim.pred_count);
        mismatch_preds ^= to_trim.predict_bits;
        if (!mismatch_preds)
            return -1;
        int first_mismatch = 0;
        while (!(mismatch_preds & 1)) {
            first_mismatch++;
            mismatch_preds >>= 1;
        }
        int mismatch_inst = find_pred_idx(to_trim, first_mismatch);
        sim_assert((mismatch_inst >= 0) &&
                   (mismatch_inst < to_trim.inst_count));
        if (mismatch_inst == (to_trim.inst_count - 1))
            return -1;
        if (params.trim_partial_hits) {
            to_trim.inst_count = mismatch_inst + 1;
            to_trim.pred_count = first_mismatch + 1;
            to_trim.predict_bits = GET_BITS_32(to_trim.predict_bits, 0,
                                               to_trim.pred_count);
            to_trim.fallthrough_pc = to_trim.insts[to_trim.inst_count - 1].pc
                + 4;
        }
        return first_mismatch;
    }

    long do_lookup(const TraceCacheLookupInfo& lookup_info) const {
        AssocArrayKey lookup_key;
        long line_num;
        int is_hit, way_num;
        if (params.is_path_assoc)
            path_assoc_hack(lookup_key, lookup_info);
        else 
            calc_lookup_key(lookup_key, lookup_info, false);
        is_hit = aarray_lookup(cam, &lookup_key, &line_num, &way_num);
        return (is_hit) ? (line_num * params.assoc + way_num) : -1;
    }

    long do_replace(const TraceCacheLookupInfo& lookup_info) {
        AssocArrayKey lookup_key;
        long line_num;
        int way_num;
        calc_lookup_key(lookup_key, lookup_info, params.is_path_assoc);
        // Fill unit may generate a block which is already in the tcache, 
        // but aarray_replace() requires the block be new.  (This is currently
        // the case since we don't have a multi-branch predictor yet.)
        if (aarray_probe(cam, &lookup_key, &line_num, &way_num)) 
            aarray_touch(cam, line_num, way_num);
        else {
            stats.fills++;
            if (aarray_replace(cam, &lookup_key, &line_num, &way_num, NULL))
                stats.evicts++;
        }
        return line_num * params.assoc + way_num;
    }

    long do_inval(const TraceCacheLookupInfo& lookup_info) {
        AssocArrayKey lookup_key;
        long line_num;
        int is_present, way_num;
        if (params.is_path_assoc)
            path_assoc_hack(lookup_key, lookup_info);
        else 
            calc_lookup_key(lookup_key, lookup_info, false);
        is_present = aarray_lookup(cam, &lookup_key, &line_num, &way_num);
        if (is_present) {
            aarray_invalidate(cam, line_num, way_num);
            stats.evicts++;
        }
        return (is_present) ? (line_num * params.assoc + way_num) : -1;
    }

private:
    // Disallow copy or assignment
    TraceCache(const TraceCache& src);
    TraceCache& operator = (const TraceCache &src);

public:
    TraceCache(const TraceCacheParams& params_);
    ~TraceCache();

    void reset();
    void reset_stats();

    void get_stats(TraceCacheStats *stats_ret) const { *stats_ret = stats; }
    void get_params(TraceCacheParams *params_ret) const 
    { *params_ret = params; }

    inline int lookup(const TraceCacheLookupInfo& lookup_info,
                      TraceCacheBlock *block_ret) {
        long entry_num = do_lookup(lookup_info);
        TDPRINTF("TC lookup: %s -> ", tcli_format(&lookup_info));
        if (entry_num >= 0) {
            stats.trace_hits++;
            const TCEntry& ent = entries[entry_num];
            stats.hit_insts += ent.block.inst_count;
            stats.hit_preds += ent.block.pred_count;
            block_assign(*block_ret, ent.block);
            TDPRINTF("hit");
            {
                int trim_idx = partial_match_trim(*block_ret, lookup_info);
                if (trim_idx >= 0) {
                    TDPRINTF(" (partial,%i)", trim_idx);
                    stats.partial_hits++;
                }
            }
            TDPRINTF(": %s\n", tcb_format(block_ret));
        } else {
            stats.trace_misses++;
            TDPRINTF("miss\n");
        }
        return entry_num >= 0;
    }

    inline int probe(const TraceCacheLookupInfo& lookup_info,
                     TraceCacheBlock *block_ret) const {
        long entry_num = do_lookup(lookup_info);
        if (entry_num >= 0) {
            const TCEntry& ent = entries[entry_num];
            block_assign(*block_ret, ent.block);
        }
        return entry_num >= 0;
    }

    inline void fill(const TraceCacheBlock& new_block) {
        TraceCacheLookupInfo li;
        sim_assert(new_block.thread_id != -1);
        sim_assert(new_block.inst_count > 0);
        li.pc = new_block.insts[0].pc;
        li.thread_id = new_block.thread_id;
        li.predict_bits = new_block.predict_bits;

        long entry_num = do_replace(li);
        TCEntry& ent = entries[entry_num];
        ent.reset();
        block_assign(ent.block, new_block);
        TDPRINTF("TC fill: %s\n", tcb_format(&ent.block));
    }

    inline void inval(const TraceCacheLookupInfo& lookup_info) {
        long entry_num = do_inval(lookup_info);
        TDPRINTF("TC inval: %s -> ", tcli_format(&lookup_info));
        if (entry_num >= 0) {
            TCEntry& ent = entries[entry_num];
            TDPRINTF("present\n");
            ent.reset();
        } else {
            TDPRINTF("not present\n");
        }
    }

    int get_block_insts() const { return params.block_insts; }
};


TraceCache::TraceCache(const TraceCacheParams& params_)
    : cam(0)
{
    const char *func = "TraceCache::TraceCache";
    params = params_;

    sim_assert(params.n_entries > 0);
    sim_assert(params.assoc > 0);
    sim_assert(params.block_insts > 0);
    sim_assert(params.pred_per_block >= 0);
    sim_assert(params.inst_bytes > 0);

    if (params.n_entries % params.assoc != 0) {
        fprintf(stderr, "%s (%s:%i): assoc (%i) doesn't divide "
                "n_entries (%li)\n", func, __FILE__, __LINE__, 
                params.assoc, params.n_entries);
        exit(1);
    }
    n_lines = params.n_entries / params.assoc;

    int log_inexact;
    inst_bytes_lg = floor_log2(params.inst_bytes, &log_inexact);
    if (log_inexact) {
        fprintf(stderr, "%s (%s:%i): inst_bytes (%i) not a power of 2\n",
                func, __FILE__, __LINE__, params.inst_bytes);
        exit(1);
    }

    cam = aarray_create(n_lines, params.assoc, "LRU");
    entries.resize(params.n_entries);
    for (long i = 0; i < params.n_entries; i++)
        entries[i].alloc_insts(params.block_insts);

    reset();
}


TraceCache::~TraceCache()
{
    if (cam)
        aarray_destroy(cam);
}


void
TraceCache::reset() 
{
    aarray_reset(cam);
    {
        TCEntryVec::iterator iter = entries.begin(), end = entries.end();
        for (; iter != end; ++iter) {
            TCEntry& ent = *iter;
            ent.reset();
        }
    }

    reset_stats();
}


void
TraceCache::reset_stats() 
{
    stats.trace_hits = 0;
    stats.trace_misses = 0;
    stats.fills = 0;
    stats.evicts = 0;
    stats.hit_insts = 0;
    stats.hit_preds = 0;
    stats.partial_hits = 0;
}


//
// C interface to TraceCache
//

TraceCache *
tc_create(const TraceCacheParams *params)
{
    return new TraceCache(*params);
}

void 
tc_destroy(TraceCache *tc)
{
    if (tc)
        delete tc;
}

void 
tc_reset(TraceCache *tc)
{
    tc->reset();
}

void 
tc_reset_stats(TraceCache *tc)
{
    tc->reset_stats();
}

int 
tc_lookup(TraceCache *tc, const TraceCacheLookupInfo *lookup_info,
          TraceCacheBlock *block_ret)
{
    return tc->lookup(*lookup_info, block_ret);
}

int
tc_probe(const TraceCache *tc, const TraceCacheLookupInfo *lookup_info,
         TraceCacheBlock *block_ret)
{
    return tc->probe(*lookup_info, block_ret);
}

void 
tc_fill(TraceCache *tc, const TraceCacheBlock *new_block) {
    tc->fill(*new_block);
}

void 
tc_inval(TraceCache *tc, const TraceCacheLookupInfo *lookup_info)
{
    tc->inval(*lookup_info);
}

void 
tc_get_stats(const TraceCache *tc, TraceCacheStats *stats_ret) 
{
    tc->get_stats(stats_ret);
}

void
tc_get_params(const TraceCache *tc, TraceCacheParams *params_ret)
{
    tc->get_params(params_ret);
}


//
// Trace cache block utility code
//

TraceCacheBlock *
tcb_alloc(const TraceCache *tc)
{
    int block_insts = tc->get_block_insts();
    TraceCacheBlock *blk = new TraceCacheBlock;
    blk->insts = new TraceCacheInst[block_insts];
    tcb_reset(blk);
    return blk;
}


TraceCacheBlock *
tcb_copy(const TraceCache *tc, const TraceCacheBlock *tcb)
{
    int block_insts = tc->get_block_insts();
    TraceCacheBlock *blk = new TraceCacheBlock;
    blk->insts = new TraceCacheInst[block_insts];
    tcb_assign(blk, tcb);
    return blk;
}


void 
tcb_assign(TraceCacheBlock *dst, const TraceCacheBlock *src)
{
    block_assign(*dst, *src);
}


void 
tcb_free(TraceCacheBlock *tcb)
{
    if (tcb) {
        if (tcb->insts)
            delete[] tcb->insts;
        delete tcb;
    }
}


void 
tcb_reset(TraceCacheBlock *tcb)
{
    tcb->thread_id = -1;
    tcb->inst_count = 0;
    tcb->pred_count = 0;
    tcb->predict_bits = 0;
    tcb->fallthrough_pc = 0;
}


int 
tcb_compare(const TraceCacheBlock *tcb1, const TraceCacheBlock *tcb2)
{
    int result = 0;

    if ((result = CMP_SCALAR(tcb1->thread_id, tcb2->thread_id)))
        goto done;

    // We'll compare the sequence of instructions one by one; PCs, target
    // info, and fallthrough PC combined all imply the inst_count, pred_count,
    // and predict_bits field.

    for (int i = 0; (i < tcb1->inst_count) && (i < tcb2->inst_count); i++) {
        const TraceCacheInst *inst1 = &tcb1->insts[i];
        const TraceCacheInst *inst2 = &tcb2->insts[i];
        if ((result = CMP_SCALAR(inst1->pc, inst2->pc)))
            goto done;
        if ((result = CMP_SCALAR(inst1->target_pc, inst2->target_pc)))
            goto done;
        if ((result = CMP_SCALAR(inst1->br_flags, inst2->br_flags)))
            goto done;
        if ((result = CMP_SCALAR(inst1->cgroup_flags, inst2->cgroup_flags)))
            goto done;
    }

    result = CMP_SCALAR(tcb1->fallthrough_pc, tcb2->fallthrough_pc);

done:
    return result;
}


const char *
tcinst_format(const TraceCacheInst *tci)
{
    static char buf[80];
    int limit = sizeof(buf);
    int used = 0;

    #define TCINST_FMT(fmt, arg) \
        used += e_snprintf(buf + used, limit - used, fmt, arg);

    TCINST_FMT(" %s", fmt_x64(tci->pc));
    if (tci->target_pc)
        TCINST_FMT("->%s", fmt_x64(tci->target_pc));
    if (tci->br_flags) {
        TCINST_FMT(" (%sT", (tci->br_flags & TBF_Br_Taken) ? "" : "N");
        if (tci->br_flags & TBF_SkipPredict)
            TCINST_FMT("%s", ",skip");
        if (tci->br_flags & TBF_MultiTarg)
            TCINST_FMT("%s", ",mult");
        TCINST_FMT("%s", ")");
    }
    if (tci->cgroup_flags) {
        TCINST_FMT("%s", " (cg:");
        if (tci->cgroup_flags & TCGF_StartsGroup)
            TCINST_FMT("%s", "S");
        if (tci->cgroup_flags & TCGF_EndsGroup)
            TCINST_FMT("%s", "E");
        TCINST_FMT("%s", ")");
    }

    #undef TCINST_FMT
    return buf;
}


const char *
tcb_format(const TraceCacheBlock *tcb)
{
    static char buf[1024];
    int limit = sizeof(buf);
    int used = 0;

    // I wish we had variadic macros in C++.  Or closures!
    #define TCB_FMT(fmt, arg) \
        used += e_snprintf(buf + used, limit - used, fmt, arg);

    TCB_FMT("id %i {", tcb->thread_id);

    for (int i = 0 ; i < tcb->inst_count; i++) {
        const TraceCacheInst *tci = &tcb->insts[i];
        TCB_FMT("%s", tcinst_format(tci));
    }
    TCB_FMT(" } pred_count %i", tcb->pred_count);
    TCB_FMT(" pred_bits 0x%s", fmt_x64(tcb->predict_bits));
    TCB_FMT(" fallthrough %s", fmt_x64(tcb->fallthrough_pc));

    #undef TCB_FMT
    return buf;
}


const char *
tcli_format(const TraceCacheLookupInfo *tcli)
{
    static char buf[80];
    e_snprintf(buf, sizeof(buf), "T%d/%s/%s", tcli->thread_id,
               fmt_x64(tcli->pc), fmt_x64(tcli->predict_bits));
    return buf;
}


void 
tcli_from_tcb(TraceCacheLookupInfo *dest, const TraceCacheBlock *tcb)
{
    dest->pc = tcb->insts[0].pc;
    dest->thread_id = tcb->thread_id;
    dest->predict_bits = tcb->predict_bits;
}


void 
tc_branch_mask(u32 *predict_bits, int pred_count)
{
    *predict_bits = GET_BITS_32(*predict_bits, 0, pred_count);
}
