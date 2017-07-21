/*
 * Program memory state manager -- this isn't trying to simulate an MMU, only 
 * to abstract away the details of managing simulated address spaces.
 *
 * Jeff Brown
 * $Id: prog-mem.cc,v 1.1.2.7.2.1.2.7 2009/12/21 06:05:56 jbrown Exp $
 */

const char RCSid_1062110501[] =
"$Id: prog-mem.cc,v 1.1.2.7.2.1.2.7 2009/12/21 06:05:56 jbrown Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <limits>
#include <map>
#include <set>
#include <string>

#include "sys-types.h"
#include "prog-mem.h"
#include "sim-assert.h"
#include "region-alloc.h"
#include "utils.h"
#include "utils-cc.h"


using std::string;


// WARNING: byte_width must be a power of 2
#define VA_ALIGN(va, byte_width)        ((va) & ~((byte_width) - 1))
// Test: is "va" aligned to a "byte_width" boundary?
#define VA_ALIGNED(va, byte_width)      (!((va) & ((byte_width) - 1)))


// Plan for PMEM_DEBUG_LEVEL:
// 0: off
// 1: top-level segment create/destroy/resize operations; no accesses
// 2: include block accesses (e.g. memset)
#define PMEM_DEBUG_LEVEL        0
#ifdef DEBUG
    // Condition: "should I print a level-x debug message?"
    #define PMEM_DEBUG_COND(x) ((PMEM_DEBUG_LEVEL >= (x)) && debug)
#else
    // Never print
    #define PMEM_DEBUG_COND(x) (0)
#endif  // DEBUG
#define PMDEBUG(x) if (!PMEM_DEBUG_COND(x)) { } else printf


namespace {

// Lowest virtual address to AutoGrowDown to.  (0 is ok.)
const mem_addr kMinGrowDownVA = 0x1000;

// Must be power of 2.  AutoGrowDown base addresses will be rounded down to
// this granularity.  This isn't a tight guarantee; it may be violated e.g.
// to prevent growing one segment so much that it overlaps another, or
// to honor kMinGrowDownVA.
const unsigned kGrowAlignBytes = 8192;


// Hack-y check that should probably get integrated into sys-types: does
// the given value overflow a size_t?
bool
sizet_overflow(i64 value)
{
    bool result = ((value < std::numeric_limits<size_t>::min()) ||
                   (value > std::numeric_limits<size_t>::max()));
    return result;
}


}       // Anonymous namespace close


static const char *PMEC_Msgs[] = {
    "No error",                         // None
    "Address not mapped",               // NoAddr
    "Access crosses segment boundary",  // Width
    "Protection violation",             // Prot
    "Auto-grow out of simulator memory", // AutoGrowMem
    "Auto-grow out of address space",   // AutoGrowAddrs
    "Auto-grow exceeds segment size limit", // AutoGrowLimit
    NULL
};


struct ProgMemSegment {
private:
    // Right now, this doesn't track its "holders" in a set, but it may need
    // to later.

    RegionAlloc *region_alloc_; // Not owned
    int ref_count_;
    i64 size_;
    i64 max_size_;
    bool is_private_;           // Flag: may not be shared
    unsigned char *base_ptr_;   // Owned, allocated via region_alloc_

    NoDefaultCopy nocopy;

public:
    ProgMemSegment(RegionAlloc *ra__, i64 size__, bool is_private__);
    ~ProgMemSegment();

    // false <=> segment is private with nonzero ref count
    bool add_ref() {
        if (is_private_ && (ref_count_ > 0)) {
            sim_assert(ref_count_ == 1);
            return false;
        }
        ++ref_count_;
        return true;
    }

    // true <=> ref count zero, caller must delete it
    bool del_ref() {
        sim_assert(ref_count_ > 0);
        --ref_count_;
        return (ref_count_ == 0);
    }

    i64 g_size() const { return size_; }
    unsigned char *g_baseptr() const { return base_ptr_; }
    void query(ProgMemSegmentInfo& ret) const;
    i64 get_maxsize(void) const { return max_size_; }
    void set_maxsize(i64 new_max_size);
    int resize(i64 new_size, bool at_start);
};


ProgMemSegment::ProgMemSegment(RegionAlloc *ra__, i64 size__,
                               bool is_private__) 
    : region_alloc_(ra__), ref_count_(0), size_(size__), max_size_(I64_MAX),
      is_private_(is_private__), base_ptr_(NULL)
{
    sim_assert(size_ > 0);
    base_ptr_ = static_cast<unsigned char *>
        (ralloc_alloc_e(region_alloc_, size_));
}


ProgMemSegment::~ProgMemSegment() {
    if (base_ptr_)
        ralloc_dealloc(region_alloc_, base_ptr_);
}


