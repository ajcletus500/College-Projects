/* SMTSIM simulator.
   
   Copyright (C) 1994-1999 by Dean Tullsen (tullsen@cs.ucsd.edu)
   ALL RIGHTS RESERVED.

   SMTSIM is distributed under the following conditions:

     You may make copies of SMTSIM for your own use and modify those copies.

     All copies of SMTSIM must retain all copyright notices contained within.

     You may not sell SMTSIM or distribute SMTSIM in conjunction with a
     commercial product or service without the express written consent of
     Dean Tullsen.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.

Significant parts of the SMTSIM simulator were written by Gun Sirer
(before it became the SMTSIM simulator) and by Jack Lo (after it became
the SMTSIM simulator).  Therefore the following copyrights may also apply:

Copyright (C) Jack Lo
Copyright (C) E. Gun Sirer

Pieces of this code may have been derived from Jim Larus\' SPIM simulator,
which contains the following copyright:

==============================================================
   Copyright (C) 1990-1998 by James Larus (larus@cs.wisc.edu).
   ALL RIGHTS RESERVED.

   SPIM is distributed under the following conditions:

     You may make copies of SPIM for your own use and modify those copies.

     All copies of SPIM must retain my name and copyright notice.

     You may not sell SPIM or distributed SPIM in conjunction with a
     commercial product or service without the expressed written consent of
     James Larus.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.
===============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>      // for FLT_MIN

// Please PLEASE be judicious about adding things to the #include list here;
// this module should remain de-coupled from the microarchitectural simulation.
// In particular, this module should _not_ be able to see the definitions of
// the "context" and "activelist" structures, and know nothing about timing.
#include "sim-assert.h"
#include "sys-types.h"
#include "emulate.h"
#include "utils.h"
#include "stash.h"
#include "inst.h"
#include "smt.h"
#include "fp-ctl.h"
#include "sign-extend.h"
#include "quirks.h"
#include "prog-mem.h"
#include "syscalls.h"
#include "app-state.h"
#include "work-queue.h"
#include "main.h"               // For CHECKPOINT_STORES, GlobalWorkQueue
#include "debug-coverage.h"
#include "bbtracker.h"
// NO "context.h", NO "dyn-inst.h"

#define PARCODE                 0

#define SIMULATE_MEM_BARRIERS   1

#define DEBUG_REGS_EMULATE      0       // enable detailed per-inst reporting

#define ENABLE_COVERAGE_EMULATE 0

#define WARN_ABOUT_UNSUPPORTED_TRAP_QUALIFIERS  0

//
// Source/destination value macros -- these simplify access to data values in
// the emulate routines, and confine the source-level dependence on the
// stash/context structures.  The SRC_* macros are to be used as rvalues,
// and the DEST_* macros as lvalues.  These macros all implicitly use emulate
// function variables as declared in the EMULATE_ARGS_DECL macro.
//

// Int register Ra/Rb source values (for non-operate instructions)
#define SRC_RAV (as->R[(st)->src_a].i)
#define SRC_RBV (as->R[(st)->src_b].i)
#define SRC_RAV_U (as->R[(st)->src_a].u)
#define SRC_RBV_U (as->R[(st)->src_b].u)

// Get the "Rb" source value for integer operate-format instructions, which
// may have an immediate byte instead of a register as a source.
#define SRC_RBV_OP \
    (((st)->immed_byte >= 0) ? (st)->immed_byte : as->R[(st)->src_b].i)

// Floating-point register Fa/Fb source values
#define SRC_FAV (as->R[(st)->src_a].f)
#define SRC_FBV (as->R[(st)->src_b].f)

// Floating-point source register Fa/Fb bit representations as integers
#define SRC_FAV_I (as->R[(st)->src_a].i)
#define SRC_FBV_I (as->R[(st)->src_b].i)
#define SRC_FAV_U (as->R[(st)->src_a].u)
#define SRC_FBV_U (as->R[(st)->src_b].u)

// Destination register L-values, for integer and FP registers
#define DEST_I (as->R[(st)->dest].i)
#define DEST_F (as->R[(st)->dest].f)

// These conditions are checked in many of the emulate routines; macro-izing 
// them makes things neater.  (The tests used to be more involved, before
// the explicit "speculative" flag was introduced.)
#define WILL_COMMIT (!speculative)

// Source memory R-values
#define SRC_MEM8 pmem_read_8(as->pmem, emu_state->srcmem, \
                               (WILL_COMMIT) ? 0 : PMAF_NoExcept)
#define SRC_MEM16 pmem_read_16(as->pmem, emu_state->srcmem, \
                               (WILL_COMMIT) ? 0 : PMAF_NoExcept)
#define SRC_MEM32 pmem_read_32(as->pmem, emu_state->srcmem, \
                               (WILL_COMMIT) ? 0 : PMAF_NoExcept)
#define SRC_MEM64 pmem_read_64(as->pmem, emu_state->srcmem, \
                               (WILL_COMMIT) ? 0 : PMAF_NoExcept)

// Writes to memory
#define SET_MEM8(val) if (CHECKPOINT_STORES || WILL_COMMIT) \
   pmem_write_8(as->pmem, emu_state->destmem, (val), \
                (WILL_COMMIT) ? 0 : PMAF_NoExcept)
#define SET_MEM16(val) if (CHECKPOINT_STORES || WILL_COMMIT) \
   pmem_write_16(as->pmem, emu_state->destmem, (val), \
                 (WILL_COMMIT) ? 0 : PMAF_NoExcept)
#define SET_MEM32(val) if (CHECKPOINT_STORES || WILL_COMMIT) \
   pmem_write_32(as->pmem, emu_state->destmem, (val), \
                 (WILL_COMMIT) ? 0 : PMAF_NoExcept)
#define SET_MEM64(val) if (CHECKPOINT_STORES || WILL_COMMIT) \
   pmem_write_64(as->pmem, emu_state->destmem, (val), \
                 (WILL_COMMIT) ? 0 : PMAF_NoExcept)


// "byte_loc" formulas used by by INSxx / EXTxx instructions
#define BYTE_LOC_LOW(rbv) (((rbv) & 0x7) * 8)
#define BYTE_LOC_HIGH(rbv) ((64 - (((rbv) & 0x7) * 8)) & 0x3f)


// Take full_width_mask, which should be a bit mask corresponding
// to the width of the desired type (e.g. 0xffff, 0xffffffff),
// and right-shift it 8*(8-rbv<2:0>) bits so that only the rightmost
//   (rbv_2_0 - (8 - mask width in bytes))
// bytes are nonzero.  This returs a _bit_ mask corresponding to the
//   BYTE_ZAP(...,NOT(byte_mask<15:8>))
// part of e.g. the INSLH instruction (see Alpha Architecture Handbook).
//
// We'll use a two-step shift, to avoid illegal shift-count 64 when
// rbv<7:0>==0.  Since we'll be shifting at least 8, we can bundle a
// constant shift with the incoming (likely constant) mask, for free.
static u64
bit_mask_insert_high(u64 full_width_mask, int rbv)
{
    // full_width_mask >> (8 * (8 - rbv<0:2>))
    return (full_width_mask >> 8) >> (8 * (8 - 1 - (rbv & 0x7)));
}


#define PERFORM_COVERAGE_EMULATE (ENABLE_COVERAGE_EMULATE && defined(DEBUG))
#if PERFORM_COVERAGE_EMULATE
    static const char *EmulateCoverageNames[] = {
        // Sorted
        "beq",
        "bge",
        "bgt",
        "blbc",
        "blbs",
        "ble",
        "blt",
        "bne",
        "br",
        "bsr",
        "call_pal_callsys",
        "call_pal_rduniq",
        "call_pal_wruniq",
        "fbeq",
        "fbge",
        "fbgt",
        "fble",
        "fblt",
        "fbne",
        "flti_adds",
        "flti_addt",
        "flti_cmpteq",
        "flti_cmptle",
        "flti_cmptlt",
        "flti_cvtqs",
        "flti_cvtqt",
        "flti_cvttq",
        "flti_cvtts",
        "flti_divs",
        "flti_divt",
        "flti_muls",
        "flti_mult",
        "flti_subs",
        "flti_subt",
        "fltl_cpys",
        "fltl_cpyse",
        "fltl_cpysn",
        "fltl_cvtlq",
        "fltl_cvtql",
        "fltl_fcmoveq",
        "fltl_fcmovge",
        "fltl_fcmovgt",
        "fltl_fcmovle",
        "fltl_fcmovlt",
        "fltl_fcmovne",
        "fltl_mf_fpcr",
        "fltl_mt_fpcr",
        "fpti_ftois",
        "fpti_ftoit",
        "fpti_maxub8",
        "fpti_maxsb8",
        "fpti_maxsw4",
        "fpti_minsw4",
        "fpti_minuw4",
        "fpti_sextb",
        "fpti_sextw",
        "inta_addl",
        "inta_addq",
        "inta_cmpbge",
        "inta_cmpeq",
        "inta_cmple",
        "inta_cmplt",
        "inta_cmpule",
        "inta_cmpult",
        "inta_s4addl",
        "inta_s4addq",
        "inta_s4subl",
        "inta_s4subq",
        "inta_s8addl",
        "inta_s8addq",
        "inta_s8subl",
        "inta_s8subq",
        "inta_subl",
        "inta_subq",
        "intl_amask",
        "intl_and",
        "intl_bic",
        "intl_bis",
        "intl_bis_smt_createstack",
        "intl_bis_smt_end_sim",
        "intl_bis_smt_fork",
        "intl_bis_smt_getid",
        "intl_bis_smt_hw_lock",
        "intl_bis_smt_print_sim",
        "intl_bis_smt_release",
        "intl_bis_smt_start_sim",
        "intl_bis_smt_terminate",
        "intl_cmoveq",
        "intl_cmovge",
        "intl_cmovgt",
        "intl_cmovlbc",
        "intl_cmovlbs",
        "intl_cmovle",
        "intl_cmovlt",
        "intl_cmovne",
        "intl_eqv",
        "intl_implver",
        "intl_ornot",
        "intl_xor",
        "intm_mull",
        "intm_mulq",
        "intm_umulh",
        "ints_extbl",
        "ints_extlh",
        "ints_extll",
        "ints_extqh",
        "ints_extql",
        "ints_extwh",
        "ints_extwl",
        "ints_insbl",
        "ints_inslh",
        "ints_insll",
        "ints_insqh",
        "ints_insql",
        "ints_inswh",
        "ints_inswl",
        "ints_mskbl",
        "ints_msklh",
        "ints_mskll",
        "ints_mskqh",
        "ints_mskql",
        "ints_mskwh",
        "ints_mskwl",
        "ints_sll",
        "ints_sra",
        "ints_srl",
        "ints_zap",
        "ints_zapnot",
        "invalid_instruction",
        "itfp_itoff",
        "itfp_itofs",
        "itfp_itoft",
        "itfp_sqrts",
        "itfp_sqrtt",
        "jmpjsr",
        "lda",
        "ldah",
        "ldbu",
        "ldl",
        "ldlq_l",
        "ldq",
        "ldq_u",
        "lds",
        "ldt",
        "ldwu",
        "mb",
        "nop",
        "stb",
        "stl",
        "stl_c_uni",
        "stlq_c",
        "stq",
        "stq_c_uni",
        "stq_u",
        "sts",
        "stt",
        "stw",
        "unimplemented_instruction",
        "wmb",
        NULL
    };
    static const char *FltiRoundCoverageNames[] = {
        // warning: index values & order of these are tied to INST<RND> and
        // FPCR<DYN> bits; see EMULATE_FLTI_PROLOGUE()
        "inst.chopped",
        "inst.minusinf",
        "inst.normal",
        "fpcr.chopped",
        "fpcr.minusinf",
        "fpcr.normal",
        "fpcr.plusinf",
        NULL
    };
    static const char *FltiTrapCoverageNames[] = {
        // warning: index values & order of these are tied to INST<TRP> bits;
        // see EMULATE_FLTI_PROLOGUE()
        "imprecise",
        "u_v",
        "reserved2",
        "reserved3",
        "reserved4",
        "su_sv",
        "reserved6",
        "sui_svi",
        NULL
    };
    static void
    coverage_helper_emulate(const char *point_name)
    {
        if (SP_F(EmulateDebugCoverage == NULL)) {
            EmulateDebugCoverage =
                debug_coverage_create("Emulate", EmulateCoverageNames, 1);
        }
        debug_coverage_reached(EmulateDebugCoverage, point_name);
    }
    static void
    coverage_helper_flti_round(const char *point_name)
    {
        if (SP_F(FltiRoundDebugCoverage == NULL)) {
            FltiRoundDebugCoverage =
                debug_coverage_create("EmulateFltiRound",
                                      FltiRoundCoverageNames, 1);
        }
        debug_coverage_reached(FltiRoundDebugCoverage, point_name);
    }
    static void
    coverage_helper_flti_trap(const char *point_name)
    {
        if (SP_F(FltiTrapDebugCoverage == NULL)) {
            FltiTrapDebugCoverage =
                debug_coverage_create("EmulateFltiTrap",
                                      FltiTrapCoverageNames, 1);
        }
        debug_coverage_reached(FltiTrapDebugCoverage, point_name);
    }
    #define COVERAGE_EMULATE(point) coverage_helper_emulate(point)
    #define COVERAGE_FLTI_ROUND(point) coverage_helper_flti_round(point)
    #define COVERAGE_FLTI_TRAP(point) coverage_helper_flti_trap(point)
#else
    #define COVERAGE_EMULATE(point) ((void) 0)
    #define COVERAGE_FLTI_ROUND(point) ((void) 0)
    #define COVERAGE_FLTI_TRAP(point) ((void) 0)
#endif  // PERFORM_COVERAGE_EMULATE


static void
emu_round_begin(const AppState *as, int flti_round_mode)
{
    FPCtlRoundingMode sim_round_mode;
    if (flti_round_mode == FLTI_RND_Dynamic) {
        // Read FPCR<DYN>.  The FPCR's bit meanings match for all but
        // FLTI_RND_Dynamic case, which changes in meaning to "round to plus
        // infinity".  We'll take advantage of this.
        flti_round_mode = GET_BITS_IDX(as->R[FPCR_REG].u, 59, 58);
    }

    switch (flti_round_mode) {
    case FLTI_RND_Chopped:  sim_round_mode = FPCtl_RoundToZero; break;
    case FLTI_RND_MinusInf: sim_round_mode = FPCtl_RoundDown; break;
    case FLTI_RND_Normal:   sim_round_mode = FPCtl_RoundToNearest; break;
    case FLTI_RND_Dynamic:
        // If the instruction itself had set "dynamic", then we read FPCR<DYN>
        // above, so here FPCR<DYN>==3 which maps to plus-infinity.
        sim_round_mode = FPCtl_RoundUp;
        break;
    default:
        sim_round_mode = 0;
        abort_printf("invalid input rounding mode 0x%x\n", flti_round_mode);
    }
    fpctl_setround(sim_round_mode);
}

static void
emu_round_end(void)
{
    fpctl_setround(FPCtl_RoundToNearest);
}


// Test: is this bit pattern a double-precision FP denormal?
static int
is_denorm_double(u64 fp_value_bits)
{
    // Per Alpha Architecture Handbook sec 2.2.6.2, T-format denormal
    // has exponent bits all zero, and some fraction bits nonzero.
    int result = (GET_BITS_IDX(fp_value_bits, 62, 52) == 0) &&
        (GET_BITS_IDX(fp_value_bits, 51, 0) != 0);
    return result;
}
/*
static int
is_denorm_float(u64 fp_value_bits)
{
    // Per Alpha Architecture Handbook sec 2.2.6.2, T-format denormal
    // has exponent bits all zero, and some fraction bits nonzero.
    int result = (GET_BITS_IDX(fp_value_bits, 62, 52) == 0) &&
        (GET_BITS_IDX(fp_value_bits, 51, 0) != 0);
    return result;
}
*/

