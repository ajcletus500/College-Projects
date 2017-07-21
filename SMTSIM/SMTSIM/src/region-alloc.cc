//
// Memory allocation manager
//
// Jeff Brown
// $Id: region-alloc.cc,v 1.9.6.1.2.1.2.4 2009/12/05 21:40:07 jbrown Exp $
//

const char RCSid_1047457375[] =
"$Id: region-alloc.cc,v 1.9.6.1.2.1.2.4 2009/12/05 21:40:07 jbrown Exp $";

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>

#include <map>
#include <vector>

#include "region-alloc.h"
#include "sys-types.h"          // for types used in utils.h declarations
#include "utils.h"              // for e.g. exit_printf()
#include "sim-assert.h"         // for e.g. sim_assert(), sim_abort()


using std::map;
using std::vector;


#ifdef __linux__
    #define HAVE_MREMAP 1
#else
    #define HAVE_MREMAP 0
#endif

#define RA_MIN(a, b) (((a) <= (b)) ? (a) : (b))

// Using mmap(), even when available, sometimes causes problems.  
// valgrind in particular seems to have trouble with it.
#ifdef VALGRIND
  #define ALLOW_MMAP            0
#else
  #define ALLOW_MMAP            1
#endif
#define ALLOW_MREMAP            1

// If set, this allows the use of the "multi-mmap" allocator.  While nifty,
// it's more complex, so disable it in the event of strange crashes.
#define ALLOW_MULTI_MMAP        1

#ifdef DEBUG
    // Expensive, explicit test to verify zero-filledness of new memory
    #define DEBUG_VERIFY_ZERO_FILL      1
#else
    #define DEBUG_VERIFY_ZERO_FILL      0
#endif

#define MEM_DEBUG_LEVEL         0
#ifdef DEBUG
    #define MDEBUG(x) if (x > MEM_DEBUG_LEVEL) { } else printf
#else
    #define MDEBUG(x) if (1) { } else printf
#endif


namespace {

size_t PageSize = 0;

inline size_t
roundup_pagesize(size_t size)
{
    size_t result = size;
    size_t extra = (size % PageSize);
    if (extra)
        result += PageSize - extra;
    sim_assert((result % PageSize) == 0);
    sim_assert(result >= size);
    return result;
}

// Add a byte offset to a void pointer
inline void *
void_boffset(void *ptr, size_t offset)
{
    return static_cast<void *>(static_cast<char *>(ptr) + offset);
}

// byte-subtract two void pointers, ptr2 - ptr1
inline size_t
void_bdiff(const void *ptr2, const void *ptr1)
{
    const char *cp2 = static_cast<const char *>(ptr2);
    const char *cp1 = static_cast<const char *>(ptr1);
    sim_assert(cp2 >= cp1);
    return cp2 - cp1;
}

void 
mmap_free(void *mem, size_t size) 
{
    MDEBUG(2)("MDEBUG: mmap_free(%p, %lu)\n", mem, (unsigned long) size);
    if (munmap(mem, size)) {
        exit_printf("munmap failed: %s\n", strerror(errno));
    }
}

// Perform the requested mmap call, and return NULL if it fails.  This
// wrapper exists because we can't just compare the result with MAP_FAILED,
// since the system headers on OSF/1 screwily define MAP_FAILED as a value
// which can't be compared with a void pointer in C++.
void *
wrap_mmap(void *mem, size_t size, int prot, int flags, int fd, off_t offset)
{
    void *result;
    result = mmap(mem, size, prot, flags, fd, offset);
    if (result == reinterpret_cast<const void *>(MAP_FAILED))
        result = NULL;
    return result;
}

// mmap a region of at least "size" bytes at address "addr".  If "addr" is 0,
// let mmap pick an address.
void *
mmap_alloc(void *mem, size_t size)
{
    void *result;
    // mmap with MAP_ANON requires the use of file descriptor -1, and gives
    // us pages which are zero-filled by default.  (If for some reason
    // this didn't work, we could try mmap'ing "/dev/zero" with MAP_PRIVATE.)
    int flags = MAP_PRIVATE | MAP_ANON;
    if (0) {
        // MAP_FIXED seems like the perfect tool for this, when the caller has
        // specified the address they want.  Unfortunately, it seems that with
        // MAP_FIXED, some mmap implementations destroy any pre-existing maps
        // which overlap!  Since there may be other, unrelated mmaps at those
        // addresses, we really really don't want to do that.
        if (mem)
            flags |= MAP_FIXED;
        result = wrap_mmap(mem, size, PROT_READ | PROT_WRITE, flags, -1, 0);
    } else {
        result = wrap_mmap(mem, size, PROT_READ | PROT_WRITE, flags, -1, 0);
        if (mem && result && (result != mem)) {
            // If the caller specified the address they want, and mmap gave us
            // a different address, then dump the mapping and return failure.
            if (munmap(result, size)) {
                exit_printf("munmap after mmap unexpectedly failed: %s\n", 
                            strerror(errno));
            }
            result = NULL;
        }
    }
    MDEBUG(2)("MDEBUG: mmap_alloc(%p, %lu) -> %p\n", mem, (unsigned long) size,
              result);
    return result;
}

void *
mmap_resize(void *mem, size_t old_size, size_t new_size)
{
    void *result;
#if HAVE_MREMAP
    result = mremap(mem, old_size, new_size, MREMAP_MAYMOVE);
    if (result == MAP_FAILED) 
        result = NULL;
#else
    abort_printf("mmap_resize called without mremap support!\n");
    result = NULL;
#endif
    MDEBUG(2)("MDEBUG: mmap_resize(%p, %lu, %lu) -> %p\n", mem, 
              (unsigned long) old_size, (unsigned long) new_size,
              result);
    return result;
}

}