void
ProgMemSegment::query(ProgMemSegmentInfo& ret) const {
    ret.size = size_;
    ret.max_size = max_size_;
    ret.is_private = is_private_;
    ret.ref_count = ref_count_;
}


void
ProgMemSegment::set_maxsize(i64 new_max_size)
{
    sim_assert(size_ <= max_size_);
    if (new_max_size < size_) {
        abort_printf("ProgMemSegment::set_maxsize: attempting to set "
                     "new max_size %s, less than current size %s\n",
                     fmt_i64(new_max_size), fmt_i64(size_));
    }
    max_size_ = new_max_size;
}


int 
ProgMemSegment::resize(i64 new_size, bool at_start)
{
    i64 old_size = size_;
    i64 size_delta = new_size - old_size;

    if (at_start && (size_delta < 0)) {
        // We don't currently support shrinking from the start of a segment,
        // since the simulator doesn't use it.  We can implement this later
        // if need be.
        abort_printf("unsupported segment shrink-at-start attempted\n");
    }

    sim_assert(old_size <= max_size_);
    if ((new_size <= 0) || (new_size > max_size_) || sizet_overflow(new_size))
        return -1;

    void *new_mem = ralloc_resize(region_alloc_, base_ptr_, new_size);
    if (!new_mem)
        return -1;
    base_ptr_ = static_cast<unsigned char *>(new_mem);
    size_ = new_size;

    if (at_start && (size_delta > 0)) {
        // This is very inefficient, try not to do it often
        memmove(base_ptr_ + size_delta, base_ptr_, old_size);
        // Zero the new bytes that we just vacated
        memset(base_ptr_, 0, size_delta);
    }

    return 0;
}


struct ProgMem {
private:
    struct SegTarget {
        ProgMemSegment *seg;    // Manually reference-counted
        unsigned access_flags;
        unsigned create_flags;
        // WARNING: copy/assignment operators in use

        SegTarget() : seg(NULL) { }
        SegTarget(ProgMemSegment *seg_, unsigned access_flags_, 
                  unsigned create_flags_) 
            : seg(seg_), access_flags(access_flags_), 
              create_flags(create_flags_)
        { }
    };

    // Using map instead of hash_map, since we do range queries on this.  As
    // used now, this tends to have very few elements (stack, data, text) so
    // the structure doesn't matter much; if we start mapping very many
    // segments, we may have to be smarter -- perhaps maintaining a
    // page-aligned hash of recent translations?
    typedef std::map<mem_addr, SegTarget> SegMap;

    string pmem_name_;          // for debugging etc.
    RegionAlloc *ra_;
    pmem_errfunc_p err_handler_;
    void *err_data_;
    SegMap seg_map_;

    NoDefaultCopy nocopy;

    unsigned char *xlate(mem_addr va, int width, unsigned flags);
    unsigned char *xlate_probe(mem_addr va, int width, unsigned flags) const;

    bool access_allowed(unsigned seg_access_flags,
                        unsigned proposed_access_flags) const {
        return (proposed_access_flags & seg_access_flags) == 
            (proposed_access_flags & PMAF_RWX);
    }

public:
    ProgMem(const string& name__, RegionAlloc *ra__,
            pmem_errfunc_p err_handler__, void *err_data__);
    ~ProgMem();

    int map_new(i64 size, mem_addr base_va, 
                unsigned access_flags, unsigned create_flags);
    int map_seg(ProgMemSegment *seg, mem_addr base_va,
                unsigned access_flags,
                unsigned create_flags);
    void unmap(mem_addr base_va);
    mem_addr get_base(mem_addr va) const;
    mem_addr get_nextbase(mem_addr va) const;
    int get_flags(mem_addr base_va, unsigned *access_flags, 
                  unsigned *create_flags) const;
    void chmod(mem_addr base_va, unsigned new_access_flags);
    ProgMemSegment *get_seg(mem_addr base_va);

    // Check whether a given access will succeed; no auto-grow.
    bool access_ok(mem_addr va, int bytes, unsigned access_flags) const {
        return xlate_probe(va, bytes, access_flags) != NULL;
    }

    unsigned read_8(mem_addr va, unsigned flags) {
        flags |= PMAF_R;
        const unsigned char *mem = xlate(va, 1, flags);
        if (!mem) return 0;
        return mem[0];
    }

    unsigned read_16(mem_addr va, unsigned flags) {
        flags |= PMAF_R;
        va = VA_ALIGN(va, 2);
        const unsigned char *mem = xlate(va, 2, flags);
        if (!mem) return 0;
        return mem[0] | (mem[1] << 8);
    }

