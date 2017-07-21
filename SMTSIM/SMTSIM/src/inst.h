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
     commerical product or service without the expressed written consent of
     James Larus.

   THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.
===============================================================
 */

#ifndef INST_H
#define INST_H

/* Alpha Instructions */
typedef enum {
  CALL_PAL,LDA=8,LDAH,LDBU,LDQ_U=0xb,LDWU,STW,STB=0xe,STQ_U=0xf,
  INTA,INTL,INTS,INTM,ITFP,FLTV,FLTI,FLTL,
  MISC,JMPJSR=0x1a,FPTI=0x1c,
  LDF=0x20,LDG,LDS,LDT,STF,STG,STS,STT,LDL,LDQ,LDL_L,LDQ_L,STL,STQ,STL_C,STQ_C,
  BR,FBEQ,FBLT,FBLE,BSR,FBNE,FBGE,FBGT,BLBC,BEQ,BLT,BLE,BLBS,BNE,BGE,BGT
} opcodefld;

typedef enum { /*mem functions, opcode = 18*/
    TRAPB = 0x0000,
    EXCB = 0x0400,
    MB = 0x4000,
    WMB = 0x4400,
    FETCH = 0x8000,
    FETCH_M = 0xA000,
    RPCC = 0xC000,
    RC = 0xE000,
    RS = 0xF000,
    WH64 = 0xF800
} op18field;

typedef enum {  /* jump instructions, opcode = 1a*/
  JMP, JSR, RETN, JSR_COROUTINE } jmpfield;

typedef enum {  /* FPTI instructions, opcode = 1c*/
  SEXTB, SEXTW, MINSW4=0x39, MINUW4=0x3B, MAXUB8=0x3C, MAXSB8=0x3E, MAXSW4=0x3F, FTOIT=0x70, FTOIS=0x78 } op1cfield;

typedef enum { /*operate, opcode = 10 */
  ADDL=0,ADDQV=0x60,CMPLE=0x6D,CMPULT=0x1D,SUBQ=0x29,S4ADDL=0x2,S4SUBQ=0x2B,
  S8SUBL=0x1B,ADDLV=0x40,CMPBGE=0xf,CMPLT=0x4d,SUBL=0x9,SUBQV=0x69,S4ADDQ=0x22,
  S8ADDL=0x12,S8SUBQ=0x3B,ADDQ=0x20,CMPEQ=0x2D,CMPULE=0x3D,SUBLV=0x49,
  S4SUBL=0xb,S8ADDQ=0x32} op10field;

typedef enum { /* operate, opcode = 11 */
  AND=0,BIC=0x8,BIS=0x20,CMOVEQ=0x24,CMOVLBC=0x16,CMOVLBS=0x14,CMOVGE=0x46,
  AMASK=0x61,IMPLVER=0x6c,
  CMOVGT=0x66,CMOVLE=0x64,CMOVLT=0x44,CMOVNE=0x26,EQV=0x48,ORNOT=0x28,XOR=0x40
  } op11field;

typedef enum { /* operate, opcode = 12 */
  EXTBL=0x06,EXTLH=0x6a,EXTLL=0x26,EXTQH=0x7a,EXTQL=0x36,EXTWH=0x5a,
  EXTWL=0x16,INSBL=0x0b,INSLH=0x67,INSLL=0x2b,INSQH=0x77,INSQL=0x3b,
  INSWH=0x57,INSWL=0x1b,MSKBL=0x2,MSKLH=0x62,MSKLL=0x22,MSKQH=0x72,
  MSKQL=0x32,MSKWH=0x52,MSKWL=0x12,SLL=0x39,SRA=0x3C,SRL=0x34,
  ZAP=0x30,ZAPNOT=0x31} op12field;

typedef enum { /* operate, opcode = 13 */
  MULL=0,MULLV=0x40,MULQ=0x20,MULQV=0x60,UMULH=0x30} op13field;

typedef enum { /* ITFP: int->FP and other FP extensions, opcode 0x14 */
    ITOFS=0x4, ITOFF=0x14, ITOFT=0x24, SQRTS=0x08b, SQRTT=0x0ab
} op14field;

typedef enum { /* float, opcode = 17 */
    // FLTL: floating-point (non-IEEE) ops
  CPYS=0x20,CPYSE=0x22,CPYSN=0x21,CVTLQ=0x10,CVTQL=0x30,CVTQLSV=0x530,
  CVTQLV=0x130,FCMOVEQ=0x2a,FCMOVGE=0x2d,FCMOVGT=0x2f,FCMOVLE=0x2e,
  FCMOVLT=0x2c,FCMOVNE=0x2b,MF_FPCR=0x25,MT_FPCR=0x24} float17field;

