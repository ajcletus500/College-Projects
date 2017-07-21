/*
 * Simulator configuration management
 *
 * Jeff Brown
 * $Id: sim-cfg.cc,v 1.11.6.31.2.2.2.17 2009/12/21 05:44:39 jbrown Exp $
 */

const char RCSid_1042751798[] =
"$Id: sim-cfg.cc,v 1.11.6.31.2.2.2.17 2009/12/21 05:44:39 jbrown Exp $";

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <set>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>

#include "kv-tree-basic.h"
#include "kv-tree-path.h"
#include "kv-tree.h"

#include "sim-assert.h"
#include "sys-types.h"
#include "sim-cfg.h"
#include "core-resources.h"
#include "sim-params.h"
#include "cache-params.h"
#include "context.h"
#include "utils.h"
#include "assoc-array.h"
#include "arg-file.h"
#include "work-queue.h"
#include "bbtracker.h"

using std::cerr;
using std::ifstream;
using std::ofstream;
using std::ostringstream;
using std::set;
using std::string;

using KVTreeBasic::Val_Int;
using KVTreeBasic::Val_Bool;
using KVTreeBasic::Val_Double;
using KVTreeBasic::Val_String;
using KVTreeBasic::Val_Bin;

//using namespace SimCfg;


namespace {

struct GlobalConf {
    KVTreePath *tree;
} Config;


void 
t_push(const string& path)
{
    try {
        Config.tree->ts_push(path);
    } catch (KVTreePath::BadPath& ex) {
        fflush(0);
        cerr << "Bad config path in push; " << ex.reason << ": \""
             << ex.path << "\"\n";
        exit(1);
    } 
}

void 
t_walk(const string& path)
{
    try {
        Config.tree->walk_const(path);
    } catch (KVTreePath::BadPath& ex) {
        fflush(0);
        cerr << "Bad config path in walk; " << ex.reason << ": \""
             << ex.path << "\"\n";
        exit(1);
    } 
}

void
t_pop()
{
    Config.tree->ts_pop();
}

const KVTreeVal * 
t_get(const string& path)
{
    const KVTreeVal *val;
    try {
        val = Config.tree->get(path);
    } catch (KVTreePath::BadPath& ex) {
        fflush(0);
        cerr << "Bad config path in get; " << ex.reason << ": \"" 
             << ex.path << "\"\n";
        exit(1);
    } 
    return val;
}

const KVTreeVal * 
t_get_ifexist(const string& path)
{
    const KVTreeVal *val = Config.tree->get_ifexist(path);
    return val;
}

KVTree * 
t_get_tree(const string& path)
{
    const KVTreeVal *val = t_get(path);
    const KVTree *val_t = dynamic_cast<const KVTree *>(val);
    if (!val_t) {
        fflush(0);
        cerr << "Config type error: value of " << Config.tree->full_path(path)
             << " is not a tree\n";
        exit(1);
    } 
    return const_cast<KVTree *>(val_t);
}

int 
t_get_int(const string& path)
{
    const KVTreeVal *val = t_get(path);
    const Val_Int *val_t = dynamic_cast<const Val_Int *>(val);
    if (!val_t) {
        fflush(0);
        cerr << "Config type error: value of " << Config.tree->full_path(path)
             << " is not an integer\n";
        exit(1);
    } 
    return val_t->val();
}

int 
t_get_posint(const string& path)
{
    int val = t_get_int(path);
    if (val <= 0) {
        fflush(0);
        cerr << "Config value error: value of " << Config.tree->full_path(path)
             << " (" << val << ") is not a positive integer\n";
        exit(1);
    } 
    return val;
}

int 
t_get_nnint(const string& path)
{
    int val = t_get_int(path);
    if (val < 0) {
        fflush(0);
        cerr << "Config value error: value of " << Config.tree->full_path(path)
             << " (" << val << ") is not a non-negative integer\n";
        exit(1);
    } 
    return val;
}

bool 
t_get_bool(const string& path)
{
    const KVTreeVal *val = t_get(path);
    const Val_Bool *val_t = dynamic_cast<const Val_Bool *>(val);
    if (!val_t) {
        fflush(0);
        cerr << "Config type error: value of " << Config.tree->full_path(path)
             << " is not a boolean\n";
        exit(1);
    } 
    return val_t->val();
}

double 
t_get_double(const string& path)
{
    const KVTreeVal *val = t_get(path);
    const Val_Double *val_t = dynamic_cast<const Val_Double *>(val);
    if (!val_t) {
        fflush(0);
        cerr << "Config type error: value of " << Config.tree->full_path(path)
             << " is not a double\n";
        exit(1);
    } 
    return val_t->val();
}

i64
t_get_i64(const string& path) 
{
    const KVTreeVal *val = t_get(path);
    {
        const Val_Double *val_t = dynamic_cast<const Val_Double *>(val);
        if (val_t) {
            double d = val_t->val();
            return double_to_i64(d);
        }
    }
    if (false) {
        // For now, disallow implicit Val_Int -> i64 conversion.  The problem
        // is that Val_Int uses the host system's native "int" type, which may
        // vary across systems and is typically narrower than an i64.  For
        // consistency, we'll stick with using doubles to store i64s.
        // This may be re-visited later.
        const Val_Int *val_t = dynamic_cast<const Val_Int *>(val);
        if (val_t) {
            int i = val_t->val();
            return i;
        }
    }
    fflush(0);
    cerr << "Config type error: value of " << Config.tree->full_path(path)
         << " is not a double\n";
    exit(1);
}

i64
t_get_nni64(const string& path)
{
    i64 val = t_get_i64(path);
    if (val < 0) {
        fflush(0);
        cerr << "Config value error: value of " << Config.tree->full_path(path)
             << " (" << val << ") is not non-negative\n";
        exit(1);
    } 
    return val;
}

u64
t_get_x64(const string& path) 
{
    const KVTreeVal *val = t_get(path);
    {
        if (const Val_Bin *val_bin = dynamic_cast<const Val_Bin *>(val)) {
            int n_bytes = int(val_bin->size());
            if (n_bytes > int(sizeof(u64))) {
                fflush(0);
                cerr << "Config type error: value of "
                     << Config.tree->full_path(path)
                     << " has too many bytes to fit a u64\n";
                exit(1);
            }
            const unsigned char *bytes = val_bin->val();
            u64 result = 0;
            for (int i = 0; i < n_bytes; ++i) {
                result = (result << 8) | bytes[i];
            }
            return result;
        }
    }
    fflush(0);
    cerr << "Config type error: value of " << Config.tree->full_path(path)
         << " is not a (hex) byte sequence\n";
    exit(1);
}

const string&
t_get_str(const string& path)
{
    const KVTreeVal *val = t_get(path);
    const Val_String *val_t = dynamic_cast<const Val_String *>(val);
    if (!val_t) {
        fflush(0);
        cerr << "Config type error: value of " << Config.tree->full_path(path)
             << " is not a string\n";
        exit(1);
    } 
    return val_t->val();
}


int
t_get_enum(const char **str_table, const string& path)
{
    const string& str_val = t_get_str(path);
    int enum_val = enum_lookup(str_table, str_val.c_str());
    if (enum_val < 0) {
        fflush(0);
        cerr << "Config type error: value of " << Config.tree->full_path(path)
             << ", \"" << str_val
             << "\", is not an enum value recognized at its use\n"
             << "Known values:";
        for (int i = 0; str_table[i]; i++)
            cerr << " " << str_table[i];
        cerr << "\n";
        exit(1);
    }
    return enum_val;
}


int
lg_param(int val, const string& name)
{
    int result = 0, log_inexact = 1;

    if (val > 0)
        result = floor_log2(val, &log_inexact);
    if (log_inexact) {
        fflush(0);
        cerr << "Config parameter error: \"" << name << "\" (" << val 
             << ") must be >0 and a power of 2.\n";
        exit(1);
    }
    return result;
}


void
read_cache_geom(CacheGeometry *dest, int cache_block_bytes)
{
    dest->size_kb = t_get_posint("size_kb");
    dest->assoc = t_get_posint("assoc");
    dest->block_bytes = cache_block_bytes;
    dest->n_banks = t_get_posint("n_banks");
    dest->wb_buffer_size = t_get_posint("wb_buffer_size");
    dest->ports.r = t_get_nnint("ports/r");
    dest->ports.w = t_get_nnint("ports/w");
    dest->ports.rw = t_get_nnint("ports/rw");
    if ((dest->ports.r + dest->ports.rw) <= 0) {
        cerr << "Config error: cache geometry has no read ports, in " <<
            Config.tree->full_path("") << "\n";
        exit(1);
    }
    if ((dest->ports.w + dest->ports.rw) <= 0) {
        cerr << "Config error: cache geometry has no write ports, in " <<
            Config.tree->full_path("") << "\n";
        exit(1);
    }

    sim_assert(dest->config_path == NULL);
    dest->config_path = e_strdup(Config.tree->full_path("").c_str());
}

void
read_op_time(OpTime& dest, const string& name)
{
    t_push(name);
    dest.latency = t_get_nnint("latency");
    dest.interval = t_get_nnint("interval");
    t_pop();
}

void
read_cache_timing(CacheTiming *dest)
{
    read_op_time(dest->access_time, "access_time");
    read_op_time(dest->access_time_wb, "access_time_wb");
    read_op_time(dest->fill_time, "fill_time");
    dest->miss_penalty = t_get_nnint("miss_penalty");
}

void
read_memunit_params(MemUnitParams *dest, int block_bytes)
{
    dest->block_bytes = block_bytes;
    dest->n_banks = t_get_posint("n_banks");
    read_op_time(dest->read_time, "read_time");
    read_op_time(dest->write_time, "write_time");
}

void
read_tcache_params(TraceCacheParams *dest)
{
    dest->n_entries = t_get_posint("n_entries");
    dest->assoc = t_get_posint("assoc");
    dest->block_insts = t_get_posint("block_insts");
    dest->pred_per_block = t_get_posint("pred_per_block");
    dest->is_path_assoc = t_get_bool("is_path_assoc");
    dest->trim_partial_hits = t_get_bool("trim_partial_hits");
}

void
read_tfill_params(TraceFillUnitParams *dest)
{
    dest->output_fifo_len = t_get_posint("output_fifo_len");
    dest->output_interval = t_get_nnint("output_interval");
    dest->align_to_bblock = t_get_bool("align_to_bblock");
    dest->branch_promote_thresh = t_get_nnint("branch_promote_thresh");
    dest->allow_indirect_jumps = t_get_bool("allow_indirect_jumps");
}

void
read_mbp_params(MultiBPredictParams *dest)
{
    dest->n_rows = t_get_posint("n_rows");
    dest->predict_width = t_get_posint("predict_width");
}

void
read_exec_timing(CoreExecTiming *dest) 
{
    for (int i = 0; i < (int) NELEM(dest->delay); i++)
        dest->delay[i] = -1;

    dest->delay[SDC_all_extra] = t_get_nnint("all_extra");
    dest->delay[SDC_int_arith] = t_get_nnint("int_arith");
    dest->delay[SDC_int_load] = t_get_nnint("int_load");
    dest->delay[SDC_int_store] = t_get_nnint("int_store");
    dest->delay[SDC_int_compare] = t_get_nnint("int_compare");
    dest->delay[SDC_int_condmove] = t_get_nnint("int_condmove");
    dest->delay[SDC_int_condbr] = t_get_nnint("int_condbr");
    dest->delay[SDC_int_mull] = t_get_nnint("int_mull");
    dest->delay[SDC_int_mulq] = t_get_nnint("int_mulq");
    dest->delay[SDC_int_umulh] = t_get_nnint("int_umulh");
    dest->delay[SDC_fp_arith] = t_get_nnint("fp_arith");
    dest->delay[SDC_fp_bit] = t_get_nnint("fp_bit");
    dest->delay[SDC_fp_load] = t_get_nnint("fp_load");
    dest->delay[SDC_fp_store] = t_get_nnint("fp_store");
    dest->delay[SDC_fp_compare] = t_get_nnint("fp_compare");
    dest->delay[SDC_fp_condmove] = t_get_nnint("fp_condmove");
    dest->delay[SDC_fp_condbr] = t_get_nnint("fp_condbr");
    dest->delay[SDC_fp_divs] = t_get_nnint("fp_divs");
    dest->delay[SDC_fp_divt] = t_get_nnint("fp_divt");
    dest->delay[SDC_uncond_br] = t_get_nnint("uncond_br");
    dest->delay[SDC_ftoi] = t_get_nnint("ftoi");
    dest->delay[SDC_itof] = t_get_nnint("itof");
    dest->delay[SDC_smt_lockrel] = t_get_nnint("smt_lockrel");
    dest->delay[SDC_smt_fork] = t_get_nnint("smt_fork");
    dest->delay[SDC_smt_terminate] = t_get_nnint("smt_terminate");

    for (int i = 0; i < (int) NELEM(dest->delay); i++) {
        if (dest->delay[i] < 0) {
            cerr <<
                "Error: CoreExecTiming delay[] array isn't completely filled "
                "in.  This is likely an error in the read_exec_timing() "
                "function (" << __FILE__ << ":" << __LINE__ << ")\n";
            sim_abort();
        }
    }
}

CoreParams *
read_core_params()
{
    CoreParams *dest = static_cast<CoreParams *>(emalloc_zero(sizeof(*dest)));

    dest->config_path = e_strdup(Config.tree->full_path("").c_str());

    dest->itlb_entries = t_get_posint("itlb_entries");
    dest->dtlb_entries = t_get_posint("dtlb_entries");
    dest->tlb_miss_penalty = t_get_nnint("tlb_miss_penalty");
    dest->tlb_filter_invalid = t_get_bool("tlb_filter_invalid");
    dest->btb_entries = t_get_posint("btb_entries");
    dest->btb_assoc = t_get_posint("btb_assoc");
    dest->pht_entries = t_get_posint("pht_entries");
    dest->br_bias_entries = t_get_posint("br_bias_entries");
    dest->loadstore_queue_size = t_get_posint("loadstore_queue_size");

    t_push("TraceCache");
    read_tcache_params(&dest->tcache);
    t_pop();

    t_push("TraceFillUnit");
    read_tfill_params(&dest->tfill);
    t_pop();

    t_push("MultiBranchPredictor");
    read_mbp_params(&dest->multi_bp);
    t_pop();

    t_push("ICache");
    dest->icache.geom = cachegeom_create();
    read_cache_geom(dest->icache.geom, GlobalParams.mem.cache_block_bytes);
    read_cache_timing(&dest->icache.timing);
    dest->icache.prefetch_nextblock = t_get_bool("prefetch_nextblock");
    t_pop();

    t_push("DCache");
    dest->dcache.geom = cachegeom_create();
    read_cache_geom(dest->dcache.geom, GlobalParams.mem.cache_block_bytes);
    read_cache_timing(&dest->dcache.timing);
    dest->dcache.prefetch_nextblock = t_get_bool("prefetch_nextblock");
    t_pop();

    t_push("L2Cache");
    dest->private_l2cache.geom = cachegeom_create();
    read_cache_geom(dest->private_l2cache.geom,
                    GlobalParams.mem.cache_block_bytes);
    read_cache_timing(&dest->private_l2cache.timing);
    dest->private_l2cache.prefetch_nextblock =
        t_get_bool("prefetch_nextblock");
    t_pop();

    t_push("Fetch");
    dest->fetch.single_limit = t_get_posint("single_limit");
    dest->fetch.total_limit = t_get_posint("total_limit");
    dest->fetch.thread_count_limit = t_get_posint("thread_count_limit");
    dest->fetch.n_stages = t_get_posint("n_stages");
    dest->fetch.enable_trace_cache = t_get_bool("enable_trace_cache");
    dest->fetch.tcache_skips_to_rename = t_get_bool("tcache_skips_to_rename");
    t_pop();

    t_push("Decode");
    dest->decode.n_stages = t_get_posint("n_stages");
    t_pop();

    t_push("Rename");
    dest->rename.int_rename_regs = t_get_posint("int_rename_regs");
    dest->rename.float_rename_regs = t_get_posint("float_rename_regs");
    dest->rename.n_stages = t_get_posint("n_stages");
    t_pop();

    t_push("Queue");
    dest->queue.int_queue_size = t_get_posint("int_queue_size");
    dest->queue.float_queue_size = t_get_posint("float_queue_size");
    dest->queue.int_ooo_issue = t_get_bool("int_ooo_issue");
    dest->queue.float_ooo_issue = t_get_bool("float_ooo_issue");
    dest->queue.max_int_issue = t_get_posint("max_int_issue");
    dest->queue.max_float_issue = t_get_posint("max_float_issue");
    dest->queue.max_ldst_issue = t_get_posint("max_ldst_issue");
    dest->queue.max_sync_issue = t_get_posint("max_sync_issue");
    t_pop();

    t_push("RegRead");
    dest->regread.n_stages = t_get_posint("n_stages");
    t_pop();

    t_push("RegWrite");
    dest->regwrite.n_stages = t_get_posint("n_stages");
    t_pop();

    t_push("Commit");
    dest->commit.single_limit = t_get_posint("single_limit");
    dest->commit.total_limit = t_get_posint("total_limit");
    dest->commit.thread_count_limit = t_get_posint("thread_count_limit");
    t_pop();

    t_push("ExecTime");
    read_exec_timing(&dest->exec_timing);
    t_pop();
    
    dest->request_bus = NULL;
    dest->reply_bus = NULL;
    dest->shared_l2cache = NULL;
    dest->shared_l3cache = NULL;

    return dest;
}

void
read_thread_params(ThreadParams *dest)
{
    dest->active_list_size = t_get_posint("active_list_size");
    dest->active_list_size_lg = lg_param(dest->active_list_size,
                                         "active_list_size");
    dest->reorder_buffer_size = t_get_posint("reorder_buffer_size");
    dest->retstack_entries = t_get_posint("retstack_entries");
    dest->discard_static_noops = t_get_bool("discard_static_noops");
}

void
add_config(const KVTreePath *new_tree)
{
    try {
        Config.tree->overlay("", new_tree->root()->copy(), false);
    } catch (KVTreePath::BadPath& ex) {
        cerr << "Bad path in config file overlay; " << ex.reason << ": \""
             << ex.path << "\"\n";
        exit(1);
    }
}

void
init_thread_core_map(SimParams *dest)
{
    int num_cores = dest->num_cores;
    int num_contexts = dest->num_contexts;
    int *map = static_cast<int *>(emalloc_zero(num_contexts * sizeof(map[0])));
    dest->thread_core_map.map = map;

    for (int thread = 0; thread < num_contexts; thread++) {
        int core_id;
        ostringstream ostr;
        ostr << "t" << thread;
        if (t_get_ifexist(ostr.str())) {
            core_id = t_get_nnint(ostr.str());
        } else {
            core_id = tcp_policy_core(dest->thread_core_map.policy, thread);
        }
        if ((core_id < 0) || (core_id >= num_cores)) {
            cerr << "Core number " << core_id << " out of range [0..." 
                 << num_cores << "), for thread " << thread << "\n";
            exit(1);
        }
        map[thread] = core_id;
    }
}

// Mangle a file name to get something usable as a config tree name.
// (We probably just make the KV-tree able to use quoted strings as keys,
// but then we have to start worrying about quoting and escaping when writing
// pathnames, bah.)
string
filename_to_keyname(const string& src)
{
    ostringstream result;
    for (string::const_iterator iter = src.begin(); iter != src.end();
         ++iter) {
        if (!isalnum(*iter) || ((iter == src.begin()) && isdigit(*iter))) {
            result << "_";
        } else {
            result << *iter;
        }
    }
    return result.str();
}


} // Anonymous namespace close


