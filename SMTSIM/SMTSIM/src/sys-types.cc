/*
 * System-dependent stuff for specific-width integer types we rely on
 *
 * Jeff Brown
 * $Id: sys-types.cc,v 1.5.6.18.2.2.2.12 2009/12/06 09:17:14 jbrown Exp $
 */

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <limits>
#include <string>

#include "sim-assert.h"
#include "sys-types.h"
#include "long-addr.h"
#include "sign-extend.h"
#include "fp-ctl.h"
// Try to keep simulation-specific #includes (e.g. main.h, context.h) out of
// here; this module is for mostly type-support code.


// See also: similar cruft in sys-types.h
#if (LONG_MAX) == 0x7fffffff
    // longs are 32-bit; assume (non-standard) long longs are 64-bit
#   define FMT_64_DEC "lld"
#   define FMT_U64_DEC "llu"
#   define FMT_U64_HEX "llx"
#   define STR2I64_FUNC strtoll
#   define STR2U64_FUNC strtoull
#elif LONG_MAX == 0x7fffffffffffffff
    // longs are 64-bit
#   define FMT_64_DEC "ld"
#   define FMT_U64_DEC "lu"
#   define FMT_U64_HEX "lx"
#   define STR2I64_FUNC strtol
#   define STR2U64_FUNC strtoul
#else
#   error "Unhandled system type(3)."
#endif


#define MAX_FMT_LEN             30
#define MAX_CONCURRENT_FMTS     100


// copied from "utils.h": it's simple enough, and helps keep us independent
#ifndef NELEM
#  define NELEM(x) ((int) (sizeof(x) / sizeof((x)[0])))
#endif


double SysTypesDoubleNan = std::numeric_limits<double>::quiet_NaN();


namespace {

static struct {
    char str[MAX_CONCURRENT_FMTS][MAX_FMT_LEN + 1];
    int next_entry;             /* The next string to use */
} SharedBuff;

struct SignExtTestCase {
    u64 input;
    int in_width;
    u64 output;
};

struct RoundingTestCase {
    FPCtlRoundingMode rounding_mode;
    unsigned expected_mismatches;       // bit-set
};

}


static char *
get_dest(void)
{
    char *dest = SharedBuff.str[SharedBuff.next_entry];
    SharedBuff.next_entry++;
    if (SharedBuff.next_entry == MAX_CONCURRENT_FMTS)
        SharedBuff.next_entry = 0;
    return dest;
}


static void
report_failed_sext(const char *name, int case_num,
                   const SignExtTestCase& test, u64 badval)
{
    fprintf(stderr, "sext test failed, %s case %d: "
            "input (0x%s,%d), wanted 0x%s, got 0x%s\n", 
            name, case_num, fmt_x64(test.input), test.in_width,
            fmt_x64(test.output), fmt_x64(badval));
}


