/*
 * Program memory state manager -- this isn't trying to simulate an MMU, only 
 * to abstract away the details of managing simulated address spaces.
 *
 * Jeff Brown
 * $Id: prog-mem.h,v 1.1.2.5.2.1.2.2 2009/12/21 05:44:38 jbrown Exp $
 */

#ifndef PROG_MEM_H
#define PROG_MEM_H

#ifdef __cplusplus
extern "C" {
#endif

struct RegionAlloc;

typedef struct ProgMem ProgMem;
typedef struct ProgMemSegment ProgMemSegment;
typedef struct ProgMemSegmentInfo ProgMemSegmentInfo;

// Zero return <=> error "caught"
typedef int (*pmem_errfunc_p)(ProgMem *pmem, mem_addr va, int width, 
                              unsigned flags, unsigned err_code,
                              void *err_data);

struct ProgMemSegmentInfo {
    i64 size;
    i64 max_size;
    int is_private;
    int ref_count;
};

// Access flags
enum {
    PMAF_None = 0,
    PMAF_R = 0x1,
    PMAF_W = 0x2,
    PMAF_X = 0x4,
    PMAF_NoExcept = 0x8,

    PMAF_RW = PMAF_R | PMAF_W,
    PMAF_RX = PMAF_R | PMAF_X,
    PMAF_RWX = PMAF_R | PMAF_W | PMAF_X
};

// Creation flags
enum {
    PMCF_None = 0,
    PMCF_AutoGrowDown = 0x1     // see AUTO_GROW_THRESH notes in prog-mem.cc
};

// Error codes
enum {
    PMEC_None = 0,              // No error
    PMEC_NoAddr,                // Address not mapped
    PMEC_Width,                 // Access crosses segment boundary
    PMEC_Prot,                  // Protection violation
    PMEC_AutoGrowMem,           // Auto-grow out of simulator memory
    PMEC_AutoGrowAddrs,         // Auto-grow out of address space
    PMEC_AutoGrowLimit,         // Auto-grow exceeds segment size limit
    PMEC_last
};


// ("name" will be copied)
ProgMem *pmem_create(const char *name, struct RegionAlloc *ra,
                     pmem_errfunc_p err_handler, void *err_data);
void pmem_destroy(ProgMem *pmem);

int pmem_map_new(ProgMem *pmem, i64 size, mem_addr base_va, 
                 unsigned access_flags, unsigned create_flags);

int pmem_map_seg(ProgMem *pmem, ProgMemSegment *seg, mem_addr base_va,
                 unsigned access_flags,
                 unsigned create_flags);

void pmem_unmap(ProgMem *pmem, mem_addr base_va);

// For a given VA, return the base VA of the containing segment (or 0)
mem_addr pmem_get_base(const ProgMem *pmem, mem_addr va);
// Return the address of the lowest segment which starts > va, or 0
mem_addr pmem_get_nextbase(const ProgMem *pmem, mem_addr va);

int pmem_get_flags(const ProgMem *pmem, mem_addr base_va, 
                   unsigned *access_flags, unsigned *create_flags);
void pmem_chmod(ProgMem *pmem, mem_addr base_va, unsigned new_access_flags);

// Return the segment beginning at base_va, or NULL if no such seg.
ProgMemSegment *pmem_get_seg(ProgMem *pmem, mem_addr base_va);

int pmem_access_ok(const ProgMem *pmem, mem_addr va, int bytes,
                   unsigned access_flags);
unsigned pmem_read_8(ProgMem *pmem, mem_addr va, unsigned flags);
unsigned pmem_read_16(ProgMem *pmem, mem_addr va, unsigned flags);
u32 pmem_read_32(ProgMem *pmem, mem_addr va, unsigned flags);
u64 pmem_read_64(ProgMem *pmem, mem_addr va, unsigned flags);
u64 pmem_read_n(ProgMem *pmem, int bytes, mem_addr va, unsigned flags);
void pmem_read_memcpy(ProgMem *pmem, void *dest, mem_addr src_va,
                      size_t len, unsigned flags);
int pmem_read_memcmp(ProgMem *pmem, const void *mem1, mem_addr mem2_va,
                     size_t len, unsigned flags);
size_t pmem_read_tofile(ProgMem *pmem, void *FILE_dest, mem_addr src_va,
                        size_t len, unsigned flags);

void pmem_write_8(ProgMem *pmem, mem_addr va, unsigned value, 
                  unsigned flags);
void pmem_write_16(ProgMem *pmem, mem_addr va, unsigned value,
                   unsigned flags);
void pmem_write_32(ProgMem *pmem, mem_addr va, u32 value, unsigned flags);
void pmem_write_64(ProgMem *pmem, mem_addr va, u64 value, unsigned flags);
void pmem_write_n(ProgMem *pmem, int bytes, mem_addr va, u64 value,
                  unsigned flags);
void pmem_write_memcpy(ProgMem *pmem, mem_addr dest_va, const void *src,
                       size_t len, unsigned flags);
void pmem_write_memset(ProgMem *pmem, mem_addr dest_va, int ch,
                       size_t len, unsigned flags);
size_t pmem_write_fromfile(ProgMem *pmem, mem_addr dest_va, void *FILE_src,
                           size_t len, unsigned flags);

const char *pmem_errcode_msg(const ProgMem *pmem, unsigned err_code);
void pmem_dump_map(const ProgMem *pmem, void *FILE_dst, const char *prefix);


// This is crufty: it may go away some day, so you shouldn't use it.
// Use the pmem_read_* / pmem_write_* routines to do what you want.
//
// This translates a given address into a pointer to that simulated memory
// byte.  (It's dangerous, since there's no guarantee that all contiguous
// simulated addresses are also contiguous in the simulator's address space.)
void *pmem_xlate_hack(ProgMem *pmem, mem_addr va, int width, 
                      unsigned flags);


i64 pms_size(const ProgMemSegment *seg);
void pms_query(const ProgMemSegment *seg, ProgMemSegmentInfo *info_ret);

// Default max_size is I64_MAX.  New max_size must not be smaller than
// current segment size.
i64 pms_get_maxsize(const ProgMemSegment *seg);
void pms_set_maxsize(ProgMemSegment *set, i64 max_size);

// Resize a segment, from the upper end.  Returns nonzero on failure.
int pms_resize(ProgMemSegment *seg, i64 new_size);


#ifdef __cplusplus
}
#endif

#endif  /* PROG_MEM_H */
