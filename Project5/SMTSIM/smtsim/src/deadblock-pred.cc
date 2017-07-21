//
// Cache dead-block predictor (and perhaps prefetcher)
//
// Jeff Brown
// $Id: deadblock-pred.cc,v 1.1.2.8 2009/07/29 19:24:46 jbrown Exp $
//

const char RCSid_1226053727[] = 
"$Id: deadblock-pred.cc,v 1.1.2.8 2009/07/29 19:24:46 jbrown Exp $";

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "hash-map.h"
#include "deadblock-pred.h"
#include "utils.h"
#include "utils-cc.h"
#include "sim-cfg.h"
#include "assoc-array.h"
#include "online-stats.h"
#include "main.h"               // for fmt_now()
//#include "core-resources.h"


using namespace SimCfg;
using std::map;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;


// DBP_DEBUG_LEVEL:
// 0: off
// 1: top-level ops
// 2: history signature / dead-block table ops
#define DBP_DEBUG_LEVEL        2

#ifdef DEBUG
    // Condition: "should I printf a level-x debug message?"
    #define DBP_DEBUG_COND(x) ((DBP_DEBUG_LEVEL >= (x)) && debug)
#else
    // Never print
    #define DBP_DEBUG_COND(x) (0)
#endif
#define DBP_DB(x) if (!DBP_DEBUG_COND(x)) { } else printf


// (add any exported enum name-arrays here)


namespace {


struct DeadBlockConf {
    int block_bytes_lg;         // not from conf file

    int hist_width;
    bool hist_update_at_commit;

    int dead_block_entries;
    int dead_block_assoc;

    NoDefaultCopy nocopy;

    DeadBlockConf(const string& cfg_path);
    ~DeadBlockConf() { }
};


DeadBlockConf::DeadBlockConf(const string& cfg_path)
{
    const string& cp = cfg_path;        // short-hand

    hist_width = conf_int(cp + "/hist_width");
    if (hist_width < 0) {
        exit_printf("bad hist_width (%d)\n", hist_width);
    }
    hist_update_at_commit = conf_bool(cp + "/hist_update_at_commit");

    dead_block_entries = conf_int(cp + "/dead_block_entries");
    if (dead_block_entries < 1) {
        exit_printf("bad dead_block_entries (%d)\n", dead_block_entries);
    }
    dead_block_assoc = conf_int(cp + "/dead_block_assoc");
    if (dead_block_assoc < 1) {
        exit_printf("bad dead_block_assoc (%d)\n", dead_block_assoc);
    }
    if (dead_block_entries % dead_block_assoc) {
        exit_printf("dead_block_assoc (%d) doesn't divide "
                    "dead_block_entries (%d)\n", dead_block_assoc,
                    dead_block_entries);
    }
}   


class BlockHistory {
    int width_;                 // (wasteful to store with each block)
    u64 sum_;                   // current signature bits

    bool dead_flag_;            // marked as predicted dead
    // WARNING: uses default copy/assignment 

public:
    BlockHistory() : width_(-1), sum_(0) { }
    BlockHistory(int width) : width_(width), sum_(0) {
        sim_assert(width_ >= 0);
        sim_assert(width < int(8*sizeof(sum_)));
    }

    void append_pc(mem_addr pc) {
        // Lai et al. didn't actually say what they meant by "truncated
        // addition", so I'll do the time-honored thing and just make
        // something up: we'll just add the significant bits of all PCs
        // in the trace, and then truncate the final result to the given
        // bit width.
        sum_ += (pc >> 2);
    }

    u64 get_sig() const {
        sim_assert(width_ >= 0);
        u64 result = GET_BITS_64(sum_, 0, width_);
        return result;
    }

    bool get_dead_flag() const { return dead_flag_; }
    void set_dead_flag(const bool& val) { dead_flag_ = val; }

    string fmt() const {
        ostringstream ostr;
        ostr << "sig=" << fmt_x64(get_sig())
             << ",d=" << fmt_bool(dead_flag_);
        return ostr.str();
    }
};


// HistoryTable has one entry per resident block, and accumulates signatures
// of memory instructions seen for each block
class HistoryTable {
#if HAVE_HASHMAP
    typedef hash_map<LongAddr, BlockHistory,
                     StlHashMethod<LongAddr> > BlockHistMap;
#else
    typedef map<LongAddr, BlockHistory> BlockHistMap;
#endif