    u32 read_32(mem_addr va, unsigned flags) {
        flags |= PMAF_R;
        va = VA_ALIGN(va, 4);
        const unsigned char *mem = xlate(va, 4, flags);
        if (!mem) return 0;
        return mem[0] | (mem[1] << 8) | (static_cast<u32>(mem[2]) << 16) |
            (static_cast<u32>(mem[3]) << 24);
    }

    u64 read_64(mem_addr va, unsigned flags) {
        flags |= PMAF_R;
        va = VA_ALIGN(va, 8);
        const unsigned char *mem = xlate(va, 8, flags);
        if (!mem) return 0;
        return mem[0] | (mem[1] << 8) | (static_cast<u64>(mem[2]) << 16) |
            (static_cast<u64>(mem[3]) << 24) |
            (static_cast<u64>(mem[4]) << 32) |
            (static_cast<u64>(mem[5]) << 40) |
            (static_cast<u64>(mem[6]) << 48) |
            (static_cast<u64>(mem[7]) << 56);
    }

    u64 read_n(int bytes, mem_addr va, unsigned flags) {
        sim_assert((bytes == 8) || (bytes == 4) || (bytes == 2) ||
                   (bytes == 1));
        flags |= PMAF_R;
        va = VA_ALIGN(va, bytes);
        const unsigned char *mem = xlate(va, bytes, flags);
        if (!mem) return 0;
        u64 result = 0;
        for (int i = (bytes - 1); i >= 0; i--) 
            result = (result << 8) | mem[i];
        return result;
    }

    void read_memcpy(void *dest, mem_addr src_va,
                     size_t len, unsigned flags);
    int read_memcmp(const void *mem1, mem_addr mem2_va,
                    size_t len, unsigned flags);
    size_t read_tofile(void *FILE_dest, mem_addr src_va,
                       size_t len, unsigned flags);

    void write_8(mem_addr va, unsigned value, unsigned flags) {
        flags |= PMAF_W;
        unsigned char *mem = xlate(va, 1, flags);
        if (!mem) return;
        mem[0] = value & 0xff;
    }

    void write_16(mem_addr va, unsigned value, unsigned flags) {
        flags |= PMAF_W;
        va = VA_ALIGN(va, 2);
        unsigned char *mem = xlate(va, 2, flags);
        if (!mem) return;
        mem[0] = value & 0xff;
        mem[1] = (value >> 8) & 0xff;
    }

    void write_32(mem_addr va, u32 value, unsigned flags) {
        flags |= PMAF_W;
        va = VA_ALIGN(va, 4);
        unsigned char *mem = xlate(va, 4, flags);
        if (!mem) return;
        mem[0] = value & 0xff;
        mem[1] = (value >> 8) & 0xff;
        mem[2] = (value >> 16) & 0xff;
        mem[3] = (value >> 24) & 0xff;
    }

    void write_64(mem_addr va, u64 value, unsigned flags) {
        flags |= PMAF_W;
        va = VA_ALIGN(va, 8);
        unsigned char *mem = xlate(va, 8, flags);
        if (!mem) return;
        mem[0] = value & 0xff;
        mem[1] = (value >> 8) & 0xff;
        mem[2] = (value >> 16) & 0xff;
        mem[3] = (value >> 24) & 0xff;
        mem[4] = (value >> 32) & 0xff;
        mem[5] = (value >> 40) & 0xff;
        mem[6] = (value >> 48) & 0xff;
        mem[7] = (value >> 56) & 0xff;
    }

    void write_n(int bytes, mem_addr va, u64 value, unsigned flags) {
        sim_assert((bytes == 8) || (bytes == 4) || (bytes == 2) ||
                   (bytes == 1));
        flags |= PMAF_W;
        va = VA_ALIGN(va, bytes);
        unsigned char *mem = xlate(va, bytes, flags);
        if (!mem) return;
        for (int i = 0; i < bytes; i++) {
            mem[i] = value & 0xff;
            value >>= 8;
        }
    }

    void write_memcpy(mem_addr dest_va, const void *src,
                      size_t len, unsigned flags);
    void write_memset(mem_addr dest_va, int ch,
                      size_t len, unsigned flags);

    size_t write_fromfile(mem_addr dest_va, void *FILE_src,
                          size_t len, unsigned flags);
    void dump_map(void *FILE_dst, const char *prefix) const;

    void *xlate_hack(mem_addr va, int width, unsigned flags) {
        return xlate(va, width, flags);
    }
};


ProgMem::ProgMem(const string& name__, RegionAlloc *ra__,
                 pmem_errfunc_p err_handler__, void *err_data__)
    : pmem_name_(name__), ra_(ra__), err_handler_(err_handler__),
      err_data_(err_data__)
{
}