static int
sign_extension_ok(void)
{
    int ok = 1;

    // Test the sign-extension macros
    // SEXT_TO_32, SEXT_TO_64, SEXT32_64, SEXT16_64

    {   // SEXT_TO_i64
        SignExtTestCase tests[] = {
            { U64_LIT(0x0), 1, 0 },
            // { U64_LIT(0x2), 1, 0 },  // Shady: source value too wide
            { U64_LIT(0x1), 1, U64_MAX },
            { U64_LIT(0x1), 2, 1 },
            { U64_LIT(0x2), 2, U64_MAX - 1 },
            { U64_LIT(0x3abcdefe), 31, U64_LIT(0x3abcdefe) },
            { U64_LIT(0x7abcdefe), 31, U64_LIT(0xfffffffffabcdefe) },
            { U64_LIT(0x70000001), 32, U64_LIT(0x70000001) },
            { U64_LIT(0x80000001), 32, U64_LIT(0xffffffff80000001) },
            { U64_LIT(0x30123456789abcde), 63, U64_LIT(0x30123456789abcde) },
            { U64_LIT(0x70123456789abcde), 63, U64_LIT(0xf0123456789abcde) },
            { U64_LIT(0x7000000000000001), 64, U64_LIT(0x7000000000000001) },
            { U64_LIT(0x8000000000000001), 64, U64_LIT(0x8000000000000001) },
        };
        for (int i = 0; i < NELEM(tests); i++) {
            u64 out = SEXT_TO_i64(tests[i].input, tests[i].in_width);
            if (out != tests[i].output) {
                report_failed_sext("SEXT_TO_i64", i, tests[i], out);
                ok = 0;
            }
        }
    }

    {   // SEXT_TO_i32
        SignExtTestCase tests[] = {
            { U64_LIT(0x0), 1, 0 },
            // { U64_LIT(0x2), 1, 0 },  // Shady: source value too wide
            { U64_LIT(0x1), 1, U64_LIT(0xffffffff) },
            { U64_LIT(0x1), 2, 1 },
            { U64_LIT(0x2), 2, U64_LIT(0xfffffffe) },
            { U64_LIT(0x3abcdefe), 31, U64_LIT(0x3abcdefe) },
            { U64_LIT(0x7abcdefe), 31, U64_LIT(0xfabcdefe) },
            { U64_LIT(0x70000001), 32, U64_LIT(0x70000001) },
            { U64_LIT(0x80000001), 32, U64_LIT(0x80000001) },
        };
        for (int i = 0; i < NELEM(tests); i++) {
            // Cast to u32: prevent further sign-extension of the result
            u64 out = static_cast<u32>(SEXT_TO_i32(tests[i].input, 
                                                   tests[i].in_width));
            if (out != tests[i].output) {
                report_failed_sext("SEXT_TO_i32", i, tests[i], out);
                ok = 0;
            }
        }
    }

    {   // SEXT32_i64
        SignExtTestCase tests[] = {
            { U64_LIT(0x7abcdab5), 0, U64_LIT(0x7abcdab5) },
            { U64_LIT(0x17abcdab5), 0, U64_LIT(0x7abcdab5) },
            { U64_LIT(0xffffffff7abcdab5), 0, U64_LIT(0x7abcdab5) },
            { U64_LIT(0xeabcdab5), 0, U64_LIT(0xffffffffeabcdab5) },

        };
        for (int i = 0; i < NELEM(tests); i++) {
            u64 out = SEXT32_i64(tests[i].input);
            if (out != tests[i].output) {
                report_failed_sext("SEXT32_i64", i, tests[i], out);
                ok = 0;
            }
        }
    }

    {   // SEXT16_i64
        SignExtTestCase tests[] = {
            { U64_LIT(0x7ab5), 0, U64_LIT(0x7ab5) },
            // { U64_LIT(0x17ab5), 0, U64_LIT(0x7ab5) },  // Shady
            { U64_LIT(0xeab5), 0, U64_LIT(0xffffffffffffeab5) },

        };
        for (int i = 0; i < NELEM(tests); i++) {
            u64 out = SEXT16_i64(tests[i].input);
            if (out != tests[i].output) {
                report_failed_sext("SEXT16_i64", i, tests[i], out);
                ok = 0;
            }
        }
    }

    return ok;
}


// Silently return a bit-set of tests which mismatch the values expected
// for double-precision, round-to-nearest operation.
static unsigned
rounding_mismatch_set(void)
{
    const char *fname = "rounding_mismatch_set";
    unsigned mismatch_tests = 0;
    {
        // Test #1, bit 0:
        // Test derived from: http://www.vinc17.org/research/extended.en.html
        // and http://www.vinc17.org/software/tst-ieee754.c
        // (Catches "round-up" case, too)
        volatile double x, y, z;
        int d;
        x = 9007199254740994.0; // 2^53 + 2
        y = 1.0 - 1/65536.0;
        z = x + y;
        d = (int) floor(z - x); // Expect: 0 for double-prec., 2 for extended
        if (d != 0) {
            mismatch_tests |= (1<<0);
            if (d != 2)
                fprintf(stderr, "%s: FP rounding "
                        "test value 1 (%d) unexpected\n", fname, d);
        }
    }
    {
        // Test #2, bit 1:
        // Negative version: catch "round-down" case
        volatile double x, y, z;
        int d;
        x = -9007199254740994.0; // 2^53 + 2
        y = -1.0 + 1/65536.0;
        z = x + y;
        d = (int) floor(z - x);
        if (d != 0) {
            mismatch_tests |= (1<<1);
            if (d != -2)
                fprintf(stderr, "%s: FP rounding "
                        "test value 2 (%d) unexpected\n", fname, d);
        }
    }
    {
        // Test #3, bit 2:
        // Copy/paste, tweaked to catch "round to zero" case.
        volatile double x, y, z;
        int d;
        x = -9007199254740994.0;
        y = 1.0 - 1/65536.0;
        z = x + y;
        d = (int) floor(z - x);
        if (d != 0) {
            mismatch_tests |= (1<<2);
            if (d != 2)
                fprintf(stderr, "%s: FP rounding "
                        "test value 3 (%d) unexpected\n", fname, d);
        }
    }
    return mismatch_tests;
}


