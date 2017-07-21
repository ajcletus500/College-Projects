//
// Working-set migration stuff
//
// Jeff Brown
// $Id: wsm.cc,v 1.1.4.84 2009/11/26 01:09:49 jbrown Exp $
//

const char RCSid_1210808870[] = 
"$Id: wsm.cc,v 1.1.4.84 2009/11/26 01:09:49 jbrown Exp $";

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <iostream>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "hash-map.h"
#include "wsm.h"
#include "utils.h"
#include "utils-cc.h"
#include "sim-cfg.h"
#include "mem-profiler.h"
#include "app-state.h"
#include "sim-params.h"
#include "callback-queue.h"
#include "app-mgr.h"
#include "main.h"
#include "context.h"
#include "core-resources.h"
#include "cache.h"
#include "cache-array.h"
#include "prefetch-streambuf.h"
#include "deadblock-pred.h"
#include "online-stats.h"
#include "gzstream.h"
#include "mem-ref-seq.h"
#include "tlb-array.h"
#include "prefetch-audit.h"
#include "mshr.h"
#include "prog-mem.h"


using std::deque;
using std::istream;
using std::list;
using std::make_pair;
using std::map;
using std::pair;
using std::ostringstream;
using std::set;
using std::string;
using std::vector;

using namespace SimCfg;


// Plan for WSM_DEBUG_LEVEL:
// 0: off
// 1: top-level per-RegularMove epoch stuff, block counts at summarize, etc.
// 2: detailed per-epoch stuff: entry selection, prefetch generation, etc.
// 3: per-sample detail during simulation (not just at RegularMove)

#define WSM_DEBUG_LEVEL        3

#ifdef DEBUG
    // Condition: "should I printf a level-x debug message?"
    #define WSM_DEBUG_COND(x) ((WSM_DEBUG_LEVEL >= (x)) && debug)
#else
    // Never print
    #define WSM_DEBUG_COND(x) (0)
#endif
#define WSMDB(x) if (!WSM_DEBUG_COND(x)) { } else printf


namespace {

// Giant bag o' knobs used across many levels of the implementation here;
// we're being lazy, taking advantage of the fact that there can be only one
// set of WSM knobs in the simulator config.
struct WSM_Config;


// sketchy: global copy of one WSM_ThreadCapture's access serial number,
// lazily placed here so the various subroutines can see it.  (pretend it's
// dynamic scoping, ha ha)
i64 GlobalAccessSerialNum = -1;

// next unique ID for PrefetchPlayerCBs (to help debugging)
i64 GlobalPrefetchPlayerNextSerial = 0;

const int kInstSizeBytes = 4;

const bool kShowExtraHistoryForecastSpam = true;

// "FutureTracePF/set_select" options: how to construct set of future
// memory accesses from a stream of MemRefSeq_Record's.
// (all schemes implicitly stop early if |L1I+L1D+L2| unique blocks are seen)
enum FuturePrefetchSelect {
    FPS_InstCountWindow,        // next window_size app insts (includes NOPs)
    FPS_MemRefWindow,           // next window_size Records (all types)
    FPS_UniqueBlockWindow,      // next window_size unique blocks (all types)
    // Note: FPS_InstData[L2]Size use "window_size" as a percentage, use
    // window_size=100 to get the full indicated sizes.
    FPS_InstDataSize,           // |L1I| inst blocks + |L1D| data blocks
    FPS_InstDataL2Size,         // |L1I| inst blocks + |L1D + L2| data blocks
    FuturePrefetchSelect_last
};
const char *FuturePrefetchSelect_names[] = {
    "InstCountWindow",
    "MemRefWindow",
    "UniqueBlockWindow",
    "InstDataSize",
    "InstDataL2Size",
    NULL
};


// We have explicit enums for table IDs to help with printing stats
enum WSMTableID {
    WSMT_NextBlockInst,
    WSMT_NextBlockData,
    WSMT_NextBlockBoth,
    WSMT_StridePC,
    WSMT_SameObj,
    WSMT_Pointer,
    WSMT_PointerChase,
    WSMT_BTB,
    WSMT_BlockBTB,
    WSMT_PCWindow,
    WSMT_SPWindow,
    WSMT_RetStack,
    WSMT_InstMRU,
    WSMT_DataMRU,
    // these aren't WSM tables per se, but we'll give them names so that
    // we can more easily track and report their prefetches
    WSMT_OracleCacheSimPF,
    WSMT_FutureTracePF,
    WSMTableID_last
};
const char *WSMTableID_names[] = {
    "NextBlockInst",
    "NextBlockData",
    "NextBlockBoth",
    "StridePC",
    "SameObj",
    "Pointer",
    "PointerChase",
    "BTB",
    "BlockBTB",
    "PCWindow",
    "SPWindow",
    "RetStack",
    "InstMRU",
    "DataMRU",
    "OracleCacheSimPF",
    "FutureTracePF",
    NULL
};


enum BGStirPolicy {
    BGStir_None,
    BGStir_Rotate,
    BGStir_CounterMove,
    BGStir_DiagonalSwap,
    BGStirPolicy_last
};
const char *BGStirPolicy_names[] = {
    "None",
    "Rotate",
    "CounterMove",
    "DiagonalSwap",
    NULL
};


struct WSM_Config {
    typedef map<int,string> IntStringMap;
    typedef set<int> IntSet;

    bool capture_all_apps;
    int default_cam_size;
    int default_ptrcam_size;
    i64 table_age_period;
    bool table_age_as_serial;

    bool lru_replacement;
    bool move_rr_early;
    int replace_no_wins_cyc;
    int replace_no_hits_ser;
    bool ignore_zero_stride;

    // knobs controlling summarize_and_expand() behavior
    bool summarize_prefetch_as_excl;
    bool aged_delta_s;
    bool optimistic_history;
    int optimistic_hist_win_thresh;
    int forecast_window;
    bool forecast_on_wins;
    int forecast_min_blocks;
    bool forecast_hits_simple;
    bool sort_by_forecast;
    int max_prefetch_per_entry;
    int serials_since_hit_cutoff;
    int min_hits_cutoff;
    double forecast_scale_shared;

    struct {
        bool enable;
        u64 begin, end;
    } inst_shared_range;

    bool prefetch_all_apps;
    IntSet prefetch_for_apps;

    struct {
        int app_id;             // app ID to regularly move along; -1 for none
        // remaining invalid unless app_id >= 0
        i64 period;             // time (or commits) between forced migrations
        bool period_as_commits; // treat "period" as A<app_id> commits
        i64 exit_at_commit;     // exit sim. when "nomad" commits this many
        bool pause_cosched;     // pause cosched threads while resident
        bool inject_excl;
        bool inject_keep_excl;
        bool discard_departing;
        bool discard_arriving;
        bool discard_tlbs_too;
        bool dbp_filter_oracle;
        bool copy_l1i, copy_l1d, copy_l2;
        struct {
            bool copy_itlb;
            bool copy_dtlb;
            bool xfers_are_free;
            int injects_per_cyc;        // -1: inf
            int phys_addr_bits;
        } tlb_copy;
        struct {
            bool enable;
            i64 period;
            i64 period_offset;
            BGStirPolicy policy;
        } bg_stir;
        struct {
            bool enable;
        } oracle_inject;
        struct {
            bool enable;
            i64 expire_cyc;
            bool dumb_addr_xfer;
            int dumb_bits_per_addr;
        } oracle_cs_pf;         // Oracle tag-dump, cachesim PF
        struct {
            bool enable;
            bool prefer_imported_streams;
            bool imports_win_ties;
        } streambuf_mig;
        struct {
            bool enable;
            i64 expire_cyc;
            bool flush_at_move;
        } summ_pf;              // Summarize + PF
        struct {
            bool enable;
            string mem_ref_file;
            IntStringMap mem_file_map;  // app_id->name, overrides mem_ref_file
            FuturePrefetchSelect set_select;
            i64 window_size;
            bool sort_blocks;
            bool intersect_departing;
            bool split_pf_queues;
            bool ignore_sync_check;
            bool limit_to_regmove_period;
            bool omit_i_blocks;
            bool omit_d_blocks;
            bool rewind_on_overrun;
        } future_pf;
    } reg_move;

    NoDefaultCopy nocopy;

    // e.g. "A5" -> 5; returns -1 if key doesn't parse.
    static int key_to_app_id(const string& conf_key);

public:

    WSM_Config(const string& cfg_path);
    ~WSM_Config() { }
};


WSM_Config::WSM_Config(const string& cfg_path)
{
    const char *fname = "WSM_Config::WSM_Config";
    string cp = cfg_path + "/";          // short-hand for config-path
    string rm_base = cp + "RegularMove/";

    capture_all_apps = conf_bool(cp + "capture_all_apps");
    default_cam_size = conf_int(cp + "default_cam_size");
    if (default_cam_size <= 0) {
        exit_printf("default_cam_size (%d) invalid\n", default_cam_size);
    }
    default_ptrcam_size = conf_int(cp + "default_ptrcam_size");
    if (default_ptrcam_size <= 0) {
        exit_printf("default_ptrcam_size (%d) invalid\n", default_ptrcam_size);
    }
    table_age_period = conf_i64(cp + "table_age_period");
    table_age_as_serial = conf_bool(cp + "table_age_as_serial");

    lru_replacement = conf_bool(cp + "lru_replacement");
    move_rr_early = conf_bool(cp + "move_rr_early");
    replace_no_wins_cyc = conf_int(cp + "replace_no_wins_cyc");
    replace_no_hits_ser = conf_int(cp + "replace_no_hits_ser");
    ignore_zero_stride = conf_bool(cp + "ignore_zero_stride");

    summarize_prefetch_as_excl = conf_bool(cp + "summarize_prefetch_as_excl");
    aged_delta_s = conf_bool(cp + "aged_delta_s");
    optimistic_history = conf_bool(cp + "optimistic_history");
    optimistic_hist_win_thresh = conf_int(cp + "optimistic_hist_win_thresh");
    forecast_window = conf_int(cp + "forecast_window");
    forecast_on_wins = conf_bool(cp + "forecast_on_wins");
    forecast_min_blocks = conf_int(cp + "forecast_min_blocks");
    forecast_hits_simple = conf_bool(cp + "forecast_hits_simple");
    sort_by_forecast = conf_bool(cp + "sort_by_forecast");
    max_prefetch_per_entry = conf_int(cp + "max_prefetch_per_entry");
    serials_since_hit_cutoff = conf_int(cp + "serials_since_hit_cutoff");
    min_hits_cutoff = conf_int(cp + "min_hits_cutoff");
    forecast_scale_shared = conf_double(cp + "forecast_scale_shared");
    if (forecast_scale_shared < 0) {
        exit_printf("forecast_scale_shared (%g) invalid\n",
                    forecast_scale_shared);
    }
    
    {
        string base2(cp + "/InstSharedRange/");
        inst_shared_range.enable = conf_bool(base2 + "enable");
        inst_shared_range.begin = conf_x64(base2 + "begin");
        inst_shared_range.end = conf_x64(base2 + "end");
        if (inst_shared_range.begin > inst_shared_range.end) {
            exit_printf("%s: bad inst_shared_range: [%s,%s)\n", fname,
                        fmt_x64(inst_shared_range.begin),
                        fmt_x64(inst_shared_range.end));
        }
    }

    prefetch_all_apps = conf_bool(cp + "prefetch_all_apps");
    {
        // read PrefetchForApps -> prefetch_for_apps, parsing app IDs from keys
        string base2(cp + "PrefetchForApps");
        set<string> app_keys;
        SimCfg::conf_read_keys(base2, &app_keys);
        FOR_CONST_ITER(set<string>, app_keys, key_iter) {
            const string& key = *key_iter;
            int key_app_id = key_to_app_id(key);
            if (key_app_id < 0) {
                exit_printf("bad app ID specifier \"%s\" as key in %s\n",
                            key.c_str(), base2.c_str());
            }
            if (conf_bool(base2 + "/" + key)) {
                prefetch_for_apps.insert(key_app_id);
            }
        }
    }

    reg_move.app_id = conf_int(rm_base + "app_id");
    reg_move.period = conf_i64(rm_base + "period");
    if ((reg_move.app_id >= 0) && (reg_move.period <= 0)) {
        exit_printf("RegularMove app_id set (%d), but period invalid (%s)\n",
                    reg_move.app_id, fmt_i64(reg_move.period));
    }

    reg_move.period_as_commits = conf_bool(rm_base + "period_as_commits");
    reg_move.exit_at_commit = conf_i64(rm_base + "exit_at_commit");
    reg_move.pause_cosched = conf_bool(rm_base + "pause_cosched");
    reg_move.inject_excl = conf_bool(rm_base + "inject_excl");
    reg_move.inject_keep_excl = conf_bool(rm_base + "inject_keep_excl");
    reg_move.discard_departing = conf_bool(rm_base + "discard_departing");
    reg_move.discard_arriving = conf_bool(rm_base + "discard_arriving");
    reg_move.discard_tlbs_too = conf_bool(rm_base + "discard_tlbs_too");
    reg_move.dbp_filter_oracle = conf_bool(rm_base + "dbp_filter_oracle");

    if (reg_move.discard_departing && reg_move.discard_arriving) {
        err_printf("%s: warning: both discard_departing and discard_arriving "
                   "are enabled, are you sure you don't have the former on "
                   "by mistake?", fname);
    }

    reg_move.copy_l1i = conf_bool(rm_base + "copy_l1i");
    reg_move.copy_l1d = conf_bool(rm_base + "copy_l1d");
    reg_move.copy_l2 = conf_bool(rm_base + "copy_l2");

    {
        string base2(rm_base + "TLBCopy/");
        reg_move.tlb_copy.copy_itlb = conf_bool(base2 + "copy_itlb");
        reg_move.tlb_copy.copy_dtlb = conf_bool(base2 + "copy_dtlb");
        reg_move.tlb_copy.xfers_are_free = conf_bool(base2 + "xfers_are_free");
        reg_move.tlb_copy.injects_per_cyc =
            conf_int(base2 + "injects_per_cyc");
        reg_move.tlb_copy.phys_addr_bits = conf_int(base2 + "phys_addr_bits");
    }

    {
        string base2(rm_base + "BackgroundStir/");
        reg_move.bg_stir.enable = conf_bool(base2 + "enable");
        reg_move.bg_stir.period = conf_i64(base2 + "period");
        if (reg_move.bg_stir.period <= 0) {
            exit_printf("RegularMove background stir period invalid (%s)\n",
                        fmt_i64(reg_move.bg_stir.period));
        }
        reg_move.bg_stir.period_offset = conf_i64(base2 + "period_offset");
        if ((reg_move.bg_stir.period_offset < 0) ||
            (reg_move.bg_stir.period_offset >= reg_move.bg_stir.period)) {
            exit_printf("RegularMove background stir period_offset "
                        "invalid (%s)\n",
                        fmt_i64(reg_move.bg_stir.period_offset));
        }
        string policy_name = conf_str(base2 + "policy");
        int policy_num = enum_lookup(BGStirPolicy_names, policy_name.c_str());
        if (policy_num < 0) {
            exit_printf("RegularMove background stir policy unknown (%s)\n",
                        policy_name.c_str());
        }
        reg_move.bg_stir.policy = BGStirPolicy(policy_num);
    }

    {
        string base2(rm_base + "OracleInject/");
        reg_move.oracle_inject.enable = conf_bool(base2 + "enable");
    }

    {
        string base2(rm_base + "OracleCacheSimPF/");
        reg_move.oracle_cs_pf.enable = conf_bool(base2 + "enable");
        reg_move.oracle_cs_pf.expire_cyc = conf_i64(base2 + "expire_cyc");
        reg_move.oracle_cs_pf.dumb_addr_xfer =
            conf_bool(base2 + "dumb_addr_xfer");
        string key = base2 + "dumb_bits_per_addr";
        reg_move.oracle_cs_pf.dumb_bits_per_addr = conf_int(key);
        if (reg_move.oracle_cs_pf.dumb_bits_per_addr < 1) {
            exit_printf("illegal config value for %s, wanted >= 1\n",
                        key.c_str());
        }
    }

    {
        string base2(rm_base + "StreambufMigrate/");
        reg_move.streambuf_mig.enable = conf_bool(base2 + "enable");
        reg_move.streambuf_mig.prefer_imported_streams =
            conf_bool(base2 + "prefer_imported_streams");
        reg_move.streambuf_mig.imports_win_ties =
            conf_bool(base2 + "imports_win_ties");
    }

    {
        string base2(rm_base + "SummarizePF/");
        reg_move.summ_pf.enable = conf_bool(base2 + "enable");
        reg_move.summ_pf.expire_cyc = conf_i64(base2 + "expire_cyc");
        reg_move.summ_pf.flush_at_move = conf_bool(base2 + "flush_at_move");
    }

    {
        string base2(rm_base + "FutureTracePF/");
        reg_move.future_pf.enable = conf_bool(base2 + "enable");
        reg_move.future_pf.mem_ref_file = conf_str(base2 + "mem_ref_file");

        // read MemFileMap -> mem_file_map, parsing app IDs from keys
        {
            set<string> app_keys;
            string base3(base2 + "MemFileMap");
            SimCfg::conf_read_keys(base3, &app_keys);
            FOR_CONST_ITER(set<string>, app_keys, key_iter) {
                const string& key = *key_iter;
                int key_app_id = key_to_app_id(key);
                if (key_app_id < 0) {
                    exit_printf("bad app ID specifier \"%s\" as key in %s\n",
                                key.c_str(), base3.c_str());
                }
                string filename = conf_str(base3 + "/" + key);
                map_put_uniq(reg_move.future_pf.mem_file_map, key_app_id,
                             filename);
            }
        }
        reg_move.future_pf.set_select =
            FuturePrefetchSelect(conf_enum(FuturePrefetchSelect_names,
                                           base2 + "set_select"));
        reg_move.future_pf.window_size = conf_i64(base2 + "window_size");
        if (reg_move.future_pf.window_size < 0) {
            exit_printf("illegal config value for %s, wanted >= 0\n",
                        (base2 + "window_size").c_str());
        }
        reg_move.future_pf.sort_blocks = conf_bool(base2 + "sort_blocks");
        reg_move.future_pf.intersect_departing =
            conf_bool(base2 + "intersect_departing");
        reg_move.future_pf.split_pf_queues = 
            conf_bool(base2 + "split_pf_queues");
        reg_move.future_pf.ignore_sync_check = 
            conf_bool(base2 + "ignore_sync_check");
        reg_move.future_pf.limit_to_regmove_period = 
            conf_bool(base2 + "limit_to_regmove_period");
        if (reg_move.future_pf.limit_to_regmove_period && 
            !reg_move.period_as_commits) {
            exit_printf("limit_to_regmove_period option requires "
                        "period_as_commits to be on\n");
        }
        reg_move.future_pf.omit_i_blocks = conf_bool(base2 + "omit_i_blocks");
        reg_move.future_pf.omit_d_blocks = conf_bool(base2 + "omit_d_blocks");
        reg_move.future_pf.rewind_on_overrun =
            conf_bool(base2 + "rewind_on_overrun");
    }

    if ((reg_move.oracle_inject.enable + reg_move.oracle_cs_pf.enable
         + reg_move.summ_pf.enable + reg_move.future_pf.enable) > 1) {
        // Not strictly necessary?
        //exit_printf("more than one mutually-exclusive prefetch/migrate "
        //"mechanism enabled\n");
    }
}


int
WSM_Config::key_to_app_id(const string& conf_key)
{
    int key_app_id = -1;
    char dummy;
    if (sscanf(conf_key.c_str(), "A%d%c", &key_app_id, &dummy) != 1) {
        // Either didn't match enough to get an int for key_app_id, or found
        // extra stuff after the int (%c will match any char)
        key_app_id = -1;
    }
    ostringstream test_format;
    test_format << "A" << key_app_id;
    if (test_format.str() != conf_key) {
        // Reject e.g. "A002" instead of returning "2" -- enforce uniqueness
        // to avoid screwiness down the road
        key_app_id = -1;
    }
    return key_app_id;
}


enum PrefetchTarg {
    PFT_Inst, PFT_Data, PrefetchTarg_last
};
const char *PrefetchTarg_names[] = {
    "Inst", "Data", NULL
};


// Info needed to submit one prefetch request to memory subsystem
// (e.g. cachesim_prefetch_for_wsm())
class PrefetchEntry {
    LongAddr base_addr_;        // (addr==0: invalid)
    PrefetchTarg targ_;
    bool excl_access_;
    bool wait_for_prev_;        // RAW dep on immediate prior PF (if any)
    int table_id_;      // unique ID of table of who requested this (or -1)
    int entry_id_;      // some sort of table-specific index (or -1)

    // warning: copy/assignment in use

public:
    // PrefetchEntry(LongAddr base_addr__, PrefetchTarg targ__,
    //               bool excl_access__)
    //     : base_addr_(base_addr__), targ_(targ__),
    //       excl_access_(excl_access__), table_id_(-1),
    //       entry_id_(-1) {
    //     sim_assert(ENUM_OK(PrefetchTarg, targ_));
    // }
    PrefetchEntry() {
        base_addr_.a = 0;
    }
    PrefetchEntry(LongAddr base_addr__, PrefetchTarg targ__,
                  bool excl_access__, bool wait_for_prev__, 
                  int table_id__, int entry_id__)
        : base_addr_(base_addr__), targ_(targ__),
          excl_access_(excl_access__), wait_for_prev_(wait_for_prev__),
          table_id_(table_id__), entry_id_(entry_id__) {
        sim_assert(base_addr_.nonzero());
        sim_assert(ENUM_OK(PrefetchTarg, targ_));
    }

    bool valid() const { return base_addr_.a != 0 ;}
    void invalidate() { base_addr_.a = 0; }

    const LongAddr& base_addr() const { return base_addr_; }
    PrefetchTarg targ() const { return targ_; }
    bool excl_access() const { return excl_access_; }
    bool wait_for_prev() const { return wait_for_prev_; }
    int table_id() const { return table_id_; }
    int entry_id() const { return entry_id_; }

    string fmt() const {
        ostringstream out;
        out << "targ=" << PrefetchTarg_names[targ_]
            << ",base=" << base_addr_.fmt()
            << ",excl=" << fmt_bool(excl_access_)
            << ",wait=" << fmt_bool(wait_for_prev_);
        return out.str();
    }
};


// A PrefetchEntry, augmented with timing/sequencing info that relates it
// with adjacent entries in some sequence
class TimedPrefetchEntry : public PrefetchEntry {
    i64 start_time_;            // min absolute (global) time to start this pf
    int gap_time_after_;        // min cyc between this ent and next

public:
    TimedPrefetchEntry(const PrefetchEntry& ent,
                       i64 start_time__, int gap_time_after__)
        : PrefetchEntry(ent), start_time_(start_time__),
          gap_time_after_(gap_time_after__) {
        sim_assert(start_time_ >= 0);
        sim_assert(gap_time_after_ >= 0);
    }
    i64 start_time() const { return start_time_; }
    int gap_time_after() const { return gap_time_after_; }

    string fmt() const {
        ostringstream out;
        out << "start_time=" << start_time_
            << ",gap_time_after=" << gap_time_after_
            << "," << PrefetchEntry::fmt();
        return out.str();
    }
};


// final post-processed sequence of (unique) blocks to be prefetched,
// with ability to further delay as-yet-unsubmitted requests.  no duplicate
// addresses occur within a single PrefetchQueue.
class PrefetchQueue {
    typedef deque<TimedPrefetchEntry> EntrySeq;
    const int queue_id_;        // for debugging messages
    i64 ready_time_;            // next time some prefetch is ready
    EntrySeq ents_;
    NoDefaultCopy nocopy;

public:
    PrefetchQueue(int queue_id__) : queue_id_(queue_id__), ready_time_(0) { }
    bool empty() const { return ents_.empty(); }
    size_t size() const { return ents_.size(); }
    int queue_id() const { return queue_id_; }
    i64 ready_time() const {
        sim_assert(!empty());
        return ready_time_;
    }

    // note: entries must already be ordered WRT time
    void push_back(const TimedPrefetchEntry& next_pf) {
        if (ents_.empty()) {
            ready_time_ = MAX_SCALAR(ready_time_, next_pf.start_time());
        } else {
            sim_assert(next_pf.start_time() >= ents_.back().start_time());
        }
        ents_.push_back(next_pf);
    }
    void defer_start(i64 min_ready_time) {
        sim_assert(min_ready_time >= 0);
        ready_time_ = MAX_SCALAR(ready_time_, min_ready_time);
    }

    const PrefetchEntry& front() const {
        sim_assert(!empty());
        return ents_.front();
    }
    void pop_front(i64 now_time) {
        sim_assert(!empty());
        int gap_from_prev = ents_.front().gap_time_after();
        ents_.pop_front();
        if (!empty()) {
            ready_time_ = MAX_SCALAR(ready_time_, ents_.front().start_time());
        }
        ready_time_ = MAX_SCALAR(ready_time_, now_time + gap_from_prev);
    }
};


// A set of ready-to-go prefetch requests.  Requests within each
// PrefetchQueue may be dependent, but they are independent across queues.
class PrefetchCollection {
    typedef list<PrefetchQueue *> QueueList;   // note: PrefetchQueues "owned"
    QueueList queues_;
    NoDefaultCopy nocopy;

    friend class PrefetchCollector;     // for access to add_new_queue()

    // ownership transferred to this
    void add_new_queue(PrefetchQueue *queue) {
        queues_.push_back(queue);
    }

public:
    PrefetchCollection() { }
    ~PrefetchCollection() {
        FOR_ITER(QueueList, queues_, iter) {
            delete *iter;
        }
    }
    bool empty() const { return queues_.empty(); }
    size_t size() const { return queues_.size(); }

    // caller owns the returned pointer
    PrefetchQueue *remove_next_queue() {
        sim_assert(!empty());
        return pop_front_ret(queues_);
    }

    typedef QueueList::const_iterator const_iterator;
    const_iterator begin() const { return queues_.begin(); }
    const_iterator end() const { return queues_.end(); }
};


// Collects "future" prefetch requests from disparate sources, then digests
// them into a PrefetchCollection
class PrefetchCollector {
    class TimeKey {
        i64 time_;          // meaning not set in stone yet
        unsigned order_;    // to impose total order in the face of equal time
    public:
        TimeKey(i64 time__, unsigned order__)
            : time_(time__), order_(order__) { }
        i64 get_time() const { return time_; }
        bool operator < (const TimeKey& k2) const {
            return (time_ < k2.time_) ||
                ((time_ == k2.time_) && (order_ < k2.order_));
        }
        string fmt() const {
            ostringstream out;
            out << time_ << "," << "order_";
            return out.str();
        }
    };
    typedef map<TimeKey,TimedPrefetchEntry> TimeEntMap;
    typedef set<LongAddr> AddrSet;
    struct PerQueueEnt {
        TimeEntMap time_to_ents;
    };
    typedef map<int,PerQueueEnt> PerQueueMap;

    PerQueueMap queues_;
    unsigned next_timekey_order_;
    NoDefaultCopy nocopy;

    unsigned alloc_key_order() {
        unsigned this_ord = next_timekey_order_;
        ++next_timekey_order_;
        if (SP_F(next_timekey_order_ <= 0)) {
            // don't expect this to happen: we don't expect to put /that/ many
            // entries in for one round of prefetching, and we don't currently
            // re-use collector instances.  we can re-visit this later without
            // changing the API, if need be.
            abort_printf("unhandled PrefetchCollector key-order overflow\n");
        }
        return this_ord;
    }

public:
    PrefetchCollector() : next_timekey_order_(0) { }

    void add_prefetch(int queue_id, i64 origin_time, int gap_time,
                      const PrefetchEntry& pf) {
        WSMDB(2)("WSM: add_prefetch: queue_id %d origin_time %s gap_time %d "
                 "prefetch %s\n", queue_id, fmt_i64(origin_time),
                 gap_time, pf.fmt().c_str());
        PerQueueEnt& queue_ent = queues_[queue_id];   // auto-creates
        unsigned key_order = alloc_key_order();
        TimeKey time_key(origin_time, key_order);
        if (SP_F(!queue_ent.time_to_ents.
                 insert(std::make_pair(time_key,
                                       TimedPrefetchEntry(pf, origin_time,
                                                          gap_time)))
                 .second)) {
            abort_printf("PrefetchCollector key collision, key %s\n",
                         time_key.fmt().c_str());
        }
    }
    PrefetchCollection *collect() const;
};


// caller owns returned object (and must free it)
PrefetchCollection *
PrefetchCollector::collect() const
{
    scoped_ptr<PrefetchCollection> result_coll(new PrefetchCollection());
    FOR_CONST_ITER(PerQueueMap, queues_, queue_iter) {
        scoped_ptr<PrefetchQueue> pf_queue(new
                                           PrefetchQueue(queue_iter->first));
        const PerQueueEnt& queue_ent = queue_iter->second;
        set<LongAddr> seen_this_queue;
        sim_assert(!queue_ent.time_to_ents.empty());
        FOR_CONST_ITER(TimeEntMap, queue_ent.time_to_ents, ent_iter) {
            const TimedPrefetchEntry& pfe = ent_iter->second;
            if (seen_this_queue.insert(pfe.base_addr()).second) {
                // insert ok => first (earliest) instance for this queue
                pf_queue->push_back(pfe);
            }
        }
        result_coll->add_new_queue(scoped_ptr_release(pf_queue));
    }
    return scoped_ptr_release(result_coll);
}


bool
looks_like_pointer(mem_addr addr)
{
    bool result = (addr != 0) &&                // easy check
        ((addr & ~U64_LIT(0xfffffff)) != 0) &&  // sketchy hard-coded
        ((addr & 7) == 0);                      // aligned
    return result;
}


// A simple "int" wrapped up for use as a key in TableCAM<>.
struct TableKey_Int {
    int id;
    // WARNING: default copy/assignment in use
    TableKey_Int() { }
    explicit TableKey_Int(int id__) : id(id__) { }
    bool operator < (const TableKey_Int& o2) const { return (id < o2.id); }
    bool operator == (const TableKey_Int& o2) const { return (id == o2.id); }
    size_t stl_hash() const {
        StlHashU32 h;
        return h(id);
    }
    string fmt() const {
        ostringstream ostr;
        ostr << id;
        return ostr.str();
    }
};


// Fixed-sized CAM class: handles lookup, replacement, and element storage
// allocation.  Uses goofy round-robin replacement.
// 
// Currently, all stored element objects exist for the lifetime of the
// containing TableCAM.  Element destructors are not called when invalidating;
// rather, the caller is expected to be able to do something useful with
// the element pointers.  If we really want the contained EntType to be
// constructed on insert and destructed on removal, we could use a
// map<int,EntType> and delete entries that we wanted to be "gone".
//
// (It's a template so that we can use it to allocate storage for the specific
// entry types.  We use a vector to manage memory for the entries, but the
// entries themselves are not constructed/destructed at each replacement.)
//
// KeyType needs comparison ops, and stl_hash() method
template <typename KeyType, typename EntType>
class TableCAM {
    typedef TableCAM<KeyType, EntType>          container_type;
    typedef const TableCAM<KeyType, EntType>    const_container_type;
public:
    // Value pairs returned by table's iterators (similar to map<>)
    typedef pair<const KeyType *, EntType *>    TableIterPair;
    typedef pair<const KeyType *, const EntType *> TableConstIterPair;
private:

    //typedef bidir_map<KeyType,int> LookupMap;
    //typedef bidir_hash_map<KeyType,int,StlHashMethod<KeyType> > LookupMap;
    #if HAVE_HASHMAP
        typedef hash_map<KeyType,int,StlHashMethod<KeyType> > LookupMap;
    #else
        typedef std::map<KeyType,int> LookupMap;
    #endif // HAVE_HASHMAP
    typedef list<int> EntryIdxList;     // indices in EntryVec
    struct EntryBundle {
        bool valid;             // flag: "key" and "ent" valid (not lru_iter)
        KeyType key;
        EntType ent;            // (object lives for life of TableCAM)
        EntryIdxList::iterator lru_iter;        // (only valid w/LRU)
        EntryBundle(const WSM_Config *conf__) : ent(conf__) { }
    };
    typedef vector<EntryBundle> EntryVec;

