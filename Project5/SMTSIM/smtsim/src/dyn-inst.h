//
// Dynamic instruction info
//
// This structure holds per-dynamic-instruction information.  It's called
// "activelist" or "alist" for historical reasons.
//
// Jeff Brown
// $Id: dyn-inst.h,v 1.1.2.11.2.3.2.5 2008/11/21 22:43:18 jbrown Exp $
//

#ifndef DYN_INST_H
#define DYN_INST_H

#ifdef __cplusplus
extern "C" {
#endif


typedef struct activelist activelist;

typedef enum {FETCHED=1, BLOCKED=2, FETCHED_BLOCKED=3, EXECUTING=4,
              INVALID=8, RETIREABLE=16, SQUASHED=32, MEMORY=64,
              EXECUTED=128
              } execstatus;


// State overwritten when an instruction's "emulate" routine is called; saving
// this lets us reverse the effects of (most) instructions.
typedef struct inst_undo_info {
    // dest reg value that was overwritten by this instruction's output
    i64 dest_reg_val;
    // memory value overwritten by this, iff it's a store; we can't just
    // dest_reg_val for that now, due to stl_c_uni/stq_c_uni
    i64 dest_mem_val;

    // These are a copy of the last_writer value for the dest reg when
    // this copy was made; if that instruction has been retired, it may no
    // longer be valid when restoring state from here, so be careful.
    int dest_lastwriter;

    // value on the ret stack overwritten by this inst if it does an rs_push
    // on a full stack, or popped if it does an rs_pop.  0 means that the
    // rs_push did not happen on a full stack, or the rs_pop was on an empty
    // stack.  Not valid for instructions which don't use the ret stack.
    i64 retstack_val;

    // Flag: set on undo_inst, to prevent double-undos
    int undone;

    // Extra variables for checking in debug mode
#ifdef DEBUG
    // A copy of the GHR as it was at fetch time, to ensure our GHR-rollback
    // is working correctly
    unsigned undo_ghr;
#endif  // DEBUG
} inst_undo_info;


typedef enum {
    MisPred_None=0,             // Not a mispredict
    MisPred_CorrectPath,        // Mispredict from correct path
    MisPred_WrongPath,          // Mispredict from a wrong path
    MisPred_last
} MisPred;
extern const char *MisPred_names[];         // context.cc


typedef enum {                  // Bit Flags
    BmtSF_None=0,               // Not BMT spill/fill related
    BmtSF_Spill=0x1,            // _Spill and _Fill are mutually exclusive
    BmtSF_Fill=0x2,
    BmtSF_Final=0x4,
    BmtSF_BlockMarker=0x8,      // Start of a fill block/end of a spill block
    BmtSF_FreeTransfer=0x10     // Skip data transfer billing
} BmtSpillFill;


struct activelist {
    activelist *next;
    activelist *mergeinst;
    int thread;                 // global thread ID (index into Contexts[])
    int id;                     // hardware inst id (index into ctx->alist[])
    struct AppState *as;
    inst_undo_info undo;
    execstatus status;
    int src1, src2, dest, deps;
    activelist *src1_waitingfor, *src2_waitingfor;
    unsigned ghr;
    int fu;
    int delay;
    int syncop, wait_sync;
    // FIXME: We assume that all the registers accessed by the instruction
    //        are for the fp reg.file if fu == FP otherwise for the int reg.file
    //        This is not entirely true (especially for mov instrs) 
    //        and should be remedied in emulate.c by maintaining iregaccs and 
    //        fpregaccs separately.
    int regaccs;                // The number of registers accessed (for stats)
    int br_flags;               // Static-instruction branch flags
    int gen_flags;              // Static-instruction misc. flags
    int mem_flags;              // Static-instruction memory op flags
    mem_addr srcmem, destmem;   // Only valid when mem_flags nonzero
    activelist **waiter;
    int numwaiting;
    int waiter_size;
    i64 fetchcycle;
    i64 renamecycle;            // cycle renamed & sent to queue
    i64 issuecycle;             // cycle issued from the queue
    i64 readycycle;             // earliest cyc all srcs ready & can execute
    i64 addrcycle;              // mem-insts only: addr ready (MAX: unknown)
    i64 donecycle;              // cycle result can write to regs & forward
    int iregs_used, fregs_used, robentry, lsqentry;
    MisPred mispredict;
    MisPred misfetch;
    int wp;
    mem_addr pc;
    i64 mb_epoch, wmb_epoch;
    struct CacheRequest *dmiss_cache_entry;
    i64 app_inst_num;
    int insts_discarded_before; // #insts discarded between this and previous

    int taken_branch;           // Correct branch direction
    mem_addr br_target;         // Correct branch target (if taken)
    mem_addr target_predict;
    int taken_predict;          // Predicted branch direction
    int skipped_bpredict;       // Didn't access/update branch predictor

    struct {
        mem_addr base_pc;       // If nonzero, base PC of parent trace block
        int predict_num;        // Predict. num in trace block, or -1
    } tc;

    struct {
        int leader_id;  // Alist# of group leader, -1 == ungrouped inst
        int overlap_next_leader; // ID of following leader, -1 => no overlap
        // "remaining" only valid for leaders:
        //   n==0: all done, may commit
        //   n>0: n instructions uncompleted
        //   n<0: size unknown, still fetching, (-n - 1) insts completed
        int remaining;
    } commit_group;

    struct {
        unsigned spillfill;
    } bmt;

    struct {
        int service_level;      // (from cache-req.h)
        int was_merged;         // flag: was merged onto an earlier request
        i64 latency;
    } icache_sim, dcache_sim;
};


#ifdef __cplusplus
}
#endif

#endif  // DYN_INST_H
