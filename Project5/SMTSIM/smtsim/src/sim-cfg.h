/*
 * Simulator configuration management
 *
 * Jeff Brown
 * $Id: sim-cfg.h,v 1.3.10.6.8.9 2009/07/24 02:04:39 jbrown Exp $
 */

#ifndef SIM_CFG_H
#define SIM_CFG_H

#ifdef __cplusplus
extern "C" {
#endif

struct SimParams;
struct bbtracker_params_t;
struct CoreParams;
struct ThreadParams;
struct AppMgrParams;
struct CacheGeometry;
struct WorkQueue;
struct KVTree;


void simcfg_init(void);
void simcfg_load_cfg(const char *filename);
void simcfg_eval_cfg(const char *config_string);
void simcfg_save_cfg(const char *filename);

// These are handy for quickly hacking in extra parameters for testing, etc.
// (copy the string from simcfg_get_str() if you want it to persist!)
int simcfg_have_val(const char *name);
int simcfg_get_int(const char *name);
int simcfg_get_bool(const char *name);
i64 simcfg_get_i64(const char *name);
u64 simcfg_get_x64(const char *name);
double simcfg_get_double(const char *name);
const char *simcfg_get_str(const char *name);
int simcfg_get_enum(const char **str_table, const char *name);


void simcfg_dump_tree(const char *name);

void simcfg_bbv_params (struct bbtracker_params_t * dest);  
void simcfg_sim_params(struct SimParams *dest);
void simcfg_init_thread_core_map(struct SimParams *dest);

// This modifies the tree, filling in the "Core_n" override trees.  Returns
// a new CoreParams object, that must be destroyed with a call to
// coreparams_destroy().
struct CoreParams *simcfg_core_params(int core_id);

// This modifies the tree, filling in the "Thread_n" override trees
void simcfg_thread_params(struct ThreadParams *dest, int thread_id);

void simcfg_appmgr_params(struct AppMgrParams *dest);


// Read an argfile and create a new workload from it.  An argfile should not
// be imported more than once.  This does not actually add a job to the
// work-queue, it just creates a new "known workload".
void simcfg_import_argfile(const char *argfile_name);

// Add a job to the config tree of the work-queue (not the actual C++
// WorkQueue object), to run the workload corresponding to the given argfile.
// If the argfile has not already been imported, this imports it.
// (This is for backward compatibility with command-line argfiles.)
void simcfg_gen_argfile_job(const char *argfile_name);

// Edit the config tree at workload_path, recursively merging in all values
// indicated by the "inherit" subtree.  Any "inherit" keys are removed from
// workload_path after expansion.  (This does not modify the parent workload
// trees, so it's less efficient than it could be, though it does allow them
// to be further modified.)
void simcfg_expand_workload(const char *workload_path);

// Iterate through the "WorkQueue/Jobs" subtree, adding each job to "dest".
void simcfg_add_jobs(struct WorkQueue *dest);


#ifdef __cplusplus
}
#endif


#ifdef __cplusplus
    // C++ only interface; mainly to pass "string" type for convenience
    #include <string>
    #include <set>

    namespace SimCfg {
        bool have_conf(const std::string& name);
        int conf_int(const std::string& name);
        bool conf_bool(const std::string& name);
        i64 conf_i64(const std::string& name);
        u64 conf_x64(const std::string& name);
        double conf_double(const std::string& name);
        const std::string& conf_str(const std::string& name);
        int conf_enum(const char **str_table, const std::string& name);
        void conf_read_keys(const std::string& tree_name,
                            std::set<std::string> *dest);
    }
#endif  // __cplusplus


#endif  /* SIM_CFG_H */
