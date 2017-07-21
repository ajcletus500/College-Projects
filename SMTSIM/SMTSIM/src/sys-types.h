// -*- C++ -*-
//
// System-dependent stuff for specific-width integer types we rely on
//
// Jeff Brown
// $Id: sys-types.h,v 1.15.6.16.2.2.2.14 2009/11/26 21:08:24 jbrown Exp $
//

#ifndef SYS_TYPES_H
#define SYS_TYPES_H

#include <stddef.h>
#include <math.h>
#include <limits.h>


#ifdef __cplusplus
extern "C" {
#endif

// If true, STL_USE_IDENTITY_HASH causes the basic hash function used to be
// the identity function.  For implementations which use hash values mod a
// prime (e.g. g++ 3.4's library), this may be fine, rendering further hash
// work unnecessary.  For other implementations, the identify function can be
// an extremely poor choice, so this option should be switched off (defined as
// 0) here or in the compilation flags.
#ifndef STL_USE_IDENTITY_HASH
#   define STL_USE_IDENTITY_HASH 1
#endif


// Call this before using any of the stuff in this module
void systypes_init(void);


// See also, other platform dependences:
// - int-size detection in sys-types.cc
// - FP-rounding controls in fp-ctl.c
// - mremap() presence in region-alloc.cc
// - OS-dependent stuff in syscalls-includes.h (and syscalls in general, ugh)

#if defined(__alpha)
    typedef unsigned char       u8;
    typedef int                 i32;
    typedef unsigned int        u32;
    typedef long                i64;
    typedef unsigned long       u64;
    typedef unsigned long       voidp_equiv;
    #define VOIDP_EQUIV_MAX     ULONG_MAX
    #define ARITH_RIGHT_SHIFTS_SAFE     1
    #define HAVE_VSNPRINTF              0
#elif defined(__FreeBSD__)
    #include <inttypes.h>       // Hmm, fbsd ~5+ seems to have <stdint.h> too..
    typedef uint8_t             u8;
    typedef int32_t             i32;
    typedef uint32_t            u32;
    typedef int64_t             i64;
    typedef uint64_t            u64;
    typedef unsigned long       voidp_equiv;
    #define VOIDP_EQUIV_MAX     ULONG_MAX
    #define ARITH_RIGHT_SHIFTS_SAFE     1
    #define HAVE_VSNPRINTF              1
#elif defined(__linux__) || defined(__APPLE__)
    #include <stdint.h>         // C99 standard; if only anybody cared...
    typedef uint8_t             u8;
    typedef int32_t             i32;
    typedef uint32_t            u32;
    typedef int64_t             i64;
    typedef uint64_t            u64;
    typedef uintptr_t           voidp_equiv;
    // This definition of VOIDP_EQUIV_MAX is crufty, when C99 offers
    // UINTPTR_MAX; however, stdint.h may not define UINTPTR_MAX in C++
    // compilation unless __STDC_LIMIT_MACROS is defined, and if stdint.h
    // was included before this file, its double-include protection will
    // likely prevent it from having any impact here.  So, we'll punt
    // and test this assumption at startup.
    #define VOIDP_EQUIV_MAX     ULONG_MAX
    #define ARITH_RIGHT_SHIFTS_SAFE     1
    #define HAVE_VSNPRINTF              1
#else
#   error "Unhandled system type(1)."
#endif

#if LONG_MAX == 0x7fffffff
    // longs are 32-bit; assume (non-standard) long longs are 64-bit
    #define I32_LIT(x) ((i32) (x ## L))
    #define U32_LIT(x) ((u32) (x ## LU))
    #define I64_LIT(x) ((i64) (x ## LL))
    #define U64_LIT(x) ((u64) (x ## LLU))
#elif LONG_MAX == 0x7fffffffffffffff
    // longs are 64-bit
    #define I32_LIT(x) ((i32) (x))
    #define U32_LIT(x) ((u32) (x ## U))
    #define I64_LIT(x) ((i64) (x ## L))
    #define U64_LIT(x) ((u64) (x ## LU))
#else
#   error "Unhandled system type(2)."
#endif

#if !defined(VOIDP_EQUIV_MAX) || (VOIDP_EQUIV_MAX == 0)
#   error "missing VOIDP_EQUIV_MAX in pointer<->integer conversion"
#endif

#define I32_MIN I32_LIT(-0x80000000)
#define I32_MAX I32_LIT(0x7fffffff)
#define U32_MAX U32_LIT(0xffffffff)

#define I64_MIN I64_LIT(-0x8000000000000000)
#define I64_MAX I64_LIT(0x7fffffffffffffff)
#define U64_MAX U64_LIT(0xffffffffffffffff)

// Outermost i64 values which can fit in a double without loss of precision
#define I64_DOUBLE_MIN I64_LIT(-0x20000000000000)
#define I64_DOUBLE_MAX I64_LIT(0x20000000000000)        // 1<<DBL_MANT_DIG


// C99 defines a "NAN" macro as evaluating to an rvalue (defined in <math.h>).
// Some systems (such as our alphas) lack it, so we'll indirect through our
// own "SimNAN" macro.  There's similar pre-C99 disagreement with the
// definition of "isnan(x)" as a macro or a function, but this hasn't caused
// us problems yet.  We'll indirect that through a macro too, just to be safe.
extern double SysTypesDoubleNan;
#define sim_isnan(x) isnan(x)
#ifdef NAN
    #define SimNAN NAN
#else
    #define SimNAN (*((const double *) (&SysTypesDoubleNan)))
#endif


// The C language does not define the effects of right-shifting a negative
// signed integer, so if the host doesn't handle it correctly, we'll perform
// the shift on the 1s-complement value (guaranteed positive) and then take
// the 1s-complement of the result.  The type of value x must be signed!  Note
// that, for an N-bit representation of x, y must be in the range [0, N).
// These conditions are not checked.
#if ARITH_RIGHT_SHIFTS_SAFE
    #define ARITH_RIGHT_SHIFT(x, y) ((x) >> (y))
#else
    #define ARITH_RIGHT_SHIFT(x, y) \
        (((x) >= 0) ? ((x) >> (y)) : ~((~(x)) >> (y)))
#endif


// Static conditional hint macros.  For use in conditionals, e.g.
// "if (SP_F(assertion_failure)) { do_something_bad(); }".
//
// "cond" must be integer-valued, NOT a pointer or floating-point.
//
// SP_T/F = "static-predict true / false"
#if defined(__GNUC__)
#   define SP_T(cond) __builtin_expect((cond), 1)
#   define SP_F(cond) __builtin_expect((cond), 0)
#else
#   define SP_T(cond) (cond)
#   define SP_F(cond) (cond)
#endif


// A single memory address value as resides in a register
typedef u64 mem_addr;                   // see also: LongAddr, StlHashMemAddr

typedef struct OpTime {
    int latency;
    int interval;
} OpTime;


// This is called just like snprintf(), but it acts differently: it
// unconditionally aborts or otherwise dies if the target buffer is too small.
int e_snprintf(char *str, size_t size, const char *format, ...);


// These format various types of values for text output, producing
// nul-terminated strings.
// They return pointers into statically allocated storage, which is recycled
// for future invocations.  This has important implications:
//  - The returned string need not be freed, and no NULL check is needed.
//  - The string returned by invocation N of one of these routines will be
//    overwritten by invocation N + MAX_CONCURRENT_FMTS, which is set in 
//    sys-types.cc.  If you need one of these strings to live longer, you'll
//    have to make your own copy.
//  - These were written for use in printf argument lists, with %s formats.
//    As long as you don't use a ridiculously large number of them in a single
//    printf, you'll be fine.
const char *fmt_i64(i64 val);
const char *fmt_u64(u64 val);
const char *fmt_x64(u64 val);
const char *fmt_mem(mem_addr val);

const char *fmt_bool(int bool_value);

// Format a value for output with SI prefixes, using at most the given number
// of significant digits.  fmt_si10() uses metric prefixes, while fmt_si2()
// uses binary-based prefixes.  If sig_figs <= 0, no rounding is done.
const char *fmt_si10(double val, int sig_figs);
const char *fmt_si2(double val, int sig_figs);

// These parse strings into 64-bit integers.  They return nonzero on error.
// No whitespace is tolerated.  The "base" parameter has the same semantics
// as that of the standard strtol() function.
int str2num_i64(i64 *result, const char *str, int base);
int str2num_u64(u64 *result, const char *str, int base);


// This converts 64-bit values to/from native generic pointers, checking that
// it fits in the native pointer type.
void *u64_to_ptr(u64 val);
u64 u64_from_ptr(const void *ptr);

// These convert i64s to ints/i32s, ensuring that the values "fit"
int i64_to_int(i64 val);
i32 i64_to_i32(i64 val);
long i64_to_long(i64 val);

i64 double_to_i64(double val);
double i64_to_double(i64 val);

// These do simple hashing on integer/pointer values.  They produce "useful"
// hash values regardless of STL_USE_IDENTITY_HASH.
size_t hash_u32(u32 key);
size_t hash_u64(u64 key);
size_t hash_voidp(const void *key);


#ifdef __cplusplus
}       // extern "C"
#endif