// Verify that FP rounding is double-precision, and acts like expected
// in all modes.
//
/// Observed rounding_mismetch_set() sets so far:
//
//                     RoundToNearest  RoundToZero  RoundUp  RoundDown
// linux-x86,default            1,2,3            3      1,3          2
// linux-x86,double                OK            3      1,3          2
// linux-amd64                     OK            3      1,3          2
// osf-alpha                       OK            3      1,3          2
// freebsd-x86                     OK            3      1,3          2
// osx-x86/32 (default: sse?)      OK            3      1,3          2
// osx-x86/32 -mfpmath=387      1,2,3            3      1,3          2
//
static bool
rounding_test_ok(void)
{
    const char *fname = "rounding_test_ok";
    bool ok = true;
    RoundingTestCase tests[] = {
        { FPCtl_RoundToNearest, 0 },
        { FPCtl_RoundToZero, (1<<2) },
        { FPCtl_RoundUp, (1<<2)|(1<<0) },
        { FPCtl_RoundDown, (1<<1) }
    };

    FPCtlRoundingMode saved_mode = fpctl_getround();
    for (int i = 0; i < NELEM(tests); ++i) {
        fpctl_setround(tests[i].rounding_mode);
        unsigned mismatches = rounding_mismatch_set();
        if (mismatches != tests[i].expected_mismatches) {
            ok = false;
            fprintf(stderr, "%s: test-set mismatch for rounding mode %s; "
                    "expected mismatch set 0x%x, got 0x%x\n", fname,
                    FPCtlRoundingMode_names[tests[i].rounding_mode],
                    tests[i].expected_mismatches, mismatches);
        }
    }
    fpctl_setround(saved_mode);

    if (!ok) {
        fprintf(stderr, "%s: FP computation isn't rounding as "
                "expected; results may be altered\n", fname);
    }
    return ok;
}


// Wrapper: calls vsnprintf(), acts like snprintf() for easy testing
static int
test_vsnprintf(char *str, size_t size, const char *format, ...)
{
#if HAVE_VSNPRINTF
    va_list ap;
    int used;
    va_start(ap, format);
    used = vsnprintf(str, size, format, ap);
    va_end(ap);
    return used;
#else
    sim_abort();
    return 0;
#endif  // HAVE_VSNPRINTF
}