static void
emu_trap_begin(AppState *as, int flti_trap_mode, int will_commit)
{
    static int warned_already[8];       // default to zeros
    sim_assert(IDX_OK(flti_trap_mode, NELEM(warned_already)));
    if (WARN_ABOUT_UNSUPPORTED_TRAP_QUALIFIERS &&
        (flti_trap_mode != FLTI_TRP_Imprecise) && will_commit &&
        !warned_already[flti_trap_mode]) {
        warned_already[flti_trap_mode] = 1;
        err_printf("unsupported FP trap qualifier 0x%x selected by "
                   "A%d PC 0x%s; ignoring this qualifier from here on\n",
                   flti_trap_mode, as->app_id, fmt_x64(as->npc));
    }
}


static void
emu_trap_end(AppState *as, const StashData *st)
{
    // See the Alpha Architecture Handbook, section 4.7.7.7:
    // "If an underflow occurs, a true zero (64 bits of zero) is always
    // stored in the result register. In the case of an IEEE operation
    // that takes an underflow arithmetic trap, a true zero is stored even
    // if the result after rounding would have been -0."
    //
    // See also section B.3, "Mapping to IEEE Standard", table B-2, "IEEE
    // Floating-Point Trap Handling".
    int flti_trap_qual = INST_FLTI_TRAP(st->inst);
    if ((flti_trap_qual == FLTI_TRP_Imprecise) ||
        (flti_trap_qual == FLTI_TRP_UnderflowEnable)) {
        int output_underflow = !(st->gen_flags && SGF_FPLiteralWrite) &&
            IS_FP_REG(st->dest) && is_denorm_double(as->R[st->dest].u);
        if (output_underflow) {
            // In "imprecise" (no trap qualifier) mode, denormals are silenty
            // zeroed.  In UnderflowEnable (/U) mode, denormals are zeroed,
            // and an FP trap is signaled (which we don't support).
            as->R[st->dest].u = 0;
        }
    } else {
        // Here, we're into precise exception completion territory.  We don't
        // support signaling exceptions into the simulated thread.  For now we
        // hand-wave and hope the host's IEEE754 is producing the FP results
        // that match what an Alpha's FP exception-completion handler would
        // have (e.g. arithmetic on denormals).
        //
        // This is awkward in that we explicitly won't get this behavior if
        // we're simulating on an alpha, since our compiled object code
        // doesn't use the "/S" trap qualifier with its arithmetic.
        // If this ends up being important, it looks like we can select
        // alpha trap modes per-object-file, with GCC flag 
        // -mfp-trap-mode={n,u,su,sui}, dec CC flag "-fptm".
    }
}


// Convert an alpha "T" value (IEEE double-precision, 64-bit) to "S" precision
// (IEEE single-precision, 32-bit).
//
// We used to just use (float) casts in each routine; this makes it explicit,
// and allows us to take care of alpha-specific behavior as needed.
static double
alpha_t_to_s(const StashData *st, double src_value)
{
    FltiTrapType trap_qual = INST_FLTI_TRAP(st->inst);
    // Rounds as needed; uses rounding mode as set by EMULATE_FLTI_PROLOGUE().
    double result = (float) src_value;

    if (finite(result)) {
        double src_mag = fabs(src_value);
        int single_prec_underflow = (src_mag > 0) && (src_mag < FLT_MIN);
        if (single_prec_underflow) {
            if ((trap_qual == FLTI_TRP_Imprecise) ||
                (trap_qual == FLTI_TRP_UnderflowEnable)) {
                // Value would underflow (into a denormal) in single-precision
                // format.  Alpha hardware generates "true zero" in this case
                // (64 0-bits), leaving it to optional exception handling to
                // compute a different output if desired.
                //
                // We can't just rely on emu_trap_end()'s denormal detection,
                // since that only sees the result after it's been written as
                // double-precision, where it's likely not a denorm anymore.
                //
                // See emu_trap_end() function for references about alpha
                // denormal handling.
                result = 0.0;
            } else {
                // As with emu_trap_end(), here we punt and hope that the
                // native IEEE754 will produce a result that matches
                // the Alpha's trap-completion handler.
            }
        }
    }
    return result;
}

// Convert an alpha "S" value (IEEE double-precision, 64-bit) to "T" precision
// (IEEE single-precision, 32-bit).
//
// We used to just use (float) casts in each routine; this makes it explicit,
// and allows us to take care of alpha-specific behavior as needed.
static double
alpha_s_to_t(const StashData *st, float src_value)
{
    FltiTrapType trap_qual = INST_FLTI_TRAP(st->inst);
    // Rounds as needed; uses rounding mode as set by EMULATE_FLTI_PROLOGUE().
    double result = (double) src_value;

    if (finite(result)) {
        double src_mag = fabs(src_value);
        int single_prec_underflow = (src_mag > 0) && (src_mag < FLT_MIN);
        if (single_prec_underflow) {
            if ((trap_qual == FLTI_TRP_Imprecise) ||
                (trap_qual == FLTI_TRP_UnderflowEnable)) {
                // Value would underflow (into a denormal) in single-precision
                // format.  Alpha hardware generates "true zero" in this case
                // (64 0-bits), leaving it to optional exception handling to
                // compute a different output if desired.
                //
                // We can't just rely on emu_trap_end()'s denormal detection,
                // since that only sees the result after it's been written as
                // double-precision, where it's likely not a denorm anymore.
                //
                // See emu_trap_end() function for references about alpha
                // denormal handling.
                result = 0.0;
            } else {
                // As with emu_trap_end(), here we punt and hope that the
                // native IEEE754 will produce a result that matches
                // the Alpha's trap-completion handler.
            }
        }
    }
    return result;
}


// Hooks for certain FLTI/ITFP operations to set common rounding and trap
// modes, at the start and end of the instruction.
//
// We could further abstract these by putting rounding/trap fields in the
// StashData struct and handling them out in emulate_inst(), but since they
// apply to so few instructions, it might not be worth the performance hit to
// test them every time.  If we do add trap support, we may end up adding trap
// outputs to EmuInstState.
//
// (These are ugly, but they save us the mechanical insertion of the
// same test code in dozens of functions, and insulate otherwise-unrelated
// emulate function body code from the passing of pieces of the emulate
// arg-list onto the rounding/trap helpers.)
#define EMULATE_FLTI_PROLOGUE() do { \
    int inst_rnd_mode = INST_FLTI_ROUND(st->inst); \
    int inst_trp_mode = INST_FLTI_TRAP(st->inst); \
    COVERAGE_FLTI_ROUND(\
        FltiRoundCoverageNames[((inst_rnd_mode == FLTI_RND_Dynamic) ? \
                                (3+GET_BITS_IDX(as->R[FPCR_REG].u, 59, 58)) : \
                                inst_rnd_mode)]); \
    COVERAGE_FLTI_TRAP(FltiTrapCoverageNames[inst_trp_mode]); \
    if (inst_rnd_mode != FLTI_RND_Normal) { \
        emu_round_begin(as, inst_rnd_mode); \
    } \
    emu_trap_begin(as, inst_trp_mode, WILL_COMMIT); \
} while(0)

#define EMULATE_FLTI_EPILOGUE() do { \
    if (INST_FLTI_ROUND(st->inst) != FLTI_RND_Normal) { \
        emu_round_end(); \
    } \
    emu_trap_end(as, st); \
} while(0)




// MAP_S, MAP_F (S/F-format exponent mapping) function from the Alpha
// Architecture Handbook.  This operates on bits strings, mapping from {0,1}^8
// to {0,1}^11, inserting a three-bit string (either 0^3 or 1^3) after the
// first bit.
//
// MAP_S:
//   Input      Output
//   1 1^7      1 1^3 1^7
//   1 X        1 0^3 X         (X in {0,1}^7 - {1^7} )
//   0 0^7      0 0^3 0^7
//   0 X        0 1^3 X         (X in {0,1}^7 - {0^7} )
//
// MAP_F:
//   Input      Output
//   1 X        1 0^3 X         (X in {0,1}^7)
//   0 0^7      0 0^3 0^7
//   0 X        0 1^3 X         (X in {0,1}^7 - {0^7} )

static inline unsigned
map_s(const unsigned bits)
{
    unsigned insert, result;
    sim_assert(bits <= 0xff);
    insert = ((bits == 0xff) || (!(bits & 0x80) && (bits != 0))) ? 0x7 : 0;
    insert <<= 7;
    result = ((bits & 0x80) << 3) | insert | (bits & 0x7f);
    sim_assert(result <= 0x7ff);
    return result;
}


static inline unsigned
map_f(const unsigned bits)
{
    unsigned insert, result;
    sim_assert(bits <= 0xff);
    insert = (!(bits & 0x80) && (bits != 0)) ? 0x7 : 0;
    insert <<= 7;
    result = ((bits & 0x80) << 3) | insert | (bits & 0x7f);
    sim_assert(result <= 0x7ff);
    return result;
}


/*  All instruction emulation is done via these short routines
    that assume that a bunch of static information about the
    instruction is known and is stored in the stash data
    structure.
*/


static void
emulate_unimplemented_instruction(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("unimplemented_instruction");
    if (WILL_COMMIT) {
        printf("ERROR: unimplemented instruction\n");
        printf("ERROR: pc = %s, opcode = %x\n",
               fmt_x64(as->npc), (INST_OPCODE(st->inst)));
        printf("ERROR: instruction = %x, cycle = %s\n",
               st->inst, fmt_i64(cyc));
        sim_abort();
    }
}

static void
emulate_invalid_instruction(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("invalid_instruction");
    if (WILL_COMMIT) {
        printf("ERROR: invalid (illegal) instruction\n");
        printf("ERROR: pc = %s, opcode = %x\n",
               fmt_x64(as->npc), (INST_OPCODE(st->inst)));
        printf("ERROR: instruction = %x, cycle = %s\n",
               st->inst, fmt_i64(cyc));
        sim_abort();
    }
}

static void
emulate_nop(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("nop");
}

static void
emulate_rpcc(EMULATE_ARGS_DECL)
{
    DEST_I = cyc;
}

static void
emulate_lda(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("lda");
    i64 disp = INST_MEM_FUNC(st->inst);
    DEST_I = SRC_RBV + SEXT_TO_i64(disp, 16);
}

static void
emulate_ldah(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldah");
    i64 disp = INST_MEM_FUNC(st->inst) << 16;
    DEST_I = SRC_RBV + SEXT32_i64(disp);
}

static void
emulate_ldq_u(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldq_u");
    DEST_I = SRC_MEM64;
}

