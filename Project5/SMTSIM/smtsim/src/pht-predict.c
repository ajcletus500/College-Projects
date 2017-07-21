/*
 * PHT (branch predictor) object
 *
 * Jeff Brown
 * $Id: pht-predict.c,v 1.4.6.1.2.1 2008/04/30 22:17:54 jbrown Exp $
 */

const char RCSid_1035872632[] = 
"$Id: pht-predict.c,v 1.4.6.1.2.1 2008/04/30 22:17:54 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "pht-predict.h"
#include "utils.h"


struct PHTPredict {
    int n_entries, inst_bytes;
    int inst_bytes_lg;

    /* in the branchtable, 0 = strongly not taken, 3 = strongly taken */
    int *entries;               /* 1D array [n_entries] */

    PHTStats stats;
};


PHTPredict *
pht_create(int n_entries, int inst_bytes)
{
    PHTPredict *n = NULL;
    int log_inexact;

    sim_assert(n_entries > 0);
    sim_assert(inst_bytes > 0);

    if (!(n = malloc(sizeof(*n)))) {
        fprintf(stderr, "%s (%s:%i): out of memory, top-level\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }
    memset(n, 0, sizeof(*n));

    n->n_entries = n_entries;
    n->inst_bytes = inst_bytes;

    floor_log2(n_entries, &log_inexact);
    if (log_inexact) {
        fprintf(stderr, "%s (%s:%i): n_entries (%i) not a power of 2\n",
                __func__, __FILE__, __LINE__, n_entries);
        goto fail;
    }

    n->inst_bytes_lg = floor_log2(inst_bytes, &log_inexact);
    if (log_inexact) {
        fprintf(stderr, "%s (%s:%i): inst_bytes (%i) not a power of 2\n",
                __func__, __FILE__, __LINE__, inst_bytes);
        goto fail;
    }

    if (!(n->entries = malloc(n->n_entries * sizeof(n->entries[0])))) {
        fprintf(stderr, "%s (%s:%i): out of memory, allocating entries\n",
                __func__, __FILE__, __LINE__);
        goto fail;
    }

    pht_reset(n);
    return n;

fail:
    pht_destroy(n);
    return NULL;
}


void
pht_destroy(PHTPredict *pht)
{
    if (pht) {
        free(pht->entries);
        free(pht);
    }
}


void
pht_reset(PHTPredict *pht)
{
    long i;

    for (i = 0; i < pht->n_entries; i++) {
        pht->entries[i] = 2;    /* weakly taken */
    }

    pht_reset_stats(pht);
}


void
pht_reset_stats(PHTPredict *pht)
{
    pht->stats.hits = pht->stats.misses = 0;
}


int
pht_lookup(PHTPredict *pht, u64 addr, int taken, unsigned ghr)
{
    int entry_num = ((addr >> pht->inst_bytes_lg) ^ ghr) & 
        (pht->n_entries - 1);
    int pred_taken = pht->entries[entry_num] > 1;

    if ((taken != 0) == pred_taken) {
        pht->stats.hits++;
    } else {
        pht->stats.misses++;
    }

    return pred_taken;
}


int
pht_probe(const PHTPredict *pht, u64 addr, unsigned ghr)
{
    int entry_num = ((addr >> pht->inst_bytes_lg) ^ ghr) & 
        (pht->n_entries - 1);
    int pred_taken = pht->entries[entry_num] > 1;

    return pred_taken;
}



void
pht_update(PHTPredict *pht, u64 addr, int taken, unsigned ghr)
{
    int entry_num = ((addr >> pht->inst_bytes_lg) ^ ghr) &
        (pht->n_entries - 1);
    int val = pht->entries[entry_num];

    if (taken) {
        if (val < 3)
            val++;
    } else {
        if (val > 0)
            val--;
    }

    pht->entries[entry_num] = val;
}


void
pht_get_stats(const PHTPredict *pht, PHTStats *dest)
{
    memcpy(dest, &pht->stats, sizeof(*dest));
}