typedef map<void *, size_t> VoidSizeMap;


struct RegionAlloc {
protected:
    bool zero_fill_new_mem;
    VoidSizeMap mem_sizes;

    RegionAlloc(bool zero_fill_new_mem_)
        : zero_fill_new_mem(zero_fill_new_mem_) { }

    virtual void *do_alloc(size_t size) = 0;
    virtual void do_dealloc(void *mem, size_t size) = 0;
    virtual bool have_resize() const { return false; }
    virtual void *do_resize(void *mem, size_t old_size, 
                            size_t new_size) { sim_abort(); return NULL; }

    // Zero out new memory, if zero_fill_new_mem is set.
    void base_zero_new(void *mem, size_t old_size, size_t new_size) {
        // note: mem_sizes() may not be updated yet; we don't auto-invoke
        // this in the RegionAlloc base class, as inherited implementations
        // may be able to avoid it (say, with mmap()).
        sim_assert(new_size >= old_size);
        if (zero_fill_new_mem && (new_size > old_size)) {
            void *start = void_boffset(mem, old_size);
            memset(start, 0, new_size - old_size);
            MDEBUG(3)("MDEBUG: base_zero_new(%p, %lu, %lu)\n", mem,
                      (unsigned long) old_size, (unsigned long) new_size);
        }
    }

    // Expensive: if zero_fill_new_mem is set, verify that
    // mem[old_size]...mem[new_size-1] are 0-bytes.
    void verify_zero_new(void *mem, size_t old_size, size_t new_size) {
        sim_assert(new_size >= old_size);
        if (!zero_fill_new_mem)
            return;
        MDEBUG(3)("MDEBUG: verify_zero_new(%p, %lu, %lu)\n", mem,
                  (unsigned long) old_size, (unsigned long) new_size);
        char *end = static_cast<char *>(mem) + new_size;
        for (char *scan = static_cast<char *>(mem) + old_size;
             scan != end; ++scan) {
            if (*scan != 0) {
                abort_printf("RegionAlloc: verify_zero_new failed; "
                             "mem %p old_size %lu new_size %lu, "
                             "nonzero @ %p\n",
                             mem, (unsigned long) old_size,
                             (unsigned long) new_size, (void *) scan);
            }
        }
    }

    // This must be called from the derived class destructor, since it uses
    // the do_dealloc() method of the derived class, which is destroyed before
    // the base class destructor is called.
    void base_cleanup() {
        VoidSizeMap::iterator iter = mem_sizes.begin(), end = mem_sizes.end();
        for (; iter != end; ++iter) {
            sim_assert(iter->first != NULL);
            sim_assert(iter->second > 0);
            do_dealloc(iter->first, iter->second);
        }
        mem_sizes.clear();
    }

public:
    virtual ~RegionAlloc() { }

