// -*- C++ -*-
//
// "On-line" statistics computation -- computes stats on data as it becomes
// available
//
// Jeff Brown
// $Id: online-stats.h,v 1.2.2.8.2.1.2.9 2009/08/03 22:31:22 jbrown Exp $
//

#ifndef ONLINE_STATS_H
#define ONLINE_STATS_H

#include <math.h>
#include <map>
#include <set>
#include <string>

#include "sys-types.h"
#include "sim-assert.h"
#include "hash-map.h"


// "Basic" stats: these stats are collected without much space and time, so you
// can throw lots of samples at them.

class BasicStat_I64 {
public:
    typedef i64 ValType;

private:
    i64 count_;
    ValType max_, min_;
    // The type to use for sum/sum_sqr is a problem; using integers risks 
    // overflow, but using floating-point risks data loss during addition.
    ValType sum_, sum_sqr_;
    bool sum_overflow_, sum_sqr_overflow_;

public:
    BasicStat_I64() { reset(); }
    ~BasicStat_I64() { }
    void reset();
    std::string fmt() const;

    void add_sample(ValType val) {
        ValType val_sqr = val * val;
        sum_ += val;
        sum_sqr_ += val_sqr;
        if (SP_F(sum_ < val)) sum_overflow_ = true;
        if (SP_F(val_sqr < val) ||
            SP_F(sum_sqr_ < val_sqr)) sum_sqr_overflow_ = true;
        if (val > max_)
            max_ = val;
        if (val < min_)
            min_ = val;
        count_++;
    }

    i64 g_count() const { return count_; }
    ValType g_min() const { sim_assert(count_); return min_; }
    ValType g_max() const { sim_assert(count_); return max_; }
    ValType g_sum() const {
        return (SP_F(sum_overflow_)) ? I64_MAX : sum_;
    }

    // Return SimNAN on overflow or insufficient sample count
    double g_mean() const;
    double g_variance(bool sample_not_population) const;

    bool g_overflow() const { return sum_overflow_ || sum_sqr_overflow_; }
};

class BasicStat_Double {
public:
    typedef double ValType;

private:
    i64 count_;
    ValType max_, min_;
    ValType sum_, sum_sqr_;

public:
    BasicStat_Double() { reset(); }
    ~BasicStat_Double() { }
    void reset();
    std::string fmt() const;

    void add_sample(ValType val) {
        sim_assert(!sim_isnan(val));
        ValType val_sqr = val * val;
        sum_ += val;
        sum_sqr_ += val_sqr;
        if (val > max_)
            max_ = val;
        if (val < min_)
            min_ = val;
        count_++;
    }

    i64 g_count() const { return count_; }
    ValType g_min() const { return (count_) ? min_ : SimNAN; }
    ValType g_max() const { return (count_) ? max_ : SimNAN; }
    ValType g_sum() const { return sum_; }

    // Return SimNAN on overflow or insufficient sample count
    double g_mean() const;
    double g_variance(bool sample_not_population) const;
};


// Histogram (counting buckets) with int keys
// This does no binning, so don't let your unique key count get out of control.
class HistCount_Int {
public:
    typedef int KeyType;
    typedef i64 CountType;
    typedef std::set<KeyType> KeySet;           // ordered set
private:
    #if HAVE_HASHMAP
        typedef hash_map<KeyType, CountType> BucketMap;
    #else
        typedef std::map<KeyType, CountType> BucketMap;
    #endif
    BucketMap counts_;
public:
    HistCount_Int() { reset(); }
    ~HistCount_Int() { }
    void reset() { counts_.clear(); }

    void add_count(KeyType key, CountType to_add = 1) {
        sim_assert(to_add >= 0);
        counts_[key] += to_add;
        sim_assert(counts_[key] >= to_add);
    }
    CountType get_count(KeyType key) const {
        BucketMap::const_iterator found = counts_.find(key);
        return (found != counts_.end()) ? found->second : 0;
    }
    CountType total_count() const;
    void get_all_keys(KeySet& dest) const;
};



// Binary (two-class) confusion matrix.
//
// This records and characterizes the performance of a classifier (predictor)
// which classifies each input as belonging to one of two classes.
//
// (This isn't anything profound, it's just basic predictive statistics which
// we seem to keep re-inventing.  Also, this uses "math people" vocabulary in
// preference to architecture-speak.)