void
simcfg_init(void)
{
    if (!Config.tree)
        Config.tree = KVTreeBasic::new_tree();
}


void
simcfg_load_cfg(const char *filename)
{
    printf("Loading config file \"%s\"...\n", filename);
    fflush(0);

    if (strcmp(filename, "-") == 0) {
        cerr << "Sorry, reading config info from stdin isn't currently "
             << "supported.\n";
        exit(1);
    }

    ifstream cfg_in(filename);
    if (!cfg_in) {
        cerr << "Couldn't open config file \"" << filename << "\": " <<
            strerror(errno) << "\n";
        exit(1);
    }

    try {
        KVTreePath *new_tree = KVTreeBasic::read_tree(filename, cfg_in);
        add_config(new_tree);
        delete new_tree;
    } catch (KVTreeBasic::BadParse& ex) {
        cerr << "Error loading config file: " << ex.reason << "\n";
        exit(1);
    }
    fflush(0);
}


void simcfg_eval_cfg(const char *config_string)
{
    std::istringstream cfg_in(config_string);
    try {
        KVTreePath *new_tree = KVTreeBasic::read_tree("(string)", cfg_in);
        add_config(new_tree);
        delete new_tree;
    } catch (KVTreeBasic::BadParse& ex) {
        fflush(0);
        cerr << "Error parsing config string: " << ex.reason << "\n";
        exit(1);
    }
}