    const DeadBlockConf *conf_;
    int peer_cache_blocks_;             // size in blocks of peer cache
    BlockHistMap va_to_hist_;
    NoDefaultCopy nocopy_;

    bool invariant() const {
        return int(va_to_hist_.size()) <= peer_cache_blocks_;
    }

public:
    HistoryTable(const DeadBlockConf *conf, int peer_cache_blocks)
        : conf_(conf), peer_cache_blocks_(peer_cache_blocks) {
        sim_assert(peer_cache_blocks_ > 0);
    }

    void reset() {
        va_to_hist_.clear();
    }

    // returns post-update history entry, or NULL
    // optionally, on a hit, writes a copy of the pre-update history
    BlockHistory *mem_access(mem_addr pc, const LongAddr& base_va,
                             BlockHistory *previous_hist_ret) {
        BlockHistory *ent = map_find(va_to_hist_, base_va);
        if (ent) {
            if (previous_hist_ret) {
                *previous_hist_ret = *ent;
            }
            ent->append_pc(pc);
        } else {
            // ignore missing entries (possible if cache block was evicted
            // before this inst made it to commit).  this doesn't cleanly
            // catch weird evict/refetch/commit interleavings.
        }
        return ent;
    }

    void block_insert(const LongAddr& base_va) {
        BlockHistory *ent = map_insert_uniq(va_to_hist_, base_va,
                                            BlockHistory(conf_->hist_width));
        if (!ent) {
            BlockHistory *dupe = map_find(va_to_hist_, base_va);
            abort_printf("HistoryTable: block_insert(%s) at %s: already "
                         "present, existing ent: %s\n", fmt_laddr(base_va),
                         fmt_now(), dupe->fmt().c_str());
        }
        sim_assert(invariant());
    }

    void block_kill(const LongAddr& base_va, BlockHistory *block_hist_ret) {
        BlockHistory *ent = map_find(va_to_hist_, base_va);
        if (!ent) {
            abort_printf("HistoryTable: block_kill(%s) at %s: block not "
                         "present\n", fmt_laddr(base_va), fmt_now());
        }
        *block_hist_ret = *ent;
        va_to_hist_.erase(base_va);
    }

    bool is_predicted_dead(const LongAddr& base_va) const {
        const BlockHistory *ent = map_find(va_to_hist_, base_va);
        return ent && ent->get_dead_flag();
    }

    void dump(FILE *out, const char *pf) const {
        FOR_CONST_ITER(BlockHistMap, va_to_hist_, iter) {
            fprintf(out, "%s%s -> %s\n", pf, iter->first.fmt().c_str(),
                    iter->second.fmt().c_str());
        }
    }
};


// An entry in the dead-block table.  Currently just used for liveness
// prediction, it could be extended to do next-block-to-fetch correlation.
class DeadBlockEnt {
    static const int SatLimit = 4;      // two-bit counter
    static const int InitialValue = (SatLimit / 2) - 1;
    static const int YesThresh = SatLimit / 2;
    int sat_count_;
public:
    DeadBlockEnt() { reset(); }
    void reset() { sat_count_ = InitialValue; }
    void incr() { sat_count_ = INCR_SAT(sat_count_, SatLimit); }
    void decr() { sat_count_ = DECR_SAT(sat_count_); }
    int get_sat_count() const { return sat_count_; }
    bool predict_dead() const {
        return sat_count_ >= YesThresh;
    }
    string fmt() const {
        ostringstream ostr;
        ostr << "cnt=" << sat_count_;
        return ostr.str();
    }
};


class DeadBlockTable {
    const DeadBlockConf *conf_;

    long dead_block_lines_;
    int dead_block_assoc_;
    typedef vector<DeadBlockEnt> EntryVec;
    AssocArray *cam_;
    EntryVec entries_;

