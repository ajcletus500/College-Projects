//
// ELF Loader: read a statically-linked alpha/ELF binary into memory
//
// Jeff Brown
// $Id: loader-elf.cc,v 1.1.2.1.2.1 2008/04/30 22:17:52 jbrown Exp $
//

const char RCSid_1158705970[] =
"$Id: loader-elf.cc,v 1.1.2.1.2.1 2008/04/30 22:17:52 jbrown Exp $";

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <map>
#include <vector>

#include "sim-assert.h"
#include "sys-types.h"
#include "loader.h"
#include "loader-private.h"
#include "app-state.h"
#include "utils.h"
#include "mem.h"
#include "prog-mem.h"
#include "sim-params.h"

#define WITH_LIBELF 1
#ifdef WITH_LIBELF

#include <libelf.h>
//#include <sys/types.h>
#include <sys/fcntl.h>          // For open(2) and related definitions

namespace {

// Threshold for detection of resizeable data segment in executable header:
// consider segments which are at least this much larger in memory then
// on disk.  (The assumption being that the contained "bss" data will be
// at least this large, but other padding won't.)
const int kDataSegDetectZeroFillBytes = 16;

struct ResidentSeg {
    mem_addr start_va;          // Starting virtual address in memory
    i64 mem_bytes;              // How many bytes to occupy
    i64 file_offset;            // Starting byte in file
    i64 file_bytes;             // Bytes to read from file (zero-fill excess)

    unsigned access_flags;      // ProgMem "PMAF" access flags for creation
    unsigned create_flags;      // ProgMem "PMCF" creation-specific flags