void 
simcfg_save_cfg(const char *filename)
{
    printf("Writing config file \"%s\"...\n", filename);
    fflush(0);

    if (strcmp(filename, "-") == 0) {
        std::cout << "--------\n";
        if (!KVTreeBasic::write_tree(std::cout, Config.tree)) {
            cerr << "Error writing config file to stdout\n";
            exit(1);
        }
        std::cout << "--------\n";
        std::cout.flush();
    } else {
        ofstream cfg_out(filename);
        if (!cfg_out) {
            cerr << "Couldn't create config file \"" << filename << "\": " <<
                strerror(errno) << "\n";
            exit(1);
        }
        if (!KVTreeBasic::write_tree(cfg_out, Config.tree)) {
            cerr << "Error writing config file to \"" << filename << "\"\n";
            exit(1);
        }
    }
}


int 
simcfg_have_val(const char *name)
{
    const KVTreeVal *val = t_get_ifexist(name);
    return (val != 0);
}

bool 
SimCfg::have_conf(const std::string& name) {
    const KVTreeVal *val = t_get_ifexist(name);
    return (val != 0);
}

int 
simcfg_get_int(const char *name)
{
    return t_get_int(name);
}

int
SimCfg::conf_int(const std::string& name)
{
    return t_get_int(name);
}