    void gen_aa_key(AssocArrayKey& key, const LongAddr& base_va,
                    const BlockHistory& block_hist) const {
        key.lookup = block_hist.get_sig() ^
            (base_va.a >> conf_->block_bytes_lg);
        key.match = base_va.id;
    }

    DeadBlockEnt& ent_idx(long line_num, int way_num) {
        return vec_idx(entries_, line_num * dead_block_assoc_ + way_num);
    }
    const DeadBlockEnt& ent_idx(long line_num, int way_num) const {
        return vec_idx(entries_, line_num * dead_block_assoc_ + way_num);
    }

public:
    DeadBlockTable(const DeadBlockConf *conf)
        : conf_(conf), cam_(0) {
        const char *fname = "DeadBlockTable";
        sim_assert(conf_->dead_block_entries > 0);
        sim_assert(conf_->dead_block_assoc > 0);
        sim_assert((conf_->dead_block_entries % conf_->dead_block_assoc) == 0);
        dead_block_lines_ = conf_->dead_block_entries /
            conf_->dead_block_assoc;
        dead_block_assoc_ = conf_->dead_block_assoc;
        if (!(cam_ = aarray_create(dead_block_lines_, dead_block_assoc_, 
                                   "LRU"))) {
            exit_printf("%s: couldn't create DeadBlockTable AssocArray\n",
                        fname);
        }
        for (int i = 0; i < conf_->dead_block_entries; ++i) {
            entries_.push_back(DeadBlockEnt());
        }
    }
    ~DeadBlockTable() {
        aarray_destroy(cam_);
    }
    void reset() {
        aarray_reset(cam_);
        FOR_ITER(EntryVec, entries_, iter) { iter->reset(); }
    }

    void block_kill(const LongAddr& base_va, const BlockHistory& final_hist) {
        AssocArrayKey key;
        long line_num; int way_num;
        gen_aa_key(key, base_va, final_hist);
        DeadBlockEnt *ent;
        if (aarray_lookup(cam_, &key, &line_num, &way_num)) {
            ent = &ent_idx(line_num, way_num);
            ent->incr();
            DBP_DB(2)(" dbt:incr,cnt=%d", ent->get_sat_count());
        } else {
            aarray_replace(cam_, &key, &line_num, &way_num, NULL);
            ent = &ent_idx(line_num, way_num);
            ent->reset();
            DBP_DB(2)(" dbt:reset,cnt=%d", ent->get_sat_count());
        }
    }

    // notify: this block was predicted dead, and subsequently referenced
    // (we only consider false positives to be mispredicts; false negatives
    // are taken as a learning opportunity)
    void mispredict(const LongAddr& base_va, const BlockHistory& hist) {
        AssocArrayKey key;
        long line_num; int way_num;
        gen_aa_key(key, base_va, hist);
        if (aarray_lookup(cam_, &key, &line_num, &way_num)) {
            DeadBlockEnt& ent = ent_idx(line_num, way_num);
            ent.decr();
            DBP_DB(2)(" dbt:decr,cnt=%d", ent.get_sat_count());
        }
    }

    bool predict_dead(const LongAddr& base_va,
                      const BlockHistory& hist) const {
        bool result = false;
        AssocArrayKey key;
        long line_num; int way_num;
        gen_aa_key(key, base_va, hist);
        if (aarray_probe(cam_, &key, &line_num, &way_num)) {
            const DeadBlockEnt& ent = ent_idx(line_num, way_num);
            result = ent.predict_dead();
        }
        return result;
    }

    void dump(FILE *out, const char *pf) const {
        for (long line = 0; line < dead_block_lines_; ++line) {
            for (int way = 0; way < dead_block_assoc_; ++way) {
                AssocArrayKey key;
                if (aarray_readkey(cam_, line, way, &key)) {
                    const DeadBlockEnt& ent = ent_idx(line, way);
                    fprintf(out, "%s(%s,%s) -> %s\n", pf,
                            fmt_u64(key.lookup), fmt_u64(key.match),
                            ent.fmt().c_str());
                }
            }
        }
    }
};
    

struct DBPStats {
    i64 execs;                  // #execs seen
    i64 commits;                // #commits seen

