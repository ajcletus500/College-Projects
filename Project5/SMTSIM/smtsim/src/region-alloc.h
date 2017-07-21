//
// Memory allocation manager
//
// Jeff Brown
// $Id: region-alloc.h,v 1.2.6.1 2007/09/19 08:48:49 jbrown Exp $
//

#ifndef REGION_ALLOC_H
#define REGION_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RegionAlloc RegionAlloc;


//
// This module provides an interface for managing dynamically allocated
// memory.  This is meant to be used for managing relatively large blocks of
// memory ("regions").  The time cost of operations, as well as the
// per-allocation space overhead, may be orders of magnitude greater than
// those of malloc, but with the benefit of more efficient VM usage.  (This
// ain't yer pappy's allocator.)
//

// Create a new allocation manager.
//
// If "zero_fill_new_mem" is set, new memory acquired by the "alloc" or
// "resize" operations will be zero-filled.
RegionAlloc *ralloc_create(int zero_fill_new_mem);

// Destroy an allocation manager and release all of its managed memory
void ralloc_destroy(RegionAlloc *ra);

//
// Allocate, resize, and de-allocate dynamic memory.
//
// These have malloc/realloc/free-like semantics.  Resizing a NULL memory
// pointer is equivalent to alloc'ing new memory.  Dealloc'ing a NULL pointer
// is a no-op.  Resizing may invalidate the current allocation and relocate it
// at a new address, even when shrinking (unlike realloc).  If a resize fails,
// the current allocation is still valid.
//
void *ralloc_alloc(RegionAlloc *ra, size_t size);
void *ralloc_resize(RegionAlloc *ra, void *mem, size_t new_size);
void ralloc_dealloc(RegionAlloc *ra, void *mem);

// These are simple wrappers for the above functions, which print an error 
// message and exit() on any failure.
void *ralloc_alloc_e(RegionAlloc *ra, size_t size);
void *ralloc_resize_e(RegionAlloc *ra, void *mem, size_t new_size);


#ifdef __cplusplus
}
#endif

#endif  /* REGION_ALLOC_H */
