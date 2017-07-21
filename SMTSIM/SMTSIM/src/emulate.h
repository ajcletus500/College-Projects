//
// Instruction emulation
//
// This module is responsible for "emulating" instructions.  By this, we mean
// that we cause the architected effects of instruction execution, without the
// microarchitectural (and timing-based) details.
//
// Jeff Brown
// $Id: emulate.h,v 1.2.12.2.6.2.2.1 2009/05/04 06:41:43 jbrown Exp $
//

#ifndef EMULATE_H
#define EMULATE_H

// Defined elsewhere
struct AppState;
struct StashData;


#ifdef __cplusplus
extern "C" {
#endif


// Interesting state calculated during emulation of an instruction, 
// aside from the register output.
typedef struct EmuInstState EmuInstState;
struct EmuInstState {
    int taken_branch;           // MUST be 0 or 1, always valid
    mem_addr br_target;         // Only valid for branches
    mem_addr srcmem, destmem;   // Only valid for memory insts
};


// This emulates a single, already-decoded instruction at "as->pc".
// The "emu_state" structure is (always) used to store some interesting things 
// about the emulated instruction, which may be useful in later simulation.
//
// It's weird to have the instructions decoded by the caller, but this
// lets parts of the simulator sneak a peek at the next instruction without 
// having to emulate it.  (Yes, that's a cop-out.)
void emulate_inst(struct AppState * restrict as,
                  const struct StashData * restrict st,
                  EmuInstState * restrict emu_state, int speculative);

// Decode the instruction from AppState "as" at location "pc", storing
// decoded info at "st".
//
// These doesn't necessarily belong here, but it's convenient to have the
// decoder in this module since it can directly see all of the emulate 
// routines it chooses from.
//
// Returns <0 on decode failure; >=0 on success (success values are currently
// not detailed; we're leaving this open for later).
//
// Failure reasons:
//   -1: unable to read instruction bytes (unmapped, alignment, permission)
//   -2: invalid instruction (bytes available, but didn't make sense)
int decode_inst(struct AppState * restrict as,
                struct StashData * restrict st,
                mem_addr pc);

// Emulate the next "inst_count" instructions in "as".
void fast_forward_app(struct AppState * restrict as, i64 inst_count);

// Calculate the destination memory address of the next instruction of "as",
// with the decode info at "st".  This wart is here so that we can checkpoint
// and undo stores.
mem_addr emu_calc_destmem(const struct AppState * restrict as,
                          const struct StashData * restrict st);

#ifdef __cplusplus
}
#endif

#endif  // EMULATE_H
