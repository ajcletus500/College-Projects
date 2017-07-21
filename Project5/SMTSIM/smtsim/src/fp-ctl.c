/*
 * Floating-point rounding control
 *
 * Jeff Brown
 * $Id: fp-ctl.c,v 1.4.12.3.2.1.2.2 2009/12/16 04:55:33 jbrown Exp $
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "fp-ctl.h"

const char *FPCtlRoundingMode_names[] = { 
    "FPCtl_RoundDown",
    "FPCtl_RoundToNearest",
    "FPCtl_RoundToZero",
    "FPCtl_RoundUp",
    NULL
};


#if defined(__alpha) || defined(__linux__)  || defined(__APPLE__)
#   ifdef __alpha
#       include <alpha/fpu.h>
#   else
#       include <fenv.h>
#   endif

    FPCtlRoundingMode 
    fpctl_getround(void)
    {
        int mode = fegetround();
        FPCtlRoundingMode result;
        switch(mode) {
        case FE_DOWNWARD:
            result = FPCtl_RoundDown; break;
        case FE_TONEAREST:
            result = FPCtl_RoundToNearest; break;
        case FE_TOWARDZERO:
            result = FPCtl_RoundToZero; break;
        case FE_UPWARD:
            result = FPCtl_RoundUp; break;
        default:
            result = 0;
            abort();
        }
        return result;
    }


    void
    fpctl_setround(FPCtlRoundingMode new_mode)
    {
        int mode;
        switch(new_mode) {
        case FPCtl_RoundDown:
            mode = FE_DOWNWARD; break;
        case FPCtl_RoundToNearest:
            mode = FE_TONEAREST; break;
        case FPCtl_RoundToZero:
            mode = FE_TOWARDZERO; break;
        case FPCtl_RoundUp:
            mode = FE_UPWARD; break;
        default:
            mode = 0;
            abort();
        }
        fesetround(mode);
    }


#elif defined(__FreeBSD__)
#   include <ieeefp.h>

    FPCtlRoundingMode 
    fpctl_getround(void)
    {
        int mode = fpgetround();
        FPCtlRoundingMode result;
        switch(mode) {
        case FP_RM:
            result = FPCtl_RoundDown; break;
        case FP_RN:
            result = FPCtl_RoundToNearest; break;
        case FP_RZ:
            result = FPCtl_RoundToZero; break;
        case FP_RP:
            result = FPCtl_RoundUp; break;
        default:
            result = 0;
            abort();
        }
        return result;
    }


    void
    fpctl_setround(FPCtlRoundingMode new_mode)
    {
        int mode;
        switch(new_mode) {
        case FPCtl_RoundDown:
            mode = FP_RM; break;
        case FPCtl_RoundToNearest:
            mode = FP_RN; break;
        case FPCtl_RoundToZero:
            mode = FP_RZ; break;
        case FPCtl_RoundUp:
            mode = FP_RP; break;
        default:
            mode = 0;
            abort();
        }
        fpsetround(mode);
    }


#else
# error "Unhandled system type."
#endif


#if defined(__linux__) && defined(__i386__) && ((LONG_MAX) == 0x7fffffff)
    #include <fpu_control.h>
    void 
    fpctl_set_double_rounding(void)
    {
        // This isn't perfect, since it reportedly doesn't affect the 
        // rounding of the exponent, but it's what we've got.
        fpu_control_t ctl_word;
        _FPU_GETCW(ctl_word);
        ctl_word &= ~_FPU_EXTENDED;
        ctl_word |= _FPU_DOUBLE;
        _FPU_SETCW(ctl_word);
    }

#elif defined(__APPLE__) && defined(__i386__) && ((LONG_MAX) == 0x7fffffff)
    // As of OSX 10.4.10 on a Core Duo, the "387" FPU state defaults to
    // extended-precision rounding internally (which we don't want), but
    // the compiler seems to default to using SSE for FPU operations
    // (as if -mfpmath=sse was set), so it doesn't cause problems.  We'll
    // set the "387" to match the alpha's double-precision rounding,
    // just in case.

    // Definitions "borrowed" from linux <fpu_control.h>; uses GCC-specific
    // assembly syntax, and is kinda crufty, but seems to work as desired.
    typedef unsigned int linux_fpu_control_t;
    #define LINUX_FPU_GETCW(cw) __asm__ __volatile__ \
      ("fnstcw %0" : "=m" (*&cw))
    #define LINUX_FPU_SETCW(cw) __asm__ __volatile__ \
      ("fldcw %0" : : "m" (*&cw))
    #define LINUX_FPU_EXTENDED 0x300
    #define LINUX_FPU_DOUBLE   0x200

    void 
    fpctl_set_double_rounding(void)
    {
        linux_fpu_control_t ctl_word;
        LINUX_FPU_GETCW(ctl_word);
        ctl_word &= ~LINUX_FPU_EXTENDED;
        ctl_word |= LINUX_FPU_DOUBLE;
        LINUX_FPU_SETCW(ctl_word);
    }

#else

    void 
    fpctl_set_double_rounding(void)
    {
    }

#endif