    i64 accesses;               // #memory operations submitted to history
    i64 accesses_absent;        // "accesses" for absent memory blocks
    i64 marked_dead;            // "accesses" leading to predict-dead
    i64 dead_false_pos;         // false positives (subset of "marked_dead")
    i64 dead_false_neg;         // false negatives (subset of "kills")
    i64 inserts;                // total blocks inserted to peer cache
    i64 kills;                  // total blocks removed from peer cache

    DBPStats() { reset(); }
    void reset() {
        execs = 0;
        commits = 0;
        accesses = 0;
        accesses_absent = 0;
        marked_dead = 0;
        dead_false_pos = 0;
        dead_false_neg = 0;
        inserts = 0;
        kills = 0;
    }
};


} // Anonymous namespace close



struct DeadBlockPred {
private:
    // (trying the google-style "member vars have trailing _")
    string id_;
    string cfg_path_;
    DeadBlockConf conf_;
    CoreResources *parent_core_;
    int peer_cache_blocks_;
    int block_bytes_;
    int block_bytes_lg_;

    scoped_ptr<HistoryTable> hist_tab_;
    scoped_ptr<DeadBlockTable> dead_block_tab_;
    DBPStats stats_;
    NoDefaultCopy nocopy_;

    void align_addr(LongAddr& addr) const {
        addr.a &= ~(block_bytes_ - 1);   // works for power-of-two block sizes
    }

    // common routine to update the tables in response to a memory access,
    // either at exec or commit of a memory up (but not both)
    void mem_access(mem_addr pc, const LongAddr& va) {
        LongAddr base_va(va);
        align_addr(base_va);
        // note: DBP_DB(1) line started by parent
        DBP_DB(1)(" base_va %s", fmt_laddr(base_va));
        BlockHistory previous_hist;
        BlockHistory *updated_hist =
            hist_tab_->mem_access(pc, base_va, &previous_hist);
        // "updated_hist" if non-null, is a live pointer to the current entry
        // for this block.  It has the trace signature _after_ this memory
        // operation, and the "was predicted dead?" flag from the most recent
        // prediction.
        stats_.accesses++;
        if (updated_hist) {
            // (cruft: HistoryTable/BlockHistory layering violation)
            DBP_DB(2)(" hist {%s} -> {%s}", previous_hist.fmt().c_str(),
                      updated_hist->fmt().c_str());
            if (updated_hist->get_dead_flag()) {
                // we've just seen a memory op for a resident block that was
                // predicted dead; let the dead block table know it was wrong.
                // (we need the pre-update history here, since the predicton
                // came before this operation)
                DBP_DB(1)(" (false-pos; marking live)");
                stats_.dead_false_pos++;
                dead_block_tab_->mispredict(base_va, previous_hist);
                updated_hist->set_dead_flag(false);
            }
            if (dead_block_tab_->predict_dead(base_va, *updated_hist)) {
                DBP_DB(1)(" (marking dead)");
                stats_.marked_dead++;
                updated_hist->set_dead_flag(true);
            }
        } else {
            stats_.accesses_absent++;
            DBP_DB(1)(" (block missing)");
        }
        DBP_DB(1)("\n");
    }

public:
    DeadBlockPred(const string& id, const string& cfg_path,
                  CoreResources *parent_core,
                  int peer_cache_blocks,
                  int block_bytes);
    ~DeadBlockPred();
    void reset();
    void print_stats(FILE *out, const char *pf) const;
    void dump(FILE *out, const char *pf) const;

    void mem_exec(mem_addr pc, const LongAddr& va) {
        DBP_DB(1)("dbp: exec %s: time %s pc %s;",
                  id_.c_str(), fmt_now(), fmt_x64(pc));
        stats_.execs++;
        if (!conf_.hist_update_at_commit) {
            mem_access(pc, va);
        }
    }

    void mem_commit(mem_addr pc, const LongAddr& va) {
        DBP_DB(1)("dbp: commit %s: time %s pc %s;",
                  id_.c_str(), fmt_now(), fmt_x64(pc));
        stats_.commits++;
        if (conf_.hist_update_at_commit) {
            mem_access(pc, va);
        }
    }