static void
emulate_stq_u(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stq_u");
    u64 rav = SRC_RAV;
    SET_MEM64(rav);
}

static void
emulate_inta_addl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_addl");
    u32 long_val = (SRC_RAV + SRC_RBV_OP);
    DEST_I = SEXT32_i64(long_val);
}

static void
emulate_inta_addq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_addq");
    DEST_I = SRC_RAV + SRC_RBV_OP;
}

static void
emulate_inta_cmple(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_cmple");
    DEST_I = SRC_RAV <= SRC_RBV_OP;
}

static void
emulate_inta_cmpult(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_cmpult");
    u64 rav = SRC_RAV;
    u64 rbv = SRC_RBV_OP;
    DEST_I = rav < rbv;
}

static void
emulate_inta_s4addl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s4addl");
    u32 long_val = ((SRC_RAV << 2) + SRC_RBV_OP);
    DEST_I = SEXT32_i64(long_val);
}

static void
emulate_inta_s4subq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s4subq");
    DEST_I = (SRC_RAV << 2) - SRC_RBV_OP;
}

static void
emulate_inta_s8subl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s8subl");
    u32 long_val = ((SRC_RAV << 3) - SRC_RBV_OP);
    DEST_I = SEXT32_i64(long_val);
}

static void
emulate_inta_cmpbge(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_cmpbge");
    u64 rav = SRC_RAV;
    u64 rbv = SRC_RBV_OP;
    u64 dst = 0;
    for (int i = 56; i >= 0; i -= 8) {
        dst = (dst << 1) | (((rav >> i) & 0xff) >= ((rbv >> i) & 0xff));
    }
    DEST_I = dst;
}

static void
emulate_inta_cmplt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_cmplt");
    DEST_I = SRC_RAV < SRC_RBV_OP;
}

static void
emulate_inta_subl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_subl");
    u32 long_val = (SRC_RAV - SRC_RBV_OP);
    DEST_I = SEXT32_i64(long_val);
}

static void
emulate_inta_subq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_subq");
    DEST_I = SRC_RAV - SRC_RBV_OP;
}

static void
emulate_inta_s4addq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s4addq");
    DEST_I = (SRC_RAV << 2) + SRC_RBV_OP;
}

static void
emulate_inta_s8addl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s8addl");
    u32 long_val = ((SRC_RAV << 3) + SRC_RBV_OP);
    DEST_I = SEXT32_i64(long_val);
}

static void
emulate_inta_s8subq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s8subq");
    DEST_I = (SRC_RAV << 3) - SRC_RBV_OP;
}

static void
emulate_inta_cmpeq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_cmpeq");
    DEST_I = (SRC_RAV == SRC_RBV_OP);
}

static void
emulate_inta_cmpule(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_cmpule");
    u64 rav = SRC_RAV;
    u64 rbv = SRC_RBV_OP;
    DEST_I = rav <= rbv;
}

static void
emulate_inta_s4subl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s4subl");
    u32 long_val = ((SRC_RAV << 2) - SRC_RBV_OP);
    DEST_I = SEXT32_i64(long_val);
}

static void
emulate_inta_s8addq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("inta_s8addq");
    DEST_I = (SRC_RAV << 3) + SRC_RBV_OP;
}

static void
emulate_intl_and(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_and");
    DEST_I = SRC_RAV & SRC_RBV_OP;
}

static void
emulate_intl_bic(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bic");
    DEST_I = SRC_RAV & ~SRC_RBV_OP;
}

static void
emulate_intl_bis_smt_fork(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_fork");
sim_abort();
#ifdef TEMP_SIMSTATE_HACKING            // simstate
    int i;

    /*
    ** Find a free context and get it going.
    */
    if (!WILL_COMMIT)
        return;
    jtimer_startstop(SimTimer, 1);

#ifdef DEBUG
    if (debugsync) {
        printf("Thread running status: {");
        for (i = 0; i < numcontexts; i++) {
            printf("%d%c ", i, (thecontexts[i]->running) ? 'R' : 'N');
        }
        printf("}\n");
    }
#endif

    for (i = 0; i < numcontexts; i++) {
        if (!thecontexts[i]->running 
            && (thecontexts[i]->masterid == ctx->masterid)) {
            thecontexts[i]->running = 1;
            break;
        }
    }

    if (i == numcontexts) {
        printf("ERROR: Couldn't find free ");
        printf("HW context for SMT_FORK\n");
    }
#ifdef DEBUG
    else if (debugsync) {
        printf("SMT_FORK for ctx context (#%d)\n",
               ctx->id);
        printf("using free HW context #%d\n", i);
    }
#endif
    // Fork begins execution at the function label passed as a parameter
    memcpy(thecontexts[i]->as.R, ctx->as.R, sizeof(ctx->as.R));

    thecontexts[i]->nextpc = ctx->as.R[4].i;
    thecontexts[i]->tc.avail = 0;
    // thecontexts[i]->as.R[30] = ctx->stacktop;
    thecontexts[i]->as.R[30].i = thecontexts[i]->seg_info.stack_init_top;

    activethreads++;
    multiplethreads = 1;

#ifdef DEBUG
    if (debugsync)
        printf("Setting pc for new context #%d to 0x%s\n", i,
               fmt_x64(thecontexts[i]->nextpc));
#endif
#endif
}

static void
emulate_intl_bis_smt_getid(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_getid");
sim_abort();
#ifdef TEMP_SIMSTATE_HACKING                    // simstate
#ifdef DEBUG
    if (debugsync)
        printf("SMT_GETID for app %d\n", as->app_id);
#endif

    // Place the ID in the function return register.
    as->R[2].i = as->app_id;
#endif
}

static void
emulate_intl_bis_smt_createstack(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_createstack");
sim_abort();
#ifdef TEMP_SIMSTATE_HACKING                    // simstate
#ifdef DEBUG
    if (debugsync)
        printf("SMT_CREATESTACK for ctx context (#%d)\n", ctx->id);
#endif
    // Does this even make sense anymore?
    ctx->as.R[30].i = ctx->seg_info.stack_init_top;
#endif
}

static void
emulate_intl_bis_smt_hw_lock(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_hw_lock");
// This instruction attempts to acquire the lock at the memory location 0($16)
// or 0($2).  If the lock is already set, then it blocks and the acquire data
// structure is updated with this information.
}

static void
emulate_intl_bis_smt_release(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_release");
// This instruction gives up the lock at the memory location 0($16) or 0($2).
}

static void
emulate_intl_bis_smt_terminate(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_terminate");
sim_abort();
#ifdef TEMP_SIMSTATE_HACKING            // simstate
    // Stop executing this thread
    if (WILL_COMMIT) {  
        ctx->running = 0;
        --activethreads;
    }
#endif
}

static void
emulate_intl_bis_smt_start_sim(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_start_sim");
sim_abort();
#ifdef TEMP_SIMSTATE_HACKING            // simstate
    if (WILL_COMMIT) {
        DEBUGPRINTF("SMT_START_SIM emulated\n");
        jtimer_startstop(SimTimer, 1);
        zero_cstats();
        zero_pstats();
    }
#endif
}

static void
emulate_intl_bis_smt_print_sim(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_print_sim");
sim_abort();
#ifdef TEMP_SIMSTATE_HACKING            // simstate
    if (WILL_COMMIT) {
        printf("********  SMT_PRINT_SIM Timing Point **********\n");
        print_sim_stats(0);
        reset_stats();
    }
#endif
}

static void
emulate_intl_bis_smt_end_sim(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis_smt_end_sim");
sim_abort();
#ifdef TEMP_SIMSTATE_HACKING            // simstate
    if (WILL_COMMIT) {
        DEBUGPRINTF("SMT_END_SIM emulated\n");
        jtimer_startstop(SimTimer, 0);
    }
#endif
}


static void
emulate_intl_bis(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_bis");
    DEST_I = SRC_RAV | SRC_RBV_OP;
}

static void
emulate_intl_cmoveq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmoveq");
    if (SRC_RAV == 0)
        DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_cmovlbc(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmovlbc");
    if ((SRC_RAV & 1) == 0)
        DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_cmovlbs(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmovlbs");
    if (SRC_RAV & 1)
        DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_cmovge(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmovge");
    if (SRC_RAV >= 0)
        DEST_I = SRC_RBV_OP;
}


static void
emulate_intl_cmovgt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmovgt");
    if (SRC_RAV > 0)
        DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_cmovle(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmovle");
    if (SRC_RAV <= 0)
        DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_cmovlt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmovlt");
    if (SRC_RAV < 0)
        DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_cmovne(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_cmovne");
    if (SRC_RAV)
        DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_eqv(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_eqv");
    DEST_I = SRC_RAV ^ ~SRC_RBV_OP;
}

static void
emulate_intl_ornot(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_ornot");
    DEST_I = SRC_RAV | ~SRC_RBV_OP;
}

static void
emulate_intl_xor(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_xor");
    DEST_I = SRC_RAV ^ SRC_RBV_OP;
}

static void
emulate_intl_amask(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_amask");
    // Pass all requested feature-bits through.  (If something's missing
    // that we need, we'll find out soon enough.)
    // If we later start implementing floating-point traps for FLTI
    // operations, we should probably make AMASK indicate that the hardware
    // does precise traps (sets bit 9), since we're simulating anyway.
    DEST_I = SRC_RBV_OP;
}

static void
emulate_intl_implver(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intl_implver");
    DEST_I = 2;         // 2: 21264/EV6 (see Alpha Arch. handbook, sec D.3)
}

static void
emulate_ints_extbl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_extbl");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U >> byte_loc) & 0xff;
}

static void
emulate_ints_extlh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_extlh");
    int byte_loc = BYTE_LOC_HIGH(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U << byte_loc) & U64_LIT(0xffffffff);
}

static void
emulate_ints_extll(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_extll");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U >> byte_loc) & U64_LIT(0xffffffff);
}

static void
emulate_ints_extqh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_extqh");
    int byte_loc = BYTE_LOC_HIGH(SRC_RBV_OP);
    DEST_I = SRC_RAV_U << byte_loc;     // mask with U64_MAX is redundant
}

static void
emulate_ints_extql(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_extql");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = SRC_RAV_U >> byte_loc;     // mask with U64_MAX is redundant
}

static void
emulate_ints_extwh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_extwh");
    int byte_loc = BYTE_LOC_HIGH(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U << byte_loc) & 0xffff;
}

static void
emulate_ints_extwl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_extwl");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U >> byte_loc) & 0xffff;
}

static void
emulate_ints_insbl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_insbl");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U & 0xff) << byte_loc;
}

static void
emulate_ints_inslh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_inslh");
    u64 rbv = SRC_RBV_OP;
    int byte_loc = BYTE_LOC_HIGH(rbv);
    u64 bit_mask_zapnot = bit_mask_insert_high(U64_LIT(0xffffffff), rbv);
    DEST_I = (SRC_RAV_U >> byte_loc) & bit_mask_zapnot;
}

static void
emulate_ints_insll(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_insll");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U & U64_LIT(0xffffffff)) << byte_loc;
}

static void
emulate_ints_insqh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_insqh");
    u64 rbv = SRC_RBV_OP;
    int byte_loc = BYTE_LOC_HIGH(rbv);
    u64 bit_mask_zapnot = bit_mask_insert_high(U64_MAX, rbv);
    DEST_I = (SRC_RAV_U >> byte_loc) & bit_mask_zapnot;
}

static void
emulate_ints_insql(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_insql");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = SRC_RAV_U << byte_loc;     // ANDing with U64_MAX is redundant
}

static void
emulate_ints_inswh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_inswh");
    u64 rbv = SRC_RBV_OP;
    int byte_loc = BYTE_LOC_HIGH(rbv);
    u64 bit_mask_zapnot = bit_mask_insert_high(U64_LIT(0xffff), rbv);
    DEST_I = (SRC_RAV_U >> byte_loc) & bit_mask_zapnot;
}

static void
emulate_ints_inswl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_inswl");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = (SRC_RAV_U & U64_LIT(0xffff)) << byte_loc;
}

static void
emulate_ints_mskbl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_mskbl");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = SRC_RAV_U & ~(U64_LIT(0xff) << byte_loc);
}

static void
emulate_ints_msklh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_msklh");
    u64 bit_mask_zapnot = bit_mask_insert_high(U64_LIT(0xffffffff),
                                               SRC_RBV_OP);
    DEST_I = SRC_RAV_U & ~bit_mask_zapnot;
}

static void
emulate_ints_mskll(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_mskll");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = SRC_RAV_U & ~(U64_LIT(0xffffffff) << byte_loc);
}

static void
emulate_ints_mskqh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_mskqh");
    u64 bit_mask_zapnot = bit_mask_insert_high(U64_MAX, SRC_RBV_OP);
    DEST_I = SRC_RAV_U & ~bit_mask_zapnot;
}

static void
emulate_ints_mskql(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_mskql");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = SRC_RAV_U & ~(U64_MAX << byte_loc);
}

static void
emulate_ints_mskwh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_mskwh");
    u64 bit_mask_zapnot = bit_mask_insert_high(U64_LIT(0xffff), SRC_RBV_OP);
    DEST_I = SRC_RAV_U & ~bit_mask_zapnot;
}