    ResidentSeg(mem_addr start_va_, i64 mem_bytes_, i64 file_offset_,
                i64 file_bytes_, unsigned access_flags_, 
                unsigned create_flags_)
        : start_va(start_va_), mem_bytes(mem_bytes_),
          file_offset(file_offset_), file_bytes(file_bytes_),
          access_flags(access_flags_), create_flags(create_flags_)
    {
    }
};

typedef std::vector<ResidentSeg> ResidentSegVec;


struct HeaderInfoSummary {
    ResidentSegVec res_segs;
    mem_addr entry_point;
    mem_addr data_seg_for_brk;          // base of segment used by brk(2), or 0
    HeaderInfoSummary()
        : entry_point(0), data_seg_for_brk(0) { }
};


// returns <0 on error
int
add_res_seg(HeaderInfoSummary *summ, const Elf64_Phdr *phdr)
{
    unsigned access_flags = 0;
    if (phdr->p_flags & PF_R)
        access_flags |= PMAF_R;
    if (phdr->p_flags & PF_W)
        access_flags |= PMAF_W;
    if (phdr->p_flags & PF_X)
        access_flags |= PMAF_X;

    summ->res_segs.push_back(
        ResidentSeg(phdr->p_vaddr, phdr->p_memsz,
                    phdr->p_offset, phdr->p_filesz, access_flags, PMCF_None));
    return 0;
}


bool
segment_vaddrs_overlap(const HeaderInfoSummary *summ)
{
    typedef std::map<mem_addr,i64> OverlapMap;
    OverlapMap base_to_len;    // Holds checked, non-overlapping segment addrs
    for (ResidentSegVec::const_iterator iter = summ->res_segs.begin();
         iter != summ->res_segs.end(); ++iter) {
        const ResidentSeg& next_seg = *iter;
        OverlapMap::const_iterator ovl_prev_iter, ovl_next_iter;
        ovl_next_iter = base_to_len.upper_bound(next_seg.start_va);
        if (ovl_next_iter != base_to_len.end()) {
            // ovl_next_iter is the lowest memory range that starts after the
            // beginning of next_seg.  Test to make sure it starts after the
            // *end* of next_seg, as well.
            if ((next_seg.start_va + next_seg.mem_bytes) >
                ovl_next_iter->first) {
                // next_seg overlaps the start of ovl_next_iter
                return true;
            }
        }
        if (ovl_next_iter != base_to_len.begin()) {
            ovl_prev_iter = ovl_next_iter;
            --ovl_prev_iter;
            // ovl_prev_iter is the highest memory range that starts
            // at an address <= the start of next_seg.  Make sure that
            // it *ends* before the start of next_seg.
            if ((ovl_prev_iter->first + ovl_prev_iter->second) >
                next_seg.start_va) {
                // ovl_prev_iter overlaps the start of next_seg
                return true;
            }
        }
        base_to_len.insert(std::make_pair(next_seg.start_va,
                                          next_seg.mem_bytes));
    }
    return false;
}


// Prints and returns NULL on error
HeaderInfoSummary *
parse_headers(const char *filename, Elf *elf_obj)
{
    const char *fname = "loader-elf.cc::parse_headers";
    HeaderInfoSummary *summ = new HeaderInfoSummary();
    const Elf64_Ehdr *elf_hdr = NULL;
    const Elf64_Phdr *prog_hdr_tab = NULL;
    std::vector<const ResidentSeg *> candidate_data_segs;

    {
        Elf_Kind kind = elf_kind(elf_obj);
        if (kind != ELF_K_ELF) {
            fprintf(stderr, "%s: elf_kind() returned kind #%lu, expected "
                    "ELF_K_ELF\n", fname, (unsigned long) kind);
            goto err;
        }
    }

    if ((elf_hdr = elf64_getehdr(elf_obj)) == NULL) {
        fprintf(stderr, "%s: elf64_getehdr() failed\n", fname);
        goto err;
    }

    // Validate ELF header fields that seem relevant to proper simulator
    // operation
    {
        unsigned long ei_class = elf_hdr->e_ident[EI_CLASS];
        unsigned long ei_data = elf_hdr->e_ident[EI_DATA];
        //unsigned long ei_osabi = elf_hdr->e_ident[EI_OSABI];
        if (ei_class != ELFCLASS64) {
            fprintf(stderr, "%s: ELF file has ei_class #%lu, "
                    "expected a 64-bit file (ELFCLASS64)\n",
                    fname, (unsigned long) ei_class);
            goto err;
        }
        if (ei_data != ELFDATA2LSB) {
            fprintf(stderr, "%s: ELF file has ei_data #%lu, "
                    "expected a two's complement LSB (ELFDATA2LSB)\n",
                    fname, (unsigned long) ei_data);
            goto err;
        }
        //DEBUGPRINTF("%s: ei_osabi #%lu\n", fname, (unsigned long) ei_osabi);
    }
    if (elf_hdr->e_type != ET_EXEC) {
        fprintf(stderr, "%s: ELF file has e_type #%ld, "
                "expected an executable file (ET_EXEC)\n",
                fname, (long) elf_hdr->e_type);
        goto err;
    }
    if (elf_hdr->e_machine != EM_ALPHA) {
        fprintf(stderr, "%s: ELF file has e_machine #%ld, "
                "expected an Alpha executable (EM_ALPHA)\n",
                fname, (long) elf_hdr->e_machine);
        goto err;
    }

    // Other ELF header fields (seem to correspond to readelf(1) output)
    // e_version
    // e_phoff
    // e_shoff
    // e_flags
    // e_ehsize
    // e_phentsize
    // e_phnum
    // e_shentsize
    // e_shnum
    // e_shstrndx

    if ((prog_hdr_tab = elf64_getphdr(elf_obj)) == NULL) {
        fprintf(stderr, "%s: elf64_getphdr() failed\n", fname);
        goto err;
    }

    // Iterate over program headers, looking for sections to load into memory.
    // First, we'll build the summary table, and then check it later for
    // overlaps.  (A corrupt enough ELF header here could have us reading
    // past the bounds of the table; yay.)
    for (i64 prog_hdr_number = 0; 
         prog_hdr_number < (i64) elf_hdr->e_phnum; prog_hdr_number++) {
        const Elf64_Phdr *prog_hdr = &prog_hdr_tab[prog_hdr_number];
        switch (prog_hdr->p_type) {
        case PT_LOAD:
            add_res_seg(summ, prog_hdr);
            break;
        case PT_DYNAMIC:
            fprintf(stderr, "%s: \"DYNAMIC\" program header present; "
                    "dynamic linking is not supported.", fname);
            goto err;
        case PT_INTERP:
            fprintf(stderr, "%s: \"INTERP\" program header present; "
                    "external interpreters aren't supported.", fname);
            goto err;
        case PT_NOTE:   // ignore
            break;
        case PT_SHLIB:
            fprintf(stderr, "%s: \"SHLIB\" program header present; "
                    "shared libraries aren't supported.", fname);
            goto err;
        case PT_PHDR:
            add_res_seg(summ, prog_hdr);
            break;
        case PT_TLS:    // not sure what this is; ignore?
            break;
        default:
            // There seem to be many other values used here which aren't
            // defined in all libelf.h's (such as GNU_STACK, GNU_EH_FRAME,...);
            // We'll just ignore those and hope for the best.
            DEBUGPRINTF("%s: ELF program header #%s, type #%s unrecognized; "
                        "ignoring.\n", fname, fmt_i64(prog_hdr_number), 
                        fmt_u64(prog_hdr->p_type));
            break;
        }

    }

    summ->entry_point = elf_hdr->e_entry;

    // Try to figure out which of the segments we've loaded ought to be
    // resized by the brk syscall, or 0 if we can't tell.
    for (ResidentSegVec::const_iterator iter = summ->res_segs.begin();
         iter != summ->res_segs.end(); ++iter) {
        const ResidentSeg *seg = &(*iter);
        // Guess: consider read+write but non-executable segments which are
        // larger in memory than in the file, as possible data segments.
        // (This is kind of a hack to avoid having to go digging through the
        // section header and string tables to look up ".bss"; if we can get
        // what we need just from the program headers, that's better... and
        // arguably that's what the program headers are _for_.)
        if ((seg->access_flags & PMAF_R) &&
            (seg->access_flags & PMAF_W) &&
            !(seg->access_flags & PMAF_X) &&
            ((seg->mem_bytes - seg->file_bytes)
             >= kDataSegDetectZeroFillBytes)) {
            candidate_data_segs.push_back(seg);
        }
    }
    // Hopefully, there's only one candidate.  If not, print a warning and
    // guess the highest-addressed segment.
    if (candidate_data_segs.empty()) {
        fprintf(stderr, "%s: warning: no data segment found in file \"%s\"\n",
                fname, filename);
    } else if (candidate_data_segs.size() == 1) {
        summ->data_seg_for_brk = candidate_data_segs[0]->start_va;
    } else {
        int cand_count = int(candidate_data_segs.size());
        fprintf(stderr, "%s: warning: multiple (%d) data segments found in "
                "file \"%s\", start_va:", fname, cand_count, filename);
        mem_addr max_va = candidate_data_segs[0]->start_va;
        for (int i = 0; i < cand_count; ++i) {
            fprintf(stderr, " %s", fmt_mem(candidate_data_segs[i]->start_va));
            if (candidate_data_segs[i]->start_va > max_va) {
                max_va = candidate_data_segs[i]->start_va;
            }
        }
        fprintf(stderr, "; using max (%s)\n", fmt_mem(max_va));
        summ->data_seg_for_brk = max_va;
    }

    return summ;

err:
    fprintf(stderr, "%s: failed to parse file \"%s\"\n", fname, filename);
    delete summ;
    return NULL;
}


int
create_res_seg(AppState *dest, FILE *exec_file, const ResidentSeg& seg)
{
    const char *fname = "loader-elf.cc::create_res_seg";
    i64 file_readsize = MIN_SCALAR(seg.mem_bytes, seg.file_bytes);
    DEBUGPRINTF("%s: creating segment, %s @ %s, access 0x%x create 0x%x\n",
                fname, fmt_i64(seg.mem_bytes),
                fmt_mem(seg.start_va), seg.access_flags,
                seg.create_flags);
    if (pmem_map_new(dest->pmem, seg.mem_bytes, seg.start_va,
                     PMAF_RW, seg.create_flags)) {
        fprintf(stderr, "%s: couldn't create segment (%s @ %s)\n", fname,
                fmt_i64(seg.mem_bytes), fmt_mem(seg.start_va));
        goto err;
    }
    if (fseek(exec_file, (long) seg.file_offset, SEEK_SET) == -1) {
        fprintf(stderr, "%s: couldn't fseek in exec file: %s\n",
                fname, strerror(errno));
        goto err;
    }
    {
        long offs_test = ftell(exec_file);
        if (offs_test == -1) {
            fprintf(stderr, "%s: couldn't ftell from exec file: %s\n",
                    fname, strerror(errno));
            goto err;
        }
        if (offs_test != seg.file_offset) {
            fprintf(stderr, "%s: fseek/ftell offset mismatch; "
                    "64-bit badness?\n", fname);
            goto err;
        }
    }
    if (pmem_write_fromfile(dest->pmem, seg.start_va, exec_file, 
                            file_readsize, PMAF_None) 
        != (size_t) file_readsize) {
        fprintf(stderr, "%s: short read/copy on segment %s @ %s\n",
                fname, fmt_i64(seg.mem_bytes), fmt_mem(seg.start_va));
        goto err;
    }
    if (seg.mem_bytes > file_readsize) {
        mem_addr zero_start = seg.start_va + file_readsize;
        i64 zero_bytes = seg.mem_bytes - file_readsize;
        pmem_write_memset(dest->pmem, zero_start, 0, zero_bytes, 0);
    }
    pmem_chmod(dest->pmem, seg.start_va, seg.access_flags);

    return 0;

err:
    return -1;
}


int 
loader_read_elf(AppState *dest, const char *filename) 
{
    const char *fname = "loader_read_elf";
    int sys_fd = -1;    // libelf requires unix-style fd, not FILE *
    FILE *exec_file = NULL;     // buffered C FILE for convenient I/O
    Elf *elf_obj = NULL;
    const HeaderInfoSummary *hdr_summ = NULL;

    if ((sys_fd = open(filename, O_RDONLY)) == -1) {
        fprintf(stderr, "%s: can't open app binary file \"%s\": %s\n",
                fname, filename, strerror(errno));
        goto err;
    }
    if ((exec_file = fopen(filename, "rb")) == NULL) {
        fprintf(stderr, "%s: can't fopen app binary file \"%s\": %s\n",
                fname, filename, strerror(errno));
        goto err;
    }
    
    if (elf_version(EV_CURRENT) == EV_NONE) {
        fprintf(stderr, "%s: ELF library, elf_version() failed: "
                "ELF library out of date?\n", fname);
        goto err;
    }

    if ((elf_obj = elf_begin(sys_fd, ELF_C_READ, NULL)) == NULL) {
        fprintf(stderr, "%s: ELF library, elf_begin() failed on \"%s\"\n",
                fname, filename);
        goto err;

    }

    if ((hdr_summ = parse_headers(filename, elf_obj)) == NULL) {
        goto err;
    }

    if (segment_vaddrs_overlap(hdr_summ)) {
        fprintf(stderr, "%s: ELF program segment virtual addresses overlap; "
                "not sure how to handle that.\n", fname);
        goto err;
    }

    for (ResidentSegVec::const_iterator iter = hdr_summ->res_segs.begin();
         iter != hdr_summ->res_segs.end(); ++iter) {
        const ResidentSeg& next_seg = *iter;
        if (create_res_seg(dest, exec_file, next_seg) < 0)
            goto err;
    }

    dest->seg_info.entry_point = hdr_summ->entry_point;

    // This is something of a misnomer, since we don't actually care whether
    // this is the start address of ".bss" (or even if there is such a
    // segment); what we're really after here is that the segment containing
    // bss_start is the one which is resized by the brk syscall.  Note that
    // this address might be zero if no such segments are detected, which
    // will cause "brk" to freak out if called.
    dest->seg_info.bss_start = hdr_summ->data_seg_for_brk;

    // The ELF headers don't seem to contain a value for GP (r29), but the
    // code at ELF entry points I've examined reset r29 right away so maybe
    // it's not needed?
    dest->seg_info.gp_value = 0;

    delete hdr_summ;
    while (elf_end(elf_obj) > 0) ;
    fclose(exec_file);
    close(sys_fd);
    return 0;

err:
    delete hdr_summ;
    if (elf_obj) {
        while (elf_end(elf_obj) > 0) ;
    }
    if (exec_file)
        fclose(exec_file);
    if (sys_fd >= 0)
        close(sys_fd);

//    for (int i = 0; (i < newseg_cnt) && new_segs[i]; i++)
//      pmem_unmap(dest->pmem, new_segs[i]);
    return -1;
}


class ElfExecLoader : public AppExecFileLoader {
    std::string name() const { return std::string("elf"); }

    bool file_type_match(const char *filename) const {
        const char *fname = "ElfExecLoader::file_type_match";
        char elf_magic_buff[SELFMAG];
        bool got_header;
        FILE *fd = fopen(filename, "rb");
        if (fd == NULL) {
            fprintf(stderr, "%s: can't open app binary file \"%s\": %s\n",
                    fname, filename, strerror(errno));
            goto err;
        }
        got_header = (fread(elf_magic_buff, sizeof(elf_magic_buff),
                                 1, fd) == 1);
        fclose(fd);
        if (!got_header ||
            (memcmp(elf_magic_buff, ELFMAG, SELFMAG) != 0)) {
            return false;
        }
        return true;
    err:
        if (fd)
            fclose(fd);
        return false;
    }

    int load_file(AppState *dest, const char *filename) const {
        return loader_read_elf(dest, filename);
    }
};

}


AppExecFileLoader *
new_exec_loader_elf(void)
{
    return new ElfExecLoader();
}


#else // WITH_LIBELF


AppExecFileLoader *
new_exec_loader_elf(void)
{
    return NULL;
}


#endif // WITH_LIBELF