    void block_insert(const LongAddr& base_va) {
        DBP_DB(1)("dbp: insert %s: time %s base_va %s\n",
                  id_.c_str(), fmt_now(), fmt_laddr(base_va));
        stats_.inserts++;
        hist_tab_->block_insert(base_va);
    }

    void block_kill(const LongAddr& base_va) {
        DBP_DB(1)("dbp: kill %s: time %s base_va %s;",
                  id_.c_str(), fmt_now(), fmt_laddr(base_va));
        stats_.kills++;
        BlockHistory final_hist;
        hist_tab_->block_kill(base_va, &final_hist);
        DBP_DB(2)(" hist {%s}", final_hist.fmt().c_str());
        if (!final_hist.get_dead_flag()) {
            // evicting/killing a block not marked dead
            DBP_DB(1)(" (false-neg)");
            stats_.dead_false_neg++;
        }
        dead_block_tab_->block_kill(base_va, final_hist);
        DBP_DB(1)("\n");
    }

    bool predict_dead(const LongAddr& base_va) const {
        DBP_DB(1)("dbp: predict_dead %s: time %s base_va %s:",
                  id_.c_str(), fmt_now(), fmt_laddr(base_va));
        bool result = hist_tab_->is_predicted_dead(base_va);
        DBP_DB(1)(" %s\n", fmt_bool(result));
        return result;
    }
};


DeadBlockPred::DeadBlockPred(const string& id, const string& cfg_path,
                             CoreResources *parent_core,
                             int peer_cache_blocks, int block_bytes)
    : id_(id), cfg_path_(cfg_path), conf_(cfg_path), parent_core_(parent_core),
      peer_cache_blocks_(peer_cache_blocks), block_bytes_(block_bytes)
{
    if (block_bytes_ < 1) {
        exit_printf("bad DeadBlockPred block_bytes (%d)\n", block_bytes_);
    }
    if ((block_bytes_lg_ = log2_exact(block_bytes_)) < 0) {
        exit_printf("DeadBlockPred block_bytes (%d) not a power of 2\n",
                    block_bytes_);
    }
    conf_.block_bytes_lg = block_bytes_lg_;
    hist_tab_.reset(new HistoryTable(&conf_, peer_cache_blocks_));
    dead_block_tab_.reset(new DeadBlockTable(&conf_));
}


DeadBlockPred::~DeadBlockPred()
{
}


void
DeadBlockPred::reset()
{
    hist_tab_->reset();
    dead_block_tab_->reset();
    stats_.reset();
}