int 
simcfg_get_bool(const char *name)
{
    return t_get_bool(name);
}

bool
SimCfg::conf_bool(const std::string& name)
{
    return t_get_bool(name);
}

i64 
simcfg_get_i64(const char *name)
{
    return t_get_i64(name);
}

u64 
simcfg_get_x64(const char *name)
{
    return t_get_x64(name);
}

i64
SimCfg::conf_i64(const std::string& name)
{
    return t_get_i64(name);
}

u64
SimCfg::conf_x64(const std::string& name)
{
    return t_get_x64(name);
}

double 
simcfg_get_double(const char *name)
{
    return t_get_double(name);
}

double
SimCfg::conf_double(const std::string& name)
{
    return t_get_double(name);
}

const char *
simcfg_get_str(const char *name)
{
    return t_get_str(name).c_str();
}

const std::string&
SimCfg::conf_str(const std::string& name)
{
    return t_get_str(name);
}

int
simcfg_get_enum(const char **str_table, const char *name)
{
    return t_get_enum(str_table, name);
}

int
SimCfg::conf_enum(const char **str_table, const std::string& name)
{
    return t_get_enum(str_table, name);
}

void
SimCfg::conf_read_keys(const std::string& tree_name,
                       std::set<std::string> *dest)
{
    dest->clear();
    KVTree *tree = t_get_tree(tree_name);

    // This single-iterator interface is lame
    tree->iter_reset();
    {
        string key;
        KVTreeVal *val;
        while (tree->iter_next(key, val)) {
            dest->insert(key);
        }
    }
}