// Warning: watch out for MAX_CONCURRENT_FMTS limits in this routine
void 
systypes_init(void)
{
    const char *fname = "systypes_init";
    int fail = 0;
    const struct {
        const char *name;
        int wanted_bits;
        int actual_bits;
    } type_sizes[] = {
        { "u8",   8, 8*sizeof(u8)  },
        { "i32", 32, 8*sizeof(i32) },
        { "u32", 32, 8*sizeof(u32) },
        { "i64", 64, 8*sizeof(i64) },
        { "u64", 64, 8*sizeof(u64) },
    };
    const struct {
        const char *name;
        const char *wanted_str;
        const char *actual_str;
    } fmt_tests[] = {
        { "i64_zero", "0"                   , fmt_i64(0) },
        { "i64_max" , "9223372036854775807" , fmt_i64(I64_MAX) },
        { "i64_min" , "-9223372036854775808", fmt_i64(I64_MIN) },
        { "u64_zero", "0"                   , fmt_u64(0) },
        { "u64_max" , "18446744073709551615", fmt_u64(U64_MAX) },
        { "x64_zero", "0"                   , fmt_x64(0) },
        { "x64_max" , "ffffffffffffffff"    , fmt_x64(U64_MAX) },
    };

    // Make sure the fixed-bit-width types are as expected
    for (int i = 0; i < NELEM(type_sizes); i++) {
        if (type_sizes[i].actual_bits != type_sizes[i].wanted_bits) {
            fprintf(stderr, "%s: type size mismatch for \"%s\": expected "
                    "%d bits, got %d\n", fname, type_sizes[i].name,
                    type_sizes[i].wanted_bits, type_sizes[i].actual_bits);
            fail = 1;
        }
    }

    // If we're promising vsnprintf(), make sure it's linkable
    if (HAVE_VSNPRINTF) {
        char test_buff[20];
        const char *expected = "12345";
        int used1, used2;
        used1 = test_vsnprintf(test_buff, sizeof(test_buff), "%d", 12345);
        if (used1 != 5) {
            fprintf(stderr, "%s: vsnprintf used1 mismatch, expected %d, "
                    "got %d\n", fname, 5, used1);
            fail = 1;
        }
        if (strcmp(test_buff, expected) != 0) {
            fprintf(stderr, "%s: vsnprintf output mismatch, expected \"%s\", "
                    "got \"%s\"\n", fname, expected, test_buff);
            fail = 1;
        }
        used2 = test_vsnprintf(test_buff, 0, "%d", 30678);
        if (used2 != used1) {
            fprintf(stderr, "%s: vsnprintf used2 mismatch, expected %d, "
                    "got %d\n", fname, used1, used2);
            fail = 1;
        }
        if (strcmp(test_buff, expected) != 0) {
            fprintf(stderr, "%s: vsnprintf zero-buffer overwrite detected\n",
                    fname);
            fail = 1;
        }
    }

    // Make sure width-dependent print routines work
    for (int i = 0; i < NELEM(fmt_tests); i++) {
        if (strcmp(fmt_tests[i].actual_str, fmt_tests[i].wanted_str)) {
            fprintf(stderr, "%s: format mismatch for \"%s\": expected \"%s\", "
                    "got \"%s\"\n", fname, fmt_tests[i].name,
                    fmt_tests[i].wanted_str, fmt_tests[i].actual_str);
            fail = 1;
        }
    }

    // Ensure VOIDP_EQUIV_MAX is set correctly
    {
        voidp_equiv void_max = std::numeric_limits<voidp_equiv>::max();
        if (void_max != VOIDP_EQUIV_MAX) {
            fprintf(stderr, "%s: VOIDP_EQUIV_MAX value mismatch\n", fname);
            fail = 1;
        }
    }

    // Make sure that LongAddr sizes match between C and C++, since we freely
    // pass LongAddr values between the two.  (We could also check individual
    // field sizeof()/offsetof() values, but hopefully that's not necessary.)
    {
        int c_size = SizeOfLongAddrFromC, cpp_size = sizeof(LongAddr);
        if (c_size != cpp_size) {
            fprintf(stderr, "%s: type size mismatch for LongAddr: "
                    "%d bytes within C++, %d bytes within C\n",
                    fname, cpp_size, c_size);
            fail = 1;
        }
    }

    // Check our NAN-related setup
    {
        double some_nan = SimNAN, not_nan = 0.0;
        if (!sim_isnan(some_nan) || sim_isnan(not_nan)) {
            fprintf(stderr, "%s: SimNAN/sim_isnan() failure\n", fname);
            fail = 1;
        }
        if (!sim_isnan(SysTypesDoubleNan)) {
            fprintf(stderr, "%s: SysTypesDoubleNan failure\n", fname);
            fail = 1;
        }
        // disable comparison test: give optimizer some leeway
        if (0 && (some_nan == some_nan)) {      // NaN is not <,==,> itself
            fprintf(stderr, "%s: SimNAN compare-equal failure\n", fname);
            fail = 1;
        }
    }
      
    // Check endian-ness: syscalls currently depend on it
    {
        u32 src_val = U32_LIT(0x31323334);              // "1234"
        u8 *bytes = reinterpret_cast<u8 *>(&src_val);
        u32 test_val = 0;               // Read from mem in big-endian order
        for (int i = 0; i < 4; i++)
            test_val = (test_val << 8) | bytes[i];
        if (test_val == src_val) {
            fprintf(stderr, "%s: big-endian memory order detected; "
                    "SMTSIM syscalls currently don't work with this :(\n",
                    fname);
            fail = 1;
        } else if (test_val != 0x34333231) {
            fprintf(stderr, "%s: unrecognized memory order detected; "
                    "wrote 0x%lx, read 0x%lx.  SMTSIM syscalls currently "
                    "don't work with this :(\n", fname, 
                    (unsigned long) src_val, 
                    (unsigned long) test_val);
            fail = 1;
        }
    }

    if (ARITH_RIGHT_SHIFTS_SAFE) {
        i64 src1 = U64_LIT(0x42468ace2468ace2), // Signed, positive
            out1 = U64_LIT(0x2123456712345671);
        i64 src2 = U64_LIT(0x82468ace2468ace2), // Signed, negative
            out2 = U64_LIT(0xc123456712345671);
        i64 tst1 = src1 >> 1;
        i64 tst2 = src2 >> 1;
        if ((tst1 != out1) || (tst2 != out2)) {
            fprintf(stderr, "%s: ARITH_RIGHT_SHIFTS_SAFE test failed; "
                    "%s != %s, or %s != %s\n", fname, 
                    fmt_x64(tst1), fmt_x64(out1),
                    fmt_x64(tst2), fmt_x64(out2));
            fail = 1;
        }
    }

    {
        i64 src1 = U64_LIT(0x42468ace2468ace2), // Signed, positive
            out1 = U64_LIT(0x2123456712345671);
        i64 src2 = U64_LIT(0x82468ace2468ace2), // Signed, negative
            out2 = U64_LIT(0xc123456712345671);
        i64 tst1 = ARITH_RIGHT_SHIFT(src1, 1);
        i64 tst2 = ARITH_RIGHT_SHIFT(src2, 1);
        if ((tst1 != out1) || (tst2 != out2)) {
            fprintf(stderr, "%s: ARITH_RIGHT_SHIFT test failed; "
                    "%s != %s, or %s != %s\n", fname, 
                    fmt_x64(tst1), fmt_x64(out1),
                    fmt_x64(tst2), fmt_x64(out2));
            fail = 1;
        }
    }

    if (!sign_extension_ok())
        fail = 1;

    // Force host environment to double-precision FP rounding.  (On some OSes,
    // x87 math defaults to using 80-bit extended-precision for operands that
    // happen to be on the FPU stack, only rounding when they're spilled to
    // memory.  This causes mismatches with Alpha math.)
    fpctl_set_double_rounding();

    if (!rounding_test_ok())
        fail = 1;

    // Round-to-nearest is the default on alphas with DEC cc, per manpage
    // WARNING: Much simulator code silently assumes that RoundToNearest is in
    // effect, so be very careful if changing the rounding mode; make sure you
    // restore it.  EMULATE_FLTI_PROLOGUE() in particular relies on this being
    // the default!
    fpctl_setround(FPCtl_RoundToNearest);

    {
        const char *test_env = getenv("SMTSIM_IGNORE_TYPEFAIL");
        int ignore_fail = test_env && (test_env[0] != '\0');
        if (ignore_fail) {
            fprintf(stderr, "%s: WARNING: IGNORING type-init failures; "
                    "probably a bad idea\n", fname);
        }
        if (fail) {
            fprintf(stderr, "%s (%s:%i): host type/op checking failed.\n",
                    fname, __FILE__, __LINE__);
            if (!ignore_fail) {
                sim_abort();
            }
        }
    }
}