static void
emulate_ints_mskwl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_mskwl");
    int byte_loc = BYTE_LOC_LOW(SRC_RBV_OP);
    DEST_I = SRC_RAV_U & ~(U64_LIT(0xffff) << byte_loc);
}

static void
emulate_ints_sll(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_sll");
    DEST_I = SRC_RAV << (SRC_RBV_OP & 0x3f);
}

static void
emulate_ints_sra(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_sra");
    i64 rav = SRC_RAV;
    i64 rbv = SRC_RBV_OP;
    DEST_I = ARITH_RIGHT_SHIFT(rav, (rbv & 0x3f));
}

static void
emulate_ints_srl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_srl");
    DEST_I = SRC_RAV_U >> (SRC_RBV_OP & 0x3f);
}

static void
emulate_ints_zap(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_zap");
    u64 rav = SRC_RAV;
    u64 rbv = SRC_RBV_OP;
    u64 temp = 0xff;
    int i;
    DEST_I = 0;
    for (i=1; i<0x100; i=i<<1) {
        if (i & ~rbv)
            DEST_I |= rav & temp;
        temp = temp << 8;
    }
}

static void
emulate_ints_zapnot(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ints_zapnot");
    u64 rav = SRC_RAV;
    u64 rbv = SRC_RBV_OP;
    u64 temp = 0xff;
    int i;
    DEST_I = 0;
    for (i=1; i<0x100; i=i<<1) {
        if (i & rbv)
            DEST_I |= rav & temp;
        temp = temp << 8;
    }
}

static void
emulate_intm_mull(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intm_mull");
    u32 long_val = (SRC_RAV * SRC_RBV_OP);
    DEST_I = SEXT32_i64(long_val);
}

static void
emulate_intm_mulq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intm_mulq");
    DEST_I = SRC_RAV * SRC_RBV_OP;
}


// Multiply two unsigned 64-bit factors, and return the _high_ 64 bits
// of the 128-bit product.
static u64
u_mult64_high64(u64 fact1, u64 fact2)
{
    // Break factors into 32-bit fragments, such that
    //   fact1 == a . b
    //   fact2 == c . d
    // ...where "." is concatenation.
    // (We'll use 64-bit values so we can multiply them without overflow.)
    const u64 low32 = U64_LIT(0xffffffff);
    u64 a = fact1 >> 32;
    u64 b = fact1 & low32;
    u64 c = fact2 >> 32;
    u64 d = fact2 & low32;

    // Now, we'll do grade school style multiplication "by hand", except in
    // base-32 instead of base 10.  Here, lo(X) and hi(X) refer to the low and
    // high 32 bits of X; carry(X,Y) is the carry-out from adding 32-bit
    // operands X and Y.
    //                                              a        b
    //    *                                         c        d
    //    ----------------------------------------------------
    //                   hi(ad)+C1  lo(lo(ad)+hi(bd))   lo(bd)
    //  + hi(ac)+C2  lo(ac)+hi(bc)             lo(bc)        0
    //           C4             C3
    //  ------------------------------------------------------
    //         SUM4           SUM3               SUM2     SUM1
    //
    // C1=carry(lo(ad),hi(bd))
    // C2=carry(lo(ac),hi(bc))
    //
    // SUM1 = lo(bd)
    // SUM2 = lo(lo(lo(ad)+hi(bd))+lo(bc))
    // C3 = carry(lo(lo(ad)+hi(bd)),lo(bc))
    // SUM3 = lo(hi(ad) + lo(ac) + hi(bc) + C1 + C3)
    // SUM4 = hi(ac) + C2 + C4
    //
    // The desired high 64 bits for output are "SUM4 . SUM3"; we can
    // collect that with 64-bit adds, which automatically handles C2 and C4.
    //
    // result = ac + hi(ad) + hi(bc) + C1 + C3
    
    u64 ac = a * c;
    u64 ad = a * d;
    u64 bc = b * c;
    u64 bd = b * d;

    int carry1 = ((ad & low32) + (bd >> 32)) >> 32;
    int carry3 = (((ad + (bd >> 32)) & low32)
                  + (bc & low32)) >> 32;
    sim_assert(!(carry1 & ~1));         // 0 or 1
    sim_assert(!(carry3 & ~1));         // 0 or 1

    u64 result = ac + (ad >> 32) + (bc >> 32) + carry1 + carry3;
    return result;
}

static void
emulate_intm_umulh(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("intm_umulh");
    DEST_I = u_mult64_high64(SRC_RAV, SRC_RBV_OP);
}

static void
emulate_itfp_itofs(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("itfp_itofs");
    // Fc <---- Rav<31> . MAP_S(Rav<30:23>) . Rav<22:0> . 0<28:0>
    // width:   1         11                  23          29
    // offset:  63        52                  29          0
    u64 longhold = SRC_RAV;
    longhold = (GET_BITS_IDX(longhold, 31, 31) << 63) |
        (((u64) map_s(GET_BITS_IDX(longhold, 30, 23))) << 52) |
        (GET_BITS_IDX(longhold, 22, 0) << 29);
    DEST_I = longhold;
}

static void
emulate_itfp_itoff(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("itfp_itoff");
    // Fc <---- Rav<31> . MAP_F(Rav<30:23>) . Rav<22:0> . 0<28:0>
    // width:   1         11                  23          29
    // offset:  63        52                  29          0
    u64 longhold = SRC_RAV;
    longhold = (GET_BITS_IDX(longhold, 31, 31) << 63) |
        (((u64) map_f(GET_BITS_IDX(longhold, 30, 23))) << 52) |
        (GET_BITS_IDX(longhold, 22, 0) << 29);
    DEST_I = longhold;
}

static void
emulate_itfp_itoft(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("itfp_itoft");
    DEST_I = SRC_RAV;
}

static void
emulate_itfp_sqrts(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("itfp_sqrts");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = alpha_t_to_s(st, sqrt(SRC_FBV));
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_itfp_sqrtt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("itfp_sqrtt");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = sqrt(SRC_FBV);
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_fltl_cpys(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_cpys");
    // Fc <---- Fav<63> . Fbv<62:0>
    // width:   1         63
    // offset:  63        0
    DEST_I = (GET_BITS_IDX(SRC_FAV_U, 63, 63) << 63) |
        GET_BITS_IDX(SRC_FBV_U, 62, 0);
}

static void
emulate_fltl_cpyse(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_cpyse");
    // Fc <---- Fav<63:52> . Fbv<51:0>
    // width:   12           52
    // offset:  52           0
    DEST_I = (GET_BITS_IDX(SRC_FAV_U, 63, 52) << 52) |
        GET_BITS_IDX(SRC_FBV_U, 51, 0);
}

static void
emulate_fltl_cpysn(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_cpysn");
    // Fc <---- not(Fav<63>) . Fbv<62:0>
    // width:   1              63
    // offset:  63             0
    DEST_I = ((GET_BITS_IDX(SRC_FAV_U, 63, 63) ^ 1) << 63) |
        GET_BITS_IDX(SRC_FBV_U, 62, 0);
}

static void
emulate_fltl_cvtlq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_cvtlq");
    // See Alpha book sec 2.2.7 and 2.2.8, "Longword-" and "Quadword Integer
    // Format in Floating-Point Unit".
    //   Fc <- SEXT(Fbv<63:62> . Fbv<58:29>)
    //   width:     2            30
    //   offset:    30           0
    u64 longhold = SRC_FBV_I;
    longhold = (GET_BITS_IDX(longhold, 63, 62) << 30) |
        GET_BITS_IDX(longhold, 58, 29);
    longhold = SEXT32_i64(longhold);
    DEST_I = longhold;
}

static void
emulate_fltl_cvtql(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_cvtql");
    // See Alpha book sec 2.2.7 and 2.2.8, "Longword-" and "Quadword Integer
    // Format in Floating-Point Unit".
    //   Fc <- Fbv<31:30> . 0<2:0> . Fbv<29:0> . 0<28:0>
    //   width:    2          3          30        29
    //   offset:   62         59         29        0
    u64 longhold = SRC_FBV_I;
    longhold = (GET_BITS_IDX(longhold, 31, 30) << 62) |
        (GET_BITS_IDX(longhold, 29, 0) << 29);
    DEST_I = longhold;
}

static void
emulate_fltl_fcmoveq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_fcmoveq");
    if ((SRC_FAV_I << 1) == 0)  // discard sign bit, match +/- 0
        DEST_I = SRC_FBV_I;
}

static void
emulate_fltl_fcmovge(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_fcmovge");
    if (((SRC_FAV_I << 1) == 0)         // +/- 0, or
        || (SRC_FAV_I >= 0))            // zero sign bit (nonnegative)
        DEST_I = SRC_FBV_I;
}

static void
emulate_fltl_fcmovgt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_fcmovgt");
    if (SRC_FAV_I > 0)                  // zero sign bit, but not all zeroes
        DEST_I = SRC_FBV_I;
}

static void
emulate_fltl_fcmovle(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_fcmovle");
    if (((SRC_FAV_I << 1) == 0) ||      // +/- 0, or
        (SRC_FAV_I < 0))                // sign bit set (negative)
        DEST_I = SRC_FBV_I;
}

static void
emulate_fltl_fcmovlt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_fcmovlt");
    if (((SRC_FAV_I << 1) != 0) &&      // not +/- 0, AND
        (SRC_FAV_I < 0))                // sign bit set (negative)
        DEST_I = SRC_FBV_I;
}

static void
emulate_fltl_fcmovne(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_fcmovne");
    if ((SRC_FAV_I << 1) != 0)  // discard sign bit, match not +/- 0
        DEST_I = SRC_FBV_I;
}

static void
emulate_fltl_mt_fpcr(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_mt_fpcr");
    u64 work = SRC_FAV_I;
    // FPCR<46:0> are "read as zero, ignore on write"
    work &= ~ BIT_MASKIDX_64(46, 0);
    DEST_I = work;
}

static void
emulate_fltl_mf_fpcr(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fltl_mf_fpcr");
    u64 work = SRC_RAV;
    // FPCR<46:0> are "read as zero, ignore on write"
    work &= ~ BIT_MASKIDX_64(46, 0);
    DEST_I = work;
}
 