void 
simcfg_dump_tree(const char *name)
{
    KVTree *tree = const_cast<KVTree*>(t_get_tree(name));
    fflush(0);
    if (!KVTreeBasic::write_tree(std::cout, tree)) {
        cerr << "Error writing tree at \"" << name << "\" to stdout\n";
        exit(1);
    }
    std::cout.flush();
}


void
simcfg_bbv_params (bbtracker_params_t * dest)
{
    t_push ("BasicBlockTracker");

    dest->create_bbv_file = t_get_bool ("create_bbv_file");
    dest->filename = t_get_str ("filename").c_str ();
    dest->interval = (long)t_get_nni64 ("interval");

    t_pop ();
}


void 
simcfg_sim_params(struct SimParams *dest)
{
    memset(dest, 0, sizeof(*dest));
    t_push("Global");

    dest->thread_length = t_get_nni64("thread_length");
    if (t_get_ifexist("allinstructions")) {
        dest->allinstructions = t_get_nni64("allinstructions");
    } else {
        dest->allinstructions = -1;
    }
    dest->num_contexts = t_get_posint("num_contexts");
    dest->num_cores = t_get_posint("num_cores");
    dest->nice_level = t_get_int("nice_level");
    dest->disable_coredump = t_get_bool("disable_coredump");
    dest->reap_alist_at_squash = t_get_bool("reap_alist_at_squash");
    dest->abort_on_alist_full = t_get_bool("abort_on_alist_full");

    dest->long_mem_cyc = t_get_nnint("/Hacking/long_mem_cyc");
    dest->long_mem_at_commit = t_get_bool("/Hacking/long_mem_at_commit");
    dest->print_appmgr_stats = t_get_bool("/Hacking/print_appmgr_stats");

    t_push("ThreadCoreMap");
    {
        const string& str = t_get_str("policy");
        int policy = tcp_parse_policy(str.c_str());
        if (policy < 0) {
            cerr << "Unrecognized thread->core policy \"" << str << "\"\n";
            exit(1);
        }
        dest->thread_core_map.policy = static_cast<ThreadCorePolicy>(policy);
    }
    t_pop();    // ThreadCoreMap


    t_push("Mem");
    dest->mem.cache_request_holders = t_get_posint("cache_request_holders");
    dest->mem.cache_block_bytes = t_get_posint("cache_block_bytes");
    dest->mem.cache_block_bytes_lg = lg_param(dest->mem.cache_block_bytes,
                                              "cache_block_bytes");
    dest->mem.page_bytes = t_get_posint("page_bytes");
    dest->mem.page_bytes_lg = lg_param(dest->mem.page_bytes, "page_bytes");
    dest->mem.inst_bytes = t_get_posint("inst_bytes");
    dest->mem.split_bus = t_get_bool("split_bus");
    read_op_time(dest->mem.bus_request_time, "bus_request_time");
    read_op_time(dest->mem.bus_transfer_time, "bus_transfer_time");
    dest->mem.stack_initial_kb = t_get_posint("stack_initial_kb");
    dest->mem.stack_max_kb = t_get_posint("stack_max_kb");
    dest->mem.use_coherence = t_get_bool("use_coherence");
    dest->mem.use_l3cache = t_get_bool("use_l3cache");
    dest->mem.private_l2caches = t_get_bool("private_l2caches");
    if (t_get_ifexist("l1l2bus_transfer_time")) {
        fflush(0);
        cerr << "Warning: old key "
             << Config.tree->full_path("l1l2bus_transfer_time")
             << " present; ignoring\n";
    }

    t_push("L2Cache");
    dest->mem.l2cache_geom = cachegeom_create();
    read_cache_geom(dest->mem.l2cache_geom, dest->mem.cache_block_bytes);
    read_cache_timing(&dest->mem.l2cache_timing);
    t_pop();    // L2Cache

    t_push("L3Cache");
    dest->mem.l3cache_geom = cachegeom_create();
    read_cache_geom(dest->mem.l3cache_geom, dest->mem.cache_block_bytes);
    read_cache_timing(&dest->mem.l3cache_timing);
    t_pop();    // L3Cache

    t_push("MainMem");
    read_memunit_params(&dest->mem.main_mem, dest->mem.cache_block_bytes);
    t_pop();    // MainMem

    t_pop();    // Mem
    t_pop();    // Global
}


