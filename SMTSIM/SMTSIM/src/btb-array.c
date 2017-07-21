/*
 * BTB object
 *
 * Jeff Brown
 * $Id: btb-array.c,v 1.6.6.2.2.1 2008/04/30 22:17:45 jbrown Exp $
 */

const char RCSid_1035567839[] = 
"$Id: btb-array.c,v 1.6.6.2.2.1 2008/04/30 22:17:45 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "btb-array.h"
#include "assoc-array.h"
#include "utils.h"


typedef struct BTBEntry {
    u64 dest;
} BTBEntry;


struct BTBArray {
    int n_entries, assoc, inst_bytes;
    int inst_bytes_lg;

    AssocArray *cam;
    BTBEntry *entries;          /* 1D array [n_entries] */
    BTBStats stats;

    struct {
        int valid;
        long line_num;  
        int way_num;
        int was_hit;

        /* Parameters passed to btb_lookup() */
        u64 addr;
        int thread_id;
    } last_lookup;
};


BTBArray *
btb_create(int n_entries, int assoc, int inst_bytes)
{
    BTBArray *n = NULL;
    int log_inexact;

    sim_assert(n_entries > 0);
    sim_assert(assoc > 0);
    sim_assert(inst_bytes > 0);

    if (!(n = malloc(sizeof(*n)))) {
        fprintf(stderr, "%s (%s:%i): out of memory, top-level\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }
    memset(n, 0, sizeof(*n));

    n->n_entries = n_entries;
    n->assoc = assoc;
    n->inst_bytes = inst_bytes;

    if (n_entries % assoc) {
        fprintf(stderr, "%s (%s:%i): assoc (%i) doesn't divide n_entries "
                "(%i)\n", __func__, __FILE__, __LINE__, assoc, n_entries);
        goto fail;
    }

    n->inst_bytes_lg = floor_log2(inst_bytes, &log_inexact);
    if (log_inexact) {
        fprintf(stderr, "%s (%s:%i): inst_bytes (%i) not a power of 2\n",
                __func__, __FILE__, __LINE__, inst_bytes);
        goto fail;
    }

    if (!(n->cam = aarray_create(n_entries / assoc, n->assoc, "LRU"))) {
        fprintf(stderr, "%s (%s:%i): couldn't create AssocArray\n",
                __func__, __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->entries = malloc(n->n_entries * sizeof(n->entries[0])))) {
        fprintf(stderr, "%s (%s:%i): out of memory, allocating entries\n",
                __func__, __FILE__, __LINE__);
        goto fail;
    }

    btb_reset(n);
    return n;

fail:
    btb_destroy(n);
    return NULL;
}


void
btb_destroy(BTBArray *btb)
{
    if (btb) {
        aarray_destroy(btb->cam);
        free(btb->entries);
        free(btb);
    }
}


void
btb_reset(BTBArray *btb)
{
    long i;
    aarray_reset(btb->cam);

    for (i = 0; i < btb->n_entries; i++) {
        BTBEntry *ent = btb->entries + i;
        ent->dest = 0;
    }

    btb->last_lookup.valid = 0;

    btb_reset_stats(btb);
}


void
btb_reset_stats(BTBArray *btb)
{
    btb->stats.hits = btb->stats.misses = 0;
    btb->stats.jump_dest_mismatch = btb->stats.miss_not_taken = 0;
}


int
btb_lookup(BTBArray *btb, u64 addr, int thread_id,
           const BTBLookupInfo *info, u64 *dest_ret)
{
    AssocArrayKey lookup_key;
    int is_hit;
    long line_num;
    int way_num;

    lookup_key.lookup = addr >> btb->inst_bytes_lg;
    lookup_key.match = thread_id;

    is_hit = aarray_lookup(btb->cam, &lookup_key, &line_num, &way_num);

    if (is_hit) {
        BTBEntry *ent = btb->entries + btb->assoc * line_num + way_num;
        btb->stats.hits++;
        if (info->is_jump && (ent->dest != info->dest))
            btb->stats.jump_dest_mismatch++;
        *dest_ret = ent->dest;
    } else {
        line_num = -1;
        way_num = -1;
        btb->stats.misses++;
        if (!info->taken)
            btb->stats.miss_not_taken++;
    }

    btb->last_lookup.valid = 1;
    btb->last_lookup.line_num = line_num;
    btb->last_lookup.way_num = way_num;
    btb->last_lookup.was_hit = is_hit;
    btb->last_lookup.addr = addr;
    btb->last_lookup.thread_id = thread_id;

    return is_hit;
}


int
btb_probe(const BTBArray *btb, u64 addr, int thread_id, u64 *dest_ret)
{
    AssocArrayKey lookup_key;
    int is_hit;
    long line_num;
    int way_num;

    lookup_key.lookup = addr >> btb->inst_bytes_lg;
    lookup_key.match = thread_id;

    is_hit = aarray_probe(btb->cam, &lookup_key, &line_num, &way_num);

    if (is_hit) {
        const BTBEntry *ent = btb->entries + line_num * btb->assoc + way_num;
        *dest_ret = ent->dest;
    }

    return is_hit;
}


void 
btb_update(BTBArray *btb, u64 addr, int thread_id, u64 dest)
{
    long line_num;
    int way_num;
    BTBEntry *ent;

    sim_assert(btb->last_lookup.valid);
    sim_assert(addr == btb->last_lookup.addr);
    sim_assert(thread_id == btb->last_lookup.thread_id);

    if (!btb->last_lookup.was_hit) {
        AssocArrayKey new_key;
        new_key.lookup = addr >> btb->inst_bytes_lg;
        new_key.match = thread_id;
        aarray_replace(btb->cam, &new_key, &line_num, &way_num, NULL);
    } else {
        line_num = btb->last_lookup.line_num;
        way_num = btb->last_lookup.way_num;
    }

    sim_assert((line_num >= 0) && (way_num >= 0));
    ent = btb->entries + line_num * btb->assoc + way_num;
    ent->dest = dest;
}


void
btb_get_stats(const BTBArray *btb, BTBStats *dest)
{
    memcpy(dest, &btb->stats, sizeof(*dest));

    dest->eff_hits = dest->hits - dest->jump_dest_mismatch;
    dest->eff_misses = dest->misses + dest->jump_dest_mismatch -
        dest->miss_not_taken;
}


u64
btb_calc_baseaddr(const BTBArray *btb, u64 addr)
{
    u64 base_addr = addr & ~(btb->inst_bytes - 1);
    return base_addr;
}