    void *alloc(size_t size) {
        sim_assert(size > 0);
        void *result = do_alloc(size);
        if (result) {
            sim_assert(mem_sizes.count(result) == 0);
            mem_sizes[result] = size;
            if (DEBUG_VERIFY_ZERO_FILL)
                verify_zero_new(result, 0, size);
        }
        MDEBUG(1)("MDEBUG: alloc(%lu) -> %p\n", (unsigned long) size, result);
        return result;
    }

    void *resize(void *mem, size_t new_size) {
        sim_assert(new_size > 0);
        void *result;
        if (mem) {
            sim_assert(mem_sizes.count(mem) != 0);
            size_t old_size = mem_sizes[mem];
            sim_assert(old_size > 0);
            if (have_resize()) {
                result = do_resize(mem, old_size, new_size);
            } else {
                size_t copy_size = RA_MIN(old_size, new_size);
                result = do_alloc(new_size);
                if (result) {
                    memcpy(result, mem, copy_size);
                    do_dealloc(mem, old_size);
                }
            }
            if (result) {
                mem_sizes.erase(mem);
                sim_assert(mem_sizes.count(result) == 0);
                mem_sizes[result] = new_size;
                if (DEBUG_VERIFY_ZERO_FILL && (new_size > old_size))
                    verify_zero_new(result, old_size, new_size);
            }
        } else {
            result = alloc(new_size);
        }
        MDEBUG(1)("MDEBUG: resize(%p, %lu) -> %p\n", mem,
                  (unsigned long) new_size, result);
        return result;
    }

    void dealloc(void *mem) {
        MDEBUG(1)("MDEBUG: dealloc(%p)\n", mem);
        if (mem) {
            sim_assert(mem_sizes.count(mem) != 0);
            size_t size = mem_sizes[mem];
            sim_assert(size > 0);
            do_dealloc(mem, size);
            mem_sizes.erase(mem);
        }
    }
};


// Use the C library's malloc
class RA_CMalloc : public RegionAlloc {
public:
    RA_CMalloc(bool zero_fill_new_mem_)
        : RegionAlloc(zero_fill_new_mem_) { }
    ~RA_CMalloc() { base_cleanup(); }

    void *do_alloc(size_t size) { 
        void *result = malloc(size);
        if (result)
            base_zero_new(result, 0, size);
        return result;
    }

    void do_dealloc(void *mem, size_t size) {
        free(mem);
    }

    bool have_resize() const { return true; }

    void *do_resize(void *mem, size_t old_size, size_t new_size) {
        void *result = realloc(mem, new_size);
        if (result && (new_size > old_size))
            base_zero_new(result, old_size, new_size);
        return result;
    }
};


// Use the C++ library's new/delete
class RA_CPPNew : public RegionAlloc {
public:
    RA_CPPNew(bool zero_fill_new_mem_)
        : RegionAlloc(zero_fill_new_mem_) { }
    ~RA_CPPNew() { base_cleanup(); }

    void *do_alloc(size_t size) { 
        void *result;
        try {
            result = static_cast<void *>(new char[size]);
        } catch(...) {
            result = 0;
        }
        if (result)
            base_zero_new(result, 0, size);
        return result;
    }

    void do_dealloc(void *mem, size_t size) {
        delete[] static_cast<char *>(mem);
    }
};


// Use POSIX mmap, and a single map per region
class RA_MmapSingle : public RegionAlloc {
public:
    RA_MmapSingle(bool zero_fill_new_mem_)
        : RegionAlloc(zero_fill_new_mem_) { }
    ~RA_MmapSingle() { base_cleanup(); }

    void *do_alloc(size_t size) { 
        // No need for base_zero_new(): anonymous mmap zero-fills new pages
        return mmap_alloc(0, size);
    }

    void do_dealloc(void *mem, size_t size) {
        mmap_free(mem, size);
    }
};


// Use mmap with non-standard mremap extension, and a single map per region
class RA_MremapSingle : public RA_MmapSingle {
public:
    RA_MremapSingle(bool zero_fill_new_mem_)
        : RA_MmapSingle(zero_fill_new_mem_) { }
    ~RA_MremapSingle() { base_cleanup(); }

    bool have_resize() const { return true; }