ProgMem::~ProgMem()
{
    {
        FOR_ITER(SegMap, seg_map_, seg_iter) {
            SegTarget& targ = seg_iter->second;
            if (targ.seg->del_ref())
                delete targ.seg;
            targ.seg = NULL;
        }
        seg_map_.clear();
    }
}


int
ProgMem::map_new(i64 size, mem_addr base_va, 
                 unsigned access_flags, unsigned create_flags) 
{
    // Sharing auto-grow-down segments would be a major pain (all holders
    // would have to have their maps updated, which brings up conflicts etc.).
    // It's easier to just make them private, since we don't currently have
    // a use for sharing them.
    bool is_private = (create_flags & PMCF_AutoGrowDown);
    PMDEBUG(1)("pmem_map_new: pmem %s size %s base_va %s access_flags 0x%x "
               "create_flags 0x%x: ", pmem_name_.c_str(), fmt_i64(size),
               fmt_x64(base_va), access_flags, create_flags);
    if ((size <= 0) || (sizet_overflow(size)))
        return -1;
    ProgMemSegment *seg = new ProgMemSegment(ra_, size, is_private);
    PMDEBUG(1)("(seg at %s) \n", fmt_x64(u64_from_ptr(seg)));
    // map_seg will print the rest of the debug info for this request.
    int stat = map_seg(seg, base_va, access_flags, create_flags);
    if (stat)
        delete seg;
    return stat;
}


int 
ProgMem::map_seg(ProgMemSegment *seg, mem_addr base_va, unsigned access_flags,
                 unsigned create_flags)
{
    PMDEBUG(1)("pmem_map_seg: pmem %s seg %s base_va %s access_flags 0x%x "
               "create_flags 0x%x: ", pmem_name_.c_str(),
               fmt_x64(u64_from_ptr(seg)), 
               fmt_x64(base_va), access_flags, create_flags);
    if (seg_map_.find(base_va) != seg_map_.end()) {
        // Something already mapped at that address
        PMDEBUG(1)("failed; something already mapped at that base address\n");
        return -1;
    }
    {
        mem_addr next_seg_start = get_nextbase(base_va);
        mem_addr limit_va = base_va + seg->g_size();
        if ((get_base(base_va) != 0) || 
            (next_seg_start && (next_seg_start < limit_va))) {
            // Proposed region overlaps existing mapping
            PMDEBUG(1)("failed; proposed region already contains "
                       "segment(s)\n");
            return -1;
        }
    }
    if (!seg->add_ref()) {
        // Couldn't add reference
        PMDEBUG(1)("failed; couldn't add reference (private segment)\n");
        return -1;
    }
    seg_map_[base_va] = SegTarget(seg, access_flags, create_flags);
    PMDEBUG(1)("ok\n");
    return 0;
}


void 
ProgMem::unmap(mem_addr base_va)
{
    const char *fname = "pmem_unmap";
    PMDEBUG(1)("%s: pmem %s base_va %s: ", fname, pmem_name_.c_str(),
               fmt_x64(base_va));
    SegMap::iterator found = seg_map_.find(base_va);
    if (found == seg_map_.end()) {
        abort_printf("%s: pmem %s base_va %s: no such segment\n", fname,
                     pmem_name_.c_str(), fmt_x64(base_va));
    }
    {
        SegTarget& targ = found->second;
        if (targ.seg->del_ref()) {
            PMDEBUG(1)("(final reference) ");
            delete targ.seg;
        }
    }
    PMDEBUG(1)("ok.\n");
    seg_map_.erase(found);
}


mem_addr
ProgMem::get_base(mem_addr va) const
{
    SegMap::const_iterator targ_iter = seg_map_.upper_bound(va);
    if (targ_iter != seg_map_.begin()) {
        --targ_iter;
        mem_addr base_va = targ_iter->first;
        if (va < (base_va + targ_iter->second.seg->g_size())) 
            return base_va;
    }   
    return 0;
}


mem_addr
ProgMem::get_nextbase(mem_addr va) const
{
    SegMap::const_iterator targ_iter = seg_map_.upper_bound(va);
    if (targ_iter != seg_map_.end()) {
        mem_addr base_va = targ_iter->first;
        return base_va;
    }   
    return 0;
}


int 
ProgMem::get_flags(mem_addr base_va, unsigned *access_flags, 
                   unsigned *create_flags) const
{
    SegMap::const_iterator found = seg_map_.find(base_va);
    if (found == seg_map_.end())
        return -1;
    if (access_flags) 
        *access_flags = found->second.access_flags;
    if (create_flags) 
        *create_flags = found->second.create_flags;
    return 0;
}


