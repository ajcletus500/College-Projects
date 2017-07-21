//
// AOUT Loader: read a statically-linked alpha/AOUT binary into memory.
// This is the "traditional" SMTSIM style loader, and used to be the only 
// loader.
//
// Jeff Brown
// $Id: loader-aout.cc,v 1.1.2.2.2.2 2008/04/30 22:17:51 jbrown Exp $
//

const char RCSid_1158738627[] =
"$Id: loader-aout.cc,v 1.1.2.2.2.2 2008/04/30 22:17:51 jbrown Exp $";

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <string>
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

// This has the Alpha file headers and such defined
#include "loader-alphafilehdr.h"


namespace {

// Returns: 0 for EOF, -1 for partial read or I/O error, 1 for word read
int
read_le_word(FILE *in, int width, u64& val_ret)
{
    u64 result = 0;
    int bytes_read;
    for (bytes_read = 0; bytes_read < width; bytes_read++) {
        const int next_byte = getc(in);
        if (next_byte == EOF)
            goto read_failed;
        result |= static_cast<u64>(next_byte) << (8 * bytes_read);
    }
    val_ret = result;
    return 1;
read_failed:
    return (bytes_read || ferror(in)) ? -1 : 0;
}


// Returns 1 on success, 0 otherwise
static int
load_alpha_execinfo(FILE *in, struct alpha_execinfo *ei)
{
    u64 val;
    memset(ei, 0, sizeof(*ei));

    // (Hey, look, inner functions)
    #define READ_n(width) if (read_le_word(in, width, val) != 1) goto r_fail
    #define READ_2(targ) do { READ_n(2); (targ) = val; } while (0)
    #define READ_4(targ) do { READ_n(4); (targ) = val; } while (0)
    #define READ_8(targ) do { READ_n(8); (targ) = val; } while (0)

    // start on "fh", 24 bytes @ 0
    READ_2(ei->fh.alpha_f_magic);
    READ_2(ei->fh.alpha_f_nscns);
    READ_4(ei->fh.alpha_f_timdat);
    READ_8(ei->fh.alpha_f_symptr);
    READ_4(ei->fh.alpha_f_nsyms);
    READ_2(ei->fh.alpha_f_opthdr);
    READ_2(ei->fh.alpha_f_flags);

    // "ah", 80 bytes @ 24
    READ_2(ei->ah.alpha_magic);
    READ_2(ei->ah.alpha_vstamp);
    READ_2(ei->ah.alpha_bldrev);
    READ_2(ei->ah.alpha_padcell);
    READ_8(ei->ah.alpha_tsize);
    READ_8(ei->ah.alpha_dsize);
    READ_8(ei->ah.alpha_bsize);
    READ_8(ei->ah.alpha_entry);
    READ_8(ei->ah.alpha_text_start);
    READ_8(ei->ah.alpha_data_start);
    READ_8(ei->ah.alpha_bss_start);
    READ_4(ei->ah.alpha_gprmask);
    READ_4(ei->ah.alpha_fprmask);
    READ_8(ei->ah.alpha_gp_value);

    #undef READ_n
    #undef READ_2
    #undef READ_4
    #undef READ_8

    return 1;
r_fail:
    return 0;
}


int 
loader_read_aout(AppState *dest, const char *filename) 
{
    struct alpha_execinfo ei;
    FILE *fd = NULL;
    i64 text_size, data_size, bss_size;
    mem_addr text_start, data_start, bss_start;
    mem_addr new_segs[10];
    int newseg_cnt = 0;

    memset(new_segs, 0, sizeof(new_segs));

    if (!(fd = (FILE *) fopen(filename, "rb"))) {
        fprintf(stderr, "Can't open app binary file \"%s\": %s\n",
                filename, strerror(errno));
        goto err;
    }
    if (load_alpha_execinfo(fd, &ei) != 1) {
        fprintf(stderr, "Short read on a.out header\n");
        goto err;
    }
    if ((ei.fh.alpha_f_magic != Alpha_ALPHAMAGIC) &&
        (ei.fh.alpha_f_magic != Alpha_ALPHAUMAGIC)) {
        fprintf(stderr, "Bad magic number in a.out file header; "
                "%s instead of %s or %s\n", 
                fmt_x64(ei.fh.alpha_f_magic), fmt_x64(Alpha_ALPHAMAGIC),
                fmt_x64(Alpha_ALPHAUMAGIC));

        goto err;
    }

    text_size = ei.ah.alpha_tsize;
    text_start = ei.ah.alpha_text_start;
    data_size = ei.ah.alpha_dsize;
    data_start = ei.ah.alpha_data_start;
    bss_size = ei.ah.alpha_bsize;
    bss_start = ei.ah.alpha_bss_start;

    text_size = ceil_mult_i64(text_start + text_size,
                              GlobalParams.mem.page_bytes) - text_start;
    data_size = ceil_mult_i64(data_start + data_size,
                              GlobalParams.mem.page_bytes) - data_start;
    bss_size = ceil_mult_i64(bss_start + bss_size,
                             GlobalParams.mem.page_bytes) - bss_start;

    DEBUGPRINTF("text_start: 0x%s    text_size: 0x%s\n"
                "data_start: 0x%s    data_size: 0x%s\n"
                "bss_start: 0x%s    bss_size: 0x%s\n"
                "entry point: 0x%s\n", 
                fmt_x64(text_start), fmt_x64(text_size),
                fmt_x64(data_start), fmt_x64(data_size),
                fmt_x64(ei.ah.alpha_bss_start), fmt_x64(ei.ah.alpha_bsize),
                fmt_x64(ei.ah.alpha_entry));

    if (pmem_map_new(dest->pmem, text_size, text_start, PMAF_RW,
                     PMCF_None)) {
        fprintf(stderr, "Couldn't create text segment (%s @ 0x%s)\n",
                fmt_i64(text_size), fmt_x64(text_start));
        goto err;
    }
    new_segs[newseg_cnt++] = text_start;

    if ((data_start + data_size) == bss_start) {
        // bss immediately follows data segment; create them together so that
        // syscalls, e.g. read(2), can span them (some benchmarks do this)
        if (pmem_map_new(dest->pmem, data_size + bss_size, data_start, PMAF_RW,
                         PMCF_None)) {
            fprintf(stderr, "Couldn't create data+bss segment (%s @ 0x%s)\n",
                    fmt_i64(data_size + bss_size), fmt_x64(data_start));
            goto err;
        }
        new_segs[newseg_cnt++] = data_start;
    } else {
        if (pmem_map_new(dest->pmem, data_size, data_start, PMAF_RW,
                         PMCF_None)) {
            fprintf(stderr, "Couldn't create data segment (%s @ 0x%s)\n",
                    fmt_i64(data_size), fmt_x64(data_start));
            goto err;
        }
        new_segs[newseg_cnt++] = data_start;
        if (pmem_map_new(dest->pmem, bss_size, bss_start, PMAF_RW,
                         PMCF_None)) {
            fprintf(stderr, "Couldn't create bss segment (%s @ 0x%s)\n",
                    fmt_i64(bss_size), fmt_x64(bss_start));
            goto err;
        }
        new_segs[newseg_cnt++] = bss_start;
    }

    fseek(fd, 0, SEEK_SET);
    if (pmem_write_fromfile(dest->pmem, text_start, fd, text_size, 
                            PMAF_None) != (size_t) text_size) {
        fprintf(stderr, "Short read/copy on text segment\n");
        goto err;
    }
    pmem_chmod(dest->pmem, text_start, PMAF_RX);

    if (pmem_write_fromfile(dest->pmem, data_start, fd, data_size, 
                            PMAF_None) != (size_t) data_size) {
        fprintf(stderr, "Short read/copy on data segment\n");
        goto err;
    }

    pmem_write_memset(dest->pmem, bss_start, 0, bss_size, 0);

    dest->seg_info.entry_point = ei.ah.alpha_entry;
    dest->seg_info.gp_value = ei.ah.alpha_gp_value;
    dest->seg_info.bss_start = bss_start;

    fclose(fd);
    return 0;

err:
    if (fd)
        fclose(fd);
    for (int i = 0; (i < newseg_cnt) && new_segs[i]; i++)
        pmem_unmap(dest->pmem, new_segs[i]);
    return -1;
}


class AoutExecLoader : public AppExecFileLoader {
    std::string name() const { return std::string("aout"); }

    bool file_type_match(const char *filename) const {
        const char *fname = "AoutExecLoader::file_type_match";
        FILE *fd = fopen(filename, "rb");
        struct alpha_execinfo ei;
        bool got_header;
        if (fd == NULL) {
            fprintf(stderr, "%s: can't open app binary file \"%s\": %s\n",
                    fname, filename, strerror(errno));
            goto err;
        }
        got_header = (load_alpha_execinfo(fd, &ei) == 1);
        fclose(fd);
        if (!got_header ||
            ((ei.fh.alpha_f_magic != Alpha_ALPHAMAGIC) &&
             (ei.fh.alpha_f_magic != Alpha_ALPHAUMAGIC))) {
            return false;
        }
        return true;
    err:
        if (fd)
            fclose(fd);
        return false;
    }

    int load_file(AppState *dest, const char *filename) const {
        return loader_read_aout(dest, filename);
    }
};


}


AppExecFileLoader *
new_exec_loader_aout(void)
{
    return new AoutExecLoader();
}
