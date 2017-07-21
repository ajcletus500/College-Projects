/*
 * Cache parameter (geometry + timing) structures
 *
 * Jeff Brown
 * $Id: cache-params.h,v 1.2.6.5.2.2.2.5 2008/10/24 20:34:27 jbrown Exp $
 */

#ifndef CACHE_PARAMS_H
#define CACHE_PARAMS_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct CacheGeometry {
    int size_kb;
    int assoc;
    int block_bytes;
    int n_banks;
    int wb_buffer_size;         // Number of outbound writeback buffer entries
    struct {
        int r, w, rw;
    } ports;                    // Ports per bank

    // Config subtree where parameters related to this cache are located
    char *config_path;          // NULL, or "owned" malloc'd C-string
} CacheGeometry;

// in cache-array.cc
CacheGeometry *cachegeom_create(void);
CacheGeometry *cachegeom_copy(const CacheGeometry *cg);
void cachegeom_destroy(CacheGeometry *cg);


typedef struct CacheTiming {
    /* the time a cache bank is occupied due to 
       a miss when the data arrives */
    /* the time a cache bank is occupied due to an access */
    OpTime access_time;
    OpTime access_time_wb;
    OpTime fill_time;
    /* miss penalty; subtract the fill time from this to get the latency */
    int miss_penalty;
} CacheTiming;


typedef struct MemUnitParams {
    int block_bytes;
    int n_banks;
    OpTime read_time;           // Block read timing
    OpTime write_time;          // Block write timing
} MemUnitParams;


#ifdef __cplusplus
}
#endif

#endif  /* CACHE_PARAMS_H */