#ifdef __cplusplus
    // C++-specific functors for hashing specific value types.

    // 32 / 64-bit hashers which always generate interesting hash values,
    // regardless of STL_USE_IDENTITY_HASH
    struct HashU32 {
        inline size_t operator() (u32 key) const {
            // Referred to as "Thomas Wang's 32 bit Mix Function", from
            //  http://www.concentric.net/~Ttwang/tech/inthash.htm 
            key += ~(key << 15);
            key ^=  (key >> 10);
            key +=  (key << 3);
            key ^=  (key >> 6);
            key += ~(key << 11);
            key ^=  (key >> 16);
            return static_cast<size_t>(key);
        }
    };

    struct HashU64 {
        inline size_t operator() (u64 key) const {
            // "Thomas Wang's 64 bit Mix Function"
            key += ~(key << 32);
            key ^=  (key >> 22);
            key += ~(key << 13);
            key ^=  (key >> 8);
            key +=  (key << 3);
            key ^=  (key >> 15);
            key += ~(key << 27);
            key ^=  (key >> 31);
            return static_cast<size_t>(key);
        }
    };

    // 32 / 64-bit hashers specifically for indexing quasi-STL hash
    // containers; sometimes these are just the identity function.
    #if STL_USE_IDENTITY_HASH
        struct StlHashU32 {
            inline size_t operator() (u32 key) const {
                return static_cast<size_t>(key);
            }
        };
        struct StlHashU64 {
            inline size_t operator() (u64 key) const {
                return static_cast<size_t>(key);
            }
        };
    #else
        typedef HashU32 StlHashU32;
        typedef HashU64 StlHashU64;
    #endif      // STL_USE_IDENTITY_HASH

    // At least some g++'s don't have a hash<> specialization for pointer
    // value hashing.  Beyond that, we shouldn't rely on hash<> until it makes
    // into some sort of standard, so we'll get out and walk.
    struct StlHashVoidPtr {
        inline size_t operator() (const void *key) const {
        #if VOIDP_EQUIV_MAX <= 0xffffffff
            StlHashU32 h;
            u32 key_cast = reinterpret_cast<u32>(key);
        #elif VOIDP_EQUIV_MAX <= 0xffffffffffffffff
            StlHashU64 h;
            u64 key_cast = reinterpret_cast<u64>(key);
        #else
            #error "unhandled pointer<->integer size conversion"
        #endif
            return h(key_cast);
        }
    };

    // Some g++ setups don't have a hash<> specialization that matches our
    // mem_addr typedef, e.g. OSX 10.5.5 / g++-4.0.1 / 32-bit x86; so, we'll
    // provide a functor to do that.
    typedef StlHashU64 StlHashMemAddr;

    // Generic hash adapter that hashes raw pointer values.  The pointer must
    // be converable to void *.
    template <typename to_hash>
    struct StlHashPointerValue {
        size_t operator() (const to_hash ptr) const {
            StlHashVoidPtr h;
            return h(static_cast<const void *>(ptr));            
        }
    };
#endif  // __cplusplus


// Now that we've got our types and hashing-bits defined, let's include
// the LongAddr types and definitions, since it's used in so many places.
#include "long-addr.h"


#endif  // SYS_TYPES_H
