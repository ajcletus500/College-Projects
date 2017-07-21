//
// Miscellaneous utility functions for SMTSIM
//
// Jeff Brown
// $Id: utils.h,v 1.11.6.16.2.2.2.17 2009/12/04 20:12:55 jbrown Exp $
//

#ifndef UTILS_H
#define UTILS_H
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

int floor_log2(u64 val, int *inexact_ret);
int log2_exact(u64 val);                // <0 <=> val not a power of 2
void *emalloc(size_t size);
void *emalloc_zero(size_t size);
void *erealloc(void *mem, size_t new_size);
int file_readable(const char *filename);
void *efopen(const char *filename, int truncate_not_read);
void efclose(void *file_handle, const char *err_filename);
void renice(int level);
void disable_coredump(void);
char *e_strdup(const char *src);

char **strarray_dup_e(char * const *src);
int strarray_size(char * const *src);
void strarray_free(char **src);

// Scans enum_table for search string, return index if found, -1 if not.
// enum_table must be terminated by a NULL pointer.
int enum_lookup(const char * const *enum_table, const char *str);

// These return non-NULL (they exit on failure)
const char *hostname_short(void);
const char *hostname_long(void);


// These manage a string that's something like argv[0], for printing.
// get_argv0() returns a pointer to a copy of the last string passed to
// set_argv0(), or a default string if set_argv0() hasn't been called.  The
// pointer will remain valid until the next call to set_argv0().
const char *get_argv0(void);
void set_argv0(const char *new_argv0);


// Function-like macro, just gripes and aborts if called.
// (The peculiar line continuation, spacing, and underscores are to ease text
// searches for uses of this name, perhaps with a following open-paren)
#define stub\
() s_t_u_b_abort(__FILE__, __LINE__)
int s_t_u_b_abort(const char *file, int line);

// Print an error message and abort; like a non-conditional assertion failure.
// (There are no variadic macros in C++ though, sigh)
#define abort_printf \
    if (abort_printf_handle1(__FILE__, __LINE__)) { } else abort_printf_handle2
// Print an error message and exit.
#define exit_printf \
    if (abort_printf_handle1(__FILE__, __LINE__)) { } else exit_printf_handle2
// Print an error message
#define err_printf \
    if (abort_printf_handle1(__FILE__, __LINE__)) { } else err_printf_handle2
// Helpers for above macros
int abort_printf_handle1(const char *file, int line);   // Returns 0
void abort_printf_handle2(const char *format, ...);
void exit_printf_handle2(const char *format, ...);
void err_printf_handle2(const char *format, ...);


// "Return" the number of elements in a statically-sized array
#define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))

// Predicate: array index is in [0..limit); multiple evaluations!
#define IDX_OK(idx, limit) (((idx) >= 0) && ((idx) < (limit)))

// Index a flat 1D array, as if it was 2D
#define IDX_2D(row, col, n_cols) ((row) * (n_cols) + (col))

// Predicate: "value" has a numeric value that lies within the range defined
// for the enum type "type".  This assumes that "type" is the name of an enum
// type whose valid members are sequentially defined in the range from
// [0 .. <type>_last) -- that is, there is a symbol named "<type>_last" which
// holds the first invalid value.  (This symbol is not created automatically,
// but placing a suitably-named symbol at the end of the enum declaration is a
// convention used in the simulator.)
#define ENUM_OK(type, value) IDX_OK((value), type ## _last)

// Given an enum type and value, evaluates to a string suitable for
// printing.  The enum type must fulfill the criteria described for ENUM_OK(),
// and additionally there must be an array "const char *<type>_names[]"
// with <type>_last elements (the final element being a NULL pointer).
// Out of range values evaluate to "invalid", allowing for unconditional
// printing.
#define ENUM_STR(type, value) \
    (ENUM_OK(type, (int)(value)) ? type ## _names[(int)(value)] : "invalid")

// Given an enum type and a string, evaluates to an integer value
// for which ENUM_STR would evaluate to a matching string.  Evaluates to
// -1 if no match is found.  The enum type must fulfill the criteria for
// ENUM_TOSTR().
#define ENUM_FROMSTR(type, str) (enum_lookup(type ## _names, (str)))

// Print an error message reporting a value, contained in a variable, which
// doesn't match an expected enum value.  Useful for "default" case
// statements.  ("type" and "variable" should not be quoted.)
#define ENUM_ABORT(type, variable) \
    abort_printf("unmatched value for enum %s, %s: %d (%s)\n", \
                 #type, #variable, (int) (variable), \
                 ENUM_STR(type, variable));

// Ceiling-to-multiple: round "val" up to the next multiple of "mult"
//#define CEIL_MULT(x, y) (((i64) x + (y-1)) & ~(y-1)): y a power of 2
i64 ceil_mult_i64(i64 val, i64 mult);


/* Cheesy inline macro version */
#define bill_resource_time(done_time_ret, next_time_ref, now, op_time) do {\
    if ((next_time_ref) < (now)) { (next_time_ref) = (now); } \
    (done_time_ret) = (next_time_ref) + (op_time).latency; \
    (next_time_ref) += (op_time).interval; \
} while(0)


