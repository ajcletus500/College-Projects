//
// Multiple-branch predictor
//
// Jeff Brown
// $Id: multi-bpredict.cc,v 1.2.2.2.2.1 2008/04/30 22:17:53 jbrown Exp $
//

const char RCSid_1054846039[] = 
"$Id: multi-bpredict.cc,v 1.2.2.2.2.1 2008/04/30 22:17:53 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>

#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "multi-bpredict.h"
#include "utils.h"

using std::vector;


MultiBPredict::MultiBPredict(const MultiBPredictParams& params_)
    : params(params_)
{
    sim_assert(params.n_rows > 0);
    sim_assert(params.predict_width > 0);
    sim_assert(params.inst_bytes > 0);

    int log_inexact;
    inst_bytes_lg = floor_log2(params.inst_bytes, &log_inexact);
    if (log_inexact) {
        fprintf(stderr, "(%s:%i): inst_bytes (%i) not a power of 2\n",
                __FILE__, __LINE__, params.inst_bytes);
        exit(1);
    }

    n_rows_lg = floor_log2(params.n_rows, &log_inexact);
    if (log_inexact) {
        fprintf(stderr, "(%s:%i): n_rows (%s) not a power of 2\n",
                __FILE__, __LINE__, fmt_u64(params.n_rows));
        exit(1);
    }

    // Does _not_ do a reset; do that from the super-class, after you've
    // initialized the good stuff
}


MultiBPredict::~MultiBPredict()
{
}


void
MultiBPredict::print_stats(void *c_FILE_out, const char *prefix) const {
    FILE *f = static_cast<FILE *>(c_FILE_out);
    fprintf(f, "%srows: %s width: %i", prefix,
            fmt_u64(params.n_rows), params.predict_width);
}



// A row of 2-bit counters in the PHT
class PHTRow {
    typedef u32 row_t;
    static const int col_bits = 2;
    static const unsigned col_mask = ((1U << col_bits) - 1);
    static const unsigned max_cols = ((8 * sizeof(row_t)) / col_bits);

    row_t cols;
    
//private:
//    PHTRow(const PHTRow& src);
//    PHTRow& operator = (const PHTRow &src);

public:
    PHTRow(int n_cols) { 
        sim_assert(n_cols > 0);
        if (static_cast<unsigned>(n_cols) > max_cols) {
            fprintf(stderr, "(%s:%i): sorry, n_cols %s is greater than this "
                    "crufty implementation supports (%i)\n", __FILE__,
                    __LINE__, fmt_u64(n_cols), max_cols);
            exit(1);
        }
    }

    unsigned get_col(int idx) const {
        sim_assert((idx >= 0) && (static_cast<unsigned>(idx) < max_cols));
        return (cols >> (col_bits * idx)) & col_mask;
    }

    bool pred_taken(int idx) const {
        return get_col(idx) > 1;
    }

    void set_col(int idx, unsigned val) {
        sim_assert((idx >= 0) && (static_cast<unsigned>(idx) < max_cols));
        sim_assert((val & col_mask) == val);
        const int shift_count = col_bits * idx;
        cols = (cols & ~(col_mask << shift_count)) | (val << shift_count);
    }

    void update(int idx, bool taken) {
        unsigned state = get_col(idx);
        if (taken) {
            if (state < 3)
                state++;
        } else {
            if (state > 0)
                state--;
        }
        set_col(idx, state);
    }

    void reset(int n_cols) {
        for (int col = 0; col < n_cols; col++)
            set_col(col, 2);    // 2: weakly taken
    }
};


class MBP_CountPerBr : public MultiBPredict {
    struct OutcomeStats {
        i64 hits, misses;
        void reset() { hits = misses = 0; }
    };

    int n_cols;
    vector<PHTRow> rows;
    vector<OutcomeStats> outcome_stats;

    // Disallow copy or assignment
    MBP_CountPerBr(const MBP_CountPerBr& src);
    MBP_CountPerBr& operator = (const MBP_CountPerBr &src);