typedef enum {
    /* float, opcode = FLTI (0x16), inst<10:5> ("SRC" and "FNC" together) */
    // FLTI: IEEE floating-point ops
  ADDS=0,ADDT=0x20,CMPTEQ=0x25,CMPTLT=0x26,CMPTLE=0x27,CMPTUN=0x24,
  CVTQS=0x3c,CVTQT=0x3e,CVTTS=0x2c,DIVS=0x3,DIVT=0x23,MULS=0x2,MULT=0x22,
  SUBS=0x1,SUBT=0x21,CVTTQ=0x2f} floatop;

typedef enum { /*palcode, opcode = 00 */
  callsys=0x83, rduniq=0x9e, wruniq=0x9f, gentrap=0xaa } palop;

// opcodes FLTI (0x16), and ITFP (0x14) RND field <12:11> bit assignment
// c.f. alpha handbook sec 4.7.5, 4.7.9
typedef enum {
    // Do not reorder!
    FLTI_RND_Chopped,           // IEEE round-to-zero, mnemonic "/C" or "c"
    FLTI_RND_MinusInf,          // Assembly mnemonic "/M" or "m" suffix
    FLTI_RND_Normal,            // IEEE round-to-nearest (no mnemonic)
    FLTI_RND_Dynamic            // (reads FPCR<DYN> aka FPCR<59:38> 
} FltiRoundType;

// opcodes FLTI (0x16), and ITFP (0x14) TRP field <15:13> bit assignment.
// Note also that CVTQL (under opcode FLTL) can use /V and /SV
//
// c.f. alpha handbook sec 4.7.7, 4.7.9
typedef enum {
    // An Alpha FP trap qualifier is a possibly-empty combo of the following:
    //   S: Precise exception completion (see sec 4.7.7.3)
    //      (Without /S, imprecise exceptions are allowed.)
    //   U: Underflow trap enable (for floating-point output)
    //   V: Integer overflow trap enable (for integer output)
    //   I: Inexact result trap enable
    // Only the following combinations are valid:
    //   <empty> U SU SUI V SV SVI; or, as a regex: (S?(U|V)I?)?
    //
    // Do not reorder!
    FLTI_TRP_Imprecise,         // default (no qualifier)
    FLTI_TRP_UnderflowEnable,   // mnemonic fp "/U", integer "/V"
    FLTI_TRP_Reserved2,
    FLTI_TRP_Reserved3,
    FLTI_TRP_Reserved4,
    FLTI_TRP_PreciseUnderflow,  // mnemonic fp "/SU" integer "/SV"
    FLTI_TRP_Reserved6,
    FLTI_TRP_PreciseUnderflowInexact    // mnemonic fp "/SUI" integer "/SVI"
} FltiTrapType;



#define INST_BITS(x, offs, width)       GET_BITS_32(x, offs, width)

#define INST_OPCODE(x)          INST_BITS(x, 26, 6)
#define INST_RA(x)              INST_BITS(x, 21, 5)
#define INST_RB(x)              INST_BITS(x, 16, 5)
#define INST_RB_IMMFLAG(x)      ((x) & 0x1000)
#define INST_RB_IMMED(x)        INST_BITS(x, 13, 8)
#define INST_RC(x)              INST_BITS(x, 0, 5)

#define INST_MEM_FUNC(x)        INST_BITS(x, 0, 16)
#define INST_BRANCH_DISP(x)     INST_BITS(x, 0, 21)
#define INST_INTOP_FUNC(x)      INST_BITS(x, 5, 7)
#define INST_FLTL_FUNC(x)       INST_BITS(x, 5, 11)
#define INST_JMPJSR_TYPE(x)     INST_BITS(x, 14, 2)
#define INST_CALLPAL_CODE(x)    INST_BITS(x, 0, 26)

// While FLTI instructions have an 11-bit function code, it's broken
// down further into sub-fields:
//   inst<15:13> "TRP" controls FP trap options
//   inst<12:11> "RND" controls FP rounding
//   inst<10:9>  "SRC" controls source operand types (e.g. S/T/Q)
//   inst<8:5>   "FNC" selects the function (e.g. add, mul)
// We use the both the "SRC" and "FNC" fields together to select
// emulate functions.
#define INST_FLTI_TRAP(x)       INST_BITS(x, 13, 3)
#define INST_FLTI_ROUND(x)      INST_BITS(x, 11, 2)
#define INST_FLTI_SRCFNC(x)       INST_BITS(x, 5, 6)


#endif // INST_H
