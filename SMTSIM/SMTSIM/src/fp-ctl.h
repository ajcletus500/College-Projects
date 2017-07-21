/*
 * Floating-point calculation control
 *
 * Jeff Brown
 * $Id: fp-ctl.h,v 1.1.12.2.2.1.2.2 2009/12/16 04:55:34 jbrown Exp $
 */

#ifndef FP_CTL_H
#define FP_CTL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { 
    FPCtl_RoundDown, FPCtl_RoundToNearest, FPCtl_RoundToZero,
    FPCtl_RoundUp, FPCtlRoundingMode_last
} FPCtlRoundingMode;
extern const char *FPCtlRoundingMode_names[];

/* Get old FP rounding mode */
FPCtlRoundingMode fpctl_getround(void);

/* Set new FP rounding mode */
void fpctl_setround(FPCtlRoundingMode new_mode);

/* Set FPU for double-precision rounding (vs x87 80-bit default, sometimes) */
void fpctl_set_double_rounding(void);

#ifdef __cplusplus
}
#endif

#endif  /* FP_CTL_H */