// (Moved here from "utils" module, since it's pretty system-dependent with
// HAVE_VSNPRINTF.)
int 
e_snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    int used;

    va_start(ap, format);
#if HAVE_VSNPRINTF
    used = vsnprintf(str, size, format, ap);
    sim_assert(used >= 0);      // shouldn't have output errors, to memory
    if (size_t(used) >= size) {
        // We don't *really* have to abort here, since data hasn't been
        // overwritten, but we should; otherwise, bad buffer-size assumptions
        // will linger until some day when HAVE_VSNPRINTF isn't available,
        // then rise up to bite us.
        fflush(NULL);
        fprintf(stderr, "e_snprintf (%s:%d): target buffer too small, "
                "format %s (%ld >= %ld)\n", __FILE__, __LINE__,
                format, (long) used, (long) size);
        sim_abort();
    }
#else
    // vsprintf() is ANSI C89
    used = vsprintf(str, format, ap);
    sim_assert(used >= 0);      // shouldn't have output errors, to memory
    if (size_t(used) >= size) {
        // Oh no, we've done it now: we've overflowed the target buffer,
        // and likely overwritten some stack or heap data.  At this point,
        // we just want to get stopped with a minimum of additional harm.
        //
        // Calling additional complicated functions like *printf, sim_abort()
        // which may fork for a stack trace, etc., is risky.  So is raising
        // any signals which may be caught in this same process.  We'll
        // grunt out a crude error message, and then do our best to die.
        // 
        // Note that this still isn't "security" safe, it's just trying to
        // robustly catch simulator blunders.

// Helper macros: direct-stringify, and stringify-after-argument-expansion
#define TOSTRING(x) #x
#define EXP_TOSTRING(x) TOSTRING(x)

        int err_fd = 2;         // stderr
        const char msg1[] = "e_snprintf (" __FILE__ ":" EXP_TOSTRING(__LINE__)
            "): vsprintf buffer overflow";
        const char msg2[] = ", format string: ";
        const char msg3[] = "\n";

#undef EXP_TOSTRING
#undef TOSTRING

        // The first write() is small with static size; hopefully at least it
        // will make it through
        write(err_fd, msg1, sizeof(msg1) - 1);  // -1: don't write \0
        write(err_fd, msg2, sizeof(msg2) - 1);
        write(err_fd, format, strlen(format));
        write(err_fd, msg3, sizeof(msg3) - 1);

#if defined(SIGKILL)
        raise(SIGKILL);         // Common in unix; uncatchably fatal
#endif
        signal(SIGABRT, SIG_DFL);       // Reset default signal handler
        // Not using sim_abort() here: things are already going weird
        abort();
    }
