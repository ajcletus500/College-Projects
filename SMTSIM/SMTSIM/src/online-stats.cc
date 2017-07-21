//
// "On-line" statistics computation -- computes stats on data as it becomes
// available
//
// Jeff Brown
// $Id: online-stats.cc,v 1.3.2.7.2.1.2.10 2009/11/19 01:23:43 jbrown Exp $
//

const char RCSid_1044044836[] =
"$Id: online-stats.cc,v 1.3.2.7.2.1.2.10 2009/11/19 01:23:43 jbrown Exp $";

#include <math.h>
#include <string.h>

#include <iomanip>
#include <limits>
#include <sstream>

#include "sim-assert.h"
#include "online-stats.h"
#include "utils.h"


using std::numeric_limits;
using std::ostringstream;


namespace {

double
variance_common(bool sample_not_population, double sum, double sum_sqr,
                i64 count)
{
    i64 denom = (sample_not_population) ? (count - 1) : count;
    double result;
    if (denom > 0) {
        double numer = sum_sqr - (sum * sum) / count;
        result = numer / denom;
    } else {
        result = SimNAN;
    }
    return result;
}


}       // Anonymous namespace close


void
BasicStat_I64::reset()
{
    count_ = 0;
    max_ = numeric_limits<ValType>::min();
    min_ = numeric_limits<ValType>::max();
    sum_ = 0;
    sum_sqr_ = 0;
    sum_overflow_ = false;
    sum_sqr_overflow_ = false;
}


std::string
BasicStat_I64::fmt() const
{
    ostringstream out;
    if (g_count()) {
        out << std::fixed << std::setprecision(0);
        out << g_min() << "/" << g_mean() << "/" << g_max()
            << " (n=" << g_count()
            << ",sd=" << sqrt(g_variance(false)) << ")";
    } else {
        out << "- (n=0)";
    }
    return out.str();
}


double 
BasicStat_I64::g_mean() const
{
    if (!count_ || sum_overflow_)
        return SimNAN;
    return static_cast<double>(sum_) / count_;
}


double 
BasicStat_I64::g_variance(bool sample_not_population) const
{
    double result;
    if (sum_overflow_ || sum_sqr_overflow_) {
        result = SimNAN;
    } else {
        result = variance_common(sample_not_population, sum_, sum_sqr_,
                                 count_);
    }
    return result;
}


void
BasicStat_Double::reset()
{
    count_ = 0;
    max_ = numeric_limits<ValType>::min();
    min_ = numeric_limits<ValType>::max();
    sum_ = 0;
    sum_sqr_ = 0;
}


std::string
BasicStat_Double::fmt() const
{
    ostringstream out;
    if (g_count()) {
        out << std::fixed << std::setprecision(0);
        out << g_min() << "/" << g_mean() << "/" << g_max()
            << " (n=" << g_count()
            << ",sd=" << sqrt(g_variance(false)) << ")";
    } else {
        out << "- (n=0)";
    }
    return out.str();
}


double 
BasicStat_Double::g_mean() const
{
    if (!count_)
        return SimNAN;
    return sum_ / count_;
}


double 
BasicStat_Double::g_variance(bool sample_not_population) const
{
    double result;
    result = variance_common(sample_not_population, sum_, sum_sqr_, count_);
    return result;
}


i64
HistCount_Int::total_count() const
{
    BucketMap::const_iterator iter = counts_.begin(), end = counts_.end();
    i64 total = 0;
    for (; iter != end; ++iter)
        total += iter->second;
    return total;
}


void
HistCount_Int::get_all_keys(HistCount_Int::KeySet& dest) const
{
    dest.clear();
    BucketMap::const_iterator iter = counts_.begin(), end = counts_.end();
    for (; iter != end; ++iter)
        dest.insert(iter->first);
}


std::string
BiConfusionMatrix::fmt_brief() const
{
    ostringstream out;
    out << "TP=" << c_[CntTP]
        << " TN=" << c_[CntTN]
        << " FP=" << c_[CntFP]
        << " FN=" << c_[CntFN];
    return out.str();
}