void
simcfg_init_thread_core_map(struct SimParams *dest)
{
    t_push("Global");
    t_push("ThreadCoreMap");
    init_thread_core_map(dest);
    t_pop();
    t_pop();
}

CoreParams * 
simcfg_core_params(int core_id)
{
    CoreParams *dest = NULL;
    sim_assert(core_id >= 0);
    string default_name = "Core";
    string specific_name;
    {
        ostringstream ostr;
        ostr << "Core_" << core_id;
        specific_name = ostr.str();
    }
    
    if (!t_get_ifexist(specific_name)) {
        // Ensure that a subtree exists for this specific core, so that
        // we can freely modify it without worry
        Config.tree->set(specific_name, new KVTree(), false);
    }

    {
        // Use default values to fill in any which aren't set in the
        // more specific trees
        const KVTree *default_core = t_get_tree(default_name);
        Config.tree->overlay(specific_name, default_core->copy(), true);
        const KVTree *default_l2cache = t_get_tree("Global/Mem/L2Cache");
        Config.tree->overlay(specific_name + "/L2Cache",
                             default_l2cache->copy(), true);
    }

    t_push(specific_name);
    dest = read_core_params();
    t_pop();

    // After all parameters are read, propogate some values implicitly
    dest->inst_bytes = GlobalParams.mem.inst_bytes;
    dest->page_bytes = GlobalParams.mem.page_bytes;
    dest->icache.cache_id = core_id;
    dest->dcache.cache_id = core_id;
    dest->private_l2cache.cache_id = core_id;
    dest->tcache.inst_bytes = dest->inst_bytes;
    dest->multi_bp.inst_bytes = dest->inst_bytes;
    return dest;
}