void 
ProgMem::chmod(mem_addr base_va, unsigned new_access_flags)
{
    const char *fname = "pmem_chmod";
    PMDEBUG(1)("%s: pmem %s base_va %s new_access_flags 0x%x: ", fname,
               pmem_name_.c_str(), fmt_x64(base_va), new_access_flags);
    SegMap::iterator found = seg_map_.find(base_va);
    if (found == seg_map_.end()) {
        abort_printf("%s: pmem %s base_va %s: no such segment\n", fname,
                     pmem_name_.c_str(), fmt_x64(base_va));
    }
    PMDEBUG(1)("ok.\n");
    found->second.access_flags = new_access_flags;
}


ProgMemSegment *
ProgMem::get_seg(mem_addr base_va)
{
    SegMap::iterator found = seg_map_.find(base_va);
    if (found == seg_map_.end()) {
        // No such segment
        return NULL;
    }
    return found->second.seg;
}


unsigned char *
ProgMem::xlate(mem_addr va, int width, unsigned flags) 
{
    const char *fname = "ProgMem::xlate";
    unsigned char *result = NULL;

    SegTarget *targ = NULL;
    mem_addr base_va = 0, limit_va = 0;

    //
    // If you change how this works, make sure to update xlate_probe() too!
    //

    SegMap::iterator above_iter = seg_map_.upper_bound(va);
    SegMap::iterator targ_iter = above_iter;

    if (targ_iter != seg_map_.begin()) {
        --targ_iter;
        targ = &targ_iter->second;
        base_va = targ_iter->first;
        limit_va = base_va + targ->seg->g_size();
        if (va >= limit_va)
            targ = NULL;
    }

    unsigned err_code = 0;

    if (!targ && (above_iter != seg_map_.end())) {
        // Check next segment up for grow-down conditions.
        //
        // Suppress growth when NoExcept flag is set: assume it's a wrong-path
        // access, which wouldn't lead to stack growth on a real OS (since
        // growth occurs in page fault handling).  This is to keep us from
        // accidentally growing the stack huge by mistake.
        SegTarget *above_targ = &above_iter->second;
        if ((va != 0) && (above_targ->create_flags & PMCF_AutoGrowDown) &&
            !(above_targ->access_flags & PMAF_NoExcept)) {
            base_va = above_iter->first;
            limit_va = base_va + above_targ->seg->g_size();
            sim_assert(base_va > va);
            const i64 old_size = above_targ->seg->g_size();

            mem_addr new_base_va = base_va;
            {
                i64 new_size = old_size;
                while (new_base_va > va) {
                    // While we could just use
                    // new_base_va=VA_ALIGN(va,kGrowAlignBytes), currently our
                    // grow-down implementation performs a memmove() of the
                    // entire segment.  So, we'll grow exponentially to help
                    // amortize the cost of those moves.
                    new_size *= 2;
                    new_base_va = limit_va - new_size;
                    if ((new_size <= 0) || (new_base_va >= limit_va)) {
                        // Overflow; stop doubling
                        new_base_va = va;
                    }
                }
            }

            // (Alignment may be disrupted by other constraints, below.)
            new_base_va = VA_ALIGN(new_base_va, kGrowAlignBytes);

            // Now new_base_va is low enough to satisfy the access.
            // Next, see if we have to move it up, to satisfy other
            // constraints.

            {
                // Ensure we don't grow "targ" down into the segment below it.
                mem_addr prev_seg_limit_va =
                    (targ_iter == seg_map_.begin()) ? 0 :
                    targ_iter->first + targ_iter->second.seg->g_size();
                // Previous segment doesn't contain the access address; if it
                // did, we wouldn't be tring to grow the next one downward.
                sim_assert(prev_seg_limit_va <= va);
                if (new_base_va < prev_seg_limit_va)
                    new_base_va = prev_seg_limit_va;
            }

            sim_assert(new_base_va <= va);

            if (new_base_va < kMinGrowDownVA) {
                new_base_va = kMinGrowDownVA;
            }

            i64 new_size = limit_va - new_base_va;
            const i64 seg_max_size = above_targ->seg->get_maxsize();
            if (new_size > seg_max_size) {
                new_size = seg_max_size;
                new_base_va = limit_va - new_size;
                if (new_base_va > va) {
                    // We cannot succeed; pretty up request for error report
                    new_base_va = VA_ALIGN(va, kGrowAlignBytes);
                    new_size = limit_va - new_base_va;
                }
            }

            // Grow segment "down", reset base_va / limit va
            // Make sure new base >= old limit
            PMDEBUG(1)("ProgMem::xlate AutoGrowDown: pmem %s growing %s @ %s "
                       "to %s @ %s for access to va %s flags 0x%x\n",
                       pmem_name_.c_str(), fmt_i64(old_size), fmt_x64(base_va),
                       fmt_i64(new_size), fmt_x64(new_base_va),
                       fmt_x64(va), flags);

            if (new_base_va > va) {
                // Segment will still not be big enough, and we've nowhere
                // left to expand to.
                err_code = PMEC_AutoGrowAddrs;
            } else if (new_size > seg_max_size) {
                err_code = PMEC_AutoGrowLimit;
            } else if (above_targ->seg->resize(new_size, true)) {
                err_code = PMEC_AutoGrowMem;
            } else {
                // Resize succeeded; update map.
                // (copies SegTarget by value, inserts with key new_base_va)
                targ = &map_put_uniq(seg_map_, new_base_va, *above_targ);
                seg_map_.erase(above_iter);
                base_va = new_base_va;
                limit_va = base_va + targ->seg->g_size();
            }
        }       
    }

    if (targ) {
        sim_assert(!err_code);
        if ((va + width) > limit_va) {
            // Width overflows end-of-segment boundary
            err_code = PMEC_Width;
        } else if (!access_allowed(targ->access_flags, flags)) {
            // Mode not allowed
            err_code = PMEC_Prot;
        } else {
            result = targ->seg->g_baseptr() + (va - base_va);
        }
    } else if (!err_code) {
        // No target, yet no other error -> not mapped
        err_code = PMEC_NoAddr;
    }

    if (err_code) {
        if (flags & PMAF_NoExcept) {
            return NULL;
        } else if (err_handler_ &&
                   (err_handler_(this, va, width, flags, err_code, 
                                 err_data_) == 0)) {
            // Handler "caught" error, we're happy
            return NULL;
        } else {
            // No error handler callback, or error handler didn't "catch" this
            // error
            fprintf(stderr, "%s: illegal memory access, va 0x%s width %d "
                    "flags 0x%x: %s\n", fname, fmt_x64(va), width, flags,
                    pmem_errcode_msg(this, err_code));
            sim_abort();
        }
    }

    sim_assert(result != NULL);
    return result;
}


