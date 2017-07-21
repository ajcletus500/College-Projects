/*
 * POSIX regexp wrapper: extended regular expression matching, slow but simple
 *
 * This is built upon the POSIX regexp library.
 *
 * This provides an easy-to-use interface which hides the state management
 * details of the underlying regular expression library, in order to provide
 * simple one-call-per-match usage with minimal burden on the caller.
 *
 * Jeff Brown
 * $JAB-Id: simple-pre.h,v 1.1 2005/02/07 07:35:45 jabrown Exp $
 */

#ifndef SIMPLE_PRE_H
#define SIMPLE_PRE_H

#ifdef __cplusplus
extern "C" {
#endif


enum {
    /* Compile options */
    SPRE_Caseless = 0x1,        /* Ignore case; posix REG_ICASE */
    SPRE_MultiLine = 0x2,       /* Recognize newlines; posix REG_NEWLINE */

    /* Match options */
    SPRE_NotBOL = 0x100,        /* Start of "text" isn't beginning of line */
    SPRE_NotEOL = 0x200         /* End of "text" isn't end of line */
};


#ifndef HAVE_SimpleRESubstr
    #define HAVE_SimpleRESubstr
    typedef struct SimpleRESubstr {
        int st;                 /* String offset of substring start */
        int len;                /* Length of substring */
    } SimpleRESubstr;
#endif


/*
 * Match "text" against "pattern", using the supplied options.  On a match,
 * returns one plus the number of parenthesized subexpressions in "pattern".
 * If "text" does not match, returns 0.  On any error, this prints an error
 * message to stderr and returns -1.
 */
int
simple_pre_nocap(const char *pattern, const char *text, int options);

/*
 * Like "simple_pre_nocap", except that on a match, this performs
 * sub-expression capture: it stores the offsets corresponding to the portions
 * of "text" which match the overall pattern as well as portions which match
 * each parenthesized sub-expression.
 *
 * Captured expression info is stored in an array "capture_array", which has
 * "capture_size" elements.  If a sub-expression didn't match any text, the
 * corresponding substring is set to start at -1 with length 0.
 *
 * Subexpression 0 is implicitly the entire matching portion of "text";
 * subexpressions 1...N correspond to parentheses in "pattern".
 */
int
simple_pre_fixcap(const char *pattern, const char *text, int options, 
                  SimpleRESubstr *capture_array, int capture_size);

/*
 * Like "simple_pre_fixcap", except that this dynamically allocates an array
 * to hold captured expression info (with malloc), and returns it to the
 * caller.  A pointer to the new array is stored at "capture_ret", and the
 * array size in elements is stored at "cap_size_ret".  The caller is
 * responsible for freeing the array.
 */
int
simple_pre_dyncap(const char *pattern, const char *text, int options, 
                  SimpleRESubstr **capture_ret, int *cap_size_ret);


#ifdef __cplusplus
}
#endif

#endif  /* SIMPLE_PRE_H */