void 
simcfg_thread_params(struct ThreadParams *dest, int thread_id)
{
    sim_assert(thread_id >= 0);
    string default_name = "Thread";
    string override_name;
    {
        ostringstream ostr;
        ostr << "Thread_" << thread_id;
        override_name = ostr.str();
    }
    
    if (t_get_ifexist(override_name)) {
        // Use the default values to fill in any which aren't set in the
        // override 
        const KVTree *default_thread = t_get_tree(default_name);
        Config.tree->overlay(override_name, default_thread->copy(), true);
        t_push(override_name);
    } else {
        t_push(default_name);
    }

    read_thread_params(dest);
    t_pop();
}


void
simcfg_appmgr_params(struct AppMgrParams *dest)
{
    // AppMgrParams is currently a placeholder with no real data,
    // so it doesn't read anything from the config yet.   
}

void
simcfg_import_argfile(const char *argfile_name)
{
    const char *fname = "simcfg_import_argfile";
    ArgFileInfo *arg_info = NULL;       // warning: may leak on exception :(

    arg_info = argfile_load(argfile_name);
    if (!arg_info) {
        exit_printf("couldn't load argfile \"%s\", exiting\n", argfile_name);
    }

    t_push("Workloads");
    string workload_id(filename_to_keyname(argfile_name));

    if (Config.tree->get_ifexist(workload_id)) {
        exit_printf("%s: workload ID '%s' already exists, importing %s\n",
                    fname, workload_id.c_str(), argfile_name);
    }

    try {
        Config.tree->walk_create(workload_id);
    } catch (KVTreePath::BadPath& ex) {
        fflush(0);
        cerr << "Bad workload path in argfile import; " << ex.reason << ": \""
             << ex.path << "\"\n";
        exit(1);
    }

    DEBUGPRINTF("%s: importing argfile \"%s\" as workload \"%s\"\n",
                fname, argfile_name, workload_id.c_str());
    if (debug)
        argfile_dump(stdout, arg_info, "  ");

    try {
        Config.tree->set("ff_dist", 
                         new Val_Double(i64_to_double(arg_info->ff_dist)),
                         false);
        if (arg_info->num_threads != 1) {
            Config.tree->set("n_threads", new Val_Int(arg_info->num_threads),
                             false);
        }
        Config.tree->ts_push_top();
        Config.tree->walk_create("argv");
        int arg_count;
        for (arg_count = 0; arg_info->argv[arg_count]; ++arg_count) {
            string key = string("_") + fmt_i64(arg_count);
            Config.tree->set(key, new Val_String(arg_info->argv[arg_count]),
                             false);
        }
        Config.tree->set("size", new Val_Int(arg_count), false);
        Config.tree->ts_pop();
        if (arg_info->redir_name.in) {
            Config.tree->set("stdin", new Val_String(arg_info->redir_name.in),
                             false);
        }
        if (arg_info->redir_name.out) {
            Config.tree->set("stdout", new
                             Val_String(arg_info->redir_name.out), false);
        }
    } catch (KVTreePath::BadPath& ex) {
        fflush(0);
        cerr << "Bad path in argfile import; " << ex.reason << ": \""
             << ex.path << "\"\n";
        exit(1);
    }

    argfile_destroy(arg_info);
    t_pop();
}


void
simcfg_gen_argfile_job(const char *argfile_name)
{
    const char *fname = "simcfg_gen_argfile_job";
    string workload_id(filename_to_keyname(argfile_name));

    if (!Config.tree->get_ifexist(string("Workloads") + "/" + workload_id)) {
        simcfg_import_argfile(argfile_name);
    }

    t_push("WorkQueue/Jobs");
    string job_id;
    {
        // Create a unique job_id.  (inefficient, but easy to read output)
        int instance_num = 1;
        do {
            job_id = workload_id + "_" + fmt_i64(instance_num);
            instance_num++;
        } while (Config.tree->get_ifexist(job_id));
    }

    DEBUGPRINTF("%s: adding job ID \"%s\" to run workload \"%s\" for "
                "argfile \"%s\"\n",
                fname, job_id.c_str(), workload_id.c_str(), argfile_name);

    try {
        Config.tree->walk_create(job_id);
        Config.tree->set("workload", new Val_String(workload_id), false);
        // Start at time 0
        Config.tree->set("start_time", new Val_Double(0), false);
    } catch (KVTreePath::BadPath& ex) {
        fflush(0);
        cerr << "Bad workload path in argfile import; " << ex.reason << ": \""
             << ex.path << "\"\n";
        exit(1);
    }

    t_pop();
}