// Stripped-down version of xlate() which makes no changes and signals no
// errors; does not perform AutoGrowDown.
unsigned char *
ProgMem::xlate_probe(mem_addr va, int width, unsigned flags) const
{
    unsigned char *result = NULL;

    const SegTarget *targ = NULL;
    mem_addr base_va = 0, limit_va = 0;

    SegMap::const_iterator above_iter = seg_map_.upper_bound(va);
    SegMap::const_iterator targ_iter = above_iter;

    if (targ_iter != seg_map_.begin()) {
        --targ_iter;
        targ = &targ_iter->second;
        base_va = targ_iter->first;
        limit_va = base_va + targ->seg->g_size();
        if (va >= limit_va)
            targ = NULL;
    }

    if (targ) {
        if ((va + width) > limit_va) {
            // Width overflow
        } else if (!access_allowed(targ->access_flags, flags)) {
            // Mode not allowed
        } else {
            result = targ->seg->g_baseptr() + (va - base_va);
        }
    } else {
        // No target, yet no other error -> not mapped
    }

    return result;
}


void 
ProgMem::read_memcpy(void *dest, mem_addr src_va,
                     size_t len, unsigned flags)
{
    unsigned char *dest_mem = static_cast<unsigned char *>(dest);
    PMDEBUG(2)("pmem_read_memcpy: pmem %s dest %s src_va %s len %s "
               "flags 0x%x\n", pmem_name_.c_str(),
               fmt_x64(u64_from_ptr(dest)), fmt_x64(src_va), fmt_i64(len),
               flags);
    for (size_t i = 0; i < len; i++) {
        unsigned val = read_8(src_va + i, flags);
        dest_mem[i] = val;
    }
}


int
ProgMem::read_memcmp(const void *mem1, mem_addr mem2_va,
                     size_t len, unsigned flags)
{
    int result = 0;
    const unsigned char *mem1_mem = static_cast<const unsigned char *>(mem1);
    PMDEBUG(2)("pmem_read_memcmp: pmem %s mem1 %s mem2_va %s len %s "
               "flags 0x%x\n", pmem_name_.c_str(),
               fmt_x64(u64_from_ptr(mem1)), fmt_x64(mem2_va), fmt_i64(len),
               flags);
    for (size_t i = 0; i < len; i++) {
        unsigned mem1_val = mem1_mem[i];
        unsigned mem2_val = read_8(mem2_va + i, flags);
        if (mem1_val != mem2_val) {
            result = (mem1_val > mem2_val) ? 1 : -1;
            break;
        }
    }
    return result;
}


