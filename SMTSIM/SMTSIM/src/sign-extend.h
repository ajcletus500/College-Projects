/*
 * Fun sign-extension code
 *
 * Jeff Brown
 * $Id: sign-extend.h,v 1.2.6.2 2007/08/22 19:55:17 jbrown Exp $
 */

#ifndef SIGN_EXTEND_H
#define SIGN_EXTEND_H

#ifdef __cplusplus
extern "C" {
#endif


// Sign-extend a w-bit value x to 32 or 64 bits.
// The type of the resulting value is signed, so it may be implicitly extended
// by other operations. 
//
// The following conditions must be met for correct operation:
//  (1) w > 0, w <= width(typeof result)
//  (2) width(typeof x) <= 32
//  (3) Your C implementation must sign-extend right shifts of signed negative
//      values.  (Note: not needed for fancier right-shift-free version.)
//  (4) Your C implementation must left-shift signed negative values as if they
//      were unsigned.
//  (5) Casts between signed negative values and unsigned types of the same
//      or lower width do not change the bit representation of the bits which
//      fit in the target type.
//  (6) x < 2^w, that is, all bits to the left of those involved in the
//      sign-extension must be zero.  (This is needed for the fancier
//      right-shift-free version, in particular.)

#if 0
  // Traditional smtsim macros: shift value left, then back right, relying
  // on signed integer right-shift to do the extension.  This ignores and 
  // overwrites any bits in "x" to the left of "w".
  #define SEXT_TO_i32(x, w) ((((i32) (x)) << (32 - (w))) >> (32 - (w)))
  #define SEXT_TO_i64(x, w) ((((i64) (x)) << (64 - (w))) >> (64 - (w)))
#else
  // A fancier, branch and right-shift-free sign extension method:
  //   sext = 1 << (len - 1)
  //   i = (i ^ sext) - sext
  // (Henry S. Warren Jr., CACM v20 n6 June 1977)
  //
  // The left shift operates only on the source bit-width, which is likely a
  // constant.  These will NOT work if "x" has bits to left of "w" set.
  #define SEXT_TO_i32(x, w) \
    ((((i32) (x)) ^ (((i32) 1) << ((w) - 1))) - (((i32) 1) << ((w) - 1)))
  #define SEXT_TO_i64(x, w) \
    ((((i64) (x)) ^ (((i64) 1) << ((w) - 1))) - (((i64) 1) << ((w) - 1)))
#endif

// Special case of above, used by several alpha-"long" emulate routines
#define SEXT32_i64(x) ((i64) ((i32) (x)))

// Used mainly in print.c for reporting offsets
#define SEXT16_i64(x) (SEXT_TO_i64((x), 16))


#ifdef __cplusplus
}
#endif

#endif  /* SIGN_EXTEND_H */
