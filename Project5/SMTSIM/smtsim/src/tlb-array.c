/*
 * Fully-associative TLB object with simplified accounting
 *
 * Jeff Brown
 * $Id: tlb-array.c,v 1.11.6.5.2.1.2.4 2009/08/05 22:40:03 jbrown Exp $
 */

const char RCSid_1035344092[] = 
"$Id: tlb-array.c,v 1.11.6.5.2.1.2.4 2009/08/05 22:40:03 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim-assert.h"
#include "sys-types.h"
#include "tlb-array.h"
#include "assoc-array.h"
#include "utils.h"
#include "cache-params.h"


struct TLBArray {
    int n_entries, page_bytes;
    int page_bytes_lg;

    AssocArray *cam;
    i64 *ready_time;            /* 1D array [n_entries] */
    TLBStats stats;
};


TLBArray *
tlb_create(int n_entries, int page_bytes)
{
    TLBArray *n = NULL;
    int log_inexact;

    sim_assert(n_entries > 0);
    sim_assert(page_bytes > 0);

    if (!(n = malloc(sizeof(*n)))) {
        fprintf(stderr, "%s (%s:%i): out of memory, top-level\n", __func__,
                __FILE__, __LINE__);
        goto fail;
    }
    memset(n, 0, sizeof(*n));

    n->n_entries = n_entries;
    n->page_bytes = page_bytes;

    n->page_bytes_lg = floor_log2(page_bytes, &log_inexact);
    if (log_inexact) {
        fprintf(stderr, "%s (%s:%i): page_bytes (%i) not a power of 2\n",
                __func__, __FILE__, __LINE__, page_bytes);
        goto fail;
    }

    if (!(n->cam = aarray_create(1, n->n_entries, "LRU"))) {
        fprintf(stderr, "%s (%s:%i): couldn't create AssocArray\n",
                __func__, __FILE__, __LINE__);
        goto fail;
    }

    if (!(n->ready_time = malloc(n->n_entries * sizeof(n->ready_time[0])))) {
        fprintf(stderr, "%s (%s:%i): out of memory, allocating ready_time\n",
                __func__, __FILE__, __LINE__);
        goto fail;
    }

    tlb_reset(n);
    return n;

fail:
    tlb_destroy(n);
    return NULL;
}


void
tlb_destroy(TLBArray *tlb)
{
    if (tlb) {
        aarray_destroy(tlb->cam);
        free(tlb->ready_time);
        free(tlb);
    }
}


void
tlb_reset(TLBArray *tlb)
{
    long i;
    aarray_reset(tlb->cam);

    for (i = 0; i < tlb->n_entries; i++) {
        tlb->ready_time[i] = 0;
    }

    tlb_reset_stats(tlb);
}


void tlb_reset_stats(TLBArray *tlb)
{
    tlb->stats.hits = tlb->stats.misses = 0;
}


i64
tlb_lookup(TLBArray *tlb, i64 now, u64 addr, int thread_id, i64 miss_penalty,
           int *is_hit_ret)
{
    u64 page = addr >> tlb->page_bytes_lg;
    AssocArrayKey lookup_key;
    long line_num = -1;
    int way_num = -1;
    int is_hit;
    i64 result = 0;
    i64 ready_time, *r_ready_time;

    lookup_key.lookup = page;
    lookup_key.match = thread_id;

    is_hit = aarray_lookup(tlb->cam, &lookup_key, &line_num, &way_num);
    if (!is_hit)
        aarray_replace(tlb->cam, &lookup_key, &line_num, &way_num, NULL);

    sim_assert((line_num >= 0) && (way_num >= 0));
    r_ready_time = &tlb->ready_time[line_num * 1 + way_num];
    ready_time = *r_ready_time;

    if (ready_time < now) ready_time = now;

    if (is_hit) {
        tlb->stats.hits++;
    } else {
        tlb->stats.misses++;
        ready_time += miss_penalty;
    }
    result = ready_time - now;

    *r_ready_time = ready_time;
    if (is_hit_ret)
        *is_hit_ret = is_hit;
    return result;
}


int
tlb_probe(const TLBArray *tlb, u64 addr, int thread_id)
{
    u64 page = addr >> tlb->page_bytes_lg;
    AssocArrayKey lookup_key;
    long line_num;
    int way_num;
    int is_hit;

    lookup_key.lookup = page;
    lookup_key.match = thread_id;
    is_hit = aarray_probe(tlb->cam, &lookup_key, &line_num, &way_num);
    return is_hit;
}