static void
emulate_flti_adds(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_adds");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = alpha_t_to_s(st, SRC_FAV + SRC_FBV);
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_addt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_addt");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = SRC_FAV + SRC_FBV;
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_cmpteq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cmpteq");
    EMULATE_FLTI_PROLOGUE();    // (no rounding; but there are still trap bits)
    DEST_F = ((SRC_FAV_I == SRC_FBV_I) ||       // bitwise-match, or both zero
              ((SRC_FAV_I<<1 == 0) && (SRC_FBV_I<<1 == 0))) ? 2.0 : 0;
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_cmptlt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cmptlt");
    EMULATE_FLTI_PROLOGUE();    // (no rounding; but there are still trap bits)
    DEST_F = (SRC_FAV < SRC_FBV) ? 2.0 : 0;
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_cmptle(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cmptle");
    EMULATE_FLTI_PROLOGUE();    // (no rounding; but there are still trap bits)
    DEST_F = (SRC_FAV <= SRC_FBV) ? 2.0 : 0;
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_cmptun(EMULATE_ARGS_DECL)
{
    DEST_F = (!(SRC_FAV < SRC_FBV) && !(SRC_FAV == SRC_FBV) && !(SRC_FAV > SRC_FBV));
//    printf("cmptun: result: %lf src: %lf %lf\n",DEST_F,SRC_FAV,SRC_FBV);    
}

static void
emulate_flti_cvtqs(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cvtqs");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = alpha_t_to_s(st, (double) SRC_FBV_I);
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_cvtqt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cvtqt");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = SRC_FBV_I;
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_cvtts(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cvtts");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = alpha_t_to_s(st, SRC_FBV);
    EMULATE_FLTI_EPILOGUE();
}
//Added by Hung-Wei
static void
emulate_flti_cvtst(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cvtst");
    EMULATE_FLTI_PROLOGUE();
//    fprintf(stderr,"The stored value : %s -> %lf\n",fmt_x64(SRC_RBV),SRC_FBV);
      DEST_F = alpha_s_to_t(st,SRC_FBV);
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_divs(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_divs");
    EMULATE_FLTI_PROLOGUE();
    if (FLOAT_DIVZERO_KNOWS_ABOUT_WP && !WILL_COMMIT && (SRC_FBV == 0)) {
        DEST_F = 0.0;
    } else {
        DEST_F = alpha_t_to_s(st, SRC_FAV / SRC_FBV);
    }
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_divt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_divt");
    EMULATE_FLTI_PROLOGUE();
    if (FLOAT_DIVZERO_KNOWS_ABOUT_WP && !WILL_COMMIT && (SRC_FBV == 0)) {
        DEST_F = 0.0;
    } else {
        DEST_F = SRC_FAV / SRC_FBV;
    }
    EMULATE_FLTI_EPILOGUE();
}
  
static void
emulate_flti_muls(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_muls");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = alpha_t_to_s(st, SRC_FAV * SRC_FBV);
    EMULATE_FLTI_EPILOGUE();
}
  
static void
emulate_flti_mult(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_mult");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = SRC_FAV * SRC_FBV;
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_subs(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_subs");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = alpha_t_to_s(st, SRC_FAV - SRC_FBV);
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_flti_subt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_subt");
    EMULATE_FLTI_PROLOGUE();
    DEST_F = SRC_FAV - SRC_FBV;
    EMULATE_FLTI_EPILOGUE();
}
// Modified by Hung-Wei	
static void
emulate_flti_cvttq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("flti_cvttq");
    EMULATE_FLTI_PROLOGUE();
    switch(INST_FLTI_ROUND(st->inst))
    {
      case FLTI_RND_Chopped:
        DEST_I = (i64)trunc((SRC_FBV));
        break;        
      case FLTI_RND_MinusInf:
        DEST_I = (i64)floor((SRC_FBV));
        break;
      case FLTI_RND_Dynamic:
      {
        //int flti_round_mode = GET_BITS_IDX(as->R[FPCR_REG].u, 59, 58);
        switch(INST_FLTI_ROUND(st->inst))
        {
          case FLTI_RND_Chopped:
            DEST_I = (i64)trunc((SRC_FBV));
            break;        
          case FLTI_RND_MinusInf:
            DEST_I = (i64)floor((SRC_FBV));
            break;
          case FLTI_RND_Dynamic:
            DEST_I = (i64)ceil((SRC_FBV));
            break;          
          default:
            DEST_I = (i64)round((SRC_FBV)); // Normal rounding. Need to be fixed.
            break;
        }
        break;
      }
      default:
        DEST_I = (i64)round((SRC_FBV)); // Normal rounding. Need to be fixed.
        break;
    }
//    DEST_I = (i64) (SRC_FBV); // The casting generates incorrect output
    EMULATE_FLTI_EPILOGUE();
}

static void
emulate_jmpjsr(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("jmpjsr");
    emu_state->taken_branch = 1;
    DEST_I = as->npc + 4;
}

static void
emulate_fpti_sextw(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fpti_sextw");
    unsigned word_val = SRC_RBV_OP & 0xffff;
    DEST_I = SEXT_TO_i64(word_val, 16);
}

static void
emulate_fpti_sextb(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fpti_sextb");
    unsigned byte_val = SRC_RBV_OP & 0xff;
    DEST_I = SEXT_TO_i64(byte_val, 8);
}

static void
emulate_fpti_maxub8(EMULATE_ARGS_DECL)   
{
    COVERAGE_EMULATE("fpti_maxub8");
    u64 ra_tmp,rb_tmp,rc_tmp;
    unsigned char c1,c2;
    int i;
  
    DEST_I=0;
    ra_tmp=SRC_RAV;
    rb_tmp=SRC_RBV;
    
    for(i=0;i<=7;i++)
    {
        c1 = ra_tmp;
        c2 = rb_tmp;
        rc_tmp = 0;
        rc_tmp = c1>c2 ? c1:c2;
        rc_tmp = rc_tmp << (8*i);
        DEST_I = DEST_I | rc_tmp;
        ra_tmp = ra_tmp >> (8*(i+1));
        rb_tmp = rb_tmp >> (8*(i+1));
    }
}

static void
emulate_fpti_maxsb8(EMULATE_ARGS_DECL)  
{
    COVERAGE_EMULATE("fpti_maxsb8");
    i64 ra_tmp,rb_tmp,rc_tmp;
    char c1,c2;
    int i;
  
    DEST_I=0;
    ra_tmp=SRC_RAV;
    rb_tmp=SRC_RBV;
    
    for(i=0;i<=7;i++)
    {
        c1 = ra_tmp;
        c2 = rb_tmp;
        rc_tmp = 0;
        rc_tmp = c1>c2 ? c1:c2;
        rc_tmp = rc_tmp << (8*i);
        DEST_I = DEST_I | rc_tmp;
        ra_tmp = ra_tmp >> (8*(i+1));
        rb_tmp = rb_tmp >> (8*(i+1));
    }
}


static void
emulate_fpti_maxsw4(EMULATE_ARGS_DECL) 
{
    COVERAGE_EMULATE("fpti_maxsw4");
    i64 ra_tmp,rb_tmp,rc_tmp;
    short int c1,c2;
    int i;
  
    DEST_I=0;
    ra_tmp=SRC_RAV;
    rb_tmp=SRC_RBV;
    
    for(i=0;i<=3;i++)
    {
        c1 = ra_tmp;
        c2 = rb_tmp;
        rc_tmp = 0;
        rc_tmp = c1>c2 ? c1:c2;
        rc_tmp = rc_tmp << (16*i);
        DEST_I = DEST_I | rc_tmp;
        ra_tmp = ra_tmp >> (16*(i+1));
        rb_tmp = rb_tmp >> (16*(i+1));
    }
}

static void
emulate_fpti_minsw4(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fpti_minsw4");
    i64 ra_tmp,rb_tmp,rc_tmp;
    short int c1,c2;
    int i;
  
    DEST_I=0;
    ra_tmp=SRC_RAV;
    rb_tmp=SRC_RBV;
    
    for(i=0;i<=3;i++)
    {
        c1 = ra_tmp;
        c2 = rb_tmp;
        rc_tmp = 0;
        rc_tmp = c1<c2 ? c1:c2;
        rc_tmp = rc_tmp << (16*i);
        DEST_I = DEST_I | rc_tmp;
        ra_tmp = ra_tmp >> (16*(i+1));
        rb_tmp = rb_tmp >> (16*(i+1));
    }
}

static void
emulate_fpti_minuw4(EMULATE_ARGS_DECL)   
{
    COVERAGE_EMULATE("fpti_minuw4");
    u64 ra_tmp,rb_tmp,rc_tmp;
    unsigned short int c1,c2;
    int i;
  
    DEST_I=0;
    ra_tmp=SRC_RAV;
    rb_tmp=SRC_RBV;
    
    for(i=0;i<=3;i++)
    {
        c1 = ra_tmp;
        c2 = rb_tmp;
        rc_tmp = 0;
        rc_tmp = c1<c2 ? c1:c2;
        rc_tmp = rc_tmp << (16*i);
        DEST_I = DEST_I | rc_tmp;
        ra_tmp = ra_tmp >> (16*(i+1));
        rb_tmp = rb_tmp >> (16*(i+1));
    }
}

static void
emulate_fpti_ftoit(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fpti_ftoit");
    DEST_I = SRC_FAV_I;
}

static void
emulate_fpti_ftois(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fpti_ftois");
    i64 fav = SRC_FAV_I;
    // Rc<63:32> <- SEXT(Fav<63>)
    // Rc<31:0>  <- Fav<63:62> . Fav<58:29>
    // width:       2            30
    // offset:      30           0
    DEST_I = (((fav < 0) ? I64_LIT(-1) : 0) << 32) |
        (GET_BITS_IDX(fav, 63, 62) << 30) |
        GET_BITS_IDX(fav, 58, 29);
}

static void
emulate_lds(EMULATE_ARGS_DECL)
{
    i64 longhold, e1, e2;

    longhold = SRC_MEM32;
    e1 = longhold & 0x40000000;
    e2 = (longhold>>23) & 0x7f;
    if (e1) {
        if (e2 == 0x3f800000)
            e2 = 0x7ff;
        else
            e2 |= 0x400;
    } else {
        if (e2 == 0)
            e2 = 0;
        else
            e2 |= 0x380;
    }
    DEST_I = (longhold & 0x80000000)<<32 |
        e2<<52 | (longhold & 0x7fffff)<<29;
/*    COVERAGE_EMULATE("lds");
    // M is a 32-bit word from memory (c.f. emulate_itfp_itofs)
    //   Fc <---- M<31> . MAP_S(M<30:23>) . M<22:0> . 0<28:0>
    //   width:   1       11                23        29
    //   offset:  63      52                29        0
    u64 longhold = SRC_MEM32;
    longhold = (GET_BITS_IDX(longhold, 31, 31) << 63) |
        (((u64) map_s(GET_BITS_IDX(longhold, 30, 23))) << 52) |
        (GET_BITS_IDX(longhold, 22, 0) << 29);
    DEST_I = longhold;*/
}

static void
emulate_ldt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldt");
    DEST_I = SRC_MEM64;
}

static void
emulate_sts(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("sts");
    // M is a 32-bit word in memory
    //   M <----  Fav<63:62> . Fav<58:29>
    //   width:   2            30
    //   offset:  30           0
    u64 longhold = SRC_FAV_I;
    longhold = (GET_BITS_IDX(longhold, 63, 62) << 30) |
        GET_BITS_IDX(longhold, 58, 29);
    SET_MEM32(longhold);
}

static void
emulate_stt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stt");
    SET_MEM64(SRC_FAV_I);
}

static void
emulate_ldl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldl");
    DEST_I = SEXT32_i64(SRC_MEM32);
}

static void
emulate_ldwu(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldwu");
    DEST_I = SRC_MEM16;
}

static void
emulate_ldbu(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldbu");
    DEST_I = SRC_MEM8;
}

static void
emulate_ldq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldq");
    DEST_I = SRC_MEM64;
}


/*ldl_l and ldq_l */
static void
emulate_ldlq_l(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ldlq_l");
}


static void
emulate_stl(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stl");
    SET_MEM32(SRC_RAV & U32_MAX);
}
 
static void
emulate_stq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stq");
    SET_MEM64(SRC_RAV);
}
 
static void
emulate_stb(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stb");
    SET_MEM8(SRC_RAV & 0xff);
}
 
static void
emulate_stw(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stw");
    SET_MEM16(SRC_RAV & 0xffff);
}


/* stl_c and stq_c */
static void
emulate_stlq_c(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stlq_c");
}


/* For single-thread execution, I prefer to use versions of
   load-locked and store-conditional that always succeed.  It
   simplifies a few things, and avoids an occasional
   annoying bug.
*/
static void
emulate_stl_c_uni(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stl_c_uni");
    SET_MEM32(SRC_RAV & U32_MAX);
    DEST_I = 1;
}


static void
emulate_stq_c_uni(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("stq_c_uni");
    SET_MEM64(SRC_RAV);
    DEST_I = 1;
}


static void
emulate_bsr(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("bsr");
    emu_state->taken_branch = 1;
    DEST_I = as->npc + 4;
}

static void
emulate_br(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("br");
    emu_state->taken_branch = 1;
    DEST_I = as->npc + 4;
}

static void
emulate_fbeq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fbeq");
    emu_state->taken_branch = (SRC_FAV == 0.0);
}

static void
emulate_fblt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fblt");
    emu_state->taken_branch = (SRC_FAV < 0.0);
}

static void
emulate_fble(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fble");
    emu_state->taken_branch = (SRC_FAV <= 0.0);
}

static void
emulate_fbne(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fbne");
    emu_state->taken_branch = (SRC_FAV != 0.0);
}

static void
emulate_fbge(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fbge");
    emu_state->taken_branch = (SRC_FAV >= 0.0);
}

static void
emulate_fbgt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("fbgt");
    emu_state->taken_branch = (SRC_FAV > 0.0);
}

static void
emulate_blbc(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("blbc");
    emu_state->taken_branch = (!(SRC_RAV & 1));
}

static void
emulate_beq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("beq");
    emu_state->taken_branch = (SRC_RAV == 0);
}

static void
emulate_blt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("blt");
    emu_state->taken_branch = (SRC_RAV < 0);
}

static void
emulate_ble(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("ble");
    emu_state->taken_branch = (SRC_RAV <= 0);
}

static void
emulate_blbs(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("blbs");
    emu_state->taken_branch = ((SRC_RAV & 1) != 0);
}

static void
emulate_bne(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("bne");
    emu_state->taken_branch = (SRC_RAV != 0) ;
}

static void
emulate_bge(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("bge");
    emu_state->taken_branch = (SRC_RAV >= 0);
}

static void
emulate_bgt(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("bgt");
    emu_state->taken_branch = (SRC_RAV > 0);
}

static void
emulate_call_pal_callsys(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("call_pal_callsys");
    if (WILL_COMMIT) {
        sim_assert(!as->exit.has_exit);
        syscalls_dosyscall(as, cyc);
        if (as->exit.has_exit) {
            // This app just syscalled exit.  (We must not emulate it further.)
            workq_app_sysexit(GlobalWorkQueue, as);
        }
    }
}

static void
emulate_call_pal_rduniq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("call_pal_rduniq");
    DEST_I = SRC_RAV;
}

static void
emulate_call_pal_wruniq(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("call_pal_wruniq");
    DEST_I = SRC_RAV;
}


static void
emulate_mb(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("mb");
}


static void
emulate_wmb(EMULATE_ARGS_DECL)
{
    COVERAGE_EMULATE("wmb");
}


/* I do my best not to decode an instruction (by decode, I
   mean collect all of the static information I possibly
   can about an instruction, including pointers to the
   operands, pointer to the emulation routine, and much
   more) more than once.  Instead, I put everything in
   these stash structures and try and keep track of them.
*/

// This initializes all fields of a StashData object, computing them based ONLY
// on the machine instruction at "pc".  The result from this may be re-used,
// so it must not compute anything that varies dynamically, or cause any other
// side effects.
// (This knows a bit about timing, which is shady for something in this module,
// but nothing here actually consumes the timing values and it's just a 
// convenient place to fill them in.)
int decode_inst(AppState * restrict as,
                struct StashData * restrict st, mem_addr pc)
{
    u32 inst;
    unsigned int ra, rb, rc;
    unsigned int fa, fb, fc;
  
