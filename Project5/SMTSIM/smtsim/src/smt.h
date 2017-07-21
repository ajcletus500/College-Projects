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


#ifndef _SMT_H_DEFINED
#define _SMT_H_DEFINED

/* some nops we have redefined to be SMT-specific instructions */

#define SMT_FORK              0x47e0141f    /* bis $31, 0, $31 */
#define SMT_GETID             0x47e0541f    /* bis $31, 2, $31 */
#define SMT_CREATESTACK       0x47e0741f    /* bis $31, 3, $31 */
#define SMT_HW_LOCK           0x47e3f41f    /* bis $31, 0x1f, $31 */
#define SMT_RELEASE           0x47e4141f    /* bis $31, 0x20, $31 */
#define SMT_TERMINATE         0x47e5941f    /* bis $31, 0x2c, $31 */
#define SMT_START_SIM         0x47e7741f    /* bis $31, 0x3b, $31 */
#define SMT_PRINT_SIM         0x47e7941f    /* bis $31, 0x3c, $31 */
#define SMT_END_SIM           0x47e7b41f    /* bis $31, 0x3d, $31 */


#endif