    const WSM_Config *conf_;
    int n_entries_;             // table capacity, # entries
    LookupMap addr_lookup_;     // KeyType (e.g. LongAddr) -> index, valid only
    EntryVec entries_;          // [0...capacity)
    EntryIdxList entry_lru_;    // front is MRU, back is LRU (empty <=> use RR)
    int rr_replace_idx_;        // next index to replace (round-robin only)
    NoDefaultCopy nocopy_;

    // returns index of next victim, doesn't change anything
    int do_replace_probe() const {
        int vic_idx = (entry_lru_.empty()) ? rr_replace_idx_ :
            entry_lru_.back();
        return vic_idx;
    }
    // returns index of next victim, updates replacement info
    int do_replace() {
        int vic_idx;
        if (entry_lru_.empty()) {
            // round-robin
            vic_idx = rr_replace_idx_;
            rr_replace_idx_ = INCR_WRAP(rr_replace_idx_, n_entries_);
        } else {
            // lru: choose index at end, move it to front (implicit touch)
            vic_idx = entry_lru_.back();
            entry_lru_.pop_back();
            entry_lru_.push_front(vic_idx);
            vec_idx(entries_, vic_idx).lru_iter = entry_lru_.begin();
        }
        return vic_idx;
    }
    // inform the replacement system that the given entry is being used
    // (it may be invalid, if we're replacing it)
    void touch_ent(int idx) {
        if (entry_lru_.empty()) {
            // using round-robin replacement; optionally bump RR pointer
            if (conf_->move_rr_early && (idx == rr_replace_idx_)) {
                rr_replace_idx_ = INCR_WRAP(rr_replace_idx_, n_entries_);
            }
        } else {
            // using LRU: move to front of entry_lru_ list
            EntryBundle& ent_bund = vec_idx(entries_, idx);
            entry_lru_.erase(ent_bund.lru_iter);
            entry_lru_.push_front(idx);
            ent_bund.lru_iter = entry_lru_.begin();
        }
    }

    // these were originally public, but we'd like to hide the details of
    // entry layout (i.e. use of vector, indexes) if possible
    bool have_entry(int idx) const {
        return vec_idx(entries_, idx).valid;
    }
    const KeyType *get_key(int idx) const {
        const EntryBundle& ent_bund = vec_idx(entries_, idx);
        return (ent_bund.valid) ? &ent_bund.key : NULL;
    }
    EntType *get_entry(int idx) {
        EntryBundle& ent_bund = vec_idx(entries_, idx);
        return (ent_bund.valid)? &ent_bund.ent : NULL;
    }
    const EntType *get_entry(int idx) const {
        const EntryBundle& ent_bund = vec_idx(entries_, idx);
        return (ent_bund.valid)? &ent_bund.ent : NULL;
    }

public:
    TableCAM(const WSM_Config *conf__, int capacity)
        : conf_(conf__), n_entries_(capacity)
    {
        sim_assert(n_entries_ > 0);
        entries_.reserve(n_entries_);
        for (int i = 0; i < n_entries_; ++i)
            entries_.push_back(EntryBundle(conf__));
        // not valid until reset()
    }
    ~TableCAM() { }
    
    void reset() {
        sim_assert(intsize(entries_) == n_entries_);
        addr_lookup_.clear();
        entry_lru_.clear();
        FOR_ITER(typename EntryVec, entries_, iter) {
            iter->valid = false;
            iter->ent.reset();
        }
        if (conf_->lru_replacement) {
            // leave index 0 as next victim
            for (int idx = n_entries_ - 1; idx >= 0; --idx) {
                entry_lru_.push_back(idx);
                vec_idx(entries_, idx).lru_iter = --(entry_lru_.end());
            }
        }
        rr_replace_idx_ = 0;
    }

    bool have_key(const KeyType& key) const { return addr_lookup_.count(key); }

    // returns entry pointer, or null if not found.  A found entry doesn't
    // imply a hit, the specific table type may have further conditions
    // (e.g. stride match)
    EntType *lookup(const KeyType& key, int *index_ret = 0) {
        int found_idx = map_at_default(addr_lookup_, key, -1);
        if (found_idx < 0)
            return NULL;
        EntryBundle& ent_bund = vec_idx(entries_, found_idx);
        sim_assert(ent_bund.valid);     // invalid ents shouldn't be in map
        touch_ent(found_idx);
        if (index_ret)
            *index_ret = found_idx;
        return &ent_bund.ent;
    }

    EntType *probe(const KeyType& key, int *index_ret = 0) {
        int found_idx = map_at_default(addr_lookup_, key, -1);
        if (found_idx < 0)
            return NULL;
        EntryBundle& ent_bund = vec_idx(entries_, found_idx);
        sim_assert(ent_bund.valid);
        if (index_ret)
            *index_ret = found_idx;
        return &ent_bund.ent;
    }
   
    // return pointer to the entry which would be evicted if replacement is
    // performed with the given key, or NULL if none valid.  new_key must
    // not already be present.
    const EntType *replace_probe(const KeyType& new_key) const {
        // (round-robin replacement ignores new_key)
        int vic_idx = do_replace_probe();
        const EntryBundle& vic_bund = vec_idx(entries_, vic_idx);
        return (vic_bund.valid) ? &vic_bund.ent : NULL;
    }

    // old key must be present; new_key must not
    void change_key(const KeyType& old_key, const KeyType& new_key) {
        int ent_idx = map_at(addr_lookup_, old_key);
        EntryBundle& ent_bund = vec_idx(entries_, ent_idx);
        sim_assert(ent_bund.valid);
        addr_lookup_.erase(old_key);
        map_put_uniq(addr_lookup_, new_key, ent_idx);
        ent_bund.key = new_key;
    }

    // hint: spare the current victim (as identified by replace_probe()),
    // since it looks important.  This isn't guaranteed, and doesn't undo
    // any replacement that's already been performed.
    void spare_current_victim() {
        // bump the victim to "replaced-anew", without invalidating it;
        // this is fine for round-robin and LRU, but may need to get smarter,
        // later
        do_replace();
    }

    // returns pointer to (possibly invalid) victim entry; caller still needs
    // to assign to it.  new_key must not already be present.
    EntType *replace(const KeyType& new_key, bool *was_valid_ret = 0) {
        // int *index_ret = 0
        int vic_idx = do_replace();
        EntryBundle& vic_bund = vec_idx(entries_, vic_idx);
        bool vic_was_valid = vic_bund.valid;
        if (vic_was_valid)
            addr_lookup_.erase(vic_bund.key);
        vic_bund.valid = true;
        vic_bund.key = new_key;
        map_put_uniq(addr_lookup_, new_key, vic_idx);
        if (was_valid_ret)
            *was_valid_ret = vic_was_valid;
        //if (index_ret)
        //    *index_ret = vic_idx;
        sim_assert(intsize(addr_lookup_) <= n_entries_);
        return &vec_idx(entries_, vic_idx).ent;
    }

    // true <=> entry found (and erased)
    bool erase_entry(const KeyType& key) {
        int ent_idx = map_at_default(addr_lookup_, key, -1);
        if (ent_idx >= 0) {
            EntryBundle& ent_bund = vec_idx(entries_, ent_idx);
            sim_assert(ent_bund.valid);
            addr_lookup_.erase(ent_bund.key);
            if (entry_lru_.empty()) {
                // using round-robin replacement; not 100% clear what's best
                // to do here
                rr_replace_idx_ = ent_idx;
            } else {
                // using LRU: move to back of entry_lru_ (will be next victim)
                entry_lru_.erase(ent_bund.lru_iter);
                entry_lru_.push_back(ent_idx);
                ent_bund.lru_iter = --(entry_lru_.end());
            }
            ent_bund.valid = false;
        }
        return (ent_idx >= 0);
    }


    // Iterators
    // (warning: missing auto iterator->const_iterator conversion)
    // (warning: ++/-- step per entries[], not all guaranteed valid)
    template <typename ParentType, typename EntryIterType,
              typename EltPairType>
    class IterTmpl {
        typedef IterTmpl<ParentType, EntryIterType, EltPairType> iter_type;
        //ParentType& parent;     // place-holder
        EntryIterType walk;
        EntryIterType first;    // first entry
        EntryIterType limit;    // one past last entry
        
        //int walk;               // index into entries[]
        //const int limit;        // one past last entry; "end" value
        EltPairType to_ret;
        void build_pair() {
            if ((walk != limit) && (walk->valid)) {
                to_ret.first = &walk->key;
                to_ret.second = &walk->ent;
            } else {
                to_ret.first = NULL;
                to_ret.second = NULL;
            }
        }
        void seek_fwd() {
            while ((walk != limit) && !walk->valid) {
                ++walk;
            }
        }
        void seek_back() {
            while ((walk != first) && !walk->valid) {
                --walk;
            }
        }

    public:
        IterTmpl(ParentType& parent, int dummy)
            //: parent(parent_), walk(0), limit(int(parent.entries.size())) {
            : walk(parent.entries_.begin()), first(walk), 
              limit(parent.entries_.end()) {
            seek_fwd();
            build_pair();
        }
        IterTmpl(ParentType& parent)   // "end" constructor
            //: parent(parent_), walk(int(parent.entries.size())),
            : walk(parent.entries_.end()), first(walk), limit(walk) {
            build_pair();
        }

        const EltPairType& operator* () const { return to_ret; }
        const EltPairType *operator-> () const { return &to_ret; }
        iter_type& operator++() {       // pre-inc
            if (walk != limit) ++walk;
            seek_fwd();
            build_pair();
            return *this;
        }
        iter_type& operator--() {       // pre-dec
            if (walk != first) --walk;
            seek_back();
            build_pair();
            return *this;
        }
        iter_type operator++(int) {    // post-inc
            iter_type temp = *this;
            ++(*this);
            return temp;
        }
        iter_type operator--(int) {    // post-dec
            iter_type temp = *this;
            --(*this);
            return temp;
        }
        bool operator== (const iter_type& o2) const
        { return walk == o2.walk; }
        bool operator!= (const iter_type& o2) const
        { return walk != o2.walk; }
    };
    typedef IterTmpl<container_type, typename EntryVec::iterator,
                     TableIterPair> iterator;
    typedef IterTmpl<const_container_type, typename EntryVec::const_iterator,
                     TableConstIterPair> const_iterator;

    iterator begin() { return iterator(*this, 0); }
    iterator end() { return iterator(*this); }
    const_iterator begin() const { return const_iterator(*this, 0); }
    const_iterator end() const { return const_iterator(*this); }
};


class TabEntStats {
    const WSM_Config *conf_;

    i64 create_cyc;
    i64 create_serial;
    i64 hits;
    i64 hits_aged;
    i64 last_hit_serial;
    i64 last_hit_cyc;
    i64 wins;
    i64 wins_aged;
    i64 last_win_serial;
    i64 last_win_cyc;

    struct AtLastAge {
        i64 age_count;
        i64 cyc;
        i64 serial;
        i64 hits;
        double hits_aged;
        i64 wins;
        double wins_aged;
        void reset() {
            age_count = 0;
            cyc = serial = 0;
            hits = wins = 0;
            hits_aged = wins_aged = 0;
        }
    };
    AtLastAge at_age;

    i64 serial_now() const {
        sim_assert(GlobalAccessSerialNum >= 0);
        return GlobalAccessSerialNum;
    }

public:
    TabEntStats(const WSM_Config *conf__) : conf_(conf__) { }

    i64 g_hits() const { return hits; }
    i64 g_hits_aged() const { return hits_aged; }
    i64 g_last_hit_serial() const { return last_hit_serial; }
    i64 g_last_hit_cyc() const { return last_hit_cyc; }
    i64 g_wins() const { return wins; }
    i64 g_wins_aged() const { return wins_aged; }
    i64 g_last_win_serial() const { return last_win_serial; }
    i64 g_last_win_cyc() const { return last_win_cyc; }

    void reset() {
        create_cyc = cyc;
        create_serial = serial_now();
        hits = hits_aged = wins = wins_aged = 0;
        last_hit_cyc = last_win_cyc = 0;
        last_hit_serial = last_win_serial = 0;
        at_age.reset();
    }
    void log_hit() {
        hits++;
        hits_aged++;
        last_hit_serial = serial_now();
        last_hit_cyc = cyc;
    }
    void log_win() {
        wins++;
        wins_aged++;
        last_win_serial = serial_now();
        last_win_cyc = cyc;
    }
    void age() {
        hits_aged /= 2;
        wins_aged /= 2;
        sim_assert(serial_now() >= 0);
        at_age.age_count++;
        at_age.serial = serial_now();
        at_age.cyc = cyc;
        at_age.hits_aged += (hits - at_age.hits);
        at_age.hits_aged /= 2;
        at_age.wins_aged += (wins - at_age.wins);
        at_age.wins_aged /= 2;
        at_age.hits = hits;
        at_age.wins = wins;
    }

    i64 life_time() const {
        return cyc - create_cyc;
    }
    i64 life_serial() const {
        return serial_now() - create_serial;
    }

    string fmt() const {
        ostringstream out;
        out << "<"
            << "lt=" << life_time() << ",ls=" << life_serial()
            << ",h=" << hits << ",ha=" << hits_aged
            << ",csh=" << cyc_since_hit() << ",ssh=" << ser_since_hit()
            << ",w=" << wins << ",wa=" << wins_aged
            << ",csw=" << cyc_since_win() << ",ssw=" << ser_since_win()
            << ",age=<"
            << "ac=" << at_age.age_count
            << ",csa=" << cyc_since_age() << ",ssa=" << ser_since_age()
            << ",h=" << at_age.hits << ",ha=" << at_age.hits_aged
            << ",w=" << at_age.wins << ",wa=" << at_age.wins_aged
            << ">>";
        return out.str();
    }

    i64 ser_since_hit() const {
        return (serial_now() - last_hit_serial);
    }
    i64 ser_since_win() const {
        return (serial_now() - last_win_serial);
    }
    i64 ser_since_age() const {
        return serial_now() - at_age.serial;
    }
    i64 cyc_since_hit() const {
        return cyc - last_hit_cyc;
    }
    i64 cyc_since_win() const {
        return cyc - last_win_cyc;
    }
    i64 cyc_since_age() const {
        return cyc - at_age.cyc;
    }
    i64 delta_s() const {
        return ((conf_->aged_delta_s) ? wins_aged : wins) - 
            (serial_now() - last_win_serial);
    }

    i64 forecast_wins_life(i64 forecast_serials) const {
        return (wins * forecast_serials) / life_serial();
    }
    i64 forecast_wins_recent(i64 forecast_serials) const {
        i64 wins_since_age = wins - at_age.wins;
        i64 serial_since_age = serial_now() - at_age.serial;
        return (serial_since_age) ?
            ((wins_since_age * forecast_serials) / serial_since_age) : 0;
    }
    // common forecast code for hits or wins (events)
    i64 forecast_events_history(i64 forecast_serials,
                                i64 events_now, i64 events_at_age,
                                i64 events_aged_at_age) const {
        // mean event rate from t0 to last age: events_at_age/table_age_period
        // mean event rate from lastage->now: events_since_age/serial_since_age
        // forecast rate: mean of above two, if "recent" has covered enough
        // forecast events: forecast rate * forecast_serials
        double historical_event_rate =
            double(events_aged_at_age) / conf_->table_age_period;
        i64 serial_since_age = serial_now() - at_age.serial;
        i64 events_since_age = events_now - events_at_age;
        double recent_event_rate;
        double forecast_event_rate;
        double include_recent_thresh = ceil(1.0 / historical_event_rate);
        if ((serial_since_age >= include_recent_thresh) ||
            (events_since_age > 2)) {
            recent_event_rate = double(events_since_age) / serial_since_age;
            if (conf_->optimistic_history &&
                (recent_event_rate > historical_event_rate) &&
                (events_since_age >=
                 conf_->optimistic_hist_win_thresh)) {
                // recent rate looks better than historical, across a decent
                // number of samples; optimistically use it for this forecast
                forecast_event_rate = recent_event_rate;
            } else {
                forecast_event_rate = (historical_event_rate +
                                       recent_event_rate) / 2;
            }
        } else {
            recent_event_rate = SimNAN;
            forecast_event_rate = historical_event_rate;
        }
        i64 forecast_events = i64(floor((forecast_event_rate *
                                         forecast_serials) + 0.5));
        if (kShowExtraHistoryForecastSpam) {
            DEBUGPRINTF(">>HIST: hist %g recent %g ssathresh %g forecast %g "
                        "-> result %s<<\n",
                        historical_event_rate, recent_event_rate,
                        include_recent_thresh, forecast_event_rate,
                        fmt_i64(forecast_events));
        }
        return forecast_events;
    }
    i64 forecast_wins_history(i64 forecast_serials) const {
        return forecast_events_history(forecast_serials, wins, at_age.wins,
                                       i64(at_age.wins_aged));
    }
    i64 forecast_hits_history(i64 forecast_serials) const {
        return forecast_events_history(forecast_serials, hits, at_age.hits,
                                       i64(at_age.hits_aged));
    }

    bool cmp_wins(const TabEntStats& ent2) const {
        return (wins < ent2.wins) ||
            ((wins == ent2.wins) && (hits < ent2.hits));
    }
    bool cmp_wins_aged(const TabEntStats& ent2) const {
        return (wins_aged < ent2.wins_aged) ||
            ((wins_aged == ent2.wins_aged) && (hits_aged < ent2.hits_aged));
    }
    bool cmp_delta_s(const TabEntStats& ent2) const {
        return delta_s() < ent2.delta_s();
    }
};


// WARNING: EntryBase objects are treated more "struct-like" than
// "object-like": they're constructed once at CAM creation time, with no
// type-specific parameters, and they're manipulated extensively by their
// parent tables' ent_{assign,init_predict,etc.} methods via entry pointers.
// (We do this in part so that specifically-typed table methods can use
// table-level variables and table-specific configuration data.)
struct EntryBase {
    const WSM_Config *conf_;
    TabEntStats stats;

    EntryBase(const WSM_Config *conf__)
        : conf_(conf__), stats(conf__) { reset(); }
    virtual ~EntryBase() { }
    virtual void reset() { }

    // given a CAM tag hit, decide if this entry qualifies as match
    // overall (e.g. stride match)
    virtual bool is_match(const MemProfSample *samp) const { return true; }

    // hook: this entry has had a CAM tag hit for the given memory operation
    // in the "sample" stage, though is_match() isn't necessarily true.
    // update the entry if it needs any bookeeping.
    virtual void touch_sample(const MemProfSample *samp) { }

    // hook: this entry is a CAM tag hit for some memory operation, in the
    // "update" stage.  (recall that different keys may be used for
    // the "sample" and "update" stages, leading to different entries being
    // visited.)
    virtual void touch_update(const MemProfSample *samp) { }

    // a new memory-operation is being inserted into the table at this entry;
    // the CAM tag has already been reassigned
    void base_assign() {
        stats.reset();
    }

    // test: OK to replace this entry?  this is just advisory, to avoid
    // replacing "good" entries; caller may replace regardless.
    virtual bool replace_ok() const {
        bool result;
        if (conf_->replace_no_hits_ser >= 0) {
            // never hit, or hasn't hit within past N accesses
            result = (stats.g_hits() == 0) ||
                (stats.ser_since_hit() > conf_->replace_no_hits_ser);
        } else {
            // pre wsm20090505d: never won, or hasn't won within N cycles
            result = (stats.g_wins() == 0) ||
                (stats.cyc_since_win() > conf_->replace_no_wins_cyc);
        }
        return result;
    }

    virtual string fmt() const = 0;

    virtual void age_entry() {
        stats.age();
    }
};


struct PerTableStats;


enum EntryExpandType {
    Expand_Single,              // an entry expands to one block
    Expand_Finite,              // an entry expands to a fixed-count # blocks
    Expand_Infinite,            // an entry can be "pumped" arbitrarily
    EntryExpandType_last
};


// abstract (template-free method call) interface for operations common to
// all tables
struct TableBase {
    virtual ~TableBase() { }
    virtual void reset() = 0;
    virtual void flush_entries() = 0;
    virtual void flush_one_app(int flush_app_id) = 0;

    // returns: whether this table should consider this sample futher for
    // the given operation type
    virtual bool sample_accept(const MemProfSample *samp) const = 0;
    virtual bool update_accept(const MemProfSample *samp) const {
        return sample_accept(samp);
    }

    // returns: was a match (CAM hit + any additional criteria)
    virtual bool sample(const AppState *as, const MemProfSample *samp) = 0;
    // phase two of sampling: update winner-dependent stats
    virtual void sample2(const AppState *as, const MemProfSample *samp,
                         bool was_winner) = 0;
    // update table (may access a different entry than sample/sample2)
    virtual void update(const AppState *as, const MemProfSample *samp) = 0;

    // summarize table contents to a reduced form suitable for inter-core
    // transfer, then 1) expand that reduced form to produce a sequence of
    // prefetches and submit them to the collector "pf_coll", and 2) estimate
    // the size in bits needed to transfer that reduced form and write it to
    // "xfer_bits_ret".  "collector_queue_id" is a bidirectional parameter,
    // a counter that's shared across multiple tables an entries, used
    // to supply unique system-wide queue IDs for a single sweep of
    // summarize_and_expand().
    virtual void summarize_and_expand(int for_app_id,
                                      PrefetchCollector *pf_coll,
                                      int *collector_queue_id,
                                      long *xfer_bits_ret) = 0;

    virtual void age_table() = 0;

    virtual string g_name() const = 0;
    virtual double g_priority() const = 0;  // static priority, not forecast
    virtual const PerTableStats& g_stats() const = 0;

    // query: does this table type use online feedback, i.e. does it
    // meaningfully use observed behavior to rank (or improve) the
    // effectiveness of individual entries?  e.g. a pattern-correlating
    // predictor would be "yes", a table which just mechanically tracks
    // the program counter would be "no".
    virtual bool uses_online_feedback() const = 0;
    virtual EntryExpandType entry_expand_type() const = 0;
};


// collection of knobs used by expand_entry()
// (seems kinda pointless to go to this extra trouble versus just using
// per-class constants, but this'll make it easier in case we want to
// virtualize it)
struct ExpandEntryParms {
    int master_id;              // address space ID for this expand sequence
    int first_block_bits;       // transfer bits to bill for first block
    int later_block_bits;       // transfer bits to bill for each additional
    PrefetchTarg prefetch_target;
    bool wait_for_prev;         // flag: prefetches are all RAW-dependent
    void assign(int master_id_, int first_block_bits_, int later_block_bits_,
                PrefetchTarg prefetch_target_, bool wait_for_prev__) {
        master_id = master_id_;
        first_block_bits = first_block_bits_;
        later_block_bits = later_block_bits_;
        prefetch_target = prefetch_target_;
        wait_for_prev = wait_for_prev__;
    }
};


struct PerTableStats {
    // rejected samples/updates aren't counted here
    i64 samples_accepted;       // submitted to sample()
    i64 samples_matched;        // subset of _accepted that matched
    i64 samples_match_wins;     // subset of _matched that won
    i64 updates_accepted;       // submitted to update()
    i64 updates_touched;        // _accepted that touched existing entry
    i64 updates_replace;        // _accepted on new data, replaced old
    i64 updates_noreplace;      // _accepted on new data, replace suppressed 
    i64 total_blocks;           // total #blocks requested by this table
    i64 total_bits;             // total #bits used for requests
    struct {
        // table-wide, one sample per summarize_and_expand()
        BasicStat_I64 entries;  // #table entries contributing
        BasicStat_I64 blocks;   // #blocks requested
        BasicStat_I64 bits;     // #bits used to encode blocks
    } per_expand;
    struct {
        // one sample per expanded entry
        BasicStat_I64 blocks;
        BasicStat_I64 bits;
    } per_expand_entry;

    void reset() {
        samples_accepted = 0;
        samples_matched = 0;
        samples_match_wins = 0;
        updates_accepted = 0;
        updates_touched = 0;
        updates_replace = 0;
        updates_noreplace = 0;
        total_blocks = 0;
        total_bits = 0;
        per_expand.entries.reset();
        per_expand.blocks.reset();
        per_expand.bits.reset();
        per_expand_entry.blocks.reset();
        per_expand_entry.bits.reset();
    }
};


// An abstract "TableBase", augmented with default implentations for the
// various operations.  This has knowledge (through the template parameters)
// of the specific entry type in use.  The specific table type which derives
// from this still needs to define methods for calculating index keys, etc.
//
// tmpl_key type needs comparison ops, fmt() format method, stl_hash()
// hash method, copy/assignment ops, etc.
template <typename tmpl_key, typename tmpl_entry>
class TableWithDefaultOps : public TableBase {
protected:
    typedef tmpl_key KeyType;
    typedef tmpl_entry EntType;
    typedef TableCAM<KeyType,EntType> CAMType;
    typedef typename CAMType::const_iterator CAMConstIterType;

    const WSM_Config *conf_;
    string name;
    int table_id;               // (for later use with PrefetchAuditor)
    double priority;
    int block_bytes_lg, block_bytes;
    scoped_ptr<CAMType> cam;
    double forecast_scale_local;
    int min_hits_cutoff_local;
    PerTableStats stats;
    NoDefaultCopy nocopy;

    // lazy: utility code
    void block_align(LongAddr *addr) const { addr->a &= ~(block_bytes - 1); }
    void block_align(mem_addr *addr) const { (*addr) &= ~(block_bytes - 1); }

    virtual KeyType sample_key(const MemProfSample *samp) const = 0;
    virtual KeyType update_key(const MemProfSample *samp) const {
        return sample_key(samp);
    }

    // (used for filtering entries to a specific app, at expand-time,
    // as well as flushing entries from a particular app)
    virtual bool key_matches_appid(const KeyType& key, int app_id) const = 0;

    // last-minute hack: query, after touch_sample(), and touch_update(),
    // "should we change the key associated with this entry?"  Returns true
    // iff so, and writes the desired new key to "new_entry_ret".  (the
    // key-change request is currently ignored if the new key is already
    // present)
    virtual bool postsample_rekey_hook(const KeyType& key, EntType *ent,
                                       KeyType *new_entry_ret) {
        return false;
    }

    // query an entry: "how good do you think you are, and how many
    // hits do you think you'll be able to produce over the next
    // <forecast_window> operations?"
    virtual void ent_forecast(const KeyType& key, const EntType *ent,
                              i64 forecast_window, i64 *forecast_count_ret,
                              i64 *priority_ret) const {
        // assert: this default generic TableStats-based forecast really only
        // makes sense for those with online feedback
        sim_assert(this->uses_online_feedback());
        i64 count;
        if (conf_->forecast_hits_simple) {
            count = ent->stats.g_hits();
        } else {
            count =
                (conf_->forecast_on_wins) ?
                ent->stats.forecast_wins_history(forecast_window)
                : ent->stats.forecast_hits_history(forecast_window);
        }
        if (this->entry_expand_type() == Expand_Single) {
            // override: table specifically says entrys can only expand to one
            // block, so we'll believe it
            count = MIN_SCALAR(count, 1);       // forecast 0 or 1 only
        }
        *forecast_count_ret = count;
        *priority_ret = count;
    }

    // required: a new memory-operation is being inserted into the table at
    // this entry; the CAM tag has already been reassigned.  this should
    // at minimum call EntryBase::base_assign().  (This seems a little
    // awkward, but this is currently how we allow table-specific info
    // to flow from a table to individual entries.)
    virtual void ent_assign(EntType *ent, const AppState *as,
                            const MemProfSample *samp) = 0;

    // table-specific support for base class summarize_and_expand() ->
    // expand_entry()
    virtual void get_expand_entry_parms(const KeyType& key, const EntType& ent,
                                        ExpandEntryParms *ret) const = 0;
    virtual void ent_init_predict(const KeyType& key, EntType *ent) const {
        // default: no work needed
    }
    virtual bool ent_can_predict(const KeyType& key,
                                 const EntType *ent) const = 0;
    virtual mem_addr ent_step_predict(const KeyType& key,
                                      EntType *ent) const = 0;

    // expand one entry in this table, generating block addresses
    // for a predicted future
    virtual void expand_entry(const KeyType& key, const EntType& ent,
                              int entry_id,
                              int pf_count_limit, i64 pf_clock_base,
                              int pf_clock_step, PrefetchCollector *pf_coll,
                              int collector_queue_id, int *xfer_blocks_ret,
                              long *xfer_bits_ret) const
    {
        mem_addr prev_pf_base_addr = 0;
        int pf_count = 0;
        int blocks_sent = 0;
        long bits_used = 0;
        ExpandEntryParms parms;
        this->get_expand_entry_parms(key, ent, &parms);
        EntType predict_ent(ent);
        this->ent_init_predict(key, &predict_ent);
        while ((pf_count < pf_count_limit) &&
               this->ent_can_predict(key, &predict_ent)) {
            mem_addr this_pf_addr = ent_step_predict(key, &predict_ent);
            block_align(&this_pf_addr);
            if (this_pf_addr && (this_pf_addr != prev_pf_base_addr)) {
                WSMDB(2)("WSM: expand_entry (tab %s id %d ent %d) key %s "
                         "pf# %d block# %d base_addr %s\n", name.c_str(),
                         table_id, entry_id, key.fmt().c_str(), pf_count,
                         blocks_sent, fmt_mem(this_pf_addr));
                bool pf_as_excl = conf_->summarize_prefetch_as_excl;
                if (conf_->inst_shared_range.enable &&
                    (this_pf_addr >= conf_->inst_shared_range.begin) &&
                    (this_pf_addr < conf_->inst_shared_range.end)) {
                    pf_as_excl = false;
                }
                PrefetchEntry pf_ent(LongAddr(this_pf_addr, parms.master_id),
                                     parms.prefetch_target,
                                     pf_as_excl, parms.wait_for_prev,
                                     table_id, entry_id);
                pf_coll->add_prefetch(collector_queue_id, pf_clock_base,
                                      pf_clock_step, pf_ent);
                prev_pf_base_addr = this_pf_addr;
                bits_used += (!blocks_sent) ? parms.first_block_bits :
                    parms.later_block_bits;
                ++blocks_sent;
            }
            ++pf_count;
        }
        *xfer_blocks_ret = blocks_sent;
        *xfer_bits_ret = bits_used;
    }

public:
    TableWithDefaultOps(const WSM_Config *conf__,
                        const string& name_, int table_id_, const string& cfg,
                        int default_size, int block_bytes_lg_)
        : TableBase(), conf_(conf__), name(name_), table_id(table_id_),
          block_bytes_lg(block_bytes_lg_),
          block_bytes(1 << block_bytes_lg)
    {
        int cam_size = default_size;
        string key;
        key = cfg + "/prio";
        priority = conf_double(key);
        key = cfg + "/cam_size";
        if (have_conf(key)) cam_size = conf_int(key);
        sim_assert(cam_size > 0);
        cam.reset(new CAMType(conf__, cam_size));
        stats.reset();

        key = cfg + "/forecast_scale_local";
        forecast_scale_local = 1.0;
        if (have_conf(key)) {
            forecast_scale_local = conf_double(key);
            if (forecast_scale_local < 0) {
                exit_printf("forecast_scale_local (%g) invalid\n",
                            forecast_scale_local);
            }
        }

        key = cfg + "/min_hits_cutoff_local";
        min_hits_cutoff_local = 0;
        if (have_conf(key)) {
            min_hits_cutoff_local = conf_int(key);
        }
    }

    string g_name() const { return name; }
    double g_priority() const { return priority; }
    const PerTableStats& g_stats() const { return stats; }
    double g_forecast_scale_local() const { return forecast_scale_local; }
    double g_min_hits_cutoff_local() const { return min_hits_cutoff_local; }
    void reset() {
        cam->reset();
        stats.reset();
    }
    void flush_entries() {
        cam->reset();
    }

