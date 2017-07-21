// -*- C++ -*-
//
// Multiple-branch predictor
//
// Jeff Brown
// $Id: multi-bpredict.h,v 1.2.2.1.6.1 2008/04/30 22:17:54 jbrown Exp $
//

#ifndef MULTI_BPREDICT_H
#define MULTI_BPREDICT_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct MultiBPredict MultiBPredict;
typedef struct MultiBPredictStats MultiBPredictStats;
typedef struct MultiBPredictParams MultiBPredictParams;


struct MultiBPredictParams {
    u32 n_rows;
    int predict_width;
    int inst_bytes;
};


MultiBPredict *mbp_create(const MultiBPredictParams *params);
void mbp_destroy(MultiBPredict *mbp);

void mbp_reset(MultiBPredict *mbp);
void mbp_reset_stats(MultiBPredict *mbp);

// Return the next predict_width predictions, the first at bit 0
u32 mbp_predict(MultiBPredict *mbp, mem_addr addr, u64 ghr);

void mbp_update(MultiBPredict *mbp, mem_addr addr, u64 ghr, 
                int outcome_num, int outcome, int was_correct);

void mbp_print_stats(const MultiBPredict *mbp, void *c_FILE_out,
                     const char *prefix);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

struct MultiBPredict {
protected:
    MultiBPredictParams params;
    int inst_bytes_lg, n_rows_lg;

public:
    MultiBPredict(const MultiBPredictParams& params);
    virtual ~MultiBPredict();

    virtual void reset() = 0;
    virtual void reset_stats() = 0;
    virtual u32 predict(mem_addr addr, u64 ghr) = 0;
    virtual void update(mem_addr addr, u64 ghr, int outcome_num,
                        bool outcome, bool was_correct) = 0;
    virtual void print_stats(void *c_FILE_out, const char *prefix) const = 0;
};

#endif  // __cplusplus

#endif  /* MULTI_BPREDICT_H */