#endif  // HAVE_VSNPRINTF

    va_end(ap);
    return used;
}


const char *
fmt_i64(i64 val)
{
    char *dest = get_dest();
    e_snprintf(dest, MAX_FMT_LEN + 1, "%" FMT_64_DEC, val);
    return dest;
}


const char *
fmt_u64(u64 val)
{
    char *dest = get_dest();
    e_snprintf(dest, MAX_FMT_LEN + 1, "%" FMT_U64_DEC, val);
    return dest;
}


const char *
fmt_x64(u64 val)
{
    char *dest = get_dest();
    e_snprintf(dest, MAX_FMT_LEN + 1, "%" FMT_U64_HEX, val);
    return dest;
}


const char *
fmt_mem(mem_addr val)
{
    char *dest = get_dest();
    e_snprintf(dest, MAX_FMT_LEN + 1, "0x%" FMT_U64_HEX, val);
    return dest;
}


const char *
fmt_laddr(LongAddr addr)
{
    char *dest = get_dest();
    e_snprintf(dest, MAX_FMT_LEN + 1, "%" FMT_U64_HEX ":%" FMT_U64_HEX,
               (u64) addr.id, addr.a);
    return dest;
}


std::string
LongAddr::fmt() const
{
    return std::string(fmt_laddr(*this));
}


const char *
fmt_bool(int bool_value)
{
    return (bool_value) ? "t" : "f";
}


static const char *
fmt_si_common(double val, int sig_figs, int base_val, int exponent_step,
              int lowest_prefix_exp, int num_prefixes,
              const char **prefixes)
{
    const char *sign = (val >= 0) ? "" : "-";
    double mag = fabs(val);
    double exp = log(mag) / log(base_val);
    int sci_exp = exponent_step * (int) floor(exp / exponent_step);
    const int highest_prefix_exp = lowest_prefix_exp +
        exponent_step * num_prefixes;

    double mant;
    const char *prefix;

    if ((sci_exp >= lowest_prefix_exp) && 
        (sci_exp <= highest_prefix_exp)) {
        int pref_idx = (sci_exp - lowest_prefix_exp) / exponent_step;
        prefix = prefixes[pref_idx];
        mant = mag / pow(base_val, sci_exp);
    } else {
        prefix = "";
        mant = mag;
    }

    if (sig_figs <= 0)
        sig_figs = DBL_DIG;

    char *dest = get_dest();
    e_snprintf(dest, MAX_FMT_LEN + 1, "%s%.*g%s", sign, sig_figs, mant,
               prefix);
    return dest;
}


const char *
fmt_si10(double val, int sig_figs)
{
    const int exponent_step = 3;
    const int base_val = 10;
    static const char *prefix_txt[] = 
        { "y", "z", "a", "f", "p", "n", "u", "m", "", 
          "k", "M", "G", "T", "P", "E", "Z", "Y" };
    const int lowest_prefix_exp = -24;

    return fmt_si_common(val, sig_figs, base_val, exponent_step,
                         lowest_prefix_exp, NELEM(prefix_txt), prefix_txt);
}