void
tlb_inject(TLBArray *tlb, i64 ready_time, u64 addr, int thread_id)
{
    u64 page = addr >> tlb->page_bytes_lg;
    AssocArrayKey lookup_key;
    long line_num = 0;
    int way_num = 0;

    lookup_key.lookup = page;
    lookup_key.match = thread_id;

    if (!aarray_lookup(tlb->cam, &lookup_key, &line_num, &way_num))
        aarray_replace(tlb->cam, &lookup_key, &line_num, &way_num, NULL);

    i64 *r_ready_time = &tlb->ready_time[line_num * 1 + way_num];
    *r_ready_time = ready_time;

    DEBUGPRINTF("tlb_inject(%s, %d)\n", fmt_mem(addr), thread_id);
}


void
tlb_get_stats(const TLBArray *tlb, TLBStats *dest)
{
    memcpy(dest, &tlb->stats, sizeof(*dest));
}


u64
tlb_calc_baseaddr(const TLBArray *tlb, u64 addr)
{
    u64 base_addr = addr & ~(tlb->page_bytes - 1);
    return base_addr;
}


LongAddr *
tlb_get_tags(const TLBArray *tlb, int master_id, int *n_tags_ret)
{
    // based on CacheArray::get_tags(...)
    // (wow, the TLB module is still C)
    LongAddr *matches = NULL;
    int n_matches = 0;
    // (our TLBs are implicitly fully-associative)
    const int n_lines = 1;
    const int assoc = tlb->n_entries;

    matches = emalloc(tlb->n_entries * sizeof(*matches));

    for (long line_num = 0; line_num < n_lines; line_num++) {
        for (int way_num = 0; way_num < assoc; way_num++) {
            AssocArrayKey ent_key;
            if (aarray_readkey(tlb->cam, line_num, way_num, &ent_key)) {
                LongAddr ent_addr;
                laddr_set(ent_addr, ent_key.lookup << tlb->page_bytes_lg,
                          ent_key.match);
                if ((master_id == -1) || (master_id == (int) ent_addr.id)) {
                    sim_assert(n_matches < tlb->n_entries);
                    matches[n_matches] = ent_addr;
                    ++n_matches;
                }
            }
        }
    }

    if (n_matches > 0) {
        // shrink to fit
        LongAddr *shrunk = realloc(matches, n_matches * sizeof(*matches));
        if (shrunk) {
            matches = shrunk;
        }
    } else {
        free(matches);
        matches = NULL;
    }
    *n_tags_ret = n_matches;
    return matches;
}


// internal helper, since iterating over array is a pain
// warning: plays games with const-ness of "tlb", it's guaranteed not to be
// modified iff "flush_matches" is false
static int
tlb_count_maybe_flush(TLBArray *tlb, int master_id, int flush_matches)
{
    // based on tlb_get_tags(...)
    // (wow, the TLB module is still C)
    int n_matches = 0;
    // (our TLBs are implicitly fully-associative)
    const int n_lines = 1;
    const int assoc = tlb->n_entries;
    sim_assert(master_id >= 0);

    for (long line_num = 0; line_num < n_lines; line_num++) {
        for (int way_num = 0; way_num < assoc; way_num++) {
            AssocArrayKey ent_key;
            if (aarray_readkey(tlb->cam, line_num, way_num, &ent_key)) {
                if (master_id == (int) ent_key.match) {
                    sim_assert(n_matches < tlb->n_entries);
                    ++n_matches;
                    if (flush_matches) {
                        aarray_invalidate(tlb->cam, line_num, way_num);
                    }
                }
            }
        }
    }

    return n_matches;
}


int
tlb_get_population(const TLBArray *tlb, int master_id)
{
    // We're playing const-games here; with flush-flag set to 0, since
    // tlb_count_maybe_flush() promises not to change the TLB.
    TLBArray *tlb_ok_not_const = C_UNCHECKED_CAST(TLBArray *, tlb);
    return tlb_count_maybe_flush(tlb_ok_not_const, master_id, 0);
}


void
tlb_flush_app(TLBArray *tlb, int master_id)
{
    int entries_flushed;
    DEBUGPRINTF("tlb_flush_app(%d) ", master_id);
    entries_flushed = tlb_count_maybe_flush(tlb, master_id, 1);
    DEBUGPRINTF("-> %d entries flushed\n", entries_flushed);
}