size_t 
ProgMem::read_tofile(void *FILE_dest, mem_addr src_va,
                     size_t len, unsigned flags)
{
    size_t result = 0;
    FILE *dest_file = static_cast<FILE *>(FILE_dest);
    const unsigned char *src_mem = xlate(src_va, len, PMAF_R | flags);
    PMDEBUG(2)("pmem_read_tofile: pmem %s FILE_dest %s src_va %s len %s "
               "flags 0x%x\n", pmem_name_.c_str(),
               fmt_x64(u64_from_ptr(FILE_dest)), fmt_x64(src_va), fmt_i64(len),
               flags);
    if (src_mem)
        result = fwrite(src_mem, 1, len, dest_file);
    return result;
}


void 
ProgMem::write_memcpy(mem_addr dest_va, const void *src,
                      size_t len, unsigned flags)
{
    const unsigned char *src_mem = static_cast<const unsigned char *>(src);
    PMDEBUG(2)("pmem_write_memcpy: pmem %s dest_va %s src %s len %s "
               "flags 0x%x\n", pmem_name_.c_str(),
               fmt_x64(dest_va), fmt_x64(u64_from_ptr(src)), fmt_i64(len),
               flags);
    for (size_t i = 0; i < len; i++) {
        unsigned val = src_mem[i];
        write_8(dest_va + i, val, flags);
    }
}


void 
ProgMem::write_memset(mem_addr dest_va, int ch,
                      size_t len, unsigned flags)
{
    int val = ch & 0xff;
    PMDEBUG(2)("pmem_write_memset: pmem %s dest_va %s ch %d len %s "
               "flags 0x%x\n", pmem_name_.c_str(),
               fmt_x64(dest_va), val, fmt_i64(len), flags);
    for (size_t i = 0; i < len; i++)
        write_8(dest_va + i, val, flags);
}


size_t 
ProgMem::write_fromfile(mem_addr dest_va, void *FILE_src,
                        size_t len, unsigned flags)
{
    size_t result = 0;
    FILE *src_file = static_cast<FILE *>(FILE_src);
    unsigned char *dest_mem = xlate(dest_va, len, PMAF_W | flags);
    PMDEBUG(2)("pmem_write_fromfile: pmem %s dest_va %s FILE_src %s len %s "
               "flags 0x%x\n", pmem_name_.c_str(),
               fmt_x64(dest_va), fmt_x64(u64_from_ptr(FILE_src)), fmt_i64(len),
               flags);
    if (dest_mem)
        result = fread(dest_mem, 1, len, src_file);
    return result;
}

void 
ProgMem::dump_map(void *FILE_dst, const char *prefix) const
{
    FILE *dst_file = static_cast<FILE *>(FILE_dst);
    FOR_CONST_ITER(SegMap, seg_map_, iter) {
        mem_addr base_va = iter->first;
        const SegTarget& targ = iter->second;
        const ProgMemSegment *seg = targ.seg;
        ProgMemSegmentInfo seg_info;
        pms_query(seg, &seg_info);
        fprintf(dst_file, "%sbase_va 0x%s: size %s private %i refs %i; "
                "create 0x%x, access 0x%x at %p\n", prefix,
                fmt_x64(base_va), fmt_i64(seg_info.size), seg_info.is_private,
                seg_info.ref_count,
                targ.create_flags, targ.access_flags,
                seg->g_baseptr());
    }
}


//
// C interface
//

ProgMem *
pmem_create(const char *name, struct RegionAlloc *ra,
            pmem_errfunc_p err_handler, void *err_data)
{
    return new ProgMem(string(name), ra, err_handler, err_data);
}

void 
pmem_destroy(ProgMem *pmem)
{
    if (pmem)
        delete pmem;
}

int
pmem_map_new(ProgMem *pmem, i64 size, mem_addr base_va, 
             unsigned access_flags, unsigned create_flags)
{
    return pmem->map_new(size, base_va, access_flags, create_flags);
}

int 
pmem_map_seg(ProgMem *pmem, ProgMemSegment *seg, mem_addr base_va,
             unsigned access_flags,
             unsigned create_flags)
{
    return pmem->map_seg(seg, base_va, access_flags, create_flags);
}

void 
pmem_unmap(ProgMem *pmem, mem_addr base_va)
{
    pmem->unmap(base_va);
}

mem_addr 
pmem_get_base(const ProgMem *pmem, mem_addr va)
{
    return pmem->get_base(va);
}

mem_addr 
pmem_get_nextbase(const ProgMem *pmem, mem_addr va)
{
    return pmem->get_nextbase(va);
}