    bool sample(const AppState *as, const MemProfSample *samp) {
        KeyType key_addr = sample_key(samp);
        EntType *ent = cam->lookup(key_addr);
        WSMDB(3)("WSM: table: %s %s -> %s\n", name.c_str(),
                 key_addr.fmt().c_str(),
                 (ent) ? (ent->fmt().c_str()) : "(null)");
        bool is_match = ent && ent->is_match(samp);
        ++stats.samples_accepted;
        if (is_match)
            ++stats.samples_matched;
        if (ent) {
            ent->touch_sample(samp);
        }
        if (is_match) {
            WSMDB(3)("WSM: %s match\n", name.c_str());
            ent->stats.log_hit();
        } else {
            WSMDB(3)("WSM: %s miss\n", name.c_str());
        }
        return is_match;
    }

    void sample2(const AppState *as, const MemProfSample *samp,
                 bool was_winner) {
        KeyType key_addr = sample_key(samp);
        EntType *ent = cam->probe(key_addr);
        if (was_winner) {
            sim_assert(ent);
            ++stats.samples_match_wins;
            ent->stats.log_win();
        }

        KeyType new_key;
        if (ent && postsample_rekey_hook(key_addr, ent, &new_key)) {
            if (!cam->have_key(new_key)) {
                WSMDB(3)("WSM: %s changing key(1), %s -> %s\n", name.c_str(),
                         key_addr.fmt().c_str(), new_key.fmt().c_str());
                cam->change_key(key_addr, new_key);
                key_addr = new_key;
            } else {
                WSMDB(3)("WSM: %s ignoring key-change, %s -> %s, "
                         "target key already exists\n", name.c_str(),
                         key_addr.fmt().c_str(), new_key.fmt().c_str());
            }
        }
    }

    void update(const AppState *as, const MemProfSample *samp) {
        KeyType key_addr = update_key(samp);
        EntType *ent = cam->lookup(key_addr);
        WSMDB(3)("WSM: %s update: %s -> %s\n", name.c_str(),
                 key_addr.fmt().c_str(),
                 (ent) ? (ent->fmt().c_str()) : "(null)");
        ++stats.updates_accepted;
        if (ent) {
            ent->touch_update(samp);
            ++stats.updates_touched;
        } else {
            const EntType *victim = cam->replace_probe(key_addr);
            if (victim && !victim->replace_ok()) {
                WSMDB(3)("WSM: %s replacement suppressed, victim: %s\n",
                         name.c_str(), victim->fmt().c_str());
                cam->spare_current_victim();
                ++stats.updates_noreplace;
            } else {
                ent = cam->replace(key_addr);
                this->ent_assign(ent, as, samp);
                ++stats.updates_replace;
            }
        }

        KeyType new_key;
        if (ent && postsample_rekey_hook(key_addr, ent, &new_key)) {
            if (!cam->have_key(new_key)) {
                WSMDB(3)("WSM: %s changing key(2), %s -> %s\n", name.c_str(),
                         key_addr.fmt().c_str(), new_key.fmt().c_str());
                cam->change_key(key_addr, new_key);
                key_addr = new_key;
            } else {
                WSMDB(3)("WSM: %s ignoring key-change, %s -> %s, "
                         "target key already exists\n", name.c_str(),
                         key_addr.fmt().c_str(), new_key.fmt().c_str());
            }
        }

        WSMDB(3)("WSM: %s updated: %s -> %s\n", name.c_str(),
                 key_addr.fmt().c_str(),
                 (ent) ? (ent->fmt().c_str()) : "(null)");
    }


    class CAMIterPrioSort {
        CAMConstIterType iter_;
        i64 forecast_;          // e.g. forecast # of useful blocks in window
        i64 prio_;
        int entry_id_;
    public:
        CAMIterPrioSort(const CAMConstIterType& iter, i64 forecast, i64 prio,
                        int entry_id)
            : iter_(iter), forecast_(forecast), prio_(prio),
              entry_id_(entry_id) { }
        const CAMConstIterType& get_iter() const { return iter_; }
        i64 get_forecast() const { return forecast_; }
        i64 get_prio() const { return prio_; }
        int get_entry_id() const { return entry_id_; }
        bool operator < (const CAMIterPrioSort& p2) const {
            return prio_ < p2.prio_;
        }
        bool operator > (const CAMIterPrioSort& p2) const {
            return prio_ > p2.prio_;
        }
    };
    struct CAMIterPrioReverseComp {      // reverse-priority compare functor
        bool operator() (const CAMIterPrioSort& c1,
                         const CAMIterPrioSort& c2) const {
            return c1 > c2;
        }
    };
    

    void summarize_and_expand(int for_app_id,
                              PrefetchCollector *pf_coll,
                              int *collector_queue_id,
                              long *xfer_bits_ret) {
        typedef vector<CAMIterPrioSort> PrioSortVec;
        PrioSortVec prio_sort;
        const CAMType *cam_raw = cam.get();     // to help pick const begin()
        const int forecast_window = conf_->forecast_window;

        int entry_index = -1;   // used to tag prefetches for reporting
        FOR_CONST_ITER(typename CAMType, *cam_raw, cam_iter) {
            const KeyType *key = cam_iter->first;
            const EntType *ent = cam_iter->second;
            ++entry_index;
            if (!this->key_matches_appid(*key, for_app_id)) {
                // skip this entry outright; it's for a different app
                continue;
            }
            i64 forecast, prio;
            this->ent_forecast(*(cam_iter->first), ent, forecast_window,
                               &forecast, &prio);
            // apply global & local scaling factors
            forecast = i64(ceil(forecast * conf_->forecast_scale_shared
                                * this->g_forecast_scale_local()));
            // apply any cutoffs
            if ((conf_->serials_since_hit_cutoff > 0) &&
                (ent->stats.ser_since_hit() >
                 conf_->serials_since_hit_cutoff)) {
                forecast = prio = 0;
            } else if (ent->stats.g_hits() <
                       MAX_SCALAR(conf_->min_hits_cutoff,
                                  this->g_min_hits_cutoff_local())) {
                forecast = prio = 0;
            }
            // store for use (after possible sorting)
            prio_sort.push_back(CAMIterPrioSort(cam_iter, forecast, prio,
                                                entry_index));
        }
        if (conf_->sort_by_forecast) {
            // sort entries in DEcreasing order of priority (i.e. best
            // forecast first)
            CAMIterPrioReverseComp comp;
            std::stable_sort(prio_sort.begin(), prio_sort.end(), comp);
        }

        WSMDB(1)("WSM: s_a_e %s; %d entries:\n", name.c_str(),
                 intsize(prio_sort));
        WSMDB(2)("  forecast scale factors: shared %g * local %g = %g\n",
                 conf_->forecast_scale_shared,
                 this->g_forecast_scale_local(),
                 conf_->forecast_scale_shared *
                 this->g_forecast_scale_local());

        // sketchy; effectively time-shares across tables, sorta
        i64 table_sim_clock = cyc;
        int entries_with_output = 0;
        int xfer_blocks_accum = 0;
        long xfer_bits_accum = 0;

        // visit entries, highest priority first (if sorting is enabled)
        FOR_CONST_ITER(typename PrioSortVec, prio_sort, prio_iter) {
            const CAMConstIterType& cam_iter(prio_iter->get_iter());
            i64 forecast = prio_iter->get_forecast();
            const int entry_id = prio_iter->get_entry_id();
            const KeyType *key = cam_iter->first;
            const EntType *ent = cam_iter->second;

            WSMDB(2)("  fc %s key[%s]: %s\n", fmt_i64(forecast),
                     key->fmt().c_str(), ent->fmt().c_str());
            if (WSM_DEBUG_COND(2) && this->uses_online_feedback()) {
                printf("    delta_s %s fc_l %s fc_r %s fc_h %s "
                       "ser_since_hit %s ser_since_win %s\n",
                       fmt_i64(ent->stats.delta_s()),
                       fmt_i64(ent->stats.forecast_wins_life(forecast_window)),
                       fmt_i64(ent->stats.
                               forecast_wins_recent(forecast_window)),
                       fmt_i64(ent->stats.
                               forecast_wins_history(forecast_window)),
                       fmt_i64(ent->stats.ser_since_hit()),
                       fmt_i64(ent->stats.ser_since_win()));
            }
            if (forecast >= conf_->forecast_min_blocks) {
                int prefetch_count =
                    MIN_SCALAR(forecast, conf_->max_prefetch_per_entry);
                int blocks_from_entry = -1;
                long bits_from_entry = -1;
                this->expand_entry(*key, *ent, entry_id,
                                   prefetch_count, table_sim_clock, 1,
                                   pf_coll, *collector_queue_id,
                                   &blocks_from_entry, &bits_from_entry);
                WSMDB(2)("WSM: key %s expanded to %d blocks, "
                         "billed %ld bits\n", key->fmt().c_str(),
                         blocks_from_entry, bits_from_entry);
                sim_assert(blocks_from_entry >= 0);
                sim_assert(bits_from_entry >= 0);
                xfer_blocks_accum += blocks_from_entry;
                xfer_bits_accum += bits_from_entry;
                ++(*collector_queue_id);
                if (blocks_from_entry > 0) {
                    ++entries_with_output;
                    stats.per_expand_entry.blocks.
                        add_sample(blocks_from_entry);
                    stats.per_expand_entry.bits.add_sample(bits_from_entry);
                }
            }
        }

        stats.total_blocks += xfer_blocks_accum;
        stats.total_bits += xfer_bits_accum;
        stats.per_expand.entries.add_sample(entries_with_output);
        stats.per_expand.blocks.add_sample(xfer_blocks_accum);
        stats.per_expand.bits.add_sample(xfer_bits_accum);
        *xfer_bits_ret = xfer_bits_accum;
    }

    void age_table() {
        FOR_ITER(typename CAMType, (*cam), cam_iter) {
            (cam_iter->second)->age_entry();
        }
    }

    void flush_one_app(int flush_app_id) {
        vector<KeyType> to_erase;
        const CAMType *cam_raw = cam.get();     // to help pick const begin()
        FOR_CONST_ITER(typename CAMType, *cam_raw, cam_iter) {
            const KeyType *key = cam_iter->first;
            if (this->key_matches_appid(*key, flush_app_id)) {
                to_erase.push_back(*key);       // value copy, to be safe
            }
        }
        FOR_CONST_ITER(typename vector<KeyType>, to_erase, key_iter) {
            cam->erase_entry(*key_iter);
        }
    }
};



struct NextBlockEnt : public EntryBase {
    int block_bytes;
    bool i_stream;              // flag: was created for i-stream accessess
    mem_addr orig_blk;          // block address at create
    mem_addr last_blk;          // most recently matched block address
    int steps_tried;            // #times keychange tried
    mem_addr change_to_addr;    // nonzero <=> key change needed, set to this

    struct {
        mem_addr next_addr;
    } predict;

    NextBlockEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    void block_align(mem_addr *addr) const { (*addr) &= ~(block_bytes - 1); }

    string fmt() const {
        ostringstream out;
        out << ((i_stream) ? "I" : "D") << " orig_blk " << fmt_mem(orig_blk)
            << " last_blk " << fmt_mem(last_blk) << " steps " << steps_tried
            << " " << stats.fmt();
        return out.str();
    }

    void touch_sample(const MemProfSample *samp) {
        // circuitously get postsample_rekey_hook() to change our key
        // to the next block
        last_blk = samp->addr.a;
        block_align(&last_blk);
        change_to_addr = last_blk + block_bytes;
    }
};


// "NextBlock": basically a non-correlated, block-stride-1 stride table,
// for I-stream, or D-stream, or both.  (This gets wrapped in
// NextBlockInstTab, NextBlockDataTab, NextBlockBothTab.)
class NextBlockTab : public TableWithDefaultOps<LongAddr,NextBlockEnt> {
    typedef LongAddr KeyType;
    typedef NextBlockEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    bool use_local_forecast_;

    bool uses_online_feedback() const { return true; }
    EntryExpandType entry_expand_type() const { return Expand_Infinite; }

    LongAddr sample_key(const MemProfSample *samp) const {
        LongAddr key_addr = samp->addr;
        block_align(&key_addr);
        // change: now we make this entry slide forward 1 block at each touch
        // key_addr.a -= block_bytes;
        return key_addr;
    }
    // LongAddr update_key(const MemProfSample *samp) const {
    //      LongAddr key_addr = samp->addr;
    //      block_align(&key_addr);
    //      return key_addr;
    // }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (int(key.id) == app_id);
    }

    // comes after ent->touch_sample(), update() calls
    bool postsample_rekey_hook(const KeyType& key, EntType *ent,
                               KeyType *new_entry_ret) {
        if (ent->change_to_addr == 0)
            return false;
        *new_entry_ret = key;
        new_entry_ret->a = ent->change_to_addr;
        ent->change_to_addr = 0;
        block_align(new_entry_ret);
        ++(ent->steps_tried);
        return true;
    }

    void get_expand_entry_parms(const LongAddr& key, const NextBlockEnt& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(key.id,
                    64,         // VA, run-len
                    0,          // delta-VA
                    (ent.i_stream) ? PFT_Inst : PFT_Data,
                    false);
    }

    void ent_assign(NextBlockEnt *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->block_bytes = block_bytes;
        ent->i_stream = (samp->op_type == MPO_InstFetch);
        ent->orig_blk = samp->addr.a;
        block_align(&ent->orig_blk);
        ent->last_blk = ent->orig_blk;
        ent->steps_tried = 0;
        ent->change_to_addr = 0;
    }
    void ent_init_predict(const LongAddr& key, NextBlockEnt *ent) const {
        ent->predict.next_addr = key.a - block_bytes;
    }
    bool ent_can_predict(const LongAddr& key, const NextBlockEnt *ent) const {
        return ent->predict.next_addr != 0;
    }
    mem_addr ent_step_predict(const LongAddr& key, NextBlockEnt *ent) const {
        mem_addr result = ent->predict.next_addr;
        ent->predict.next_addr += block_bytes;
        return result;
    }

    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        if (use_local_forecast_) {
            if (key.a == ent->last_blk) {
                // this entry was unable to shift its key to the next block up,
                // at its last update.  we'll infer that there's another entry
                // describing the blocks immediately following, and so
                // limit the sequence length to 1.
                *forecast_count_ret = 1;
            } else {
                // +1: count never-hit-on initial block (since key address
                // shifts up on hit)
                *forecast_count_ret = ent->stats.g_hits() + 1;
            }
            *priority_ret = *forecast_count_ret;
        } else {
            DefaultOps::ent_forecast(key, ent, forecast_window,
                                     forecast_count_ret, priority_ret);
        }
    }

public:
    NextBlockTab(const WSM_Config *conf__, string table_name,
                 int table_id_, const string& cfg, int default_size,
                 int block_bytes_lg_)
        : DefaultOps(conf__, table_name, table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        use_local_forecast_ = conf_bool(cfg + "/use_local_forecast");
    }

    virtual bool sample_accept(const MemProfSample *samp) const {
        abort_printf("this sample_accept() needs to get redefined by child\n");
        return false;
    }
};


class NextBlockInstTab : public NextBlockTab {
public:
    NextBlockInstTab(const WSM_Config *conf__, int table_id_,
                     const string& cfg, int default_size, int block_bytes_lg_)
        : NextBlockTab(conf__, "NextBlockInst",
                       table_id_, cfg, default_size, block_bytes_lg_)
    { }
    bool sample_accept(const MemProfSample *samp) const {
        return (samp->op_type == MPO_InstFetch);
    }
};


class NextBlockDataTab : public NextBlockTab {
public:
    NextBlockDataTab(const WSM_Config *conf__, int table_id_,
                     const string& cfg, int default_size, int block_bytes_lg_)
        : NextBlockTab(conf__, "NextBlockData",
                       table_id_, cfg, default_size, block_bytes_lg_)
    { }
    bool sample_accept(const MemProfSample *samp) const {
        return (samp->op_type != MPO_InstFetch);
    }
};


class NextBlockBothTab : public NextBlockTab {
public:
    NextBlockBothTab(const WSM_Config *conf__, int table_id_,
                     const string& cfg, int default_size, int block_bytes_lg_)
        : NextBlockTab(conf__, "NextBlockBoth",
                       table_id_, cfg, default_size, block_bytes_lg_)
    { }
    bool sample_accept(const MemProfSample *samp) const {
        return true;
    }
};



struct StridePCEnt : public EntryBase {
    bool i_stream;              // flag: was created for i-stream accessess
    mem_addr orig_addr;         // initial miss address at create
    // 0: invalid addrs
    i64 prev_addr;
    i64 prev_prev_addr;

    StridePCEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    string fmt() const {
        ostringstream out;
        out << ((i_stream) ? "I" : "D") << " orig " << fmt_mem(orig_addr)
            << " p " << fmt_mem(prev_addr) << " " << fmt_mem(prev_prev_addr)
            << " (delta " << (prev_addr - prev_prev_addr) << ") "
            << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        bool result = false;
        if (prev_prev_addr) {
            i64 test_stride = prev_addr - prev_prev_addr;
            if (((test_stride != 0) || !conf_->ignore_zero_stride) &&
                ((prev_addr + test_stride) == i64(samp->addr.a))) {
                result = true;
            }
        }
        return result;
    }
    bool have_nz_stride() const {
        return (prev_prev_addr != 0) && (prev_prev_addr != prev_addr);
    }
    i64 g_stride() const {
        return (prev_prev_addr) ? (prev_addr - prev_prev_addr) : 0;
    }
    void touch_sample(const MemProfSample *samp) {
        sim_assert(samp->addr.a != 0);
        if ((i64(samp->addr.a) != prev_addr) ||
            !conf_->ignore_zero_stride) {
            prev_prev_addr = prev_addr;
            prev_addr = samp->addr.a;
        }
    }
};


class StridePCTab : public TableWithDefaultOps<LongAddr,StridePCEnt> {
    typedef LongAddr KeyType;
    typedef StridePCEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    bool accept_ifetch_;

    bool uses_online_feedback() const { return true; }
    EntryExpandType entry_expand_type() const { return Expand_Infinite; }

    LongAddr sample_key(const MemProfSample *samp) const {
        LongAddr key_addr = samp->addr;
        key_addr.a = samp->pc;
        return key_addr;
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (int(key.id) == app_id);
    }

    void get_expand_entry_parms(const LongAddr& key, const StridePCEnt& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(key.id,
                    64,       // VA + stride + count
                    0,        // no per-block extra
                    (ent.i_stream) ? PFT_Inst : PFT_Data,
                    false);
    }
    void ent_assign(StridePCEnt *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->i_stream = (samp->op_type == MPO_InstFetch);
        sim_assert(samp->addr.a != 0);
        ent->orig_addr = samp->addr.a;
        ent->prev_prev_addr = 0;
        ent->prev_addr = ent->orig_addr;
    }
    bool ent_can_predict(const LongAddr& key, const StridePCEnt *ent) const {
        return ent->have_nz_stride();
    }
    mem_addr ent_step_predict(const LongAddr& key, StridePCEnt *ent) const {
        i64 stride = ent->prev_addr - ent->prev_prev_addr;
        mem_addr result = ent->prev_addr;       // (re-fetch at new core)
        ent->prev_prev_addr = ent->prev_addr;
        ent->prev_addr += stride;
        return result;
    }

public:
    StridePCTab(const WSM_Config *conf__, int table_id_, const string& cfg,
                int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "StridePC", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        accept_ifetch_ = conf_bool(cfg + "/accept_ifetch");
    }

    bool sample_accept(const MemProfSample *samp) const {
        return ((samp->pc) != 0) &&
            (accept_ifetch_ || (samp->op_type != MPO_InstFetch)) ;
    }
};



struct SameObjEnt : public EntryBase {
    int min_match_span_bytes;         // (parent min_block_span, in bytes)

    int min_offset, max_offset;         // note: max is INclusive
    int last_offset, last_regnum;
    i64 touches;

    struct {
        int next_offset;
    } predict;

    SameObjEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    string fmt() const {
        ostringstream out;
        out << "[" << min_offset << "," << max_offset << "] last "
            << last_offset << " r" << last_regnum << " touches " << touches
            << " " << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        // (is called before "touch"
        bool result = false;
        int new_min = MIN_SCALAR(min_offset, samp->offset);
        int new_max = MAX_SCALAR(max_offset, samp->offset);
        int new_span_bytes = (new_max + 1) - new_min;
        result = (new_span_bytes >= min_match_span_bytes);
        return result;
    }
    void touch_sample(const MemProfSample *samp) {
        int offset = samp->offset;
        if (offset < min_offset) min_offset = offset;
        if (offset > max_offset) max_offset = offset;
        last_offset = offset;
        last_regnum = samp->addr_regnum;
        touches++;
    }
};


class SameObjTab : public TableWithDefaultOps<LongAddr,SameObjEnt> {
    typedef LongAddr KeyType;
    typedef SameObjEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;
    
    bool prefetch_from_zero_;
    int min_block_span_;

    bool uses_online_feedback() const { return true; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    LongAddr sample_key(const MemProfSample *samp) const {
        return LongAddr(samp->addr_regval, samp->addr.id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (int(key.id) == app_id);
    }

    void get_expand_entry_parms(const LongAddr& key, const SameObjEnt& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(key.id,
                    64,        // VA + min/max offset
                    0,
                    PFT_Data,
                    false);
    }
    void ent_assign(SameObjEnt *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->min_match_span_bytes = min_block_span_ * block_bytes;
        ent->min_offset = ent->max_offset = ent->last_offset = samp->offset;
        ent->last_regnum = samp->addr_regnum;
        ent->touches = 1;
    }
    void ent_init_predict(const LongAddr& key, SameObjEnt *ent) const {
        // c.f. ent_forecast()
        int start_offset = (prefetch_from_zero_) ? 0 : ent->min_offset;
        ent->predict.next_offset = start_offset;
    }
    bool ent_can_predict(const LongAddr& key, const SameObjEnt *ent) const {
        return ent->predict.next_offset <= ent->max_offset;
    }
    mem_addr ent_step_predict(const LongAddr& key, SameObjEnt *ent) const {
        mem_addr result = key.a + ent->predict.next_offset;
        ent->predict.next_offset += block_bytes;
        return result;
    }

    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        int start_offset = (prefetch_from_zero_) ? 0 : ent->min_offset;
        int spanned_blocks = 
            i64(ceil(double((ent->max_offset + 1) - start_offset)
                     / block_bytes));
        // use spanned_blocks, but clamp at hit-count to keep us from going
        // crazy on a wide (but mostly useless) span
        i64 pf_blocks = MIN_SCALAR(spanned_blocks, ent->touches);
        if (spanned_blocks < min_block_span_)
            pf_blocks = 0;      // reject tiny ranges
        *forecast_count_ret = pf_blocks;
        *priority_ret = *forecast_count_ret;
    }

public:
    SameObjTab(const WSM_Config *conf__, int table_id_, const string& cfg,
               int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "SameObj", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        prefetch_from_zero_ = conf_bool(cfg + "/prefetch_from_zero");
        min_block_span_ = conf_int(cfg + "/min_block_span");
        sim_assert(min_block_span_ >= 0);
    }

    bool sample_accept(const MemProfSample *samp) const {
        int addr_reg = samp->addr_regnum;
        return (samp->op_type != MPO_InstFetch) &&
            !IS_ZERO_REG(addr_reg) && (addr_reg != AlphaReg_gp) &&
            (addr_reg != AlphaReg_sp) && (samp->offset >= 0);
    }
};



struct PointerEnt : public EntryBase {
    mem_addr load_pc;           // producer load PC
    mem_addr loaded_from;       // address producer loaded from; 0: invalid
    i64 pc_runlen;              // # of touches since producer ID changed
    i64 addr_runlen;
    struct {
        int step_num;           // 0: next is ptr val, 1: next is src, 2: done
    } predict;

    PointerEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    string fmt() const {
        ostringstream out;
        out << "pc " << fmt_mem(load_pc) << " (run " << pc_runlen << ")"
            << " from " << fmt_mem(loaded_from) 
            << " (run " << addr_runlen << ")"
            << " " << stats.fmt();
        return out.str();
    }
    void touch_update(const MemProfSample *samp) {
        pc_runlen = (load_pc == samp->pc) ? (pc_runlen + 1) : 1;
        addr_runlen = (loaded_from == samp->addr.a) ? (addr_runlen + 1) : 1;
        load_pc = samp->pc;
        loaded_from = samp->addr.a;
    }
};


class PointerTab : public TableWithDefaultOps<LongAddr,PointerEnt> {
    typedef LongAddr KeyType;
    typedef PointerEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;
    
    bool prefetch_source_also_;

