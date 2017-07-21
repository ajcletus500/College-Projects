//
// DebugCoverageTracker: conveniently track & report code coverage.
//
// Jeff Brown
// $Id: debug-coverage.cc,v 1.1.2.6 2009/12/02 19:12:04 jbrown Exp $
//

const char RCSid_1259030130[] = 
"$Id: debug-coverage.cc,v 1.1.2.6 2009/12/02 19:12:04 jbrown Exp $";

#include <stdlib.h>
#include <stdio.h>

#include "sys-types.h"
#include "debug-coverage.h"
#include "sim-assert.h"
#include "utils.h"


// Forcing the use of values of a dense enum type to represent coverage points
// would make this much faster, but would require programmers maintain both an
// enum type (with associated compiler-namespace constraints, enum-type
// visibility concerns across modules, etc.) as well as a string list, and to
// keep the two synchronized.  We want this to take less effort.
//
// Note that we hash/compare raw char pointers in order to save string
// traversals.  This relies on the compiler and linker collapsing duplicate
// string constants into shared storage; e.g. gcc-4.3 / GNU ld 2.15.92.0.2 do
// NOT do this across compilation units, but they do do this within a unit.


namespace {

// If this env variable is set to a non-empty string, any auto-generated
// coverage reporting (e.g. from the destructor) is suppressed.
const char DisableAutoReportEnvName[] = "SMTSIM_DISABLE_AUTO_COVERAGE";

int GlobalDisableAutoReports = -1;     // -1: unread, 0 or 1 otherwise


}       // Anonymous namespace close


DebugCoverageTracker::DebugCoverageTracker(const char *tracker_name,
                                           const char **all_point_names,
                                           bool auto_report_at_destroy)
    : tracker_name_(tracker_name),
      all_point_names_(all_point_names),
      auto_report_at_destroy_(auto_report_at_destroy)      
{
    for (int point_walk = 0; all_point_names_[point_walk] != NULL;
         ++point_walk) {
        const char *point_name = all_point_names_[point_walk];
        map_put_uniq(reached_counts_, point_name, 0);
    }
}


DebugCoverageTracker::~DebugCoverageTracker()
{
    if (GlobalDisableAutoReports < 0) {
        const char *disable_value = getenv(DisableAutoReportEnvName);
        GlobalDisableAutoReports = (disable_value != NULL) &&
            (disable_value[0] != '\0');
        sim_assert(GlobalDisableAutoReports >= 0);
    }
    if (auto_report_at_destroy_ && !GlobalDisableAutoReports) {
        this->report(stdout, "DebugCoverage: ");
    }
}


void
DebugCoverageTracker::reached(const char *point_name)
{
    int *val_ptr = map_find(reached_counts_, point_name);
    if (SP_T(val_ptr != NULL)) {
        ++(*val_ptr);
        if (*val_ptr < 0) {
            --(*val_ptr);       // overflow-by-1: put it back at max value
        }
    } else {
        abort_printf("DebugCoverageTracker::reached: "
                     "unmatched point_name \"%s\" in tracker \"%s\"\n",
                     point_name, tracker_name_.c_str());
    }
}


void
DebugCoverageTracker::report(void *c_FILE_out, const char *prefix) const
{
    FILE *out = static_cast<FILE *>(c_FILE_out);
    fprintf(out, "%sbegin coverage report for %s\n", prefix,
            tracker_name_.c_str());
    int num_reached = 0;
    FOR_CONST_ITER(ReachedCountMap, reached_counts_, reached_iter) {
        if (reached_iter->second > 0)
            ++num_reached;
    }
    fprintf(out, "%s%d of %d coverage points reached, %d unreached:\n",
            prefix, num_reached, intsize(reached_counts_),
            intsize(reached_counts_) - num_reached);
    for (int point_walk = 0; all_point_names_[point_walk] != NULL;
         ++point_walk) {
        const char *point_name = all_point_names_[point_walk];
        coverage_count_t point_val = map_at(reached_counts_, point_name);
        fprintf(out, "%s  %s: %s\n", prefix, point_name, fmt_i64(point_val));
    }
    fprintf(out, "%send coverage report for %s\n", prefix,
            tracker_name_.c_str());
}



//
// C interface
//


DebugCoverageTracker *
debug_coverage_create(const char *tracker_name,
                      const char **all_point_names,
                      int auto_report_at_destroy)
{
    return new DebugCoverageTracker(tracker_name, all_point_names,
                                    auto_report_at_destroy);
}

void
debug_coverage_destroy(DebugCoverageTracker *tracker)
{
    delete tracker;
}

void
debug_coverage_reached(DebugCoverageTracker *tracker, const char *point_name)
{
    tracker->reached(point_name);
}

void
debug_coverage_report(const DebugCoverageTracker *tracker, 
                      void *c_FILE_out, const char *prefix)
{
    tracker->report(c_FILE_out, prefix);
}