    void *do_resize(void *mem, size_t old_size, size_t new_size) {
        size_t r_old_size = roundup_pagesize(old_size);
        size_t r_new_size = roundup_pagesize(new_size);
        void *result = mem;
        if (r_old_size != r_new_size) {
            result = mmap_resize(mem, r_old_size, r_new_size);
        }
        if (result && (new_size > old_size)) {
            // zero out newly-allocated memory up to the next page boundary
            // (newly-allocated pages will be zero-filled, but there may be
            // old data within the last allocated page, from a previous
            // shrink.)
            size_t zero_end_size = r_old_size;
            if (zero_end_size > new_size)
                zero_end_size = new_size;
            base_zero_new(result, old_size, zero_end_size);
        }
        return result;
    }
};


// Use POSIX mmap, and multiple maps per region to avoid memcpy-ing.
class RA_MmapMulti : public RegionAlloc {
    struct SubRegion {
        void *base;
        size_t size;
        SubRegion(void *base_, size_t size_) : base(base_), size(size_) { }
    };

    typedef vector<SubRegion> SRVec;
    typedef map<void *, SRVec> SRVecMap;

    SRVecMap sub_regions;
    int grow_count;
    int grow_copies;
    int shrink_count;
    int shrink_copies;

    static size_t subregion_totalsize(const SRVec& srv) {
        size_t result = 0;
        if (!srv.empty()) {
            result = void_bdiff(srv.back().base, srv.front().base) +
                srv.back().size;
        }
        return result;
    }

    void destroy_subregions(SRVec& srv) {
        sim_assert(!srv.empty());
        void *mem = srv.front().base;
        sim_assert(sub_regions.count(mem) != 0);
        SRVec::const_iterator iter = srv.begin(), end = srv.end();
        for (; iter != end; ++iter)
            mmap_free(iter->base, iter->size);
        srv.clear();
        sub_regions.erase(mem); 
    }

    void subregion_addfirst(void *mem, size_t size) {
        sim_assert(sub_regions.count(mem) == 0);
        sub_regions[mem].push_back(SubRegion(mem, size));
    }

    static void subregion_free_last(SRVec& srv) {
        sim_assert(srv.size() > 1);
        mmap_free(srv.back().base, srv.back().size);
        srv.pop_back();
    }

    // Append a new sub-region at the end of the current set, if possible
    static bool subregion_append(SRVec& srv, size_t size) {
        sim_assert(size > 0);
        size_t r_size = roundup_pagesize(size);
        sim_assert(!srv.empty());
        const SubRegion& last_sr = srv.back();
        void *region_addr = last_sr.base;
        region_addr = void_boffset(region_addr, last_sr.size);
        void *new_mem = mmap_alloc(region_addr, r_size);
        if (new_mem)
            srv.push_back(SubRegion(new_mem, r_size));
        return (new_mem != 0);
    }

    // Do the work we were trying to avoid: mmap an entire new piece of memory
    // for the region, then copy all of the old sub-regions into it.
    void *subregion_recopy_all(SRVec& srv, size_t old_size, size_t new_size) {
        size_t r_new_size = roundup_pagesize(new_size);
        void *new_mem = mmap_alloc(0, r_new_size);
        if (new_mem) {
            void *old_mem = srv.front().base;
            size_t copy_size = RA_MIN(old_size, new_size);
            memcpy(new_mem, old_mem, copy_size);
            destroy_subregions(srv);    // invalidates srv
            subregion_addfirst(new_mem, r_new_size);
        }
        return new_mem;
    }

    void print_copy_stats() const {
        MDEBUG(1)("MDEBUG: RA_MmapMulti: copied on %i/%i grows, "
                  "%i/%i shrinks\n",
                  grow_copies, grow_count, shrink_copies, shrink_count);
    }

public:
    RA_MmapMulti(bool zero_fill_new_mem_) 
        : RegionAlloc(zero_fill_new_mem_),
          grow_count(0), grow_copies(0), shrink_count(0), shrink_copies(0) {
    }
    ~RA_MmapMulti() { base_cleanup(); }

    void *do_alloc(size_t size) { 
        size_t r_size = roundup_pagesize(size);
        void *result = mmap_alloc(0, r_size);
        if (result)
            subregion_addfirst(result, r_size);
        return result;
    }