    if (SP_F(((pc & 3) != 0) || !pmem_access_ok(as->pmem, pc, 4, PMAF_RX)))
        goto fail_inst_unreadable;

    inst = pmem_read_32(as->pmem, pc, PMAF_RX); 
    ra = INST_RA(inst);
    rb = INST_RB(inst);
    rc = INST_RC(inst);
    fa = FP_REG(ra);
    fb = FP_REG(rb);
    fc = FP_REG(rc);

    st->src_a = st->src_b = st->dest = IZERO_REG;
    st->immed_byte = -1;
    st->inst = inst;
    st->br_flags = SBF_NotABranch;
    st->gen_flags = SGF_None;
    st->mem_flags = SMF_NoMemOp;
    st->emulate = NULL;
    st->whichfu = INTEGER;
    st->delay_class = SDC_int_arith;
    st->syncop = 0;
    st->regaccs = 3;

    switch (INST_OPCODE(inst)) {
    case LDA:
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->emulate = emulate_lda;
        break;
    case LDAH:
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->emulate = emulate_ldah;
        break;
    case LDQ_U:
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->delay_class = SDC_int_load;
        st->emulate = emulate_ldq_u;
        st->mem_flags = SMF_Read | SMF_Width(8) | SMF_AlignToQuad;
        break;    
    case STB:
        st->src_a = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_stb;
        st->mem_flags = SMF_Write | SMF_Width(1);
        st->delay_class = SDC_int_store;
        break;    
    case STW:
        st->src_a = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_stw;
        st->mem_flags = SMF_Write | SMF_Width(2);
        st->delay_class = SDC_int_store;
        break;    
    case STQ_U:
        st->src_a = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_stq_u;
        st->mem_flags = SMF_Write | SMF_Width(8) | SMF_AlignToQuad;
        st->delay_class = SDC_int_store;
        break;    
    case INTA:
        if (INST_RB_IMMFLAG(inst)) {
            st->immed_byte = INST_RB_IMMED(inst);
            st->regaccs = 2;
        } else {
            st->src_b = rb;
            st->regaccs = 3;
        }
        st->dest = rc;
        st->src_a = ra;

        switch (INST_INTOP_FUNC(inst)) {
        case ADDL:
        case ADDLV:
            st->emulate = emulate_inta_addl;
            break;
        case ADDQ:
        case ADDQV:
            st->emulate = emulate_inta_addq;
            break;
        case CMPLE:
            st->emulate = emulate_inta_cmple;
            st->delay_class = SDC_int_compare;
            break;
        case CMPULT:
            st->emulate = emulate_inta_cmpult;
            st->delay_class = SDC_int_compare;
            break;
        case S4ADDL:
            st->emulate = emulate_inta_s4addl;
            break;
        case S4SUBQ:
            st->emulate = emulate_inta_s4subq;
            break;
        case S8SUBL:
            st->emulate = emulate_inta_s8subl;
            break;
        case CMPBGE:
            st->emulate = emulate_inta_cmpbge;
            st->delay_class = SDC_int_compare;
            break;
        case CMPLT:
            st->emulate = emulate_inta_cmplt;
            st->delay_class = SDC_int_compare;
            break;
        case SUBL:
        case SUBLV:
            st->emulate = emulate_inta_subl;
            break;
        case SUBQ:
        case SUBQV:
            st->emulate = emulate_inta_subq;
            break;
        case S4ADDQ:
            st->emulate = emulate_inta_s4addq;
            break;
        case S8ADDL:
            st->emulate = emulate_inta_s8addl;
            break;
        case S8SUBQ:
            st->emulate = emulate_inta_s8subq;
            break;
        case CMPEQ:
            st->emulate = emulate_inta_cmpeq;
            st->delay_class = SDC_int_compare;
            break;
        case CMPULE:
            st->emulate = emulate_inta_cmpule;
            st->delay_class = SDC_int_compare;
            break;
        case S4SUBL:
            st->emulate = emulate_inta_s4subl;
            break;
        case S8ADDQ:
            st->emulate = emulate_inta_s8addq;
            break;
        default:
            st->emulate = emulate_invalid_instruction;
            break;
        }
        break;
    case INTL:
        if (INST_RB_IMMFLAG(inst)) {
            st->immed_byte = INST_RB_IMMED(inst);
            st->regaccs = 2;
        } else {
            st->src_b = rb;
            st->regaccs = 3;
        }
        st->dest = rc;
        st->src_a = ra;

        switch (INST_INTOP_FUNC(inst)) {
        case AND:
            st->emulate = emulate_intl_and;
            break;
        case BIC:
            st->emulate = emulate_intl_bic;
            break;
        case BIS:
            switch (inst) {
            case SMT_FORK:
                st->gen_flags = SGF_SmtPrimitive;
                st->emulate = emulate_intl_bis_smt_fork;
                st->delay_class = SDC_smt_fork;
                break;
            case SMT_GETID:
                st->gen_flags = SGF_SmtPrimitive;
                st->emulate = emulate_intl_bis_smt_getid;
                break;
            case SMT_CREATESTACK:
                st->gen_flags = SGF_SmtPrimitive;
                st->emulate = emulate_intl_bis_smt_createstack;
                break;
            case SMT_HW_LOCK:
                st->gen_flags = SGF_SmtPrimitive | SGF_SyncAtCommit;
                st->syncop = SMT_HW_LOCK;
                st->whichfu = SYNCH;
                st->src_a = 2;
                st->emulate = emulate_intl_bis_smt_hw_lock;
                st->mem_flags = SMF_Read | SMF_Write | SMF_Width(4) |
                    SMF_SmtLockRel;
                st->delay_class = SDC_smt_lockrel;
                break;
            case SMT_RELEASE:
                st->gen_flags = SGF_SmtPrimitive | SGF_SyncAtCommit;
                st->whichfu = SYNCH;
                st->syncop = SMT_RELEASE;
                st->src_a = 2;
                st->emulate = emulate_intl_bis_smt_release;
                st->mem_flags = SMF_Write | SMF_Width(4) | SMF_SmtLockRel;
                st->delay_class = SDC_smt_lockrel;
                break;
            case SMT_TERMINATE:
                st->gen_flags = SGF_SmtPrimitive;
                st->whichfu = SYNCH;
                st->syncop = SMT_TERMINATE;
                st->emulate = emulate_intl_bis_smt_terminate;
                st->delay_class = SDC_smt_terminate;
                break;
            case SMT_START_SIM:
                st->emulate = emulate_intl_bis_smt_start_sim;
                st->gen_flags = SGF_SmtPrimitive;
                break;
            case SMT_PRINT_SIM:
                st->gen_flags = SGF_SmtPrimitive;
                st->emulate = emulate_intl_bis_smt_print_sim;
                break;
            case SMT_END_SIM:
                st->emulate = emulate_intl_bis_smt_end_sim;
                st->gen_flags = SGF_SmtPrimitive;
                break;
            default:
                st->emulate = emulate_intl_bis;
                break;
            }
            break;
        case CMOVEQ:
            st->emulate = emulate_intl_cmoveq;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case CMOVLBC:
            st->emulate = emulate_intl_cmovlbc;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case CMOVLBS:
            st->emulate = emulate_intl_cmovlbs;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case CMOVGE:
            st->emulate = emulate_intl_cmovge;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case CMOVGT:
            st->emulate = emulate_intl_cmovgt;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case CMOVLE:
            st->emulate = emulate_intl_cmovle;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case CMOVLT:
            st->emulate = emulate_intl_cmovlt;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case CMOVNE:
            st->emulate = emulate_intl_cmovne;
            st->gen_flags = SGF_CondWrite;
            st->delay_class = SDC_int_condmove;
            break;
        case EQV:
            st->emulate = emulate_intl_eqv;
            break;
        case ORNOT:
            st->emulate = emulate_intl_ornot;
            break;
        case XOR:
            st->emulate = emulate_intl_xor;
            break;
        case AMASK:
            st->emulate = emulate_intl_amask;
            break;
        case IMPLVER:
            st->emulate = emulate_intl_implver;
            break;
        default:
            st->emulate = emulate_invalid_instruction;
            break;
        }
        break;
    case INTS:
        if (INST_RB_IMMFLAG(inst)) {
            st->immed_byte = INST_RB_IMMED(inst);
            st->regaccs = 2;
        } else {
            st->src_b = rb;
            st->regaccs = 3;
        }
        st->dest = rc;
        st->src_a = ra;

        switch (INST_INTOP_FUNC(inst)) {
        case EXTBL:
            st->emulate = emulate_ints_extbl;
            break;
        case EXTLH:
            st->emulate = emulate_ints_extlh;
            break;
        case EXTLL:
            st->emulate = emulate_ints_extll;
            break;
        case EXTQH:
            st->emulate = emulate_ints_extqh;
            break;
        case EXTQL:
            st->emulate = emulate_ints_extql;
            break;
        case EXTWH:
            st->emulate = emulate_ints_extwh;
            break;
        case EXTWL:
            st->emulate = emulate_ints_extwl;
            break;
        case INSBL:
            st->emulate = emulate_ints_insbl;
            break;
        case INSLH:
            st->emulate = emulate_ints_inslh;
            break;
        case INSLL:
            st->emulate = emulate_ints_insll;
            break;
        case INSQH:
            st->emulate = emulate_ints_insqh;
            break;
        case INSQL:
            st->emulate = emulate_ints_insql;
            break;
        case INSWH:
            st->emulate = emulate_ints_inswh;
            break;
        case INSWL:
            st->emulate = emulate_ints_inswl;
            break;
        case MSKBL:
            st->emulate = emulate_ints_mskbl;
            break;
        case MSKLH:
            st->emulate = emulate_ints_msklh;
            break;
        case MSKLL:
            st->emulate = emulate_ints_mskll;
            break;
        case MSKQH:
            st->emulate = emulate_ints_mskqh;
            break;
        case MSKQL:
            st->emulate = emulate_ints_mskql;
            break;
        case MSKWH:
            st->emulate = emulate_ints_mskwh;
            break;
        case MSKWL:
            st->emulate = emulate_ints_mskwl;
            break;
        case SLL:
            st->emulate = emulate_ints_sll;
            break;
        case SRA:
            st->emulate = emulate_ints_sra;
            break;          
        case SRL:
            st->emulate = emulate_ints_srl;
            break;
        case ZAP:
            st->emulate = emulate_ints_zap;
            break;
        case ZAPNOT:
            st->emulate = emulate_ints_zapnot;
            break;
        default:
            st->emulate = emulate_invalid_instruction;
            break;
        }
        break;
    case FPTI:
    {
        int int_func = INST_INTOP_FUNC(inst);
        int fp_func = INST_FLTL_FUNC(inst);

        if (INST_RB_IMMFLAG(inst)) {
            st->immed_byte = INST_RB_IMMED(inst); st->regaccs = 1;
        } else {
            st->src_b = rb;
            st->regaccs = 2;
        }
        st->dest = rc;

        st->emulate = emulate_unimplemented_instruction;

        switch (int_func) {
        case SEXTB:
            st->emulate = emulate_fpti_sextb;
            break;
        case SEXTW:
            st->emulate = emulate_fpti_sextw;
            break;
        case MAXUB8:          
            st->src_a = ra;
            st->src_b = rb;
            st->immed_byte = -1;
            st->regaccs = 3;
            st->emulate = emulate_fpti_maxub8;
            break;
        case MAXSB8:          
            st->src_a = ra;
            st->src_b = rb;
            st->immed_byte = -1;
            st->regaccs = 3;
            st->emulate = emulate_fpti_maxsb8;
            break;
        case MAXSW4:         
            st->src_a = ra;
            st->src_b = rb;
            st->immed_byte = -1;
            st->regaccs = 3;
            st->emulate = emulate_fpti_maxsw4;
            break;
        case MINSW4:        
            st->src_a = ra;
            st->src_b = rb;
            st->immed_byte = -1;
            st->regaccs = 3;
            st->emulate = emulate_fpti_minsw4;
            break;
        case MINUW4:
            st->src_a = ra;
            st->src_b = rb;
            st->immed_byte = -1;
            st->regaccs = 3;
            st->emulate = emulate_fpti_minuw4;
            break;
        case FTOIT:
        case FTOIS:
            // Cruft: if the high bits of the 11-bit FP function field aren't
            // 0, then the function# depends on whether we view the format
            // as integer operate or FP operate.  It's not clear what to do
            // in that case, so we'll pretend we don't recognize it.
            if (fp_func == int_func) {
                st->src_a = fa;
                st->src_b = fb;
                st->regaccs = 3;
                st->immed_byte = -1;
                st->whichfu = FP;
                st->delay_class = SDC_ftoi;
                st->emulate = (int_func == FTOIT) ? emulate_fpti_ftoit : 
                    emulate_fpti_ftois;
            }
            break;
        }
        break;
    }
    case INTM:
        if (INST_RB_IMMFLAG(inst)) {
            st->immed_byte = INST_RB_IMMED(inst);
            st->regaccs = 2;
        } else {
            st->src_b = rb; 
            st->regaccs = 3;
        }
        st->dest = rc;
        st->src_a = ra;

        switch (INST_INTOP_FUNC(inst)) {
        case MULL:
        case MULLV:
            st->emulate = emulate_intm_mull;
            st->delay_class = SDC_int_mull;
            break;
        case MULQ:
        case MULQV:
            st->emulate = emulate_intm_mulq;
            st->delay_class = SDC_int_mulq;
            break;
        case UMULH:
            st->emulate = emulate_intm_umulh;
            st->delay_class = SDC_int_umulh;
            break;
        default:
            st->emulate = emulate_unimplemented_instruction;
            break;
        }
        break;
    case ITFP:
        st->dest = fc;
        st->src_a = fa;
        st->src_b = fb;
        st->regaccs = 3;
        st->whichfu = FP;
        st->delay_class = SDC_itof;

        switch (INST_FLTL_FUNC(inst)) {
        case ITOFS:
            st->src_a = ra;
            st->emulate = emulate_itfp_itofs;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case ITOFF:
            st->src_a = ra;
            st->emulate = emulate_itfp_itoff;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case ITOFT:
            st->src_a = ra;
            st->emulate = emulate_itfp_itoft;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case SQRTS:
            st->delay_class = SDC_fp_divs;
            st->emulate = emulate_itfp_sqrts;
            break;
        case SQRTT:
            st->delay_class = SDC_fp_divt;
            st->emulate = emulate_itfp_sqrtt;
            break;
        default:
            st->emulate = emulate_unimplemented_instruction;
        }
        break;
    case FLTL:
        // Non-IEEE ("indepdendent") floating-point operations
        st->dest = fc;
        st->src_a = fa;
        st->src_b = fb;
        st->regaccs = 3;
        st->whichfu = FP;
        st->delay_class = SDC_fp_bit;

        switch (INST_FLTL_FUNC(inst)) {
        case CPYS:
            st->emulate = emulate_fltl_cpys;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case CPYSE:
            st->emulate = emulate_fltl_cpyse;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case CPYSN:
            st->emulate = emulate_fltl_cpysn;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case CVTLQ:
            st->src_a = IZERO_REG;
            st->emulate = emulate_fltl_cvtlq;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case CVTQL:
        case CVTQLV:
        case CVTQLSV:
            // These are really all the same operation, "CVTQL".  Unlike the
            // other FLTL operations, CVTQL allows the "/V" and "/SV" trap
            // qualifiers.  Rather than decode them seperately (like we do
            // with the FLTI opcode), we'll match them directly and let the
            // emulate routine worry about it.
            st->src_a = IZERO_REG;
            st->emulate = emulate_fltl_cvtql;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case FCMOVEQ:
            /* if Rb = f31, this is a special synch operation, READ_IF_FULL */
            st->emulate = emulate_fltl_fcmoveq;
            st->gen_flags = SGF_CondWrite | SGF_FPLiteralWrite;
            st->delay_class = SDC_fp_condmove;
            break;
        case FCMOVGE:
            st->emulate = emulate_fltl_fcmovge;
            st->gen_flags = SGF_CondWrite | SGF_FPLiteralWrite;
            st->delay_class = SDC_fp_condmove;
            break;
        case FCMOVGT:
            st->emulate = emulate_fltl_fcmovgt;
            st->gen_flags = SGF_CondWrite | SGF_FPLiteralWrite;
            st->delay_class = SDC_fp_condmove;
            break;
        case FCMOVLE:
            st->emulate = emulate_fltl_fcmovle;
            st->gen_flags = SGF_CondWrite | SGF_FPLiteralWrite;
            st->delay_class = SDC_fp_condmove;
            break;
        case FCMOVLT:
            st->emulate = emulate_fltl_fcmovlt;
            st->gen_flags = SGF_CondWrite | SGF_FPLiteralWrite;
            st->delay_class = SDC_fp_condmove;
            break;
        case FCMOVNE:
            st->emulate = emulate_fltl_fcmovne;
            st->gen_flags = SGF_CondWrite | SGF_FPLiteralWrite;
            st->delay_class = SDC_fp_condmove;
            break;
        case MT_FPCR:
            st->src_a = fa;
            st->src_b = IZERO_REG;
            st->dest = FPCR_REG;
            st->emulate = emulate_fltl_mt_fpcr;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        case MF_FPCR:
            st->src_a = FPCR_REG;
            st->src_b = IZERO_REG;
            st->dest = fa;
            st->emulate = emulate_fltl_mf_fpcr;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        default:
            st->emulate = emulate_invalid_instruction;
            break;
        }
        break;
    case FLTI:
        // IEEE floating-point instructions
        st->dest = fc;
        st->src_a = fa;
        st->src_b = fb;
        st->regaccs = 3;
        st->whichfu = FP;
        st->delay_class = SDC_fp_arith;

        switch (INST_FLTI_SRCFNC(inst)) {
        case ADDS:
            st->emulate = emulate_flti_adds;
            break;
        case ADDT:
            st->emulate = emulate_flti_addt;
            break;
        case CMPTEQ:
            st->emulate = emulate_flti_cmpteq;
            st->delay_class = SDC_fp_compare;
            break;
        case CMPTLT:
            st->emulate = emulate_flti_cmptlt;
            st->delay_class = SDC_fp_compare;
            break;
        case CMPTLE:
            st->emulate = emulate_flti_cmptle;
            st->delay_class = SDC_fp_compare;
            break;
        case CMPTUN:
            // Added by Hung-Wei
            st->emulate = emulate_flti_cmptun;
            st->delay_class = SDC_fp_compare;
            break;
        case CVTQS:
            st->src_a = IZERO_REG;
            st->emulate = emulate_flti_cvtqs;
            break;
        case CVTQT:
            st->src_a = IZERO_REG;
            st->emulate = emulate_flti_cvtqt;
            break;
        case CVTTS:
            st->src_a = IZERO_REG;
            if(INST_FLTI_TRAP(st->inst)==2||INST_FLTI_TRAP(st->inst)==6)
            {
              st->emulate = emulate_flti_cvtst;
//              fprintf(stderr,"Value: %lf\n",as->R[st->src_b].f);
            }
            else
              st->emulate = emulate_flti_cvtts;
            break;
        case DIVS: 
            st->gen_flags |= SGF_DetermVarDelay;
            st->emulate = emulate_flti_divs;
            st->delay_class = SDC_fp_divs;
            break;
        case DIVT:
            st->gen_flags |= SGF_DetermVarDelay;
            st->emulate = emulate_flti_divt;
            st->delay_class = SDC_fp_divt;
            break;
        case MULS:
            st->emulate = emulate_flti_muls;
            break;
        case MULT:
            st->emulate = emulate_flti_mult;
            break;
        case SUBS:
            st->emulate = emulate_flti_subs;
            break;
        case SUBT:
            st->emulate = emulate_flti_subt;
            break;
        case CVTTQ:
            st->src_a = FZERO_REG;
            st->emulate = emulate_flti_cvttq;
            st->gen_flags = SGF_FPLiteralWrite;
            break;
        default:
            st->emulate = emulate_unimplemented_instruction;
            break;
        }
        break;
    case FLTV:
        st->emulate = emulate_unimplemented_instruction;
        break;
    case MISC:
        switch (INST_MEM_FUNC(inst)) {
        case TRAPB:
            st->emulate = emulate_nop;
            st->regaccs = 0;
            break;
        case MB:
            st->regaccs = 0;
            if (SIMULATE_MEM_BARRIERS) {
                st->emulate = emulate_mb;
                st->syncop = MB;
            } else {
                st->emulate = emulate_nop;
            }
            break;
        case WMB:
            st->regaccs = 0;
            if (SIMULATE_MEM_BARRIERS) {
                st->emulate = emulate_wmb;
                st->syncop = WMB;
            } else {
                st->emulate = emulate_nop;
            }
            break;
        case EXCB:
            st->regaccs = 0;
            // modified by Hung-Wei
            st->emulate = emulate_nop;
            break;
        case FETCH:
        case FETCH_M:
        case RC:
        case RS:
            st->emulate = emulate_unimplemented_instruction;
            break;
        case RPCC:
            // modified by Hung-Wei
            st->regaccs = 1;
            st->dest = ra;
            st->whichfu = INTLDST;
            st->emulate = emulate_rpcc;
            break;
        case WH64:
            st->regaccs = 0;
            st->whichfu = INTLDST;
            st->emulate = emulate_nop;
            break;
        default:
            st->emulate = emulate_invalid_instruction;
            break;
        }
        break;
    case JMPJSR:
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Uncond;
        st->emulate = emulate_jmpjsr;
        st->delay_class = SDC_uncond_br;

        switch (INST_JMPJSR_TYPE(inst)) {
        case JMP:
            break;
        case JSR:
            st->br_flags |= SBF_RS_Push;
            break;
        case RETN:
            st->br_flags |= SBF_RS_Pop;
            break;
        case JSR_COROUTINE:
            st->br_flags |= SBF_RS_PopPush;
            break;
        }
        break;
    case LDF:
        st->delay_class = SDC_fp_load;
        st->emulate = emulate_unimplemented_instruction;
        st->mem_flags = SMF_Read | SMF_Width(4);
        break;
    case LDS:
        st->delay_class = SDC_fp_load;
        st->dest = fa;
        st->src_b = rb; 
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_lds;
        st->mem_flags = SMF_Read | SMF_Width(4);
        break;  
    case LDG:
        st->delay_class = SDC_fp_load;
        st->emulate = emulate_unimplemented_instruction;
        st->mem_flags = SMF_Read | SMF_Width(8);
        break;
    case LDT:
        st->delay_class = SDC_fp_load;
        st->dest = fa;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_ldt;
        st->mem_flags = SMF_Read | SMF_Width(8);
        break;    
    case STF:
        st->delay_class = SDC_fp_store;
        st->emulate = emulate_unimplemented_instruction;
        st->mem_flags = SMF_Write | SMF_Width(4);
        break;
    case STS:
        st->delay_class = SDC_fp_store;
        st->src_a = fa;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_sts;
        st->mem_flags = SMF_Write | SMF_Width(4);
        break;
    case STG:
        st->delay_class = SDC_fp_store;
        st->emulate = emulate_unimplemented_instruction;
        st->mem_flags = SMF_Write | SMF_Width(8);
        break;
    case STT:
        st->delay_class = SDC_fp_store;
        st->src_a = fa;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_stt;
        st->mem_flags = SMF_Write | SMF_Width(8);
        break;
    case LDL:
        st->delay_class = SDC_int_load;
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_ldl;
        st->mem_flags = SMF_Read | SMF_Width(4);
        break;    
    case LDWU:
        st->delay_class = SDC_int_load;
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_ldwu;
        st->mem_flags = SMF_Read | SMF_Width(2);
        break;    
    case LDBU:
        st->delay_class = SDC_int_load;
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_ldbu;
        st->mem_flags = SMF_Read | SMF_Width(1);
        break;    
    case LDQ:
        st->delay_class = SDC_int_load;
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_ldq;
        st->mem_flags = SMF_Read | SMF_Width(8);
        break;
    case LDL_L:
        st->delay_class = SDC_int_load;
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        if (PARCODE) {
            st->gen_flags = SGF_SmtPrimitive | SGF_SyncAtCommit;
            st->syncop = LDL_L;
            st->whichfu = SYNCH;
            st->emulate = emulate_ldlq_l;
        } else {
            st->whichfu = INTLDST;
            st->emulate = emulate_ldl;
        }
        st->mem_flags = SMF_Read | SMF_Width(4);
        break;
    case LDQ_L:
        st->delay_class = SDC_int_load;
        st->dest = ra;
        st->src_b = rb;
        st->regaccs = 2;
        if (PARCODE) {
            st->gen_flags = SGF_SmtPrimitive | SGF_SyncAtCommit;
            st->syncop = LDQ_L;
            st->whichfu = SYNCH;
            st->emulate = emulate_ldlq_l;
        } else {
            st->whichfu = INTLDST;
            st->emulate = emulate_ldq;
        }
        st->mem_flags = SMF_Read | SMF_Width(8);
        break;
    case STL:
        st->delay_class = SDC_int_store;
        st->src_a = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_stl;
        st->mem_flags = SMF_Write | SMF_Width(4);
        break;    
    case STQ:
        st->delay_class = SDC_int_store;
        st->src_a = ra;
        st->src_b = rb;
        st->regaccs = 2;
        st->whichfu = INTLDST;
        st->emulate = emulate_stq;
        st->mem_flags = SMF_Write | SMF_Width(8);
        break;    
    case STL_C: 
        st->delay_class = SDC_int_store;
        st->dest = ra;
        st->src_a = ra;
        st->src_b = rb;
        st->regaccs = 3;
        if (PARCODE) {
            st->gen_flags = SGF_SmtPrimitive | SGF_SyncAtCommit;
            st->syncop = STL_C;
            st->whichfu = SYNCH;
            st->emulate = emulate_stlq_c;
            st->mem_flags = SMF_Write | SMF_Width(4);
        } else {
            st->whichfu = INTLDST;
            st->emulate = emulate_stl_c_uni;
            st->mem_flags = SMF_Write | SMF_Width(4);
        }
        break;    
    case STQ_C: 
        st->delay_class = SDC_int_store;
        st->dest = ra;
        st->src_a = ra;
        st->src_b = rb;
        st->regaccs = 3;
        if (PARCODE) {
            st->gen_flags = SGF_SmtPrimitive | SGF_SyncAtCommit;
            st->syncop = STQ_C;
            st->whichfu = SYNCH;
            st->emulate = emulate_stlq_c;
            st->mem_flags = SMF_Write | SMF_Width(8);
        } else {
            st->whichfu = INTLDST;
            st->emulate = emulate_stq_c_uni;
            st->mem_flags = SMF_Write | SMF_Width(8);
        }
        break;    
    case BSR: 
        st->dest = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Uncond | SBF_StaticTargDisp | SBF_RS_Push;
        st->emulate = emulate_bsr;
        st->delay_class = SDC_uncond_br;
        break;
    case BR:
        st->dest = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Uncond | SBF_StaticTargDisp;
        st->emulate = emulate_br;
        st->delay_class = SDC_uncond_br;
        break;
    case FBEQ:
        st->src_a = fa;
        st->regaccs = 1;
        st->whichfu = FP;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_fbeq;
        st->delay_class = SDC_fp_condbr;
        break;
    case FBLT:
        st->src_a = fa;
        st->regaccs = 1;
        st->whichfu = FP;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_fblt;
        st->delay_class = SDC_fp_condbr;
        break;
    case FBLE:
        st->src_a = fa;
        st->regaccs = 1;
        st->whichfu = FP;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_fble;
        st->delay_class = SDC_fp_condbr;
        break;
    case FBNE:
        st->src_a = fa;
        st->regaccs = 1;
        st->whichfu = FP;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_fbne;
        st->delay_class = SDC_fp_condbr;
        break;
    case FBGE:
        st->src_a = fa;
        st->regaccs = 1;
        st->whichfu = FP;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_fbge;
        st->delay_class = SDC_fp_condbr;
        break;
    case FBGT:
        st->src_a = fa;
        st->regaccs = 1;
        st->whichfu = FP;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_fbgt;
        st->delay_class = SDC_fp_condbr;
        break;
    case BLBC:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_blbc;
        st->delay_class = SDC_int_condbr;
        break;
    case BEQ:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_beq;
        st->delay_class = SDC_int_condbr;
        break;
    case BLT:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_blt;
        st->delay_class = SDC_int_condbr;
        break;
    case BLE:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_ble;
        st->delay_class = SDC_int_condbr;
        break;
    case BLBS:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_blbs;
        st->delay_class = SDC_int_condbr;
        break;
    case BNE:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_bne;
        st->delay_class = SDC_int_condbr;
        break;
    case BGE:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_bge;
        st->delay_class = SDC_int_condbr;
        break;
    case BGT:
        st->src_a = ra;
        st->regaccs = 1;
        st->whichfu = INTEGER;
        st->br_flags = SBF_Br_Cond | SBF_StaticTargDisp;
        st->emulate = emulate_bgt;
        st->delay_class = SDC_int_condbr;
        break;
    case CALL_PAL:
        switch (INST_CALLPAL_CODE(inst)) {
        case callsys:
        default:
            st->regaccs = 0;
            st->br_flags = SBF_Br_Uncond;
            st->whichfu = INTEGER;
            st->emulate = emulate_call_pal_callsys;
            st->gen_flags = SGF_PipeExclusive | SGF_SysCall;
            break;
        case rduniq:
            st->src_a = HWUNIQ_REG;
            st->regaccs = 1;
            st->dest = 0;
            st->emulate = emulate_call_pal_rduniq;
            break;
        case wruniq:
            st->src_a = 16;
            st->regaccs = 1;
            st->dest = HWUNIQ_REG;
            st->emulate = emulate_call_pal_wruniq;
            break;
        }
        break;
    default:
        st->emulate = emulate_invalid_instruction;
        break;
    }

