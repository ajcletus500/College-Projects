//
// mem.h: macros to emulate the old-style memory access routines
//
// Jeff Brown
// $Id: mem.h,v 1.6.10.3.6.1 2008/04/30 22:17:53 jbrown Exp $
//

#ifndef MEM_H
#define MEM_H

#include "prog-mem.h"

#define read_mem_64(ctx, addr) pmem_read_64(ctx->as->pmem, addr, 0)
#define read_mem_32(ctx, addr) pmem_read_32(ctx->as->pmem, addr, 0)
#define set_mem_64(ctx, addr, val) pmem_write_64(ctx->as->pmem, addr, val, 0)
#define set_mem_32(ctx, addr, val) pmem_write_32(ctx->as->pmem, addr, val, 0)

#define safe_read_mem_64(ctx, addr) \
        pmem_read_64(ctx->as->pmem, addr, PMAF_NoExcept)
#define safe_read_mem_32(ctx, addr) \
        pmem_read_32(ctx->as->pmem, addr, PMAF_NoExcept)

#endif          // MEM_H
