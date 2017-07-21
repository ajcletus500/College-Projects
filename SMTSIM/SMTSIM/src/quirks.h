/*
 * Knobs to control quirky simulator behavior
 *
 * Jeff Brown
 * $Id: quirks.h,v 1.2.6.5.6.1.2.2 2009/06/03 22:25:15 jbrown Exp $
 */

#ifndef QUIRKS_H
#define QUIRKS_H

#ifdef __cplusplus
extern "C" {
#endif



#define CONDBRANCH_WP_MAGIC_BTB                 0
#define RETURN_WP_MAGIC_BTB                     0
#define CP_MISPREDICT_MAGIC_BTB                 0
#define CONDBRANCH_MISP_PSYCHIC_GHR             0
#define IGNORE_WP_MISPREDICTS                   0
#define BTBMISS_NT_PSYCHIC_BPRED                0

#define FLOAT_DIVZERO_WP_SHORTDELAY             0
#define FLOAT_DIVZERO_KNOWS_ABOUT_WP            0


#ifdef __cplusplus
}
#endif

#endif  /* QUIRKS_H */
