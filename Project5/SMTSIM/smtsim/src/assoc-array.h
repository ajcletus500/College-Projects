/*
 * N-way-associative array object with goofy non-opaque keys
 *
 * Jeff Brown
 * $Id: assoc-array.h,v 1.11.6.4.2.1.2.1.6.1 2009/12/25 06:31:47 jbrown Exp $
 */

#ifndef ASSOC_ARRAY_H
#define ASSOC_ARRAY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AssocArray AssocArray;
typedef struct AssocArrayKey AssocArrayKey;


struct AssocArrayKey {
    // Note: "lookup" field should have block-offset bits shifted out already
    u64 lookup;         /* Determines line number, also used in matching */
    u32 match;          /* Used in matching only */
};


/*
 * n_lines must be a power of two.
 */
AssocArray *aarray_create(long n_lines, int assoc,
                          const char *replace_policy_name);

// Like aarray_create(), but uses the given path in the global simulator
// config tree for additional parameters.
AssocArray *aarray_create_simcfg(long n_lines, int assoc,
                                 const char *config_path);

void aarray_destroy(AssocArray *array);

void aarray_reset(AssocArray *array);


/*
 * Look up the key in the array, updating usage information.
 *
 * If the key is not found, zero is returned.
 *
 * If the key is found, nonzero is returned, and the corresponding entry
 * location is written to "line_num_ret" and "way_num_ret".
 */
int aarray_lookup(AssocArray *array, const AssocArrayKey *key,
                  long *line_num_ret, int *way_num_ret);


/*
 * Look up a key in the array like aarray_lookup(), but don't change any
 * state.
 */
int aarray_probe(const AssocArray *array, const AssocArrayKey *key,
                 long *line_num_ret, int *way_num_ret);


/*
 * Replace an entry in the array.  The new key must not already be in the
 * array.  A line/way is selected for replacement, and is overwritten with the
 * given key.  The location selected for replacement is written to
 * "line_num_ret" and "way_num_ret".  Nonzero is returned iff the old key was
 * valid, and the old key is optionally written to "old_key_ret".
 */
int aarray_replace(AssocArray *array, const AssocArrayKey *key,
                   long *line_num_ret, int *way_num_ret,
                   AssocArrayKey *old_key_ret);


/*
 * Invalidate an entry in the array.
 */
void aarray_invalidate(AssocArray *array, long line_num, int way_num);


/*
 * Touch an entry -- if you're updating an entry you know to be in the array,
 * this updates its replacement info, but does not affect lookup stats.  (It's
 * not clear that this models anything realistic.)
 */
void aarray_touch(AssocArray *array, long line_num, int way_num);


/* 
 * Read a key out of the array.
 * 
 * This reads out the key for the given entry.  If the entry is currently
 * valid, nonzero is returned and a copy of the key is written to "key_ret",
 * if it is non-NULL.  Otherwise, zero is returned and "key_ret" is ignored.
 */
int aarray_readkey(const AssocArray *array, long line_num, int way_num,
                   AssocArrayKey *key_ret);


#ifdef __cplusplus
}
#endif

#endif  /* ASSOC_ARRAY_H */