const char *
fmt_si2(double val, int sig_figs)
{
    const int exponent_step = 10;
    const int base_val = 2;
    static const char *prefix_txt[] = 
        { "", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei" };
    const int lowest_prefix_exp = 0;

    return fmt_si_common(val, sig_figs, base_val, exponent_step,
                         lowest_prefix_exp, NELEM(prefix_txt), prefix_txt);
}


/* based on jabrown's str2num_long() */
int 
str2num_i64(i64 *result, const char *str, int base)
{
    i64 value;
    char *endptr;

    if (!str[0] || isspace(static_cast<unsigned char>(str[0])))
        goto err_syntax;

    {
        unsigned range_error = 0;
        int old_errno = errno;
        errno = !ERANGE;
        value = STR2I64_FUNC(str, &endptr, base);
        if (errno == ERANGE)
            range_error = 1;
        errno = old_errno;
        if (range_error)
            goto err_range;
    }
   
    if (*endptr != '\0')
        goto err_syntax;

    *result = value;
    return 0;

err_range:
err_syntax:     
    return 1;
}


/* based on jabrown's str2num_ulong() */
int 
str2num_u64(u64 *result, const char *str, int base)
{
    u64 value;
    char *endptr;

    if (!str[0] || isspace(static_cast<unsigned char>(str[0])))
        goto err_syntax;

    {
        unsigned range_error = 0;
        int old_errno = errno;
        errno = !ERANGE;
        value = STR2U64_FUNC(str, &endptr, base);
        if (errno == ERANGE)
            range_error = 1;
        errno = old_errno;
        if (range_error)
            goto err_range;
    }
   
    if (*endptr != '\0')
        goto err_syntax;

    *result = value;
    return 0;

err_range:
err_syntax:     
    return 1;
}


void *
u64_to_ptr(u64 val)
{
    voidp_equiv val_equiv = static_cast<voidp_equiv>(val);
    if (SP_F(val_equiv != val)) {
        fprintf(stderr, "u64_to_ptr (%s:%i): bad cast of 0x%s to void *, "
                "value out of range (result was 0x%s)\n", __FILE__, __LINE__,
                fmt_x64(val), fmt_x64(val_equiv));
        sim_abort();
    }
    return reinterpret_cast<void *>(val_equiv);
}


u64 
u64_from_ptr(const void *ptr)
{
    voidp_equiv ptr_val = reinterpret_cast<voidp_equiv>(ptr);
    u64 result = static_cast<u64>(ptr_val);
    if (SP_F(ptr_val != result)) {
        fprintf(stderr, "u64_from_ptr (%s:%i): bad cast of %p(0x%s) to u64, "
                "value out of range (result was 0x%s)\n", __FILE__, __LINE__,
                ptr, fmt_x64(ptr_val), fmt_x64(result));
        sim_abort();
    }
    return result;
}


int
i64_to_int(i64 val)
{
    if (SP_F((val < INT_MIN) || (val > INT_MAX))) {
        fprintf(stderr, "i64_to_int overflow: %s\n", fmt_i64(val));
        sim_abort();
    }
    return static_cast<int>(val);
}


i32
i64_to_i32(i64 val)
{
    if (SP_F((val < I32_MIN) || (val > I32_MAX))) {
        fprintf(stderr, "i64_to_i32 overflow: %s\n", fmt_i64(val));
        sim_abort();
    }
    return static_cast<i32>(val);
}


long
i64_to_long(i64 val)
{
    if (SP_F((val < LONG_MIN) || (val > LONG_MAX))) {
        fprintf(stderr, "i64_to_long overflow: %s\n", fmt_i64(val));
        sim_abort();
    }
    return static_cast<long>(val);
}


i64
double_to_i64(double val)
{
    double f_val = floor(val);
    if (SP_F((f_val < I64_MIN) || (f_val > I64_MAX))) {
        fprintf(stderr, "double_to_i64 overflow: %.15g\n", val);
        sim_abort();
    }
    return static_cast<i64>(f_val);
}


double
i64_to_double(i64 val)
{
    if (SP_F((val < I64_DOUBLE_MIN) || (val > I64_DOUBLE_MAX))) {
        fprintf(stderr, "i64_to_double mantissa overflow: %s\n", fmt_i64(val));
        sim_abort();
    }
    return static_cast<double>(val);
}


size_t
hash_u32(u32 key)
{
    HashU32 hasher;
    return hasher(key);
}


size_t
hash_u64(u64 key)
{
    HashU64 hasher;
    return hasher(key);
}


size_t
hash_voidp(const void *key)
{
    //StlHashVoidPtr hasher;
    HashU64 hasher;
    return hasher(reinterpret_cast<u64>(key));
}


size_t
hash_laddr(LongAddr addr)
{
    return addr.hash();
}