std::string
BiConfusionMatrix::fmt_verbose(const std::string& prefix) const
{
    ostringstream out;
    out << std::fixed << std::setprecision(4);
    out << prefix
        << "TP: " << c_[CntTP]
        << " TN: " << c_[CntTN]
        << " FP: " << c_[CntFP]
        << " FN: " << c_[CntFN]
        << "\n";
    out << prefix
        << "P: " << actual_pos()
        << " N: " << actual_neg()
        << " P': " << predict_pos()
        << " N': " << predict_neg()
        << "\n";
    out << prefix
        << "match: " << match()
        << " mismatch: " << mismatch()
        << " all: " << all()
        << "\n";
    out << prefix
        << "Sens: " << sensitivity()
        << " Spec: " << specificity()
        << " FPR: " << false_pos_rate()
        << " FNR: " << false_neg_rate()
        << "\n";
    out << prefix
        << "PPV: " << pos_pred_value()
        << " NPV: " << neg_pred_value()
        << "\n";
    out << prefix
        << "MCC: " << matthews_cc()
        << " Acc: " << accuracy()
        << "\n";
    return out.str();
}


double
BiConfusionMatrix::matthews_cc() const
{
    // copy values to doubles to avoid 1) overflow and 2) lots of casts
    double tp = c_[CntTP], tn = c_[CntTN], fp = c_[CntFP], fn = c_[CntFN];
    double result;
    result = ((tp * tn) - (fp * fn)) /
        sqrt((tp + fp) * (tp + fn) * (tn + fp) * (tn + fn));
    return result;
}


double
quantile_kq(const double *sorted_data, int data_nelem,
            bool sample_not_population, int k, int q)
{
    double result;
    sim_assert((k > 0) && (k < q));
    sim_assert(data_nelem >= 0);
    
    abort_printf("untested stats code\n");

    if (!data_nelem) {
        result = SimNAN;
    } else if (!sample_not_population) {
        // entire population: straightforward calculation
        if ((data_nelem * k) % q) {
            // doesn't evenly divide; round up to next integer index, use that
            // (then subtract 1, for 0-based array) 
            int idx = (data_nelem * k) / q;
            // idx' = floor(idx) + 1 - 1 == idx, for integer >= 0
            sim_assert(IDX_OK(idx, data_nelem));
            result = sorted_data[idx];
        } else {
            // k/q evenly divides array; use mean of nearest two elements
            int idx1 = ((data_nelem * k) / q) - 1;
            int idx2 = idx1 + 1;
            sim_assert(IDX_OK(idx1, data_nelem));
            sim_assert(IDX_OK(idx2, data_nelem));
            result = (sorted_data[idx1] + sorted_data[idx2]) / 2.0;
        }
    } else {
        // estimation is trickier
        if (((data_nelem + 1) * k) % q) {
            // use weighted mean of adjacent elements, if we have them
            int idx1 = ((data_nelem + 1) * k) / q - 1;
            int idx2 = idx1 + 1;
            if ((idx1 < 0) || (idx2 >= data_nelem)) {
                result = SimNAN;
            } else {
                double weight = (data_nelem + 1) * (double(k) / q);
                weight = weight - floor(weight);
                sim_assert(IDX_OK(idx1, data_nelem));
                sim_assert(IDX_OK(idx2, data_nelem));
                sim_assert((weight >= 0.0) && (weight < 1.0));
                result = (1.0 - weight) * sorted_data[idx1] +
                    weight * sorted_data[idx2];
            }
        } else {
            // k/q evenly divides (n+1)
            int idx = ((data_nelem + 1) * k) / q;
            // k<q => (k/q)<1 => idx<(n+1); idx>0; idx an int => 1<=idx<=n
            --idx;
            sim_assert(IDX_OK(idx, data_nelem));
            result = sorted_data[idx];
        }
    }

    return result;
}


double
quantile_p(const double *sorted_data, int data_nelem,
           bool sample_not_population, double p)
{
    // Here, p takes the place of "k/q".  We'll just always use weighted
    // means instead of trying to hit a specific element (i.e., not
    // bothering to test "is n*p an integer?").

    abort_printf("untested stats code\n");

    double result;
    sim_assert((p > 0.0) && (p < 1.0));
    sim_assert(data_nelem >= 0);

    {
        // use weighted mean of adjacent elements; which two varies by method
        int idx1, idx2;
        double weight;

        if (!sample_not_population) {
            idx1 = i64(floor(data_nelem * p));
            weight = (data_nelem * p) - idx1;
        } else {
            // estimation
            idx1 = i64(floor((data_nelem + 1) * p) - 1);
            weight = ((data_nelem + 1) * p) - 1 - idx1;
        }
        idx2 = idx1 + 1;
        if ((idx1 < 0) || (idx2 >= data_nelem)) {
            result = SimNAN;
        } else {
            sim_assert(IDX_OK(idx1, data_nelem));
            sim_assert(IDX_OK(idx2, data_nelem));
            sim_assert((weight >= 0.0) && (weight < 1.0));
            result = (1.0 - weight) * sorted_data[idx1] +
                weight * sorted_data[idx2];
        }
    }

    return result;
}