int
pmem_get_flags(const ProgMem *pmem, mem_addr base_va, 
               unsigned *access_flags, unsigned *create_flags)
{
    return pmem->get_flags(base_va, access_flags, create_flags);
}

int
pmem_access_ok(const ProgMem *pmem, mem_addr va, int bytes, 
               unsigned access_flags)
{
    return pmem->access_ok(va, bytes, access_flags);
}

void 
pmem_chmod(ProgMem *pmem, mem_addr base_va, unsigned new_access_flags)
{
    pmem->chmod(base_va, new_access_flags);
}

ProgMemSegment *
pmem_get_seg(ProgMem *pmem, mem_addr base_va)
{
    return pmem->get_seg(base_va);
}

unsigned 
pmem_read_8(ProgMem *pmem, mem_addr va, unsigned flags)
{
    return pmem->read_8(va, flags);
}

unsigned 
pmem_read_16(ProgMem *pmem, mem_addr va, unsigned flags)
{
    return pmem->read_16(va, flags);
}

u32 
pmem_read_32(ProgMem *pmem, mem_addr va, unsigned flags)
{
    return pmem->read_32(va, flags);
}

u64 
pmem_read_64(ProgMem *pmem, mem_addr va, unsigned flags)
{
    return pmem->read_64(va, flags);
}

u64 
pmem_read_n(ProgMem *pmem, int bytes, mem_addr va, unsigned flags)
{
    return pmem->read_n(bytes, va, flags);
}

void 
pmem_read_memcpy(ProgMem *pmem, void *dest, mem_addr src_va,
                 size_t len, unsigned flags)
{
    pmem->read_memcpy(dest, src_va, len, flags);
}

int
pmem_read_memcmp(ProgMem *pmem, const void *mem1, mem_addr mem2_va,
                 size_t len, unsigned flags)
{
    return pmem->read_memcmp(mem1, mem2_va, len, flags);
}

size_t 
pmem_read_tofile(ProgMem *pmem, void *FILE_dest, mem_addr src_va,
                 size_t len, unsigned flags)
{
    return pmem->read_tofile(FILE_dest, src_va, len, flags);
}

void 
pmem_write_8(ProgMem *pmem, mem_addr va, unsigned value, unsigned flags)
{
    pmem->write_8(va, value, flags);
}

void 
pmem_write_16(ProgMem *pmem, mem_addr va, unsigned value, unsigned flags)
{
    pmem->write_16(va, value, flags);
}

void 
pmem_write_32(ProgMem *pmem, mem_addr va, u32 value, unsigned flags)
{
    pmem->write_32(va, value, flags);
}

void 
pmem_write_64(ProgMem *pmem, mem_addr va, u64 value, unsigned flags)
{
    pmem->write_64(va, value, flags);
}

void 
pmem_write_n(ProgMem *pmem, int bytes, mem_addr va, u64 value,
             unsigned flags)
{
    pmem->write_n(bytes, va, value, flags);
}

void 
pmem_write_memcpy(ProgMem *pmem, mem_addr dest_va, const void *src,
                  size_t len, unsigned flags)
{
    pmem->write_memcpy(dest_va, src, len, flags);
}

void
pmem_write_memset(ProgMem *pmem, mem_addr dest_va, int ch,
                  size_t len, unsigned flags)
{
    pmem->write_memset(dest_va, ch, len, flags);
}

size_t 
pmem_write_fromfile(ProgMem *pmem, mem_addr dest_va, void *FILE_src,
                    size_t len, unsigned flags)
{
    return pmem->write_fromfile(dest_va, FILE_src, len, flags);
}

const char *
pmem_errcode_msg(const ProgMem *pmem, unsigned err_code)
{
    return (err_code < PMEC_last) ? PMEC_Msgs[err_code] : 
        "(unrecognized code)";
}

void
pmem_dump_map(const ProgMem *pmem, void *FILE_dst, const char *prefix)
{
    pmem->dump_map(FILE_dst, prefix);
}

void *
pmem_xlate_hack(ProgMem *pmem, mem_addr va, int width, 
                 unsigned flags)
{
    return pmem->xlate_hack(va, width, flags);
}

i64
pms_size(const ProgMemSegment *seg)
{
    return seg->g_size();
}

void 
pms_query(const ProgMemSegment *seg, ProgMemSegmentInfo *info_ret)
{
    seg->query(*info_ret);
}

i64
pms_get_maxsize(const ProgMemSegment *seg)
{
    return seg->get_maxsize();
}

void
pms_set_maxsize(ProgMemSegment *seg, i64 max_size)
{
    seg->set_maxsize(max_size);
}

int 
pms_resize(ProgMemSegment *seg, i64 new_size)
{
    return seg->resize(new_size, false);
}
