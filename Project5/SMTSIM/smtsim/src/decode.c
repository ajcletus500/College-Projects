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

 */

#include <stdio.h>

#include "sim-assert.h"
#include "main.h"
#include "core-resources.h"
#include "dyn-inst.h"
#include "context.h"
#include "inject-inst.h"


static inline void
detect_misfetches(const StageQueue * restrict stage)
{
    for (activelist * restrict instrn = stageq_head(*stage); instrn != NULL;
         instrn = instrn->next) {
        if (instrn->misfetch != MisPred_None) {
            sim_assert(!Contexts[instrn->thread]->misfetch_discovered);
            Contexts[instrn->thread]->misfetch_discovered = instrn;
        }
    }
}


/* Pass on to the next stage, as long as it is empty */

static void
decode_for_core(CoreResources * restrict core)
{
    const int decode1 = core->stage.decode1;
    const int rename1 = core->stage.rename1;

    /* if rename stage clear, move previously fetched instructions to the
       rename stage
    */

    // Shift instructions from decode1...N to the next stage
    // (decode2...N, rename1) if clear
    for (int src_stage = rename1 - 1; src_stage >= decode1;
         src_stage--) {
        if (stageq_count(core->stage.s[src_stage + 1]) == 0) {
            // Detect misfetches in decode1
            if (src_stage == decode1) 
                detect_misfetches(&core->stage.s[src_stage]);
            stageq_assign(core->stage.s[src_stage + 1], 
                          core->stage.s[src_stage]);
        } else {
            DEBUGPRINTF("C%i: decode%i backed up\n", core->core_id,
                        src_stage - decode1 + 1);
        }
    }
}


void
decode()
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources * restrict core = Cores[core_id];
        int rename1_was_avail =
            !stageq_count(core->stage.s[core->stage.rename1]);
        if (core->rename_inject_won_last) {
            decode_for_core(core);
            service_rename_inject(core);
        } else {
            service_rename_inject(core);
            decode_for_core(core);
        }
        // alternate access to this core's rename latch, between instructions
        // from the decode latch, and instructions waiting for injection.
        // (only alternate when rename was actually available for input,
        // to avoid weird biases from it tending to be full on odd vs. even
        // cycles)
        if (rename1_was_avail)
            core->rename_inject_won_last ^= 1;
    }
}



