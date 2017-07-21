/*
 * PHT (branch predictor) object
 *
 * Jeff Brown
 * $Id: pht-predict.h,v 1.2.16.1 2008/04/30 22:17:54 jbrown Exp $
 */

#ifndef PHT_PREDICT_H
#define PHT_PREDICT_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct PHTPredict PHTPredict;
typedef struct PHTStats PHTStats;


struct PHTStats {
    i64 hits;           /* Taken/not-taken prediction matched */
    i64 misses;         /* Prediction mismatched */
};


PHTPredict *pht_create(int n_entries, int inst_bytes);
void pht_destroy(PHTPredict *pht);

void pht_reset(PHTPredict *pht);
void pht_reset_stats(PHTPredict *pht);

/*
 */
int pht_lookup(PHTPredict *pht, u64 addr, int taken, unsigned ghr);

int pht_probe(const PHTPredict *pht, u64 addr, unsigned ghr);

void pht_update(PHTPredict *pht, u64 addr, int taken, unsigned ghr);

void pht_get_stats(const PHTPredict *pht, PHTStats *dest);


#ifdef __cplusplus
}
#endif

#endif  /* PHT_PREDICT_H */