    inline u32 select_row(mem_addr addr, u64 ghr) const {
        u32 entry_num = ((addr >> inst_bytes_lg) ^ ghr) & 
            (params.n_rows - 1);
        return entry_num;
    }

public:
    MBP_CountPerBr(const MultiBPredictParams& params_)
        : MultiBPredict(params_)
    {
        n_cols = params.predict_width;
        for (u32 i = 0; i < params.n_rows; i++)
            rows.push_back(PHTRow(n_cols));
        outcome_stats.resize(params.predict_width);
        reset();
    }

    ~MBP_CountPerBr() { }

    void reset() {
        for (u32 i = 0; i < params.n_rows; i++)
            rows[i].reset(n_cols);
        reset_stats();
    }

    void reset_stats() {
        for (int i = 0; i < params.predict_width; i++)
            outcome_stats[i].reset();
    }

    u32 predict(mem_addr addr, u64 ghr) {
        u32 result = 0;
        u32 row_num = select_row(addr, ghr);
        PHTRow& row = rows[row_num];
        for (int col = 0; col < n_cols; col++) {
            if (row.pred_taken(col))
                result |= static_cast<u32>(1) << col;
        }
        return result;
    }

    void update(mem_addr addr, u64 ghr, int outcome_num, bool outcome,
                bool was_correct) {
        u32 row_num = select_row(addr, ghr);
        PHTRow& row = rows[row_num];
        sim_assert((outcome_num >= 0) && (outcome_num < n_cols));
        row.update(outcome_num, outcome);
        OutcomeStats& os = outcome_stats[outcome_num];
        if (was_correct)
            os.hits++;
        else
            os.misses++;
    }

    void print_stats(void *c_FILE_out, const char *prefix) const {
        MultiBPredict::print_stats(c_FILE_out, prefix);
        FILE *f = static_cast<FILE *>(c_FILE_out);
        fprintf(f, " type: CountPerBr\n");
        i64 total_hits = 0, total_misses = 0;

        fprintf(f, "%shits/misses:", prefix);
        for (int i = 0; i < n_cols; i++) {
            const OutcomeStats& os = outcome_stats[i];
            fprintf(f, " %s/%s", fmt_i64(os.hits), fmt_i64(os.misses));
            total_hits += os.hits;
            total_misses += os.misses;
        }
        fprintf(f, "\n");

        fprintf(f, "%shit rates(%%):", prefix);
        for (int i = 0; i < n_cols; i++) {
            const OutcomeStats& os = outcome_stats[i];
            fprintf(f, " %.2f", (100.0 * os.hits) / (os.hits + os.misses));
        }
        fprintf(f, "\n");

        fprintf(f, "%suse dist:", prefix);
        for (int i = 0; i < n_cols; i++) {
            const OutcomeStats& os = outcome_stats[i];
            fprintf(f, " %.3f", (double) (os.hits + os.misses) 
                    / (total_hits + total_misses));
        }
        fprintf(f, "\n");
    }
};



//
// C interface
//


MultiBPredict *
mbp_create(const MultiBPredictParams *params)
{
    return new MBP_CountPerBr(*params);
}

void 
mbp_destroy(MultiBPredict *mbp)
{
    if (mbp)
        delete mbp;
}

void mbp_reset(MultiBPredict *mbp)
{
    mbp->reset();
}

void mbp_reset_stats(MultiBPredict *mbp)
{
    mbp->reset_stats();
}

u32 
mbp_predict(MultiBPredict *mbp, mem_addr addr, u64 ghr)
{
    return mbp->predict(addr, ghr);
}

void 
mbp_update(MultiBPredict *mbp, mem_addr addr, u64 ghr, int outcome_num,
           int outcome, int was_correct)
{
    mbp->update(addr, ghr, outcome_num, outcome, was_correct);
}

void 
mbp_print_stats(const MultiBPredict *mbp, void *c_FILE_out,
                const char *prefix)
{
    mbp->print_stats(c_FILE_out, prefix);
}