    // We're going to cheat, and force operations on f31 to really happen to
    // r31, so we only need to worry about testing for and re-zeroing r31.  We
    // can get away with this, because the bits in "0" are equivalent to "0.0"
    // in IEEE floating-point, which the simulator already depends on having.
    if (st->src_a == FZERO_REG)
        st->src_a = IZERO_REG;
    if (st->src_b == FZERO_REG)
        st->src_b = IZERO_REG;
    if (st->dest == FZERO_REG)
        st->dest = IZERO_REG;

    if (!st->br_flags && IS_ZERO_REG(st->dest) && !st->syncop &&
        !(st->gen_flags & SGF_SmtPrimitive) &&
        ((st->whichfu == INTEGER) ||
         (st->whichfu == INTLDST && !(st->mem_flags & SMF_Write)) ||
         (st->whichfu == FP))) {
        // (No-op test migrated from setup_instruction)
        /*
        ** If destination register for non-branch / non-SMT-synch
        ** instuctions is 31 (integer) or 63 (floating point),
        ** then treat as a no-op since these registers are
        ** always zero.
        */
        st->gen_flags |= SGF_StaticNoop;
    }

    sim_assert(st->emulate != NULL);
    sim_assert(st->delay_class != SDC_all_extra);

    if (SP_F(st->emulate == emulate_invalid_instruction))
        goto fail_invalid_inst;