    void do_dealloc(void *mem, size_t size) {
        sim_assert(sub_regions.count(mem) != 0);
        SRVec& srv = sub_regions[mem];
        destroy_subregions(srv);
    }

    bool have_resize() const { return true; }

    void *do_resize(void *mem, size_t old_size, size_t new_size) {
        sim_assert(sub_regions.count(mem) != 0);
        SRVec& srv = sub_regions[mem];
        sim_assert(!srv.empty());
        void *result = mem;
        if (new_size < old_size) {
            shrink_count++;
            size_t r_new_size = roundup_pagesize(new_size);
            void *new_end = void_boffset(mem, r_new_size);
            while (srv.back().base >= new_end)
                // Free any unneeded sub-regions
                subregion_free_last(srv);
            sim_assert(!srv.empty());
            // Shrink the remaining region(s), if mostly unused
            SubRegion& final_region = srv.back();
            size_t total_size = void_bdiff(final_region.base, mem) + 
                final_region.size;
            if (r_new_size < (total_size / 2)) {
                shrink_copies++;
                result = subregion_recopy_all(srv, old_size, new_size);
            }
        } else if (new_size > old_size) {
            grow_count++;
            // Allocate a new sub-region, if needed.
            size_t already_avail = subregion_totalsize(srv);
            bool recopied_all = false;
            if (new_size > already_avail) {
                if (!subregion_append(srv, new_size - already_avail)) {
                    grow_copies++;
                    result = subregion_recopy_all(srv, old_size, new_size);
                    // recopy-all creates all new mmaps, which are all
                    // zero-filled
                    recopied_all = true;
                }
            }

            if (result && !recopied_all) {
                // zero out new-to-caller memory, up to the next allocation
                // boundary
                size_t zero_end_size = already_avail;
                if (zero_end_size > new_size)
                    zero_end_size = new_size;
                base_zero_new(result, old_size, zero_end_size);
            }
        }
        print_copy_stats();
        return result;
    }
};



//
// C wrappers for methods
//

RegionAlloc *
ralloc_create(int zero_fill_new_mem)
{
    RegionAlloc *result;

    if (!PageSize) {
        PageSize = getpagesize();
        if (PageSize <= 0) {
            abort_printf("RegionAlloc: couldn't get page size!\n");
        }
    }

    if (!ALLOW_MMAP) {
        // Fall back to ANSI C malloc() and friends
        result = new RA_CMalloc(zero_fill_new_mem);
    } else if (HAVE_MREMAP && ALLOW_MREMAP) {
        // If mremap() is available, use the allocator built on that, since 
        // it's the best of the bunch
        result = new RA_MremapSingle(zero_fill_new_mem);
    } else if (ALLOW_MULTI_MMAP) {
        // If there's no mremap(), then prefer the spiffy new multi-mmap
        // allocator, which avoids copying.  It's more complex 
        // (and therefore bug-prone) than the single-mmap allocator.
        result = new RA_MmapMulti(zero_fill_new_mem);
    } else {
        result = new RA_MmapSingle(zero_fill_new_mem);
    }

    return result;
}

void 
ralloc_destroy(RegionAlloc *ra)
{
    if (ra)
        delete ra;
}

void *
ralloc_alloc(RegionAlloc *ra, size_t size)
{
    return ra->alloc(size);
}

void *
ralloc_resize(RegionAlloc *ra, void *mem, size_t new_size)
{
    return ra->resize(mem, new_size);
}

void 
ralloc_dealloc(RegionAlloc *ra, void *mem)
{
    ra->dealloc(mem);
}


//
// Error-catching wrappers for the wrappers (whee!)
//

void *
ralloc_alloc_e(RegionAlloc *ra, size_t size)
{
    void *result = ralloc_alloc(ra, size);
    if (!result) {
        exit_printf("ralloc_alloc_e: allocation of %lu bytes failed\n",
                    (unsigned long) size);
    }
    return result;
}

void *
ralloc_resize_e(RegionAlloc *ra, void *mem, size_t new_size)
{
    void *result = ralloc_resize(ra, mem, new_size);
    if (!result) {
        exit_printf("ralloc_resize_e: resize of %p to %lu bytes failed\n",
                    mem, (unsigned long) new_size);
    }
    return result;
}