// Extract bits<offset+width-1:offset> of a value.  Width must be less
// than the total width of "val".  Result unsigned.
#define GET_BITS_32(val, offset, width) \
    (((val) >> (offset)) & ((U32_LIT(1) << (width)) - 1))

#define GET_BITS_64(val, offset, width) \
    (((val) >> (offset)) & ((U64_LIT(1) << (width)) - 1))

// Extract bits<left_offs:right_offs> of a value.
#define GET_BITS_IDX(val, left_offs, right_offs) \
    GET_BITS_64((val), (right_offs), (left_offs) - (right_offs) + 1)


// Generate unsigned value with a single bit set; doesn't handle offset>=width
#define SET_BIT_32(offset) (U32_LIT(1) << (offset))
#define SET_BIT_64(offset) (U64_LIT(1) << (offset))


// Generate unsigned values with <width> consecutive bits set, shifted
// left <offset> bits from bit 0.  <width> MUST be less than the width
// of the underlying type.
#define BIT_MASK_32(offset, width) (((U32_LIT(1) << (width)) - 1) << (offset))
#define BIT_MASK_64(offset, width) (((U64_LIT(1) << (width)) - 1) << (offset))
// Create bit masks with bits <left_offs:right_offs> set
#define BIT_MASKIDX_32(left_offs, right_offs) \
    BIT_MASK_32((right_offs), (left_offs) - (right_offs) + 1)
#define BIT_MASKIDX_64(left_offs, right_offs) \
    BIT_MASK_64((right_offs), (left_offs) - (right_offs) + 1)


// Compare signed or unsigned scalar values, producing -1/0/1, ala Perl's <=>
#define CMP_SCALAR(v1, v2) (((v1) < (v2)) ? -1 : (((v2) < (v1)) ? 1 : 0))

// Typical min/max select macros
#define MIN_SCALAR(a, b) (((a) <= (b)) ? (a) : (b))
#define MAX_SCALAR(a, b) (((a) >= (b)) ? (a) : (b)) 

// Clamp a scalar value x to be in [low_lim,high_lim).
// (precondition: high_lim > low_lim, all values compatible)
#define CLAMP_SCALAR(x, low_lim, high_lim) \
    (((x) < (low_lim)) ? (low_lim) : \
        (((x) < (high_lim)) ? (x) : ((high_lim) - 1)))

// Increment or decrement an integer in [0..lim), with wraparound or
// saturation.  These tend to optimize well.  Warning: multiple arg evaluation.
// (precondition: x an integer in [0..lim), and lim fits in type)
#define INCR_WRAP(x, lim) ((((x) + 1) < (lim)) ? ((x) + 1) : 0)
#define DECR_WRAP(x, lim) ((x) ? ((x) - 1) : ((lim) - 1))
#define INCR_SAT(x, lim) ((((x) + 1) < (lim)) ? ((x) + 1) : (x))
#define DECR_SAT(x) ((x) ? ((x) - 1) : (x))

// Convert some arbitrary truth-compatible value to 1 or 0
#define TO_BOOL(x) ((x) ? 1 : 0)


// C++ lacks variadic macros, so we'll abuse syntax a bit here; this
// may generate "ambiguous-else" warnings when instantiated in conditionals
// without surrounding braces, but it does act as expected.
#define DEBUGPRINTF if (!debug) { } else printf
#define DEBUGPRINTF_cache printf

// This bit of ugliness strips away pointer qualifiers, and is useful for
// getting rid of "cast-away-const" warnings (since C lacks const_cast).  This
// must only be used for pointers (though really, it shouldn't be used at
// all).  It should have negligible-to-no runtime overhead.
#define C_UNCHECKED_CAST(target_pointer_type, val) \
    ((target_pointer_type) ((voidp_equiv) (val)))


// DEBUGPRINTF & "debug".  These don't fit too well in this module, but
// they're used so widely -- don't bring in extra dependences -- it's not
// worth the pain of moving them.  We use an explicit value instead of a 
// smarter function, because the flag is tested so very widely.
//
// (I'd prefer DEBUG to be always #defined to 0 or 1, and "debug" to be
// capitalized to hint at their special status.  It's not worth breaking other
// folks' code against earlier versions, though.)
#ifdef DEBUG
    // Storage defined in utils.c
    extern int debug;
#else
    // Define these as constant rvalues, so that 1) the compiler has a decent
    // shot at optimizing things away, and 2) they're not somehow changed.
    #define debug 0
#endif


#ifdef __cplusplus
}       // closes extern "C" block
#endif

#endif  /* UTILS_H */
