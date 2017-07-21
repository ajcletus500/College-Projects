// -*- C++ -*-
//
// DebugCoverageTracker: conveniently track & report code coverage.
//
// Jeff Brown
// $Id: debug-coverage.h,v 1.1.2.4 2009/12/02 19:12:04 jbrown Exp $
//

#ifndef DEBUG_COVERAGE_H
#define DEBUG_COVERAGE_H

typedef struct DebugCoverageTracker DebugCoverageTracker;

#ifdef __cplusplus

#include <string>
#include <map>

#include "utils-cc.h"

struct DebugCoverageTracker {
private:
    std::string tracker_name_;
    const char **all_point_names_;
    bool auto_report_at_destroy_;
    // Note: map by _pointer_ value itself
    typedef int coverage_count_t;
    typedef std::map<const char *, coverage_count_t> ReachedCountMap;
    ReachedCountMap reached_counts_;

    NoDefaultCopy nocopy;

public:
    // all_point_names is an array of C strings with all possible "point_name"
    // values; it must be terminated by a NULL pointer.
    DebugCoverageTracker(const char *tracker_name,
                         const char **all_point_names,
                         bool auto_report_at_destroy);
    ~DebugCoverageTracker();

    // WARNING: "reached" only accepts the specific pointer values which were
    // presented in "all_point_names" at creation-time; simple string-equality
    // is not sufficient.  (This makes the common case of using constants
    // within a single source file much faster; if you need support for using
    // this across compilation units, you'll have to do your own string
    // uniquification.)
    void reached(const char *point_name);
    void report(void *c_FILE_out, const char *prefix) const;
};


#endif // __cplusplus


#ifdef __cplusplus
extern "C" {
#endif


DebugCoverageTracker *debug_coverage_create(const char *tracker_name,
                                            const char **all_point_names,
                                            int auto_report_at_destroy);
void debug_coverage_destroy(DebugCoverageTracker *tracker);

void debug_coverage_reached(DebugCoverageTracker *tracker,
                            const char *point_name);
void debug_coverage_report(const DebugCoverageTracker *tracker, 
                           void *c_FILE_out, const char *prefix);


#ifdef __cplusplus
}
#endif


#endif  // DEBUG_COVERAGE_H