    // success
    return 0;

 fail_inst_unreadable:
    return -1;
 fail_invalid_inst:
    return -2;
}


void
emulate_inst(AppState * restrict as, const StashData * restrict st,
             EmuInstState * restrict emu_state, int speculative)
{
#if DEBUG_REGS_EMULATE
    reg_u old_dest_reg;
    mem_addr old_pc = as->npc;
    i64 inst_num = as->stats.total_insts;
    old_dest_reg.i = as->R[st->dest].i;
#endif

    if (st->mem_flags) {
        // This code is duplicated in "emu_calc_destmem()"!
        i32 disp = INST_MEM_FUNC(st->inst);
        mem_addr va = as->R[st->src_b].i + SEXT_TO_i64(disp, 16);

        // 1) For most loads/stores, use just Rb + sign-extended offset
        // 2) For LDQ_U / STQ_U, it's #1 with the low three bits cleared
        // 3) For SMT_HW_LOCK / SMT_RELEASE, the address is in R2

        if (st->mem_flags & SMF_AlignToQuad)
            va &= ~ U64_LIT(7);
        else if (st->mem_flags & SMF_SmtLockRel)
            va = as->R[2].i;

        if (st->mem_flags & SMF_Read)
            emu_state->srcmem = va;
        if (st->mem_flags & SMF_Write)
            emu_state->destmem = va;

#ifdef DEBUG
        if (st->mem_flags & SMF_Write) {
            // Make darned sure that emu_calc_destmem works right
            mem_addr test_va = emu_calc_destmem(as, st);
            sim_assert(test_va == va);
        }
#endif
    }

    if (st->br_flags) {
        // calc_br_targ()
        if (SBF_StaticTarget(st->br_flags)) {
            i32 disp = INST_BRANCH_DISP(st->inst);
            disp = (SEXT_TO_i64(disp, 21) << 2) + 4;
            emu_state->br_target = (i64) as->npc + disp;
        } else {
            emu_state->br_target = as->R[st->src_b].i & ~3;
        }
    }

    sim_assert(as->R[IZERO_REG].i == 0);
    sim_assert(as->R[FZERO_REG].f == 0.0);
    sim_assert(!as->exit.has_exit);

    emu_state->taken_branch = 0;
    
    st->emulate(as, st, emu_state, speculative);
//    if((as->npc>=0x12001c300 && as->npc <=0x12001c394) ||(as->npc>=0x120015550 && as->npc <=0x1200156ec) || (as->npc >= 0x20035300 && as->npc <=0x200353e8))
//    {
//      printf("pc: %s inst: %x func: %x trap: %x result: %s src: %s %s\n",fmt_x64(as->npc),INST_OPCODE(st->inst), INST_FLTI_SRCFNC(st->inst),INST_FLTI_TRAP(st->inst),fmt_x64(as->R[st->dest].i),fmt_x64(as->R[st->src_a].i),fmt_x64(as->R[st->src_b].i));
//    }
//    if(as->npc == 0x120020eac || as->npc == 0x200353e8)
//      printf("pc: %s return: %lf\n",fmt_x64(as->npc),as->R[32].f);
//    if((st->mem_flags & SMF_Write) && emu_state->destmem == 0x1202205108)
//      printf("pc: %s store inst: %x func: %x trap: %x result: %s src: %s %s\n",fmt_x64(as->npc),INST_OPCODE(st->inst), INST_FLTI_SRCFNC(st->inst),INST_FLTI_TRAP(st->inst),fmt_x64(as->R[st->dest].i),fmt_x64(as->R[st->src_a].i),fmt_x64(as->R[st->src_b].i));
//    if(as->npc==0x12000f790)
//    {
//      printf("pc: %s result: %s src: %s %s\n",fmt_x64(as->npc),fmt_x64(as->R[st->dest].i),fmt_x64(as->R[st->src_a].i),fmt_x64(as->R[st->src_b].i));
//    }
    as->R[IZERO_REG].i = 0;
    as->npc = (emu_state->taken_branch) ? emu_state->br_target : (as->npc + 4);
    as->stats.total_insts++;

    sim_assert(as->R[IZERO_REG].i == 0);
    sim_assert(as->R[FZERO_REG].f == 0.0);
    sim_assert((emu_state->taken_branch & 1) ==
               emu_state->taken_branch); // 0 or 1

#if DEBUG_REGS_EMULATE
    if (debug) {
        printf("emu inst %s pc %s", fmt_i64(inst_num), fmt_x64(old_pc));
        if (!IS_ZERO_REG(st->dest)) {
            if (IS_FP_REG(st->dest)) {
                printf(", f%d %s -> %s", FP_UNREG(st->dest),
                       fmt_x64(old_dest_reg.i), fmt_x64(as->R[st->dest].i));
            } else {
                printf(", r%d %s -> %s", st->dest, 
                       fmt_x64(old_dest_reg.i), fmt_x64(as->R[st->dest].i));
            }
        }
        if (st->mem_flags & SMF_Read)
            printf(", srcmem %s/%d", fmt_x64(emu_state->srcmem),
                   SMF_GetWidth(st->mem_flags));
        if (st->mem_flags & SMF_Write) {
            printf(", destmem %s/%d src %s%d ",
                   fmt_x64(emu_state->destmem), SMF_GetWidth(st->mem_flags),
                   (IS_FP_REG(st->src_a)) ? "f" : "r",
                   (IS_FP_REG(st->src_a)) ? FP_UNREG(st->src_a) : st->src_a);
            if (IS_FP_REG(st->src_a)) {
                printf("%s", fmt_x64(as->R[st->src_a].i));
            } else {
                printf("%s", fmt_x64(as->R[st->src_a].i));
            }
        }
        if (st->br_flags)
            printf(", br taken %d pc -> %s", 
                   emu_state->taken_branch, fmt_x64(as->npc));
        if (1) {
            printf(", src [");
            if (!IS_ZERO_REG(st->src_a)) {
                printf(" %s%d", (IS_FP_REG(st->src_a)) ? "f" : "r",
                       (IS_FP_REG(st->src_a)) ? FP_UNREG(st->src_a) 
                       : st->src_a);
            }
            if (!IS_ZERO_REG(st->src_b)) {
                printf(" %s%d", (IS_FP_REG(st->src_b)) ? "f" : "r",
                       (IS_FP_REG(st->src_b)) ? FP_UNREG(st->src_b) 
                       : st->src_b);
            }
            printf(" ]");
        }
        printf("\n");
    }
#endif
}


void
fast_forward_app(AppState * restrict as, i64 inst_count)
{
    EmuInstState emu_state;
    Stash * restrict stash = as->stash;
    int insts_in_bb = 1;
    
    sim_assert(as->app_id >= 0);
    
    if (BBTrackerParams.create_bbv_file)
        init_bb_tracker(BBTrackerParams.filename, BBTrackerParams.interval);
    
    for (i64 i = 0; i < inst_count; i++) {
        const StashData * restrict stash_ent =
            stash_decode_inst(stash, as->npc);
        if (SP_F(!stash_ent)) {
            const char *fname = "fast_forward_app";
            err_printf("%s: instruction decode failed, A%d "
                       "inst #%s pc 0x%s\n", fname, as->app_id, fmt_i64(i),
                       fmt_x64(as->npc));
            fprintf(stderr, "ProgMem map:\n");
            pmem_dump_map(as->pmem, stderr, "  ");
            sim_abort();
        }
        /* Create a basic block vector file if requested */
        if (BBTrackerParams.create_bbv_file) {
            if (stash_ent->br_flags != SBF_NotABranch) {
                bb_tracker((long)as->npc, insts_in_bb);
                insts_in_bb = 1;
            } else {
                insts_in_bb++;
            }
        }
        
        emulate_inst(as, stash_ent, &emu_state, 0);
        if (as->exit.has_exit)
            break;
    }
}


mem_addr
emu_calc_destmem(const struct AppState * restrict as,
                 const struct StashData * restrict st)
{
    // This code is copied from emulate_inst() -- make sure they match
    i32 disp = INST_MEM_FUNC(st->inst);
    mem_addr va = as->R[st->src_b].i + SEXT_TO_i64(disp, 16);
    
    sim_assert(st->mem_flags & SMF_Write);

    if (st->mem_flags & SMF_AlignToQuad)
        va &= ~ U64_LIT(7);
    else if (st->mem_flags & SMF_SmtLockRel)
        va = as->R[2].i;

    return va;
}