void
DeadBlockPred::print_stats(FILE *out, const char *pf) const
{
    // Each access leads (implicitly) to a prediction, a chance for that
    // access to be the final touch before the next eviction.  Note that
    // mistakes are not discovered right away; false positives are not
    // discovered until a future access, and false negatives are not
    // discovered until a subsequent eviction.
    //
    // "positive": access was in fact the last touch before evict
    // "negative": access was not the last touch before evict
    // broken down:
    //   "true positive": last-touch access that was marked dead
    //   "true negative": non-last-touch access that was not marked dead
    //   "false positive": non-last-touch access that was marked dead
    //   "false negative": last-touch access that was not marked dead
    //
    // ignoring delayed detection, some identities:
    //   kills = total_pos (by definition; kill defines last-touch)
    ///  accesses = total_pos + total_neg
    //   accesses = true_pos + false_pos + true_neg + false_neg
    //   marked_dead = true_pos + false_pos
    //   accesses - marked_dead = true_neg + false_neg
    //
    // total_pos = true_pos + false_neg
    // total_neg = true_neg + false_pos
    // false_pos_rate = false_pos / total_neg = "alpha"
    // false_neg_rate = false_neg / total_pos = "beta"
    // sensitivity = true_pos / (true_pos + false_neg) = true_pos / total_pos
    //             = "power"
    // specificity = true_neg / (true_neg + false_pos) = true_neg / total_neg
    // pos_pred_value = true_pos / (true_pos + false_pos)
    // neg_pred_value = true_neg / (true_neg + false_neg)
    //
    // false_pos_rate: fraction of negatives we screwed up
    // false_neg_rate: fraction of positives we screwed up
    // sensitivity: fraction of positives that we got right ("detected")
    // specificity: fraction of negatives that we got right ("ignored")
    // pos_pred_value: when we said "positive", fraction we got right
    // neg_pred_value: when we said "negative", fraction we got right

    fprintf(out, "%sexecs: %s commits: %s\n", pf, 
            fmt_i64(stats_.execs), fmt_i64(stats_.commits));
    fprintf(out, "%saccesses: %s absent: %s %.2f%%\n", pf, 
            fmt_i64(stats_.accesses), fmt_i64(stats_.accesses_absent),
            100.0 * double(stats_.accesses_absent) / stats_.accesses);
    fprintf(out, "%smarked_dead: %s (%s %.2f%% wrong)\n", pf, 
            fmt_i64(stats_.marked_dead), fmt_i64(stats_.dead_false_pos),
            100.0 * double(stats_.dead_false_pos) / stats_.marked_dead);
    fprintf(out, "%sinserts: %s kills: %s\n", pf, 
            fmt_i64(stats_.inserts), fmt_i64(stats_.kills));

    // we exclude marked_dead from the calculations, since it may 
    // include false-positive predictions we haven't discovered yet
    // (the actual outcome isn't known until an eviction or future access)
    i64 total_pos = stats_.kills;
    i64 total_neg = stats_.accesses - total_pos;
    i64 false_pos = stats_.dead_false_pos;
    i64 false_neg = stats_.dead_false_neg;
    i64 true_pos = total_pos - false_neg;
    i64 true_neg = total_neg - false_pos;
    // true_pos = stats_.marked_dead - false_pos;
    // true_neg = (stats_.accesses - stats_.marked_dead) - false_neg;
    {
        BiConfusionMatrix mat(true_pos, true_neg, false_pos, false_neg);
        fprintf(out, "%spredictor BiConfusionMatrix:\n", pf);
        string child_pre = string(pf) + "  ";
        fputs(mat.fmt_verbose(child_pre).c_str(), out);
    }
}


void
DeadBlockPred::dump(FILE *out, const char *pf) const
{
    fprintf(out, "%sid: %s cfg_path: %s\n", pf,
            id_.c_str(), cfg_path_.c_str());
    fprintf(out, "%shist table:\n", pf);
    hist_tab_->dump(stdout, (string(pf) + "  ").c_str());
    fprintf(out, "%sdead block table:\n", pf);
    dead_block_tab_->dump(stdout, (string(pf) + "  ").c_str());
}



//
// C interface
//

DeadBlockPred *
dbp_create(const char *id, const char *config_path,
           struct CoreResources *parent_core, int peer_cache_blocks,
           int block_bytes)
{
    return new DeadBlockPred(string(id), string(config_path), parent_core,
                             peer_cache_blocks, block_bytes);
}

void
dbp_destroy(DeadBlockPred *dbp)
{
    delete dbp;
}

void
dbp_mem_exec(DeadBlockPred *dbp, mem_addr pc, LongAddr va)
{
    dbp->mem_exec(pc, va);
}

void
dbp_mem_commit(DeadBlockPred *dbp, mem_addr pc, LongAddr va)
{
    dbp->mem_commit(pc, va);
}

void
dbp_block_insert(DeadBlockPred *dbp, LongAddr base_va)
{
    dbp->block_insert(base_va);
}

void
dbp_block_kill(DeadBlockPred *dbp, LongAddr base_va)
{
    dbp->block_kill(base_va);
}

int
dbp_predict_dead(const DeadBlockPred *dbp, LongAddr base_va)
{
    return dbp->predict_dead(base_va);
}

void
dbp_print_stats(const DeadBlockPred *dbp, void *c_FILE_out,
                const char *prefix)
{
    dbp->print_stats(static_cast<FILE *>(c_FILE_out), prefix);
}

void
dbp_dump(const DeadBlockPred *dbp, void *c_FILE_out, const char *prefix)
{
    dbp->dump(static_cast<FILE *>(c_FILE_out), prefix);
}
