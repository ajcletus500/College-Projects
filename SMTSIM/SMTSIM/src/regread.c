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

 */


#include "sim-assert.h"
#include "main.h"
#include "core-resources.h"
#include "dyn-inst.h"


/* Just pass on to the next stage */
// Placeholder for reading source values from register file and bypass net

void
regread(void)
{
    int core_id;
    for (core_id = 0; core_id < CoreCount; core_id++) {
        CoreResources * restrict core = Cores[core_id];
        const int rread1 = core->stage.rread1;
        const int rreadN = core->stage.rread1 + core->params.regread.n_stages
            - 1;
        const int rwrite1 = core->stage.rwrite1;
        const int regread_cyc = core->params.regread.n_stages;

        // Move instructions from final regread stage into exec or regwrite
        while (stageq_count(core->stage.s[rreadN]) > 0) {
            activelist * restrict inst = stageq_head(core->stage.s[rreadN]);
            stageq_dequeue(core->stage.s[rreadN]);
            if ((inst->delay > 0) ||
                (inst->mem_flags && (inst->donecycle > (cyc + regread_cyc)))) {
                // Send an inst to the exec stage if it has any business there,
                // or if it needs to wait on memory for any reason.
                stageq_enqueue(core->stage.exec, inst);
            } else {
                // If this instruction has a magic 0-cyc execute delay, send
                // it straight to regwrite.
                stageq_enqueue(core->stage.s[rwrite1], inst);
            }
        }

        // Shift instructions from (rread1...N-1) to the next stage
        // (rread2...N)
        for (int src_stage = rreadN - 1; src_stage >= rread1;
             src_stage--) {
            sim_assert(stageq_count(core->stage.s[src_stage + 1]) == 0);
            stageq_assign(core->stage.s[src_stage + 1], 
                          core->stage.s[src_stage]);
        }
    }
}
