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

#include <stdio.h>
#include "main.h"
#include "mem.h"
#include "inst.h"
#include "smt.h"
#include "sign-extend.h"
#include "context.h"
#include "app-state.h"


const char *AlphaIntReg_names[] = { 
    "v0",
    "t0",
    "t1",
    "t2",
    "t3",
    "t4",
    "t5",
    "t6",
    "t7",
    "s0",
    "s1",
    "s2",
    "s3",
    "s4",
    "s5",
    "fp",       // aka s6
    "a0",
    "a1",
    "a2",
    "a3",
    "a4",
    "a5",
    "t8",
    "t9",
    "t10",
    "t11",
    "ra",
    "pv",       // aka t12
    "at",
    "gp",
    "sp",
    "zero",
    NULL
};


/* This routine prints out a disassembly-like listing for
 *  debugging.
 */
    
void print(int thread, int alist_id, u64 pc, unsigned steps) {
    u32 inst;
    unsigned int offset, ra, rb, rc; /* for decoding */
    u64 rbv;
    u64 va;
    long disp;
    int i;

    context *current = Contexts[thread];

    if (0)
        for (i=0;i<thread;i++) printf("        ");
    printf("T%ds%d/A%d:", thread, alist_id, current->as->app_id);
    if (!current->wrong_path && !current->follow_sync) 
      printf("C");
    else {
      if (current->wrong_path) {
        printf("W");
        if (!pc) {
          printf(":        <%s>PC = 0\n", fmt_i64(cyc));
          return;
        }
      }
      if (current->follow_sync) printf("S");
    }
    if (current->commit_group.fetching_leader >= 0) printf("(cg)");
    printf(":");
    while(steps-- > 0) {
        const char *opname = "<unknown>";
        inst = read_mem_32(current, pc);
        printf("0x%s: <%s>: [0x%08x]  ", fmt_x64(pc), fmt_i64(cyc), inst);
        ra = INST_RA(inst);
        rb = INST_RB(inst);
        rc = INST_RC(inst);
        offset = INST_MEM_FUNC(inst);
        disp = INST_BRANCH_DISP(inst);
        va = current->as->R[rb].i + SEXT16_i64(offset);
        switch (INST_OPCODE(inst)) {
        case LDA:printf("lda"); goto mem;
        case LDAH:printf("ldah"); goto mem;
        case LDQ_U:printf("ldq_u"); goto memlq;
        case STQ_U:printf("stq_u"); goto memsq;
        case INTA:
          switch (INST_INTOP_FUNC(inst)) {
          case ADDLV:printf("addl/v"); goto reg3;
          case ADDL:printf("addl"); goto reg3;
          case ADDQ:printf("addq"); goto reg3;
          case ADDQV:printf("addq/v"); goto reg3;
          case CMPLE:printf("cmple"); goto reg3;
          case CMPULT:printf("cmpult"); goto reg3;
          case S4ADDL:printf("s4addl"); goto reg3;
          case S4SUBQ:printf("s4subq"); goto reg3;
          case S8SUBL:printf("s8subl"); goto reg3;
          case CMPBGE:printf("cmpbge"); goto reg3;
          case CMPLT:printf("cmplt"); goto reg3;
          case SUBLV:printf("sublv"); goto reg3;
          case SUBL:printf("subl"); goto reg3;
          case SUBQV:printf("subq/v"); goto reg3;
          case SUBQ:printf("subq"); goto reg3;
          case S4ADDQ:printf("s4addq"); goto reg3;
          case S8ADDL:printf("s8addl"); goto reg3;
          case S8SUBQ:printf("s8subq"); goto reg3;
          case CMPEQ:printf("cmpeq"); goto reg3;
          case CMPULE:printf("cmpule"); goto reg3;
          case S4SUBL:printf("s4subl"); goto reg3;
          case S8ADDQ:printf("s8addq"); goto reg3;
          reg3: 
            if (INST_RB_IMMFLAG(inst)) {
              rbv = INST_RB_IMMED(inst);
              printf(" $%d,%s, $%d\n", ra, fmt_i64(rbv), rc);
            }
            else {
              printf(" $%d,$%d, $%d\n", ra, rb, rc);
            }
            break;
          }
          break;
        case INTL:
          switch (INST_INTOP_FUNC(inst)) {
          case AND:printf("and"); goto reg3_;
          case BIC:printf("bic"); goto reg3_;
          case BIS:
            switch (inst) {
            case SMT_FORK:
              printf("smt_fork\n");break;
            case SMT_GETID:
              printf("smt_getid\n");break;
            case SMT_CREATESTACK:
              printf("smt_createstack\n");break;
            case SMT_HW_LOCK:
              printf("smt_HW_LOCK [0x%s]\n", fmt_x64(current->as->R[2].i));
              break;
            case SMT_RELEASE:
              printf("smt_RELEASE [0x%s]\n", fmt_x64(current->as->R[2].i));
              break;
            default:
              printf("bis"); goto reg3_;
            }
            break;
          case CMOVEQ:printf("cmoveq"); goto reg3_;
          case CMOVLBC:printf("cmovlbc"); goto reg3_;
          case CMOVLBS:printf("cmovlbs"); goto reg3_;
          case CMOVGE:printf("cmovge"); goto reg3_;
          case CMOVGT:printf("cmovgt"); goto reg3_;
          case CMOVLE:printf("cmovle"); goto reg3_;
          case CMOVLT:printf("cmovlt"); goto reg3_;
          case CMOVNE:printf("cmovne"); goto reg3_;
          case EQV:printf("eqv"); goto reg3_;
          case ORNOT:printf("ornot"); goto reg3_;
          case XOR:printf("xor"); goto reg3_;
          case AMASK:printf("amask"); goto reg3_;
          case IMPLVER:printf("implver"); goto reg3_;
          reg3_: 
            if (INST_RB_IMMFLAG(inst)) {
              rbv = INST_RB_IMMED(inst);
              printf(" $%d,%s, $%d\n", ra, fmt_i64(rbv), rc);
            }
            else {
              printf(" $%d,$%d, $%d\n", ra, rb, rc);
            }
            break;
          }
          break;
        case INTS:
          switch (INST_INTOP_FUNC(inst)) {
          case EXTBL:printf("extbl"); goto reg3__;
          case EXTLH:printf("extlh"); goto reg3__;
          case EXTLL:printf("extll"); goto reg3__;
          case EXTQH:printf("extqh"); goto reg3__;
          case EXTQL:printf("extql"); goto reg3__;
          case EXTWH:printf("extwh"); goto reg3__;
          case EXTWL:printf("extwl"); goto reg3__;
          case INSBL:printf("insbl"); goto reg3__;
          case INSLH:printf("inslh"); goto reg3__;
          case INSLL:printf("insll"); goto reg3__;
          case INSQH:printf("insqh"); goto reg3__;
          case INSQL:printf("insql"); goto reg3__;
          case INSWH:printf("inswh"); goto reg3__;
          case INSWL:printf("inswl"); goto reg3__;
          case MSKBL:printf("mskbl"); goto reg3__;
          case MSKLH:printf("msklh"); goto reg3__;
          case MSKLL:printf("mskll"); goto reg3__;
          case MSKQH:printf("mskqh"); goto reg3__;
          case MSKQL:printf("mskql"); goto reg3__;
          case MSKWH:printf("mskwh"); goto reg3__;
          case MSKWL:printf("mskwl"); goto reg3__;
          case SLL:printf("sll"); goto reg3__;
          case SRA:printf("sra"); goto reg3__;
          case SRL:printf("srl"); goto reg3__;
          case ZAP:printf("zap"); goto reg3__;
          case ZAPNOT:printf("zapnot"); goto reg3__;
          reg3__: 
            if (INST_RB_IMMFLAG(inst)) {
              rbv = INST_RB_IMMED(inst);
              printf(" $%d,%s, $%d\n", ra, fmt_i64(rbv), rc);
            }
            else {
              printf(" $%d,$%d, $%d\n", ra, rb, rc);
            }
            break;
          }
          break;
        case FPTI:
            switch (INST_INTOP_FUNC(inst)) {
            case SEXTB: opname = "sextb"; goto reg2_i;
            case SEXTW: opname = "sextw"; goto reg2_i;
            case FTOIT: opname = "ftoit"; goto reg2_f2i;
            case FTOIS: opname = "ftois"; goto reg2_f2i;
            }
        reg2_i:
            printf("%s $%d, $%d\n", opname, rb, rc); break;
        reg2_f2i:
            printf("%s $f%d, $%d\n", opname, ra, rc); break;
        case INTM:
          switch (INST_INTOP_FUNC(inst)) {
          case MULLV:printf("mull/v"); goto reg3___;
          case MULL:printf("mull"); goto reg3___;
          case MULQV:printf("mulq/v"); goto reg3___;
          case MULQ:printf("mulq"); goto reg3___;
          case UMULH:printf("umulh"); goto reg3___;
          reg3___: 
            if (INST_RB_IMMFLAG(inst)) {
              rbv = INST_RB_IMMED(inst);
              printf(" $%d,%s, $%d\n", ra, fmt_i64(rbv), rc);
            }
            else {
              printf(" $%d,$%d, $%d\n", ra, rb, rc);
            }
            break;
          }
          break;
        case ITFP: {
            int is_i2f = 0;
            switch (INST_FLTL_FUNC(inst)) {
            case ITOFS: opname = "itofs"; is_i2f = 1; break;
            case ITOFF: opname = "itoff"; is_i2f = 1; break;
            case ITOFT: opname = "itoft"; is_i2f = 1; break;
            case SQRTS: opname = "sqrts"; break;
            case SQRTT: opname = "sqrtt"; break;
            }
            if (is_i2f) {
                printf("%s $%d, $f%d\n", opname, ra, rc);
            } else {
                printf("%s $f%d, $f%d\n", opname, rb, rc);
            }
            break;
        }
        case FLTL:
          switch (INST_FLTL_FUNC(inst)) {
          case CPYS:printf("cpys");goto _reg3;
          case CPYSE:printf("cpyse");goto _reg3;
          case CPYSN:printf("cpysn");goto _reg3;
          case CVTLQ:printf("cvtlq");goto reg2_;
          case CVTQL:printf("cvtql");goto reg2_;
          case CVTQLSV:printf("cvtql/sv");goto reg2_;
          case CVTQLV:printf("cvtql/v");goto reg2_;
          case FCMOVEQ:printf("fcmoveq");goto _reg3;
          case FCMOVGE:printf("fcmovege");goto _reg3;
          case FCMOVGT:printf("fcmovgt");goto _reg3;
          case FCMOVLE:printf("fcmovle");goto _reg3;
          case FCMOVLT:printf("fcmovlt");goto _reg3;
          case FCMOVNE:printf("fcmovne");goto _reg3;
          case MT_FPCR:printf("mt_fpcr %d", ra);break;
          case MF_FPCR:printf("mf_fpcr %d", ra);break;
          reg2_:
            printf(" $%d, $%d\n", rb, rc);
            break;
          _reg3: 
            printf(" $%d,$%d, $%d\n", ra, rb, rc);
            break;
          }
          break;
        case FLTI:
          switch (INST_FLTI_SRCFNC(inst)) {
          case ADDS:printf("adds");goto __reg3;
          case ADDT:printf("addt");goto __reg3;
          case CMPTEQ:printf("cmpteq");goto __reg3;
          case CMPTLT:printf("cmptlt");goto __reg3;
          case CMPTLE:printf("cmptle");goto __reg3;
          case CMPTUN:printf("cmptun");goto __reg3;
          case CVTQS:printf("cvtqs");goto __reg3;
          case CVTQT:printf("cvtqt");goto __reg3;
          case CVTTS:printf("cvtts");goto __reg3;
          case DIVS: printf("divs");goto __reg3;
          case DIVT:printf("divt");goto __reg3;
          case MULS:printf("muls");goto __reg3;
          case MULT:printf("mult");goto __reg3;
          case SUBS:printf("subs");goto __reg3;
          case SUBT:printf("subt");goto __reg3;
          case CVTTQ:printf("cvttq");goto __reg3;
          __reg3: 
            printf(" $%d,$%d, $%d\n", ra, rb, rc);
            break;
          }
          break;
        case MISC:
          switch (inst & 0xffff) {
          case FETCH:printf("fetch\n");break;
          case RC : printf("rc\n");break;
          case TRAPB: printf("trapb\n");break;
          case MB: printf("mb\n");break;
          case RS: printf("rs\n");break;
          case FETCH_M: printf("fetch_m\n");break;
          case RPCC: printf("rpcc\n");break;
          }
          break;
        case JMPJSR:
        {
            const char *op = "jmpjsr";
            mem_addr dest_pc = current->as->R[rb].i & ~3;
            switch (INST_JMPJSR_TYPE(inst)) {
            case JMP: op = "jmp"; break;
            case JSR: op = "jsr"; break;
            case RETN: op = "retn"; break;
            case JSR_COROUTINE: op = "jsr_coroutine"; break;
            }
            printf("%s $%d, ($%d) [0x%s]\n", op, ra, rb, fmt_x64(dest_pc));
        }
        break;
        case LDS:printf("lds");goto memfs;
        case LDF:printf("ldf");goto memf;
        case LDT:printf("ldt");goto memft;
        case LDG:printf("ldg");goto memf;
        case STS:printf("sts");goto memsfs;
        case STF:printf("stf");goto memsf;
        case STT:printf("stt");goto memsft;
        case STG:printf("stg");goto memsf;
        case LDL:printf("ldl");goto memlw;
        case LDQ:printf("ldq");goto memlq;
        case LDL_L:printf("ldl_l");goto memlw;
        case LDQ_L:printf("ldq_l");goto memlq;
        case STL:printf("stl");goto memsw;
        case STQ:printf("stq");goto memsq;
        case STL_C:printf("stl_c");goto memsw;
        case STQ_C:printf("stq_c");goto memsq;
        case LDBU:printf("ldbu");goto meml8;
        case LDWU:printf("ldwu");goto meml16;
        case STB:printf("stb");goto mems8;
        case STW:printf("stw");goto mems16;
        case BSR:printf("bsr");goto branch; 
        case BR:printf("br");goto branch; 
        case FBEQ:printf("fbeq");goto fbranch; 
        case FBLT:printf("fblt");goto fbranch; 
        case FBLE:printf("fble");goto fbranch; 
        case FBNE:printf("fbne");goto fbranch; 
        case FBGE:printf("fbge");goto fbranch; 
        case FBGT:printf("fbgt");goto fbranch; 
        case BLBC:printf("blbc");goto branch; 
        case BEQ:printf("beq");goto branch; 
        case BLT:printf("blt");goto branch; 
        case BLE:printf("ble");goto branch; 
        case BLBS:printf("blbs");goto branch; 
        case BNE:printf("bne");goto branch; 
        case BGE:printf("bge");goto branch; 
        case BGT:printf("bgt");goto branch; 
        case CALL_PAL:
          switch(inst & 0x3ffffff) {
          case callsys:printf("syscall\n");break;
          case rduniq:printf("rduniq\n");break;
          case wruniq:printf("wruniq\n");break;
          default:printf("call_pal\n");
          }
          break;
        default:
            printf("op.%x ??\n", (inst >> 26));
            break;
        mem:
          printf(" $%d, %s($%d) [0x%s]\n",ra, fmt_i64(SEXT16_i64(offset)),rb,
                 fmt_x64(va));
          break;
        memf:
          printf(" $f%d, %s($%d) [0x%s]\n",ra, fmt_i64(SEXT16_i64(offset)),rb,
                 fmt_x64(va));
          break;
        memfs:
          printf(" $f%d, %s($%d) [0x%s] = 0x%s\n",ra,
                 fmt_i64(SEXT16_i64(offset)), rb, fmt_x64(va),
                 fmt_x64(safe_read_mem_32(current, va)));
          break;
        memft:
          printf(" $f%d, %s($%d) [0x%s]\n",ra, fmt_i64(SEXT16_i64(offset)),rb,
                 fmt_x64(va));
          break;
        memlw:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n", ra,
                 fmt_i64(SEXT16_i64(offset)), rb, fmt_x64(va), 
                 fmt_x64(safe_read_mem_32(current, va)));
          break;
        memsw:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n",ra, 
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), fmt_x64(current->as->R[ra].i));
          break;
        memsf:
          printf(" $f%d, %s($%d) [0x%s] = %.15g\n",ra, 
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), current->as->R[FP_REG(ra)].f);
          break;
        memsfs:
          { 
              i64 i_result = 
                  (((current->as->R[FP_REG(ra)].i>>32)&0xc0000000)|
                   ((current->as->R[FP_REG(ra)].i>>29) & 0x3ffffff));
              printf(" $f%d, %s($%d) [0x%s] = 0x%s\n", ra, 
                     fmt_i64(SEXT16_i64(offset)), rb,
                     fmt_x64(va), fmt_x64(i_result));
          }     
          break;
        memsft:
          printf(" $f%d, %s($%d) [0x%s] = %.15g\n",ra,
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), current->as->R[FP_REG(ra)].f);
          break;
        memlq:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n",ra,
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), fmt_x64(safe_read_mem_64(current, va)));
          break;
        memsq:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n",ra, 
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), fmt_x64(current->as->R[ra].i));
          break;
        meml8:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n",ra,
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), 
                 fmt_x64((safe_read_mem_32(current, va & ~3) >> (8 * (va & 3)))
                         & 0xff));
        mems8:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n",ra,
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), fmt_x64(current->as->R[ra].i & 0xff));
        meml16:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n",ra,
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), 
                 fmt_x64((safe_read_mem_32(current, va & ~3) >> (8 * (va & 2)))
                         & 0xffff));
        mems16:
          printf(" $%d, %s($%d) [0x%s] = 0x%s\n",ra,
                 fmt_i64(SEXT16_i64(offset)),
                 rb, fmt_x64(va), fmt_x64(current->as->R[ra].i & 0xffff));
        branch:
          printf(" $%d [0x%s], ",ra, fmt_x64(current->as->R[ra].i));
          printf("0x%s\n", fmt_x64(pc + (SEXT_TO_i64(disp, 21) << 2) + 4));
          break;
        fbranch:
          printf(" $%d [%.15g], ",ra, current->as->R[FP_REG(ra)].f);
          printf("0x%s\n", fmt_x64(pc + (SEXT_TO_i64(disp, 21) << 2) + 4));
          break;
        }
        pc += 4;
    }
}