    bool uses_online_feedback() const { return true; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    LongAddr sample_key(const MemProfSample *samp) const {
        return LongAddr(samp->addr_regval, samp->addr.id);
    }
    LongAddr update_key(const MemProfSample *samp) const {
        return LongAddr(samp->data_regval, samp->addr.id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (int(key.id) == app_id);
    }

    void get_expand_entry_parms(const LongAddr& key, const PointerEnt& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(key.id,
                    32,        // VA
                    (prefetch_source_also_) ? 32 : 0,        // delta-VA
                    PFT_Data,
                    false);
    }
    void ent_assign(PointerEnt *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->load_pc = samp->pc;
        ent->loaded_from = samp->addr.a;
        ent->pc_runlen = 1;
        ent->addr_runlen = 1;
    }
    void ent_init_predict(const LongAddr& key, PointerEnt *ent) const {
        ent->predict.step_num = 0;
    }
    bool ent_can_predict(const LongAddr& key, const PointerEnt *ent) const {
        return ent->predict.step_num < 2;
    }
    mem_addr ent_step_predict(const LongAddr& key, PointerEnt *ent) const {
        mem_addr result = 0;
        switch (ent->predict.step_num) {
        case 0:
            result = key.a;
            ent->predict.step_num = (prefetch_source_also_) ? 1 : 2;
            break;
        case 1:
            result = ent->loaded_from;
            ent->predict.step_num = 2;
            break;
        }
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = (prefetch_source_also_) ? 2 : 1;
        *priority_ret = *forecast_count_ret;
    }

public:
    PointerTab(const WSM_Config *conf__, int table_id_, const string& cfg,
               int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "Pointer", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        prefetch_source_also_ = conf_bool(cfg + "/prefetch_source_also");
    }

    bool sample_accept(const MemProfSample *samp) const {
        return !IS_ZERO_REG(samp->addr_regnum);
    }
    bool update_accept(const MemProfSample *samp) const {
        return (samp->op_type == MPO_DataLoad) &&
            !IS_ZERO_REG(samp->data_regnum) && 
            (samp->width == 8) &&
            looks_like_pointer(samp->data_regval);
    }
};



struct PointerChaseEnt : public EntryBase {
    int steps_tried;            // #times keychange tried
    mem_addr loaded_from;       // address pointer value loaded from
    mem_addr change_to_addr;    // nonzero <=> key change needed, set to this

    struct {
        mem_addr next_ptr_addr;
        ProgMem *parent_pmem;   // or NULL
    } predict;

    PointerChaseEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    string fmt() const {
        ostringstream out;
        out << "loaded_from " << fmt_mem(loaded_from)
            << " steps " << steps_tried << " " << stats.fmt();
        return out.str();
    }

    void touch_sample(const MemProfSample *samp) {
        // circuitously get postsample_rekey_hook() to change our key
        // to the pointer value loaded, if it can
        loaded_from = samp->addr.a;
        change_to_addr = samp->data_regval;
    }
};


// "PointerChase": like NextBlockTab, but detects walks along a pointer chain
class PointerChaseTab : public TableWithDefaultOps<LongAddr,PointerChaseEnt> {
    typedef LongAddr KeyType;
    typedef PointerChaseEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    bool use_local_forecast_;

    bool uses_online_feedback() const { return true; }
    EntryExpandType entry_expand_type() const { return Expand_Infinite; }

    LongAddr sample_key(const MemProfSample *samp) const {
        LongAddr key_addr = samp->addr;
        return key_addr;
    }
    LongAddr update_key(const MemProfSample *samp) const {
        return LongAddr(samp->data_regval, samp->addr.id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (int(key.id) == app_id);
    }

    // comes after ent->touch_sample(), update() calls
    bool postsample_rekey_hook(const KeyType& key, EntType *ent,
                               KeyType *new_entry_ret) {
        if (ent->change_to_addr == 0)
            return false;
        *new_entry_ret = key;
        new_entry_ret->a = ent->change_to_addr;
        ent->change_to_addr = 0;
        ++(ent->steps_tried);
        return true;
    }

    void get_expand_entry_parms(const LongAddr& key, const EntType& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(key.id,
                    64,         // VA, chase distance
                    0,          // delta-VA
                    PFT_Data,
                    true);      // R-A-W dependence between prefetches
    }

    void ent_assign(EntType *ent, const AppState *as, 
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->loaded_from = samp->addr.a;
        ent->steps_tried = 0;
        ent->change_to_addr = 0;
    }
    void ent_init_predict(const LongAddr& key, PointerChaseEnt *ent) const {
        AppState *parent_app = appstate_lookup_id(key.id);
        if (parent_app) {
            // ent->predict.next_ptr_addr = key.a;
            ent->predict.next_ptr_addr = ent->loaded_from;
            ent->predict.parent_pmem = parent_app->pmem;
        } else {
            ent->predict.next_ptr_addr = 0;
            ent->predict.parent_pmem = NULL;
        }
    }
    bool ent_can_predict(const LongAddr& key,
                         const PointerChaseEnt *ent) const {
        return ent->predict.next_ptr_addr != 0;
    }
    mem_addr ent_step_predict(const LongAddr& key,
                              PointerChaseEnt *ent) const {
        mem_addr result = ent->predict.next_ptr_addr;
        sim_assert(ent->predict.parent_pmem != NULL);
        ent->predict.next_ptr_addr = pmem_read_64(ent->predict.parent_pmem, 
                                                  ent->predict.next_ptr_addr,
                                                  PMAF_R | PMAF_NoExcept);
        if (!looks_like_pointer(ent->predict.next_ptr_addr)) {
            // no more after this one
            ent->predict.next_ptr_addr = 0;
        }
        return result;
    }

    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        if (use_local_forecast_) {
            if (key.a == ent->loaded_from) {
                // this entry was unable to shift its key to the pointer
                // target at its last update.  we'll infer that there's
                // another entry describing the chain starting there, and so
                // limit the sequence length to 1.
                *forecast_count_ret = 1;
            } else {
                *forecast_count_ret = ent->stats.g_hits() + 1;
            }
            *priority_ret = *forecast_count_ret;
        } else {
            DefaultOps::ent_forecast(key, ent, forecast_window,
                                     forecast_count_ret, priority_ret);
        }
    }

public:
    PointerChaseTab(const WSM_Config *conf__, int table_id_, const string& cfg,
                    int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "PointerChase", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        use_local_forecast_ = conf_bool(cfg + "/use_local_forecast");
    }

    bool sample_accept(const MemProfSample *samp) const {
        return (samp->op_type == MPO_DataLoad) &&
            !IS_ZERO_REG(samp->data_regnum) && 
            (samp->width == 8) &&
            looks_like_pointer(samp->data_regval);
    }
};



struct BTBEnt : public EntryBase {
    mem_addr br_pc;             // branch PC
    struct {
        int step_num;           // 0: next is target, 1: next is src, 2: done
    } predict;

    BTBEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    string fmt() const {
        ostringstream out;
        out << "br_pc " << fmt_mem(br_pc)
            << " " << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        return (samp->pc == br_pc);
    }
    void touch_sample(const MemProfSample *samp) {
        br_pc = samp->pc;
    }
};


class BTBTab : public TableWithDefaultOps<LongAddr,BTBEnt> {
    typedef LongAddr KeyType;
    typedef BTBEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    bool prefetch_branch_also_;

    bool uses_online_feedback() const { return true; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    LongAddr sample_key(const MemProfSample *samp) const {
        return samp->addr;
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (int(key.id) == app_id);
    }

    void get_expand_entry_parms(const LongAddr& key, const BTBEnt& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(key.id,
                    32,        // VA
                    (prefetch_branch_also_) ? 32 : 0,        // delta-VA
                    PFT_Inst,
                    false);
    }
    void ent_assign(BTBEnt *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->br_pc = samp->pc;
    }
    void ent_init_predict(const LongAddr& key, BTBEnt *ent) const {
        ent->predict.step_num = 0;
    }
    bool ent_can_predict(const LongAddr& key, const BTBEnt *ent) const {
        return ent->predict.step_num < 2;
    }
    mem_addr ent_step_predict(const LongAddr& key, BTBEnt *ent) const {
        mem_addr result = 0;
        switch (ent->predict.step_num) {
        case 0:
            result = key.a;
            ent->predict.step_num = (prefetch_branch_also_) ? 1 : 2;
            break;
        case 1:
            result = ent->br_pc;
            ent->predict.step_num = 2;
            break;
        }
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = (prefetch_branch_also_) ? 2 : 1;
        *priority_ret = *forecast_count_ret;
    }

public:
    BTBTab(const WSM_Config *conf__, int table_id_, const string& cfg,
           int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "BTB", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        prefetch_branch_also_ = conf_bool(cfg + "/prefetch_branch_also");
    }

    bool sample_accept(const MemProfSample *samp) const {
        return (samp->op_type == MPO_InstFetch) && samp->pc;
    }
};



struct BlockBTBEnt : public EntryBase {
    mem_addr br_pc_blk;         // block-aligned branch PC
    int block_bytes;
    struct {
        int step_num;           // 0: next is target, 1: next is src, 2: done
    } predict;

    BlockBTBEnt(const WSM_Config *conf__)
        : EntryBase(conf__), block_bytes(0) { }

    mem_addr btb_block_align(mem_addr a) const {
        sim_assert(block_bytes > 0);
        return a & ~(block_bytes - 1);
    }

    string fmt() const {
        ostringstream out;
        out << "br_pc_blk " << fmt_mem(br_pc_blk)
            << " " << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        return (btb_block_align(samp->pc) == br_pc_blk);
    }
    void touch_sample(const MemProfSample *samp) {
        br_pc_blk = btb_block_align(samp->pc);
    }
};


class BlockBTBTab : public TableWithDefaultOps<LongAddr,BlockBTBEnt> {
    typedef LongAddr KeyType;
    typedef BlockBTBEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    bool prefetch_branch_also_;

    bool uses_online_feedback() const { return true; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    LongAddr sample_key(const MemProfSample *samp) const {
        LongAddr result = samp->addr;
        block_align(&result);
        return result;
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (int(key.id) == app_id);
    }

    void get_expand_entry_parms(const LongAddr& key, const BlockBTBEnt& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(key.id,
                    32,        // VA
                    (prefetch_branch_also_) ? 32 : 0,        // delta-VA
                    PFT_Inst,
                    false);
    }
    void ent_assign(BlockBTBEnt *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->block_bytes = this->block_bytes;
        ent->br_pc_blk = samp->pc;
        block_align(&ent->br_pc_blk);
    }
    void ent_init_predict(const LongAddr& key, BlockBTBEnt *ent) const {
        ent->predict.step_num = 0;
    }
    bool ent_can_predict(const LongAddr& key, const BlockBTBEnt *ent) const {
        return ent->predict.step_num < 2;
    }
    mem_addr ent_step_predict(const LongAddr& key, BlockBTBEnt *ent) const {
        mem_addr result = 0;
        switch (ent->predict.step_num) {
        case 0:
            result = key.a;
            block_align(&result);
            ent->predict.step_num = (prefetch_branch_also_) ? 1 : 2;
            break;
        case 1:
            result = ent->br_pc_blk;
            ent->predict.step_num = 2;
            break;
        }
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = (prefetch_branch_also_) ? 2 : 1;
        *priority_ret = *forecast_count_ret;
    }

public:
    BlockBTBTab(const WSM_Config *conf__, int table_id_, const string& cfg,
                int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "BlockBTB", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        prefetch_branch_also_ = conf_bool(cfg + "/prefetch_branch_also");
    }

    bool sample_accept(const MemProfSample *samp) const {
        return (samp->op_type == MPO_InstFetch) && samp->pc;
    }
};


// PCWindow: store the most recent PC seen, and prefetch a fixed-size window
// of nearby blocks.
struct PCWindowEnt : public EntryBase {
    mem_addr created_min_blk;   // first block touched by window (at create)
    mem_addr created_max_blk;   // one past last touched by create (exclusive)

    mem_addr pc;        // most recent PC
    int master_id;      // address-space ID for this app
    struct {
        int num_blocks; // #blocks predicted so far
    } predict;

    PCWindowEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    string fmt() const {
        ostringstream out;
        out << "win [" << fmt_mem(created_min_blk) 
            << "," << fmt_mem(created_max_blk) <<")"
            << " pc " << fmt_mem(pc)
            << " " << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        // experimental
        // (samp->pc >= created_min_blk) && (samp->pc < created_max_blk);
        return true;
    }
    void touch_sample(const MemProfSample *samp) {
        pc = samp->pc;
    }
};


class PCWindowTab : public TableWithDefaultOps<TableKey_Int,PCWindowEnt> {
    typedef TableKey_Int KeyType;
    typedef PCWindowEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    int window_size_;           // #blocks to prefetch
    int window_offset_;         // start PF at align(PC) plus this many blocks
 
    bool uses_online_feedback() const { return false; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    KeyType sample_key(const MemProfSample *samp) const {
        return KeyType(samp->app_id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (key.id == app_id);
    }

    void get_expand_entry_parms(const KeyType& key, const EntType& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(ent.master_id,
                    64,         // VA + block count
                    0,          // delta-VA
                    PFT_Inst,
                    false);
    }
    void ent_assign(EntType *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->created_min_blk = samp->pc - (block_bytes * window_offset_);
        block_align(&ent->created_min_blk);
        ent->created_max_blk = ent->created_min_blk +
            (block_bytes * window_size_ - 1);   // last _byte_ in window
        block_align(&ent->created_max_blk);
        ent->created_max_blk += block_bytes;    // start of one block past
        ent->pc = samp->pc;
        ent->master_id = samp->addr.id;
    }
    void ent_init_predict(const KeyType& key, EntType *ent) const {
        ent->predict.num_blocks = 0;
        this->block_align(&ent->pc);
        int pf_min_offset = window_offset_;
        int pf_max_offset = window_offset_ + window_size_;      // exclusive
        if ((pf_min_offset < 0) && (pf_max_offset > 0)) {
            // we'll PF forward from offset 0, then wrap around
        } else {
            ent->pc += window_offset_ * this->block_bytes;
        }
    }
    bool ent_can_predict(const KeyType& key, const EntType *ent) const {
        return (ent->predict.num_blocks < window_size_) &&
            (ent->pc != 0);
    }
    mem_addr ent_step_predict(const KeyType& key, EntType *ent) const {
        mem_addr result;
        result = ent->pc;
        ent->pc += this->block_bytes;
        ++(ent->predict.num_blocks);
        int pf_min_offset = window_offset_;
        int pf_max_offset = window_offset_ + window_size_;      // exclusive
        if ((pf_min_offset < 0) && (pf_max_offset > 0) &&
            (ent->predict.num_blocks == pf_max_offset)) {
            // wrap around to negative offsets
            ent->pc -= window_size_ * this->block_bytes;
        }
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = (ent->pc) ? (window_size_) : 0;
        *priority_ret = *forecast_count_ret;
    }


public:
    PCWindowTab(const WSM_Config *conf__, int table_id_, const string& cfg,
                int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "PCWindow", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        window_size_ = conf_int(cfg + "/window_size");
        if (window_size_ < 1) {
            exit_printf("PCWindow: bad window_size %d\n", window_size_);
        }
        window_offset_ = conf_int(cfg + "/window_offset");
    }

    bool sample_accept(const MemProfSample *samp) const {
        return samp->pc != 0;
    }
};


// SPWindow: like PCWindow, but for the stack pointer; store the most recent
// SP seen, and prefetch a fixed-size window of nearby blocks.
struct SPWindowEnt : public EntryBase {
    mem_addr sp;        // most recently seen stack pointer
    int master_id;      // address-space ID for this app
    struct {
        int num_blocks; // #blocks predicted so far
    } predict;

    SPWindowEnt(const WSM_Config *conf__) : EntryBase(conf__) { }

    string fmt() const {
        ostringstream out;
        out << "sp " << fmt_mem(sp)
            << " " << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        return true;
    }
    void touch_sample(const MemProfSample *samp) {
        sp = (samp->addr_regnum == AlphaReg_sp) ? samp->addr_regval
            : samp->data_regval;
    }
};


class SPWindowTab : public TableWithDefaultOps<TableKey_Int,SPWindowEnt> {
    typedef TableKey_Int KeyType;
    typedef SPWindowEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    int window_size_;           // #blocks to prefetch
    int window_offset_;         // start PF at align(SP) plus this many blocks

    bool uses_online_feedback() const { return false; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    KeyType sample_key(const MemProfSample *samp) const {
        return KeyType(samp->app_id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (key.id == app_id);
    }

    void get_expand_entry_parms(const KeyType& key, const EntType& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(ent.master_id,
                    64,         // VA + block count
                    0,          // delta-VA
                    PFT_Data,
                    false);
    }
    void ent_assign(EntType *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->master_id = samp->addr.id;
        ent->touch_sample(samp);
    }
    void ent_init_predict(const KeyType& key, EntType *ent) const {
        ent->predict.num_blocks = 0;
        this->block_align(&ent->sp);
        int pf_min_offset = window_offset_;
        int pf_max_offset = window_offset_ + window_size_;      // exclusive
        if ((pf_min_offset < 0) && (pf_max_offset > 0)) {
            // we'll PF forward from offset 0, then wrap around
        } else {
            ent->sp += window_offset_ * this->block_bytes;
        }
    }
    bool ent_can_predict(const KeyType& key, const EntType *ent) const {
        return (ent->predict.num_blocks < window_size_) && (ent->sp != 0);
    }
    mem_addr ent_step_predict(const KeyType& key, EntType *ent) const {
        mem_addr result;
        result = ent->sp;
        ent->sp += this->block_bytes;
        ++(ent->predict.num_blocks);
        int pf_min_offset = window_offset_;
        int pf_max_offset = window_offset_ + window_size_;      // exclusive
        if ((pf_min_offset < 0) && (pf_max_offset > 0) &&
            (ent->predict.num_blocks == pf_max_offset)) {
            // wrap around to negative offsets
            ent->sp -= window_size_ * this->block_bytes;
        }
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = (ent->sp) ? (window_size_) : 0;
        *priority_ret = *forecast_count_ret;
    }

public:
    SPWindowTab(const WSM_Config *conf__, int table_id_, const string& cfg,
                int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "SPWindow", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        window_size_ = conf_int(cfg + "/window_size");
        if (window_size_ < 1) {
            exit_printf("SPWindow: bad window_size %d\n", window_size_);
        }
        window_offset_ = conf_int(cfg + "/window_offset");
    }

    bool sample_accept(const MemProfSample *samp) const {
        return (samp->addr_regnum == AlphaReg_sp) ||
            (samp->data_regnum == AlphaReg_sp);
    }
};


// RetStack: maintain a shadow copy of the return stack, as best we can
struct RetStackEnt : public EntryBase {
    const AppState *parent_as;
    int max_stack_size;
    int master_id;
    list<mem_addr> frames;      // stores (PC+4) at call; back is most recent
    struct {
        int blocks_this_lev;    // #blocks predicted at this level
        mem_addr prev_addr;     // most recent addr predicted
    } predict;

    RetStackEnt(const WSM_Config *conf__)
        : EntryBase(conf__), parent_as(NULL), max_stack_size(0) { }

    string fmt() const {
        ostringstream out;
        out << "frames:";
        FOR_CONST_ITER(list<mem_addr>, frames, iter) {
            out << " " << fmt_mem(*iter);
        }
        out << "; " << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        return true;
    }
    void touch_sample(const MemProfSample *samp) {
        // Reminder: CAM hit implies that address spaces matched, so previous
        // AppState pointer should be fine.  Also, we should have valid PC,
        // care of sample_accept() filter
        sim_assert((parent_as != NULL) && (parent_as->app_id == samp->app_id));
        // this is cheating, but only a teensy bit: it wouldn't hard at all
        // to stuff these ~2 bits into MemProfSample
        const StashData *decode_info = stash_decode_inst(parent_as->stash,
                                                         samp->pc);
        sim_assert(decode_info != NULL);
        if (SBF_ReadsRetStack(decode_info->br_flags)) {
            if (!frames.empty()) {
                frames.pop_back();
            }
        }
        if (SBF_WritesRetStack(decode_info->br_flags)) {
            frames.push_back(samp->pc + kInstSizeBytes);
            while (intsize(frames) >= max_stack_size) {
                frames.pop_front();
            }
        }
    }
};


class RetStackTab : public TableWithDefaultOps<TableKey_Int,RetStackEnt> {
    typedef TableKey_Int KeyType;
    typedef RetStackEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    int max_stack_size_;        // #entries (PCs) in return stack
    int pf_levels_;             // #stack levels to consider for prefetch
    int window_size_;           // #blocks to prefetch in each frame
    int window_offset_;         // offset PF window by this many blocks

    bool uses_online_feedback() const { return false; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    KeyType sample_key(const MemProfSample *samp) const {
        return KeyType(samp->app_id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (key.id == app_id);
    }

    void get_expand_entry_parms(const KeyType& key, const EntType& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(ent.master_id,
                    intsize(ent.frames) * 64,   // per-frame VA + block count
                    0,                 // delta-VA
                    PFT_Inst,
                    false);
    }
    void ent_assign(EntType *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->parent_as = as;
        ent->max_stack_size = max_stack_size_;
        ent->master_id = samp->addr.id;
        ent->frames.clear();
        ent->touch_sample(samp);        // may or may not update "frames"
    }
    void ent_init_predict(const KeyType& key, EntType *ent) const {
        ent->predict.prev_addr = 0;
        ent->predict.blocks_this_lev = window_size_; // force frame stack pop
        while (intsize(ent->frames) > pf_levels_) {
            // discard any frames outside the desired level
            ent->frames.pop_front();
        }
    }
    bool ent_can_predict(const KeyType& key, const EntType *ent) const {
        return (ent->predict.blocks_this_lev < window_size_) ||
            !ent->frames.empty();
    }
    mem_addr ent_step_predict(const KeyType& key, EntType *ent) const {
        mem_addr result;
        if (ent->predict.blocks_this_lev < window_size_) {
            ent->predict.prev_addr += this->block_bytes;
            ++(ent->predict.blocks_this_lev);
        } else {
            ent->predict.blocks_this_lev = 0;
            ent->predict.prev_addr = pop_back_ret(ent->frames);
            this->block_align(&ent->predict.prev_addr);
            ent->predict.prev_addr += window_offset_ * this->block_bytes;
        }
        result = ent->predict.prev_addr;
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = 
            MIN_SCALAR(intsize(ent->frames), pf_levels_) * window_size_;
        *priority_ret = *forecast_count_ret;
    }

public:
    RetStackTab(const WSM_Config *conf__, int table_id_, const string& cfg,
                int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "RetStack", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        max_stack_size_ = conf_int(cfg + "/max_stack_size");
        if (max_stack_size_ < 1) {
            exit_printf("RetStack: bad max_stack_size %d\n",
                        max_stack_size_);
        }
        pf_levels_ = conf_int(cfg + "/pf_levels");
        if (pf_levels_ < 1) {
            exit_printf("RetStack: bad pf_levels %d\n", pf_levels_);
        }
        window_size_ = conf_int(cfg + "/window_size");
        if (window_size_ < 1) {
            exit_printf("RetStack: bad window_size %d\n", window_size_);
        }
        window_offset_ = conf_int(cfg + "/window_offset");
    }

    bool sample_accept(const MemProfSample *samp) const {
        // filter: we want only taken branches; EntType::touch_sample()
        // will figure out call/ret/etc, later.
        return (samp->op_type == MPO_InstFetch) &&
            (samp->pc != 0);
    }
};


// InstMRU: track N most recently used I-stream blocks
// DataMRU: track N most recently used D-stream blocks
struct MRUEnt : public EntryBase {
    int window_size_bytes;    // POWER OF 2
    int max_mru_windows;
    i64 sample_hits;    // touch_sample() calls which matched an existing entry
    i64 sample_misses;  // touch_sample() calls which did not
    int master_id;

    typedef list<mem_addr> MRUAddrList;
    typedef map<mem_addr,MRUAddrList::iterator> MRUAddrMap;
    MRUAddrList mru_order;      // WINDOW addrs (first is MRU, last is LRU)
    MRUAddrMap mru_to_order;    // WINDOW addr -> iter in mru_order list

    struct {
        // #blocks already predicted in MRU window (front of mru_order)
        int blocks_used_this_window;
    } predict;

    MRUEnt(const WSM_Config *conf__)
        : EntryBase(conf__), window_size_bytes(0), max_mru_windows(0) { }
    
    mem_addr mru_window_align(mem_addr a) const {
        sim_assert(window_size_bytes > 0);
        return a & ~(window_size_bytes - 1);
    }

    string fmt() const {
        ostringstream out;
        char missrate[20];
        e_snprintf(missrate, sizeof(missrate), "%.2f",
                   100 * double(sample_misses) /
                   (sample_hits + sample_misses));
        out << "samples: " << (sample_hits + sample_misses)
            << " (mr " << missrate << "%)"
            << " windows:";
        FOR_CONST_ITER(MRUAddrList, mru_order, iter) {
            out << " " << fmt_mem(*iter);
        }
        out << "; " << stats.fmt();
        return out.str();
    }
    bool is_match(const MemProfSample *samp) const {
        mem_addr base_addr = mru_window_align(samp->addr.a);
        return mru_to_order.count(base_addr) != 0;
    }
    bool replace_ok() const {
        return false;
    }
    void touch_sample(const MemProfSample *samp) {
        // reminder: CAM hit implies addr-space ID match
        mem_addr base_addr = mru_window_align(samp->addr.a);
        MRUAddrMap::iterator map_found = mru_to_order.find(base_addr);
        if (map_found != mru_to_order.end()) {
            // found; move-to-front
            mru_order.erase(map_found->second);
            mru_order.push_front(base_addr);
            (map_found->second) = mru_order.begin();
            ++sample_hits;
        } else {
            // not found; remove LRU if full, then insert new as MRU
            while (intsize(mru_order) >= max_mru_windows) {
                mem_addr to_remove = pop_back_ret(mru_order);
                mru_to_order.erase(to_remove);
            }                
            mru_order.push_front(base_addr);
            mru_to_order.insert(std::make_pair(base_addr, mru_order.begin()));
            ++sample_misses;
        }
    }
};


class InstMRUTab : public TableWithDefaultOps<TableKey_Int,MRUEnt> {
    typedef TableKey_Int KeyType;
    typedef MRUEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    int max_mru_windows_;          // #entries (PCs) in MRU table
    int window_size_;             // in blocks, POWER OF 2
    int window_size_bytes_;       // POWER OF 2

    // we could actually do online feedback with the hit/miss data available
    bool uses_online_feedback() const { return false; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    KeyType sample_key(const MemProfSample *samp) const {
        return KeyType(samp->app_id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (key.id == app_id);
    }

    void get_expand_entry_parms(const KeyType& key, const EntType& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(ent.master_id,
                    intsize(ent.mru_order) * 48,       // VA per window
                    0,                  // delta-VA
                    PFT_Inst,
                    false);
    }
    void ent_assign(EntType *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->window_size_bytes = this->window_size_bytes_;
        ent->max_mru_windows = max_mru_windows_;
        ent->master_id = samp->addr.id;
        ent->mru_order.clear();
        ent->mru_to_order.clear();
        ent->sample_hits = 0;
        ent->sample_misses = 0;
        ent->touch_sample(samp);
    }
    void ent_init_predict(const KeyType& key, EntType *ent) const {
        ent->mru_to_order.clear();      // we're going to gobble up mru_order
        ent->predict.blocks_used_this_window = 0;
    }
    bool ent_can_predict(const KeyType& key, const EntType *ent) const {
        return !ent->mru_order.empty();
    }
    mem_addr ent_step_predict(const KeyType& key, EntType *ent) const {
        sim_assert(!ent->mru_order.empty());
        sim_assert(ent->predict.blocks_used_this_window < window_size_);
        mem_addr result = ent->mru_order.front() +
            block_bytes * ent->predict.blocks_used_this_window;
        ++ent->predict.blocks_used_this_window;
        if (ent->predict.blocks_used_this_window == window_size_) {
            ent->predict.blocks_used_this_window = 0;
            ent->mru_order.pop_front();
        }
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = intsize(ent->mru_order) * window_size_;
        *priority_ret = *forecast_count_ret;
    }

public:
    InstMRUTab(const WSM_Config *conf__, int table_id_, const string& cfg,
               int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "InstMRU", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        max_mru_windows_ = conf_int(cfg + "/max_mru_windows");
        if (max_mru_windows_ < 1) {
            exit_printf("InstMRU: bad max_mru_windows %d\n", max_mru_windows_);
        }

        window_size_ = conf_int(cfg + "/window_size");
        if ((window_size_ < 1) || (log2_exact(window_size_) < 0)) {
            exit_printf("InstMRU: bad window_size %d\n", window_size_);
        }
        window_size_bytes_ = block_bytes * window_size_; // both powers of 2
        sim_assert(log2_exact(window_size_bytes_) >= 0);
    }

    bool sample_accept(const MemProfSample *samp) const {
        // filter: we want only I-stream accesses
        return (samp->op_type == MPO_InstFetch);
    }
};


// sigh, lots of copy/paste sharing with InstMRUTab
class DataMRUTab : public TableWithDefaultOps<TableKey_Int,MRUEnt> {
    typedef TableKey_Int KeyType;
    typedef MRUEnt EntType;
    typedef TableWithDefaultOps<KeyType,EntType> DefaultOps;

    int max_mru_windows_;          // #entries (PCs) in MRU table
    int window_size_;             // in blocks, POWER OF 2
    int window_size_bytes_;       // POWER OF 2

    // we could actually do online feedback with the hit/miss data available
    bool uses_online_feedback() const { return false; }
    EntryExpandType entry_expand_type() const { return Expand_Finite; }

    KeyType sample_key(const MemProfSample *samp) const {
        return KeyType(samp->app_id);
    }
    bool key_matches_appid(const KeyType& key, int app_id) const {
        return (key.id == app_id);
    }

    void get_expand_entry_parms(const KeyType& key, const EntType& ent,
                                ExpandEntryParms *ret) const {
        ret->assign(ent.master_id,
                    intsize(ent.mru_order) * 48,       // VA per window
                    0,                 // delta-VA
                    PFT_Data,
                    false);
    }
    void ent_assign(EntType *ent, const AppState *as,
                    const MemProfSample *samp) {
        ent->base_assign();
        ent->window_size_bytes = this->window_size_bytes_;
        ent->max_mru_windows = max_mru_windows_;
        ent->master_id = samp->addr.id;
        ent->mru_order.clear();
        ent->mru_to_order.clear();
        ent->sample_hits = 0;
        ent->sample_misses = 0;
        ent->touch_sample(samp);
    }
    void ent_init_predict(const KeyType& key, EntType *ent) const {
        ent->mru_to_order.clear();      // we're going to gobble up mru_order
        ent->predict.blocks_used_this_window = 0;
    }
    bool ent_can_predict(const KeyType& key, const EntType *ent) const {
        return !ent->mru_order.empty();
    }
    mem_addr ent_step_predict(const KeyType& key, EntType *ent) const {
        sim_assert(!ent->mru_order.empty());
        sim_assert(ent->predict.blocks_used_this_window < window_size_);
        mem_addr result = ent->mru_order.front() +
            block_bytes * ent->predict.blocks_used_this_window;
        ++ent->predict.blocks_used_this_window;
        if (ent->predict.blocks_used_this_window == window_size_) {
            ent->predict.blocks_used_this_window = 0;
            ent->mru_order.pop_front();
        }
        return result;
    }
    void ent_forecast(const KeyType& key, const EntType *ent,
                      i64 forecast_window, i64 *forecast_count_ret,
                      i64 *priority_ret) const {
        *forecast_count_ret = intsize(ent->mru_order) * window_size_;
        *priority_ret = *forecast_count_ret;
    }

public:
    DataMRUTab(const WSM_Config *conf__, int table_id_, const string& cfg,
               int default_size, int block_bytes_lg_)
        : DefaultOps(conf__, "DataMRU", table_id_, cfg, default_size,
                     block_bytes_lg_)
    {
        max_mru_windows_ = conf_int(cfg + "/max_mru_windows");
        if (max_mru_windows_ < 1) {
            exit_printf("DataMRU: bad max_mru_windows %d\n", max_mru_windows_);
        }

        window_size_ = conf_int(cfg + "/window_size");
        if ((window_size_ < 1) || (log2_exact(window_size_) < 0)) {
            exit_printf("DataMRU: bad window_size %d\n", window_size_);
        }
        window_size_bytes_ = block_bytes * window_size_; // both powers of 2
        sim_assert(log2_exact(window_size_bytes_) >= 0);
    }

    bool sample_accept(const MemProfSample *samp) const {
        // filter: we want only D-stream accesses
        return (samp->op_type == MPO_DataLoad) ||
            (samp->op_type == MPO_DataStore);
    }
};


typedef set<LongAddr> AddrSet;
class PrefetchPlayerCB;
typedef vector<PrefetchPlayerCB *> PrefetchPlayerVec;


// Restricted type of CBQ_Callback, which can be scheduled in at most one
// CallbackQueue.  These hold a pointer to that CallbackQueue, so they can be
// unscheduled if necessary (e.g. parent object destruction).

class SingleQueueCallback : public CBQ_Callback {
    CallbackQueue *sched_queue_;        // NULL <=> not scheduled in a queue
public:
    SingleQueueCallback() : sched_queue_(NULL) { }
    ~SingleQueueCallback() {
        // Somewhere, somebody forgot to unschedule this from sched_queue_, or
        // perhaps forgot to tell us about it.  Since we don't specify
        // ownership here, we can't just cancel it without possibly leaking;
        // also, we'd prefer things be taken care of at the more-specific
        // use sites, versus possibly much later in this generic destructor.
        sim_assert(!sched_queue_);
    }
    bool is_sched() const { return sched_queue_ != NULL; }
    CallbackQueue *get_sched_queue() { return sched_queue_; }
    // was unscheduled, now being added to a CallbackQueue
    void scheduled_in(CallbackQueue *next_sched_queue) {
        sim_assert(sched_queue_ == NULL);
        sim_assert(next_sched_queue != NULL);
        sched_queue_ = next_sched_queue;
    }
    // was scheduled in a queue; now done
    void sched_to_done() {
        sim_assert(sched_queue_ != NULL);
        sched_queue_ = NULL;        
    }
};


// Prefetch "playback" object: plays prefetches from one PrefetchQueue
// (Each instance of this is called repeatedly from CallbackQueue)
//
// These are scheduled in at most one CallbackQueue (like AppSummaryCallbacks)
class PrefetchPlayerCB : public SingleQueueCallback {
    scoped_ptr<PrefetchQueue> pfq_;
    AddrSet *shared_base_addr_filter_;        // may be NULL; not owned
    CoreResources *core_;
    i64 expire_time_;           // stop if invoked after this time (always)
    i64 create_time_;
    i64 serial_;                // unique player ID for debugging, etc.
    PrefetchEntry prev_pf_;     // most recently issued PF from this queue
    bool waiting_for_prev_;     // flag: currently stalled for "prev_pf"

    bool filter_accept_prefetch(const LongAddr& base_addr) const {
        // perform prefetch if either the address filter is not in-use,
        // or if the address filter had not seen this address previously
        bool accept =
            !shared_base_addr_filter_ ||
            !shared_base_addr_filter_->count(base_addr);
        return accept;
    }
    void filter_mark_prefetched(const LongAddr& base_addr) {
        if (shared_base_addr_filter_) {
            bool insert_ok = shared_base_addr_filter_->insert(base_addr)
                .second;
            sim_assert(insert_ok);
        }
    }

public:
    // Takes ownership of "pfq"
    PrefetchPlayerCB(PrefetchQueue *pfq__, 
                     set<LongAddr> *shared_base_addr_filter__,
                     CoreResources *core__, i64 expire_time__)
        : pfq_(pfq__),
          shared_base_addr_filter_(shared_base_addr_filter__),
          core_(core__), expire_time_(expire_time__),
          create_time_(cyc), serial_(GlobalPrefetchPlayerNextSerial),
          prev_pf_(), waiting_for_prev_(false)
    {
        WSMDB(2)("PrefetchPlayerCB: serial %s created at time %s "
                 "from collected queue_id %d with ready_time %s, size %d\n",
                 fmt_i64(serial_), fmt_now(), pfq_->queue_id(),
                 fmt_i64(pfq_->ready_time()), intsize(*pfq_));
        ++GlobalPrefetchPlayerNextSerial;
    }

    i64 ready_time() const {
        return (pfq_->empty()) ? 0 : pfq_->ready_time();
    }

    i64 invoke(CBQ_Args *args) {
        const char *fname = "PrefetchPlayerCB";
        WSMDB(2)("%s: serial %s queue_id %d created %s, invoked at %s, "
                 "%d ents remaining\n",
                 fname, fmt_i64(serial_), pfq_->queue_id(),
                 fmt_i64(create_time_), fmt_now(), intsize(*pfq_));
        if (cyc >= expire_time_) {
            WSMDB(2)("%s: expired at time %s (expire_time %s), "
                     "%d ents remaining.\n", fname, fmt_now(),
                     fmt_i64(expire_time_), intsize(*pfq_));
            this->sched_to_done();
            return -1;  // callback done
        }
        bool cache_busy = false;

        while (!pfq_->empty() && (pfq_->ready_time() <= cyc)) {
            const PrefetchEntry& next_pf = pfq_->front();
            WSMDB(2)("%s: time %s size %d next_pf %s\n", fname, fmt_now(),
                     intsize(*pfq_), next_pf.fmt().c_str());
            CacheSource pf_csource = (next_pf.targ() == PFT_Inst) ? 
                CSrc_L1_ICache : CSrc_L1_DCache;

            if (waiting_for_prev_) {
                sim_assert(prev_pf_.valid());
                // hack: probe MSHRs to see if prior request is still pending
                const MshrTable *mshr = (prev_pf_.targ() == PFT_Inst) ?
                    core_->inst_mshr : core_->data_mshr;
                if (mshr_any_producer(mshr, prev_pf_.base_addr())) {
                    WSMDB(2)("%s: stalling for prior PF: %s\n",
                             fname, prev_pf_.base_addr().fmt().c_str());
                    cache_busy = true;
                    break;
                }
            }

            // copy PF info in case we decide to wait for it, since
            // "pop_front" on queue will destroy it otherwise.
            prev_pf_ = next_pf;
            waiting_for_prev_ = false;

            if (!filter_accept_prefetch(next_pf.base_addr())) {
                // block has already been prefetched; skip it
                // warning: invalidates "next_pf"
                WSMDB(2)("%s: already-prefetched; dropping\n", fname);
                pfq_->pop_front(cyc);
            } else if (cachesim_prefetch_for_wsm(core_, next_pf.base_addr(),
                                                 next_pf.excl_access(),
                                                 pf_csource,
                                                 next_pf.table_id(),
                                                 next_pf.entry_id())) {
                // prefetch accepted by mem subsystem
                filter_mark_prefetched(next_pf.base_addr());
                // warning: invalidates "next_pf"
                pfq_->pop_front(cyc);
            } else {
                // prefetch rejected for now; spin until it succeeds
                // (we should probably have just one "player" per target
                // structure, to avoid redundant spinning)
                //
                // Also lame: this lets one "hot" bank stop independent
                // prefetches to other banks, which is sad.
                cache_busy = true;
                break;
            }

            if (!pfq_->empty() && pfq_->front().wait_for_prev()) {
                waiting_for_prev_ = true;
            }
        }

        i64 next_pf_time;
        if (pfq_->empty()) {
            WSMDB(2)("%s: serial %s queue_id %d created %s, "
                     "finished at time %s\n",
                     fname, fmt_i64(serial_), pfq_->queue_id(),
                     fmt_i64(create_time_), fmt_now());
            next_pf_time = -1;  // destroy this callback
        } else {
            // note: we just lamely poll every cycle if the cache looks full
            next_pf_time = ((cache_busy) ? cyc + 1 : pfq_->ready_time());
        }
        if (next_pf_time < 0)
            this->sched_to_done();
        return next_pf_time;
    }        
};


void
estimate_bus_xfer_time(OpTime* dest, long xfer_size_bits)
{
    if (xfer_size_bits > 0) {
        long xfer_blocks = (long)
            ceil(double(xfer_size_bits) /
                 (8.0 * GlobalParams.mem.cache_block_bytes));
        sim_assert(xfer_blocks > 0);
        const OpTime& base_op = GlobalParams.mem.bus_transfer_time;
        // Aggregate latency and intervals of "x" fully-pipelined
        // back-to-back transfers, x > 0:
        //   L(x) = L(1) + I(1) * (x - 1)
        //   I(x) = I(1) * x
        // ...holds for all three cases of L(1) <=> I(1)
        dest->latency = base_op.latency + base_op.interval * (xfer_blocks - 1);
        dest->interval = base_op.interval * xfer_blocks;
        // (assume: we lock the bus down once, and send the address data
        //  flat-out)
    } else {
        dest->latency = 0;
        dest->interval = 0;
    }
}


// inventory of all blocks held by a core's caches
class CoreCacheInventory {
    const CoreResources * restrict core_;
    int master_id_spec_;

    AddrSet icache_;
    AddrSet dcache_;
    AddrSet l2cache_;
    AddrSet seen_excl_;

public:

    // master_id_spec selects desired master_id, or -1 for "all"
    CoreCacheInventory(const CoreResources * restrict core,
                       int master_id_spec)
        : core_(core), master_id_spec_(master_id_spec) {
        {
            int n_tags = 0;
            LongAddr *tags = cache_get_tags(core_->icache, master_id_spec_,
                                            &n_tags);
            for (int i = 0; i < n_tags; i++) {
                if (!tags[i].nonzero())
                    continue;
                icache_.insert(tags[i]);
                if (!seen_excl_.count(tags[i]) &&
                    cache_access_ok(core_->icache, tags[i], Cache_ReadExcl)) {
                    seen_excl_.insert(tags[i]);
                }
            }
            free(tags);
        }
        {
            int n_tags = 0;
            LongAddr *tags = cache_get_tags(core_->dcache, master_id_spec_,
                                            &n_tags);
            for (int i = 0; i < n_tags; i++) {
                if (!tags[i].nonzero())
                    continue;
                dcache_.insert(tags[i]);
                if (!seen_excl_.count(tags[i]) &&
                    cache_access_ok(core_->dcache, tags[i], Cache_ReadExcl)) {
                    seen_excl_.insert(tags[i]);
                }
            }
            free(tags);
        }
        if (GlobalParams.mem.private_l2caches) {
            int n_tags = 0;
            LongAddr *tags = cache_get_tags(core_->l2cache, master_id_spec_,
                                            &n_tags);
            for (int i = 0; i < n_tags; i++) {
                if (!tags[i].nonzero())
                    continue;
                l2cache_.insert(tags[i]);
                if (!seen_excl_.count(tags[i]) &&
                    cache_access_ok(core_->l2cache, tags[i], Cache_ReadExcl)) {
                    seen_excl_.insert(tags[i]);
                }
            }
            free(tags);
        }
    }

    // cheesy internals-exposing returns (convenient now, though)
    const AddrSet& get_icache_blocks() const { return icache_; }
    const AddrSet& get_dcache_blocks() const { return dcache_; }
    const AddrSet& get_l2cache_blocks() const { return l2cache_; }

    bool block_seen_excl(const LongAddr& addr) const {
        return seen_excl_.count(addr);
    }
};


// forward decls for utility code down toward the end of the file
void discard_core_cached_app(CoreResources * restrict core,
                             AppState * restrict as,
                             unsigned cache_select_mask,
                             bool downgrade_only, bool tlbs_too);
int cache_size_blocks(const CacheArray* cache);


} // Anonymous namespace close



// one-shot callback that calls sim_exit_ok() with a pre-set message
// (may make this globally-usable at some point)
class SimExitOkCB : public CBQ_Callback {
    string msg;
public:
    SimExitOkCB(const string& msg_) : msg(msg_) { }
    SimExitOkCB(const char *msg_) : msg(msg_) { }
    i64 invoke(CBQ_Args *args) {
        sim_exit_ok(msg.c_str());       // doesn't return
        abort_printf("exit returned, WTH?\n");
        // no need to unlinking this from its caller, since there won't be
        // a chance to explicitly delete it anyway
        return -1;
    }        
};


// Block of hardware implementing one set of tables for monitoring and
// summarizing application behavior.  (These used to be owned by instances of
// AppStateExtras, but have since changed to being owned by instances of
// CoreResources, since they're modelling hardware.)
struct WSM_ThreadCapture {
    typedef vector<TableBase *> TableVec;
    class TableAgeCB;
    
    WSM_Coord *parent;
    const WSM_Config *conf;
    string name;                // name for debug printing
    string cfg_path;            // config path

    int block_bytes_lg;
    i64 access_serial;          // serial number over all accesses
    CallbackQueue *serial_watcher;      // callback "time" is access_serial
    TableAgeCB *age_cb;         // (owned by some CallbackQueue)
    i64 sample_count;
    i64 expand_calls;           // #calls to summarize_and_expand()
    NoDefaultCopy nocopy;

    struct {
        // "owned" type-specific pointers to components, for destruction and
        // perhaps later twiddling
        scoped_ptr<NextBlockInstTab> nextblk_inst;
        scoped_ptr<NextBlockDataTab> nextblk_data;
        scoped_ptr<NextBlockBothTab> nextblk_both;
        scoped_ptr<StridePCTab> stridepc;
        scoped_ptr<SameObjTab> sameobj;
        scoped_ptr<PointerTab> pointer;
        scoped_ptr<PointerChaseTab> pointer_chase;
        scoped_ptr<BTBTab> btb;
        scoped_ptr<BlockBTBTab> blkbtb;
        scoped_ptr<PCWindowTab> pcwindow;
        scoped_ptr<SPWindowTab> spwindow;
        scoped_ptr<RetStackTab> retstack;
        scoped_ptr<InstMRUTab> instmru;
        scoped_ptr<DataMRUTab> datamru;
    } tab;

    // vector of pointers to tables, cast as base type, in priority order,
    // for batch processing
    TableVec tables;

    WSM_ThreadCapture(WSM_Coord *parent__, const WSM_Config *conf__,
                      const string& name__, const string& config_path__);
    ~WSM_ThreadCapture();
    void reset();
    void flush_tables(int app_id_or_neg1);
    void sample(AppState *as, const MemProfSample *samp);

    void summarize_and_expand(int for_app_id, PrefetchCollector *pf_coll,
                              long *xfer_bits_ret);
    void age_tables();
    void print_stats(FILE *out, const char *pf) const;
};


class WSM_AppSummary;

// Info held about each "interesting" application, by WSM_Coord.
class WSM_CoordPerApp {
    WSM_Coord *parent_;                 // not owned
    AppState *as_;                      // not owned
    scoped_ptr<WSM_AppSummary> last_summary_;
    bool prefetch_enabled_;

    struct {
        string memref_filename;
        scoped_ptr<MemRefSeq_Reader> memref_reader;
        scoped_ptr<istream> memref_istream;
    } future_pf_;

    i64 dbp_blocks_discarded_;  // #blocks discarded due to deadblock-pred

    NoDefaultCopy nocopy;

public:
    WSM_CoordPerApp(WSM_Coord *parent__, const WSM_Config *conf__,
                    AppState *app__);
    ~WSM_CoordPerApp();
    void print_stats(FILE *out, const char *pf) const;

    AppState *get_app() { return as_; }
    int app_id() const { return as_->app_id; }
    int app_master_id() const { return as_->app_master_id; }
    bool prefetch_for_this_app() const { return prefetch_enabled_; }

    void dbp_block_discarded() { ++dbp_blocks_discarded_; }

    bool have_summary() const { return last_summary_; }
    WSM_AppSummary *get_summary() { return last_summary_.get(); }
    // handoff: claims ownership of "summary"
    void handoff_summary(WSM_AppSummary *summary);
    void discard_any_summary();

    // NULL <=> no reader
    MemRefSeq_Reader *get_memref_reader() {
        return future_pf_.memref_reader.get();
    }
    // WARNING: may invalidate previous returns from get_memref_reader()!
    void rewind_memref_reader();
};


WSM_CoordPerApp::WSM_CoordPerApp(WSM_Coord *parent__, const WSM_Config *conf,
                                 AppState *app__)
    : parent_(parent__), as_(app__), prefetch_enabled_(false),
      dbp_blocks_discarded_(0)
{
    if (conf->reg_move.future_pf.enable) {
        // Use a matching entry from MemFileMap, falling back to mem_ref_file
        const string *filename =
            map_find(conf->reg_move.future_pf.mem_file_map, as_->app_id);
        if (!filename)
            filename = &conf->reg_move.future_pf.mem_ref_file;

        WSMDB(1)("RegularMove: opening file for A%d future-prefetch data: "
                 "\"%s\"\n", as_->app_id, filename->c_str());
        future_pf_.memref_filename = *filename;
        future_pf_.memref_istream.
            reset(open_istream_auto_decomp(filename->c_str()));
        istream *in_str = future_pf_.memref_istream.get();
        if (!in_str) {
            exit_printf("couldn't open MemRefSeq file for A%d: \"%s\"\n",
                        as_->app_id, filename->c_str());
        }
        future_pf_.memref_reader.reset(new MemRefSeq_Reader(in_str));
    }

    {
        // Policy: which apps do we want to prefetch for?
        if (conf->reg_move.app_id >= 0) {
            // original "RegularMove" mode; only prefetch for the nomad app
            prefetch_enabled_ = (as_->app_id == conf->reg_move.app_id);
        } else {
            // prefetch only for apps requested in config
            prefetch_enabled_ = conf->prefetch_all_apps ||
                conf->prefetch_for_apps.count(as_->app_id);
        }
    }
}


WSM_CoordPerApp::~WSM_CoordPerApp()
{
}


void
WSM_CoordPerApp::rewind_memref_reader()
{
    const string *filename = &future_pf_.memref_filename;

    WSMDB(1)("RegularMove: re-opening file for A%d future-prefetch data: "
             "\"%s\"\n", as_->app_id, filename->c_str());
    sim_assert(future_pf_.memref_reader);
    delete scoped_ptr_release(future_pf_.memref_reader);
    delete scoped_ptr_release(future_pf_.memref_istream);

    // copy-paste from constructor
    future_pf_.memref_istream.
        reset(open_istream_auto_decomp(filename->c_str()));
    istream *in_str = future_pf_.memref_istream.get();
    if (!in_str) {
        exit_printf("couldn't re-open MemRefSeq file for A%d: \"%s\"\n",
                    as_->app_id, filename->c_str());
    }
    future_pf_.memref_reader.reset(new MemRefSeq_Reader(in_str));
}


void
WSM_CoordPerApp::print_stats(FILE *out, const char *pf) const
{
    fprintf(out, "%sWSM_CoordPerApp A%d stats:\n", pf, as_->app_id);
    fprintf(out, "%s  dbp_blocks_discarded: %s\n", pf,
            fmt_i64(dbp_blocks_discarded_));
}


struct WSM_Coord {
private:
    typedef map<AppState *,WSM_CoordPerApp *> AppInfoMap;
    typedef set<WSM_ThreadCapture *> CaptureSet;
    typedef map<context *,AppState *> PausedCtxMap;   // context -> app

    class RegularMoveCB;
    class RegularMoveStartupCB;
    class BackgroundStirCB;
    class BackgroundPauseCB;
    class CoschedHaltedCB;

private:
    string cfg_path_;           // config path
    const WSM_Config conf_;

    struct {
        AppState *as;           // NULL if disabled, or before discovery
        int prev_targ_core;     // most recent target core

        // This struct holds callbacks created by this WSM_Coord, pending
        // executions somewhere; it's perhaps needlessly verbose to track
        // these seperately, versus just having one CallbackSet for all held
        // by CallbackQueue, a CallbackSet for those held by a commit-watch,
        // etc.; while that woud simplify the cleanup code, having these
        // pointers available with full type info may prove handy later,
        // or in debugging.
        //
        // Basic rules: all callbacks created in this module should be linked
        // here somehow; callback invoke() methods should unlink themselves
        // when requesting destruction (e.g. one-shot callbacks); remaining
        // linked callbacks should be descheduled and destroyed in the
        // appropriate destructor.
        //
        // (GEQ stands for GlobalEventQueue)
        struct {
            SimExitOkCB *exit;                  // commit-watch
            RegularMoveStartupCB *starter;      // GEQ
            RegularMoveCB *mover;               // GEQ or commit-watch
            BackgroundStirCB *bg_stirrer;       // GEQ or commit-watch
            set<BackgroundPauseCB *> bg_pausers;// owned by appmgr migrate sys
            set<CoschedHaltedCB *> cosched_halted;  // owned by context_halt!
        } pend_cb;
    } reg_move;

    AppInfoMap per_app_info_;           // value pointers owned by this
    CaptureSet capture_units_;          // (not owned, but hold pointers to us)
    PausedCtxMap paused_contexts_;
    NoDefaultCopy nocopy;

    void pause_cosched_threads(int core_id, int nopause_app_id);
    void resume_cosched_threads(int core_id, int nopause_app_id);

public:
    WSM_Coord(const string& config_path);
    ~WSM_Coord();
    WSM_ThreadCapture *new_threadcap(const string& name__);
    void register_app(struct AppState *app);
    void destroying_app(struct AppState *app);

    bool want_sample(struct context *ctx, struct AppState *app) const;
    void detach_capture(WSM_ThreadCapture *cap);

    void signal_activate(struct AppState *app, struct context *ctx);
    void signal_deactivate(struct AppState *app, struct context *ctx);
};


class CacheInjectCB;
class OraclePrefetchCB;
class CacheDiscardCB;
class StreambufMigCB;
class StreambufStopCB;
class SummarizePrefetchCB;
class FuturePrefetchCB;
class TLBCopyCB;


class WSM_AppSummary {
    const WSM_Config *conf_;
    CallbackQueue *play_time_queue_;    // not owned
    AppState *as_;                      // not owned
    CoreResources *summary_source_core_;
    i64 create_time_;
    bool started_;                      // has been used for an activation
    CoreResources *activate_target_core_;

    struct {
        // These callbacks are all owned by this object, and NOT by whatever
        // CallbackQueues they happen to be scheduled in.
        scoped_ptr<CacheInjectCB> cache_inject;         // immediate
        scoped_ptr<OraclePrefetchCB> oracle_prefetch;   // immediate
        scoped_ptr<CacheDiscardCB> discard_departing;   // play_time
        scoped_ptr<CacheDiscardCB> discard_arriving;    // immediate
        scoped_ptr<StreambufMigCB> sb_migrate;          // play_time
        // StreambufStopCB is scheduled by schedule_deactivate(), at the end
        // of the deactivate-cycle, to stop the prefetcher on the source core
        // from pulling in more data.  (We put it in GlobalEventQueue since
        // deactivate can be triggered via commit; if we stopped streams
        // there, and other commits in the same cycle could restart it.)
        scoped_ptr<StreambufStopCB> sb_stop;            // GlobalEventQueue
        scoped_ptr<SummarizePrefetchCB> summarize_prefetch;   // immediate
        scoped_ptr<FuturePrefetchCB> future_prefetch;   // immediate
        scoped_ptr<TLBCopyCB> tlb_copy;                 // play_time

        PrefetchPlayerVec pf_players;                   // play_time
    } cb_;

    // address filter used by some PrefetchPlayers
    AddrSet pf_addr_filter_;                    // address filter

    NoDefaultCopy nocopy;

public:
    WSM_AppSummary(const WSM_Config *conf, CallbackQueue *play_time_queue,
                   AppState *app, CoreResources *source_core);
    ~WSM_AppSummary();          // destructor follows class decls for callbacks

    bool ready_to_start() const { return !started_; }
    void start_for_activate(CoreResources *target_core);

    CoreResources *get_source_core() {
        sim_assert(summary_source_core_ != NULL);
        return summary_source_core_;
    }
    CoreResources *get_target_core() {
        sim_assert(activate_target_core_ != NULL);
        return activate_target_core_;
    }

    void add_cache_inject(CacheInjectCB *new_cb) {
        sim_assert(!cb_.cache_inject);
        cb_.cache_inject.reset(new_cb);
    }
    void add_oracle_prefetch(OraclePrefetchCB *new_cb) {
        sim_assert(!cb_.oracle_prefetch);
        cb_.oracle_prefetch.reset(new_cb);
    }
    void add_discard_departing(CacheDiscardCB *new_cb) {
        sim_assert(!cb_.discard_departing);
        cb_.discard_departing.reset(new_cb);
    }
    void add_discard_arriving(CacheDiscardCB *new_cb) {
        sim_assert(!cb_.discard_arriving);
        cb_.discard_arriving.reset(new_cb);
    }
    void add_sb_migrate(StreambufMigCB *new_cb) {
        sim_assert(!cb_.sb_migrate);
        cb_.sb_migrate.reset(new_cb);
    }
    void add_sb_stop(StreambufStopCB *new_cb) {
        sim_assert(!cb_.sb_stop);
        cb_.sb_stop.reset(new_cb);
    }
    void add_summarize_prefetch(SummarizePrefetchCB *new_cb) {
        sim_assert(!cb_.summarize_prefetch);
        cb_.summarize_prefetch.reset(new_cb);
    }
    void add_future_prefetch(FuturePrefetchCB *new_cb) {
        sim_assert(!cb_.future_prefetch);
        cb_.future_prefetch.reset(new_cb);
    }
    void add_tlb_copy(TLBCopyCB *new_cb) {
        sim_assert(!cb_.tlb_copy);
        cb_.tlb_copy.reset(new_cb);
    }

    // place-holder for possible policy changes later
    bool request_as_excl(const LongAddr& base_addr, bool excl_at_source) const;

    // (utility routine for post-activate)
    // takes ownership of a PrefetchPlayer, and schedule it for playback 
    // in a time-queue at the given start_time
    void handoff_start_pf_player(PrefetchPlayerCB *pf_player, i64 start_time);

    AddrSet *get_shared_addr_filter() {
        return &pf_addr_filter_;
    }
};


bool
WSM_AppSummary::request_as_excl(const LongAddr& base_addr,
                                bool excl_at_source) const
{
    bool result = conf_->reg_move.inject_excl ||
        (conf_->reg_move.inject_keep_excl && excl_at_source);
    if (conf_->inst_shared_range.enable &&
        (base_addr.a >= conf_->inst_shared_range.begin) &&
        (base_addr.a < conf_->inst_shared_range.end)) {
        result = false;
    }
    return result;
}


void
WSM_AppSummary::handoff_start_pf_player(PrefetchPlayerCB *pf_player,
                                        i64 start_time)
{
    sim_assert(started_);
    callbackq_enqueue_unowned(play_time_queue_, start_time, pf_player);
    pf_player->scheduled_in(play_time_queue_);
    cb_.pf_players.push_back(pf_player);
}


void
WSM_CoordPerApp::handoff_summary(WSM_AppSummary *summary)
{
    // (method down here for WSM_AppSummary destructor visibility)
    if (last_summary_) {
        delete scoped_ptr_release(last_summary_);
    }
    last_summary_.reset(summary);
}


void
WSM_CoordPerApp::discard_any_summary()
{
    if (last_summary_) {
        delete scoped_ptr_release(last_summary_);
    }
}


// Specialized callback type for use specifically within WSM_AppSummary.  For
// convenience, these hold pointers to (shared) WSM config, as well as to
// specific objects relevant to 1) the subject application, and 2) this
// specific migration.
//
// These callbacks are strictly owned by the WSM_AppSummary they were created
// for.
class AppSummaryCallback : public SingleQueueCallback {
protected:
    const WSM_Config *conf_;
    WSM_CoordPerApp *app_info_;
    WSM_AppSummary *parent_;
    NoDefaultCopy nocopy;
public:
    AppSummaryCallback(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                       WSM_AppSummary *parent__)
        : conf_(conf__), app_info_(app_info__), parent_(parent__)
    { }
};


// This is a one-shot callback, which dumps caches from one core and injects
// their contents into those of another core
class CacheInjectCB : public AppSummaryCallback {
    typedef map<LongAddr,unsigned> InjectMaskMap;
    static const unsigned kInjectExclFlag = 0x100;      // hack
    InjectMaskMap inject_masks_;

public:
    CacheInjectCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                  WSM_AppSummary *parent__)
        : AppSummaryCallback(conf__, app_info__, parent__)
    {
        const char *fname = "CacheInjectCB";
        CoreResources *from_core = parent__->get_source_core();
        const int master_id = app_info_->app_master_id();
        WSMDB(1)("%s: time %s, cache inventory masterid %d C%d\n",
                 fname, fmt_now(), master_id, from_core->core_id);

        CoreCacheInventory cache_inv(from_core, master_id);

        // populate inject_masks_ for later use at activate-time
        if (conf_->reg_move.copy_l1i) {
            const AddrSet& i_blocks(cache_inv.get_icache_blocks());
            WSMDB(1)("%s: source L1I has %d tags for id %d\n", fname,
                     intsize(i_blocks), master_id);
            FOR_CONST_ITER(AddrSet, i_blocks, iter) {
                if (conf_->reg_move.dbp_filter_oracle &&
                    from_core->i_dbp &&
                    dbp_predict_dead(from_core->i_dbp, *iter)) {
                    app_info_->dbp_block_discarded();
                    continue;
                }
                inject_masks_[*iter] |= CACHE_INJECT_L1I;
            }
        }
        if (conf_->reg_move.copy_l1d) {
            const AddrSet& d_blocks(cache_inv.get_dcache_blocks());
            WSMDB(1)("%s: source L1D has %d tags for id %d\n", fname,
                     intsize(d_blocks), master_id);
            FOR_CONST_ITER(AddrSet, d_blocks, iter) {
                if (conf_->reg_move.dbp_filter_oracle && from_core->d_dbp
                    && dbp_predict_dead(from_core->d_dbp, *iter)) {
                    app_info_->dbp_block_discarded();
                    continue;
                }
                inject_masks_[*iter] |= CACHE_INJECT_L1D;
            }
        }
        if (conf_->reg_move.copy_l2 && GlobalParams.mem.private_l2caches) {
            const AddrSet& l2_blocks(cache_inv.get_l2cache_blocks());
            WSMDB(1)("%s: source L2 has %d tags for id %d\n", fname, 
                     intsize(l2_blocks), master_id);
            FOR_CONST_ITER(AddrSet, l2_blocks, iter) {
                if (conf_->reg_move.dbp_filter_oracle &&
                    from_core->l2_dbp &&
                    dbp_predict_dead(from_core->l2_dbp, *iter)) {
                    app_info_->dbp_block_discarded();
                    continue;
                }
                inject_masks_[*iter] |= CACHE_INJECT_L2;
            }
        }

        FOR_ITER(InjectMaskMap, inject_masks_, iter) {
            const LongAddr& addr = iter->first;
            if (parent_->request_as_excl(addr,
                                         cache_inv.block_seen_excl(addr))) {
                // make sure our hack-flag doesn't conflict with CACHE_INJECT_*
                sim_assert(!(iter->second & kInjectExclFlag));
                iter->second |= kInjectExclFlag;
            }
        }
    }

    i64 invoke(CBQ_Args *args) {
        const char *fname = "CacheInjectCB";
        const int master_id = app_info_->app_master_id();
        CoreResources *from_core = parent_->get_source_core();
        CoreResources *to_core = parent_->get_target_core();

        WSMDB(1)("%s: time %s, cache inject masterid %d"
                 " C%d -> C%d\n", fname, fmt_now(), master_id,
                 from_core->core_id, to_core->core_id);

        FOR_CONST_ITER(InjectMaskMap, inject_masks_, iter) {
            const LongAddr& addr = iter->first;
            sim_assert(iter->second != 0);
            unsigned inject_mask = iter->second;
            bool inject_as_excl = inject_mask & kInjectExclFlag;
            inject_mask &= ~kInjectExclFlag;
            cachesim_oracle_inject_core(to_core, addr, inject_as_excl,
                                        inject_mask);
        }

        if (this->is_sched())
            this->sched_to_done();
        return -1;      // callback abandoned by callbackq
    }
};


// This is a one-shot callback; it schedules PrefetchPlayerCBs which
// perform prefetches.
class OraclePrefetchCB : public AppSummaryCallback {
    scoped_ptr<PrefetchCollection> collected_queues_;

public:
    OraclePrefetchCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                     WSM_AppSummary *parent__)
        : AppSummaryCallback(conf__, app_info__, parent__)
    {
        const char *fname = "OraclePrefetchCB";
        CoreResources *from_core = parent_->get_source_core();
        const int master_id = app_info_->app_master_id();

        WSMDB(1)("%s: time %s, cache inventory masterid %d C%d\n",
                 fname, fmt_now(), master_id, from_core->core_id);

        CoreCacheInventory cache_inv(from_core, master_id);

        PrefetchCollector coll;
        // oracle: let PF run as fast as cache B/W allows
        const i64 pf_time_l1i = cyc;
        const i64 pf_time_l1d = cyc;
        const i64 pf_time_l2 = cyc;

        // use cache inventory to build up prefetch sets
        if (conf_->reg_move.copy_l1i) {
            const AddrSet& i_blocks(cache_inv.get_icache_blocks());
            WSMDB(1)("%s: source L1I has %d tags for id %d\n", fname,
                     intsize(i_blocks), master_id);
            FOR_CONST_ITER(AddrSet, i_blocks, iter) {
                if (conf_->reg_move.dbp_filter_oracle && from_core->i_dbp &&
                    dbp_predict_dead(from_core->i_dbp, *iter)) {
                    app_info_->dbp_block_discarded();
                    continue;
                }
                bool pf_as_excl =
                    parent_->request_as_excl(*iter,
                                             cache_inv.block_seen_excl(*iter));
                PrefetchEntry pf_ent(*iter, PFT_Inst, pf_as_excl, false,
                                     WSMT_OracleCacheSimPF, -1);
                coll.add_prefetch(0, pf_time_l1i, 0, pf_ent);
            }
        }
        if (conf_->reg_move.copy_l1d) {
            const AddrSet& d_blocks(cache_inv.get_dcache_blocks());
            WSMDB(1)("%s: source L1D has %d tags for id %d\n", fname,
                     intsize(d_blocks), master_id);
            FOR_CONST_ITER(AddrSet, d_blocks, iter) {
                if (conf_->reg_move.dbp_filter_oracle && from_core->d_dbp &&
                    dbp_predict_dead(from_core->d_dbp, *iter)) { 
                    app_info_->dbp_block_discarded();
                    continue;
                }
                bool pf_as_excl =
                    parent_->request_as_excl(*iter,
                                             cache_inv.block_seen_excl(*iter));
                PrefetchEntry pf_ent(*iter, PFT_Data, pf_as_excl, false,
                                     WSMT_OracleCacheSimPF, -1);
                coll.add_prefetch(1, pf_time_l1d, 0, pf_ent);
            }
        }
        if (conf_->reg_move.copy_l2 && GlobalParams.mem.private_l2caches) {
            const AddrSet& l2_blocks(cache_inv.get_l2cache_blocks());
            WSMDB(1)("%s: source L2 has %d tags for id %d\n", fname,
                     intsize(l2_blocks), master_id);
            FOR_CONST_ITER(AddrSet, l2_blocks, iter) {
                if (conf_->reg_move.dbp_filter_oracle && from_core->l2_dbp &&
                    dbp_predict_dead(from_core->l2_dbp, *iter)) {
                    app_info_->dbp_block_discarded();
                    continue;
                }
                bool pf_as_excl =
                    parent_->request_as_excl(*iter,
                                             cache_inv.block_seen_excl(*iter));
                // Map all L2 stuff to "data" pathway
                PrefetchEntry pf_ent(*iter, PFT_Data, pf_as_excl, false,
                                     WSMT_OracleCacheSimPF, -1);
                coll.add_prefetch(1, pf_time_l2, 0, pf_ent);
            }
        }
        collected_queues_.reset(coll.collect());
    }

    i64 invoke(CBQ_Args *args) {
        const char *fname = "OraclePrefetchCB";
        CoreResources *to_core = parent_->get_target_core();
        WSMDB(1)("%s: time %s, start prefetch for A%d at C%d\n",
                 fname, fmt_now(), app_info_->app_id(), to_core->core_id);

        i64 start_time = 0;
        i64 expire_time = (conf_->reg_move.oracle_cs_pf.expire_cyc >= 0) ?
            (cyc + conf_->reg_move.oracle_cs_pf.expire_cyc) : I64_MAX;
        long xfer_bits = 0;
        PrefetchPlayerVec player_cbs;

        while (!collected_queues_->empty()) {
            scoped_ptr<PrefetchQueue>
                pf_queue(collected_queues_->remove_next_queue());
            sim_assert(!pf_queue->empty());
            start_time = MAX_SCALAR(start_time, pf_queue->ready_time());
            if (conf_->reg_move.oracle_cs_pf.dumb_addr_xfer) {
                int xfer_blocks = int(pf_queue->size());
                xfer_bits += xfer_blocks *
                    conf_->reg_move.oracle_cs_pf.dumb_bits_per_addr;
            }
            // not using address filter: blocks guaranteed unique already
            player_cbs.
                push_back(new PrefetchPlayerCB(scoped_ptr_release(pf_queue),
                                               NULL, to_core,
                                               expire_time));
        }

        if ((xfer_bits > 0) && !player_cbs.empty()) {
            OpTime xfer_time;
            estimate_bus_xfer_time(&xfer_time, xfer_bits);
            WSMDB(1)("%s: time %s, dumb_addr_xfer "
                     "combo op-time {%d,%d}; start_time %s", fname, 
                     fmt_now(), xfer_time.latency,
                     xfer_time.interval, fmt_i64(start_time));
            i64 xfer_done_time =
                corebus_access(to_core->reply_bus, xfer_time);
            start_time = MAX_SCALAR(start_time, xfer_done_time);
            WSMDB(1)(" -> %s\n", fmt_i64(start_time));
        }

        FOR_ITER(PrefetchPlayerVec, player_cbs, iter) {
            PrefetchPlayerCB *pf_player = *iter;
            // tranfers player ownership to parent_
            parent_->handoff_start_pf_player(pf_player, start_time);
        }
        player_cbs.clear();

        if (this->is_sched())
            this->sched_to_done();
        return -1;      // callback abandoned by callbackq
    }
};


// One-shot callback
// Has two modes: "discard arriving" or "discard departing", selected at
// create-time, that determines which core gets the discard operation.
class CacheDiscardCB : public AppSummaryCallback {
    bool discard_arriving_mode_;        // selects arriving vs. departing mode
public:
    CacheDiscardCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                   WSM_AppSummary *parent__, 
                   bool discard_arriving_mode)
        : AppSummaryCallback(conf__, app_info__, parent__),
          discard_arriving_mode_(discard_arriving_mode)
    { }
    i64 invoke(CBQ_Args *args) {
        const char *fname = "CacheDiscardCB";
        CoreResources *discard_core =
            (discard_arriving_mode_) ? parent_->get_target_core() :
            parent_->get_source_core();
        const unsigned cache_select_mask =
            CACHE_INJECT_L1I | CACHE_INJECT_L1D | CACHE_INJECT_L2;
        const bool downgrade_only = false;
        WSMDB(1)("%s: (%s) time %s, master_id %d C%d\n", fname,
                 (discard_arriving_mode_) ? "arriving" : "departing",
                 fmt_now(), app_info_->app_master_id(), discard_core->core_id);
        discard_core_cached_app(discard_core, app_info_->get_app(),
                                cache_select_mask, downgrade_only,
                                conf_->reg_move.discard_tlbs_too);
        // assert: "arriving" mode <=> immediate
        sim_assert(discard_arriving_mode_ == !this->is_sched());
        if (this->is_sched())
            this->sched_to_done();
        // callback abandoned by callbackq (in departing mode), return value
        // ignored in arriving mode
        return -1;
    }
};


// Two-shot callback: reschedules itself once (maybe)!
// At its first invocation, this bills the bus and reschedules itself after
// the transfer; at the second, it starts prefetching.  If there's no
// prefetching to be done, this won't reschedule itself at all.
class StreambufMigCB : public AppSummaryCallback {
    PFStreamExported *sb_exported_;     // owned; has C destroy func
    bool bus_xfer_billed_;
public:
    StreambufMigCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                   WSM_AppSummary *parent__)
        : AppSummaryCallback(conf__, app_info__, parent__),
          sb_exported_(NULL), bus_xfer_billed_(false)
    {
        const char *fname = "StreambufMigCB";
        CoreResources *from_core = parent__->get_source_core();
        const int master_id = app_info_->app_master_id();
        WSMDB(1)("%s: time %s, export masterid %d from C%d\n",
                 fname, fmt_now(), master_id, from_core->core_id);

        if (from_core->d_streambuf) {
            sb_exported_ = pfsg_export(from_core->d_streambuf, master_id);
            if (pfse_empty(sb_exported_)) {
                WSMDB(1)("%s: skipping empty streambuf export\n", fname);
                pfse_destroy(sb_exported_);
                sb_exported_ = NULL;
            }
        } else {
            WSMDB(1)("%s: C%d has no D-streambuf, skipping\n", fname,
                     from_core->core_id);
        }
    }
    ~StreambufMigCB() {
        pfse_destroy(sb_exported_);
    }
    i64 invoke(CBQ_Args *args) {
        const char *fname = "StreambufMigCB";
        CoreResources *to_core = parent_->get_target_core();
        i64 resched_time;
        if (!sb_exported_ || !to_core->d_streambuf) {
            WSMDB(1)("%s: time %s, streambuf ->C%d: no %s, "
                     "nothing to do\n", fname, fmt_now(), to_core->core_id,
                     (!sb_exported_) ? "export data" : "target streambuf");
            resched_time = -1;
        } else if (!bus_xfer_billed_) {
            long xfer_bits = pfse_estimate_size_bits(sb_exported_);
            OpTime xfer_time;
            estimate_bus_xfer_time(&xfer_time, xfer_bits);
            i64 bus_done_time = corebus_access(to_core->reply_bus, xfer_time);
            WSMDB(1)("%s: time %s, streambuf ->C%d xfer est %ld bits,"
                     " op-time {%d,%d}; resched at bus_done_time %s\n",
                     fname, fmt_now(),
                     to_core->core_id, xfer_bits, xfer_time.latency,
                     xfer_time.interval, fmt_i64(bus_done_time));
            bus_xfer_billed_ = true;
            resched_time = bus_done_time;
        } else {
            WSMDB(1)("%s: time %s, import streambuf state"
                     " ->C%d\n", fname, fmt_now(), to_core->core_id);
            pfsg_import(to_core->d_streambuf, sb_exported_,
                        conf_->reg_move.streambuf_mig.prefer_imported_streams,
                        conf_->reg_move.streambuf_mig.imports_win_ties);
            resched_time = -1;
        }
        if (resched_time < 0)
            this->sched_to_done();
        return resched_time;
    }
};


// One-shot callback
class StreambufStopCB : public AppSummaryCallback {
public:
    StreambufStopCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                    WSM_AppSummary *parent__)
        : AppSummaryCallback(conf__, app_info__, parent__)
    { }
    i64 invoke(CBQ_Args *args) {
        const char *fname = "StreambufStopCB";
        CoreResources *from_core = parent_->get_source_core();
        const int master_id = app_info_->app_master_id();
        WSMDB(1)("%s: time %s, stop C%d d_streambuf on master_id %d\n", fname,
                 fmt_now(), from_core->core_id, master_id);
        pfsg_stop_thread_pf(from_core->d_streambuf, master_id);
        this->sched_to_done();
        return -1;
    }
};


// One-shot callback.  Like OraclePrefetchCB, this schedules PrefetchPlayerCBs
// to perform later prefetches.
class SummarizePrefetchCB : public AppSummaryCallback {
    scoped_ptr<PrefetchCollection> collected_queues_;
    long xfer_bits_;

public:
    SummarizePrefetchCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                        WSM_AppSummary *parent__)
        : AppSummaryCallback(conf__, app_info__, parent__),
          xfer_bits_(-1)
    {
        const char *fname = "SummarizePrefetchCB";
        CoreResources *from_core = parent_->get_source_core();
        int app_id = app_info_->app_id();

        WSMDB(1)("%s: time %s, summarizing for A%d on C%d\n",
                 fname, fmt_now(), app_id, from_core->core_id);

        PrefetchCollector collector;
        if (WSM_ThreadCapture *threadcap = from_core->wsm_capture) {
            threadcap->summarize_and_expand(app_id, &collector, &xfer_bits_);
            sim_assert(xfer_bits_ >= 0);
            if (conf_->reg_move.summ_pf.flush_at_move) {
                threadcap->flush_tables(app_id);
            }
        }

        int coll_blocks = 0;
        collected_queues_.reset(collector.collect());
        FOR_CONST_ITER(PrefetchCollection, *collected_queues_, iter) {
            coll_blocks += int((*iter)->size());
        }
        WSMDB(1)("%s: collected %d blocks in %d queues,"
                 " xfer_bits %ld\n", fname, coll_blocks,
                 intsize(*collected_queues_), xfer_bits_);
    }

    i64 invoke(CBQ_Args *args) {
        const char *fname = "SummarizePrefetchCB";
        CoreResources *to_core = parent_->get_target_core();
        WSMDB(1)("%s: time %s, start prefetch for A%d at C%d\n",
                 fname, fmt_now(), app_info_->app_id(), to_core->core_id);

        i64 expire_time = (conf_->reg_move.summ_pf.expire_cyc >= 0) ?
            (cyc + conf_->reg_move.summ_pf.expire_cyc) : I64_MAX;
        PrefetchPlayerVec player_cbs;
        vector<i64> queue_ready_times;
        
        AddrSet *addr_filter = parent_->get_shared_addr_filter();
        // no need to clear; the filter was created anew for this
        // WSM_AppSummary instance

        while (!collected_queues_->empty()) {
            scoped_ptr<PrefetchQueue>
                pf_queue(collected_queues_->remove_next_queue());
            sim_assert(!pf_queue->empty());
            queue_ready_times.push_back(pf_queue->ready_time());
            player_cbs.
                push_back(new PrefetchPlayerCB(scoped_ptr_release(pf_queue),
                                               addr_filter, to_core,
                                               expire_time));
        }

        i64 xfer_done_time = 0;
        if ((xfer_bits_ > 0) && !player_cbs.empty()) {
            // adjust start time based on transfer size estimate
            OpTime xfer_time;
            estimate_bus_xfer_time(&xfer_time, xfer_bits_);
            WSMDB(1)("%s: time %s, xfer "
                     "combo op-time {%d,%d}; xfer_done_time %s", fname, 
                     fmt_now(), xfer_time.latency, xfer_time.interval,
                     fmt_i64(xfer_done_time));
            xfer_done_time =
                corebus_access(to_core->reply_bus, xfer_time);
            WSMDB(1)(" -> %s\n", fmt_i64(xfer_done_time));
        }

        sim_assert(player_cbs.size() == queue_ready_times.size());
        for (int i = 0; i < intsize(player_cbs); ++i) {
            PrefetchPlayerCB *player_cb = player_cbs[i];
            i64 start_time = queue_ready_times[i];
            start_time = MAX_SCALAR(start_time, xfer_done_time);
            // transfers player ownership to parent_
            parent_->handoff_start_pf_player(player_cb, start_time);
        }
        player_cbs.clear();
        queue_ready_times.clear();

        if (this->is_sched())
            this->sched_to_done();
        return -1;      // callback abandoned by callbackq
    }
};


// This is a one-shot callback
class FuturePrefetchCB : public AppSummaryCallback {

    // Hack: since we can't rewind a MemRefSeq_Reader, if we read until we
    // reach or exceed our target, we'll have gone too far; so, we'll stop if
    // we reach or exceed a commit count that's short of our target.  (Since a
    // record is generated for I-fetch operations, the max-run-len between
    // memory instrucitons isn't the only upper bound on this; keep in mind
    // that I-fetches of no-ops are omitted, though.)
    static const int kCommitLimitStopShort = 20;

    typedef map<mem_addr,unsigned> FutureMaskMap;
    typedef set<mem_addr> MemAddrSet;
    typedef vector<mem_addr> MemAddrVec;

    int block_bytes_;                   // power of two
    FutureMaskMap blocks_seen_;
    MemAddrVec blocks_order_;            // entries guaranteed unique
    MemAddrSet departing_blocks_;        // (empty if !intersect_departing)
    scoped_ptr<PrefetchCollection> collected_queues;

    void block_align(mem_addr *addr) const { (*addr) &= ~(block_bytes_ - 1); }

    // true <=> "loc_mask" added something not already seen for this block
    bool add_future_addr(mem_addr addr, unsigned loc_mask) {
        if (conf_->reg_move.future_pf.omit_i_blocks) {
            loc_mask &= ~CACHE_INJECT_L1I;
        }
        if (conf_->reg_move.future_pf.omit_d_blocks) {
            loc_mask &= ~CACHE_INJECT_L1D;
        }
        bool loc_has_new = false;
        if (loc_mask != 0) {
            block_align(&addr);
            unsigned *seen_mask = &blocks_seen_[addr];      // auto-inserts 0
            if (*seen_mask == 0) {
                // first time this block was mentioned
                blocks_order_.push_back(addr);
            }
            loc_has_new = (loc_mask & ~(*seen_mask)) != 0;
            *seen_mask |= loc_mask;
        }
        return loc_has_new;
    }

    // populates blocks_seen_ and blocks_order_ according to set_select policy
    void read_future_blocks(CoreResources *core_for_sizes);
    // edits blocks_seen_ and blocks_order_
    void deadblock_filter_future(CoreResources *source_core);
    // populates departing_blocks_
    void inventory_departing_blocks(const CoreCacheInventory& cache_inv);

    static int mem_seq_nops_before(const AppState *as, mem_addr pc);
    static bool mem_seq_sync_check(const MemRefSeq_Record& next_mem_ref,
                                   i64 next_app_inst, i64 next_app_commit,
                                   const AppState *as);

public:
    FuturePrefetchCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
                     WSM_AppSummary *parent__)
        : AppSummaryCallback(conf__, app_info__, parent__)
    {
        const char *fname = "FuturePrefetchCB";
        CoreResources *from_core = parent_->get_source_core();
        const int master_id = app_info_->app_master_id();
        WSMDB(1)("%s: time %s, A%d departing C%d\n", fname, fmt_now(),
                 app_info_->app_id(), from_core->core_id);

        block_bytes_ = 1 << GlobalParams.mem.cache_block_bytes_lg;

        CoreCacheInventory cache_inv(from_core, master_id);
        if (conf_->reg_move.future_pf.intersect_departing)
            inventory_departing_blocks(cache_inv);

        // we'd like to defer read_future_blocks() until invoke(), since it
        // has options that adjust sizes for the target core (which isn't
        // known until invoke).  however, deadblock_filter_future() requires
        // access to both the future block set, AND the deadblock predictor
        // state on the source core.  we'll punt here and just have
        // read_future_blocks() use the source core for cache size limits.
        // :(

        this->read_future_blocks(from_core);
        sim_assert(blocks_seen_.size() == blocks_order_.size());
        if (conf_->reg_move.dbp_filter_oracle) {
            this->deadblock_filter_future(from_core);
            sim_assert(blocks_seen_.size() == blocks_order_.size());
        }

        if (conf_->reg_move.future_pf.sort_blocks) {
            // sorts addresses, increasing order
            std::stable_sort(blocks_order_.begin(), blocks_order_.end());
        }

        PrefetchCollector coll;
        const int data_pf_queue_id =
            (conf_->reg_move.future_pf.split_pf_queues) ? 1 : 0;
        FOR_CONST_ITER(MemAddrVec, blocks_order_, iter) {
            if (!conf_->reg_move.future_pf.intersect_departing ||
                departing_blocks_.count(*iter)) {
                LongAddr addr(*iter, master_id);
                unsigned seen_mask = map_at(blocks_seen_, *iter);
                bool pf_as_excl =
                    parent_->request_as_excl(addr, 
                                             cache_inv.block_seen_excl(addr));
                if (seen_mask & CACHE_INJECT_L1I) {
                    PrefetchEntry pf_ent(addr, PFT_Inst, pf_as_excl, false,
                                         WSMT_FutureTracePF, -1);
                    coll.add_prefetch(0, cyc, 0, pf_ent);
                }
                if (seen_mask & ~CACHE_INJECT_L1I) {    // seen outside L1I
                    PrefetchEntry pf_ent(addr, PFT_Data, pf_as_excl, false,
                                         WSMT_FutureTracePF, -1);
                    coll.add_prefetch(data_pf_queue_id, cyc, 0, pf_ent);
                }
            }
        }

        collected_queues.reset(coll.collect());
    }

    i64 invoke(CBQ_Args *args) {
        const char *fname = "FuturePrefetchCB";
        CoreResources *to_core = parent_->get_target_core();
        WSMDB(1)("%s: time %s, start prefetch for A%d at C%d\n",
                 fname, fmt_now(), app_info_->app_id(), to_core->core_id);

        sim_assert(collected_queues->size() <= 2);
        i64 expire_time = I64_MAX;

        PrefetchPlayerVec player_cbs;
        while (!collected_queues->empty()) {
            scoped_ptr<PrefetchQueue>
                pf_queue(collected_queues->remove_next_queue());
            sim_assert(!pf_queue->empty());
            // not using address filter: blocks guaranteed unique already
            player_cbs.
                push_back(new PrefetchPlayerCB(scoped_ptr_release(pf_queue),
                                               NULL, to_core, expire_time));
        }

        FOR_ITER(PrefetchPlayerVec, player_cbs, iter) {
            PrefetchPlayerCB *pf_player = *iter;
            // tranfers player ownership to parent_
            parent_->handoff_start_pf_player(pf_player,
                                             pf_player->ready_time());
        }
        player_cbs.clear();

        if (this->is_sched())
            this->sched_to_done();
        return -1;      // callback abandoned by callbackq
    }
};


void
FuturePrefetchCB::
inventory_departing_blocks(const CoreCacheInventory& cache_inv)
{
    const char *fname = "FuturePrefetchCB::inventory_departing_blocks";
    const int master_id = app_info_->app_master_id();
    sim_assert(departing_blocks_.empty());
    const AddrSet& i_blocks(cache_inv.get_icache_blocks());
    WSMDB(2)("%s: source L1I has %d tags for id %d\n", fname,
             intsize(i_blocks), master_id);
    FOR_CONST_ITER(AddrSet, i_blocks, iter) {
        departing_blocks_.insert(iter->a);
    }
    const AddrSet& d_blocks(cache_inv.get_dcache_blocks());
    WSMDB(2)("%s: source L1D has %d tags for id %d\n", fname,
             intsize(d_blocks), master_id);
    FOR_CONST_ITER(AddrSet, d_blocks, iter) {
        departing_blocks_.insert(iter->a);
    }
    if (GlobalParams.mem.private_l2caches) {
        const AddrSet& l2_blocks(cache_inv.get_l2cache_blocks());
        WSMDB(2)("%s: source L2 has %d tags for id %d\n", fname,
                 intsize(l2_blocks), master_id);
        FOR_CONST_ITER(AddrSet, l2_blocks, iter) {
            departing_blocks_.insert(iter->a);
        }
    }
    WSMDB(2)("%s: inventory has %d unique blocks\n", fname,
             intsize(departing_blocks_));
    if (WSM_DEBUG_COND(3)) {
        printf("%s: dump of departing_blocks_ inventory:\n", fname);
        FOR_CONST_ITER(MemAddrSet, departing_blocks_, iter) {
            printf("  %s\n", fmt_x64(*iter));
        }
    }
}


// helper for mem_seq_sync_check(): given an appstate and a valid PC, 
// scan instruction memory and count the number of contiguous static 
// "no-op" instructions before that PC.
int
FuturePrefetchCB::
mem_seq_nops_before(const AppState *as, mem_addr pc)
{
    int run_len = 0;
    mem_addr scan_pc = pc - kInstSizeBytes;
    const StashData *stash_data;
    while (((stash_data = stash_decode_inst(as->stash, scan_pc)) != NULL) &&
           (stash_data->gen_flags & SGF_StaticNoop)) {
        ++run_len;
        scan_pc -= kInstSizeBytes;
    }
    return run_len;
}


struct SyncCheckCoverageStore {
    int case1_1;
    int case1_2;
    int case2_1;
    int case2_2a;
    int case2_2b;
    int case2_3a;
    int case2_3b;
} SyncCheckCoverage;

// minimal check to make sure that the trace file we're following
// resembles the current execution, at the given app_inst_num
bool
FuturePrefetchCB::
mem_seq_sync_check(const MemRefSeq_Record& next_mem_ref, i64 next_app_inst,
                   i64 next_app_commit, const AppState *as)
{
    const i64 delta_inst_count = next_mem_ref.app_inst_num() - next_app_inst;
    const i64 delta_commit_count = next_mem_ref.app_commit_num() -
        next_app_commit;
    const MemProfOp op_type = next_mem_ref.op_type();
    bool match;
    sim_assert(delta_inst_count >= 0);  // otherwise, our seek messed up

    // hack: our trace collector isn't perfect; it's driven from commit, so it
    // never sees no-op instructions if discard_static_noops is set.  for
    // InstFetch operations, if the fetch operation starts at a no-op, it will
    // either 1) be totally discarded, if it fetches only no-ops for an entire
    // block, or 2) it will reflect the first non-nop instruction fetched in
    // that block.  Unfortunately this messes up our nice invariant check; we
    // can detect the presence of a leading "nop-slide", but we can't tell
    // from the info in a MemRefSeq_Record where along the nop-slide the
    // InstFetch really started.
    int nop_slide_len = (op_type == MPO_InstFetch) ?
        mem_seq_nops_before(as, next_mem_ref.addr()) : 0;

    if (delta_inst_count == 0) {
        // simpler case: the next memory reference from the file is for the
        // next instruction to be emulated.  if the memory reference is for
        // I-fetch, then the address should match the next PC; otherwise,
        // the PCs should match.
        switch (op_type) {
        case MPO_InstFetch:
            match = (next_mem_ref.addr() == as->npc);
            ++SyncCheckCoverage.case1_1;
            break;
        case MPO_DataLoad:
        case MPO_DataStore:
            match = (next_mem_ref.pc() == as->npc);
            ++SyncCheckCoverage.case1_2;
            break;
        default:
            ENUM_ABORT(MemProfOp, op_type);
            match = false;
        }
    } else {
        // tricky case: the next memory reference is the earliest in the
        // file which comes *after* the next instruction to be emulated.
        // if all is well, this means that the next instruction is not
        // a memory instruction, and (during MemProf trace collection)
        // it wasn't at the start of a fetch block.  since "next_mem_ref"
        // is the next memory operation, there can be only 0 or 1 taken
        // branches between "next_app_inst" and "next_mem_ref", because
        // taken branches would trigger a new fetch operation.
        //
        // subcase 1: fetch block with next_app_inst also contains
        // next_mem_ref.  then op_type == DataLoad or DataStore, and
        // pc == as->npc + 4*delta_inst_count.
        // 
        // subcase 2a: fetch block with next_app_inst does not end in a taken
        // branch.  next_mem_ref must be the fetch of the fallthrough block;
        // op_type == InstFetch, operation pc==0, and
        // fetch addr == as->npc + 4*delta_inst_count
        //
        // subcase 2b: fetch block with next_app_inst does not end in a taken
        // branch, but is the target of a branch, AND contains contains only
        // no-ops.  next_mem_ref must be the fetch of the fallthrough block;
        // op_type == InstFetch, fetch addr == as->npc + 4*delta_inst_count.
        // HOWEVER, branch pc != 0 (otherwise it'd just be subcase 2a) and
        // nop_slide_len >= delta_inst_count.  Note that this isn't trivially
        // exclusive from subcase 3, and it's hard to tell, since we don't
        // necessarily know what the branch target itself was.  (they may
        // actually be exclusive, but I lack the brainpower to tell right now)
        //
        // subcase 3: fetch block with next_app_inst ends in a taken branch;
        // then the next op must be an InstFetch at the branch target,
        // and branch pc == as->npc + 4*(delta_inst_count - 1).
        // (note that this can be affected by the branch target being a
        // no-op; see note above at "nop_slide_len" assignment.)
        switch (op_type) {
        case MPO_InstFetch:
            if (next_mem_ref.pc()) {
                if ((nop_slide_len >= delta_inst_count) &&
                    (next_mem_ref.addr() ==
                     (as->npc + kInstSizeBytes * delta_inst_count))) {
                    // subcase 2b: this one is especially tricky, since we're
                    // not yet certain it doesn't overlap with subcase 3.
                    // we'll use a match itself as the evidence that it's this
                    // case, for lack of a decent proof.
                    match = true;
                    ++SyncCheckCoverage.case2_2b; 
                } else if (!nop_slide_len) {
                    // 3a: branch target not preceded by noops
                    match = (next_mem_ref.pc() ==
                             (as->npc +
                              kInstSizeBytes * (delta_inst_count - 1)));
                    ++SyncCheckCoverage.case2_3a;
                } else {
                    // 3b: branch target preceded by noops; relax check by
                    // solving for "delta_inst_count" and then doing a
                    // range check.
                    // (branch pc at end of block must be >= npc)
                    i64 delta_insts = next_mem_ref.pc() - as->npc;
                    delta_insts /= kInstSizeBytes;
                    // 3a check above is equivalent to
                    // "delta_insts == delta_inst_count - 1"; we'll then allow
                    // the extra distance from the nop slide
                    match = 
                        (next_mem_ref.pc() >= as->npc) &&
                        ((delta_insts % kInstSizeBytes) == 0) &&
                        (delta_insts >=
                             (delta_inst_count - 1 - nop_slide_len));
                    ++SyncCheckCoverage.case2_3b;
                }
            } else {
                // subcase 2a
                match = (next_mem_ref.addr() ==
                         (as->npc + kInstSizeBytes * delta_inst_count));
                ++SyncCheckCoverage.case2_2a;                
            }
            break;
        case MPO_DataLoad:
        case MPO_DataStore:
            // subcase 1
            match = (next_mem_ref.pc() ==
                     (as->npc + kInstSizeBytes * delta_inst_count));
            ++SyncCheckCoverage.case2_1;
            break;
        default:
            ENUM_ABORT(MemProfOp, op_type);
            match = false;
        }
    }

    if ((delta_commit_count < 0) ||
        (delta_commit_count > delta_inst_count)) {
        // commit-delta out of range; it should be [0..delta_inst_count] if
        // we're still sync'd with the trace.  negative values are obviously
        // bogus, and since each commit is also an app_inst step, 
        // we shouldn't have more commits than app_insts.
        match = false;
    }
    
    return match;
}


void
FuturePrefetchCB::read_future_blocks(CoreResources *core_for_sizes)
{
    const char *fname = "FuturePrefetchCB::read_future_blocks";
    AppState *as = app_info_->get_app();
    MemRefSeq_Reader *reader = app_info_->get_memref_reader();
    const i64 next_app_inst = as->stats.total_insts;
    const i64 next_app_commit = as->extra->total_commits;
    FuturePrefetchSelect set_select = conf_->reg_move.future_pf.set_select;

    WSMDB(2)("%s: time %s next_app_inst %s next_app_commit %s "
             "set_select %s\n", fname, fmt_now(),
             fmt_i64(next_app_inst), fmt_i64(next_app_commit),
             ENUM_STR(FuturePrefetchSelect, set_select));

    sim_assert(reader != NULL);

    MemRefSeq_Record mem_ref;   // re-used many times

    sim_assert(blocks_seen_.empty() && blocks_order_.empty());

    if (!reader->read_rec(&mem_ref)) {
        exit_printf("%s: MemRefSeq_Reader not ready, preparing scan for "
                    "inst #%s; time %s, clean_eof %d\n", fname,
                    fmt_i64(next_app_inst), fmt_now(), reader->clean_eof());
    }
    WSMDB(2)("%s: test record, read before scan: %s\n", fname,
             mem_ref.fmt().c_str());

    // mem_ref is now the record following last migration's scan;
    // we'll use it to make sure we didn't read so far ahead last time that
    // we consumed the records needed for this migration.
    if ((mem_ref.app_inst_num() >= next_app_inst) &&
        conf_->reg_move.future_pf.rewind_on_overrun) {
        // attempt to handle this, if allowed, by re-opening and seeking from
        // the start of the (probably-compressed) input file.  (we could avoid
        // this for the "RegularMove every 1M commits" experiments, but not
        // in the less-predictable STUMP experiments)
        app_info_->rewind_memref_reader();
        reader = app_info_->get_memref_reader();
        if (!reader->read_rec(&mem_ref)) {
            exit_printf("%s: MemRefSeq_Reader not ready, re-reading after "
                        "rewind; time %s, clean_eof %d\n", fname,
                        fmt_now(), reader->clean_eof());
        }
        WSMDB(2)("%s: test record, read after rewind: %s\n", fname,
                 mem_ref.fmt().c_str());
    }
    if (mem_ref.app_inst_num() >= next_app_inst) {
        exit_printf("%s: next MemRefSeq_Record in trace is for app inst %s, "
                    "beyond next_app_inst %s; overrun detected at time %s\n",
                    fname, fmt_i64(mem_ref.app_inst_num()), 
                    fmt_i64(next_app_inst), fmt_now());
    }

    if (!reader->scan_by_instnum(&mem_ref, next_app_inst)) {
        exit_printf("%s: MemRefSeq_Reader scan for inst #%s failed; time %s "
                    "clean_eof %d\n", fname, fmt_i64(next_app_inst),
                    fmt_now(), reader->clean_eof());
    }
    // mem_ref is now the first record with app_inst_num >= next_app_inst
    WSMDB(2)("%s: scan ok, next_app_inst %s, next PC %s, "
             "post-scan record: %s\n", fname, fmt_i64(next_app_inst),
             fmt_x64(as->npc), mem_ref.fmt().c_str());

    if (!mem_seq_sync_check(mem_ref, next_app_inst, next_app_commit, as)) {
        err_printf("%s: trace-to-emulate sync check failed! A%d "
                   "next_app_inst %s next_app_commit %s pc %s, mem_ref: %s\n",
                   fname,  as->app_id, fmt_i64(next_app_inst),
                   fmt_i64(next_app_commit), fmt_x64(as->npc),
                   mem_ref.fmt().c_str());
        if (!conf_->reg_move.future_pf.ignore_sync_check) {
            sim_abort();
        }
    }

    if (next_app_inst < mem_ref.app_inst_num()) {
        // If the next trace record is ahead of the next instruction, fake in
        // the I-access that will be needed to load the instruction (since we
        // know we're going to have to fetch it at the new core).  (We account
        // for this one access below in the FPS_InstDataSize case.)
        WSMDB(3)("%s: adding initial I-cache access for next PC %s\n",
                 fname, fmt_x64(as->npc));
        add_future_addr(as->npc, CACHE_INJECT_L1I);
    }

    i64 window_size = conf_->reg_move.future_pf.window_size;
    int unique_block_limit = cache_size_blocks(core_for_sizes->icache) + 
        cache_size_blocks(core_for_sizes->dcache) +
        ((GlobalParams.mem.private_l2caches) ? 
         cache_size_blocks(core_for_sizes->l2cache) : 0);
    i64 stop_at_commit_num = I64_MAX;

    if (conf_->reg_move.future_pf.limit_to_regmove_period) {
        sim_assert(kCommitLimitStopShort < conf_->reg_move.period);
        stop_at_commit_num = next_app_commit + (conf_->reg_move.period -
                                                kCommitLimitStopShort);
    }

    sim_assert(window_size >= 0);

    switch (set_select) {
    case FPS_InstCountWindow:
        {
            i64 limit_inst_num = next_app_inst + window_size;
            while ((intsize(blocks_seen_) < unique_block_limit) &&
                   (mem_ref.app_commit_num() < stop_at_commit_num) &&
                   (mem_ref.app_inst_num() < limit_inst_num)) {
                unsigned loc_mask =
                    (mem_ref.op_type() == MPO_InstFetch) ? CACHE_INJECT_L1I :
                    CACHE_INJECT_L1D;
                add_future_addr(mem_ref.addr(), loc_mask);
                if (!reader->read_rec(&mem_ref))
                    break; // we'll check for error/EOF afterward
            }
        }
        break;
    case FPS_MemRefWindow:
        {
            i64 refs_to_read = window_size;
            while ((intsize(blocks_seen_) < unique_block_limit) &&
                   (mem_ref.app_commit_num() < stop_at_commit_num) &&
                   (refs_to_read > 0)) {
                unsigned loc_mask =
                    (mem_ref.op_type() == MPO_InstFetch) ? CACHE_INJECT_L1I :
                    CACHE_INJECT_L1D;
                add_future_addr(mem_ref.addr(), loc_mask);
                --refs_to_read;
                if (!reader->read_rec(&mem_ref))
                    break;
            }
        }
        break;
    case FPS_UniqueBlockWindow:
        if (window_size > unique_block_limit)
            window_size = unique_block_limit;
        while ((intsize(blocks_seen_) < window_size) &&
               (mem_ref.app_commit_num() < stop_at_commit_num)) {
            unsigned loc_mask =
                (mem_ref.op_type() == MPO_InstFetch) ? CACHE_INJECT_L1I :
                CACHE_INJECT_L1D;
            add_future_addr(mem_ref.addr(), loc_mask);
            if (!reader->read_rec(&mem_ref))
                break;
        }
        break;
    case FPS_InstDataSize:
    case FPS_InstDataL2Size:
        {
            int inst_blocks_left = cache_size_blocks(core_for_sizes->icache);
            int data_blocks_left = cache_size_blocks(core_for_sizes->dcache) +
                (((set_select == FPS_InstDataL2Size) &&
                  GlobalParams.mem.private_l2caches) ? 
                 cache_size_blocks(core_for_sizes->l2cache) : 0);
            // treat window_size as a percentage by which to scale
            // cache sizes
            inst_blocks_left =
                i64(floor((window_size / 100.0) * inst_blocks_left));
            data_blocks_left =
                i64(floor((window_size / 100.0) * data_blocks_left));
            if (inst_blocks_left && (intsize(blocks_seen_) == 1)) {
                // account for faked initial I-access, above
                --inst_blocks_left;
            }
            WSMDB(2)("%s: for set_select %s, initial inst_blocks_left %d, "
                     "data_blocks_left %d\n", fname, 
                     ENUM_STR(FuturePrefetchSelect, set_select),
                     inst_blocks_left, data_blocks_left);
            while ((intsize(blocks_seen_) < unique_block_limit) &&
                   (mem_ref.app_commit_num() < stop_at_commit_num) &&
                   ((inst_blocks_left > 0) || (data_blocks_left > 0))) {
                if (mem_ref.op_type() == MPO_InstFetch) {
                    if ((inst_blocks_left > 0) &&
                        add_future_addr(mem_ref.addr(), CACHE_INJECT_L1I)) {
                        --inst_blocks_left;
                    }
                } else {
                    if ((data_blocks_left > 0) &&
                        add_future_addr(mem_ref.addr(), CACHE_INJECT_L1D)) {
                        --data_blocks_left;
                    }
                }
                if (!reader->read_rec(&mem_ref))
                    break;
            }
            WSMDB(2)("%s: for set_select %s, final inst_blocks_left %d, "
                     "data_blocks_left %d\n", fname, 
                     ENUM_STR(FuturePrefetchSelect, set_select),
                     inst_blocks_left, data_blocks_left);
        }
        break;
    default:
        ENUM_ABORT(FuturePrefetchSelect, set_select);
    }

    if (!reader->good()) {
        // EOF, I/O or error
        const char *desc;
        if (reader->data_error()) {
            desc = "data error";
        } else if (reader->clean_eof()) {
            desc = "exhausted(EOF)";
        } else {
            desc = "failed mysteriously";
        }
        exit_printf("%s: input %s during future scan, time %s.  A%d "
                    "next_app_inst %s pc %s, mem_ref: %s\n", fname, desc,
                    fmt_now(), as->app_id, fmt_i64(next_app_inst),
                    fmt_x64(as->npc), mem_ref.fmt().c_str());
    }

    sim_assert(blocks_seen_.size() == blocks_order_.size());
    WSMDB(2)("%s: sequence build complete; size %d blocks, "
             "last app_inst_num read was %s\n", fname, intsize(blocks_seen_),
             fmt_i64(mem_ref.app_inst_num()));
    if (WSM_DEBUG_COND(3)) {
        WSMDB(3)("%s: future block sequence and masks:\n", fname);
        FOR_CONST_ITER(MemAddrVec, blocks_order_, order_iter) {
            unsigned mask = map_at(blocks_seen_, *order_iter);
            printf("  %s -> 0x%x\n", fmt_x64(*order_iter), mask);
        }
    }
}


void
FuturePrefetchCB::deadblock_filter_future(CoreResources *source_core)
{
    const char *fname = "FuturePrefetchCB::deadblock_filter_future";
    const int master_id = app_info_->app_master_id();
    MemAddrSet to_remove;

    WSMDB(2)("%s: %d blocks to consider\n", fname, intsize(blocks_seen_));

    FOR_ITER(FutureMaskMap, blocks_seen_, mask_iter) {
        LongAddr block_addr(mask_iter->first, master_id);
        unsigned mask = mask_iter->second;
        if ((mask & CACHE_INJECT_L1I) && source_core->i_dbp &&
            dbp_predict_dead(source_core->i_dbp, block_addr)) {
            app_info_->dbp_block_discarded();
            mask &= ~CACHE_INJECT_L1I;
        }
        if ((mask & CACHE_INJECT_L1D) && source_core->d_dbp &&
            dbp_predict_dead(source_core->d_dbp, block_addr)) {
            app_info_->dbp_block_discarded();
            mask &= ~CACHE_INJECT_L1D;
        }
        if ((mask & CACHE_INJECT_L2) && source_core->l2_dbp &&
            dbp_predict_dead(source_core->l2_dbp, block_addr)) {
            app_info_->dbp_block_discarded();
            mask &= ~CACHE_INJECT_L2;
        }
        if (!source_core->l2_dbp &&
            (mask != mask_iter->second) &&
            !(mask & (CACHE_INJECT_L1D | CACHE_INJECT_L1I))) {
            // If we lack an L2 deadblock predictor, and we've predicted the
            // L1 blocks dead, we'll go ahead and remove any L2 bit.
            mask &= ~CACHE_INJECT_L2;
        }
        if (mask != mask_iter->second) {
            mask_iter->second = mask;
            if (!(mask & (CACHE_INJECT_L1I | CACHE_INJECT_L1D |
                          CACHE_INJECT_L2))) {
                to_remove.insert(mask_iter->first);
            }
        }
    }

    if (!to_remove.empty()) {
        MemAddrVec new_order;           // will replace blocks_order_
        FOR_CONST_ITER(MemAddrVec, blocks_order_, iter) {
            if (to_remove.count(*iter)) {
                WSMDB(3)("%s: removing block: %s\n", fname, fmt_mem(*iter));
                blocks_seen_.erase(*iter);      // (and don't add to new_order)
            } else {
                new_order.push_back(*iter);     // keep block
            }
        }
        blocks_order_.swap(new_order);
    }

    WSMDB(2)("%s: %d blocks removed, %d remaining\n", fname,
             intsize(to_remove), intsize(blocks_seen_));
}


// Multi-stage callback: may reschedule itself!
// (based on StreambufMigCB)
class TLBCopyCB : public AppSummaryCallback {
    int tlb_injects_per_cyc_;   // shortcut; also, uses INT_MAX for inf
    LongAddr *itlb_export_;     // malloc'd, or NULL
    int itlb_export_size_;      // #elts in itlb_export_
    LongAddr *dtlb_export_;     // malloc'd, or NULL
    int dtlb_export_size_;      // #elts in dtlb_export_
    int next_itlb_inject_;      // index into itlb_export_
    int next_dtlb_inject_;      // index into dtlb_export_
    bool bus_xfer_done_;        // flag: bus already billed (or wasn't needed)
public:
    TLBCopyCB(const WSM_Config *conf__, WSM_CoordPerApp *app_info__,
              WSM_AppSummary *parent__) 
        : AppSummaryCallback(conf__, app_info__, parent__),
          itlb_export_(NULL), itlb_export_size_(0),
          dtlb_export_(NULL), dtlb_export_size_(0),
          next_itlb_inject_(0), next_dtlb_inject_(0),
          bus_xfer_done_(false)
    {
        const char *fname = "TLBCopyCB";
        CoreResources *from_core = parent__->get_source_core();
        const int master_id = app_info_->app_master_id();
        tlb_injects_per_cyc_ = conf_->reg_move.tlb_copy.injects_per_cyc;
        if (tlb_injects_per_cyc_ < 0)
            tlb_injects_per_cyc_ = INT_MAX;
        if (conf_->reg_move.tlb_copy.xfers_are_free)
            bus_xfer_done_ = true;
        WSMDB(1)("%s: time %s, TLB export for A%d from C%d; ",
                 fname, fmt_now(), app_info_->app_id(), from_core->core_id);

        if (conf_->reg_move.tlb_copy.copy_itlb)
            itlb_export_ = tlb_get_tags(from_core->itlb, master_id,
                                        &itlb_export_size_);
        if (conf_->reg_move.tlb_copy.copy_dtlb)
            dtlb_export_ = tlb_get_tags(from_core->dtlb, master_id,
                                        &dtlb_export_size_);
        WSMDB(1)("dumped %d itlb, %d dtlb entries\n", itlb_export_size_,
                 dtlb_export_size_);
        sim_assert(itlb_export_size_ >= 0);
        sim_assert(dtlb_export_size_ >= 0);
    }
    ~TLBCopyCB() {
        free(itlb_export_);
        free(dtlb_export_);
    }
    i64 invoke(CBQ_Args *args) {
        const char *fname = "TLBCopyCB";
        CoreResources *to_core = parent_->get_target_core();
        i64 resched_time;
        WSMDB(1)("%s: time %s, TLB inject for A%d to C%d; ",
                 fname, fmt_now(), app_info_->app_id(), to_core->core_id);
        if (!bus_xfer_done_) {
            // bill for a bus transfer of all virtual and physical addresses
            // (we really should pipeline transfers and injects)
            int virt_bits_per_ent = 64 - GlobalParams.mem.page_bytes_lg;
            int phys_bits_per_ent =
                conf_->reg_move.tlb_copy.phys_addr_bits -
                GlobalParams.mem.page_bytes_lg;
            sim_assert(virt_bits_per_ent > 0);
            sim_assert(phys_bits_per_ent > 0);
            long xfer_bits = (itlb_export_size_ + dtlb_export_size_) * 
                (virt_bits_per_ent + phys_bits_per_ent);
            OpTime xfer_time;
            estimate_bus_xfer_time(&xfer_time, xfer_bits);
            i64 bus_done_time = corebus_access(to_core->reply_bus, xfer_time);
            WSMDB(1)("bus xfer est %ld bits, op-time {%d,%d}, "
                     "bus_done_time %s; ",
                     xfer_bits, xfer_time.latency,
                     xfer_time.interval, fmt_i64(bus_done_time));
            bus_xfer_done_ = true;
            // reschedule injection for after
            resched_time = bus_done_time;
        } else {
            // inject entries until we've done them all, then exit
            int itlb_this_cyc = 0, dtlb_this_cyc = 0;
            while ((next_itlb_inject_ < itlb_export_size_) &&
                   ((itlb_this_cyc < tlb_injects_per_cyc_))) {
                tlb_inject(to_core->itlb, cyc,
                           itlb_export_[next_itlb_inject_].a,
                           itlb_export_[next_itlb_inject_].id);
                ++next_itlb_inject_;
                ++itlb_this_cyc;
            }
            while ((next_dtlb_inject_ < dtlb_export_size_) &&
                   ((dtlb_this_cyc < tlb_injects_per_cyc_))) {
                tlb_inject(to_core->dtlb, cyc,
                           dtlb_export_[next_dtlb_inject_].a,
                           dtlb_export_[next_dtlb_inject_].id);
                ++next_dtlb_inject_;
                ++dtlb_this_cyc;
            }
            WSMDB(1)("%d itlb, %d dtlb injects this cyc, %d/%d entries left; ",
                     itlb_this_cyc, dtlb_this_cyc,
                     (itlb_export_size_ - next_itlb_inject_),
                     (dtlb_export_size_ - next_dtlb_inject_));
            if ((next_itlb_inject_ < itlb_export_size_) ||
                (next_dtlb_inject_ < dtlb_export_size_)) {
                // there are still entries left; continue next cycle
                resched_time = cyc + 1;
            } else {
                // all done!
                resched_time = -1;
            }
        }
        if (resched_time >= 0) {
            WSMDB(1)("rescheduling at time %s\n", fmt_i64(resched_time));
        } else {
            WSMDB(1)("all done\n");
            this->sched_to_done();
        }
        return resched_time;
    }
};


extern "C" void regmove_cb_hook(void);
void
regmove_cb_hook(void)
{
}


// This one gets rescheduled over and over, and never self-terminates
class WSM_Coord::RegularMoveCB : public CBQ_Callback {
    WSM_Coord& wc;
public:
    RegularMoveCB(WSM_Coord& coord_) : wc(coord_) {
    }
    i64 invoke(CBQ_Args *args) {
        regmove_cb_hook();      // for easy breakpoints
        AppStateExtras *ase = wc.reg_move.as->extra;
        const int from_core_id = wc.reg_move.prev_targ_core;
        const int to_core_id = INCR_WRAP(from_core_id, CoreCount);
        const int nomad_app_id = wc.conf_.reg_move.app_id;
        //CoreResources *from_core = Cores[from_core_id];
        //CoreResources *to_core = Cores[to_core_id];
        wc.reg_move.prev_targ_core = to_core_id;
        printf("RegularMove: time %s commits %s, moving A%d from C%d to C%d\n",
               fmt_now(), fmt_i64(ase->total_commits),
               nomad_app_id, from_core_id, to_core_id);
        if (GlobalPFAudit)
            pfa_note_migrate(GlobalPFAudit, cyc, wc.reg_move.as, from_core_id,
                             to_core_id);
        appmgr_migrate_app_soon(GlobalAppMgr, nomad_app_id,
                                wc.reg_move.prev_targ_core,
                                CtxHaltStyle_Fast, NULL);
        if (wc.conf_.reg_move.pause_cosched) {
            wc.resume_cosched_threads(from_core_id, nomad_app_id);
            wc.pause_cosched_threads(to_core_id, nomad_app_id);
        }
        i64 next_sched_point = (wc.conf_.reg_move.period_as_commits) ?
            (ase->total_commits + wc.conf_.reg_move.period) :
            (cyc + wc.conf_.reg_move.period);
        sim_assert(next_sched_point >= 0);
        return next_sched_point;
    }
};


// one-shot callback; multiple outstanding instances possible
class WSM_Coord::CoschedHaltedCB : public CBQ_Callback {
    WSM_Coord *wc;      // NULL => parent is gone for some reason
    context *ctx;
    AppState *app;
public:
    CoschedHaltedCB(WSM_Coord& wc_, context *ctx_, AppState *app_)
        : wc(&wc_), ctx(ctx_), app(app_) {
        sim_assert(wc != NULL);
        sim_assert(ctx != NULL);
        sim_assert(app != NULL);
    }
    i64 invoke(CBQ_Args *args) {
        if (!wc) {
            appmgr_signal_idlectx(GlobalAppMgr, ctx);
            return -1;
        }
        if (!wc->paused_contexts_.insert(make_pair(ctx, app)).second) {
            AppState *other_app = wc->paused_contexts_[ctx];
            abort_printf("CoschedHaltedCB: duplicate context ID T%d, apps "
                         "A%d and A%d?\n", ctx->id, app->app_id,
                         other_app->app_id);
        }
        if (!wc->reg_move.pend_cb.cosched_halted.erase(this)) {
            abort_printf("unlinked CoschedHaltedCB, T%d A%d\n",
                         ctx->id, app->app_id);
        }
        return -1;
    }
    void parent_wsm_deleted() {
        wc = NULL;
    }
};


// one-shot callback; pause a background app after it's been migrated
// to co-reside with the nomad (based on CoschedHaltedCB)
class WSM_Coord::BackgroundPauseCB : public CBQ_Callback {
    WSM_Coord *wc_;     // NULL => parent is gone for some reason
    AppState *app_;
    int target_core_;   // expected target core, or -1 for "don't care"
public:
    BackgroundPauseCB(WSM_Coord& wc__, AppState *app__, int target_core__)
        : wc_(&wc__), app_(app__), target_core_(target_core__) {
        sim_assert(wc_ != NULL);
        sim_assert(app_ != NULL);
    }
    i64 invoke(CBQ_Args *args) {
        if (!wc_) {
            // app is running; should be safe to just leave it alone
            return -1;
        }
        int ctx_id_to_pause = app_->extra->last_go_ctx_id;
        sim_assert(IDX_OK(ctx_id_to_pause, CtxCount));
        context *ctx = Contexts[ctx_id_to_pause];
        assert_ifthen(target_core_ >= 0,
                      ctx->core->core_id == target_core_);

        // the rest is a single-thread excerpt from pause_cosched_threads()
        WSM_Coord::CoschedHaltedCB *halt_done_cb =
            new CoschedHaltedCB(*wc_, ctx, app_);
        context_halt_cb(ctx, CtxHaltStyle_Fast, halt_done_cb);
        wc_->reg_move.pend_cb.cosched_halted.insert(halt_done_cb);
        return -1;      // destroy callback at return
    }
    void parent_wsm_deleted() {
        wc_ = NULL;
    }
};


// This one gets rescheduled over and over, and never self-terminates
class WSM_Coord::BackgroundStirCB : public CBQ_Callback {
    // PolicySchedule maps app_id->core_id; core_id==-1
    // means "not running".  (warning: missing app_id isn't always
    // handled well, it may act as "ignore" or "not running" in different
    // places!)
    typedef std::map<int,int> PolicySchedule;
    typedef void (BackgroundStirCB::*PolicyMethodPtr)
        (const PolicySchedule *in, PolicySchedule *out) const;

    WSM_Coord& wc_;
    int nomad_app_id_;                  // for convenience
    AppState *nomad_app_;
    BGStirPolicy policy_;
    PolicyMethodPtr policy_impl_;
    NoDefaultCopy nocopy;

    string fmt_sched(const PolicySchedule *sched) const;
    void read_current_sched(PolicySchedule *out, bool include_paused) const;
    void apply_changed_sched(const PolicySchedule *curr_sched,
                             const PolicySchedule *new_sched);

    void policy_none(const PolicySchedule *in, PolicySchedule *out) const;
    void policy_rotate(const PolicySchedule *in, PolicySchedule *out) const;
    void policy_countermove(const PolicySchedule *in,
                            PolicySchedule *out) const;
    void policy_diagonalswap(const PolicySchedule *in,
                             PolicySchedule *out) const;

public:
    BackgroundStirCB(WSM_Coord& coord_)
        : wc_(coord_), nomad_app_id_(wc_.conf_.reg_move.app_id),
          nomad_app_(wc_.reg_move.as),
          policy_(wc_.conf_.reg_move.bg_stir.policy),
          policy_impl_(NULL) {
        switch (policy_) {
        case BGStir_None:
            policy_impl_ = &BackgroundStirCB::policy_none;
            break;
        case BGStir_Rotate:
            policy_impl_ = &BackgroundStirCB::policy_rotate;
            break;
        case BGStir_CounterMove:
            policy_impl_ = &BackgroundStirCB::policy_countermove;
            break;
        case BGStir_DiagonalSwap:
            policy_impl_ = &BackgroundStirCB::policy_diagonalswap;
            break;
        default:
            ENUM_ABORT(BGStirPolicy, policy_);
        }
    }
    i64 invoke(CBQ_Args *args) {
        const char *fname = "BackgroundStirCB::invoke";
        AppStateExtras *ase = nomad_app_->extra;
        PolicySchedule curr_sched, new_sched;
        sim_assert(policy_impl_ != NULL);

        printf("%s: time %s commits %s, current sched: ", fname, 
               fmt_now(), fmt_i64(ase->total_commits));
        this->read_current_sched(&curr_sched, true);
        printf("%s\n", this->fmt_sched(&curr_sched).c_str());

        // whee, method pointer syntax
        (this->*policy_impl_)(&curr_sched, &new_sched);
        printf("%s: new sched: %s\n", fname,
               this->fmt_sched(&new_sched).c_str());

        this->apply_changed_sched(&curr_sched, &new_sched);

        i64 next_sched_point = (wc_.conf_.reg_move.period_as_commits) ?
            (ase->total_commits + wc_.conf_.reg_move.bg_stir.period) :
            (cyc + wc_.conf_.reg_move.bg_stir.period);
        sim_assert(next_sched_point >= 0);
        return next_sched_point;
    }
};


string
WSM_Coord::BackgroundStirCB::fmt_sched(const PolicySchedule *sched) const
{
    typedef set<int> AppIDSet;
    typedef map<int,AppIDSet> InverseSched;
    InverseSched core2apps;  
    FOR_CONST_ITER(PolicySchedule, *sched, iter) {
        core2apps[iter->second].insert(iter->first);
    }

    AppIDSet paused_apps;
    FOR_CONST_ITER(PausedCtxMap, wc_.paused_contexts_, iter) {
        paused_apps.insert(iter->second->app_id);
    }

    ostringstream out;
    out << "[";
    int cores_printed = 0;
    FOR_CONST_ITER(InverseSched, core2apps, core_iter) {
        const AppIDSet& apps = core_iter->second;
        if (cores_printed > 0)
            out << " ";
        out << core_iter->first << ":";
        int core_apps_printed = 0;
        if (apps.count(nomad_app_id_)) {
            out << "N";
            ++core_apps_printed;
        }
        FOR_CONST_ITER(AppIDSet, apps, app_iter) {
            int app_id = *app_iter;
            if (app_id != nomad_app_id_) {
                if (core_apps_printed > 0)
                    out << ",";
                out << app_id;
                if (paused_apps.count(app_id))
                    out << "(P)";
                ++core_apps_printed;
            }
        }
        ++cores_printed;
    }
    out << "]";
    return out.str();
}


void
WSM_Coord::BackgroundStirCB::read_current_sched(PolicySchedule *out,
                                                bool include_paused) const
{
    out->clear();
    for (int core_id = 0 ; core_id < CoreCount; ++core_id) {
        const CoreResources * restrict core = Cores[core_id];
        for (int core_ctx_id = 0; core_ctx_id < core->n_contexts;
             ++core_ctx_id) {
            const context * restrict ctx = core->contexts[core_ctx_id];
            if (ctx->as) {
                map_put_uniq(*out, ctx->as->app_id, core_id);
            } else if (include_paused) {
                const AppState *paused_app = 
                    map_at_default(wc_.paused_contexts_,
                                   const_cast<context *>(ctx), NULL);
                if (paused_app) {
                    map_put_uniq(*out, paused_app->app_id, core_id);
                }
            }
        }
    }
}


void
WSM_Coord::BackgroundStirCB::
        apply_changed_sched(const PolicySchedule *curr_sched,
                            const PolicySchedule *new_sched)
{
    const char *fname = "BackgroundStirCB::apply_changed_sched";

    map<int,int> paused_apps;   // app_id -> context_id
    FOR_CONST_ITER(PausedCtxMap, wc_.paused_contexts_, iter) {
        map_put_uniq(paused_apps, iter->second->app_id, iter->first->id);
    }

    int nomad_curr_core = map_at(*curr_sched, nomad_app_id_);
    sim_assert(nomad_curr_core >= 0);

    {
        int test_new_core = map_at_default(*new_sched, nomad_app_id_, -1);
        if (test_new_core != nomad_curr_core) {
            abort_printf("%s: BG stir schedule attempting to change nomad!  "
                         "nomad_curr_core %d, new_sched -> %d\n", fname,
                         nomad_curr_core, test_new_core);
        }
    }
        
    PolicySchedule changed_apps;        // app id with change -> new core or -1
    //set<int> contexts_to_halt;

    FOR_CONST_ITER(PolicySchedule, *new_sched, new_iter) {
        int app_id = new_iter->first;
        int new_core_id = new_iter->second;
        int old_core_id = map_at_default(*curr_sched, app_id, -1);
        if (new_core_id != old_core_id) {
            if (app_id == nomad_app_id_) {
                abort_printf("attempting to move nomad app %d via "
                             "BackgroundStirCB, something's wrong\n", 
                             nomad_app_id_);
            }
            map_put_uniq(changed_apps, app_id, new_core_id);
            // if (old_core_id >= 0) {
            //     const AppState * restrict old_app =
            //         appstate_lookup_id(app_id);
            //     sim_assert(old_app != NULL);
            //     int old_ctx_id = old_app->extra->last_go_ctx_id;
            //     sim_assert(IDX_OK(old_ctx_id, CtxCount));
            //     sim_assert(Contexts[old_ctx_id]->core ==
            //                Cores[old_core_id]);
            //     if (!contexts_to_halt.insert(old_ctx_id).second) {
            //         abort_printf("duplicate old_ctx_id %d\n", old_ctx_id);
            //     }
            // }
        }
    }

    if (debug) {
        printf("%s: old %s new %s -> changed %s\n", fname,
               this->fmt_sched(curr_sched).c_str(),
               this->fmt_sched(new_sched).c_str(),
               this->fmt_sched(&changed_apps).c_str());
        // printf(" contexts_to_halt [");
        // FOR_CONST_ITER(set<int>, contexts_to_halt, iter) {
        //     printf(" %d", *iter);
        // }
        // printf("]\n");
    }

    FOR_CONST_ITER(PolicySchedule, changed_apps, changed_iter) {
        int app_id = changed_iter->first;
        if (paused_apps.count(app_id)) {
            context *unpause_ctx = Contexts[paused_apps[app_id]];
            // For paused apps which we're moving, we'll unpause them, but set
            // them to fetch well into the future; the coming migrate
            // operation will kick in before they actually get a chance to
            // fetch.  (This will get all screwed up if a halt is pending from
            // pause_cosched_threads(); we're manually keeping them spaced
            // apart in sim-time.)
            wc_.paused_contexts_.erase(unpause_ctx);
            context_go(unpause_ctx, appstate_lookup_id(app_id), cyc + 100);
        }
    }

    FOR_CONST_ITER(PolicySchedule, changed_apps, changed_iter) {
        int app_id = changed_iter->first;
        int new_core_id = changed_iter->second;
        // appmgr_alter_mutablemap_sched(GlobalAppMgr, app_id, new_core_id);
        CBQ_Callback *post_migrate_cb = NULL;
        if ((new_core_id == nomad_curr_core) &&
            wc_.conf_.reg_move.pause_cosched) {
            // set up a callback to pause this BG app after swap-in has
            // completed at the new core
            AppState * app_to_pause = appstate_lookup_id(app_id);
            post_migrate_cb = new BackgroundPauseCB(wc_, app_to_pause,
                                                    new_core_id);
        }
        appmgr_migrate_app_soon(GlobalAppMgr, app_id, new_core_id,
                                CtxHaltStyle_Fast, post_migrate_cb);
    }
}


void
WSM_Coord::BackgroundStirCB::policy_none(const PolicySchedule *in,
                                         PolicySchedule *out) const
{
    *out = *in;
}


void
WSM_Coord::BackgroundStirCB::policy_rotate(const PolicySchedule *in,
                                           PolicySchedule *out) const
{
    // nomad is rotated "clockwise" on its own; we'll rotate background
    // apps "counter-clockwise", leaving the nomad alone
    FOR_CONST_ITER(PolicySchedule, *in, in_iter) {
        int app_id = in_iter->first;
        int curr_core = in_iter->second;
        int new_core = (app_id == nomad_app_id_) ? curr_core :
            DECR_WRAP(curr_core, CoreCount);
        map_put_uniq(*out, app_id, new_core);
    }
}


void
WSM_Coord::BackgroundStirCB::policy_countermove(const PolicySchedule *in,
                                                PolicySchedule *out) const
{
    // nomad is rotated "clockwise" on its own; at each step, we'll move one
    // BG application from the core the nomad just moved to, to the core it
    // just came from.  (Note that this was designed to use one fewer
    // background thread than the other schemes.)
    int nomad_curr_core = map_at(*in, nomad_app_id_);
    sim_assert(nomad_curr_core >= 0);

    int bg_app_on_nomad_core = -1;
    FOR_CONST_ITER(PolicySchedule, *in, in_iter) {
        if ((in_iter->first != nomad_app_id_) &&
            (in_iter->second == nomad_curr_core)) {
            bg_app_on_nomad_core = in_iter->first;
            break;
        }
    }

    *out = *in;
    if (bg_app_on_nomad_core >= 0) {
        int new_core = DECR_WRAP(nomad_curr_core, CoreCount);
        (*out)[bg_app_on_nomad_core] = new_core;
    }
}


void
WSM_Coord::BackgroundStirCB::policy_diagonalswap(const PolicySchedule *in,
                                                 PolicySchedule *out) const
{
    // nomad is rotated "clockwise" on its own; at each step, we'll take one
    // BG application from the core the nomad just moved to, and swap it with
    // one BG application from the core "diagonally opposite" that core.
    // (Note that this was designed to work for even numbers of cores only;
    // the "diagonally opposite" relation doesn't line up right for odd core
    // counts.)
    int nomad_curr_core = map_at(*in, nomad_app_id_);
    sim_assert(nomad_curr_core >= 0);
    int diagonal_core = (nomad_curr_core + (CoreCount / 2)) % CoreCount;

    int bg_app_on_nomad_core = -1;
    FOR_CONST_ITER(PolicySchedule, *in, in_iter) {
        if ((in_iter->first != nomad_app_id_) &&
            (in_iter->second == nomad_curr_core)) {
            bg_app_on_nomad_core = in_iter->first;
            break;
        }
    }

    int bg_app_on_diagonal_core = -1;
    if (diagonal_core != nomad_curr_core) {
        FOR_CONST_ITER(PolicySchedule, *in, in_iter) {
            if (in_iter->second == diagonal_core) {
                sim_assert(in_iter->first != nomad_app_id_);
                bg_app_on_diagonal_core = in_iter->first;
                break;
            }
        }
    }

    *out = *in;
    if (bg_app_on_nomad_core >= 0) {
        (*out)[bg_app_on_nomad_core] = diagonal_core;
    }
    if (bg_app_on_diagonal_core >= 0) {
        (*out)[bg_app_on_diagonal_core] = nomad_curr_core;
    }
}


// one-shot callback to get RegularMove "nomad" started the first time
class WSM_Coord::RegularMoveStartupCB : public CBQ_Callback {
    WSM_Coord& wc;
    RegularMoveCB *move_cb;     // callback to pass on for future scheduling
    NoDefaultCopy nocopy;
public:
    RegularMoveStartupCB(WSM_Coord& coord_, RegularMoveCB *move_cb_)
        : wc(coord_), move_cb(move_cb_) { }
    ~RegularMoveStartupCB() { delete move_cb; }
    i64 invoke(CBQ_Args *args) {
        int to_core = wc.reg_move.prev_targ_core;
        // Don't update prev_targ_core the first time around
        WSMDB(1)("RegularMove: time %s, starting nomad A%d on C%d\n",
                 fmt_now(), wc.conf_.reg_move.app_id, to_core);

        if (wc.conf_.reg_move.bg_stir.enable) {
            sim_assert(!wc.reg_move.pend_cb.bg_stirrer);
            wc.reg_move.pend_cb.bg_stirrer = new BackgroundStirCB(wc);
        }

        if (wc.conf_.reg_move.period_as_commits) {
            AppStateExtras *ase = wc.reg_move.as->extra;
            callbackq_enqueue(ase->watch.commit_count,
                              ase->total_commits + wc.conf_.reg_move.period,
                              move_cb);
            if (wc.conf_.reg_move.bg_stir.enable) {
                callbackq_enqueue(ase->watch.commit_count,
                                  ase->total_commits +
                                  wc.conf_.reg_move.bg_stir.period_offset,
                                  wc.reg_move.pend_cb.bg_stirrer);
            }
        } else {
            callbackq_enqueue(GlobalEventQueue, cyc + wc.conf_.reg_move.period,
                              move_cb);
            if (wc.conf_.reg_move.bg_stir.enable) {
                callbackq_enqueue(GlobalEventQueue,
                                  cyc +
                                  wc.conf_.reg_move.bg_stir.period_offset,
                                  wc.reg_move.pend_cb.bg_stirrer);
            }
        }

        sim_assert(!wc.reg_move.pend_cb.mover);
        wc.reg_move.pend_cb.mover = move_cb;
        move_cb = NULL;

        // The first migration (to the next core) is scheduled, now get the
        // "nomad" started ASAP.  (This is needed when the trigger for
        // migrations is dependent on the nomad already executing, e.g. using
        // commit counts.)
        appmgr_migrate_app_soon(GlobalAppMgr, wc.conf_.reg_move.app_id,
                                to_core, CtxHaltStyle_Fast, NULL);
        wc.reg_move.pend_cb.starter = NULL;    // deleted after return
        return -1;      // one-shot
    }
};


WSM_AppSummary::WSM_AppSummary(const WSM_Config *conf,
                               CallbackQueue *play_time_queue,
                               AppState *app,
                               CoreResources *source_core)
    : conf_(conf), play_time_queue_(play_time_queue), as_(app),
      summary_source_core_(source_core), create_time_(cyc),
      started_(false), activate_target_core_(NULL)
{
}


void
WSM_AppSummary::start_for_activate(CoreResources *target_core)
{
    const char *fname = "WSM_AppSummary::start_for_activate";

    WSMDB(1)("%s: time %s, activating summary for A%d (created %s)\n", fname,
             fmt_now(), as_->app_id, fmt_i64(create_time_));

    sim_assert(!started_);
    started_ = true;
    activate_target_core_ = target_core;

    if (cb_.discard_arriving) {
        // Trigger the "discard arriving blocks" code immediately
        callback_invoke(cb_.discard_arriving.get(), NULL);
    }
    if (cb_.cache_inject) {
        // Trigger the "inject new blocks" code immediately as well; we used
        // to wait 2 cycles (RegularMove callback triggered at "cyc"; halt
        // gets serviced at cyc+1; we'll attempt injection at cyc+2) but now
        // that activation is split from deactivation, all of this stuff
        // happens after the halt is done, just before fill instruction
        // inject.
        callback_invoke(cb_.cache_inject.get(), NULL);
    }
    if (cb_.oracle_prefetch) {
        // Trigger the "oracle prefetch" right away; this calculates transfer
        // times, bills the bus, and then kicks off a set of PrefetchPlayerCBs
        callback_invoke(cb_.oracle_prefetch.get(), NULL);
    }
    if (cb_.discard_departing) {
        // set the "discard blocks from departing app" callback to occur a
        // few cycles after the oracle-cache-dump callbacks happen, since
        // this will trash the cache tags they depend on.
        // (note that this is still a pretty dumb thing to do, since
        // it's going to prevent any cache-to-cache transfers to satisfy
        // later prefetch requests; we only wanted to prevent shared data
        // re-use from helping our single-threaded runs artificially)
        callbackq_enqueue_unowned(play_time_queue_, cyc + 10,
                                  cb_.discard_departing.get());
        cb_.discard_departing->scheduled_in(play_time_queue_);
    }
    if (cb_.sb_migrate) {
        // It looks a little silly to schedule this same-cycle instead of
        // invoking it directly; it re-schedules itself for a later time,
        // though, so it's simpler this way.
        callbackq_enqueue_unowned(play_time_queue_, cyc, cb_.sb_migrate.get());
        cb_.sb_migrate->scheduled_in(play_time_queue_);
    }
    if (cb_.summarize_prefetch) {
        // As with OraclePrefetchCB, this calculates transfer times, bills the
        // bus, and then kicks off a set of PrefetchPlayerCBs
        callback_invoke(cb_.summarize_prefetch.get(), NULL);
    }
    if (cb_.future_prefetch) {
        // This also kicks off a set of PrefetchPlayerCBs
        callback_invoke(cb_.future_prefetch.get(), NULL);
    }
    if (cb_.tlb_copy) {
        callbackq_enqueue_unowned(play_time_queue_, cyc, cb_.tlb_copy.get());
        cb_.tlb_copy->scheduled_in(play_time_queue_);
    }
}


// This destructor comes after the declarations for the various classes
// contained therein, so it knows how to invoke their destructors.
WSM_AppSummary::~WSM_AppSummary()
{
    typedef vector<SingleQueueCallback *> CallbackPtrVec;

    CallbackPtrVec cbs_to_cleanup;    // some pointers may be NULL
    cbs_to_cleanup.push_back(cb_.cache_inject.get());
    cbs_to_cleanup.push_back(cb_.oracle_prefetch.get());
    cbs_to_cleanup.push_back(cb_.discard_departing.get());
    cbs_to_cleanup.push_back(cb_.discard_arriving.get());
    cbs_to_cleanup.push_back(cb_.sb_migrate.get());
    cbs_to_cleanup.push_back(cb_.sb_stop.get());
    cbs_to_cleanup.push_back(cb_.summarize_prefetch.get());
    cbs_to_cleanup.push_back(cb_.future_prefetch.get());
    cbs_to_cleanup.push_back(cb_.tlb_copy.get());

    FOR_ITER(PrefetchPlayerVec, cb_.pf_players, iter) {
        cbs_to_cleanup.push_back(*iter);
    }

    FOR_ITER(CallbackPtrVec, cbs_to_cleanup, cb_iter) {
        SingleQueueCallback *cb = *cb_iter;
        if (cb && cb->is_sched()) {
            CallbackQueue *queue = cb->get_sched_queue();
            cb->sched_to_done();
            callbackq_cancel_ret(queue, cb);
            // these callbacks aren't "owned" by the various callback queues;
            // we're just unlinking them, and then letting scoped_ptr<>
            // delete them later.
        }
    }

    FOR_ITER(PrefetchPlayerVec, cb_.pf_players, iter) {
        delete *iter;
    }
    cb_.pf_players.clear();
}


void
WSM_Coord::pause_cosched_threads(int core_id, int nopause_app_id)
{
    const char *fname = "pause_cosched_threads";
    CoreResources * restrict core = Cores[core_id];
    const int n_contexts = core->n_contexts;

    for (int thread_idx = 0; thread_idx < n_contexts; thread_idx++) {
        context * restrict ctx = core->contexts[thread_idx];
        AppState * restrict as = ctx->as;
        if (as && (as->app_id != nopause_app_id)) {
            WSMDB(1)("%s: halting A%d on C%d T%d\n", fname, as->app_id,
                     ctx->core->core_id, ctx->id);
            WSM_Coord::CoschedHaltedCB *halt_done_cb =
                new CoschedHaltedCB(*this, ctx, as);
            context_halt_cb(ctx, CtxHaltStyle_Fast, halt_done_cb);
            reg_move.pend_cb.cosched_halted.insert(halt_done_cb);
        }
    }
}


void
WSM_Coord::resume_cosched_threads(int core_id, int nopause_app_id)
{
    const char *fname = "resume_cosched_threads";
    CoreResources * restrict core = Cores[core_id];
    const int n_contexts = core->n_contexts;

    for (int thread_idx = 0; thread_idx < n_contexts; thread_idx++) {
        context * ctx = core->contexts[thread_idx];
        AppState * as = map_at_default(paused_contexts_, ctx, NULL);
        if (as) {
            sim_assert(ctx->as == NULL);        // you're supposed to be idle
            sim_assert(as->app_id != nopause_app_id);
            WSMDB(1)("%s: resuming A%d on C%d T%d\n", fname, as->app_id,
                     ctx->core->core_id, ctx->id);
            paused_contexts_.erase(ctx);
            context_go(ctx, as, cyc);
        }
    }
}


WSM_Coord::WSM_Coord(const string& config_path)
    : cfg_path_(config_path), conf_(config_path)
{
    string base;   // scratch

    reg_move.as = NULL;
    reg_move.prev_targ_core = 0;

    reg_move.pend_cb.exit = NULL;
    reg_move.pend_cb.starter = NULL;
    reg_move.pend_cb.mover = NULL;
    reg_move.pend_cb.bg_stirrer = NULL;
    reg_move.pend_cb.bg_pausers.clear();        // owned by appmgr
    reg_move.pend_cb.cosched_halted.clear();
}


WSM_Coord::~WSM_Coord()
{
    // We can't necessarily just up and delete a WSM_Coord, since AppState
    // commit-watch queues may contain callbacks which refer to it; to be
    // safe, we should clean up apps first.
    sim_assert(per_app_info_.empty());

    // We can't delete our children ThreadCaptures safely, since we've handed
    // "ownership" to whoever created them (i.e. in CoreResources).  Since
    // they hold pointers into us (in particular, for our WSM_Config), we
    // can't depart first, either.
    sim_assert(capture_units_.empty());

    {
        FOR_ITER(set<BackgroundPauseCB *>, reg_move.pend_cb.bg_pausers,
                 iter) {
            (*iter)->parent_wsm_deleted();
        }
        reg_move.pend_cb.bg_pausers.clear();    // unlink, don't delete yet

        if (reg_move.pend_cb.starter) {
            // (deletes)
            callbackq_cancel(GlobalEventQueue, reg_move.pend_cb.starter);
            reg_move.pend_cb.starter = NULL;
        }
        FOR_ITER(set<CoschedHaltedCB *>, reg_move.pend_cb.cosched_halted,
                 iter) {
            // Strange case: context_halt() outstanding when destruction
            // occurs.  The core is going to invoke and destroy these callbacks
            // when halting is done, but there won't be any WSM stuff left
            // to operate on them.  We'll set these callbacks to fall back
            // to calling appmgr_signal_idlectx() when their parent object
            // has been deleted.
            (*iter)->parent_wsm_deleted();
        }
        reg_move.pend_cb.cosched_halted.clear(); // unlink, don't delete yet
    }

}


WSM_ThreadCapture *
WSM_Coord::new_threadcap(const string& name__)
{
    WSMDB(1)("WSM_Coord::new_threadcap: creating capture for %s\n", 
             name__.c_str());

    WSM_ThreadCapture *result =
        new WSM_ThreadCapture(this, &conf_, name__, cfg_path_);
    capture_units_.insert(result);
    return result;
}


void
WSM_Coord::register_app(struct AppState *app)
{
    const char *fname = "WSM_Coord::register_app";
    if (WSM_CoordPerApp *existing = map_at_default(per_app_info_, app, NULL)) {
        abort_printf("%s: A%d already registered, info ptr %p\n",
                     fname, app->app_id, (const void *) existing);
    }

    map_put_uniq(per_app_info_, app, new WSM_CoordPerApp(this, &conf_, app));

    if ((conf_.reg_move.app_id >= 0) &&
        (app->app_id == conf_.reg_move.app_id)) {
        WSMDB(1)("%s: matched app A%d, creating startup callback\n", fname,
                 app->app_id);
        sim_assert(!reg_move.as);
        reg_move.as = app;
        sim_assert(!reg_move.pend_cb.starter);
        reg_move.pend_cb.starter = new
            RegularMoveStartupCB(*this, new RegularMoveCB(*this)); 
        // We'll use a one-shot callback to kick off the RegularMove process.
        // (We can't necessarily schedule this app for processing yet, since
        // we may be calling this constructor for an AppState which has not
        // yet been registered with the AppMgr.  We don't handle this in the
        // general case now, but instead implictly rely on
        // appmgr_add_ready_app() being called before the next simulation
        // cycle.
        callbackq_enqueue(GlobalEventQueue, cyc, reg_move.pend_cb.starter);
        if (conf_.reg_move.exit_at_commit > 0) {
            reg_move.pend_cb.exit = new SimExitOkCB("regmove exit-commit");
            callbackq_enqueue(app->extra->watch.commit_count,
                              conf_.reg_move.exit_at_commit,
                              reg_move.pend_cb.exit);
        }
    }
}


void
WSM_Coord::destroying_app(struct AppState *app)
{
    const char *fname = "WSM_Coord::destroying_app";

    WSM_CoordPerApp *app_info = map_at_default(per_app_info_, app, NULL);
    if (!app_info) {
        abort_printf("%s: A%d not registered, can't clean up\n", fname,
                     app->app_id);
    }

    if ((conf_.reg_move.app_id >= 0) &&
        (app->app_id == conf_.reg_move.app_id)) {
        WSMDB(1)("%s: matched nomad A%d, cleaning up RegMove callbacks\n",
                 fname, app->app_id);
        sim_assert(app == reg_move.as);
        if (reg_move.pend_cb.mover) {
            CallbackQueue *period_queue = (conf_.reg_move.period_as_commits)
                ? app->extra->watch.commit_count : GlobalEventQueue;
            callbackq_cancel(period_queue, reg_move.pend_cb.mover);
            reg_move.pend_cb.mover = NULL;
        }
        if (reg_move.pend_cb.bg_stirrer) {
            CallbackQueue *period_queue = (conf_.reg_move.period_as_commits)
                ? app->extra->watch.commit_count : GlobalEventQueue;
            callbackq_cancel(period_queue, reg_move.pend_cb.bg_stirrer);
            reg_move.pend_cb.bg_stirrer = NULL;
        }
        if (reg_move.pend_cb.exit) {
            callbackq_cancel(app->extra->watch.commit_count,
                             reg_move.pend_cb.exit);
            reg_move.pend_cb.exit = NULL;
        }
    }

    per_app_info_.erase(app);
    delete app_info;
}


bool
WSM_Coord::want_sample(struct context *ctx, struct AppState *app) const
{
    WSM_CoordPerApp *app_info = map_at_default(per_app_info_, app, NULL);
    // collect samples for known apps, if either we're forced to, or if it's
    // an app we'll want to prefetch for
    bool result = app_info && 
        ((conf_.capture_all_apps) || app_info->prefetch_for_this_app());
    return result;
}


void
WSM_Coord::detach_capture(WSM_ThreadCapture *cap)
{
    sim_assert(capture_units_.count(cap));
    capture_units_.erase(cap);
}


void
WSM_Coord::signal_activate(struct AppState *app, struct context *ctx)
{
    const char *fname = "WSM_Coord::signal_activate";
    WSMDB(1)("%s: time %s, A%d being activated on C%d/T%d\n", fname, fmt_now(),
             app->app_id, ctx->core->core_id, ctx->id);
    WSM_CoordPerApp *app_info = map_at_default(per_app_info_, app, NULL);
    if (!app_info || !(app_info->prefetch_for_this_app()) ||
        !(app_info->have_summary())) {
        WSMDB(1)("%s: ignoring %s app A%d\n", fname, 
                 (!app_info) ? "unregistered" : 
                 ((!(app_info->prefetch_for_this_app())) ? "non-interesting" :
                  "non-summarized"), app->app_id);
        if (app_info)
            app_info->discard_any_summary();
        return;
    }
    WSM_AppSummary *summary = app_info->get_summary();
    if (summary->ready_to_start()) {
        summary->start_for_activate(ctx->core);
    }
}


void
WSM_Coord::signal_deactivate(struct AppState *app, struct context *ctx)
{
    const char *fname = "WSM_Coord::signal_deactivate";
    WSMDB(1)("%s: time %s, A%d being deactivated on C%d/T%d\n", fname,
             fmt_now(), app->app_id, ctx->core->core_id, ctx->id);

    // warning: ctx->as may be NULL, if the halt has already completed
    sim_assert((ctx->as == NULL) || (ctx->as == app));
    scoped_ptr<WSM_AppSummary> out_summary;

    WSM_CoordPerApp *app_info = map_at_default(per_app_info_, app, NULL);

    // first check: do we care about this app?  if not, return early.
    if (!app_info || !(app_info->prefetch_for_this_app())) {
        WSMDB(1)("%s: ignoring %s app A%d\n", fname, 
                 (!app_info) ? "unregistered" : "non-interesting", 
                 app->app_id);
        if (app_info)
            app_info->discard_any_summary();
        return;
    }

    out_summary.reset(new WSM_AppSummary(&conf_, GlobalEventQueue, app,
                                         ctx->core));
    
    if (conf_.reg_move.oracle_inject.enable) {
        CacheInjectCB *inject_cb =
            new CacheInjectCB(&conf_, app_info, out_summary.get());
        out_summary->add_cache_inject(inject_cb);
    }
    if (conf_.reg_move.discard_departing) {
        CacheDiscardCB *discard_cb =
            new CacheDiscardCB(&conf_, app_info, out_summary.get(), false);
        out_summary->add_discard_departing(discard_cb);
    }
    if (conf_.reg_move.discard_arriving) {
        CacheDiscardCB *discard_cb =
            new CacheDiscardCB(&conf_, app_info, out_summary.get(), true);
        out_summary->add_discard_arriving(discard_cb);
    }
    if (conf_.reg_move.oracle_cs_pf.enable) {
        OraclePrefetchCB *prefetch_cb =
            new OraclePrefetchCB(&conf_, app_info, out_summary.get());
        out_summary->add_oracle_prefetch(prefetch_cb);
    }
    if (conf_.reg_move.streambuf_mig.enable) {
        StreambufMigCB *sb_migrate_cb =
            new StreambufMigCB(&conf_, app_info, out_summary.get());
        StreambufStopCB *sb_stop_cb = 
            new StreambufStopCB(&conf_, app_info, out_summary.get());
        out_summary->add_sb_migrate(sb_migrate_cb);
        out_summary->add_sb_stop(sb_stop_cb);
        callbackq_enqueue_unowned(GlobalEventQueue, cyc + 2, sb_stop_cb);
        sb_stop_cb->scheduled_in(GlobalEventQueue);
    }
    if (conf_.reg_move.summ_pf.enable) { 
        SummarizePrefetchCB *prefetch_cb =
            new SummarizePrefetchCB(&conf_, app_info, out_summary.get());
        out_summary->add_summarize_prefetch(prefetch_cb);
    }
    if (conf_.reg_move.future_pf.enable) {
        FuturePrefetchCB *prefetch_cb =
            new FuturePrefetchCB(&conf_, app_info, out_summary.get());
        out_summary->add_future_prefetch(prefetch_cb);
    }
    if (conf_.reg_move.tlb_copy.copy_itlb ||
        conf_.reg_move.tlb_copy.copy_dtlb) {
        TLBCopyCB *tlb_copy_cb =
            new TLBCopyCB(&conf_, app_info, out_summary.get());
        out_summary->add_tlb_copy(tlb_copy_cb);
    }

    app_info->handoff_summary(scoped_ptr_release(out_summary));
}


// Rescheduled over and over; never self-terminates
class WSM_ThreadCapture::TableAgeCB : public CBQ_Callback {
    WSM_ThreadCapture& tc;
    i64 age_period;
public:
    TableAgeCB(WSM_ThreadCapture& tc_)
        : tc(tc_) {
        age_period = tc_.conf->table_age_period;
        sim_assert(age_period > 0);
    }
    i64 invoke(CBQ_Args *args) {
        tc.age_tables();
        i64 now = (tc.conf->table_age_as_serial) ? tc.access_serial : cyc;
        return now + age_period;
    }
};


WSM_ThreadCapture::
WSM_ThreadCapture(WSM_Coord *parent__, const WSM_Config *conf__,
                  const string& name__, const string& config_path__)
    : parent(parent__), conf(conf__), name(name__), cfg_path(config_path__),
      access_serial(0), serial_watcher(0), age_cb(0)
{
    if (!conf_bool(cfg_path + "/enable")) {
        exit_printf("shouldn't be creating ThreadCaptures when disabled, hmm");
    }

    block_bytes_lg = GlobalParams.mem.cache_block_bytes_lg;
    sim_assert(block_bytes_lg > 0);

    serial_watcher = callbackq_create();
    
    if (conf->table_age_period > 0) {
        age_cb = new TableAgeCB(*this);
        if (conf->table_age_as_serial) {
            callbackq_enqueue(serial_watcher,
                              access_serial + conf->table_age_period, age_cb);
        } else {
            callbackq_enqueue(GlobalEventQueue, cyc + conf->table_age_period,
                              age_cb);
        }
    }

    // sigh this is dumb; component tables reference these
    GlobalAccessSerialNum = access_serial;

    // Create component tables, populate tables vector in an arbitrary order
    {
        string sub(cfg_path + "/NextBlockInst");
        if (conf_bool(sub + "/enable")) {
            tab.nextblk_inst.
                reset(new NextBlockInstTab(conf, WSMT_NextBlockInst,
                                           sub, conf->default_cam_size,
                                           block_bytes_lg));
            tables.push_back(tab.nextblk_inst.get());
        }
    }
    {
        string sub(cfg_path + "/NextBlockData");
        if (conf_bool(sub + "/enable")) {
            tab.nextblk_data.
                reset(new NextBlockDataTab(conf, WSMT_NextBlockData,
                                           sub, conf->default_cam_size,
                                           block_bytes_lg));
            tables.push_back(tab.nextblk_data.get());
        }
    }
    {
        string sub(cfg_path + "/NextBlockBoth");
        if (conf_bool(sub + "/enable")) {
            tab.nextblk_both.
                reset(new NextBlockBothTab(conf, WSMT_NextBlockBoth,
                                           sub, conf->default_cam_size,
                                           block_bytes_lg));
            tables.push_back(tab.nextblk_both.get());
        }
    }
    {
        string sub(cfg_path + "/StridePC");
        if (conf_bool(sub + "/enable")) {
            tab.stridepc.reset(new StridePCTab(conf, WSMT_StridePC,
                                               sub, conf->default_cam_size,
                                               block_bytes_lg));
            tables.push_back(tab.stridepc.get());
        }
    }
    {
        string sub(cfg_path + "/SameObj");
        if (conf_bool(sub + "/enable")) {
            tab.sameobj.reset(new SameObjTab(conf, WSMT_SameObj,
                                             sub, conf->default_cam_size,
                                             block_bytes_lg));
            tables.push_back(tab.sameobj.get());
        }
    }
    {
        string sub(cfg_path + "/Pointer");
        if (conf_bool(sub + "/enable")) {
            tab.pointer.reset(new PointerTab(conf, WSMT_Pointer,
                                             sub, conf->default_ptrcam_size,
                                             block_bytes_lg));
            tables.push_back(tab.pointer.get());
        }
    }
    {
        string sub(cfg_path + "/PointerChase");
        if (conf_bool(sub + "/enable")) {
            tab.pointer_chase.reset
                (new PointerChaseTab(conf, WSMT_PointerChase, sub,
                                     conf->default_ptrcam_size,
                                     block_bytes_lg));
            tables.push_back(tab.pointer_chase.get());
        }
    }
    {
        string sub(cfg_path + "/BTB");
        if (conf_bool(sub + "/enable")) {
            tab.btb.reset(new BTBTab(conf, WSMT_BTB,
                                     sub, conf->default_ptrcam_size,
                                     block_bytes_lg));
            tables.push_back(tab.btb.get());
        }
    }
    {
        string sub(cfg_path + "/BlockBTB");
        if (conf_bool(sub + "/enable")) {
            tab.blkbtb.reset(new BlockBTBTab(conf, WSMT_BlockBTB,
                                             sub, conf->default_ptrcam_size,
                                             block_bytes_lg));
            tables.push_back(tab.blkbtb.get());
        }
    }
    {
        string sub(cfg_path + "/PCWindow");
        if (conf_bool(sub + "/enable")) {
            tab.pcwindow.reset(new PCWindowTab(conf, WSMT_PCWindow,
                                               sub, 1,
                                               block_bytes_lg));
            tables.push_back(tab.pcwindow.get());
        }
    }
    {
        string sub(cfg_path + "/SPWindow");
        if (conf_bool(sub + "/enable")) {
            tab.spwindow.reset(new SPWindowTab(conf, WSMT_SPWindow,
                                               sub, 1,
                                               block_bytes_lg));
            tables.push_back(tab.spwindow.get());
        }
    }
    {
        string sub(cfg_path + "/RetStack");
        if (conf_bool(sub + "/enable")) {
            tab.retstack.reset(new RetStackTab(conf, WSMT_RetStack,
                                               sub, 1,
                                               block_bytes_lg));
            tables.push_back(tab.retstack.get());
        }
    }
    {
        string sub(cfg_path + "/InstMRU");
        if (conf_bool(sub + "/enable")) {
            tab.instmru.reset(new InstMRUTab(conf, WSMT_InstMRU,
                                             sub, 1,
                                             block_bytes_lg));
            tables.push_back(tab.instmru.get());
        }
    }
    {
        string sub(cfg_path + "/DataMRU");
        if (conf_bool(sub + "/enable")) {
            tab.datamru.reset(new DataMRUTab(conf, WSMT_DataMRU,
                                             sub, 1,
                                             block_bytes_lg));
            tables.push_back(tab.datamru.get());
        }
    }

    // Sort tables by priority, checking for uniqueness
    {
        map<double,TableBase *> prio2tab;
        FOR_CONST_ITER(TableVec, tables, iter) {
            double prio = (*iter)->g_priority();
            if (!prio2tab.insert(make_pair(prio, *iter)).second) {
                exit_printf("priority conflict: tables '%s' and '%s' share "
                            "priority %.15g\n",
                            prio2tab[prio]->g_name().c_str(),
                            (*iter)->g_name().c_str(), prio);
            }
        }
        tables.clear();
        for (map<double,TableBase *>::iterator iter = prio2tab.begin(); 
             iter != prio2tab.end(); ++iter) {
            tables.push_back(iter->second);
        }
    }

    // resets all of our component tables, too
    reset();

    // sigh this is dumb
    GlobalAccessSerialNum = -1;
}


WSM_ThreadCapture::~WSM_ThreadCapture()
{
    parent->detach_capture(this);
    if (age_cb) {
        // note: callbackq_cancel deletes age_cb
        callbackq_cancel((conf->table_age_as_serial) ?
                         serial_watcher : GlobalEventQueue, age_cb);
    }
    if (serial_watcher) {
        callbackq_destroy(serial_watcher);
    }
    // scoped_ptr will handle much of the deletion work
}


void
WSM_ThreadCapture::reset()
{
    WSMDB(1)("WSM_ThreadCapture: reset %s\n", name.c_str());
    sample_count = 0;
    expand_calls = 0;
    FOR_ITER(TableVec, tables, iter) {
        (*iter)->reset();
    }
}


void
WSM_ThreadCapture::flush_tables(int app_id_or_neg1)
{
    WSMDB(1)("WSM_ThreadCapture: flush_tables %s, A%d\n", name.c_str(),
             app_id_or_neg1);
    FOR_ITER(TableVec, tables, iter) {
        if (app_id_or_neg1 >= 0) {
            (*iter)->flush_one_app(app_id_or_neg1);
        } else {
            (*iter)->flush_entries();
        }
    }
}


void
WSM_ThreadCapture::sample(AppState *as, const MemProfSample *samp)
{
    WSMDB(3)("WSM_ThreadCapture: sample %s, %s\n", name.c_str(),
             fmt_memsamp_static(samp));
    GlobalAccessSerialNum = access_serial;
    TableBase *winner_ptr = 0;
    string matches;
    ++sample_count;
    for (TableVec::iterator iter = tables.begin(); iter != tables.end();
         ++iter) {
        bool is_match = false;
        if ((*iter)->sample_accept(samp)) {
            is_match = (*iter)->sample(as, samp);
            if (is_match && !winner_ptr) {
                // take first winner (assume tables[] in priority order)
                winner_ptr = *iter;
            }
        }
        matches += (is_match) ? "1" : "0";
    }
    for (TableVec::iterator iter = tables.begin(); iter != tables.end();
         ++iter) {
        if ((*iter)->sample_accept(samp)) {
            bool is_winner = (*iter) == winner_ptr;
            (*iter)->sample2(as, samp, is_winner);
        }
    }
    for (TableVec::iterator iter = tables.begin(); iter != tables.end();
         ++iter) {
        if ((*iter)->update_accept(samp)) {
            (*iter)->update(as, samp);
        }
    }
    WSMDB(3)("WSM: sample matches: %s, winner: %s\n", matches.c_str(),
             (winner_ptr) ? winner_ptr->g_name().c_str() : "(none)");
    access_serial++;
    if (callbackq_ready(serial_watcher, access_serial)) {
        callbackq_service(serial_watcher, access_serial, NULL);
    }
    GlobalAccessSerialNum = -1;
}


void
WSM_ThreadCapture::summarize_and_expand(int for_app_id,
                                        PrefetchCollector *pf_coll,
                                        long *xfer_bits_ret)
{
    WSMDB(1)("WSM_ThreadCapture: summarize_and_expand %s for A%d\n",
             name.c_str(), for_app_id);
    GlobalAccessSerialNum = access_serial;
    long total_xfer_bits = 0;
    int shared_queue_id_counter = 0;
    FOR_ITER(TableVec, tables, iter) {
        TableBase *table = *iter;
        long table_xfer_bits = -1;
        table->summarize_and_expand(for_app_id, pf_coll,
                                    &shared_queue_id_counter,
                                    &table_xfer_bits);
        sim_assert(table_xfer_bits >= 0);
        total_xfer_bits += table_xfer_bits;
    }
    GlobalAccessSerialNum = -1;
    ++expand_calls;
    *xfer_bits_ret = total_xfer_bits;
}


void
WSM_ThreadCapture::age_tables()
{
    WSMDB(1)("WSM_ThreadCapture: %s aging tables, cyc %s\n",
             name.c_str(), fmt_now());
    GlobalAccessSerialNum = access_serial;
    FOR_ITER(TableVec, tables, iter) {
        (*iter)->age_table();
    }
    GlobalAccessSerialNum = -1;
}


void
WSM_ThreadCapture::print_stats(FILE *out, const char *pf) const
{
    fprintf(out, "%sThreadCapture %s stats:\n", pf, name.c_str());
    i64 all_table_blocks = 0, all_table_bits = 0;
    FOR_CONST_ITER(TableVec, tables, iter) {
        const TableBase& table = **iter;
        const PerTableStats& st = table.g_stats();
        all_table_blocks += st.total_blocks;
        all_table_bits += st.total_bits;
    }
    fprintf(out, "%s  samples: %s\n", pf, fmt_i64(sample_count));
    fprintf(out, "%s  expand calls: %s blocks reqd: %s bits used: %s\n", pf,
            fmt_i64(expand_calls), fmt_i64(all_table_blocks),
            fmt_i64(all_table_bits));
    FOR_CONST_ITER(TableVec, tables, iter) {
        const TableBase& table = **iter;
        const PerTableStats& st = table.g_stats();
        fprintf(out, "%s  table %s prio %.15g:\n", pf, table.g_name().c_str(),
                table.g_priority());
        fprintf(out, "%s    samples accepted: %s matched: %s wins: %s\n",
                pf, fmt_i64(st.samples_accepted), fmt_i64(st.samples_matched),
                fmt_i64(st.samples_match_wins));
        fprintf(out, "%s    updates accepted: %s touched: %s"
                " replace: %s noreplace: %s\n", pf,
                fmt_i64(st.updates_accepted), fmt_i64(st.updates_touched),
                fmt_i64(st.updates_replace), fmt_i64(st.updates_noreplace));
        fprintf(out, "%s    blocks reqd: %s bits used: %s\n", pf,
                fmt_i64(st.total_blocks), fmt_i64(st.total_bits));
        fprintf(out, "%s    per-expand entries: %s\n", pf, 
                st.per_expand.entries.fmt().c_str());
        fprintf(out, "%s    per-expand blocks: %s bits: %s\n", pf, 
                st.per_expand.blocks.fmt().c_str(),
                st.per_expand.bits.fmt().c_str());
        fprintf(out, "%s    per-entry blocks: %s bits: %s\n", pf, 
                st.per_expand_entry.blocks.fmt().c_str(),
                st.per_expand_entry.bits.fmt().c_str());
    }
}


//
// C interface
//


WSM_Coord *
wsm_coord_create(const char *config_path)
{
    return new WSM_Coord(string(config_path));
}

void
wsm_coord_destroy(WSM_Coord *coord)
{
    delete coord;
}

WSM_ThreadCapture *
wsm_coord_create_threadcap(WSM_Coord *coord, const char *name)
{
    return coord->new_threadcap(string(name));
}

void
wsm_coord_register_app(WSM_Coord *coord, struct AppState *app)
{
    coord->register_app(app);
}

void
wsm_coord_destroying_app(WSM_Coord *coord, struct AppState *app)
{
    coord->destroying_app(app);
}

int
wsm_coord_wantsample(const WSM_Coord *coord,
                     struct context *ctx, struct AppState *app)
{
    return coord->want_sample(ctx, app);
}

const char *
wsm_table_name(const WSM_Coord *coord, int table_id)
{
    return (ENUM_OK(WSMTableID, table_id)) ?
        ENUM_STR(WSMTableID, table_id) : NULL;
}

void
wsm_coord_signal_activate(WSM_Coord *coord, struct AppState *app,
                          struct context *ctx)
{
    coord->signal_activate(app, ctx);
}

void
wsm_coord_signal_deactivate(WSM_Coord *coord, struct AppState *app,
                            struct context *ctx)
{
    coord->signal_deactivate(app, ctx);
}

void
wsm_threadcap_reset(WSM_ThreadCapture *tc)
{
    tc->reset();
}

void
wsm_threadcap_destroy(WSM_ThreadCapture *tc)
{
    delete tc;
}

void
wsm_threadcap_sample(WSM_ThreadCapture *tc, AppState *app,
                     const struct MemProfSample *samp)
{
    tc->sample(app, samp);
}

void
wsm_threadcap_printstats(const WSM_ThreadCapture *tc,
                         void *c_FILE_out, const char *prefix)
{
    tc->print_stats(static_cast<FILE *>(c_FILE_out), prefix);
}



//
// Misc. utility code
//

namespace {

// cache_select_mask is bitflags composed of CACHE_INJECT_*
void
discard_core_cached_app(CoreResources * restrict core,
                        AppState * restrict as,
                        unsigned cache_select_mask,
                        bool downgrade_only, bool tlbs_too)
{
    const char *fname = "discard_core_cached_app";
    typedef map<LongAddr,unsigned> DiscardMaskMap;
    const int master_id = as->app_master_id;
    WSMDB(2)("RegularMove: %s time %s, masterid %d C%d mask 0x%x "
             "downgrade %d tlbs_too %d; ", fname, fmt_now(), master_id,
             core->core_id, cache_select_mask, downgrade_only, tlbs_too);
    CoreCacheInventory cache_inv(core, master_id);
    DiscardMaskMap discard_masks;
    if (cache_select_mask & CACHE_INJECT_L1I) {
        const AddrSet& i_blocks(cache_inv.get_icache_blocks());
        FOR_CONST_ITER(AddrSet, i_blocks, iter) {
            discard_masks[*iter] |= CACHE_INJECT_L1I;
        }
    }
    if (cache_select_mask & CACHE_INJECT_L1D) {
        const AddrSet& d_blocks(cache_inv.get_dcache_blocks());
        FOR_CONST_ITER(AddrSet, d_blocks, iter) {
            discard_masks[*iter] |= CACHE_INJECT_L1D;
        }
    }
    if (GlobalParams.mem.private_l2caches &&
        (cache_select_mask & CACHE_INJECT_L2)) {
        const AddrSet& l2_blocks(cache_inv.get_l2cache_blocks());
        FOR_CONST_ITER(AddrSet, l2_blocks, iter) {
            discard_masks[*iter] |= CACHE_INJECT_L2;
        }
    }

    WSMDB(2)("%d unique block addresses.\n", intsize(discard_masks));
    FOR_CONST_ITER(DiscardMaskMap, discard_masks, iter) {
        sim_assert(iter->second != 0);
        cachesim_oracle_discard_block(core, iter->first, downgrade_only,
                                      iter->second);
    }

    if (tlbs_too) {
        tlb_flush_app(core->itlb, master_id);
        tlb_flush_app(core->dtlb, master_id);
    }
}


// return the capacity of the given cache, in blocks
int
cache_size_blocks(const CacheArray *cache)
{
    int n_blocks;
    cache_get_geom(cache, NULL, &n_blocks);    
    sim_assert(n_blocks > 0);
    return n_blocks;
}


}       // Anonymous namespace close
