//
// "Stash" -- caching wrapper around instruction decoder.  This manages
// information per-STATIC instruction only; see notes with StashData struct
// definition, below.
//
// Jeff Brown
// $Id: stash.h,v 1.10.6.6.6.1.2.3 2009/12/18 08:38:56 jbrown Exp $
//


/* SMTSIM simulator.
   
   Copyright (C) 1994-1999 by Dean Tullsen (tullsen@cs.ucsd.edu)
   ALL RIGHTS RESERVED.

   SMTSIM is distributed under the following conditions:

     You may make copies of SMTSIM for your own use and modify those copies.

     All copies of SMTSIM must retain all copyright notices contained within.

     You may not sell SMTSIM or distribute SMTSIM in conjunction with a
     commerical product or service without the express written consent of
     Dean Tullsen.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.

Significant parts of the SMTSIM simulator were written by Jack Lo.
Therefore the following copyrights may also apply:

Copyright (C) Jack Lo
 */


#ifndef STASH_H
#define STASH_H


// Defined elsewhere
struct AppState;
struct EmuInstState;


#ifdef __cplusplus
extern "C" {
#endif

typedef struct Stash Stash;


typedef enum {
    FP=0, SYNCH, INTEGER, INTLDST, FUTYPES
} StashFUType;

typedef enum {
    SMF_NoMemOp = 0,
    SMF_WidthMask = 0xff,
    SMF_Read = 0x100,
    SMF_Write = 0x200, 
    SMF_AlignToQuad = 0x400,
    SMF_SmtLockRel = 0x800
} StashMemFlags;

#define SMF_Width(x) (x)
#define SMF_GetWidth(x) ((x) & SMF_WidthMask)


typedef enum {
    // SBF_Br_*, SBF_RS_* are each mutually-exclusive sets
    SBF_NotABranch = 0,
    SBF_Br_Uncond = 0x1,
    SBF_Br_Cond = 0x2,
    SBF_StaticTargDisp = 0x4,
    SBF_RS_Push = 0x8,
    SBF_RS_Pop = 0x10,
    SBF_RS_PopPush = 0x20
} StashBranchFlags;

#define SBF_CondBranch(x) ((x) & SBF_Br_Cond)
#define SBF_UncondBranch(x) ((x) & SBF_Br_Uncond)
#define SBF_StaticTarget(x) ((x) & SBF_StaticTargDisp)
#define SBF_IndirectBranch(x) ((x) && !SBF_StaticTarget(x))
#define SBF_UsesRetStack(x) \
        ((x) & (SBF_RS_Push | SBF_RS_Pop | SBF_RS_PopPush))
#define SBF_ReadsRetStack(x) \
        ((x) & (SBF_RS_Pop | SBF_RS_PopPush))
#define SBF_WritesRetStack(x) \
        ((x) & (SBF_RS_Push | SBF_RS_PopPush))


// General-purpose unrelated "other" flags
typedef enum {
    SGF_None = 0,
    SGF_SmtPrimitive = 0x1,
    SGF_PipeExclusive = 0x2,
    SGF_SysCall = 0x4,
    SGF_DetermVarDelay = 0x8,   // Delay not const, but determined by input
    SGF_CondWrite = 0x10,       // Conditional write: may not write dest reg
    SGF_StaticNoop = 0x20,      // Static no-op instruction: always a noop
    SGF_SyncAtCommit = 0x40,    // Sync instruction, updates at commit
    SGF_FPLiteralWrite = 0x80   // Writes literal bit-string to FP reg
} StashGenFlags;


typedef enum StashDelayClass {
    SDC_all_extra,              // Additonal latency added to all insts

    // Integer latency classes
    SDC_int_arith,
    SDC_int_load, SDC_int_store,
    SDC_int_compare, SDC_int_condmove,
    SDC_int_condbr,
    SDC_int_mull, SDC_int_mulq, SDC_int_umulh,

    // FP latency classes
    SDC_fp_arith, SDC_fp_bit,
    SDC_fp_load, SDC_fp_store,
    SDC_fp_compare, SDC_fp_condmove,
    SDC_fp_condbr,
    SDC_fp_divs, SDC_fp_divt,

    // Misc. latencies
    SDC_uncond_br,
    SDC_ftoi, SDC_itof,
    SDC_smt_lockrel,
    SDC_smt_fork,
    SDC_smt_terminate,

    StashDelayClass_count               // Must be last!
} StashDelayClass;


// Sketchy-looking macros which contain the argument declaration and 
// argument-use strings for each emulate function.  (There are many of them,
// and this is an easy, if a bit confusing, way to maintain them.)
#define EMULATE_ARGS_DECL struct AppState * restrict as, \
                          const struct StashData * restrict st, \
                          struct EmuInstState * restrict emu_state, \
                          int speculative
#define EMULATE_ARGS_USE as, st, emu_state, speculative


//
// The StashData structure contains *static* information as decoded from a
// particular machine instruction.  The values here should depend only on
// the raw instruction bytes, not the instruction's PC, when it was decoded,
// machine state when emulated, or anything else.  This should be written to
// only when decoding an instruction.
//

typedef struct StashData {
    int src_a;          // Source register, is "Ra"/"Fa", iff possible
    int src_b;          // Source register, is "Rb"/"Fb", iff possible
    int dest;           // Dest. register number
    int immed_byte;     // -1 => no immediate
   
    int regaccs;       // Counts inst register accesses (for stats)
                       // Can be 1/2/3
     
    u32 inst;           // Raw instruction bytes

    StashBranchFlags br_flags;          // Nonzero iff a branch
    StashGenFlags gen_flags;
    StashMemFlags mem_flags;            // Nonzero iff a memory operation

    void (*emulate)(EMULATE_ARGS_DECL);

    StashFUType whichfu;
    StashDelayClass delay_class;
    u32 syncop;
} StashData;


StashData *stashdata_copy(const StashData *sdata);
void stashdata_destroy(StashData *sdata);


// (It'd be conceptually nicer to make stash_create() take a function pointer
// to the instruction decoder, instead of directly calling decode_inst(), but
// that wouldn't gain us anything at this point.)
Stash *stash_create(struct AppState *as);
void stash_destroy(Stash *stash);

void stash_reset(Stash *stash);


// Look up a given application instruction in the stash, returning a pointer
// to a StashData struct describing it.  If the instruction isn't found,
// allocates a new entry and invokes the decoder to fill it in, and then
// returns a pointer to that.
//
// The returned pointer is "owned" by the Stash, and may be invalidated by ANY
// FUTURE CALL to this module, so copy out any data you want to keep.
// stashdata_copy() and stashdata_destroy() can help you there.
//
// (Currently never returns NULL since the simulator aborts on failed decode;
// at some point in the future, if decode is allowed to fail, this would then
// return NULL.)
const StashData *stash_decode_inst(Stash *stash, mem_addr pc);

// Non-modifying test if an instruction is present (already decoded)
int stash_probe_inst(const Stash *stash, mem_addr pc);

// Invalidate some cached decode data
void stash_flush_inst(Stash *stash, mem_addr pc);


#ifdef __cplusplus
}
#endif

#endif