class BiConfusionMatrix {
public:
    typedef i64 CountType;

private:
    // We use an array of counters instead of individual variables, to allow
    // us to play with direct index calculation of a case statement or
    // if-cascade.  (It may seem petty, but the direct-calculation code
    // produces _drastically_ tighter code than the original case-statement.)
    enum CountIdx {     // WARNING: specific values matter! see idx()
        CntTN = 0,      // true negative
        CntFN = 1,      // false negative
        CntFP = 2,      // false positive
        CntTP = 3       // true positive
    };

    CountType c_[4];    // Sample counts, indexed by values from CountIdx

    // Generate an index into c_[] for one sample
    static int idx(bool classified_positive, bool actually_positive) {
        return int(classified_positive << 1) + int(actually_positive);
    }
    
public:
    BiConfusionMatrix() { reset(); }
    BiConfusionMatrix(CountType tp, CountType tn, CountType fp, CountType fn) {
        c_[CntTN] = tn;
        c_[CntFN] = fn;
        c_[CntFP] = fp;
        c_[CntTP] = tp;
    }
    ~BiConfusionMatrix() { }
    void reset() {
        c_[0] = 0; c_[1] = 0; c_[2] = 0; c_[3] = 0;
    }
    // compact string, no newline
    std::string fmt_brief() const;
    // verbose multi-line string, each line starting with "prefix"
    std::string fmt_verbose(const std::string& prefix) const;

    void add_sample(bool classified_positive, bool actually_positive) {
        ++c_[idx(classified_positive, actually_positive)];
    }

    CountType true_pos() const { return c_[CntTP]; }
    CountType true_neg() const { return c_[CntTN]; }
    CountType false_pos() const { return c_[CntFP]; }
    CountType false_neg() const { return c_[CntFN]; }

    CountType actual_pos() const { return c_[CntTP] + c_[CntFN]; }
    CountType actual_neg() const { return c_[CntTN] + c_[CntFP]; }
    CountType predict_pos() const { return c_[CntTP] + c_[CntFP]; }
    CountType predict_neg() const { return c_[CntTN] + c_[CntFN]; }
    CountType match() const { return c_[CntTP] + c_[CntTN]; }
    CountType mismatch() const { return c_[CntFP] + c_[CntFN]; }
    CountType all() const {
        return c_[CntTP] + c_[CntTN] + c_[CntFP] + c_[CntFN];
    }

    // false positive rate AKA alpha
    double false_pos_rate() const { return double(c_[CntFP]) / actual_neg(); }
    // false negative rate AKA beta
    double false_neg_rate() const { return double(c_[CntFN]) / actual_pos(); }
    // sensitivity AKA "true positive rate", "recall", "power", 1-FNR
    double sensitivity() const { return double(c_[CntTP]) / actual_pos(); }
    // specificity AKA "true negative rate", 1-FPR
    double specificity() const { return double(c_[CntTN]) / actual_neg(); }
    // positive predictive value AKA "precision"
    double pos_pred_value() const { return double(c_[CntTP]) / predict_pos(); }
    double neg_pred_value() const { return double(c_[CntTN]) / predict_neg(); }

    // Matthews Correlation Coefficient, on [-1,1]
    // 1.0: perfect prediction, 0.0: random, -1.0: inverted
    double matthews_cc() const;
    double accuracy() const { return double(match()) / all(); }

    // false discovery rate = 1-PPV
    double false_discovery_rate() const {
        return double(c_[CntFP]) / predict_pos();
    }
};


#ifdef __cplusplus
extern "C" {
#endif

// Simple quantile functions: these compute quantiles from an array of sorted
// values.  These don't do anything fancy; they're just glorified indexing
// functions (which sometimes interpolate between two values).  These return
// SimNAN if the given quantile is unavailable.  If "sample_not_population" is
// set, these use a modified formula which seeks to estimate the population's
// quantiles from the given samples.
//
// Returns the <k>-th <q>-quantile.  Requires 0 < k < q.
double quantile_kq(const double *sorted_data, int data_nelem,
                   bool sample_not_population, int k, int q);
// Returns the <p>-quantile.  Requires 0 < p < 1.
double quantile_p(const double *sorted_data, int data_nelem,
                  bool sample_not_population, double p);

#ifdef __cplusplus
}       // closes extern "C" block
#endif

#endif  // ONLINE_STATS_H
