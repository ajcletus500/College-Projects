//
// Register number definitions
//
// Jeff Brown
// $Id: reg-defs.h,v 1.1.2.2.6.1.2.6 2009/07/29 10:52:54 jbrown Exp $
//

#ifndef REG_DEFS_H
#define REG_DEFS_H


#ifdef __cplusplus
extern "C" {
#endif


// Register file number assignment: any registers for which dependences
// end up being tracked in a "register-like" way go in the AppState register
// file.  Currently, the layout is:
//   0..31: integer regs
//   32..63: FP regs
//   64: FPCR
//   65: Process unique context value (we call it "HWUNIQ")
#define FPR_BASE        32      // Register array index of first FP register
#define FPR_COUNT       32      // Number of FP regs
#define FP_REG(x)       ((x) + FPR_BASE)
#define FP_UNREG(x)     ((x) - FPR_BASE)
#define IZERO_REG       31      // Integer zero register
#define FZERO_REG       FP_REG(31)      // decode_inst() substitutes IZERO_REG
#define IS_FP_REG(x)    (((x) >= FPR_BASE) && ((x) < (FPR_BASE + FPR_COUNT)))
#define IS_ZERO_REG(x)  ((x) == IZERO_REG)
#define IS_ZERO_REG2(x) (((x) == IZERO_REG) || ((x) == FZERO_REG))
#define FPCR_REG        64      // Index of FPCR reg
#define HWUNIQ_REG      65      // Index of context unique ID reg
#define MAXREG          66      // Size of AppState register file


// emulate.c needs sizeof(i64) == sizeof(double) == sizeof(reg_u); it's checked
typedef union reg_u { i64 i; u64 u; double f; } reg_u;


typedef enum {
    AlphaReg_v0 = 0,
    AlphaReg_t0 = 1,
    AlphaReg_t1 = 2,
    AlphaReg_t2 = 3,
    AlphaReg_t3 = 4,
    AlphaReg_t4 = 5,
    AlphaReg_t5 = 6,
    AlphaReg_t6 = 7,
    AlphaReg_t7 = 8,
    AlphaReg_s0 = 9,
    AlphaReg_s1 = 10,
    AlphaReg_s2 = 11,
    AlphaReg_s3 = 12,
    AlphaReg_s4 = 13,
    AlphaReg_s5 = 14,
    AlphaReg_s6 = 15,       // aliased with fp
    AlphaReg_fp = 15,       // aliased with s6
    AlphaReg_a0 = 16,
    AlphaReg_a1 = 17,
    AlphaReg_a2 = 18,
    AlphaReg_a3 = 19,
    AlphaReg_a4 = 20,
    AlphaReg_a5 = 21,
    AlphaReg_t8 = 22,
    AlphaReg_t9 = 23,
    AlphaReg_t10 = 24,
    AlphaReg_t11 = 25,
    AlphaReg_ra = 26,
    AlphaReg_pv = 27,       // aliased with t12
    AlphaReg_t12 = 27,      // aliased with pv
    AlphaReg_at = 28,
    AlphaReg_gp = 29,
    AlphaReg_sp = 30,
    AlphaReg_zero = 31,
    AlphaIntReg_last
} AlphaIntReg;

extern const char *AlphaIntReg_names[];         // hiding in print.c


#ifdef __cplusplus
}
#endif

#endif  // REG_DEFS_H