namespace {

// Read the workload subtree at the top of the Config.tree directory stack,
// and expand it (depth-first traversal, merge children first) into targ_tree.
void
recursive_workload_expand(set<const KVTree *>& seen,
                          KVTree *targ_tree)
{
    const char *fname = "recursive_workload_inherit";
    const KVTree *this_tree = Config.tree->ts_top();
    if (seen.count(this_tree)) {
        exit_printf("%s: cycle detected, expanding workload trees at "
                    "%s\n", fname, Config.tree->full_path(".").c_str());
    }
    seen.insert(this_tree);

    // Note: the "inherit" nodes are children from the perpective of the
    // (implicit) tree traversal graph, even though they're parents from an
    // inheritance perspective.  Yay, confusion.

    // Visit "inherit" children first
    bool have_old = simcfg_have_val("inherit_0");
    bool have_new = simcfg_have_val("inherit");
    if (have_old && have_new) {
        exit_printf("%s: both old and new style workload-inherit present in "
                    "config at \"%s\"\n", fname,
                    Config.tree->full_path(".").c_str());
    } else if (have_new) {
        int list_size = simcfg_get_int("inherit/size");
        for (int inherit_idx = 0; inherit_idx < list_size; inherit_idx++) {
            string inherit_key = string("inherit/_") + fmt_i64(inherit_idx);
            string inherit_str = t_get_str(inherit_key);
            t_push("..");
            t_walk(inherit_str);
            recursive_workload_expand(seen, targ_tree);
            t_pop();
        }
    } else {
        for (int inherit_idx = 0; true; inherit_idx++) {
            string inherit_key = string("inherit_") + fmt_i64(inherit_idx);
            if (!t_get_ifexist(inherit_key))
                break;
            string inherit_str = t_get_str(inherit_key);
            t_push("..");
            t_walk(inherit_str);
            recursive_workload_expand(seen, targ_tree);
            t_pop();
        }
    }

    // Expand this_tree into targ_tree
    // (The pointers are equal when at the root of expansion.)
    if (this_tree != targ_tree) {
        // (leaves lots of extra "inherit_N" keys in the final targ_tree)
        targ_tree->overlay(this_tree->copy(), false);
    }
}

} // Anonymous namespace close


void
simcfg_expand_workload(const char *workload_path)
{
    const char *fname = "simcfg_expand_workload";
    t_push(workload_path);
    KVTree *workload_tree = Config.tree->ts_top();      // "pwd"

    // We'll first accumulate values from all inheritance sources into a 
    // new (non-linked) subtree, without changing the main config tree
    KVTree *inherited_tree = new KVTree();
    set<const KVTree *> seen;

    DEBUGPRINTF("%s: expanding workload path \"%s\"\n", fname, workload_path);
    recursive_workload_expand(seen, inherited_tree);

    // Clean up "inherit_N" keys and "inherit" subtrees, a jumble which may
    // have been partially overwritten in various places
    for (int inherit_idx = 0; true; inherit_idx++) {
        string inherit_key = string("inherit_") + fmt_i64(inherit_idx);
        if (!inherited_tree->del_child(inherit_key)) {
            break;
        }
    }
    inherited_tree->del_child("inherit");

    // Now, overlay inherited values on the workload's subtree.
    // This destroys inherited_tree.
    workload_tree->overlay(inherited_tree, false);
    inherited_tree = NULL;

    // Remove expanded "inherit_N" keys.  (This is optional, we could just
    // as easily leave them in and allow them to be re-expanded later.)
    for (int inherit_idx = 0; true; inherit_idx++) {
        string inherit_key = string("inherit_") + fmt_i64(inherit_idx);
        if (!workload_tree->del_child(inherit_key)) {
            break;
        }
    }
    workload_tree->del_child("inherit");

    t_pop();

    if (debug) {
        DEBUGPRINTF("%s: expansion result:\n--------\n", fname);
        simcfg_dump_tree(workload_path);
        DEBUGPRINTF("--------\n");
    }
}


void
simcfg_add_jobs(struct WorkQueue *dest)
{
    string jobs_base("WorkQueue/Jobs");
    KVTree *jobs_tree = t_get_tree(jobs_base);
    set<string> job_ids;

    // This iterator interface is lame
    jobs_tree->iter_reset();
    {
        string key;
        KVTreeVal *val;
        while (jobs_tree->iter_next(key, val)) {
            job_ids.insert(key);
        }
    }

    for (set<string>::const_iterator iter = job_ids.begin();
         iter != job_ids.end(); ++iter) {
        workq_add_job_simcfg(dest, (jobs_base + "/" + *iter).c_str());
    }
}
